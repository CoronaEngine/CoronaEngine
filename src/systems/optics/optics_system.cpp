#include <corona/events/display_system_events.h>
#include <corona/events/optics_system_events.h>
#include <corona/kernel/core/i_logger.h>
#include <corona/kernel/core/kernel_context.h>
#include <corona/kernel/event/i_event_bus.h>
#include <corona/kernel/event/i_event_stream.h>
#include <corona/resource/resource_manager.h>
#include <corona/resource/types/image.h>
#include <corona/shared_data_hub.h>
#include <corona/systems/geometry/geometry_system.h>
#include <corona/systems/optics/optics_system.h>

#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <system_error>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "hardware.h"

// CORONA_ENABLE_VISION is controlled by CMake (-DCORONA_ENABLE_VISION).

//#define CORONA_VISION_IMPORT_DEMO

#ifdef CORONA_ENABLE_VISION
#include "base/import/parameter_set.h"
#include "base/import/json_util.h"
#include "base/import/project_desc.h"
#include "base/mgr/global.h"
#include "base/mgr/pipeline.h"
#include "base/mgr/scene.h"
#include "base/sensor/frame_buffer.h"
#include "base/sensor/light_field_types.h"
#include "base/sensor/sensor.h"
#include "rhi/context.h"
#include "vision/vision_geometry_adapter.h"
#include "vision/vision_camera_adapter.h"
#include "vision/vision_light_adapter.h"
#include "vision/vision_render_mode_config.h"
#include "vision/vision_zero_copy_bridge.h"
#endif

namespace {

struct RenderInstanceBatch {
    std::vector<Hardware::InstanceInfo> instances;
    std::vector<Hardware::MaterialInfo> materials;
    std::vector<std::uintptr_t> actorHandles;

    void clear() {
        instances.clear();
        materials.clear();
        actorHandles.clear();
    }
};

struct UVec2Upload {
    uint32_t x;
    uint32_t y;
};

struct FVec3Upload {
    float x;
    float y;
    float z;
};

struct FVec4Upload {
    float x;
    float y;
    float z;
    float w;
};

struct Mat4Upload {
    float values[16];
};

struct ImagePixelExtent {
    uint32_t width = 0;
    uint32_t height = 0;
};

struct OpticsEventViewport {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

[[nodiscard]] OpticsEventViewport optics_event_viewport(
    const Corona::CameraDevice& camera,
    const ImagePixelExtent& presented_extent) {
    if (!camera.viewport_rect_active || camera.view_open ||
        camera.view_width <= 0 || camera.view_height <= 0) {
        return {0, 0, presented_extent.width, presented_extent.height};
    }

    return {
        static_cast<uint32_t>(std::max(camera.view_x, 0)),
        static_cast<uint32_t>(std::max(camera.view_y, 0)),
        static_cast<uint32_t>(std::max(camera.view_width, 1)),
        static_cast<uint32_t>(std::max(camera.view_height, 1)),
    };
}

[[nodiscard]] UVec2Upload upload_value(const ktm::uvec2& value) {
    return {value.x, value.y};
}

[[nodiscard]] FVec3Upload upload_value(const ktm::fvec3& value) {
    return {value.x, value.y, value.z};
}

[[nodiscard]] FVec4Upload upload_value(const ktm::fvec4& value) {
    return {value.x, value.y, value.z, value.w};
}

[[nodiscard]] Mat4Upload upload_value(const ktm::fmat4x4& value) {
    Mat4Upload upload{};
    static_assert(sizeof(upload) == sizeof(value));
    std::memcpy(&upload, &value, sizeof(upload));
    return upload;
}

[[nodiscard]] ImagePixelExtent hardware_image_extent(const Corona::Horizon::HardwareImage& image) {
    if (!image) {
        return {};
    }
    const auto extent = image.extent();
    return {extent.width, extent.height};
}

[[nodiscard]] Corona::Horizon::RasterizerPipelineDesc make_visibility_pipeline_desc() {
    Corona::Horizon::RasterizerPipelineDesc desc;
    auto vertex_shader = Corona::Horizon::PipelineShaderDesc::from_slang_module(
        Corona::Horizon::PipelineShaderStage::Vertex,
        visibility_vert_glsl_t::slangModule);
    auto fragment_shader = Corona::Horizon::PipelineShaderDesc::from_slang_module(
        Corona::Horizon::PipelineShaderStage::Fragment,
        visibility_frag_glsl_t::slangModule);
    desc.set_shaders(std::move(vertex_shader), std::move(fragment_shader));
    desc.depth_stencil.depth_test_enabled = true;
    desc.depth_stencil.depth_write_enabled = true;
    desc.depth_stencil.depth_compare_op = Corona::Horizon::CompareOp::LessOrEqual;
    desc.rasterizer.cull_mode = Corona::Horizon::CullMode::None;
    desc.blend.attachments = {Corona::Horizon::BlendStateDesc::opaque_attachment()};
    desc.depth_attachment =
        Corona::Horizon::DepthAttachmentDesc::with_format(Corona::Horizon::Format::D32);
    return desc;
}
constexpr char kMouseIconRelativePath[] = "assets/icon/mouse_icon.png";

struct CursorIconPixels {
    std::vector<unsigned char> rgba;
    int width = 0;
    int height = 0;
};

std::filesystem::path find_mouse_icon_path() {
    std::error_code ec;
    auto current = std::filesystem::current_path(ec);
    if (!ec) {
        for (auto dir = current; !dir.empty(); dir = dir.parent_path()) {
            auto candidate = dir / kMouseIconRelativePath;
            if (std::filesystem::exists(candidate, ec) && !ec) {
                return candidate;
            }
            ec.clear();
            if (dir == dir.parent_path()) {
                break;
            }
        }
    }
    return std::filesystem::path(kMouseIconRelativePath);
}

std::optional<CursorIconPixels> load_mouse_icon_pixels() {
    const auto icon_path = find_mouse_icon_path();
    const auto image_id = Corona::Resource::ResourceManager::get_instance().import_sync(icon_path);
    if (image_id == Corona::Resource::IResource::INVALID_UID) {
        CFW_LOG_WARNING("Optics cursor icon load failed: {}", icon_path.string());
        return std::nullopt;
    }

    auto image = Corona::Resource::ResourceManager::get_instance().acquire_read<Corona::Resource::Image>(image_id);
    if (!image || image->get_width() <= 0 || image->get_height() <= 0 || image->get_data() == nullptr) {
        CFW_LOG_WARNING("Optics cursor icon data invalid: {}", icon_path.string());
        return std::nullopt;
    }

    CursorIconPixels pixels;
    pixels.width = image->get_width();
    pixels.height = image->get_height();
    const int channels = image->get_channels();
    const auto pixel_count = static_cast<size_t>(pixels.width) * static_cast<size_t>(pixels.height);
    pixels.rgba.resize(pixel_count * 4);
    const unsigned char* src = image->get_data();
    if (channels == 4) {
        std::copy(src, src + pixel_count * 4, pixels.rgba.begin());
    } else if (channels == 3) {
        for (size_t i = 0; i < pixel_count; ++i) {
            pixels.rgba[i * 4 + 0] = src[i * 3 + 0];
            pixels.rgba[i * 4 + 1] = src[i * 3 + 1];
            pixels.rgba[i * 4 + 2] = src[i * 3 + 2];
            pixels.rgba[i * 4 + 3] = 255;
        }
    } else if (channels == 1) {
        for (size_t i = 0; i < pixel_count; ++i) {
            pixels.rgba[i * 4 + 0] = src[i];
            pixels.rgba[i * 4 + 1] = src[i];
            pixels.rgba[i * 4 + 2] = src[i];
            pixels.rgba[i * 4 + 3] = 255;
        }
    } else {
        CFW_LOG_WARNING("Optics cursor icon has unsupported channel count: {}", channels);
        return std::nullopt;
    }
    return pixels;
}

[[nodiscard]] std::string normalize_scene_path_key(const std::string& raw_path) {
    if (raw_path.empty()) {
        return {};
    }

    std::error_code ec;
    std::filesystem::path path = std::filesystem::u8path(raw_path);
    auto normalized = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        ec.clear();
        normalized = path.is_absolute() ? path : std::filesystem::absolute(path, ec);
        if (ec) {
            normalized = path;
        }
    }
    auto key = normalized.lexically_normal().generic_string();
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
#endif
    return key;
}

[[nodiscard]] bool has_external_live_bindings_for_scene(const std::string& scene_path) {
    const auto target_key = normalize_scene_path_key(scene_path);
    if (target_key.empty()) {
        return false;
    }

    auto& hub = Corona::SharedDataHub::instance();
    for (auto scene_it = hub.scene_storage().cbegin(); scene_it != hub.scene_storage().cend(); ++scene_it) {
        const auto& scene_dev = *scene_it;
        if (!scene_dev.enabled) {
            continue;
        }
        for (auto actor_handle : scene_dev.actor_handles) {
            const auto binding = hub.external_vision_binding(actor_handle);
            if (!binding) {
                continue;
            }
            if (normalize_scene_path_key(binding->source_path) == target_key) {
                return true;
            }
        }
    }
    return false;
}

#ifdef CORONA_ENABLE_VISION
constexpr uint64_t kVisionRuntimeIdleEvictFrames = 240;

[[nodiscard]] ::vision::float4x4 corona_transform_to_vision_o2w(
    const Corona::ModelTransform& transform) {
    const ktm::fmat4x4 corona_mat = transform.compute_matrix();
    ::vision::float4x4 o2w = ::vision::make_float4x4(1.f);
    // Corona/Native uses +Z-forward left-handed coordinates. Vision uses
    // -Z-forward coordinates, so convert object transforms by F * M * F where
    // F = diag(1, 1, -1, 1), matching the built-in Vision geometry adapter.
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float value = corona_mat[col][row];
            if (row == 2) value = -value;
            if (col == 2) value = -value;
            o2w[col][row] = value;
        }
    }
    return o2w;
}

void mix_hash(std::size_t& sig, std::size_t value) {
    sig ^= value + 0x9e3779b97f4a7c15ULL + (sig << 6) + (sig >> 2);
}

void mix_hash_float(std::size_t& sig, float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float must be 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    mix_hash(sig, static_cast<std::size_t>(bits));
}

[[nodiscard]] std::size_t external_live_transform_signature(
    const Corona::ModelTransform& transform,
    int shape_index) {
    std::size_t sig = 0;
    mix_hash(sig, static_cast<std::size_t>(shape_index));
    mix_hash_float(sig, transform.position.x);
    mix_hash_float(sig, transform.position.y);
    mix_hash_float(sig, transform.position.z);
    mix_hash_float(sig, transform.euler_rotation.x);
    mix_hash_float(sig, transform.euler_rotation.y);
    mix_hash_float(sig, transform.euler_rotation.z);
    mix_hash_float(sig, transform.scale.x);
    mix_hash_float(sig, transform.scale.y);
    mix_hash_float(sig, transform.scale.z);
    return sig;
}

[[nodiscard]] int external_live_shape_index(const Corona::ExternalVisionBindingDevice& binding) {
    if (binding.shape_index >= 0) {
        return binding.shape_index;
    }

    constexpr std::string_view prefix = "/scene/shapes/";
    if (binding.json_path.rfind(prefix, 0) != 0) {
        return -1;
    }
    try {
        return std::stoi(binding.json_path.substr(prefix.size()));
    } catch (...) {
        return -1;
    }
}

struct ExternalLiveResolvedTransform {
    int shape_index{-1};
    std::size_t signature{0};
    ::vision::float4x4 o2w{};
};

[[nodiscard]] std::optional<ExternalLiveResolvedTransform> resolve_external_live_transform(
    std::uintptr_t actor_handle,
    const Corona::ExternalVisionBindingDevice& binding) {
    const int shape_index = external_live_shape_index(binding);
    if (actor_handle == 0 || shape_index < 0) {
        return std::nullopt;
    }

    auto& hub = Corona::SharedDataHub::instance();
    auto actor = hub.actor_storage().try_acquire_read(actor_handle);
    if (!actor) {
        return std::nullopt;
    }

    for (auto profile_handle : actor->profile_handles) {
        auto profile = hub.profile_storage().try_acquire_read(profile_handle);
        if (!profile) {
            continue;
        }

        std::uintptr_t geometry_handle = profile->geometry_handle;
        if (geometry_handle == 0 && profile->optics_handle != 0) {
            if (auto optics = hub.optics_storage().try_acquire_read(profile->optics_handle)) {
                geometry_handle = optics->geometry_handle;
            }
        }
        if (geometry_handle == 0) {
            continue;
        }

        auto geometry = hub.geometry_storage().try_acquire_read(geometry_handle);
        if (!geometry || geometry->transform_handle == 0) {
            continue;
        }

        auto transform = hub.model_transform_storage().try_acquire_read(geometry->transform_handle);
        if (!transform) {
            continue;
        }

        ExternalLiveResolvedTransform result;
        result.shape_index = shape_index;
        result.signature = external_live_transform_signature(*transform, shape_index);
        result.o2w = corona_transform_to_vision_o2w(*transform);
        return result;
    }

    return std::nullopt;
}
#endif

void apply_pending_camera_moves() {
    auto& hub = Corona::SharedDataHub::instance();
    auto moves = hub.drain_camera_moves();
    if (moves.empty()) {
        return;
    }

    auto& camera_storage = hub.camera_storage();
    for (const auto& move : moves) {
        if (auto camera = camera_storage.try_acquire_write(move.camera_handle)) {
            camera->position = move.position;
            camera->forward = move.forward;
            camera->world_up = move.world_up;
            camera->fov = move.fov;
        }
    }
}

void apply_pending_camera_viewport_updates() {
    auto& hub = Corona::SharedDataHub::instance();
    auto updates = hub.drain_camera_viewport_updates();
    if (updates.empty()) {
        return;
    }

    auto& camera_storage = hub.camera_storage();
    for (const auto& update : updates) {
        if (auto camera = camera_storage.acquire_write(update.camera_handle)) {
            camera->surface = update.surface;
            camera->follows_default_surface = false;
            camera->view_open = update.view_open;
            camera->viewport_rect_active = true;
            camera->view_x = update.x;
            camera->view_y = update.y;
            camera->view_width = update.width;
            camera->view_height = update.height;
            const auto render_width =
                static_cast<std::uint32_t>(std::max(update.render_width, 1));
            const auto render_height =
                static_cast<std::uint32_t>(std::max(update.render_height, 1));
            camera->width = render_width;
            camera->height = render_height;
            camera->aspect = static_cast<float>(render_width) /
                             static_cast<float>(render_height);
        }
    }
}

void apply_pending_camera_state_updates() {
    auto& hub = Corona::SharedDataHub::instance();
    auto updates = hub.drain_camera_state_updates();
    if (updates.empty()) {
        return;
    }

    auto& camera_storage = hub.camera_storage();
    for (const auto& update : updates) {
        if (auto camera = camera_storage.acquire_write(update.camera_handle)) {
            if (Corona::has_camera_state_field(
                    update.fields, Corona::CameraStateUpdateField::Surface)) {
                camera->surface = update.surface;
                camera->follows_default_surface = false;
            }
            if (Corona::has_camera_state_field(
                    update.fields, Corona::CameraStateUpdateField::Size)) {
                camera->width = update.width;
                camera->height = update.height;
                camera->aspect = static_cast<float>(update.width) /
                                 static_cast<float>(update.height);
            }
            if (Corona::has_camera_state_field(
                    update.fields, Corona::CameraStateUpdateField::OutputMode)) {
                camera->output_mode = update.output_mode;
            }
            if (Corona::has_camera_state_field(
                    update.fields, Corona::CameraStateUpdateField::RenderBackend)) {
                camera->render_backend = update.render_backend;
            }
            if (Corona::has_camera_state_field(
                    update.fields, Corona::CameraStateUpdateField::VisionRenderMode)) {
                camera->vision_render_mode = update.vision_render_mode;
            }
            if (Corona::has_camera_state_field(
                    update.fields, Corona::CameraStateUpdateField::ViewState)) {
                camera->view_open = update.view_open;
                camera->view_x = update.view_x;
                camera->view_y = update.view_y;
                camera->view_width = update.view_width;
                camera->view_height = update.view_height;
                camera->move_speed = update.move_speed;
            }
        }
    }
}

void apply_pending_camera_releases() {
    auto& hub = Corona::SharedDataHub::instance();
    for (const auto& release : hub.drain_camera_releases()) {
        if (release.actor_pick_handle != 0) {
            hub.actor_pick_storage().deallocate(release.actor_pick_handle);
        }
        hub.camera_storage().deallocate(release.camera_handle);
    }
}

[[nodiscard]] ktm::fmat4x4 make_orthographic_lh(float width,
                                                float height,
                                                float near_plane,
                                                float far_plane) {
    ktm::fmat4x4 proj = ktm::fmat4x4::from_eye();
    const float depth = std::max(far_plane - near_plane, 1e-4f);

    proj[0][0] = 2.0f / std::max(width, 1e-4f);
    proj[1][1] = -2.0f / std::max(height, 1e-4f);
    proj[2][2] = 1.0f / depth;
    proj[3][2] = -near_plane / depth;
    return proj;
}

[[nodiscard]] ktm::fmat4x4 make_camera_basis_matrix(const Corona::CameraDevice& camera) {
    const ktm::fvec3 forward = ktm::normalize(camera.forward);
    ktm::fvec3 right = ktm::cross(camera.world_up, forward);
    if (ktm::length(right) < 1e-5f) {
        right = ktm::fvec3{1.0f, 0.0f, 0.0f};
    } else {
        right = ktm::normalize(right);
    }
    const ktm::fvec3 up = ktm::normalize(ktm::cross(forward, right));

    ktm::fmat4x4 basis = ktm::fmat4x4::from_eye();
    basis[0][0] = right.x;
    basis[0][1] = right.y;
    basis[0][2] = right.z;
    basis[1][0] = up.x;
    basis[1][1] = up.y;
    basis[1][2] = up.z;
    basis[2][0] = forward.x;
    basis[2][1] = forward.y;
    basis[2][2] = forward.z;
    basis[3][0] = camera.position.x;
    basis[3][1] = camera.position.y;
    basis[3][2] = camera.position.z;
    return basis;
}

[[nodiscard]] ktm::fmat4x4 multiply_ktm_mat4(const ktm::fmat4x4& lhs,
                                             const ktm::fmat4x4& rhs) {
    ktm::fmat4x4 out{};
    for (std::size_t col = 0; col < 4; ++col) {
        for (std::size_t row = 0; row < 4; ++row) {
            out[col][row] = lhs[0][row] * rhs[col][0] +
                            lhs[1][row] * rhs[col][1] +
                            lhs[2][row] * rhs[col][2] +
                            lhs[3][row] * rhs[col][3];
        }
    }
    return out;
}

[[nodiscard]] Corona::Horizon::ImageUsageFlags optics_storage_image_usage() {
    return Corona::Horizon::ImageUsageFlags::Storage |
           Corona::Horizon::ImageUsageFlags::ColorAttachment |
           Corona::Horizon::ImageUsageFlags::Sampled |
           Corona::Horizon::ImageUsageFlags::TransferSrc |
           Corona::Horizon::ImageUsageFlags::TransferDst;
}

[[nodiscard]] Corona::Horizon::HardwareImage make_storage_image(
    uint32_t width,
    uint32_t height,
    Corona::Horizon::Format format,
    std::string_view name) {
    return Corona::Horizon::HardwareImage(Corona::Horizon::HardwareImageDesc::texture_2d(
        width,
        height,
        format,
        optics_storage_image_usage(),
        std::string(name)));
}

[[nodiscard]] Corona::Horizon::HardwareImage make_depth_image(
    uint32_t width,
    uint32_t height,
    std::string_view name) {
    auto image = Corona::Horizon::HardwareImage(
        Corona::Horizon::HardwareImageDesc::depth_attachment(
            width,
            height,
            Corona::Horizon::Format::D32,
            std::string(name)));
    image.set_clear_depth(1.0f, 0);
    return image;
}

template <typename T>
[[nodiscard]] Corona::Horizon::HardwareBuffer make_storage_buffer(uint64_t count,
                                                                  std::string_view name) {
    Corona::Horizon::HardwareBufferDesc desc;
    desc.element_count = count;
    desc.element_size = static_cast<uint32_t>(sizeof(T));
    desc.usage = Corona::Horizon::BufferUsageFlags::TransferSrc |
                 Corona::Horizon::BufferUsageFlags::TransferDst |
                 Corona::Horizon::BufferUsageFlags::Storage;
    desc.debug_name = std::string(name);
    (void)desc.byte_size();
    return Corona::Horizon::HardwareBuffer(desc);
}

template <typename T>
bool write_object_bytes(const Corona::Horizon::HardwareBuffer& buffer, const T& value) {
    return buffer.write_bytes(std::as_bytes(std::span<const T>(&value, 1)));
}

template <typename T>
bool write_array_bytes(const Corona::Horizon::HardwareBuffer& buffer,
                       const T* data,
                       std::size_t count) {
    return buffer.write_bytes(std::as_bytes(std::span<const T>(data, count)));
}

[[nodiscard]] bool has_native_local_correction(const Corona::GeometryDevice& geom) {
    const auto& offset = geom.native_local_correction_offset;
    return std::abs(geom.native_local_correction_scale - 1.0f) > 1e-6f ||
           std::abs(offset.x) > 1e-6f ||
           std::abs(offset.y) > 1e-6f ||
           std::abs(offset.z) > 1e-6f;
}

[[nodiscard]] ktm::fmat4x4 apply_native_local_correction(
    const ktm::fmat4x4& model_matrix,
    const Corona::GeometryDevice& geom) {
    if (!has_native_local_correction(geom)) {
        return model_matrix;
    }

    ktm::fmat4x4 correction = ktm::fmat4x4::from_eye();
    correction[0][0] = geom.native_local_correction_scale;
    correction[1][1] = geom.native_local_correction_scale;
    correction[2][2] = geom.native_local_correction_scale;
    correction[3][0] = geom.native_local_correction_offset.x;
    correction[3][1] = geom.native_local_correction_offset.y;
    correction[3][2] = geom.native_local_correction_offset.z;
    return multiply_ktm_mat4(model_matrix, correction);
}

bool collect_actor_instances_for_visibility(
    const Corona::SceneDevice& scene,
    Corona::Horizon::RasterizerPipeline<visibility_vert_glsl_t, visibility_frag_glsl_t>& target_visibility,
    uint32_t target_vp_descriptor,
    bool follow_camera_pass,
    const ktm::fmat4x4* camera_basis,
    RenderInstanceBatch& batch) {
    batch.clear();

    auto& hub = Corona::SharedDataHub::instance();
    auto& actor_storage = hub.actor_storage();
    auto& profile_storage = hub.profile_storage();
    auto& optics_storage = hub.optics_storage();
    auto& geom_storage = hub.geometry_storage();
    auto& transform_storage = hub.model_transform_storage();

    bool has_instances = false;
    uint32_t object_id = 1;
    for (auto actor_handle : scene.actor_handles) {
        auto actor = actor_storage.try_acquire_read(actor_handle);
        if (!actor) {
            ++object_id;
            continue;
        }

        if (actor->follow_camera != follow_camera_pass) {
            ++object_id;
            continue;
        }

        for (auto profile_handle : actor->profile_handles) {
            auto profile = profile_storage.try_acquire_read(profile_handle);
            if (!profile || profile->optics_handle == 0) continue;

            auto optics_acc = optics_storage.try_acquire_read(profile->optics_handle);
            if (!optics_acc) continue;
            const auto& optics = *optics_acc;

            if (!optics.visible) {
                ++object_id;
                continue;
            }
            // Mesh texture descriptors are non-const, so the geometry storage must be
            // write-acquired here. Skipping on transient lock contention causes flicker.
            if (auto geom = geom_storage.try_acquire_write(optics.geometry_handle)) {
                ktm::fmat4x4 model_matrix{ktm::fmat4x4::from_eye()};
                if (auto transform = transform_storage.try_acquire_read(geom->transform_handle)) {
                    model_matrix = transform->compute_matrix();
                    model_matrix = apply_native_local_correction(model_matrix, *geom);
                    if (camera_basis != nullptr) {
                        model_matrix = multiply_ktm_mat4(*camera_basis, model_matrix);
                    }
                }

                for (auto& m : geom->mesh_handles) {
                    auto material_id = static_cast<uint32_t>(batch.materials.size());
                    {
                        Hardware::MaterialInfo mat_info{};

                        const float lighting_enabled = optics.bEnableLighting ? 1.0f : 0.0f;
                        mat_info.textureDescriptor = m.textureBuffer ? m.textureBuffer.storeSampledDescriptor() : 0;

                        if (optics.bEnableLighting) {
                            mat_info.metallic = optics.metallic;
                            mat_info.roughness = optics.roughness;
                            mat_info.subsurface = optics.subsurface;
                            mat_info.specular = optics.specular;
                            mat_info.specularTint = optics.specularTint;
                            mat_info.anisotropic = optics.anisotropic;
                            mat_info.sheen = optics.sheen;
                            mat_info.sheenTint = optics.sheenTint;
                            mat_info.clearcoat = optics.clearcoat;
                            mat_info.clearcoatGloss = optics.clearcoatGloss;
                        } else {
                            mat_info.metallic = 0.0f;
                            mat_info.roughness = 1.0f;
                            mat_info.subsurface = 0.0f;
                            mat_info.specular = 0.0f;
                            mat_info.specularTint = 0.0f;
                            mat_info.anisotropic = 0.0f;
                            mat_info.sheen = 0.0f;
                            mat_info.sheenTint = 0.0f;
                            mat_info.clearcoat = 0.0f;
                            mat_info.clearcoatGloss = 0.0f;
                        }

                        mat_info.lightingEnabled = lighting_enabled;
                        mat_info.materialColor = ktm::fvec4{
                            m.materialColor[0], m.materialColor[1],
                            m.materialColor[2], m.materialColor[3]};
                        batch.materials.push_back(mat_info);
                    }

                    auto instance_id = static_cast<uint32_t>(batch.instances.size());
                    {
                        Hardware::InstanceInfo inst{};
                        inst.modelMatrix = model_matrix;
                        inst.vertexBufferIndex =
                            m.vertexStorageBuffer ? m.vertexStorageBuffer.storeDescriptor() : 0;
                        inst.indexBufferIndex =
                            m.indexStorageBuffer ? m.indexStorageBuffer.storeDescriptor() : 0;
                        inst.materialID = material_id;
                        inst.objectID = object_id;
                        batch.instances.push_back(inst);
                        batch.actorHandles.push_back(actor_handle);
                        has_instances = true;
                    }

                    target_visibility[visibility_vert_glsl_t::pushConsts::modelMatrix] =
                        upload_value(model_matrix);
                    target_visibility[visibility_vert_glsl_t::pushConsts::uniformBufferIndex] =
                        target_vp_descriptor;
                    target_visibility[visibility_vert_glsl_t::pushConsts::instanceID] =
                        instance_id + 1;
                    target_visibility[visibility_frag_glsl_t::pushConsts::textureIndex] =
                        m.textureBuffer ? m.textureBuffer.storeSampledDescriptor() : static_cast<uint32_t>(0);
                    target_visibility.record(m.indexBuffer, m.vertexBuffer);
                }
            }
            ++object_id;
        }
    }
    return has_instances;
}

void upload_instance_tables(const RenderInstanceBatch& batch,
                            Corona::Horizon::HardwareBuffer& instance_buffer,
                            Corona::Horizon::HardwareBuffer& material_buffer) {
    if (!batch.instances.empty()) {
        (void)write_array_bytes(instance_buffer, batch.instances.data(), batch.instances.size());
    }
    if (!batch.materials.empty()) {
        (void)write_array_bytes(material_buffer, batch.materials.data(), batch.materials.size());
    }
}

#ifdef CORONA_ENABLE_VISION
vision::Device* visionDevicePtr = nullptr;

[[nodiscard]] auto make_default_vision_project_desc() -> vision::ProjectDesc {
    // Each *Desc has in-class default initializers; the overridden
    // init(const ParameterSet&) is only used for JSON-driven configuration.
    // The ParameterSet MUST wrap an empty JSON *object* (not a default-
    // constructed null): NodeDesc::set_parameter() asserts is_object() and the
    // various init() helpers call ps.value("param", ...)/set_parameter(), which
    // raise nlohmann type_errors on a null payload. Because nlohmann is built
    // with JSON_NOEXCEPTION here, that surfaces as abort()/SIGABRT instead of a
    // catchable std::exception, crashing before any pipeline plugin is created.
    const vision::ParameterSet empty_ps{vision::DataWrap::object()};
    vision::ProjectDesc project_desc;
    project_desc.pipeline_desc.init(empty_ps);
    project_desc.renderer_desc.sampler_desc.init(empty_ps);
    project_desc.renderer_desc.spectrum_desc.init(empty_ps);
    project_desc.renderer_desc.light_sampler_desc.init(empty_ps);
    project_desc.renderer_desc.integrator_desc.init(empty_ps);
    project_desc.renderer_desc.warper_desc.init(empty_ps);
    project_desc.renderer_desc.render_setting.init(empty_ps);
    project_desc.scene_desc.sensor_desc.init(empty_ps);
    project_desc.output_desc.init(empty_ps);
    return project_desc;
}

void bind_pipeline_scene_resource_early(
    vision::Pipeline& pipeline,
    const std::shared_ptr<Corona::Systems::Vision::VisionSceneResource>& scene_resource) {
    if (!scene_resource) {
        return;
    }
    if (scene_resource->has_logical_scene()) {
        return;
    }

    auto logical_scene = std::make_shared<vision::SceneData>();
    scene_resource->set_logical_scene(logical_scene);
    pipeline.bind_shared_scene_data(std::move(logical_scene));
}

[[nodiscard]] auto create_vision_pipeline(
    Corona::CameraVisionRenderMode mode,
    const std::shared_ptr<Corona::Systems::Vision::VisionSceneResource>& scene_resource = {})
    -> ocarina::SP<vision::Pipeline> {
    auto project_desc = make_default_vision_project_desc();
    project_desc.output_desc.denoise =
        Corona::Systems::Vision::vision_render_mode_uses_denoise(mode);
    auto pipeline = vision::Node::create_shared<vision::Pipeline>(project_desc.pipeline_desc);
    if (!pipeline) {
        return {};
    }
    bind_pipeline_scene_resource_early(*pipeline, scene_resource);
    pipeline->init_project(project_desc);
    pipeline->init_postprocessor(project_desc.renderer_desc.denoiser_desc);
    pipeline->init();
    pipeline->set_output_denoise(project_desc.output_desc.denoise);
    return pipeline;
}

void prepare_enabled_denoiser_for_runtime_switch(vision::Pipeline& pipeline) {
    auto* integrator = pipeline.renderer().integrator().get();
    auto* illum = dynamic_cast<vision::IlluminationIntegrator*>(integrator);
    if (illum == nullptr) {
        return;
    }
    auto* denoiser = illum->denoiser();
    if (denoiser == nullptr || !denoiser->enabled()) {
        return;
    }
    denoiser->prepare();
    denoiser->compile();
    pipeline.upload_bindless_array();
}

[[nodiscard]] auto select_scene_camera_handle(const Corona::SceneDevice& scene) -> std::uintptr_t {
    if (scene.active_camera_handle != 0 &&
        std::find(scene.camera_handles.begin(),
                  scene.camera_handles.end(),
                  scene.active_camera_handle) != scene.camera_handles.end()) {
        return scene.active_camera_handle;
    }
    return scene.camera_handles.empty() ? 0 : scene.camera_handles.front();
}

struct VisionModeSelection {
    bool has_visible_camera{false};
    Corona::CameraVisionRenderMode mode{Corona::CameraVisionRenderMode::PathTracing};
    std::uintptr_t selected_camera{0};
    bool conflict{false};
    std::size_t conflict_signature{0};
    std::string conflict_summary;
};

[[nodiscard]] VisionModeSelection select_visible_vision_render_mode() {
    VisionModeSelection selection;
    std::hash<std::string> hash_string;
    std::string signature_source;

    for (const auto& scene : Corona::SharedDataHub::instance().scene_storage()) {
        if (!scene.enabled) continue;
        for (const auto camera_handle : scene.camera_handles) {
            auto camera =
                Corona::SharedDataHub::instance().camera_storage().try_acquire_read(camera_handle);
            if (!camera || camera->render_backend != Corona::CameraRenderBackend::Vision ||
                camera->surface == nullptr) {
                continue;
            }

            const auto mode = camera->vision_render_mode;
            if (!selection.has_visible_camera) {
                selection.has_visible_camera = true;
                selection.mode = mode;
                selection.selected_camera = camera_handle;
            } else if (mode != selection.mode) {
                selection.conflict = true;
            }

            if (!signature_source.empty()) {
                signature_source.push_back(';');
            }
            signature_source.append(std::to_string(camera_handle));
            signature_source.push_back('=');
            signature_source.append(
                std::string(Corona::Systems::Vision::vision_render_mode_name(mode)));

            if (!selection.conflict_summary.empty()) {
                selection.conflict_summary.append(", ");
            }
            selection.conflict_summary.append("camera ");
            selection.conflict_summary.append(std::to_string(camera_handle));
            selection.conflict_summary.append("=");
            selection.conflict_summary.append(
                std::string(Corona::Systems::Vision::vision_render_mode_name(mode)));
        }
    }

    selection.conflict_signature = hash_string(signature_source);
    return selection;
}

void log_vision_pipeline_diagnostics(vision::Pipeline& pipeline,
                                     const std::string& label) {
    auto* fb = pipeline.frame_buffer();
    if (fb == nullptr) {
        CFW_LOG_WARNING("OpticsSystem: Vision pipeline {} has no framebuffer", label);
        return;
    }

    const auto pixel_res = fb->resolution();
    const auto raytracing_res = fb->raytracing_resolution();
    const bool lightfield =
        dynamic_cast<const vision::ILightFieldFrameBuffer*>(fb) != nullptr;
    const bool output_denoise = pipeline.output_desc().denoise;

    std::string denoiser_type = "none";
    bool denoiser_enabled = false;
    bool denoiser_supports_lightfield = false;
    if (auto* integrator = pipeline.renderer().integrator().get()) {
        if (auto* illum = dynamic_cast<vision::IlluminationIntegrator*>(integrator)) {
            if (auto* denoiser = illum->denoiser()) {
                denoiser_type = std::string(denoiser->impl_type());
                denoiser_enabled = denoiser->enabled();
                denoiser_supports_lightfield = denoiser->supports_lightfield();
            }
        }
    }

    const bool ssat_active = lightfield &&
                             denoiser_type == "SSAT" &&
                             denoiser_enabled &&
                             output_denoise &&
                             denoiser_supports_lightfield;

    CFW_LOG_INFO(
        "OpticsSystem: Vision pipeline {} framebuffer={}, pixel_res=({}, {}), "
        "raytracing_res=({}, {}), lightfield={}, denoiser={}, "
        "denoiser_enabled={}, output_denoise={}, SSAT active={}",
        label,
        std::string(fb->impl_type()),
        pixel_res.x,
        pixel_res.y,
        raytracing_res.x,
        raytracing_res.y,
        lightfield,
        denoiser_type,
        denoiser_enabled,
        output_denoise,
        ssat_active);
}

std::string describe_vision_pipeline_key(
    const Corona::Systems::Vision::VisionPipelineKey& key) {
    std::string result = "source=";
    result.append(std::string(Corona::Systems::Vision::vision_pipeline_source_name(key.source)));
    result.append(", mode=");
    result.append(std::string(Corona::Systems::Vision::vision_render_mode_name(key.mode)));
    result.append(", path=");
    result.append(key.scene_path.empty() ? "<engine-built>" : key.scene_path);
    return result;
}

std::string describe_vision_scene_resource_key(
    const Corona::Systems::Vision::VisionSceneResourceKey& key) {
    std::string result = "source=";
    result.append(std::string(Corona::Systems::Vision::vision_pipeline_source_name(key.source)));
    result.append(", path=");
    result.append(key.source_path_key.empty() ? "<engine-built>" : key.source_path_key);
    return result;
}

std::array<float, 16> flatten_vision_matrix(const vision::float4x4& matrix) {
    std::array<float, 16> result{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            result[static_cast<std::size_t>(col * 4 + row)] = matrix[col][row];
        }
    }
    return result;
}

vision::float4x4 unflatten_vision_matrix(const std::array<float, 16>& values) {
    auto result = vision::make_float4x4(1.f);
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            result[col][row] = values[static_cast<std::size_t>(col * 4 + row)];
        }
    }
    return result;
}

void sync_logical_instances_from_pipeline_scene(
    Corona::Systems::Vision::VisionSceneResource& scene_resource,
    vision::Scene& scene) {
    std::vector<Corona::Systems::Vision::VisionLogicalInstanceRecord> records;
    auto& groups = scene.groups();
    for (std::size_t group_index = 0; group_index < groups.size(); ++group_index) {
        auto& group = groups[group_index];
        if (!group) {
            continue;
        }
        group->for_each([&](vision::SP<vision::ShapeInstance> instance, vision::uint instance_index) {
            if (!instance) {
                return;
            }
            records.push_back({
                .key = {.shape_index = static_cast<int>(group_index),
                        .instance_index = static_cast<int>(instance_index)},
                .actor_handle = 0,
                .transform_signature = 0,
                .object_to_world = flatten_vision_matrix(instance->handle().o2w()),
            });
        });
    }
    scene_resource.replace_logical_instances(std::move(records));
}

void apply_logical_instances_to_pipeline_scene(
    const Corona::Systems::Vision::VisionSceneResource& scene_resource,
    vision::Scene& scene) {
    auto& groups = scene.groups();
    for (const auto& [key, record] : scene_resource.logical_instances) {
        if (key.shape_index < 0 || key.instance_index < 0) {
            continue;
        }
        const auto group_index = static_cast<std::size_t>(key.shape_index);
        if (group_index >= groups.size() || !groups[group_index]) {
            continue;
        }
        auto& group = groups[group_index];
        bool applied = false;
        group->for_each([&](vision::SP<vision::ShapeInstance> instance,
                            vision::uint instance_index) {
            if (applied || !instance ||
                static_cast<int>(instance_index) != key.instance_index) {
                return;
            }
            instance->set_o2w(unflatten_vision_matrix(record.object_to_world));
            instance->init_aabb();
            applied = true;
        });
    }

    for (auto& group : groups) {
        if (!group) {
            continue;
        }
        group->aabb = vision::Box3f{};
        group->for_each([&](vision::SP<vision::ShapeInstance> instance, vision::uint) {
            if (instance) {
                group->aabb.extend(instance->aabb);
            }
        });
    }
    scene.fill_instances();
}

void bind_pipeline_scene_gpu_resource(
    vision::Pipeline& pipeline,
    Corona::Systems::Vision::VisionSceneResource& scene_resource,
    Corona::Systems::Vision::VisionPipelineSource source,
    Corona::CameraVisionRenderMode mode,
    const std::string& scene_path) {
    if (!scene_resource.has_logical_scene()) {
        scene_resource.set_logical_scene(pipeline.shared_scene_data());
    }

    const bool created_gpu_resource = !pipeline.scene().geometry().has_gpu_resource();
    auto scene_gpu_resource = pipeline.scene().geometry().gpu_resource();
    if (!scene_gpu_resource) {
        scene_gpu_resource = std::make_shared<vision::GeometryGpuResource>(
            pipeline.device());
    }
    if (created_gpu_resource) {
        CFW_LOG_INFO(
            "OpticsSystem: created per-runtime Vision scene GPU view "
            "(source={}, mode={}, path={})",
            std::string(Corona::Systems::Vision::vision_pipeline_source_name(source)),
            std::string(Corona::Systems::Vision::vision_render_mode_name(mode)),
            scene_path);
    } else {
        CFW_LOG_INFO(
            "OpticsSystem: reusing per-runtime Vision scene GPU view "
            "(source={}, mode={}, path={})",
            std::string(Corona::Systems::Vision::vision_pipeline_source_name(source)),
            std::string(Corona::Systems::Vision::vision_render_mode_name(mode)),
            scene_path);
    }
    pipeline.scene().bind_geometry_gpu_resource(std::move(scene_gpu_resource));
    if (scene_resource.logical_instance_count() == 0u) {
        sync_logical_instances_from_pipeline_scene(scene_resource, pipeline.scene());
    } else {
        apply_logical_instances_to_pipeline_scene(scene_resource, pipeline.scene());
    }
}

// Loads a Vision scene from disk and brings it to a renderable state, mirroring
// the reference snippet (import_scene -> init -> prepare -> prepare_view_texture).
// Resolves relative texture/mesh references against the scene's own folder.
// Returns an empty pointer if the file is missing or import fails so the caller
// can skip without crashing.
[[nodiscard]] auto import_vision_scene_from_file(const std::filesystem::path& scene_path,
                                                Corona::CameraVisionRenderMode mode,
                                                const std::shared_ptr<
                                                    Corona::Systems::Vision::VisionSceneResource>&
                                                    scene_resource,
                                                Corona::Systems::Vision::VisionPipelineSource source)
    -> ocarina::SP<vision::Pipeline> {
    std::error_code ec;
    if (!std::filesystem::exists(scene_path, ec)) {
        CFW_LOG_ERROR("OpticsSystem: Vision scene not found: {}", scene_path.string());
        return {};
    }

    auto project_data = vision::create_json_from_file(scene_path);
    Corona::Systems::Vision::configure_vision_scene_for_mode(project_data, mode);

    // Resolve relative texture/mesh references against the scene's own folder.
    const auto scene_folder = scene_path.parent_path();
    vision::Global::instance().set_scene_path(scene_folder);

    vision::ProjectDesc project_desc;
    project_desc.scene_path = scene_folder;
    project_desc.init(project_data);

    auto pipeline = vision::Node::create_shared<vision::Pipeline>(project_desc.pipeline_desc);
    if (!pipeline) {
        CFW_LOG_ERROR("OpticsSystem: Vision pipeline creation returned null for {}",
                      scene_path.string());
        return {};
    }
    bind_pipeline_scene_resource_early(*pipeline, scene_resource);
    pipeline->init_project(project_desc);
    if (scene_resource) {
        bind_pipeline_scene_gpu_resource(
            *pipeline, *scene_resource, source, mode, scene_path.string());
    }
    pipeline->init_postprocessor(project_desc.renderer_desc.denoiser_desc);
    pipeline->init();
    pipeline->set_output_denoise(project_desc.output_desc.denoise);
    pipeline->prepare();
    // prepare() does not create FrameBuffer::view_texture_; the render path tone
    // maps into it and we later read it back, so create it explicitly here.
    pipeline->frame_buffer()->prepare_view_texture();
    return pipeline;
}

#ifdef CORONA_VISION_IMPORT_DEMO
// Absolute path to a known-good Vision scene used purely to verify that the
// Vision backend can produce a picture in isolation (i.e. without the
// CoronaEngine->Vision scene-building adapters). Change this to point at any
// local *.json scene. Kept as a constant so it is trivial to edit/relocate.
constexpr const char* kVisionDemoScenePath =
    R"(E:\CoronaExample\test_vision\render_scene\cbox\vision_scene.json)";
#endif
#endif  // CORONA_ENABLE_VISION
}  // namespace

namespace Corona::Systems {

struct OpticsSystem::NativeViewResources {
    Horizon::HardwareImage visibility;
    Horizon::HardwareImage depth;
    std::optional<Horizon::RasterizerPipeline<visibility_vert_glsl_t, visibility_frag_glsl_t>>
        visibility_pipeline;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t last_used_frame = 0;
};

// per-camera UI overlay 中间产物（Native 与 Vision 共用，单一分配来源）。
struct OpticsSystem::UiViewResources {
    Horizon::HardwareImage ui_visibility;  ///< RGBA32_UINT StorageImage
    Horizon::HardwareImage ui_depth;       ///< D32_FLOAT DepthImage
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t last_used_frame = 0;
};

#ifdef CORONA_ENABLE_VISION
struct OpticsSystem::VisionPipelineRuntime {
    ocarina::SP<vision::Pipeline> pipeline;
    std::shared_ptr<VisionSceneResource> scene_resource;
    VisionPipelineSource source{VisionPipelineSource::EngineBuilt};
    std::string scene_path;
    Corona::CameraVisionRenderMode mode{Corona::CameraVisionRenderMode::PathTracing};
    uint64_t last_used_frame{0};
    uint64_t scene_gpu_transform_version{0};

    // Zero-copy path: shares Vision's pre-tonemap linear color buffer with Vulkan
    // and resolves it via the vision_resolve compute pass.
    std::unordered_map<std::uintptr_t, std::unique_ptr<Vision::VisionZeroCopyBridge>> bridges;
    std::unordered_set<std::uintptr_t> zero_copy_disabled;
    std::unordered_map<std::uintptr_t, Horizon::HardwareBuffer> readback_buffers;
    std::unordered_map<std::uintptr_t, std::vector<ocarina::float4>> readback_pixels;
    std::unordered_set<std::uintptr_t> retained_contexts;

    void commit_and_clear_contexts() noexcept {
        if (pipeline) {
            pipeline->commit_command();
        }
        clear_camera_runtime_state();
        if (pipeline) {
            pipeline->clear_view_contexts();
        }
    }

    void clear_camera_runtime_state() noexcept {
        bridges.clear();
        zero_copy_disabled.clear();
        readback_buffers.clear();
        readback_pixels.clear();
        retained_contexts.clear();
    }

    void bind_shared_scene_gpu_resource() {
        if (!pipeline || !scene_resource) {
            return;
        }
        const bool needs_geometry_prepare =
            !pipeline->scene().geometry().has_gpu_resource();
        bind_pipeline_scene_gpu_resource(
            *pipeline, *scene_resource, source, mode, scene_path);
        if (needs_geometry_prepare) {
            pipeline->prepare_geometry();
        }
        scene_gpu_transform_version = scene_resource->logical_transform_version;
    }

    void upload_shared_scene_transforms_if_needed() {
        if (!pipeline || !scene_resource ||
            scene_gpu_transform_version == scene_resource->logical_transform_version) {
            return;
        }
        apply_logical_instances_to_pipeline_scene(*scene_resource, pipeline->scene());
        pipeline->activate_view_context(0u);
        pipeline->update_geometry();
        pipeline->invalidate_all_view_contexts();
        scene_gpu_transform_version = scene_resource->logical_transform_version;
    }

    void reset_pipeline(ocarina::SP<vision::Pipeline> next_pipeline,
                        VisionPipelineSource next_source,
                        std::string next_scene_path,
                        Corona::CameraVisionRenderMode next_mode) {
        commit_and_clear_contexts();
        pipeline = std::move(next_pipeline);
        source = next_source;
        scene_path = std::move(next_scene_path);
        mode = next_mode;
        scene_gpu_transform_version = 0;
        bind_shared_scene_gpu_resource();
    }
};

struct VisibleVisionCamera {
    std::uintptr_t camera_handle{0};
    Corona::CameraDevice camera;
    Corona::SceneDevice scene;
};

OpticsSystem::VisionPipelineKey OpticsSystem::make_vision_pipeline_key(
    std::string scene_path,
    CameraVisionRenderMode mode,
    VisionPipelineSource source) const {
    if (source == VisionPipelineSource::EngineBuilt) {
        scene_path.clear();
    } else {
        scene_path = normalize_scene_path_key(scene_path);
    }
    return VisionPipelineKey{std::move(scene_path), mode, source};
}

OpticsSystem::VisionSceneResourceKey OpticsSystem::make_vision_scene_resource_key(
    std::string scene_path,
    VisionPipelineSource source) const {
    if (source == VisionPipelineSource::EngineBuilt) {
        scene_path.clear();
    } else {
        scene_path = normalize_scene_path_key(scene_path);
    }
    return VisionSceneResourceKey{std::move(scene_path), source};
}

std::shared_ptr<OpticsSystem::VisionSceneResource>
OpticsSystem::get_or_create_vision_scene_resource(
    const VisionSceneResourceKey& key,
    std::string display_source_path) {
    auto [it, inserted] = vision_scene_resources_.try_emplace(key);
    if (inserted || !it->second) {
        auto resource = std::make_shared<VisionSceneResource>();
        resource->key = key;
        resource->display_source_path = std::move(display_source_path);
        it->second = std::move(resource);
        CFW_LOG_INFO("OpticsSystem: created shared Vision scene resource ({})",
                     describe_vision_scene_resource_key(key));
    } else if (!display_source_path.empty() && it->second->display_source_path.empty()) {
        it->second->display_source_path = std::move(display_source_path);
    }
    return it->second;
}

void OpticsSystem::release_unused_vision_scene_resources() {
    for (auto it = vision_scene_resources_.begin(); it != vision_scene_resources_.end();) {
        if (it->second && it->second.use_count() == 1) {
            CFW_LOG_INFO("OpticsSystem: releasing unused shared Vision scene resource ({})",
                         describe_vision_scene_resource_key(it->first));
            it = vision_scene_resources_.erase(it);
            continue;
        }
        ++it;
    }
}

OpticsSystem::VisionPipelineRuntime& OpticsSystem::get_or_create_runtime(
    const VisionPipelineKey& key) {
    auto [it, inserted] = vision_runtimes_.try_emplace(key);
    if (inserted || !it->second) {
        it->second = std::make_unique<VisionPipelineRuntime>();
        it->second->scene_resource = get_or_create_vision_scene_resource(
            make_vision_scene_resource_key(key.scene_path, key.source),
            key.scene_path);
        it->second->source = key.source;
        it->second->scene_path = key.scene_path;
        it->second->mode = key.mode;
        CFW_LOG_INFO("OpticsSystem: created Vision runtime ({})",
                     describe_vision_pipeline_key(key));
    } else if (!it->second->scene_resource) {
        it->second->scene_resource = get_or_create_vision_scene_resource(
            make_vision_scene_resource_key(key.scene_path, key.source),
            key.scene_path);
    }
    return *it->second;
}

OpticsSystem::VisionPipelineRuntime* OpticsSystem::ensure_external_vision_runtime(
    const VisionPipelineKey& key,
    bool force_reload_scene_resource) {
    if (key.source == VisionPipelineSource::EngineBuilt || key.scene_path.empty()) {
        return &get_or_create_runtime(key);
    }

    try {
        const auto scene_resource_key =
            make_vision_scene_resource_key(key.scene_path, key.source);
        if (force_reload_scene_resource) {
            for (auto it = vision_runtimes_.begin(); it != vision_runtimes_.end();) {
                if (!(make_vision_scene_resource_key(it->first.scene_path, it->first.source) ==
                      scene_resource_key)) {
                    ++it;
                    continue;
                }
                if (it->second) {
                    CFW_LOG_INFO(
                        "OpticsSystem: releasing Vision runtime before shared scene reload ({})",
                        describe_vision_pipeline_key(it->first));
                    it->second->commit_and_clear_contexts();
                }
                it = vision_runtimes_.erase(it);
            }
        }

        auto& runtime = get_or_create_runtime(key);
        auto scene_resource =
            get_or_create_vision_scene_resource(scene_resource_key, key.scene_path);
        runtime.scene_resource = scene_resource;
        if (force_reload_scene_resource && scene_resource) {
            CFW_LOG_INFO("OpticsSystem: reloading shared Vision scene resource ({})",
                         describe_vision_scene_resource_key(scene_resource->key));
            scene_resource->reset_loaded_scene();
        }

        if (runtime.pipeline && !force_reload_scene_resource) {
            runtime.pipeline->set_output_denoise(
                Vision::vision_render_mode_uses_denoise(key.mode));
            return &runtime;
        }

        auto pipeline = import_vision_scene_from_file(
            std::filesystem::u8path(key.scene_path),
            key.mode,
            scene_resource,
            key.source);
        if (!pipeline) {
            CFW_LOG_ERROR("OpticsSystem: External Vision scene import failed: {}",
                          key.scene_path);
            release_unused_vision_scene_resources();
            return nullptr;
        }

        log_vision_pipeline_diagnostics(
            *pipeline,
            std::string("external import mode=") +
                std::string(Vision::vision_render_mode_name(key.mode)));
        runtime.reset_pipeline(std::move(pipeline), key.source, key.scene_path, key.mode);
        CFW_LOG_INFO("OpticsSystem: loaded Vision runtime ({})",
                     describe_vision_pipeline_key(key));
        return &runtime;
    } catch (const std::exception& e) {
        CFW_LOG_ERROR("OpticsSystem: External Vision scene import threw: {}", e.what());
        return nullptr;
    }
}

void OpticsSystem::evict_idle_vision_runtimes(uint64_t frame_index) {
    for (auto it = vision_runtimes_.begin(); it != vision_runtimes_.end();) {
        if (active_vision_runtime_key_ && *active_vision_runtime_key_ == it->first) {
            ++it;
            continue;
        }
        auto& runtime = it->second;
        if (!runtime || runtime->last_used_frame == 0 ||
            frame_index <= runtime->last_used_frame + kVisionRuntimeIdleEvictFrames) {
            ++it;
            continue;
        }
        CFW_LOG_INFO("OpticsSystem: evicting idle Vision runtime ({})",
                     describe_vision_pipeline_key(it->first));
        runtime->commit_and_clear_contexts();
        it = vision_runtimes_.erase(it);
    }
    release_unused_vision_scene_resources();
}

void OpticsSystem::activate_single_vision_runtime_key(const VisionPipelineKey& key) {
    if (active_vision_runtime_key_ && *active_vision_runtime_key_ == key) {
        (void)get_or_create_runtime(key);
        return;
    }

    for (auto it = vision_runtimes_.begin(); it != vision_runtimes_.end();) {
        if (it->first == key) {
            ++it;
            continue;
        }
        if (it->second) {
            CFW_LOG_INFO("OpticsSystem: releasing inactive Vision runtime ({})",
                         describe_vision_pipeline_key(it->first));
            it->second->commit_and_clear_contexts();
        }
        it = vision_runtimes_.erase(it);
    }
    release_unused_vision_scene_resources();
    active_vision_runtime_key_ = key;
    (void)get_or_create_runtime(key);
    CFW_LOG_INFO("OpticsSystem: active Vision runtime key ({})",
                 describe_vision_pipeline_key(key));
}

OpticsSystem::VisionPipelineRuntime& OpticsSystem::active_vision_runtime() {
    if (!active_vision_runtime_key_) {
        active_vision_runtime_key_ = make_vision_pipeline_key(
            "", current_vision_render_mode_, VisionPipelineSource::EngineBuilt);
        CFW_LOG_INFO("OpticsSystem: active Vision runtime key ({})",
                     describe_vision_pipeline_key(*active_vision_runtime_key_));
    }
    return get_or_create_runtime(*active_vision_runtime_key_);
}

void OpticsSystem::clear_vision_runtimes() {
    if (!vision_runtimes_.empty()) {
        CFW_LOG_INFO("OpticsSystem: clearing {} Vision runtime(s)",
                     vision_runtimes_.size());
    }
    for (auto& [key, runtime] : vision_runtimes_) {
        if (runtime) {
            CFW_LOG_INFO("OpticsSystem: releasing Vision runtime ({})",
                         describe_vision_pipeline_key(key));
            runtime->commit_and_clear_contexts();
        }
    }
    vision_runtimes_.clear();
    vision_scene_resources_.clear();
    active_vision_runtime_key_.reset();
    last_vision_runtime_group_signature_ = 0;
}
#endif

OpticsSystem::OpticsSystem() {
    set_target_fps(60);
}

OpticsSystem::~OpticsSystem() = default;

bool OpticsSystem::initialize_vision_backend_if_enabled() {
    // Vision backend is lazily initialized on first switch to Vision mode.
    return true;
}

bool OpticsSystem::initialize_hardware_resources() {
    try {
        hardware_ = std::make_unique<Hardware>();

        hardware_->gbufferSize.x = 1920;
        hardware_->gbufferSize.y = 1080;

        const auto w = hardware_->gbufferSize.x;
        const auto h = hardware_->gbufferSize.y;

        // --- Visibility Buffer (逐相机共享的中间产物) ---
        hardware_->visibilityImage =
            make_storage_image(w, h, Horizon::Format::RGBA32_UINT, "optics.visibility");
        hardware_->depthImage = make_depth_image(w, h, "optics.depth");
        hardware_->uiVisibilityImage =
            make_storage_image(w, h, Horizon::Format::RGBA32_UINT, "optics.ui_visibility");
        hardware_->uiDepthImage = make_depth_image(w, h, "optics.ui_depth");

        // --- Uniform buffers ---
        hardware_->uniformBuffer =
            make_storage_buffer<Hardware::UniformBufferObject>(1, "optics.uniform");
        hardware_->vpUniformBuffer =
            make_storage_buffer<Hardware::VPUniformBufferObject>(1, "optics.vp_uniform");
        hardware_->uiVpUniformBuffer =
            make_storage_buffer<Hardware::VPUniformBufferObject>(1, "optics.ui_vp_uniform");

        // --- Instance & Material table buffers (pre-allocate reasonable capacity) ---
        constexpr uint32_t kMaxInstances = 4096;
        constexpr uint32_t kMaxMaterials = 1024;
        hardware_->instanceInfoBuffer =
            make_storage_buffer<Hardware::InstanceInfo>(kMaxInstances, "optics.instances");
        hardware_->uiInstanceInfoBuffer =
            make_storage_buffer<Hardware::InstanceInfo>(kMaxInstances, "optics.ui_instances");
        hardware_->materialTableBuffer =
            make_storage_buffer<Hardware::MaterialInfo>(kMaxMaterials, "optics.materials");
        hardware_->uiMaterialTableBuffer =
            make_storage_buffer<Hardware::MaterialInfo>(kMaxMaterials, "optics.ui_materials");
        hardware_->actorPickBuffer =
            make_storage_buffer<std::uint32_t>(1, "optics.actor_pick");

        // Sky-driven ambient: 9 SH coefficients × vec3 = 27 floats. Written by
        // sky_sh_project.comp each environment change, read by lighting.comp.
        hardware_->skyIrradianceSHBuffer =
            make_storage_buffer<float>(27, "optics.sky_irradiance_sh");

        // finalOutputImage 不再在此创建：每个 surface 的最终输出由
        // acquire_surface_target() 按需创建（改造1: per-surface 输出）。
    } catch (const std::exception&) {
        CFW_LOG_CRITICAL("OpticsSystem: Failed to initialize hardware resources");
        return false;
    }

    return true;
}

bool OpticsSystem::initialize_render_pipelines() {
    try {
        hardware_->visibilityPipeline.emplace(make_visibility_pipeline_desc());
        hardware_->uiVisibilityPipeline.emplace(make_visibility_pipeline_desc());
        hardware_->lightingPipeline.emplace(lighting_comp_glsl, ktm::uvec3(8, 8, 1));
        hardware_->skyPipeline.emplace(sky_comp_glsl, ktm::uvec3(8, 8, 1));
        hardware_->skySHProjectPipeline.emplace(sky_sh_project_comp_glsl, ktm::uvec3(64, 1, 1));
        hardware_->tonemapPipeline.emplace(tonemap_comp_glsl, ktm::uvec3(8, 8, 1));
        hardware_->debugResolvePipeline.emplace(debug_resolve_comp_glsl, ktm::uvec3(8, 8, 1));
        hardware_->visibilityDebugResolvePipeline.emplace(visibility_debug_resolve_comp_glsl, ktm::uvec3(8, 8, 1));
        hardware_->actorPickPipeline.emplace(actor_pick_comp_glsl, ktm::uvec3(1, 1, 1));
        hardware_->opticsOverlayPipeline.emplace(optics_overlay_comp_glsl, ktm::uvec3(8, 8, 1));
        hardware_->opticsCursorPipeline.emplace(optics_cursor_comp_glsl, ktm::uvec3(8, 8, 1));
        hardware_->opticsUiWarpPipeline.emplace(optics_ui_warp_comp_glsl, ktm::uvec3(8, 8, 1));
        hardware_->opticsCompositePipeline.emplace(optics_composite_comp_glsl, ktm::uvec3(8, 8, 1));
#ifdef CORONA_ENABLE_VISION
        hardware_->visionResolvePipeline.emplace(vision_resolve_comp_glsl, ktm::uvec3(8, 8, 1));
#endif
        hardware_->shaderHasInit = true;
    } catch (const std::exception& e) {
        CFW_LOG_CRITICAL("OpticsSystem: Failed to initialize typed pipelines: {}", e.what());
        return false;
    }

    return true;
}

bool OpticsSystem::ensure_cursor_icon_texture() {
    if (!hardware_) {
        return false;
    }
    if (hardware_->cursorIconImage) {
        return true;
    }
    if (hardware_->cursorIconLoadAttempted) {
        return false;
    }
    hardware_->cursorIconLoadAttempted = true;

    auto pixels = load_mouse_icon_pixels();
    if (!pixels) {
        return false;
    }

    Horizon::HardwareImage icon(Horizon::HardwareImageDesc::texture_2d(
        static_cast<uint32_t>(pixels->width),
        static_cast<uint32_t>(pixels->height),
        Horizon::Format::SRGBA8_UNORM,
        Horizon::ImageUsageFlags::Sampled | Horizon::ImageUsageFlags::TransferDst,
        "optics.cursor_icon"));
    if (!icon) {
        CFW_LOG_WARNING("Optics cursor icon GPU image creation failed");
        return false;
    }

    Horizon::HardwareExecutor executor;
    const auto upload_pixels = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(pixels->rgba.data()),
        pixels->rgba.size());
    const Horizon::SubmitReceipt upload_receipt =
        executor.stream()
            << icon.upload(upload_pixels)
            << Horizon::commit();
    executor.wait_idle(upload_receipt);
    hardware_->cursorIconImage = std::move(icon);
    CFW_LOG_INFO("Optics cursor icon uploaded ({}x{})", pixels->width, pixels->height);
    return true;
}

void OpticsSystem::bind_native_view_resources(std::uintptr_t camera_handle,
                                              uint32_t width,
                                              uint32_t height,
                                              uint64_t frame_index) {
    width = std::max(width, 1u);
    height = std::max(height, 1u);

    auto& resources_ptr = native_view_resources_[camera_handle];
    if (!resources_ptr) {
        resources_ptr = std::make_unique<NativeViewResources>();
    }
    auto& resources = *resources_ptr;
    if (resources.width != width || resources.height != height ||
        !resources.visibility || !resources.depth || !resources.visibility_pipeline) {
        hardware_->executor.wait_idle(hardware_->executor.last_receipt());
        resources.visibility =
            make_storage_image(width, height, Horizon::Format::RGBA32_UINT, "optics.native_visibility");
        resources.depth = make_depth_image(width, height, "optics.native_depth");
        resources.visibility_pipeline.emplace(make_visibility_pipeline_desc());
        resources.visibility_pipeline->visibilityData = resources.visibility;
        resources.visibility_pipeline->bind_depth_target(resources.depth);
        resources.width = width;
        resources.height = height;
    }
    resources.last_used_frame = frame_index;

    hardware_->gbufferSize.x = width;
    hardware_->gbufferSize.y = height;
    hardware_->visibilityImage = resources.visibility;
    hardware_->depthImage = resources.depth;

    // UI visibility/depth 由共享 helper 统一分配并绑定（Native 与 Vision 同源）。
    ensure_ui_view_resources(camera_handle, width, height, frame_index);
}

void OpticsSystem::ensure_ui_view_resources(std::uintptr_t camera_handle,
                                            uint32_t width,
                                            uint32_t height,
                                            uint64_t frame_index) {
    width = std::max(width, 1u);
    height = std::max(height, 1u);

    auto& resources_ptr = ui_view_resources_[camera_handle];
    if (!resources_ptr) {
        resources_ptr = std::make_unique<UiViewResources>();
    }
    auto& resources = *resources_ptr;
    if (resources.width != width || resources.height != height ||
        !resources.ui_visibility || !resources.ui_depth) {
        hardware_->executor.wait_idle(hardware_->executor.last_receipt());
        resources.ui_visibility =
            make_storage_image(width, height, Horizon::Format::RGBA32_UINT, "optics.ui_visibility");
        resources.ui_depth = make_depth_image(width, height, "optics.ui_depth");
        resources.width = width;
        resources.height = height;
    }
    resources.last_used_frame = frame_index;

    // 不修改 gbufferSize（调用方负责）。仅绑定共享句柄到本相机的 UI 图。
    hardware_->uiVisibilityImage = resources.ui_visibility;
    hardware_->uiDepthImage = resources.ui_depth;
}

void OpticsSystem::evict_idle_native_view_resources(uint64_t frame_index) {
    for (auto it = native_view_resources_.begin(); it != native_view_resources_.end();) {
        const auto& resources = *it->second;
        const bool idle =
            frame_index > resources.last_used_frame &&
            (frame_index - resources.last_used_frame) > kNativeViewIdleEvictFrames;
        if (idle) {
            it = native_view_resources_.erase(it);
        } else {
            ++it;
        }
    }
}

void OpticsSystem::evict_idle_ui_view_resources(uint64_t frame_index) {
    for (auto it = ui_view_resources_.begin(); it != ui_view_resources_.end();) {
        const auto& resources = *it->second;
        const bool idle =
            frame_index > resources.last_used_frame &&
            (frame_index - resources.last_used_frame) > kUiViewIdleEvictFrames;
        if (idle) {
            it = ui_view_resources_.erase(it);
        } else {
            ++it;
        }
    }
}

OpticsSystem::SurfaceRenderTarget& OpticsSystem::acquire_surface_target(void* surface,
                                                                        uint32_t width,
                                                                        uint32_t height,
                                                                        uint64_t frame_index) {
    width = std::max(width, 1u);
    height = std::max(height, 1u);

    auto& target = surface_targets_[surface];

    // 首次出现该 surface：分配独立的 image_storage 句柄。
    if (target.image_handle == 0) {
        target.image_handle = SharedDataHub::instance().image_storage().allocate();
        // 触碰一次写句柄以保活存储项；逐帧的 image/executor 在渲染提交后更新。
        if (auto accessor =
                SharedDataHub::instance().image_storage().acquire_write(target.image_handle)) {
            // keep-alive only
        }
    }

    // 分辨率变化或首次：创建/重建该 surface 的 Optics 输出图。
    if (!target.final_output || !target.ui_overlay || !target.ui_warped_overlay ||
        !target.composite_output ||
        target.width != width || target.height != height) {
        if (target.image_handle != 0) {
            if (auto image_device =
                    SharedDataHub::instance().image_storage().acquire_write(target.image_handle)) {
                hardware_->executor.wait_idle(image_device->consumed_receipt);
            }
        }
        hardware_->executor.wait_idle(hardware_->executor.last_receipt());
        target.final_output =
            make_storage_image(width, height, Horizon::Format::RGBA16_FLOAT, "optics.surface_final");
        target.ui_overlay =
            make_storage_image(width, height, Horizon::Format::RGBA16_FLOAT, "optics.surface_overlay");
        target.ui_warped_overlay =
            make_storage_image(width, height, Horizon::Format::RGBA16_FLOAT, "optics.surface_warped_overlay");
        target.composite_output =
            make_storage_image(width, height, Horizon::Format::RGBA16_FLOAT, "optics.surface_composite");
        target.width = width;
        target.height = height;
    }

    target.last_used_frame = frame_index;
    return target;
}

OpticsSystem::SurfaceRenderTarget& OpticsSystem::acquire_offscreen_screenshot_target(
    std::uintptr_t camera_handle,
    uint32_t width,
    uint32_t height,
    uint64_t frame_index) {
    width = std::max(width, 1u);
    height = std::max(height, 1u);

    auto& target = offscreen_screenshot_targets_[camera_handle];
    if (!target.final_output || !target.ui_overlay || !target.composite_output ||
        target.width != width || target.height != height) {
        hardware_->executor.wait_idle(hardware_->executor.last_receipt());
        target.final_output =
            make_storage_image(width, height, Horizon::Format::RGBA16_FLOAT, "optics.offscreen_final");
        target.ui_overlay =
            make_storage_image(width, height, Horizon::Format::RGBA16_FLOAT, "optics.offscreen_overlay");
        target.composite_output =
            make_storage_image(width, height, Horizon::Format::RGBA16_FLOAT, "optics.offscreen_composite");
        target.width = width;
        target.height = height;
    }

    target.last_used_frame = frame_index;
    return target;
}

void OpticsSystem::evict_idle_surface_targets(uint64_t frame_index) {
    for (auto it = surface_targets_.begin(); it != surface_targets_.end();) {
        const auto& target = it->second;
        const bool idle =
            frame_index > target.last_used_frame &&
            (frame_index - target.last_used_frame) > kSurfaceTargetIdleEvictFrames;
        if (idle) {
            if (target.image_handle != 0) {
                SharedDataHub::instance().image_storage().deallocate(target.image_handle);
            }
            it = surface_targets_.erase(it);
        } else {
            ++it;
        }
    }
}

void OpticsSystem::evict_idle_offscreen_screenshot_targets(uint64_t frame_index) {
    for (auto it = offscreen_screenshot_targets_.begin();
         it != offscreen_screenshot_targets_.end();) {
        const auto& target = it->second;
        const bool idle =
            frame_index > target.last_used_frame &&
            (frame_index - target.last_used_frame) > kSurfaceTargetIdleEvictFrames;
        if (idle) {
            it = offscreen_screenshot_targets_.erase(it);
        } else {
            ++it;
        }
    }
}

bool OpticsSystem::initialize(Kernel::ISystemContext* ctx) {
    (void)ctx;

    if (!initialize_vision_backend_if_enabled()) {
        return false;
    }

    if (!initialize_hardware_resources()) {
        return false;
    }

    if (!initialize_render_pipelines()) {
        return false;
    }

    if (auto* event_bus = ctx->event_bus()) {
        screenshot_request_sub_id_ = event_bus->subscribe<Events::ScreenshotRequestEvent>(
            [this](const Events::ScreenshotRequestEvent& event) {
                if (event.camera_handle == 0 || event.file_path.empty() ||
                    !SharedDataHub::instance().camera_storage().try_acquire_read(event.camera_handle)) {
                    if (event.completion_promise) {
                        event.completion_promise->set_value(false);
                    }
                    return;
                }
                std::lock_guard<std::mutex> lock(screenshot_mutex_);
                pending_screenshots_.push_back({event.camera_handle, event.file_path, event.completion_promise});
            });

        backend_switch_sub_id_ = event_bus->subscribe<Events::RenderBackendSwitchEvent>(
            [this](const Events::RenderBackendSwitchEvent& event) {
                if (event.camera_handle == 0) {
                    return;
                }
                CameraStateUpdateCommand command{};
                command.camera_handle = event.camera_handle;
                command.fields = CameraStateUpdateField::RenderBackend;
#ifdef CORONA_ENABLE_VISION
                command.render_backend =
                    event.backend == static_cast<int>(RenderBackend::Vision)
                        ? CameraRenderBackend::Vision
                        : CameraRenderBackend::Native;
#else
                command.render_backend = CameraRenderBackend::Native;
#endif
                SharedDataHub::instance().enqueue_camera_state_update(command);
            });

#ifdef CORONA_ENABLE_VISION
        vision_scene_load_sub_id_ = event_bus->subscribe<Events::VisionSceneLoadEvent>(
            [this](const Events::VisionSceneLoadEvent& event) {
                // Only stash the path here (any thread). The actual import touches
                // the CUDA pipeline and MUST run on the render thread, so it is
                // deferred to apply_pending_vision_scene_load() in update().
                std::lock_guard<std::mutex> lock(vision_scene_load_mutex_);
                pending_vision_scene_load_ = event.scene_path;
            });
#endif
    }

    return true;
}

void OpticsSystem::update() {
    apply_pending_camera_moves();
    apply_pending_camera_viewport_updates();
    apply_pending_camera_state_updates();
    apply_pending_camera_releases();

    // 延迟获取 GeometrySystem 指针（不能在 initialize() 中获取，会死锁）
    // initialize_all() 持锁遍历系统，get_system() 也需同锁 → 非递归 mutex 重入崩溃
    if (!geometry_system_ && !geometry_system_queried_) {
        geometry_system_queried_ = true;
        auto& kernel = Kernel::KernelContext::instance();
        if (auto* sys_mgr = kernel.system_manager()) {
            auto sys = sys_mgr->get_system("Geometry");
            geometry_system_ = dynamic_cast<GeometrySystem*>(sys ? sys.get() : nullptr);
        }
    }

#ifdef CORONA_ENABLE_VISION
    std::vector<std::uintptr_t> requested_vision_cameras;
    bool camera_views_ready = true;
    for (const auto& scene : SharedDataHub::instance().scene_storage()) {
        if (!scene.enabled) {
            continue;
        }
        for (const auto camera_handle : scene.camera_handles) {
            if (auto camera =
                    SharedDataHub::instance().camera_storage().try_acquire_read(camera_handle);
                camera) {
                if (camera->view_open && camera->surface == nullptr) {
                    camera_views_ready = false;
                }
                if (camera->render_backend == CameraRenderBackend::Vision &&
                    camera->surface != nullptr) {
                    requested_vision_cameras.push_back(camera_handle);
                }
            }
        }
    }

    if (camera_views_ready && !requested_vision_cameras.empty() &&
        !vision_initialized_ && !init_vision_lazy()) {
        CFW_LOG_WARNING("OpticsSystem: Vision init failed, falling back affected cameras to Native");
        for (const auto camera_handle : requested_vision_cameras) {
            if (auto camera =
                    SharedDataHub::instance().camera_storage().acquire_write(camera_handle)) {
                camera->render_backend = CameraRenderBackend::Native;
            }
        }
    }
#endif

    if (!hardware_->shaderHasInit || !hardware_->lightingPipeline ||
        !hardware_->skyPipeline || !hardware_->tonemapPipeline ||
        !hardware_->skySHProjectPipeline ||
        !hardware_->debugResolvePipeline || !hardware_->opticsOverlayPipeline ||
        !hardware_->opticsCursorPipeline || !hardware_->opticsUiWarpPipeline ||
        !hardware_->opticsCompositePipeline) {
        return;
    }

    static float frame_count = 0.0f;
    static uint64_t frame_index = 0;

    float dt = delta_time();
    frame_count += dt;
    ++frame_index;

    optics_pipeline(frame_count, frame_index);
}

void OpticsSystem::optics_pipeline(float frame_count, uint64_t frame_index) {
    drain_viewport_ui_pointer_commands();

    auto& lighting = *hardware_->lightingPipeline;
    auto& sky = *hardware_->skyPipeline;
    auto& tonemap = *hardware_->tonemapPipeline;
    // UI overlay/warp/composite 管线现由 compose_surface_ui_overlay() 内部使用。

    fail_unrenderable_pending_screenshots();

    for (auto scene_it = SharedDataHub::instance().scene_storage().cbegin();
         scene_it != SharedDataHub::instance().scene_storage().cend(); ++scene_it) {
        const auto& scene = *scene_it;
        if (!scene.enabled)
            continue;

        for (auto cam_handle : scene.camera_handles) {
            if (auto camera = SharedDataHub::instance().camera_storage().try_acquire_read(cam_handle)) {
                if (camera->render_backend == CameraRenderBackend::Vision) {
                    if (camera->surface == nullptr && has_pending_screenshot(cam_handle)) {
                        fail_pending_screenshots(cam_handle);
                    }
                    continue;
                }
                void* surface = camera->surface;
                const bool offscreen_screenshot = surface == nullptr;
                if (offscreen_screenshot && !has_pending_screenshot(cam_handle)) {
                    continue;
                }

                SurfaceRenderTarget* target_ptr = nullptr;
                try {
                    target_ptr = offscreen_screenshot
                                     ? &acquire_offscreen_screenshot_target(cam_handle,
                                                                            camera->width,
                                                                            camera->height,
                                                                            frame_index)
                                     : &acquire_surface_target(surface, camera->width,
                                                               camera->height, frame_index);
                } catch (const std::exception& error) {
                    CFW_LOG_ERROR("OpticsSystem: failed to allocate render target for camera {}: {}",
                                  cam_handle, error.what());
                    fail_pending_screenshots(cam_handle);
                    continue;
                }
                auto& target = *target_ptr;

                if (!offscreen_screenshot) {
                    if (auto consumed_device =
                            SharedDataHub::instance().image_storage().acquire_write(target.image_handle)) {
                        hardware_->executor.wait(consumed_device->consumed_receipt);
                    }
                }
                bind_native_view_resources(cam_handle, camera->width, camera->height,
                                           frame_index);
                auto& visibility =
                    *native_view_resources_.at(cam_handle)->visibility_pipeline;

                // ================================================================
                // 1. Update camera uniform buffers
                // ================================================================
                hardware_->uniformBufferObjects.eyePosition = camera->position;
                hardware_->uniformBufferObjects.eyeDir = camera->forward;
                hardware_->uniformBufferObjects.eyeViewMatrix = camera->compute_view_matrix();
                hardware_->uniformBufferObjects.eyeProjMatrix = camera->compute_projection_matrix();
                hardware_->vpUniformBufferObjects.viewProjMatrix = camera->compute_view_proj_matrix();
                (void)write_object_bytes(hardware_->vpUniformBuffer,
                                         hardware_->vpUniformBufferObjects);

                // Configure visibility pipeline render targets
                visibility.visibilityData = hardware_->visibilityImage;
                visibility.bind_depth_target(hardware_->depthImage);

                auto& actor_storage = SharedDataHub::instance().actor_storage();
                auto& profile_storage = SharedDataHub::instance().profile_storage();
                auto& optics_storage = SharedDataHub::instance().optics_storage();
                auto& geom_storage = SharedDataHub::instance().geometry_storage();
                auto& transform_storage = SharedDataHub::instance().model_transform_storage();

                RenderInstanceBatch sceneBatch;

                auto collect_actor_instances_for_pass =
                    [&](auto& target_visibility,
                        const ktm::fmat4x4& view_proj_matrix,
                        bool follow_camera_pass,
                        const ktm::fmat4x4* camera_basis,
                        RenderInstanceBatch& batch) -> bool {
                    batch.clear();

                    bool has_instances = false;
                    uint32_t recorded_draws = 0;
                    uint32_t object_id = 1;
                    for (auto actor_handle : scene.actor_handles) {
                        auto actor = actor_storage.try_acquire_read(actor_handle);
                        if (!actor) {
                            ++object_id;
                            continue;
                        }

                        if (actor->follow_camera != follow_camera_pass) {
                            ++object_id;
                            continue;
                        }

                        for (auto profile_handle : actor->profile_handles) {
                            auto profile = profile_storage.try_acquire_read(profile_handle);
                            if (!profile || profile->optics_handle == 0) continue;

                            auto optics_acc = optics_storage.try_acquire_read(profile->optics_handle);
                            if (!optics_acc) continue;
                            const auto& optics = *optics_acc;

                            if (!optics.visible) {
                                ++object_id;
                                continue;
                            }
                            // 阻塞写锁：mesh 的 textureBuffer.storeDescriptor() 是非 const，
                            // 必须持写句柄。此处绝不能用 _nowait——拿不到锁就跳过会导致该物体
                            // 本帧不进 instance/material 表（模型没上 GPU），表现为闪烁。用阻塞
                            // 版 try_acquire_write 等锁（不漏帧），槽位失效时返回无效句柄而非抛异常。
                            if (auto geom = geom_storage.try_acquire_write(optics.geometry_handle)) {
                                ktm::fmat4x4 model_matrix{ktm::fmat4x4::from_eye()};
                                ktm::fvec3 world_center{0.0f, 0.0f, 0.0f};
                                if (auto transform = transform_storage.try_acquire_read(geom->transform_handle)) {
                                    model_matrix = transform->compute_matrix();
                                    // 必须在 camera_basis 变换前提取世界位置（否则 model_matrix[3] 不再是世界坐标）
                                    world_center = transform->position;
                                    model_matrix = apply_native_local_correction(model_matrix, *geom);
                                    if (camera_basis != nullptr) {
                                        model_matrix = multiply_ktm_mat4(*camera_basis, model_matrix);
                                    }
                                }

                                // ---- 计算物体在世界空间中的包围信息（用于LOD选择） ----
                                float bounding_radius = 1.0f;
                                bool use_lod = false;

                                if (geometry_system_) {
                                    auto& ms = SharedDataHub::instance().mechanics_storage();
                                    if (auto mech = ms.try_acquire_read(profile->mechanics_handle)) {
                                        float dx = mech->max_xyz.x - mech->min_xyz.x;
                                        float dy = mech->max_xyz.y - mech->min_xyz.y;
                                        float dz = mech->max_xyz.z - mech->min_xyz.z;
                                        bounding_radius = std::sqrt(dx*dx + dy*dy + dz*dz) * 0.5f;
                                        use_lod = true;
                                    }
                                }

                                for (uint32_t mesh_index = 0; mesh_index < static_cast<uint32_t>(geom->mesh_handles.size()); ++mesh_index) {
                                    auto& m = geom->mesh_handles[mesh_index];

                                    // ---- LOD 缓冲替换 ----
                                    Horizon::HardwareBuffer render_vb = m.vertexBuffer;
                                    Horizon::HardwareBuffer render_ib = m.indexBuffer;
                                    Horizon::HardwareBuffer render_vs = m.vertexStorageBuffer;
                                    Horizon::HardwareBuffer render_is = m.indexStorageBuffer;

                                    if (use_lod && geometry_system_) {
                                        float screen_ratio = GeometrySystem::compute_screen_ratio(
                                            camera->position, camera->fov,
                                            world_center, bounding_radius);

                                        if (auto* lod_buf = geometry_system_->resolve_lod_buffers(
                                                optics.geometry_handle, mesh_index,
                                                screen_ratio)) {
                                            if (lod_buf->vertex_buffer && lod_buf->index_buffer) {
                                                render_vb = lod_buf->vertex_buffer;
                                                render_ib = lod_buf->index_buffer;
                                                if (lod_buf->vertex_storage) render_vs = lod_buf->vertex_storage;
                                                if (lod_buf->index_storage)  render_is = lod_buf->index_storage;
                                            }
                                        }
                                    }
                                    // --- Collect material info ---
                                    auto materialID = static_cast<uint32_t>(batch.materials.size());
                                    {
                                        Hardware::MaterialInfo mat_info{};

                                        // 光照开关：bEnableLighting 为 true 时物体接收光照，false 时不接收光照
                                        float lighting_enabled = optics.bEnableLighting ? 1.0f : 0.0f;

                                        mat_info.textureDescriptor = m.textureBuffer
                                                                         ? m.textureBuffer.storeSampledDescriptor()
                                                                         : 0;

                                        // 当光照关闭时，将 BRDF 参数设为中性值，使物体不受方向光影响
                                        if (optics.bEnableLighting) {
                                            mat_info.metallic = optics.metallic;
                                            mat_info.roughness = optics.roughness;
                                            mat_info.subsurface = optics.subsurface;
                                            mat_info.specular = optics.specular;
                                            mat_info.specularTint = optics.specularTint;
                                            mat_info.anisotropic = optics.anisotropic;
                                            mat_info.sheen = optics.sheen;
                                            mat_info.sheenTint = optics.sheenTint;
                                            mat_info.clearcoat = optics.clearcoat;
                                            mat_info.clearcoatGloss = optics.clearcoatGloss;
                                        } else {
                                            // 关闭光照：使用中性BRDF参数（完全漫反射，无高光，无清漆等）
                                            mat_info.metallic = 0.0f;
                                            mat_info.roughness = 1.0f;
                                            mat_info.subsurface = 0.0f;
                                            mat_info.specular = 0.0f;
                                            mat_info.specularTint = 0.0f;
                                            mat_info.anisotropic = 0.0f;
                                            mat_info.sheen = 0.0f;
                                            mat_info.sheenTint = 0.0f;
                                            mat_info.clearcoat = 0.0f;
                                            mat_info.clearcoatGloss = 0.0f;
                                        }

                                        mat_info.lightingEnabled = lighting_enabled;
                                        mat_info.materialColor = ktm::fvec4{
                                            m.materialColor[0], m.materialColor[1],
                                            m.materialColor[2], m.materialColor[3]};
                                        batch.materials.push_back(mat_info);
                                    }

                                    // --- Collect instance info ---
                                    auto instanceID = static_cast<uint32_t>(batch.instances.size());
                                    {
                                        Hardware::InstanceInfo inst{};
                                        inst.modelMatrix = model_matrix;
                                        inst.vertexBufferIndex = render_vs
                                                                     ? render_vs.storeDescriptor()
                                                                     : 0;
                                        inst.indexBufferIndex = render_is
                                                                    ? render_is.storeDescriptor()
                                                                    : 0;
                                        inst.materialID = materialID;
                                        inst.objectID = object_id;
                                        batch.instances.push_back(inst);
                                        batch.actorHandles.push_back(actor_handle);
                                        has_instances = true;
                                    }

                                    // --- Record visibility draw call ---
                                    const ktm::fmat4x4 clip_matrix =
                                        multiply_ktm_mat4(view_proj_matrix, model_matrix);
                                    target_visibility[visibility_vert_glsl_t::pushConsts::modelMatrix] =
                                        upload_value(clip_matrix);
                                    target_visibility[visibility_vert_glsl_t::pushConsts::uniformBufferIndex] =
                                        0u;
                                    // VBuffer uses 1-based instanceID (0 = background sentinel after clear)
                                    target_visibility[visibility_vert_glsl_t::pushConsts::instanceID] =
                                        instanceID + 1;
                                    // Keep texture descriptor in the push constants for shader ABI compatibility.
                                    if (m.textureBuffer) {
                                        target_visibility[visibility_frag_glsl_t::pushConsts::textureIndex] =
                                            m.textureBuffer.storeSampledDescriptor();
                                    } else {
                                        target_visibility[visibility_frag_glsl_t::pushConsts::textureIndex] =
                                            static_cast<uint32_t>(0);
                                    }
                                    target_visibility.record(render_ib, render_vb);
                                    ++recorded_draws;
                                }
                            }
                            ++object_id;
                        }
                    }
                    return has_instances;
                };

                auto upload_instance_tables = [&](const RenderInstanceBatch& batch,
                                                  Horizon::HardwareBuffer& instance_buffer,
                                                  Horizon::HardwareBuffer& material_buffer) {
                    if (!batch.instances.empty()) {
                        (void)write_array_bytes(instance_buffer,
                                                batch.instances.data(),
                                                batch.instances.size());
                    }
                    if (!batch.materials.empty()) {
                        (void)write_array_bytes(material_buffer,
                                                batch.materials.data(),
                                                batch.materials.size());
                    }
                };

                const uint32_t sceneVpDescriptor = hardware_->vpUniformBuffer.storeDescriptor();
                visibility.clear_records();
                collect_actor_instances_for_pass(visibility,
                                                 hardware_->vpUniformBufferObjects.viewProjMatrix,
                                                 false,
                                                 nullptr,
                                                 sceneBatch);
                upload_instance_tables(sceneBatch,
                                       hardware_->instanceInfoBuffer,
                                       hardware_->materialTableBuffer);

                // ================================================================
                // 4. Environment parameters
                // ================================================================
                ktm::fvec3 sun_dir;
                sun_dir.x = 1.0f;
                sun_dir.y = 1.0f;
                sun_dir.z = 1.0f;
                std::uint32_t floor_grid_enabled = 1;
                ktm::fvec3 sun_color{1.0f, 0.949f, 0.853f};
                float sun_intensity = 10.0f;
                float sky_intensity = 20.0f;
                float exposure = 1.0f;
                if (scene.environment != 0) {
                    if (auto env = SharedDataHub::instance().environment_storage().try_acquire_read(
                            scene.environment)) {
                        sun_dir = env->sun_position;
                        floor_grid_enabled = env->floor_grid_enabled;
                        sun_color = env->sun_color;
                        sun_intensity = env->sun_intensity;
                        sky_intensity = env->sky_intensity;
                        exposure = env->exposure;
                    }
                }
                sun_dir = ktm::normalize(sun_dir);

                (void)write_object_bytes(hardware_->uniformBuffer,
                                         hardware_->uniformBufferObjects);
                const uint32_t uboDescriptor = hardware_->uniformBuffer.storeDescriptor();
                const uint32_t depthSampledDescriptor =
                    hardware_->depthImage.storeSampledDescriptor();

                // Offscreen cameras (no surface) render to a dedicated image so
                // they never collide with the display pipeline's per-surface output.
                // Display cameras render into their own surface target (改造1).
                Horizon::HardwareImage& render_target = target.final_output;
                Horizon::HardwareImage* presented_target = &render_target;
                const uint32_t finalOutputDescriptor = render_target.storeStorageDescriptor();

                // ================================================================
                // 5. Lighting pass: VBuffer decode + PBR direct illumination
                // ================================================================
                lighting.pushConsts.gbufferSize = upload_value(hardware_->gbufferSize);
                lighting.pushConsts.visibilityImageIndex =
                    hardware_->visibilityImage.storeStorageDescriptor();
                lighting.pushConsts.depthImageIndex = depthSampledDescriptor;
                lighting.pushConsts.instanceInfoBufferIndex =
                    hardware_->instanceInfoBuffer.storeDescriptor();
                lighting.pushConsts.materialTableBufferIndex =
                    hardware_->materialTableBuffer.storeDescriptor();
                lighting.pushConsts.vpBufferIndex =
                    hardware_->vpUniformBuffer.storeDescriptor();
                lighting.pushConsts.finalOutputImage = finalOutputDescriptor;
                lighting.pushConsts.uniformBufferIndex = uboDescriptor;
                lighting.bind_storage_image(0, hardware_->visibilityImage);
                lighting.bind_storage_image(1, render_target);
                lighting.pushConsts.skyIrradianceSHBufferIndex =
                    hardware_->skyIrradianceSHBuffer.storeDescriptor();
                lighting.pushConsts.ambientIntensity = 0.0f;
                lighting.pushConsts.sun_dir = upload_value(sun_dir);
                {
                    ktm::fvec3 lightColor;
                    lightColor.x = sun_color.x * sun_intensity;
                    lightColor.y = sun_color.y * sun_intensity;
                    lightColor.z = sun_color.z * sun_intensity;
                    lighting.pushConsts.lightColor = upload_value(lightColor);
                }

                // ================================================================
                // 5b. Sky → SH9 projection (sky-driven ambient).
                // Recompute the 9 SH coefficients only when the environment
                // lighting signature (sun_dir + sky_intensity) changes; otherwise
                // reuse the persistent skyIrradianceSHBuffer. Static lighting
                // amortizes to ~zero. The dispatch (when needed) is recorded
                // before lighting so program order guarantees the SH write lands
                // before lighting reads it on the same executor.
                // ================================================================
                std::size_t sky_sig = 0;
                mix_hash_float(sky_sig, sun_dir.x);
                mix_hash_float(sky_sig, sun_dir.y);
                mix_hash_float(sky_sig, sun_dir.z);
                mix_hash_float(sky_sig, sky_intensity);
                const bool sky_sh_needs_update =
                    !sky_sh_initialized_ || sky_sig != sky_sh_signature_;
                if (sky_sh_needs_update) {
                    auto& skySH = *hardware_->skySHProjectPipeline;
                    skySH.pushConsts.outputBufferIndex =
                        hardware_->skyIrradianceSHBuffer.storeDescriptor();
                    skySH.pushConsts.sampleCount = 64u;
                    skySH.pushConsts.sun_dir = upload_value(sun_dir);
                    skySH.pushConsts.sky_intensity = sky_intensity;
                }

                // ================================================================
                // 6. Sky pass: atmospheric scattering + floor grid
                // ================================================================
                sky.pushConsts.gbufferSize = upload_value(hardware_->gbufferSize);
                sky.pushConsts.gbufferDepthImage = depthSampledDescriptor;
                sky.pushConsts.finalOutputImage = finalOutputDescriptor;
                sky.pushConsts.uniformBufferIndex = uboDescriptor;
                sky.pushConsts.sun_dir = upload_value(sun_dir);
                sky.pushConsts.floor_grid_enabled = floor_grid_enabled;
                sky.pushConsts.cameraFov = camera->fov;
                sky.pushConsts.sky_intensity = sky_intensity;
                sky.bind_storage_image(0, render_target);

                // ================================================================
                // 7. Tonemap pass: ACES filmic HDR → LDR
                // ================================================================
                tonemap.pushConsts.gbufferSize = upload_value(hardware_->gbufferSize);
                tonemap.pushConsts.inputImage = finalOutputDescriptor;
                tonemap.pushConsts.outputImage = finalOutputDescriptor;
                tonemap.pushConsts.exposure = exposure;
                tonemap.bind_storage_image(0, render_target);

                // ================================================================
                // 8. GPU sync & dispatch
                // ================================================================
                const bool is_debug_mode = camera->output_mode != CameraOutputMode::FinalColor;
                const uint32_t dispatchX = hardware_->gbufferSize.x;
                const uint32_t dispatchY = hardware_->gbufferSize.y;
                const auto actor_pick_request = take_pending_actor_pick(cam_handle);

                if (actor_pick_request) {
                    auto& actorPick = *hardware_->actorPickPipeline;
                    actorPick.pushConsts.pixel =
                        upload_value(ktm::uvec2{actor_pick_request->x, actor_pick_request->y});
                    actorPick.pushConsts.visibilityImageIndex =
                        hardware_->visibilityImage.storeStorageDescriptor();
                    actorPick.pushConsts.outputBufferIndex =
                        hardware_->actorPickBuffer.storeDescriptor();
                    actorPick.bind_storage_image(0, hardware_->visibilityImage);
                }

                Horizon::SubmitReceipt latest_submit_receipt;
                {
                    auto stream = hardware_->executor.stream();
                    if (is_debug_mode) {
                        // ============================================================
                        // Debug path: visibility + debug_resolve only (skip lighting/sky/tonemap)
                        // ============================================================
                        // Map CameraOutputMode to debugMode uint
                        uint32_t debugMode = 0;
                        switch (camera->output_mode) {
                            case CameraOutputMode::BaseColor:
                                debugMode = 0;
                                break;
                            case CameraOutputMode::Normal:
                                debugMode = 1;
                                break;
                            case CameraOutputMode::WorldPosition:
                                debugMode = 2;
                                break;
                            case CameraOutputMode::ObjectID:
                                debugMode = 3;
                                break;
                            case CameraOutputMode::VisibilityBuffer:
                                debugMode = 4;
                                break;
                            default:
                                debugMode = 0;
                                break;
                        }

                        stream << visibility(hardware_->gbufferSize.x, hardware_->gbufferSize.y);
                        if (debugMode == 4u) {
                            auto& visibilityDebugResolve = *hardware_->visibilityDebugResolvePipeline;
                            visibilityDebugResolve.pushConsts.gbufferSize =
                                upload_value(hardware_->gbufferSize);
                            visibilityDebugResolve.pushConsts.visibilityImageIndex =
                                hardware_->visibilityImage.storeSampledDescriptor();
                            visibilityDebugResolve.pushConsts.outputImageIndex =
                                render_target.storeStorageDescriptor();
                            visibilityDebugResolve.bind_storage_image(0, render_target);
                            stream << visibilityDebugResolve(dispatchX, dispatchY, 1);
                        } else {
                            auto& debugResolve = *hardware_->debugResolvePipeline;

                            debugResolve.pushConsts.gbufferSize = upload_value(hardware_->gbufferSize);
                            debugResolve.pushConsts.visibilityImageIndex =
                                hardware_->visibilityImage.storeSampledDescriptor();
                            debugResolve.pushConsts.depthImageIndex = depthSampledDescriptor;
                            debugResolve.pushConsts.instanceInfoBufferIndex =
                                hardware_->instanceInfoBuffer.storeDescriptor();
                            debugResolve.pushConsts.materialTableBufferIndex =
                                hardware_->materialTableBuffer.storeDescriptor();
                            debugResolve.pushConsts.vpBufferIndex =
                                hardware_->vpUniformBuffer.storeDescriptor();
                            debugResolve.pushConsts.outputImageIndex = finalOutputDescriptor;
                            debugResolve.pushConsts.debugMode = debugMode;
                            debugResolve.bind_storage_image(0, render_target);

                            stream << debugResolve(dispatchX, dispatchY, 1);
                        }
                    } else {
                        // ============================================================
                        // Normal rendering path: full pipeline
                        // ============================================================
                        stream << visibility(hardware_->gbufferSize.x, hardware_->gbufferSize.y);
                        if (sky_sh_needs_update) {
                            stream << (*hardware_->skySHProjectPipeline)(1, 1, 1);
                        }
                        stream << lighting(dispatchX, dispatchY, 1)
                               << sky(dispatchX, dispatchY, 1)
                               << tonemap(dispatchX, dispatchY, 1);
                    }

                    if (actor_pick_request) {
                        stream << (*hardware_->actorPickPipeline)(1, 1, 1);
                    }

                    if (!is_debug_mode) {
                        const auto ui_state =
                            SharedDataHub::instance().viewport_ui_state(cam_handle);
                        presented_target = compose_surface_ui_overlay(
                            stream,
                            cam_handle, *camera, scene, target, render_target,
                            ui_state.mode, ui_state.calibration, frame_index);
                    }

                    latest_submit_receipt = stream << Horizon::commit();
                }

                if (!is_debug_mode && sky_sh_needs_update) {
                    sky_sh_signature_ = sky_sig;
                    sky_sh_initialized_ = true;
                }

                if (actor_pick_request) {
                    complete_actor_pick(*actor_pick_request, sceneBatch.actorHandles);
                }


                    // 仅在真正发生合成时 commit，保持无 follow-actor 时的原有行为
                    // （此时 render_target 已在上方 scene pass 提交）。



                if (offscreen_screenshot) {
                    process_pending_screenshots(cam_handle, *presented_target);
                    continue;
                }

                process_pending_screenshots(cam_handle, *presented_target);

                // 显示相机把自己 surface 的输出发布给 DisplaySystem（按 surface 区分）。
                if (auto image_device =
                        SharedDataHub::instance().image_storage().acquire_write(target.image_handle)) {
                    image_device->image = *presented_target;
                    image_device->submit_receipt = latest_submit_receipt;
                }

                if (auto* event_bus = context()->event_bus()) {
                    const auto presented_extent = hardware_image_extent(*presented_target);
                    const auto viewport =
                        optics_event_viewport(*camera, presented_extent);
                    event_bus->publish<Events::OpticsFrameReadyEvent>({surface,
                                                                       target.image_handle,
                                                                       frame_index,
                                                                       presented_extent.width,
                                                                       presented_extent.height,
                                                                       viewport.x,
                                                                       viewport.y,
                                                                       viewport.width,
                                                                       viewport.height});
                }

#ifdef CORONA_ENABLE_VISION
                // (Vision render path runs in run_vision_frame below)
#endif
            }
        }
    }

#ifdef CORONA_ENABLE_VISION
    if (vision_initialized_) {
        run_vision_frame(frame_count, frame_index);
    }
#endif

    // 回收长期空闲（相机解绑 / 视口关闭）的 surface 目标，约束动态开关下的显存占用。
    evict_idle_surface_targets(frame_index);
    evict_idle_offscreen_screenshot_targets(frame_index);
    evict_idle_native_view_resources(frame_index);
    evict_idle_ui_view_resources(frame_index);
}

void OpticsSystem::drain_viewport_ui_pointer_commands() {
    auto commands = SharedDataHub::instance().drain_viewport_ui_pointer_commands();
    for (const auto& command : commands) {
        if (command.camera_handle == 0) {
            continue;
        }

        auto& state = viewport_cursor_states_[command.camera_handle];
        if (command.sequence < state.sequence) {
            continue;
        }

        std::string event_type = command.event_type;
        std::transform(event_type.begin(), event_type.end(), event_type.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        const bool hide_event =
            event_type == "leave" || event_type == "mouseout" ||
            event_type == "pointerleave" || event_type == "cancel" ||
            event_type == "pointercancel" || event_type == "blur";

        state.x = command.x;
        state.y = command.y;
        state.buttons = command.buttons;
        state.modifiers = command.modifiers;
        state.cursor_shape = command.cursor_shape;
        state.sequence = command.sequence;
        state.visible = !hide_event && command.cursor_shape != ViewportUiCursorShape::Hidden;
    }
}

Horizon::HardwareImage* OpticsSystem::compose_surface_ui_overlay(
    Horizon::HardwareStream& stream,
    std::uintptr_t camera_handle,
    const CameraDevice& camera,
    const SceneDevice& scene,
    SurfaceRenderTarget& target,
    Horizon::HardwareImage& background,
    ViewportUiMode mode,
    const ViewportUiCalibration& calibration,
    uint64_t frame_index) {
    (void)frame_index;

    auto& uiVisibility = *hardware_->uiVisibilityPipeline;
    auto& opticsOverlay = *hardware_->opticsOverlayPipeline;
    auto& opticsCursor = *hardware_->opticsCursorPipeline;
    auto& opticsUiWarp = *hardware_->opticsUiWarpPipeline;
    auto& opticsComposite = *hardware_->opticsCompositePipeline;

    const uint32_t dispatchX = (hardware_->gbufferSize.x + 7u) / 8u;
    const uint32_t dispatchY = (hardware_->gbufferSize.y + 7u) / 8u;
    uint32_t cursorDispatchX = dispatchX;
    uint32_t cursorDispatchY = dispatchY;

    // follow-camera UI 使用正交投影：把跟随相机的 actor 以屏幕贴合方式光栅化。
    const ktm::fmat4x4 camera_basis = make_camera_basis_matrix(camera);
    constexpr float kFollowCameraOrthoHeight = 2.0f;
    constexpr float kFollowCameraNear = -1000.0f;
    constexpr float kFollowCameraFar = 1000.0f;
    const float ortho_width = kFollowCameraOrthoHeight * camera.aspect;
    const ktm::fmat4x4 ortho_proj =
        make_orthographic_lh(ortho_width, kFollowCameraOrthoHeight,
                             kFollowCameraNear, kFollowCameraFar);

    hardware_->vpUniformBufferObjects.viewProjMatrix =
        multiply_ktm_mat4(ortho_proj, camera.compute_view_matrix());
    (void)write_object_bytes(hardware_->uiVpUniformBuffer,
                             hardware_->vpUniformBufferObjects);
    const uint32_t uiVpDescriptor = hardware_->uiVpUniformBuffer.storeDescriptor();

    uiVisibility.visibilityData = hardware_->uiVisibilityImage;
    uiVisibility.bind_depth_target(hardware_->uiDepthImage);

    RenderInstanceBatch uiBatch;
    const bool has_follow_camera_instances =
        collect_actor_instances_for_visibility(scene, uiVisibility, uiVpDescriptor,
                                               /*follow_camera_pass=*/true,
                                               &camera_basis, uiBatch);

    const bool stereo_ui = mode == ViewportUiMode::Stereo3D;
    const bool cursor_icon_ready = stereo_ui && ensure_cursor_icon_texture();
    const auto cursor_it = viewport_cursor_states_.find(camera_handle);
    const bool cursor_visible =
        stereo_ui && cursor_icon_ready && cursor_it != viewport_cursor_states_.end() && cursor_it->second.visible &&
        cursor_it->second.cursor_shape != ViewportUiCursorShape::Hidden &&
        std::isfinite(cursor_it->second.x) && std::isfinite(cursor_it->second.y);
    const ViewportCursorState* cursor_state =
        cursor_visible ? &cursor_it->second : nullptr;

    const auto ui_instance_count = static_cast<std::uint32_t>(uiBatch.instances.size());
    auto& ui_log_state = ui_pass_log_states_[camera_handle];
    if (!ui_log_state.has_state ||
        ui_log_state.has_follow_camera_instances != has_follow_camera_instances ||
        ui_log_state.stereo_ui != stereo_ui ||
        ui_log_state.cursor_visible != cursor_visible ||
        ui_log_state.instance_count != ui_instance_count ||
        ui_log_state.width != hardware_->gbufferSize.x ||
        ui_log_state.height != hardware_->gbufferSize.y) {
        ui_log_state = UiPassLogState{
            .has_state = true,
            .has_follow_camera_instances = has_follow_camera_instances,
            .stereo_ui = stereo_ui,
            .cursor_visible = cursor_visible,
            .instance_count = ui_instance_count,
            .width = hardware_->gbufferSize.x,
            .height = hardware_->gbufferSize.y,
        };
        CFW_LOG_INFO("Optics UI pass: camera={} mode={} follow_camera_instances={} cursor={} output={}x{} warp={}",
                     camera_handle,
                     stereo_ui ? "stereo3d" : "flat2d",
                     ui_instance_count,
                     cursor_visible ? "visible" : "hidden",
                     hardware_->gbufferSize.x,
                     hardware_->gbufferSize.y,
                     (stereo_ui && (has_follow_camera_instances || cursor_visible)) ? "submitted" : "skipped");
    }

    if (!has_follow_camera_instances && !cursor_visible) {
        return &background;
    }

    const uint32_t overlayDescriptor = target.ui_overlay.storeStorageDescriptor();
    if (has_follow_camera_instances) {
        upload_instance_tables(uiBatch,
                               hardware_->uiInstanceInfoBuffer,
                               hardware_->uiMaterialTableBuffer);

        opticsOverlay.pushConsts.gbufferSize = upload_value(hardware_->gbufferSize);
        opticsOverlay.pushConsts.visibilityImageIndex =
            hardware_->uiVisibilityImage.storeStorageDescriptor();
        opticsOverlay.pushConsts.instanceInfoBufferIndex =
            hardware_->uiInstanceInfoBuffer.storeDescriptor();
        opticsOverlay.pushConsts.materialTableBufferIndex =
            hardware_->uiMaterialTableBuffer.storeDescriptor();
        opticsOverlay.pushConsts.vpBufferIndex = uiVpDescriptor;
        opticsOverlay.pushConsts.outputImage = overlayDescriptor;
        opticsOverlay.bind_storage_image(0, hardware_->uiVisibilityImage);
        opticsOverlay.bind_storage_image(1, target.ui_overlay);
    }

    if (cursor_visible && cursor_state != nullptr) {
        const bool preserve_existing_overlay = has_follow_camera_instances;
        uint32_t cursor_origin_x = 0;
        uint32_t cursor_origin_y = 0;
        uint32_t cursor_width = hardware_->gbufferSize.x;
        uint32_t cursor_height = hardware_->gbufferSize.y;
        if (preserve_existing_overlay && hardware_->gbufferSize.x > 0u &&
            hardware_->gbufferSize.y > 0u) {
            constexpr int32_t kCursorPadding = 4;
            constexpr uint32_t kCursorExtent = 56;
            const auto cursor_x = static_cast<int32_t>(std::floor(cursor_state->x));
            const auto cursor_y = static_cast<int32_t>(std::floor(cursor_state->y));
            cursor_origin_x = static_cast<uint32_t>(std::max(cursor_x - kCursorPadding, 0));
            cursor_origin_y = static_cast<uint32_t>(std::max(cursor_y - kCursorPadding, 0));
            cursor_origin_x = std::min(cursor_origin_x, hardware_->gbufferSize.x - 1u);
            cursor_origin_y = std::min(cursor_origin_y, hardware_->gbufferSize.y - 1u);
            cursor_width = std::min(kCursorExtent, hardware_->gbufferSize.x - cursor_origin_x);
            cursor_height = std::min(kCursorExtent, hardware_->gbufferSize.y - cursor_origin_y);
        }

        opticsCursor.pushConsts.outputImage = overlayDescriptor;
        opticsCursor.pushConsts.outputWidth = hardware_->gbufferSize.x;
        opticsCursor.pushConsts.outputHeight = hardware_->gbufferSize.y;
        opticsCursor.pushConsts.originX = cursor_origin_x;
        opticsCursor.pushConsts.originY = cursor_origin_y;
        opticsCursor.pushConsts.cursorX = cursor_state->x;
        opticsCursor.pushConsts.cursorY = cursor_state->y;
        opticsCursor.pushConsts.cursorShape =
            static_cast<std::uint32_t>(cursor_state->cursor_shape);
        opticsCursor.pushConsts.cursorImage = hardware_->cursorIconImage.storeDescriptor();
        opticsCursor.pushConsts.cursorSize = 48.0f;
        opticsCursor.pushConsts.preserveExisting = preserve_existing_overlay ? 1u : 0u;
        opticsCursor.bind_storage_image(0, target.ui_overlay);
        cursorDispatchX = (cursor_width + 7u) / 8u;
        cursorDispatchY = (cursor_height + 7u) / 8u;
    }

    uint32_t compositeOverlayDescriptor = overlayDescriptor;
    if (stereo_ui) {
        opticsUiWarp.pushConsts.inputImage = overlayDescriptor;
        opticsUiWarp.pushConsts.outputImage =
            target.ui_warped_overlay.storeStorageDescriptor();
        opticsUiWarp.pushConsts.outputWidth = hardware_->gbufferSize.x;
        opticsUiWarp.pushConsts.outputHeight = hardware_->gbufferSize.y;
        opticsUiWarp.pushConsts.lenticularPitch = calibration.lenticular_pitch;
        opticsUiWarp.pushConsts.slant = std::tan(calibration.slant_angle_radians);
        opticsUiWarp.pushConsts.phaseOffset = calibration.phase_offset;
        opticsUiWarp.pushConsts.parallaxScale = calibration.parallax_scale;
        opticsUiWarp.pushConsts.rgbSubpixelOffsets = upload_value(ktm::fvec4(
            calibration.rgb_subpixel_offsets[0],
            calibration.rgb_subpixel_offsets[1],
            calibration.rgb_subpixel_offsets[2],
            0.0f));
        compositeOverlayDescriptor = target.ui_warped_overlay.storeStorageDescriptor();
        opticsUiWarp.bind_storage_image(0, target.ui_overlay);
        opticsUiWarp.bind_storage_image(1, target.ui_warped_overlay);
    }

    opticsComposite.pushConsts.bgImage = background.storeStorageDescriptor();
    opticsComposite.pushConsts.fgImage = compositeOverlayDescriptor;
    opticsComposite.pushConsts.outputImage = target.composite_output.storeStorageDescriptor();
    opticsComposite.pushConsts.outputWidth = hardware_->gbufferSize.x;
    opticsComposite.pushConsts.outputHeight = hardware_->gbufferSize.y;
    opticsComposite.bind_storage_image(0, background);
    opticsComposite.bind_storage_image(1, stereo_ui ? target.ui_warped_overlay : target.ui_overlay);
    opticsComposite.bind_storage_image(2, target.composite_output);

    if (has_follow_camera_instances) {
        stream << uiVisibility(hardware_->gbufferSize.x, hardware_->gbufferSize.y)
               << opticsOverlay(dispatchX, dispatchY, 1);
    }
    if (cursor_visible) {
        stream << opticsCursor(cursorDispatchX, cursorDispatchY, 1);
    }
    if (stereo_ui) {
        stream << opticsUiWarp(dispatchX, dispatchY, 1);
    }
    stream << opticsComposite(dispatchX, dispatchY, 1);
    // 注意：此处不 commit，由调用方在合适时机统一提交。

    return &target.composite_output;
}

namespace {

// Convert IEEE 754 half-precision float (16-bit) to single-precision float.
float half_to_float(uint16_t h) {
    const uint32_t sign = (h >> 15) & 0x1;
    const uint32_t exponent = (h >> 10) & 0x1F;
    const uint32_t mantissa = h & 0x3FF;

    float result;
    if (exponent == 0) {
        result = std::ldexp(static_cast<float>(mantissa), -24);  // denorm or zero
    } else if (exponent == 31) {
        result = (mantissa == 0) ? INFINITY : NAN;
    } else {
        result = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
    }
    return sign ? -result : result;
}

}  // namespace

std::optional<OpticsSystem::ActorPickRequest> OpticsSystem::take_pending_actor_pick(std::uintptr_t camera_handle) {
    std::uintptr_t pick_handle = 0;
    std::uint32_t camera_width = 0;
    std::uint32_t camera_height = 0;
    if (auto camera = SharedDataHub::instance().camera_storage().try_acquire_read(camera_handle)) {
        pick_handle = camera->actor_pick_handle;
        camera_width = camera->width;
        camera_height = camera->height;
    }
    if (pick_handle == 0 || camera_width == 0 || camera_height == 0) {
        return std::nullopt;
    }

    auto pick = SharedDataHub::instance().actor_pick_storage().try_acquire_write(pick_handle);
    if (!pick || !pick->pending) {
        return std::nullopt;
    }

    ActorPickRequest request;
    request.pick_handle = pick_handle;
    request.request_id = pick->request_id;
    request.x = pick->x;
    request.y = pick->y;
    pick->pending = false;

    if (request.x >= camera_width || request.y >= camera_height) {
        pick->actor_handle = 0;
        pick->result_x = request.x;
        pick->result_y = request.y;
        pick->result_request_id = request.request_id;
        pick->result_ready = true;
        return std::nullopt;
    }

    pick->result_ready = false;
    return request;
}

void OpticsSystem::complete_actor_pick(const ActorPickRequest& request,
                                       const std::vector<std::uintptr_t>& scene_actor_handles) {
    std::uint32_t instance_id = 0;
    if (!hardware_->actorPickBuffer.read(std::span<std::uint32_t>(&instance_id, 1))) {
        CFW_LOG_ERROR("OpticsSystem: Failed to read actor pick result from GPU");
    }

    std::uintptr_t actor_handle = 0;
    if (instance_id > 0) {
        const auto instance_index = static_cast<std::size_t>(instance_id - 1);
        if (instance_index < scene_actor_handles.size()) {
            actor_handle = scene_actor_handles[instance_index];
        }
    }

    if (auto pick = SharedDataHub::instance().actor_pick_storage().try_acquire_write(request.pick_handle)) {
        if (pick->request_id != request.request_id) {
            return;
        }
        pick->actor_handle = actor_handle;
        pick->result_x = request.x;
        pick->result_y = request.y;
        pick->result_request_id = request.request_id;
        pick->result_ready = true;
    }
}

#ifdef CORONA_ENABLE_VISION
void OpticsSystem::process_vision_actor_pick(std::uintptr_t camera_handle,
                                             const CameraDevice& camera,
                                             const SceneDevice& scene,
                                             uint64_t frame_index) {
    const auto actor_pick_request = take_pending_actor_pick(camera_handle);
    if (!actor_pick_request) {
        return;
    }

    bind_native_view_resources(camera_handle, camera.width, camera.height, frame_index);
    auto& visibility = *native_view_resources_.at(camera_handle)->visibility_pipeline;

    hardware_->vpUniformBufferObjects.viewProjMatrix = camera.compute_view_proj_matrix();
    (void)write_object_bytes(hardware_->vpUniformBuffer,
                             hardware_->vpUniformBufferObjects);
    const uint32_t scene_vp_descriptor = hardware_->vpUniformBuffer.storeDescriptor();

    RenderInstanceBatch scene_batch;
    collect_actor_instances_for_visibility(scene,
                                           visibility,
                                           scene_vp_descriptor,
                                           false,
                                           nullptr,
                                           scene_batch);
    upload_instance_tables(scene_batch,
                           hardware_->instanceInfoBuffer,
                           hardware_->materialTableBuffer);

    auto& actor_pick = *hardware_->actorPickPipeline;
    actor_pick.pushConsts.pixel =
        upload_value(ktm::uvec2{actor_pick_request->x, actor_pick_request->y});
    actor_pick.pushConsts.visibilityImageIndex =
        hardware_->visibilityImage.storeStorageDescriptor();
    actor_pick.pushConsts.outputBufferIndex = hardware_->actorPickBuffer.storeDescriptor();
    actor_pick.bind_storage_image(0, hardware_->visibilityImage);

    const Horizon::SubmitReceipt actor_pick_receipt =
        hardware_->executor.stream()
            << visibility(hardware_->gbufferSize.x, hardware_->gbufferSize.y)
            << actor_pick(1, 1, 1)
            << Horizon::commit();
    hardware_->executor.wait_idle(actor_pick_receipt);

    complete_actor_pick(*actor_pick_request, scene_batch.actorHandles);
}
#endif

bool OpticsSystem::has_pending_screenshot(std::uintptr_t camera_handle) {
    std::lock_guard<std::mutex> lock(screenshot_mutex_);
    return std::any_of(pending_screenshots_.begin(), pending_screenshots_.end(),
                       [camera_handle](const PendingScreenshot& req) {
                           return req.camera_handle == camera_handle;
                       });
}

void OpticsSystem::fail_pending_screenshots(std::uintptr_t camera_handle) {
    std::vector<PendingScreenshot> matched;
    {
        std::lock_guard<std::mutex> lock(screenshot_mutex_);
        auto it = std::remove_if(pending_screenshots_.begin(), pending_screenshots_.end(),
                                 [camera_handle](const PendingScreenshot& req) {
                                     return req.camera_handle == camera_handle;
                                 });
        matched.assign(std::make_move_iterator(it), std::make_move_iterator(pending_screenshots_.end()));
        pending_screenshots_.erase(it, pending_screenshots_.end());
    }

    for (auto& req : matched) {
        if (req.completion_promise) {
            req.completion_promise->set_value(false);
        }
    }
}

void OpticsSystem::fail_unrenderable_pending_screenshots() {
    std::vector<std::uintptr_t> pending_handles;
    {
        std::lock_guard<std::mutex> lock(screenshot_mutex_);
        pending_handles.reserve(pending_screenshots_.size());
        for (const auto& req : pending_screenshots_) {
            if (std::find(pending_handles.begin(), pending_handles.end(), req.camera_handle) ==
                pending_handles.end()) {
                pending_handles.push_back(req.camera_handle);
            }
        }
    }

    for (const auto camera_handle : pending_handles) {
        if (!SharedDataHub::instance().camera_storage().try_acquire_read(camera_handle)) {
            fail_pending_screenshots(camera_handle);
            continue;
        }

        bool in_enabled_scene = false;
        bool in_any_scene = false;
        for (const auto& scene : SharedDataHub::instance().scene_storage()) {
            if (std::find(scene.camera_handles.begin(), scene.camera_handles.end(), camera_handle) ==
                scene.camera_handles.end()) {
                continue;
            }
            in_any_scene = true;
            if (scene.enabled) {
                in_enabled_scene = true;
                break;
            }
        }
        if (!in_any_scene || !in_enabled_scene) {
            fail_pending_screenshots(camera_handle);
        }
    }
}

void OpticsSystem::process_pending_screenshots(std::uintptr_t camera_handle,
                                               Horizon::HardwareImage& render_target) {
    std::vector<PendingScreenshot> matched;
    {
        std::lock_guard<std::mutex> lock(screenshot_mutex_);
        auto write_it = pending_screenshots_.begin();
        for (auto read_it = pending_screenshots_.begin(); read_it != pending_screenshots_.end(); ++read_it) {
            if (read_it->camera_handle == camera_handle) {
                matched.push_back(std::move(*read_it));
            } else {
                if (write_it != read_it) {
                    *write_it = std::move(*read_it);
                }
                ++write_it;
            }
        }
        pending_screenshots_.erase(write_it, pending_screenshots_.end());
    }

    if (matched.empty()) {
        return;
    }

    const uint32_t w = hardware_->gbufferSize.x;
    const uint32_t h = hardware_->gbufferSize.y;
    if (w == 0 || h == 0) {
        CFW_LOG_WARNING("OpticsSystem: Cannot take screenshot - zero render dimensions");
        for (auto& req : matched) {
            if (req.completion_promise) req.completion_promise->set_value(false);
        }
        return;
    }

    bool visibility_screenshot = false;
    if (auto camera = SharedDataHub::instance().camera_storage().try_acquire_read(camera_handle)) {
        visibility_screenshot = camera->output_mode == CameraOutputMode::VisibilityBuffer;
    }

    const uint64_t pixel_count = static_cast<uint64_t>(w) * h;
    const uint64_t buffer_size = pixel_count * 8;  // RGBA16F = 4 channels * 2 bytes
    Horizon::HardwareBuffer staging_buffer =
        make_storage_buffer<std::uint16_t>(pixel_count * 4, "optics.screenshot_staging");
    if (!staging_buffer) {
        CFW_LOG_ERROR("OpticsSystem: Failed to create staging buffer for screenshot");
        for (auto& req : matched) {
            if (req.completion_promise) req.completion_promise->set_value(false);
        }
        return;
    }

    const Horizon::SubmitReceipt screenshot_copy_receipt =
        hardware_->executor.stream()
            << render_target.copy_to(staging_buffer)
            << Horizon::commit();
    hardware_->executor.wait_idle(screenshot_copy_receipt);

    std::vector<uint16_t> half_data(pixel_count * 4);
    (void)buffer_size;
    if (!staging_buffer.read(std::span<std::uint16_t>(half_data.data(), half_data.size()))) {
        CFW_LOG_ERROR("OpticsSystem: Failed to read screenshot data from GPU");
        for (auto& req : matched) {
            if (req.completion_promise) req.completion_promise->set_value(false);
        }
        return;
    }

    std::vector<std::uint32_t> visibility_data(pixel_count * 4);
    std::size_t non_zero_visibility = 0;
    {
        Horizon::HardwareBuffer visibility_staging =
            make_storage_buffer<std::uint32_t>(pixel_count * 4, "optics.screenshot_visibility_staging");
        const Horizon::SubmitReceipt visibility_copy_receipt =
            hardware_->executor.stream()
                << hardware_->visibilityImage.copy_to(visibility_staging)
                << Horizon::commit();
        hardware_->executor.wait_idle(visibility_copy_receipt);
        if (visibility_staging.read(std::span<std::uint32_t>(visibility_data.data(), visibility_data.size()))) {
            for (uint64_t i = 0; i < pixel_count; ++i) {
                const std::uint32_t instance = visibility_data[i * 4];
                if (instance != 0) {
                    ++non_zero_visibility;
                }
            }
        }
    }

    // Convert RGBA16F to RGBA8
    std::vector<uint8_t> rgba8(pixel_count * 4);
    for (uint64_t i = 0; i < pixel_count * 4; ++i) {
        float v = half_to_float(half_data[i]);
        v = std::fmax(0.0f, std::fmin(1.0f, v));
        rgba8[i] = static_cast<uint8_t>(v * 255.0f + 0.5f);
    }
    if (visibility_screenshot && non_zero_visibility != 0) {
        for (uint64_t i = 0; i < pixel_count; ++i) {
            const std::uint32_t instance = visibility_data[i * 4 + 0];
            const std::uint32_t primitive = visibility_data[i * 4 + 1];
            uint8_t* pixel = rgba8.data() + i * 4;
            if (instance == 0) {
                pixel[0] = 0;
                pixel[1] = 0;
                pixel[2] = 0;
                pixel[3] = 255;
                continue;
            }

            const std::uint32_t hash = (instance * 2654435761u) ^ (primitive * 2246822519u);
            pixel[0] = static_cast<uint8_t>(64u + (hash & 0x7Fu));
            pixel[1] = static_cast<uint8_t>(64u + ((hash >> 8u) & 0x7Fu));
            pixel[2] = static_cast<uint8_t>(64u + ((hash >> 16u) & 0x7Fu));
            pixel[3] = 255;
        }
    }

    for (const auto& req : matched) {
        std::filesystem::path file_path(req.file_path);
        auto image = std::make_shared<Resource::Image>(file_path);
        image->set_data(rgba8.data(), static_cast<int>(w), static_cast<int>(h), 4);

        auto rid = Resource::IResource::generate_uid(file_path);
        auto& manager = Resource::ResourceManager::get_instance();
        manager.add_resource(rid, image);

        if (manager.export_sync(rid, file_path)) {
            if (req.completion_promise) {
                req.completion_promise->set_value(true);
            }
        } else {
            CFW_LOG_ERROR("OpticsSystem: Failed to save screenshot to {}", req.file_path);
            if (req.completion_promise) {
                req.completion_promise->set_value(false);
            }
        }
    }
}

void OpticsSystem::shutdown() {
    if (auto* event_bus = context()->event_bus()) {
        if (screenshot_request_sub_id_ != 0) {
            event_bus->unsubscribe(screenshot_request_sub_id_);
        }
        if (backend_switch_sub_id_ != 0) {
            event_bus->unsubscribe(backend_switch_sub_id_);
        }
#ifdef CORONA_ENABLE_VISION
        if (vision_scene_load_sub_id_ != 0) {
            event_bus->unsubscribe(vision_scene_load_sub_id_);
        }
#endif
    }

    // 释放所有 per-surface 渲染目标的存储句柄与 GPU 图（改造1）。
    for (auto& [surface, target] : surface_targets_) {
        if (target.image_handle != 0) {
            SharedDataHub::instance().image_storage().deallocate(target.image_handle);
        }
    }
    surface_targets_.clear();
    offscreen_screenshot_targets_.clear();
    native_view_resources_.clear();
    ui_view_resources_.clear();
#ifdef CORONA_ENABLE_VISION
    clear_vision_runtimes();
#endif
    hardware_.reset();
}
#ifdef CORONA_ENABLE_VISION
std::size_t OpticsSystem::compute_vision_scene_signature() const {
    // Lightweight per-frame change detector. Traverses the same hierarchy as
    // build_vision_geometry (enabled scene → actor → profile → optics → geometry)
    // and folds the topology/transform/material-relevant fields into one hash.
    // Any meaningful change to imported/removed geometry, transforms, material
    // params or per-mesh color flips this signature, triggering a rebuild.
    std::size_t sig = 0;
    auto mix = [&sig](std::size_t v) {
        // 64-bit hash_combine (boost-style golden ratio constant).
        sig ^= v + 0x9e3779b97f4a7c15ULL + (sig << 6) + (sig >> 2);
    };
    auto mix_float = [&mix](float f) {
        // Hash the raw bit pattern so small value changes are detected.
        std::uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(f), "float must be 32-bit");
        std::memcpy(&bits, &f, sizeof(bits));
        mix(static_cast<std::size_t>(bits));
    };

    auto& hub = SharedDataHub::instance();
    auto& actor_storage = hub.actor_storage();
    auto& profile_storage = hub.profile_storage();
    auto& optics_storage = hub.optics_storage();
    auto& geom_storage = hub.geometry_storage();
    auto& transform_storage = hub.model_transform_storage();

    for (auto scene_it = hub.scene_storage().cbegin(); scene_it != hub.scene_storage().cend(); ++scene_it) {
        const auto& scene_dev = *scene_it;
        if (!scene_dev.enabled) continue;
        for (auto actor_handle : scene_dev.actor_handles) {
            auto actor = actor_storage.try_acquire_read(actor_handle);
            if (!actor) continue;
            mix(static_cast<std::size_t>(actor_handle));
            for (auto profile_handle : actor->profile_handles) {
                auto profile = profile_storage.try_acquire_read(profile_handle);
                if (!profile || profile->optics_handle == 0 || profile->geometry_handle == 0) continue;

                auto optics = optics_storage.try_acquire_read(profile->optics_handle);
                if (!optics) continue;

                // visible toggle changes topology of the Vision scene.
                mix(optics->visible ? 0x1u : 0x2u);
                if (!optics->visible) continue;

                // Material parameters bridged into the Vision principled BSDF.
                mix_float(optics->metallic);
                mix_float(optics->roughness);
                mix_float(optics->subsurface);
                mix_float(optics->anisotropic);
                mix_float(optics->sheen);
                mix_float(optics->sheenTint);
                mix_float(optics->clearcoat);
                mix_float(optics->clearcoatGloss);

                auto geom = geom_storage.try_acquire_read(optics->geometry_handle);
                if (!geom) continue;
                mix(static_cast<std::size_t>(optics->geometry_handle));
                mix(static_cast<std::size_t>(geom->model_resource_handle));
                mix(geom->mesh_handles.size());

                // Per-mesh material color (texture-color replacement detection).
                for (const auto& mesh_dev : geom->mesh_handles) {
                    mix_float(mesh_dev.materialColor[0]);
                    mix_float(mesh_dev.materialColor[1]);
                    mix_float(mesh_dev.materialColor[2]);
                    mix_float(mesh_dev.materialColor[3]);

                    // Mesh data readiness: for procedurally-generated geometry the
                    // vertex/index buffers are uploaded asynchronously, so the
                    // element count flips 0 -> N once the GPU upload completes.
                    // Folding it into the signature makes that transition trigger
                    // one more rebuild even though no logical field changed.
                    const auto& vbuf = mesh_dev.vertexBuffer
                                           ? mesh_dev.vertexBuffer
                                           : mesh_dev.vertexStorageBuffer;
                    mix(static_cast<std::size_t>(vbuf.get_element_count()));
                }

                // Object-to-world transform (position / rotation / scale).
                if (auto transform = transform_storage.try_acquire_read(geom->transform_handle)) {
                    mix_float(transform->position.x);
                    mix_float(transform->position.y);
                    mix_float(transform->position.z);
                    mix_float(transform->euler_rotation.x);
                    mix_float(transform->euler_rotation.y);
                    mix_float(transform->euler_rotation.z);
                    mix_float(transform->scale.x);
                    mix_float(transform->scale.y);
                    mix_float(transform->scale.z);
                }
            }
        }
    }
    return sig;
}

Vision::VisionBuildResult OpticsSystem::rebuild_vision_scene(VisionPipelineRuntime& runtime) {
    Vision::VisionBuildResult result;
    auto& pipeline = runtime.pipeline;
    if (!pipeline) return result;
    try {
        pipeline->activate_view_context(0u);
        auto& scene = pipeline->scene();
        result = Vision::build_vision_geometry(scene);
        if (runtime.scene_resource) {
            sync_logical_instances_from_pipeline_scene(*runtime.scene_resource, scene);
        }

        // build_vision_geometry() clears and rebuilds the scene's meshes/shapes,
        // which also tears down the light manager state established during
        // init_vision_lazy(). If we don't re-register the lights here, the
        // following scene.prepare() reinitialises the light sampler with a missing
        // (or geometry-introduced area-light only) env_light_, corrupting the
        // light sampler's env index / PMF bookkeeping and crashing the CUDA device
        // (observed: process exit code -1 right after "Vision scene rebuilt").
        // Mirror the initialization path: always re-inject a single Infinite sky
        // light (+ optional point sun) from the current Corona environment so the
        // env_light_ assignment stays valid across rebuilds.
        Corona::EnvironmentDevice env{};
        for (auto sd_it = SharedDataHub::instance().scene_storage().cbegin();
             sd_it != SharedDataHub::instance().scene_storage().cend(); ++sd_it) {
            const auto& sd = *sd_it;
            if (!sd.enabled) continue;
            if (sd.environment != 0) {
                if (auto e = SharedDataHub::instance().environment_storage().try_acquire_read(sd.environment)) {
                    env = *e;
                    break;
                }
            }
        }
        Vision::setup_vision_lights(scene, env);

        // A scene rebuild changes topology: new meshes, materials (and therefore
        // new bindless texture handles) and a freshly rebuilt light manager.
        //
        // We must NOT call the full pipeline->prepare() here. That method is
        // a one-shot initialisation path (FixedRenderPipeline::prepare() runs
        // Pipeline::prepare() -> scene().prepare() -> renderer_.prepare(scene()) ->
        // image_pool().prepare(stream()) -> ...). Re-running it on an already-initialised
        // pipeline reallocates the framebuffer / sensor / image-pool device buffers
        // that the render loop is already holding references to, which crashes the
        // CUDA device (observed: crash on the following prepare_view_texture()).
        //
        // The correct runtime update is an INCREMENTAL sequence that only refreshes
        // the parts affected by the topology change, while leaving the framebuffer,
        // view texture and sensor (resolution unchanged) untouched:
        //   scene.prepare()         -> re-encode materials/sensor for the new scene
        //   prepare_geometry()      -> rebuild geometry device buffers + accel
        //   prepare_lights(scene)   -> rebuild the light sampler's device buffers
        //   upload_scene_bindless_array()
        //                           -> publish scene-owned mesh/light/material handles
        //   upload_bindless_array() -> publish the new material texture handles
        //   compile()               -> recompile the integrator for the new
        //                              light/material/instance counts
        //   invalidate()            -> reset accumulation
        //
        // prepare_lights() is CRITICAL: setup_vision_lights() above changed the light
        // set, but Scene::prepare() does NOT touch the light sampler. The official init
        // path runs renderer_.prepare(scene()) -> prepare_lights(scene) ->
        // light_sampler_->prepare(scene_bindless, scene_device), which rebuilds the on-device light count / PMF /
        // env-index buffers. Skipping it leaves the UniformLightSampler indexing a stale
        // light buffer with the new (different) light_num(), so the very first render()
        // after the rebuild performs an out-of-bounds GPU read and crashes the CUDA
        // device (observed: process exit -1 right after "Vision scene rebuilt", with no
        // "This scene contains N light types" log emitted during the rebuild).
        // It must run AFTER prepare_geometry() because area lights reference shapes.
        scene.prepare();
        if (runtime.scene_resource) {
            runtime.scene_resource->mark_transforms_changed();
        }
        pipeline->prepare_geometry();
        if (runtime.scene_resource) {
            runtime.scene_resource->mark_scene_gpu_transforms_uploaded();
            runtime.scene_gpu_transform_version =
                runtime.scene_resource->logical_transform_version;
        }
        pipeline->renderer().prepare_lights(scene);
        pipeline->upload_scene_bindless_array();
        pipeline->upload_bindless_array();
        pipeline->compile();
        pipeline->rebuild_view_context_renderers();
        pipeline->invalidate_all_view_contexts();
    } catch (const std::exception& e) {
        CFW_LOG_ERROR("OpticsSystem: Vision scene rebuild failed: {}", e.what());
    }
    return result;
}

void OpticsSystem::sync_vision_dynamic_scene(VisionPipelineRuntime& runtime) {
    if (!vision_initialized_) return;

    const std::size_t sig = compute_vision_scene_signature();

    // Debounce: only rebuild after the signature has stayed stable for a few frames,
    // batching bursts of edits (e.g. importing several objects) into one rebuild.
    if (sig != vision_pending_signature_) {
        vision_pending_signature_ = sig;
        vision_stable_frames_ = 0;
        vision_rebuild_retries_ = 0;  // 内容发生变化，清零重试计数
        return;
    }

    if (sig == vision_applied_signature_) {
        return;  // nothing changed since the last applied rebuild
    }

    if (++vision_stable_frames_ < kVisionRebuildDebounceFrames) {
        return;  // still settling
    }
    vision_stable_frames_ = 0;

    const Vision::VisionBuildResult result = rebuild_vision_scene(runtime);

    if (result.instance_count > 0 || result.candidate_count == 0) {
        // 重建成功，或场景本就为空（candidate_count==0 是合法的 0）：接受签名，
        // 停止重试，避免对空场景每帧空转重建。
        vision_applied_signature_ = sig;
        vision_rebuild_retries_ = 0;
    } else {
        // 有候选物体但 0 实例 → 网格数据尚未就绪：不锁定签名，下一帧继续重试。
        if (++vision_rebuild_retries_ >= kVisionRebuildMaxRetries) {
            CFW_LOG_ERROR(
                "OpticsSystem: Vision rebuild produced 0 instances from {} candidates after {} "
                "retries; accepting empty result to avoid busy-loop",
                result.candidate_count, vision_rebuild_retries_);
            vision_applied_signature_ = sig;  // 兜底：达到上限后接受，停止重试
            vision_rebuild_retries_ = 0;
        }
        // 否则保持 vision_applied_signature_ 不变；由于签名未变，下一帧去抖立即满足，
        // 会再次触发 rebuild，直到数据就绪或达到上限。
    }
}

void OpticsSystem::sync_external_live_vision_transforms(VisionPipelineRuntime& runtime) {
    auto& pipeline = runtime.pipeline;
    auto scene_resource = runtime.scene_resource;
    if (!vision_initialized_ || !pipeline || !scene_resource || runtime.scene_path.empty()) {
        return;
    }

    const auto current_scene_key = scene_resource->key.source_path_key.empty()
                                       ? normalize_scene_path_key(runtime.scene_path)
                                       : scene_resource->key.source_path_key;
    if (current_scene_key.empty()) {
        return;
    }

    auto& hub = SharedDataHub::instance();
    auto& vision_scene = pipeline->scene();
    auto& groups = vision_scene.groups();

    bool changed = false;
    std::size_t updated_actors = 0;
    std::unordered_set<std::uintptr_t> active_bound_actors;

    for (auto scene_it = hub.scene_storage().cbegin(); scene_it != hub.scene_storage().cend(); ++scene_it) {
        const auto& scene_dev = *scene_it;
        if (!scene_dev.enabled) {
            continue;
        }

        for (auto actor_handle : scene_dev.actor_handles) {
            const auto binding = hub.external_vision_binding(actor_handle);
            if (!binding) {
                continue;
            }
            if (normalize_scene_path_key(binding->source_path) != current_scene_key) {
                continue;
            }

            const auto resolved = resolve_external_live_transform(actor_handle, *binding);
            if (!resolved) {
                continue;
            }

            active_bound_actors.insert(actor_handle);
            const auto cached =
                scene_resource->external_live_transform_signatures.find(actor_handle);
            const bool actor_signature_changed =
                cached == scene_resource->external_live_transform_signatures.end() ||
                cached->second != resolved->signature;

            const auto group_index = static_cast<std::size_t>(resolved->shape_index);
            if (group_index >= groups.size() || !groups[group_index]) {
                continue;
            }

            auto& group = groups[group_index];
            group->aabb = ::vision::Box3f{};
            const auto object_to_world = flatten_vision_matrix(resolved->o2w);
            bool logical_instance_changed = false;
            group->for_each([&](::vision::SP<::vision::ShapeInstance> instance,
                                uint instance_index) {
                if (!instance) {
                    return;
                }
                logical_instance_changed |= scene_resource->upsert_logical_instance({
                    .key = {.shape_index = resolved->shape_index,
                            .instance_index = static_cast<int>(instance_index)},
                    .actor_handle = actor_handle,
                    .transform_signature = resolved->signature,
                    .object_to_world = object_to_world,
                });
                instance->set_o2w(resolved->o2w);
                instance->init_aabb();
                group->aabb.extend(instance->aabb);
            });

            scene_resource->external_live_transform_signatures[actor_handle] =
                resolved->signature;
            if (actor_signature_changed || logical_instance_changed) {
                changed = true;
                ++updated_actors;
            }
        }
    }

    for (auto it = scene_resource->external_live_transform_signatures.begin();
         it != scene_resource->external_live_transform_signatures.end();) {
        if (active_bound_actors.contains(it->first)) {
            ++it;
        } else {
            it = scene_resource->external_live_transform_signatures.erase(it);
        }
    }

    if (!changed) {
        return;
    }

    try {
        pipeline->activate_view_context(0u);
        scene_resource->mark_transforms_changed();
        pipeline->update_geometry();
        scene_resource->mark_scene_gpu_transforms_uploaded();
        runtime.scene_gpu_transform_version = scene_resource->logical_transform_version;
        pipeline->invalidate_all_view_contexts();
        CFW_LOG_DEBUG("OpticsSystem: external_live updated {} proxy actor transform(s)",
                      updated_actors);
    } catch (const std::exception& e) {
        CFW_LOG_ERROR("OpticsSystem: external_live transform sync failed: {}", e.what());
    }
}

bool OpticsSystem::init_vision_lazy() {
    if (vision_initialized_) return true;
    try {
        // ocarina::Device is non-default-constructible; use auto so the type is
        // deduced from create_device(). Function-local static ensures single init.
        static auto s_device = ocarina::RHIContext::instance().create_device("cuda");
        visionDevicePtr = &s_device;
        visionDevicePtr->init_rtx();
        vision::Global::instance().set_device(visionDevicePtr);
        vision::Global::instance().set_scene_path(std::filesystem::current_path());

#ifdef CORONA_VISION_IMPORT_DEMO
        // Verification demo: load a known-good scene straight from disk instead of
        // building the Vision scene from CoronaEngine data. This isolates the
        // Vision render path so we can confirm it produces a picture at all.
        const auto key = make_vision_pipeline_key(kVisionDemoScenePath,
                                                  current_vision_render_mode_,
                                                  VisionPipelineSource::ExternalFile);
        auto scene_resource = get_or_create_vision_scene_resource(
            make_vision_scene_resource_key(kVisionDemoScenePath,
                                           VisionPipelineSource::ExternalFile),
            kVisionDemoScenePath);
        auto pipeline = import_vision_scene_from_file(
            std::filesystem::path{kVisionDemoScenePath},
            current_vision_render_mode_,
            scene_resource,
            VisionPipelineSource::ExternalFile);
        if (!pipeline) {
            CFW_LOG_ERROR("OpticsSystem: Vision demo pipeline import failed");
            return false;
        }
        activate_single_vision_runtime_key(key);
        auto& runtime = active_vision_runtime();
        runtime.reset_pipeline(std::move(pipeline),
                               VisionPipelineSource::ExternalFile,
                               kVisionDemoScenePath,
                               current_vision_render_mode_);
        vision_initialized_ = true;
        return true;
#else
        std::optional<std::string> pending_external_scene;
        {
            std::lock_guard<std::mutex> lock(vision_scene_load_mutex_);
            if (pending_vision_scene_load_ && !pending_vision_scene_load_->empty()) {
                pending_external_scene.swap(pending_vision_scene_load_);
            }
        }
        if (pending_external_scene) {
            const auto mode_selection = select_visible_vision_render_mode();
            const auto requested_mode = mode_selection.has_visible_camera
                                            ? mode_selection.mode
                                            : current_vision_render_mode_;
            if (!load_external_vision_scene(*pending_external_scene,
                                            requested_mode,
                                            std::nullopt,
                                            true)) {
                CFW_LOG_ERROR("OpticsSystem: failed to initialize Vision from external scene: {}",
                              *pending_external_scene);
                return false;
            }
            vision_initialized_ = true;
            const bool external_live = has_external_live_bindings_for_scene(*pending_external_scene);
            vision_applied_signature_ = 0;
            vision_pending_signature_ = 0;
            vision_stable_frames_ = 0;
            vision_rebuild_retries_ = 0;
            CFW_LOG_INFO("OpticsSystem: initialized Vision from {} scene: {}",
                         external_live ? "external_live" : "external",
                         *pending_external_scene);
            return true;
        }

        const auto key = make_vision_pipeline_key(
            "", current_vision_render_mode_, VisionPipelineSource::EngineBuilt);
        auto scene_resource = get_or_create_vision_scene_resource(
            make_vision_scene_resource_key("", VisionPipelineSource::EngineBuilt),
            "");
        auto pipeline = create_vision_pipeline(current_vision_render_mode_, scene_resource);
        if (!pipeline) {
            CFW_LOG_ERROR("OpticsSystem: Failed to create Vision pipeline without external scene import");
            return false;
        }
        bind_pipeline_scene_gpu_resource(*pipeline,
                                         *scene_resource,
                                         VisionPipelineSource::EngineBuilt,
                                         current_vision_render_mode_,
                                         "");

        // Populate Vision scene directly from CoronaEngine scene data.
        auto& scene = pipeline->scene();
        Vision::build_vision_geometry(scene);
        sync_logical_instances_from_pipeline_scene(*scene_resource, scene);

        // Always inject lights. Vision's UniformLightSampler divides by light_num()
        // and indexes the light buffer; an empty light set (no environment in the
        // Corona scene) causes a 1/0 PMF and an out-of-bounds GPU read -> device crash.
        // When no environment is found we fall back to a default EnvironmentDevice so
        // the scene still receives a directional sun + sky light.
        Corona::EnvironmentDevice env{};
        for (auto sd_it = SharedDataHub::instance().scene_storage().cbegin();
             sd_it != SharedDataHub::instance().scene_storage().cend(); ++sd_it) {
            const auto& sd = *sd_it;
            if (!sd.enabled) continue;
            if (sd.environment != 0) {
                if (auto e = SharedDataHub::instance().environment_storage().try_acquire_read(sd.environment)) {
                    env = *e;
                    break;
                }
            }
        }
        Vision::setup_vision_lights(scene, env);

        pipeline->prepare();

        // Pipeline::prepare() allocates the internal per-pixel device buffers but
        // does NOT create FrameBuffer::view_texture_ (only prepare_view_texture()
        // does). The render path tone-maps into view_texture_ and we later read it
        // via fill_window_buffer(view_texture()). Without this the first frame uses
        // an uninitialized texture. The official vision-gui/vision-eval apps also
        // call prepare_view_texture() right after prepare().
        pipeline->frame_buffer()->prepare_view_texture();

        // sync_vision_camera uploads to GPU device buffers. Those device buffers are
        // only allocated during Pipeline::prepare() (Scene::prepare -> Sensor::prepare
        // -> EncodedObject::prepare_data -> reset_device_buffer). Running it
        // before prepare() uploads into unallocated device memory and crashes the
        // CUDA device deterministically.
        for (auto sd_it = SharedDataHub::instance().scene_storage().cbegin();
             sd_it != SharedDataHub::instance().scene_storage().cend(); ++sd_it) {
            const auto& sd = *sd_it;
            if (!sd.enabled) continue;
            const auto camera_handle = select_scene_camera_handle(sd);
            if (camera_handle == 0) continue;
            auto camera = SharedDataHub::instance().camera_storage().try_acquire_read(camera_handle);
            if (!camera) continue;
            Vision::sync_vision_camera(*pipeline, *camera);
            break;
        }

        activate_single_vision_runtime_key(key);
        auto& runtime = active_vision_runtime();
        runtime.reset_pipeline(std::move(pipeline),
                               VisionPipelineSource::EngineBuilt,
                               "",
                               current_vision_render_mode_);
        vision_initialized_ = true;

        // Establish the dynamic-scene signature baseline so subsequent edits are
        // detected as changes against the initially-built scene.
        vision_applied_signature_ = compute_vision_scene_signature();
        vision_pending_signature_ = vision_applied_signature_;
        vision_stable_frames_ = 0;

        return true;
#endif  // CORONA_VISION_IMPORT_DEMO
    } catch (const std::exception& e) {
        CFW_LOG_ERROR("OpticsSystem: Vision init failed: {}", e.what());
        return false;
    }
}

void OpticsSystem::run_vision_frame(float frame_count, uint64_t frame_index) {
    (void)frame_count;
    apply_pending_vision_scene_load();

    auto cleanup_runtime_contexts =
        [&](VisionPipelineRuntime& runtime,
            const std::unordered_set<std::uintptr_t>& active_contexts) {
            auto& pipeline = runtime.pipeline;
            if (!pipeline) {
                return;
            }

            for (auto it = runtime.bridges.begin(); it != runtime.bridges.end();) {
                if (active_contexts.contains(it->first)) {
                    ++it;
                    continue;
                }
                const auto camera_handle = it->first;
                bool camera_exists = false;
                bool retain_bridge = false;
                if (auto camera = SharedDataHub::instance().camera_storage().try_acquire_read(
                        camera_handle)) {
                    camera_exists = true;
                    retain_bridge = camera->surface != nullptr;
                }

                // 选定该相机的最终输出：显示相机用其 surface 专属目标，离屏用共享离屏图。
                // A visible camera may switch back to this runtime at any time. Keep
                // its imported bridge alive while the surface exists; closing or
                // suspending the view clears the surface and releases the bridge.
                if (retain_bridge) {
                    if (!runtime.retained_contexts.contains(camera_handle)) {
                        pipeline->commit_command();
                        pipeline->invalidate_view_context(camera_handle);
                        runtime.retained_contexts.insert(camera_handle);
                    }
                    ++it;
                    continue;
                }

                pipeline->commit_command();
                it = runtime.bridges.erase(it);
                if (camera_exists) {
                    pipeline->invalidate_view_context(camera_handle);
                    runtime.retained_contexts.insert(camera_handle);
                } else {
                    pipeline->remove_view_context(camera_handle);
                }
            }
            for (auto it = runtime.readback_buffers.begin();
                 it != runtime.readback_buffers.end();) {
                if (active_contexts.contains(it->first)) {
                    ++it;
                    continue;
                }
                const auto camera_handle = it->first;
                bool camera_exists = false;
                bool retain_context = false;
                if (auto camera = SharedDataHub::instance().camera_storage().try_acquire_read(
                        camera_handle)) {
                    camera_exists = true;
                    retain_context = camera->surface != nullptr;
                }

                if (retain_context) {
                    if (!runtime.retained_contexts.contains(camera_handle)) {
                        pipeline->commit_command();
                        pipeline->invalidate_view_context(camera_handle);
                        runtime.retained_contexts.insert(camera_handle);
                    }
                    ++it;
                    continue;
                }

                pipeline->commit_command();
                runtime.readback_pixels.erase(camera_handle);
                runtime.zero_copy_disabled.erase(camera_handle);
                it = runtime.readback_buffers.erase(it);
                if (camera_exists) {
                    pipeline->invalidate_view_context(camera_handle);
                    runtime.retained_contexts.insert(camera_handle);
                } else {
                    pipeline->remove_view_context(camera_handle);
                }
            }
            for (auto it = runtime.retained_contexts.begin();
                 it != runtime.retained_contexts.end();) {
                if (SharedDataHub::instance().camera_storage().try_acquire_read(*it)) {
                    ++it;
                    continue;
                }
                pipeline->remove_view_context(*it);
                it = runtime.retained_contexts.erase(it);
            }
            pipeline->activate_view_context(0u);
        };

    auto render_vision_camera =
        [&](VisionPipelineRuntime& runtime,
            std::uintptr_t cam_handle,
            const CameraDevice& camera,
            const SceneDevice& scene,
            std::unordered_set<std::uintptr_t>& active_contexts) {
            auto& pipeline = runtime.pipeline;
            if (!pipeline) {
                return;
            }

            active_contexts.insert(cam_handle);
            runtime.retained_contexts.erase(cam_handle);
            process_vision_actor_pick(cam_handle, camera, scene, frame_index);
            try {
                const auto resolution =
                    ocarina::make_uint2(std::max(camera.width, 1u),
                                       std::max(camera.height, 1u));
                if (pipeline->has_view_context(cam_handle)) {
                    if (!pipeline->activate_view_context(cam_handle)) {
                        return;
                    }
                    const auto* existing_fb = pipeline->frame_buffer();
                    bool recreate_context = existing_fb == nullptr;
                    if (existing_fb != nullptr) {
                        const auto existing_res = existing_fb->resolution();
                        recreate_context = existing_res.x != resolution.x ||
                                           existing_res.y != resolution.y;
                    }
                    if (recreate_context) {
                        pipeline->commit_command();
                        runtime.bridges.erase(cam_handle);
                        runtime.zero_copy_disabled.erase(cam_handle);
                        runtime.readback_buffers.erase(cam_handle);
                        runtime.readback_pixels.erase(cam_handle);
                        runtime.retained_contexts.erase(cam_handle);
                        pipeline->remove_view_context(cam_handle);
                    }
                }
                if (!pipeline->has_view_context(cam_handle) &&
                    !pipeline->create_view_context(cam_handle, resolution)) {
                    CFW_LOG_ERROR(
                        "OpticsSystem: unable to allocate Vision view context for camera {}",
                        cam_handle);
                    return;
                }
                if (!pipeline->activate_view_context(cam_handle)) {
                    return;
                }

                Vision::sync_vision_camera(*pipeline, camera);
                pipeline->upload_data();
                pipeline->display(1.0 / 60.0);

                auto* fb = pipeline->frame_buffer();
                const auto res = fb->resolution();
                const uint32_t w = res.x;
                const uint32_t h = res.y;

                const uint64_t pixel_count = static_cast<uint64_t>(w) * static_cast<uint64_t>(h);
                std::optional<uint32_t> vision_src_descriptor;
                if (!runtime.zero_copy_disabled.contains(cam_handle)) {
                    auto& bridge = runtime.bridges[cam_handle];
                    if (!bridge) {
                        bridge = std::make_unique<Vision::VisionZeroCopyBridge>();
                    }
                    if (bridge->ensure(*pipeline, w, h) &&
                        bridge->copy_from_framebuffer(*pipeline)) {
                        vision_src_descriptor = bridge->imported().storeDescriptor();
                    } else {
                        runtime.bridges.erase(cam_handle);
                        runtime.zero_copy_disabled.insert(cam_handle);
                    }
                }

                if (!vision_src_descriptor) {
                    auto& pixels = runtime.readback_pixels[cam_handle];
                    pixels.resize(pixel_count);

                    const auto src = fb->display_source_buffer();
                    pipeline->stream() << src.download(pixels.data())
                                       << ocarina::synchronize()
                                       << ocarina::commit();

                    auto& fallback_buffer = runtime.readback_buffers[cam_handle];
                    const uint64_t float_count = pixel_count * 4u;
                    if (!fallback_buffer ||
                        fallback_buffer.get_element_count() != float_count) {
                        fallback_buffer = make_storage_buffer<float>(
                            float_count, "optics.vision_readback");
                    }
                    if (!fallback_buffer.write_bytes(
                            std::as_bytes(std::span<const ocarina::float4>(
                                pixels.data(), pixels.size())))) {
                        CFW_LOG_ERROR(
                            "OpticsSystem: unable to upload Vision fallback buffer for camera {}",
                            cam_handle);
                        return;
                    }
                    vision_src_descriptor = fallback_buffer.storeDescriptor();
                }

                void* surface = camera.surface;
                hardware_->gbufferSize = ktm::uvec2{w, h};
                auto& target = acquire_surface_target(surface, w, h, frame_index);
                if (auto consumed_device =
                        SharedDataHub::instance().image_storage().acquire_write(
                            target.image_handle)) {
                    hardware_->executor.wait(consumed_device->consumed_receipt);
                }

                auto stream = hardware_->executor.stream();
                {
                    auto& visionResolve = *hardware_->visionResolvePipeline;
                    visionResolve.pushConsts.gbufferSize = upload_value(hardware_->gbufferSize);
                    visionResolve.pushConsts.srcBufferIndex = *vision_src_descriptor;
                    visionResolve.pushConsts.outputImage =
                        target.final_output.storeStorageDescriptor();
                    visionResolve.pushConsts.exposure = 1.0f;
                    visionResolve.bind_storage_image(0, target.final_output);

                    const uint32_t dispatchX = (w + 7u) / 8u;
                    const uint32_t dispatchY = (h + 7u) / 8u;
                    // 不在此 commit：UI overlay pass 紧随其后读 final_output 作为背景，
                    // 整帧在同一 executor 上按程序序记录、末尾统一提交一次。
                    stream << visionResolve(dispatchX, dispatchY, 1);
                }

                // 与 Native 共用的 UI overlay 层：gbufferSize 已在上方设为 {w,h}。
                ensure_ui_view_resources(cam_handle, w, h, frame_index);
                const auto ui_state =
                    SharedDataHub::instance().viewport_ui_state(cam_handle);
                Horizon::HardwareImage* presented = compose_surface_ui_overlay(
                    stream,
                    cam_handle, camera, scene, target, target.final_output,
                    ui_state.mode, ui_state.calibration, frame_index);

                const Horizon::SubmitReceipt vision_submit_receipt =
                    stream << Horizon::commit();

                process_pending_screenshots(cam_handle, *presented);

                if (auto image_device =
                        SharedDataHub::instance().image_storage().acquire_write(
                            target.image_handle)) {
                    image_device->image = *presented;
                    image_device->submit_receipt = vision_submit_receipt;
                }

                if (auto* event_bus = context()->event_bus()) {
                    const auto presented_extent = hardware_image_extent(*presented);
                    const auto viewport = optics_event_viewport(camera, presented_extent);
                    event_bus->publish<Events::OpticsFrameReadyEvent>(
                        {surface,
                         target.image_handle,
                         frame_index,
                         presented_extent.width,
                         presented_extent.height,
                         viewport.x,
                         viewport.y,
                         viewport.width,
                         viewport.height});
                }
            } catch (const std::exception& error) {
                CFW_LOG_ERROR("OpticsSystem: Vision camera {} failed: {}",
                              cam_handle, error.what());
            }
        };

#ifndef CORONA_VISION_IMPORT_DEMO
    auto& seed_runtime = active_vision_runtime();
    if (!seed_runtime.pipeline) return;

    if (seed_runtime.source != VisionPipelineSource::EngineBuilt &&
        !seed_runtime.scene_path.empty()) {
        std::unordered_map<VisionPipelineKey,
                           std::vector<VisibleVisionCamera>,
                           VisionPipelineKeyHash>
            camera_groups;

        for (auto scene_it = SharedDataHub::instance().scene_storage().cbegin();
             scene_it != SharedDataHub::instance().scene_storage().cend(); ++scene_it) {
            const auto& scene = *scene_it;
            if (!scene.enabled) continue;

            for (auto cam_handle : scene.camera_handles) {
                auto camera =
                    SharedDataHub::instance().camera_storage().try_acquire_read(cam_handle);
                if (!camera || camera->render_backend != CameraRenderBackend::Vision ||
                    camera->surface == nullptr) {
                    continue;
                }

                const auto key = make_vision_pipeline_key(seed_runtime.scene_path,
                                                          camera->vision_render_mode,
                                                          seed_runtime.source);
                camera_groups[key].push_back({cam_handle, *camera, scene});
            }
        }

        if (camera_groups.empty()) {
            last_vision_runtime_group_signature_ = 0;
            evict_idle_vision_runtimes(frame_index);
            return;
        }

        last_vision_mode_conflict_signature_ = 0;
        std::unordered_set<VisionPipelineRuntime*> active_runtimes;
        std::unordered_set<VisionSceneResource*> active_scene_resources;
        std::unordered_set<VisionSceneResource*> synced_external_live_resources;
        std::vector<std::string> runtime_group_diagnostics;
        std::size_t visible_camera_count = 0;
        for (auto& [key, cameras] : camera_groups) {
            auto* runtime = ensure_external_vision_runtime(key);
            if (runtime == nullptr || !runtime->pipeline) {
                continue;
            }

            runtime->last_used_frame = frame_index;
            active_runtimes.insert(runtime);
            visible_camera_count += cameras.size();
            if (runtime->scene_resource) {
                active_scene_resources.insert(runtime->scene_resource.get());
            }

            std::vector<std::uintptr_t> camera_handles;
            camera_handles.reserve(cameras.size());
            for (const auto& visible_camera : cameras) {
                camera_handles.push_back(visible_camera.camera_handle);
            }
            std::sort(camera_handles.begin(), camera_handles.end());
            std::string camera_list;
            for (const auto camera_handle : camera_handles) {
                if (!camera_list.empty()) {
                    camera_list.push_back(',');
                }
                camera_list.append(std::to_string(camera_handle));
            }
            std::string diagnostic = "runtime={";
            diagnostic.append(describe_vision_pipeline_key(key));
            diagnostic.append("}, shared_scene={");
            diagnostic.append(runtime->scene_resource
                                  ? describe_vision_scene_resource_key(
                                        runtime->scene_resource->key)
                                  : "<none>");
            diagnostic.append("}, camera_count=");
            diagnostic.append(std::to_string(cameras.size()));
            diagnostic.append(", cameras=[");
            diagnostic.append(camera_list);
            diagnostic.push_back(']');
            runtime_group_diagnostics.push_back(std::move(diagnostic));

            if (runtime->source == VisionPipelineSource::ExternalLive &&
                runtime->scene_resource &&
                synced_external_live_resources.insert(runtime->scene_resource.get()).second) {
                sync_external_live_vision_transforms(*runtime);
            }
            if (runtime->source == VisionPipelineSource::ExternalLive &&
                runtime->scene_resource) {
                runtime->upload_shared_scene_transforms_if_needed();
            }

            std::unordered_set<std::uintptr_t> active_contexts;
            for (const auto& visible_camera : cameras) {
                render_vision_camera(*runtime,
                                     visible_camera.camera_handle,
                                     visible_camera.camera,
                                     visible_camera.scene,
                                     active_contexts);
            }
            cleanup_runtime_contexts(*runtime, active_contexts);
        }

        std::sort(runtime_group_diagnostics.begin(), runtime_group_diagnostics.end());
        std::string group_signature_source;
        for (const auto& diagnostic : runtime_group_diagnostics) {
            if (!group_signature_source.empty()) {
                group_signature_source.push_back('|');
            }
            group_signature_source.append(diagnostic);
        }
        const auto group_signature = std::hash<std::string>{}(group_signature_source);
        if (!runtime_group_diagnostics.empty() &&
            group_signature != last_vision_runtime_group_signature_) {
            last_vision_runtime_group_signature_ = group_signature;
            CFW_LOG_INFO(
                "OpticsSystem: external Vision runtime groups active_runtimes={}, "
                "shared_scene_resources={}, visible_cameras={}",
                active_runtimes.size(),
                active_scene_resources.size(),
                visible_camera_count);
            for (const auto& diagnostic : runtime_group_diagnostics) {
                CFW_LOG_INFO("OpticsSystem: external Vision runtime group {}", diagnostic);
            }
        }

        const std::unordered_set<std::uintptr_t> no_active_contexts;
        for (auto& [key, runtime] : vision_runtimes_) {
            if (!runtime || active_runtimes.contains(runtime.get())) {
                continue;
            }
            cleanup_runtime_contexts(*runtime, no_active_contexts);
        }
        evict_idle_vision_runtimes(frame_index);
        return;
    }
#endif  // CORONA_VISION_IMPORT_DEMO

    last_vision_runtime_group_signature_ = 0;

#ifndef CORONA_VISION_IMPORT_DEMO
    const auto mode_selection = select_visible_vision_render_mode();
    if (mode_selection.has_visible_camera) {
        if (mode_selection.conflict &&
            mode_selection.conflict_signature != last_vision_mode_conflict_signature_) {
            last_vision_mode_conflict_signature_ = mode_selection.conflict_signature;
            CFW_LOG_WARNING(
                "OpticsSystem: multiple visible engine-built Vision cameras request different "
                "render modes ({}); using camera {} mode '{}' for this single runtime",
                mode_selection.conflict_summary,
                mode_selection.selected_camera,
                std::string(Vision::vision_render_mode_name(mode_selection.mode)));
        } else if (!mode_selection.conflict) {
            last_vision_mode_conflict_signature_ = 0;
        }
        apply_vision_render_mode(mode_selection.mode);
    }
#endif

    auto& runtime = active_vision_runtime();
    auto& pipeline = runtime.pipeline;
    if (!pipeline) return;

#ifndef CORONA_VISION_IMPORT_DEMO
    if (mode_selection.has_visible_camera &&
        runtime.source == VisionPipelineSource::EngineBuilt) {
        sync_vision_dynamic_scene(runtime);
    } else if (mode_selection.has_visible_camera &&
               runtime.source == VisionPipelineSource::ExternalLive) {
        sync_external_live_vision_transforms(runtime);
    }
#endif

    runtime.last_used_frame = frame_index;
    std::unordered_set<std::uintptr_t> active_contexts;
    for (auto scene_it = SharedDataHub::instance().scene_storage().cbegin();
         scene_it != SharedDataHub::instance().scene_storage().cend(); ++scene_it) {
        const auto& scene = *scene_it;
        if (!scene.enabled) continue;

        for (auto cam_handle : scene.camera_handles) {
            auto camera = SharedDataHub::instance().camera_storage().try_acquire_read(cam_handle);
            if (!camera || camera->render_backend != CameraRenderBackend::Vision) {
                continue;
            }
            if (camera->surface == nullptr) {
                if (has_pending_screenshot(cam_handle)) {
                    fail_pending_screenshots(cam_handle);
                }
                continue;
            }

            render_vision_camera(runtime, cam_handle, *camera, scene, active_contexts);
        }
    }

    cleanup_runtime_contexts(runtime, active_contexts);
    evict_idle_vision_runtimes(frame_index);
}

void OpticsSystem::apply_pending_vision_scene_load() {
    std::optional<std::string> request;
    {
        std::lock_guard<std::mutex> lock(vision_scene_load_mutex_);
        if (!pending_vision_scene_load_) return;
        request.swap(pending_vision_scene_load_);
    }

    const std::string& path = *request;
    const auto mode_selection = select_visible_vision_render_mode();
    const auto requested_mode = mode_selection.has_visible_camera
                                    ? mode_selection.mode
                                    : current_vision_render_mode_;
    if (!path.empty()) {
        if (load_external_vision_scene(path, requested_mode, std::nullopt, true)) {
            const auto& runtime = active_vision_runtime();
            vision_applied_signature_ = 0;
            vision_pending_signature_ = 0;
            vision_stable_frames_ = 0;
            vision_rebuild_retries_ = 0;
            CFW_LOG_INFO("OpticsSystem: {} Vision scene loaded: {}",
                         runtime.source == VisionPipelineSource::ExternalLive
                             ? "external_live"
                             : "external",
                         path);
        }
        return;
    }

    const auto& runtime = active_vision_runtime();
    if (runtime.pipeline && runtime.source == VisionPipelineSource::EngineBuilt) {
        // Empty path is a state request: leave ExternalFile mode and use the
        // engine-built scene. If that state is already active, the normal
        // dynamic signature sync below will rebuild only when scene data changed.
        return;
    }

    try {
        const auto key = make_vision_pipeline_key(
            "", requested_mode, VisionPipelineSource::EngineBuilt);
        auto scene_resource = get_or_create_vision_scene_resource(
            make_vision_scene_resource_key("", VisionPipelineSource::EngineBuilt),
            "");
        auto pipeline = create_vision_pipeline(requested_mode, scene_resource);
        if (!pipeline) {
            CFW_LOG_ERROR("OpticsSystem: failed to recreate engine-built Vision pipeline");
            return;
        }
        bind_pipeline_scene_gpu_resource(*pipeline,
                                         *scene_resource,
                                         VisionPipelineSource::EngineBuilt,
                                         requested_mode,
                                         "");
        auto& scene = pipeline->scene();
        Vision::build_vision_geometry(scene);
        sync_logical_instances_from_pipeline_scene(*scene_resource, scene);

        Corona::EnvironmentDevice env{};
        for (auto sd_it = SharedDataHub::instance().scene_storage().cbegin();
             sd_it != SharedDataHub::instance().scene_storage().cend(); ++sd_it) {
            const auto& sd = *sd_it;
            if (!sd.enabled) continue;
            if (sd.environment != 0) {
                if (auto e = SharedDataHub::instance().environment_storage().try_acquire_read(sd.environment)) {
                    env = *e;
                    break;
                }
            }
        }
        Vision::setup_vision_lights(scene, env);
        pipeline->prepare();
        pipeline->frame_buffer()->prepare_view_texture();
        pipeline->set_output_denoise(
            Vision::vision_render_mode_uses_denoise(requested_mode));

        for (auto sd_it = SharedDataHub::instance().scene_storage().cbegin();
             sd_it != SharedDataHub::instance().scene_storage().cend(); ++sd_it) {
            const auto& sd = *sd_it;
            if (!sd.enabled) continue;
            const auto camera_handle = select_scene_camera_handle(sd);
            if (camera_handle == 0) continue;
            auto camera = SharedDataHub::instance().camera_storage().try_acquire_read(camera_handle);
            if (!camera) continue;
            Vision::sync_vision_camera(*pipeline, *camera);
            break;
        }

        activate_single_vision_runtime_key(key);
        auto& runtime = active_vision_runtime();
        runtime.reset_pipeline(std::move(pipeline),
                               VisionPipelineSource::EngineBuilt,
                               "",
                               requested_mode);
        current_vision_render_mode_ = requested_mode;
        vision_applied_signature_ = compute_vision_scene_signature();
        vision_pending_signature_ = vision_applied_signature_;
        vision_stable_frames_ = 0;
        vision_rebuild_retries_ = 0;
        if (requested_mode != CameraVisionRenderMode::PathTracing) {
            CFW_LOG_WARNING(
                "OpticsSystem: engine-built Vision scene can only toggle denoise in "
                "Phase 2; requested mode '{}' does not change framebuffer or denoiser type",
                std::string(Vision::vision_render_mode_name(requested_mode)));
        }
        CFW_LOG_INFO("OpticsSystem: restored engine-built Vision scene");
    } catch (const std::exception& e) {
        CFW_LOG_ERROR("OpticsSystem: restoring engine-built Vision scene failed: {}", e.what());
    }
}

void OpticsSystem::apply_vision_render_mode(CameraVisionRenderMode mode) {
    auto& runtime = active_vision_runtime();
    auto& pipeline = runtime.pipeline;
    if (!pipeline) {
        return;
    }

    auto rekey_active_runtime = [&]() {
        const auto key = make_vision_pipeline_key(runtime.scene_path, runtime.mode, runtime.source);
        if (active_vision_runtime_key_ && *active_vision_runtime_key_ == key) {
            return;
        }
        if (!active_vision_runtime_key_) {
            active_vision_runtime_key_ = key;
            (void)get_or_create_runtime(key);
            CFW_LOG_INFO("OpticsSystem: active Vision runtime key ({})",
                         describe_vision_pipeline_key(key));
            return;
        }
        auto node = vision_runtimes_.extract(*active_vision_runtime_key_);
        if (!node.empty()) {
            node.key() = key;
            vision_runtimes_.insert(std::move(node));
        } else {
            (void)get_or_create_runtime(key);
        }
        active_vision_runtime_key_ = key;
        CFW_LOG_INFO("OpticsSystem: active Vision runtime key ({})",
                     describe_vision_pipeline_key(key));
    };

    if (mode == current_vision_render_mode_) {
        pipeline->set_output_denoise(Vision::vision_render_mode_uses_denoise(mode));
        return;
    }

    if (mode == CameraVisionRenderMode::PathTracing) {
        pipeline->set_output_denoise(false);
        runtime.mode = mode;
        current_vision_render_mode_ = mode;
        rekey_active_runtime();
        log_vision_pipeline_diagnostics(
            *pipeline,
            std::string("mode switch ") + std::string(Vision::vision_render_mode_name(mode)));
        return;
    }

    if (runtime.scene_path.empty()) {
        const bool was_denoise_enabled =
            Vision::vision_render_mode_uses_denoise(current_vision_render_mode_);
        pipeline->set_output_denoise(true);
        if (!was_denoise_enabled) {
            prepare_enabled_denoiser_for_runtime_switch(*pipeline);
            pipeline->clear_view_contexts();
        }
        runtime.mode = mode;
        current_vision_render_mode_ = mode;
        rekey_active_runtime();
        CFW_LOG_WARNING(
            "OpticsSystem: requested Vision mode '{}' on engine-built scene; "
            "Phase 2 only toggles denoise without changing framebuffer or denoiser type",
            std::string(Vision::vision_render_mode_name(mode)));
        log_vision_pipeline_diagnostics(
            *pipeline,
            std::string("mode switch ") + std::string(Vision::vision_render_mode_name(mode)));
        return;
    }

    const auto source_path = runtime.scene_path;
    const auto source_type = runtime.source;
    if (!load_external_vision_scene(source_path, mode, source_type)) {
        CFW_LOG_WARNING(
            "OpticsSystem: failed to switch external Vision scene '{}' to mode '{}'; "
            "continuing with previous pipeline mode '{}'",
            source_path,
            std::string(Vision::vision_render_mode_name(mode)),
            std::string(Vision::vision_render_mode_name(current_vision_render_mode_)));
        return;
    }
}

bool OpticsSystem::load_external_vision_scene(const std::string& scene_path,
                                              CameraVisionRenderMode mode,
                                              std::optional<VisionPipelineSource> source_override,
                                              bool force_reload_scene_resource) {
    const auto source = source_override.value_or(
        has_external_live_bindings_for_scene(scene_path)
            ? VisionPipelineSource::ExternalLive
            : VisionPipelineSource::ExternalFile);
    const auto key = make_vision_pipeline_key(scene_path, mode, source);
    auto* runtime = ensure_external_vision_runtime(key, force_reload_scene_resource);
    if (runtime == nullptr || !runtime->pipeline) {
        return false;
    }

    active_vision_runtime_key_ = key;
    current_vision_render_mode_ = mode;
    CFW_LOG_INFO("OpticsSystem: active Vision runtime key ({})",
                 describe_vision_pipeline_key(key));
    return true;
}
#endif  // CORONA_ENABLE_VISION

}  // namespace Corona::Systems
