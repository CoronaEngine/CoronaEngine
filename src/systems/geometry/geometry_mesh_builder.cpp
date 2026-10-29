#include "geometry_draw_buffers.h"
#include "geometry_upload_batch.h"
#include <corona/systems/geometry/model_node_import.h>
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

[[nodiscard]] ktm::fmat4x4 to_ktm_matrix(const std::array<float, 16>& src) {
    ktm::fmat4x4 dst = ktm::fmat4x4::from_eye();
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            dst[col][row] = src[static_cast<std::size_t>(col * 4 + row)];
        }
    }
    return dst;
}

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
        case Horizon::Format::BC1_UNORM:
        case Horizon::Format::BC1_UNORM_SRGB: return blocks_w * blocks_h * 8;   // 8B / 4x4 块
        case Horizon::Format::BC3_UNORM:
        case Horizon::Format::BC3_UNORM_SRGB: return blocks_w * blocks_h * 16;  // 16B / 4x4 块
        default:                              return static_cast<std::size_t>(width) * height * 4;  // RGBA8
    }
}

bool upload_geometry_texture(Horizon::HardwareImage& texture,
                             std::span<const std::byte> bytes,
                             const char* label, GeometryDetail::UploadBatch& uploads) {
    if (!texture || bytes.empty()) {
        CFW_LOG_WARNING("[GeometryMeshBuilder] Texture upload skipped (label={}, valid={}, bytes={})",
                        label, static_cast<bool>(texture), bytes.size_bytes());
        return false;
    }
    Horizon::HardwareBuffer staging;
    try {
        // Horizon 移除了 HardwareImage::upload()：改为 staging buffer + copy_from()。
        Horizon::HardwareBufferDesc staging_desc;
        staging_desc.element_count = bytes.size_bytes();
        staging_desc.element_size = 1;
        staging_desc.usage = Horizon::BufferUsage_TransferSrc;
        staging_desc.cpu_access = Horizon::CpuAccessMode::Write;
        staging = Horizon::HardwareBuffer(staging_desc, bytes);
    } catch (const std::exception& exc) {
        CFW_LOG_WARNING("[GeometryMeshBuilder] Texture upload failed (label={}, error={})",
                        label, exc.what());
        return false;
    }
    // A batch flush also contains vertex/index uploads. Propagate submission or
    // completion errors so a partially uploaded mesh is never published ready.
    uploads.copy_image(staging, texture, bytes.size_bytes());
    return true;
}

// Cache colour/data views separately, but share MR/AO and repeated materials.
const SceneGpuTextureCache::Entry& load_material_image(
    SceneGpuTextureCache& cache, std::uint64_t id, bool srgb,
    GeometryDetail::UploadBatch& uploads) {
    auto& entries = srgb ? cache.entries : cache.linear_entries;
    if (auto found = entries.find(id); found != entries.end()) return found->second;
    SceneGpuTextureCache::Entry entry;
    auto image = Resource::ResourceManager::get_instance().acquire_read<Resource::Image>(id);
    if (image && image->get_width() > 0 && image->get_height() > 0) {
        const auto width = static_cast<std::uint32_t>(image->get_width());
        const auto height = static_cast<std::uint32_t>(image->get_height());
        auto format = srgb ? Horizon::Format::SRGBA8_UNORM : Horizon::Format::RGBA8_UNORM;
        std::vector<unsigned char> rgba;
        std::span<const std::byte> bytes;
        if (image->is_compressed()) {
            const auto& compressed = image->get_compressed_data();
            if (compressed.format == Resource::CompressedData::Format::BC1) {
                format = srgb ? Horizon::Format::BC1_UNORM_SRGB : Horizon::Format::BC1_UNORM;
                bytes = std::as_bytes(std::span(compressed.data));
            } else if (compressed.format == Resource::CompressedData::Format::BC3) {
                format = srgb ? Horizon::Format::BC3_UNORM_SRGB : Horizon::Format::BC3_UNORM;
                bytes = std::as_bytes(std::span(compressed.data));
            }
        } else if (const auto* src = image->get_data()) {
            const int channels = image->get_channels();
            const auto pixels = std::size_t(width) * height;
            if (channels == 4) bytes = std::as_bytes(std::span(src, pixels * 4));
            else if (channels >= 1 && channels <= 3) {
                rgba.resize(pixels * 4);
                for (std::size_t i = 0; i < pixels; ++i) {
                    for (int c = 0; c < 3; ++c) rgba[4*i+c] = src[channels*i + (channels < 3 ? 0 : c)];
                    rgba[4*i+3] = channels == 2 ? src[channels*i+1] : 255;
                }
                bytes = std::as_bytes(std::span(rgba));
            }
        }
        if (!bytes.empty()) {
            entry.image = make_geometry_texture(width, height, format,
                srgb ? "geometry.material.srgb" : "geometry.material.linear");
            if (upload_geometry_texture(entry.image, bytes, "material", uploads)) {
                entry.valid = true;
                entry.gpu_bytes = gpu_texture_bytes(format, width, height);
                entry.mem = std::make_shared<Memory::GpuMemToken>(Memory::ResKind::Texture, entry.gpu_bytes);
            }
        }
    }
    if (!entry.valid) {
        entry.image = {};
        CFW_LOG_WARNING("[GeometryMeshBuilder] Material texture unavailable (id={}, srgb={}); using authored factor", id, srgb);
    }
    return entries.emplace(id, std::move(entry)).first->second;
}

std::shared_ptr<const MeshMaterialDevice> make_material(
    const Resource::Scene& scene, std::uint32_t index, SceneGpuTextureCache& cache,
    GeometryDetail::UploadBatch& uploads) {
    if (auto found = cache.materials.find(index); found != cache.materials.end()) return found->second;
    auto material = std::make_shared<MeshMaterialDevice>();
    if (index < scene.data.materials.size()) material->source = scene.data.materials[index];
    const auto& source = material->source;
    const std::array ids{source.albedo_texture, source.normal_texture,
        source.metallic_texture, source.roughness_texture, source.occlusion_texture,
        source.emissive_texture, source.specular_texture, source.specular_color_texture};
    for (std::size_t slot = 0; slot < ids.size(); ++slot) {
        if (ids[slot] == Resource::InvalidTextureId) continue;
        const bool colour = slot == MeshMaterialDevice::BaseColor ||
            slot == MeshMaterialDevice::Emissive || slot == MeshMaterialDevice::SpecularColor;
        const auto& image = load_material_image(cache, ids[slot], colour, uploads);
        material->textures[slot] = image.image;
        material->memory[slot] = image.mem;
    }
    cache.materials.emplace(index, material);
    return material;
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
        GeometryDetail::UploadBatch uploads;
        static const unsigned char white_pixel[4] = {255, 255, 255, 255};  // 不透明白色
        g_placeholder_texture = std::make_unique<Horizon::HardwareImage>(
            make_geometry_texture(1, 1, Horizon::Format::SRGBA8_UNORM,
                                  "geometry.placeholder_texture"));
        const bool placeholder_upload_ok = upload_geometry_texture(
            *g_placeholder_texture,
            std::as_bytes(std::span<const unsigned char>(white_pixel, sizeof(white_pixel))),
            "placeholder", uploads);
        uploads.finish();
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

std::vector<MeshDevice> build_mesh_devices_from_scene_range(
    const Resource::Scene& scene,
    std::size_t first_mesh,
    std::size_t mesh_count,
    SceneGpuTextureCache& texture_cache) {

    Horizon::HardwareImage& placeholder_texture = get_placeholder_texture();
    GeometryDetail::UploadBatch uploads;

    const std::size_t end_mesh = std::min(first_mesh + mesh_count, scene.data.meshes.size());
    std::vector<MeshDevice> mesh_devices;
    mesh_devices.reserve(end_mesh - first_mesh);
    texture_cache.entries.reserve(std::max(texture_cache.entries.size(), scene.data.materials.size()));

    // ---- 遍历本批 mesh，创建 GPU 缓冲 ----
    for (std::uint32_t mesh_idx = static_cast<std::uint32_t>(first_mesh);
         mesh_idx < end_mesh; ++mesh_idx) {
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
        auto buffers = GeometryDetail::make_draw_buffers(vertices, indices, uploads);
        dev.draw_clusters = std::move(buffers.clusters);
        dev.vertexBuffer = dev.vertexStorageBuffer = std::move(buffers.vertices);
        dev.indexBuffer = dev.indexStorageBuffer = std::move(buffers.indices);
        dev.shadowIndexBuffer = std::move(buffers.shadow_indices);
        // One shared vertex allocation, original indices, spatial shadow indices.
        dev.mesh_mem = Corona::Memory::GpuMemToken(Corona::Memory::ResKind::Mesh,
            buffers.byte_size);

        dev.materialIndex = mesh.material_index;
        dev.material = make_material(scene, mesh.material_index, texture_cache, uploads);
        dev.materialColor = dev.material->source.base_color;
        dev.textureBuffer = dev.material->textures[MeshMaterialDevice::BaseColor];
        dev.tex_mem = dev.material->memory[MeshMaterialDevice::BaseColor];
        const bool texture_created = static_cast<bool>(dev.textureBuffer);
        if (!dev.textureBuffer) dev.textureBuffer = placeholder_texture;

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

        CFW_LOG_DEBUG("[GeometryMeshBuilder] Mesh GPU resources staged "
                      "(mesh={}, device={}, vertices={}, indices={}, max_index={}, texture_created={}, tex={})",
                      mesh_idx, device_idx, vertices.size(), indices.size(), max_index,
                      texture_created, static_cast<bool>(dev.textureBuffer));

        // ---- 将构建好的 MeshDevice 加入数组 ----
        mesh_devices.emplace_back(std::move(dev));
    }

    uploads.finish();
    return mesh_devices;
}

std::vector<MeshDevice> build_mesh_devices_from_scene(
    const Resource::Scene& scene) {
    SceneGpuTextureCache texture_cache;
    return build_mesh_devices_from_scene_range(
        scene, 0, scene.data.meshes.size(), texture_cache);
}

SceneGpuMeshData make_scene_gpu_mesh_data(
    const Resource::Scene& scene,
    std::vector<MeshDevice> mesh_devices) {
    SceneGpuMeshData result;
    result.mesh_devices = std::move(mesh_devices);
    result.model_nodes = ModelNodes::from_scene(scene.data);
    result.node_instance_prefix=scene.data.node_instance_prefix;
    result.has_node_instance_prefix=scene.data.has_node_instance_prefix;

    if (!scene.data.mesh_instances.empty()) {
        result.mesh_instances.reserve(scene.data.mesh_instances.size());
        for (const auto& instance : scene.data.mesh_instances) {
            if (instance.mesh_index >= result.mesh_devices.size()) {
                continue;
            }
            MeshInstanceDevice gpu_instance;
            gpu_instance.mesh_index = instance.mesh_index;
            gpu_instance.object_id = instance.object_id;
            gpu_instance.node_index = instance.node_index;
            gpu_instance.local_bounds_min = scene.data.meshes[instance.mesh_index].aabb_min;
            gpu_instance.local_bounds_max = scene.data.meshes[instance.mesh_index].aabb_max;
            gpu_instance.transform = to_ktm_matrix(instance.transform);
            gpu_instance.original_transform = gpu_instance.transform;
            result.mesh_instances.push_back(gpu_instance);
        }
    }

    if (result.mesh_instances.empty()) {
        result.mesh_instances.reserve(result.mesh_devices.size());
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(result.mesh_devices.size()); ++i) {
            MeshInstanceDevice gpu_instance;
            gpu_instance.mesh_index = i;
            gpu_instance.local_bounds_min = scene.data.meshes[i].aabb_min;
            gpu_instance.local_bounds_max = scene.data.meshes[i].aabb_max;
            gpu_instance.node_delta_prefix =
                !scene.data.meshes[i].bone_weights.empty() && scene.data.skeleton
                    ? scene.data.skeleton->global_inverse : scene.data.node_instance_prefix;
            for(uint32_t n=0;n<scene.data.nodes.size();++n)if(scene.data.nodes[n].mesh_index==i){gpu_instance.node_index=n;break;}
            result.mesh_instances.push_back(gpu_instance);
        }
    }

    return result;
}

SceneGpuMeshData build_gpu_mesh_data_from_scene(
    const Resource::Scene& scene) {
    return make_scene_gpu_mesh_data(scene, build_mesh_devices_from_scene(scene));
}

}  // namespace Corona::Systems
