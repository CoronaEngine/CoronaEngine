/// @file geometry_mesh_builder.cpp
/// @brief 从 Resource::Scene 构建 GPU MeshDevice 数组的单一实现。
///
/// 详见 geometry_mesh_builder.h。本文件是初始加载 / 距离重载 / LRU 恢复
/// 三条路径共用的 GPU 构建逻辑（此前在 Python API 层与 GeometrySystem 各有一份）。

#include <corona/systems/geometry/geometry_mesh_builder.h>

#include <horizon/core/logging.h>
#include <corona/resource/resource.h>
#include <corona/resource/resource_manager.h>
#include <corona/resource/types/scene.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>

namespace Corona::Systems {

namespace {

template <typename T>
Horizon::HardwareBuffer make_geometry_buffer(const std::vector<T>& data,
                                             Horizon::BufferUsageFlags usage,
                                             std::string name = {}) {
    if (data.empty()) {
        CFW_LOG_WARNING("[GeometryMeshBuilder] Buffer creation skipped for empty data (name={})",
                        name);
        return {};
    }
    Horizon::HardwareBufferDesc desc;
    desc.element_count = data.size();
    desc.element_size = static_cast<uint32_t>(sizeof(T));
    desc.usage = usage;
    desc.debug_name = std::move(name);
    return Horizon::HardwareBuffer(desc, std::as_bytes(std::span<const T>(data.data(), data.size())));
}

Horizon::HardwareImage make_geometry_texture(uint32_t width,
                                             uint32_t height,
                                             Horizon::Format format,
                                             std::string name = {}) {
    return Horizon::HardwareImage(Horizon::HardwareImageDesc::texture_2d(
        width,
        height,
        format,
        Horizon::ImageUsage_Sampled | Horizon::ImageUsage_TransferDst,
        std::move(name)));
}

// GPU 纹理显存字节估算（P0 记账用）。BC 块压缩按 4x4 块计，其余按 RGBA8。
[[nodiscard]] std::size_t gpu_texture_bytes(Horizon::Format format,
                                            uint32_t width, uint32_t height) {
    const std::size_t blocks_w = (static_cast<std::size_t>(width) + 3) / 4;
    const std::size_t blocks_h = (static_cast<std::size_t>(height) + 3) / 4;
    switch (format) {
        case Horizon::Format::BC1_UNORM_SRGB: return blocks_w * blocks_h * 8;   // 8B / 4x4 块
        case Horizon::Format::BC3_UNORM_SRGB: return blocks_w * blocks_h * 16;  // 16B / 4x4 块
        default:                              return static_cast<std::size_t>(width) * height * 4;  // RGBA8
    }
}

bool upload_geometry_texture(Horizon::HardwareImage& texture,
                             std::span<const std::byte> bytes,
                             const char* label) {
    if (!texture || bytes.empty()) {
        CFW_LOG_WARNING("[GeometryMeshBuilder] Texture upload skipped (label={}, valid={}, bytes={})",
                        label, static_cast<bool>(texture), bytes.size_bytes());
        return false;
    }
    try {
        // Horizon 移除了 HardwareImage::upload()：改为 staging buffer + copy_from()。
        Horizon::HardwareBufferDesc staging_desc;
        staging_desc.element_count = bytes.size_bytes();
        staging_desc.element_size = 1;
        staging_desc.usage = Horizon::BufferUsage_TransferSrc;
        staging_desc.cpu_access = Horizon::CpuAccessMode::Write;
        const Horizon::HardwareBuffer staging(staging_desc, bytes);

        Horizon::HardwareExecutor executor;
        const auto receipt = executor.stream()
            << texture.copy_from(staging)
            << Horizon::commit();
        // staging 是局部量，必须等 GPU 用完再析构。
        executor.wait_idle(receipt);
        return true;
    } catch (const std::exception& exc) {
        CFW_LOG_WARNING("[GeometryMeshBuilder] Texture upload failed (label={}, error={})",
                        label, exc.what());
        return false;
    }
}

// ----------------------------------------------------------------------------
// 进程级共享占位纹理（1x1 白）——占位纹理的唯一所有者
// ----------------------------------------------------------------------------
// 用 unique_ptr + mutex 而非函数局部 static：后者无法在 GPU device 析构前显式释放，
// 会导致 device 销毁后才析构 HardwareImage → crash。本模块由 GeometrySystem 在
// shutdown() 中调用 release_geometry_placeholder_texture() 显式释放。
std::mutex                              g_placeholder_mutex;
std::unique_ptr<Horizon::HardwareImage> g_placeholder_texture;

// 返回共享占位纹理的引用，首次调用时惰性创建。线程安全。
Horizon::HardwareImage& get_placeholder_texture() {
    std::lock_guard lock(g_placeholder_mutex);
    if (!g_placeholder_texture) {
        static const unsigned char white_pixel[4] = {255, 255, 255, 255};  // 不透明白色
        g_placeholder_texture = std::make_unique<Horizon::HardwareImage>(
            make_geometry_texture(1, 1, Horizon::Format::SRGBA8_UNORM,
                                  "geometry.placeholder_texture"));
        const bool placeholder_upload_ok = upload_geometry_texture(
            *g_placeholder_texture,
            std::as_bytes(std::span<const unsigned char>(white_pixel, sizeof(white_pixel))),
            "placeholder");
        if (!placeholder_upload_ok) {
            CFW_LOG_WARNING("[GeometryMeshBuilder] Failed to upload placeholder texture");
        }
    }
    return *g_placeholder_texture;
}

}  // namespace

void release_geometry_placeholder_texture() {
    std::lock_guard lock(g_placeholder_mutex);
    g_placeholder_texture.reset();
}

std::vector<MeshDevice> build_mesh_devices_from_scene(
    const Resource::Scene& scene) {

    Horizon::HardwareImage& placeholder_texture = get_placeholder_texture();

    auto& resource_manager = Resource::ResourceManager::get_instance();

    std::vector<MeshDevice> mesh_devices;
    mesh_devices.reserve(scene.data.meshes.size());

    struct SharedTextureEntry {
        Horizon::HardwareImage image;
        std::shared_ptr<Corona::Memory::GpuMemToken> mem;
        std::size_t gpu_bytes = 0;
        bool valid = false;
    };
    std::unordered_map<std::uint64_t, SharedTextureEntry> shared_textures;
    shared_textures.reserve(scene.data.materials.size());

    // ---- 遍历所有 mesh，创建 GPU 缓冲 ----
    for (std::uint32_t mesh_idx = 0; mesh_idx < scene.data.meshes.size(); ++mesh_idx) {
        const auto& mesh = scene.data.meshes[mesh_idx];  // 当前 mesh 的 CPU 端数据
        const auto& vertices = scene.get_mesh_vertices(mesh_idx);
        const auto& indices = scene.get_mesh_indices(mesh_idx);

        if (vertices.empty() || indices.empty()) {
            CFW_LOG_WARNING("[GeometryMeshBuilder] Skipping empty mesh (mesh={}, vertices={}, indices={})",
                            mesh_idx, vertices.size(), indices.size());
            continue;
        }
        const auto max_index_it = std::max_element(indices.begin(), indices.end());
        const std::uint16_t max_index = (max_index_it != indices.end()) ? *max_index_it : 0;
        if (static_cast<std::size_t>(max_index) >= vertices.size()) {
            CFW_LOG_ERROR("[GeometryMeshBuilder] Skipping mesh with invalid index range "
                          "(mesh={}, vertices={}, indices={}, max_index={})",
                          mesh_idx, vertices.size(), indices.size(), max_index);
            continue;
        }

        const std::uint32_t device_idx = static_cast<std::uint32_t>(mesh_devices.size());
        MeshDevice dev{};  // 零初始化 MeshDevice（所有句柄为 0/null）
        dev.vertex_count = static_cast<std::uint32_t>(vertices.size());
        dev.index_count = static_cast<std::uint32_t>(indices.size());
        dev.max_index = max_index;

        // ---- 创建顶点/索引缓冲（4 个）----
        // vertexBuffer / indexBuffer：渲染管线使用（Vertex Shader 读取）
        // vertexStorageBuffer / indexStorageBuffer：Compute Shader 使用（可读写）
        dev.vertexBuffer = make_geometry_buffer(
            vertices,
            Horizon::BufferUsage_TransferDst | Horizon::BufferUsage_Vertex,
            "geometry.vertex");
        dev.indexBuffer = make_geometry_buffer(
            indices,
            Horizon::BufferUsage_TransferDst | Horizon::BufferUsage_Index,
            "geometry.index");
        dev.vertexStorageBuffer = make_geometry_buffer(
            vertices,
            Horizon::BufferUsage_TransferSrc | Horizon::BufferUsage_TransferDst |
                Horizon::BufferUsage_Storage,
            "geometry.vertex_storage");
        dev.indexStorageBuffer = make_geometry_buffer(
            indices,
            Horizon::BufferUsage_TransferSrc | Horizon::BufferUsage_TransferDst |
                Horizon::BufferUsage_Storage,
            "geometry.index_storage");

        // ---- GPU mesh 显存记账（P0）----
        // 顶点/索引各上传两份（普通 + storage），故 ×2。
        {
            const std::size_t mesh_gpu_bytes =
                2u * vertices.size() * sizeof(Resource::Vertex) +
                2u * indices.size()  * sizeof(std::uint16_t);
            dev.mesh_mem = Corona::Memory::GpuMemToken(Corona::Memory::ResKind::Mesh, mesh_gpu_bytes);
        }

        // ---- 材质索引 ----
        // material_index 指向 scene.data.materials 数组
        // InvalidIndex（最大值）表示无材质 → 降级为 0（使用默认材质）
        dev.materialIndex = (mesh.material_index != Resource::InvalidIndex)
                                ? mesh.material_index                    // 有效材质索引
                                : 0;                                    // 降级为默认材质

        // ---- 读取材质颜色（base_color：RGBA 漫反射颜色）----
        if (mesh.material_index != Resource::InvalidIndex &&
            mesh.material_index < scene.data.materials.size()) {
            dev.materialColor = scene.data.materials[mesh.material_index].base_color;
        }

        // ---- 纹理处理：按唯一 texture_id 共享 GPU 纹理 ----
        // 同一个 albedo texture_id 在第一处遇到时创建并立即上传；后续 mesh 只复用
        // 同一个 HardwareImage 句柄。
        bool texture_created = false;
        uint32_t texture_width = 0;
        uint32_t texture_height = 0;
        Horizon::Format texture_format = Horizon::Format::SRGBA8_UNORM;

        if (mesh.material_index != Resource::InvalidIndex &&
            mesh.material_index < scene.data.materials.size()) {
            const auto texture_id = scene.data.materials[mesh.material_index].albedo_texture;
            if (texture_id != Resource::InvalidTextureId) {
                const auto cache_it = shared_textures.find(texture_id);
                if (cache_it != shared_textures.end()) {
                    dev.textureBuffer = cache_it->second.image;
                    dev.tex_mem = cache_it->second.mem;
                    texture_created = cache_it->second.valid;
                } else {
                    SharedTextureEntry entry;
                    auto texture_data = resource_manager.acquire_read<Resource::Image>(texture_id);
                    if (texture_data && texture_data->get_data() != nullptr) {
                        const int tex_width    = texture_data->get_width();
                        const int tex_height   = texture_data->get_height();
                        const int tex_channels = texture_data->get_channels();
                        if (tex_width > 0 && tex_height > 0 && tex_channels > 0) {
                            texture_width = static_cast<uint32_t>(tex_width);
                            texture_height = static_cast<uint32_t>(tex_height);

                            std::vector<unsigned char> converted_rgba;
                            std::span<const std::byte> upload_bytes;
                            bool supported_format = true;

                            if (texture_data->is_compressed()) {
                                const auto& compressed = texture_data->get_compressed_data();
                                if (compressed.format == Resource::CompressedData::Format::BC1) {
                                    texture_format = Horizon::Format::BC1_UNORM_SRGB;
                                } else if (compressed.format == Resource::CompressedData::Format::BC3) {
                                    texture_format = Horizon::Format::BC3_UNORM_SRGB;
                                } else if (compressed.format == Resource::CompressedData::Format::ASTC_4x4) {
                                    CFW_LOG_WARNING("[GeometryMeshBuilder] ASTC_4x4 texture is not "
                                                    "supported by current Horizon format enum; "
                                                    "using placeholder texture");
                                    supported_format = false;
                                }
                                if (supported_format && !compressed.data.empty()) {
                                    upload_bytes = std::span<const std::byte>(
                                        reinterpret_cast<const std::byte*>(compressed.data.data()),
                                        compressed.data.size());
                                }
                            } else {
                                texture_format = Horizon::Format::SRGBA8_UNORM;
                                unsigned char* src_data = texture_data->get_data();
                                const std::size_t pixel_count =
                                    static_cast<std::size_t>(tex_width) * tex_height;
                                if (tex_channels == 4) {
                                    upload_bytes = std::span<const std::byte>(
                                        reinterpret_cast<const std::byte*>(src_data),
                                        pixel_count * 4);
                                } else if (tex_channels == 3) {
                                    converted_rgba.resize(pixel_count * 4);
                                    for (std::size_t i = 0; i < pixel_count; ++i) {
                                        converted_rgba[i * 4 + 0] = src_data[i * 3 + 0];
                                        converted_rgba[i * 4 + 1] = src_data[i * 3 + 1];
                                        converted_rgba[i * 4 + 2] = src_data[i * 3 + 2];
                                        converted_rgba[i * 4 + 3] = 255;
                                    }
                                    upload_bytes = std::span<const std::byte>(
                                        reinterpret_cast<const std::byte*>(converted_rgba.data()),
                                        converted_rgba.size());
                                } else if (tex_channels == 1) {
                                    converted_rgba.resize(pixel_count * 4);
                                    for (std::size_t i = 0; i < pixel_count; ++i) {
                                        converted_rgba[i * 4 + 0] = src_data[i];
                                        converted_rgba[i * 4 + 1] = src_data[i];
                                        converted_rgba[i * 4 + 2] = src_data[i];
                                        converted_rgba[i * 4 + 3] = 255;
                                    }
                                    upload_bytes = std::span<const std::byte>(
                                        reinterpret_cast<const std::byte*>(converted_rgba.data()),
                                        converted_rgba.size());
                                }
                            }

                            if (supported_format && !upload_bytes.empty()) {
                                entry.image = make_geometry_texture(
                                    texture_width, texture_height, texture_format,
                                    "geometry.material_texture");
                                if (upload_geometry_texture(entry.image, upload_bytes, "material")) {
                                    entry.gpu_bytes = gpu_texture_bytes(
                                        texture_format, texture_width, texture_height);
                                    entry.mem = std::make_shared<Corona::Memory::GpuMemToken>(
                                        Corona::Memory::ResKind::Texture, entry.gpu_bytes);
                                    entry.valid = true;
                                    texture_created = true;
                                    dev.textureBuffer = entry.image;
                                    dev.tex_mem = entry.mem;
                                } else {
                                    CFW_LOG_WARNING("[GeometryMeshBuilder] Failed to upload material "
                                                    "texture (mesh={}, texture_id={}); using placeholder",
                                                    mesh_idx, texture_id);
                                }
                            }
                        }
                    }
                    if (!entry.valid) {
                        entry.image = placeholder_texture;
                    }
                    shared_textures.emplace(texture_id, std::move(entry));
                }
            }
        }

        // ---- 无纹理/纹理失败：使用共享白色占位纹理 ----
        if (!texture_created) {
            dev.textureBuffer = placeholder_texture;
            dev.tex_mem.reset();
        }

        if (!dev.vertexBuffer || !dev.indexBuffer ||
            !dev.vertexStorageBuffer || !dev.indexStorageBuffer ||
            !dev.textureBuffer) {
            CFW_LOG_ERROR("[GeometryMeshBuilder] Skipping mesh with invalid GPU resources "
                          "(mesh={}, vertices={}, indices={}, max_index={}, vb={}, ib={}, vsb={}, isb={}, tex={})",
                          mesh_idx, vertices.size(), indices.size(), max_index,
                          static_cast<bool>(dev.vertexBuffer),
                          static_cast<bool>(dev.indexBuffer),
                          static_cast<bool>(dev.vertexStorageBuffer),
                          static_cast<bool>(dev.indexStorageBuffer),
                          static_cast<bool>(dev.textureBuffer));
            continue;
        }

        CFW_LOG_DEBUG("[GeometryMeshBuilder] Mesh GPU resources ready "
                      "(mesh={}, device={}, vertices={}, indices={}, max_index={}, texture_created={}, tex={})",
                      mesh_idx, device_idx, vertices.size(), indices.size(), max_index,
                      texture_created, static_cast<bool>(dev.textureBuffer));

        // ---- 将构建好的 MeshDevice 加入数组 ----
        mesh_devices.emplace_back(std::move(dev));
    }

    return mesh_devices;
}

}  // namespace Corona::Systems
