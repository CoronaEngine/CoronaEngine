#include <corona/systems/display/display_frame_commands.h>
#include <corona/systems/acoustics/acoustics_commands.h>
#include <corona/systems/mechanics/mechanics_commands.h>
#include <corona/systems/optics/environment_commands.h>
#include <corona/systems/optics/optics_commands.h>
#include "camera_viewport_updates.h"
#include <corona/utils/runtime_paths.h>
#include <corona/utils/scene_path_key.h>
#include <corona/diagnostics/thread_phase.h>
#include <corona/events/display_system_events.h>
#include <corona/events/optics_system_events.h>
#include <horizon/core/logging.h>
#include <corona/kernel/core/kernel_context.h>
#include <corona/kernel/event/i_event_bus.h>
#include <corona/kernel/event/i_event_stream.h>
#include <corona/resource/resource_manager.h>
#include <corona/resource/types/image.h>
#include <corona/resource/types/scene.h>
#include <corona/shared_data_hub.h>
#include <corona/systems/geometry/geometry_system.h>
#include <corona/systems/optics/optics_system.h>


#include <array>
#include <atomic>
#include <cstdio>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <system_error>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "hardware.h"
#include "../common/render_resource_bindings.h"
#include "optics_debug_labels.h"
#include "../common/diagnostic_env.h"
#include "../common/vision_restir_settings.h"


#ifdef _WIN32
#include <Windows.h>
#endif
#include "base/import/parameter_set.h"
#include "base/import/project_desc.h"
#include "base/mgr/global.h"
#include "hotfix_runtime/engine_hotfix.h"
#include "base/mgr/pipeline.h"
#include "core/util/switch_profile.h"
#include "base/mgr/scene.h"
#include "base/scattering/material.h"
#include "base/sensor/frame_buffer.h"
#include "base/sensor/light_field_types.h"
#include "base/sensor/sensor.h"
#include "rhi/context.h"
#include "vision/vision_geometry_adapter.h"
#include "vision/vision_geometry_sync_policy.h"
#include "vision/vision_skinning_bridge.h"
#include "vision/vision_camera_adapter.h"
#include "vision/vision_light_adapter.h"
#include "vision/vision_medium_adapter.h"
#include "vision/vision_render_mode_config.h"
#include "vision/vision_interop_lifetime.h"
#include "vision/vision_zero_copy_bridge.h"

namespace {

constexpr std::uint64_t kInitialInstanceTableCapacity = 4096;
constexpr std::uint64_t kInitialMaterialTableCapacity = 1024;

std::mutex g_invalid_optics_mesh_log_mutex;
std::unordered_set<std::string> g_invalid_optics_mesh_logs;
std::mutex g_optics_material_log_mutex;
std::unordered_set<std::string> g_optics_material_logs;

[[nodiscard]] bool env_flag_enabled(const char* name) {
    return Corona::Systems::Diagnostics::parse_env_flag(std::getenv(name));
}

[[nodiscard]] std::optional<std::uint64_t> env_u64(const char* name) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') return std::nullopt;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(raw, &end, 0);
    if (end == raw) return std::nullopt;
    return static_cast<std::uint64_t>(value);
}

struct OpticsDiagConfig {
    bool disable_textures = false;
    bool disable_albedo_sample = false;
    bool force_solid_material = false;
    bool profile = false;
    bool debug_labels = false;
    std::optional<std::uintptr_t> only_actor;
    std::optional<std::uintptr_t> only_geometry;
    std::optional<std::uint32_t> only_mesh;
    std::uint32_t draw_limit = std::numeric_limits<std::uint32_t>::max();
};

[[nodiscard]] const OpticsDiagConfig& optics_diag_config() {
    static const OpticsDiagConfig cfg = [] {
        OpticsDiagConfig c;
        c.profile = env_flag_enabled("CORONA_OPTICS_DIAG_PROFILE");
        c.debug_labels = env_flag_enabled("CORONA_OPTICS_DIAG_DEBUG_LABELS");
#ifdef CORONA_NATIVE_DEVELOPER_DIAGNOSTICS
        c.disable_textures = env_flag_enabled("CORONA_OPTICS_DIAG_DISABLE_TEXTURES");
        c.disable_albedo_sample = env_flag_enabled("CORONA_OPTICS_DIAG_DISABLE_ALBEDO_SAMPLE");
        c.force_solid_material = env_flag_enabled("CORONA_OPTICS_DIAG_FORCE_SOLID_MATERIAL");
        if (auto v = env_u64("CORONA_OPTICS_DIAG_ONLY_ACTOR")) {
            c.only_actor = static_cast<std::uintptr_t>(*v);
        }
        if (auto v = env_u64("CORONA_OPTICS_DIAG_ONLY_GEOMETRY")) {
            c.only_geometry = static_cast<std::uintptr_t>(*v);
        }
        if (auto v = env_u64("CORONA_OPTICS_DIAG_ONLY_MESH")) {
            c.only_mesh = static_cast<std::uint32_t>(*v);
        }
        if (auto v = env_u64("CORONA_OPTICS_DIAG_DRAW_LIMIT")) {
            c.draw_limit = *v > std::numeric_limits<std::uint32_t>::max()
                ? std::numeric_limits<std::uint32_t>::max()
                : static_cast<std::uint32_t>(*v);
        }
#endif
        return c;
    }();
    return cfg;
}

[[nodiscard]] bool diag_actor_allowed(EntityId actor_handle) {
    const auto& diag = optics_diag_config();
    return !diag.only_actor || *diag.only_actor == actor_handle;
}

[[nodiscard]] bool diag_geometry_allowed(EntityId geometry_handle) {
    const auto& diag = optics_diag_config();
    return !diag.only_geometry || *diag.only_geometry == geometry_handle;
}

[[nodiscard]] bool diag_mesh_allowed(std::uint32_t mesh_index) {
    const auto& diag = optics_diag_config();
    return !diag.only_mesh || *diag.only_mesh == mesh_index;
}

[[nodiscard]] std::string make_optics_draw_label(std::string_view pass,
                                                 std::uintptr_t actor,
                                                 std::uintptr_t geometry,
                                                 std::uint32_t mesh,
                                                 std::uint32_t frame,
                                                 std::uint32_t instance_id,
                                                 std::uint32_t material_id,
                                                 std::uint32_t texture_descriptor,
                                                 std::uint32_t vertex_descriptor,
                                                 std::uint32_t index_descriptor,
                                                 std::uint32_t vertex_count,
                                                 std::uint32_t index_count,
                                                 std::uint32_t max_index) {
    if (!Corona::Systems::OpticsDetail::debug_labels_enabled(
            optics_diag_config().profile, optics_diag_config().debug_labels)) {
        return {};
    }
    std::ostringstream out;
    out << "Optics/" << pass
        << " frame=" << frame
        << " actor=" << actor
        << " geometry=" << geometry
        << " mesh=" << mesh
        << " instance=" << instance_id
        << " material=" << material_id
        << " tex_desc=" << texture_descriptor
        << " v_desc=" << vertex_descriptor
        << " i_desc=" << index_descriptor
        << " vertices=" << vertex_count
        << " indices=" << index_count
        << " max_index=" << max_index;
    return out.str();
}

void log_invalid_optics_mesh_once(EntityId actor_handle,
                                  EntityId geometry_handle,
                                  std::uint32_t mesh_index,
                                  bool slot_valid,
                                  bool vertex_valid,
                                  bool index_valid,
                                  bool vertex_storage_valid,
                                  bool index_storage_valid,
                                  std::uint32_t vertex_descriptor,
                                  std::uint32_t index_descriptor,
                                  std::uint32_t texture_descriptor) {
    const std::string key = std::to_string(actor_handle) + ":" +
                            std::to_string(geometry_handle) + ":" +
                            std::to_string(mesh_index);
    {
        std::lock_guard lock(g_invalid_optics_mesh_log_mutex);
        if (!g_invalid_optics_mesh_logs.insert(key).second) return;
    }

    CFW_LOG_WARNING("OpticsSystem: skipped invalid mesh draw "
                    "(actor={}, geometry={}, mesh={}, slot_valid={}, vertex={}, index={}, "
                    "vertex_storage={}, index_storage={}, vertex_desc={}, index_desc={}, texture_desc={})",
                    actor_handle, geometry_handle, mesh_index, slot_valid,
                    vertex_valid, index_valid, vertex_storage_valid, index_storage_valid,
                    vertex_descriptor, index_descriptor, texture_descriptor);
}

void log_optics_material_once(EntityId actor_handle,
                              EntityId geometry_handle,
                              std::uint32_t mesh_index,
                              std::uint32_t material_id,
                              std::uint32_t texture_descriptor,
                              const Corona::Horizon::HardwareImage& texture,
                              bool texture_ready,
                              bool disable_albedo_sample) {
    if (!optics_diag_config().profile) {
        return;
    }

    const bool texture_valid = static_cast<bool>(texture);
    if (texture_descriptor == 0u && !texture_valid && !disable_albedo_sample) {
        return;
    }

    const std::string key = std::to_string(actor_handle) + ":" +
                            std::to_string(geometry_handle) + ":" +
                            std::to_string(mesh_index) + ":" +
                            std::to_string(material_id) + ":" +
                            std::to_string(texture_descriptor);
    {
        std::lock_guard lock(g_optics_material_log_mutex);
        if (!g_optics_material_logs.insert(key).second) return;
    }

    // 新版 Horizon: texture.extent() 已移除，需要在创建时手动跟踪
    // 暂时省略 extent 信息
    CFW_LOG_INFO("OpticsSystem: material table entry "
                 "(actor={}, geometry={}, mesh={}, material={}, texture_desc={}, "
                 "texture_ready={}, texture_valid={}, albedo_sample_disabled={})",
                 actor_handle,
                 geometry_handle,
                 mesh_index,
                 material_id,
                 texture_descriptor,
                 texture_ready,
                 texture_valid,
                 disable_albedo_sample);
}

Hardware::MaterialInfo pack_ui_material(const Corona::Systems::GeometrySystem::MeshSlot& mesh,
                                        const Corona::OpticsDevice& optics,
                                        std::uint32_t texture_descriptor) {
    Hardware::MaterialInfo result{texture_descriptor, mesh.material_color};
    if (!mesh.material && !optics.principled) return result;

    Corona::Resource::MaterialParameters value = optics.principled
        ? optics.principled->surface : mesh.material->source;
    if (optics.principled) {
        result.textureDescriptor = 0;
    } else if (optics.material_overrides) {
        // Apply global overrides before material-specific overrides.
        for (bool specific : {false, true}) {
            for (const auto& row : *optics.material_overrides) {
                if ((row.index.has_value() || row.material != "*") == specific &&
                    row.matches(mesh.material_index, mesh.material->source.name)) {
                    row.apply(value);
                }
            }
        }
    }
    for (std::size_t i = 0; i < result.materialColor.size(); ++i) {
        const float channel = value.base_color[i];
        result.materialColor[i] = std::isfinite(channel) ? std::clamp(channel, 0.0f, 1.0f) : 1.0f;
    }
    return result;
}

struct RenderInstanceBatch {
    struct ResourceKeepAlive {
        std::vector<Corona::Horizon::HardwareBuffer> vertex_draw_buffers;
        std::vector<Corona::Horizon::HardwareBuffer> index_draw_buffers;
        std::vector<Corona::Horizon::HardwareBuffer> vertex_storage_buffers;
        std::vector<Corona::Horizon::HardwareBuffer> index_storage_buffers;
        std::vector<Corona::Horizon::HardwareImage> sampled_textures;
    };

    std::vector<Hardware::InstanceInfo> instances;
    std::vector<Hardware::MaterialInfo> materials;
    std::shared_ptr<ResourceKeepAlive> resource_keep_alive = std::make_shared<ResourceKeepAlive>();

    void clear() {
        instances.clear();
        materials.clear();
        resource_keep_alive = std::make_shared<ResourceKeepAlive>();
    }

    void keep_mesh_resources(const Corona::Systems::GeometrySystem::MeshSlot& slot,
                             std::uint32_t texture_descriptor) {
        if (!resource_keep_alive) {
            resource_keep_alive = std::make_shared<ResourceKeepAlive>();
        }
        if (slot.geo.vertex) {
            resource_keep_alive->vertex_draw_buffers.push_back(slot.geo.vertex);
        }
        if (slot.geo.index) {
            resource_keep_alive->index_draw_buffers.push_back(slot.geo.index);
        }
        if (slot.geo.vertex_storage) {
            resource_keep_alive->vertex_storage_buffers.push_back(slot.geo.vertex_storage);
        }
        if (slot.geo.index_storage) {
            resource_keep_alive->index_storage_buffers.push_back(slot.geo.index_storage);
        }
        if (texture_descriptor != 0u && slot.texture) {
            resource_keep_alive->sampled_textures.push_back(slot.texture);
        }
    }
};

struct UVec2Upload {
    uint32_t x;
    uint32_t y;
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

[[nodiscard]] FVec4Upload upload_value(const ktm::fvec4& value) {
    return {value.x, value.y, value.z, value.w};
}

[[nodiscard]] Mat4Upload upload_value(const ktm::fmat4x4& value) {
    Mat4Upload upload{};
    static_assert(sizeof(upload) == sizeof(value));
    std::memcpy(&upload, &value, sizeof(upload));
    return upload;
}


[[nodiscard]] Corona::Horizon::RasterizerPipelineDesc make_ui_visibility_pipeline_desc() {
    Corona::Horizon::RasterizerPipelineDesc desc;
    // 新版 Horizon: shader 在构造 RasterizerPipeline 时传入，不在 desc 中设置
    desc.depth_test_enabled = true;
    desc.depth_write_enabled = true;
    desc.depth_compare_op = Corona::Horizon::CompareOp::LessOrEqual;
    // 新版没有 cull_mode 字段，默认行为
    desc.blend_enabled = false;  // opaque
    desc.clear_depth_target = true;
    desc.debug_name = "corona.ui_visibility";
    return desc;
}

constexpr uint64_t kVisionRuntimeIdleEvictFrames = 240;

void apply_pending_camera_viewport_updates() {
    auto& hub = Corona::SharedDataHub::instance();
    auto updates = hub.drain_camera_viewport_updates();
    if (updates.empty()) {
        return;
    }

    // 逐帧都会收到视口更新：聚合行降到 DEBUG（历史上 70 秒刷过 899 行完全相同的 INFO）。
    CFW_LOG_DEBUG("Optics: applying {} camera viewport update(s)", updates.size());

    for (const auto& update : updates) {
        // 只在视口真的变化时打一条 INFO；重复的相同更新不再刷屏。
        bool viewport_changed = false;
        if (auto camera = Corona::find_camera(update.camera_handle)) {
            viewport_changed = camera->surface != update.surface ||
                               camera->view_open != update.view_open ||
                               camera->view_x != update.x || camera->view_y != update.y ||
                               camera->view_width != update.width ||
                               camera->view_height != update.height;
        }
        if (Corona::Systems::apply_camera_viewport_update(hub, update) && viewport_changed) {
            CFW_LOG_INFO("Optics: camera viewport handle={} surface={} open={} rect={}x{}+{},{} render={}x{}",
                         update.camera_handle, update.surface, update.view_open, update.width,
                         update.height, update.x, update.y, update.render_width,
                         update.render_height);
        }
    }
}

std::vector<Corona::CameraStateUpdateCommand> apply_pending_camera_state_updates() {
    auto& hub = Corona::SharedDataHub::instance();
    auto updates = hub.drain_camera_state_updates();
    if (updates.empty()) {
        return updates;
    }

    auto camera_storage = Corona::Ecs::all_components<Corona::CameraDevice>();
    for (const auto& update : updates) {
        if (auto camera = Corona::find_camera(update.camera_handle)) {
            if (Corona::has_camera_state_field(update.fields, Corona::CameraStateUpdateField::Projection)) {
                camera->projection_x_sign = update.projection_x_sign;
                camera->near_plane = update.near_plane;
                camera->far_plane = update.far_plane;
            }
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
                    update.fields, Corona::CameraStateUpdateField::VisionRenderMode)) {
                camera->vision_render_mode = update.vision_render_mode;
            }
            if (Corona::has_camera_state_field(
                    update.fields, Corona::CameraStateUpdateField::VisionDenoise)) {
                camera->vision_denoise = update.vision_denoise;
            }
            if (Corona::has_camera_state_field(update.fields, Corona::CameraStateUpdateField::VisionDenoiser)) {
                camera->vision_denoiser = update.vision_denoiser;
            }
            if (Corona::has_camera_state_field(update.fields, Corona::CameraStateUpdateField::VisionReblurSettings)) {
                camera->vision_reblur_settings = update.vision_reblur_settings;
            }
            if (Corona::has_camera_state_field(update.fields, Corona::CameraStateUpdateField::VisionPtSettings)) {
                camera->vision_pt_settings = update.vision_pt_settings;
            }
            if (Corona::has_camera_state_field(update.fields, Corona::CameraStateUpdateField::VisionRestirSettings)) {
                camera->vision_restir_settings = update.vision_restir_settings;
            }
            if (Corona::has_camera_state_field(update.fields, Corona::CameraStateUpdateField::VisionRelaxSettings)) {
                camera->vision_relax_settings = update.vision_relax_settings;
            }
            if (Corona::has_camera_state_field(
                    update.fields, Corona::CameraStateUpdateField::VisionAccumulation)) {
                camera->vision_accumulation = update.vision_accumulation;
            }
            if (Corona::has_camera_state_field(
                    update.fields, Corona::CameraStateUpdateField::VisionStablePlanes)) {
                camera->vision_stable_planes = update.vision_stable_planes;
            }
            if (Corona::has_camera_state_field(
                    update.fields, Corona::CameraStateUpdateField::ViewState)) {
                camera->view_open = update.view_open;
            }
            if (Corona::has_camera_state_field(update.fields, Corona::CameraStateUpdateField::ViewState) ||
                Corona::has_camera_state_field(update.fields, Corona::CameraStateUpdateField::ViewSettings)) {
                camera->view_x = update.view_x;
                camera->view_y = update.view_y;
                camera->view_width = update.view_width;
                camera->view_height = update.view_height;
                camera->move_speed = update.move_speed;
            }
        }
    }
    return updates;
}

void acknowledge_camera_state_updates(const std::vector<Corona::CameraStateUpdateCommand>& updates) {
    auto& hub = Corona::SharedDataHub::instance();
    for (const auto& update : updates) {
        // Retire requests only after the committed snapshot is published.
        // A newer request may arrive during this batch; its sequence must survive.
        if (Corona::has_camera_state_field(
                update.fields, Corona::CameraStateUpdateField::VisionDenoise)) {
            hub.acknowledge_camera_vision_denoise(update.camera_handle, update.sequence);
        }
        if (Corona::has_camera_state_field(update.fields, Corona::CameraStateUpdateField::VisionDenoiser)) {
            hub.acknowledge_camera_vision_denoiser(update.camera_handle, update.sequence);
        }
        if (Corona::has_camera_state_field(update.fields, Corona::CameraStateUpdateField::VisionReblurSettings)) {
            hub.acknowledge_camera_vision_reblur_settings(update.camera_handle, update.sequence);
        }
        if (Corona::has_camera_state_field(update.fields, Corona::CameraStateUpdateField::VisionPtSettings)) {
            hub.acknowledge_camera_vision_pt_settings(update.camera_handle, update.sequence);
        }
        if (Corona::has_camera_state_field(update.fields, Corona::CameraStateUpdateField::VisionRestirSettings)) {
            hub.acknowledge_camera_vision_restir_settings(update.camera_handle, update.sequence);
        }
        if (Corona::has_camera_state_field(update.fields, Corona::CameraStateUpdateField::VisionRelaxSettings)) {
            hub.acknowledge_camera_vision_relax_settings(update.camera_handle, update.sequence);
        }
        if (Corona::has_camera_state_field(
                update.fields, Corona::CameraStateUpdateField::VisionAccumulation)) {
            hub.acknowledge_camera_vision_accumulation(update.camera_handle, update.sequence);
        }
        if (Corona::has_camera_state_field(
                update.fields, Corona::CameraStateUpdateField::VisionStablePlanes)) {
            hub.acknowledge_camera_vision_stable_planes(update.camera_handle, update.sequence);
        }
    }
}

void apply_pending_camera_moves() {
    auto& hub = Corona::SharedDataHub::instance();
    auto moves = hub.drain_camera_moves();
    if (moves.empty()) {
        return;
    }

    auto camera_storage = Corona::Ecs::all_components<Corona::CameraDevice>();
    for (const auto& move : moves) {
        if (auto camera = Corona::find_camera(move.camera_handle)) {
            camera->position = move.position;
            camera->forward = move.forward;
            camera->world_up = move.world_up;
            camera->fov = move.fov;
        }
    }
}

void apply_pending_transform_updates() {
    auto& hub = Corona::SharedDataHub::instance();
    auto updates = hub.drain_transform_updates();
    if (updates.empty()) {
        return;
    }

    for (const auto& update : updates) {
        if (auto transform = Corona::find_transform(update.transform_handle)) {
            if (Corona::has_transform_field(update.fields, Corona::TransformField::Position)) {
                transform->position = update.position;
            }
            if (Corona::has_transform_field(update.fields, Corona::TransformField::Rotation)) {
                transform->euler_rotation = update.euler_rotation;
            }
            if (Corona::has_transform_field(update.fields, Corona::TransformField::Scale)) {
                transform->scale = update.scale;
            }
            if (Corona::has_transform_field(update.fields, Corona::TransformField::Matrix)) {
                // 列主序仿射矩阵：与 API::Geometry::set_matrix 的写入口径一致，
                // TRS 归一化为单位（矩阵本身已经含平移/旋转/缩放/剪切）。
                for (int c = 0; c < 4; ++c) {
                    for (int r = 0; r < 4; ++r) {
                        transform->affine_basis[c][r] = update.matrix[c * 4 + r];
                    }
                }
                transform->affine_basis[3] = ktm::fvec4{0.0f, 0.0f, 0.0f, 1.0f};
                transform->position = update.position;
                transform->euler_rotation = ktm::fvec3{0.0f, 0.0f, 0.0f};
                transform->scale = ktm::fvec3{1.0f, 1.0f, 1.0f};
            }
        }
    }
}

void apply_pending_camera_releases() {
    auto& hub = Corona::SharedDataHub::instance();
    for (const auto& release : hub.drain_camera_releases()) {
        hub.clear_camera_state_updates(release.camera_handle);
        hub.destroy_camera(release.camera_handle);
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
    return Corona::Horizon::ImageUsage_Storage |
           Corona::Horizon::ImageUsage_ColorAttachment |
           Corona::Horizon::ImageUsage_Sampled |
           Corona::Horizon::ImageUsage_TransferSrc |
           Corona::Horizon::ImageUsage_TransferDst;
}

[[nodiscard]] Corona::Horizon::HardwareImage make_storage_image(
    uint32_t width,
    uint32_t height,
    Corona::Horizon::Format format,
    std::string_view name) {
    auto image = Corona::Horizon::HardwareImage(Corona::Horizon::HardwareImageDesc::texture_2d(
        width,
        height,
        format,
        optics_storage_image_usage(),
        std::string(name)));
    return image;
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
[[nodiscard]] Corona::Horizon::HardwareBuffer make_storage_buffer(
    uint64_t count,
    std::string_view name,
    Corona::Horizon::CpuAccessMode cpu_access = Corona::Horizon::CpuAccessMode::Write) {
    Corona::Horizon::HardwareBufferDesc desc;
    desc.element_count = count;
    desc.element_size = static_cast<uint32_t>(sizeof(T));
    desc.usage = Corona::Horizon::BufferUsage_TransferSrc |
                 Corona::Horizon::BufferUsage_TransferDst |
                 Corona::Horizon::BufferUsage_Storage;
    // Read-back buffers must declare host read access: HardwareBuffer::read_bytes
    // refuses a buffer that is host-write-only, and it is the call that invalidates
    // non-coherent host memory before the CPU reads GPU results.
    desc.cpu_access = cpu_access;
    desc.debug_name = std::string(name);
    (void)desc.byte_size();
    return Corona::Horizon::HardwareBuffer(desc);
}

[[nodiscard]] std::uint64_t grow_table_capacity(std::uint64_t current,
                                                std::uint64_t required) {
    std::uint64_t next = current == 0 ? 1 : current;
    while (next < required) {
        next *= 2;
    }
    return next;
}

template <typename T>
bool ensure_storage_buffer_capacity(Corona::Horizon::HardwareBuffer& buffer,
                                    std::uint64_t& capacity,
                                    std::uint64_t required,
                                    const std::string& name) {
    if (required <= capacity && buffer) {
        return true;
    }

    const std::uint64_t old_capacity = capacity;
    const std::uint64_t new_capacity = grow_table_capacity(capacity, required);
    auto new_buffer = make_storage_buffer<T>(new_capacity, name);
    if (!new_buffer) {
        CFW_LOG_ERROR("OpticsSystem: failed to resize {} table buffer from {} to {} entries",
                      name, old_capacity, new_capacity);
        return false;
    }

    buffer = std::move(new_buffer);
    capacity = new_capacity;
    CFW_LOG_WARNING("OpticsSystem: resized {} table buffer from {} to {} entries (required={})",
                    name, old_capacity, new_capacity, required);
    return true;
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


bool collect_ui_instances(
    const Corona::SceneDevice& scene,
    Corona::Horizon::RasterizerPipeline<visibility_vert_glsl_t, visibility_frag_glsl_t>& target_visibility,
    const ktm::fmat4x4& camera_basis,
    RenderInstanceBatch& batch,
    const Corona::Systems::GeometrySystem* geometry_system,
    std::uint64_t frame_index,
    std::vector<Corona::Horizon::HardwareBuffer>& out_indirect_buffers) {
    batch.clear();
    const auto& diag = optics_diag_config();


    bool has_instances = false;
    uint32_t recorded_draws = 0;
    uint32_t object_id = 1;
    for (auto actor_handle : scene.actor_handles) {
        auto actor = Corona::find_actor(actor_handle);
        const auto* actor_profiles = Corona::find_profiles(actor_handle);
        if (!actor || !actor_profiles) {
            ++object_id;
            continue;
        }
        if (!diag_actor_allowed(actor_handle)) {
            ++object_id;
            continue;
        }

        // ������Դ����ʱ�䣨���� ResourceManager LRU��
        {
            for (auto ph : actor_profiles->handles) {
                auto prof = Corona::find_profile(ph);
                if (!prof || !prof->geometry_handle) continue;
                auto g = Corona::find_geometry(prof->geometry_handle);
                if (!g || !g->model_resource_handle) continue;
                auto mr = Corona::find_model_resource(g->model_resource_handle);
                if (mr && mr->model_id) {
                    Corona::Resource::ResourceManager::get_instance().touch(mr->model_id);
                }
                break;
            }
        }

        const auto* actor_follow_camera = Corona::find_follow_camera(actor_handle);
        if (!actor_follow_camera || !actor_follow_camera->enabled) {
            ++object_id;
            continue;
        }

        for (auto profile_handle : actor_profiles->handles) {
            auto profile = Corona::find_profile(profile_handle);
            if (!profile || profile->optics_handle == 0) continue;

            auto optics_acc = Corona::find_optics(profile->optics_handle);
            if (!optics_acc) continue;
            const auto& optics = *optics_acc;
            const auto* optics_visibility = Corona::find_visibility(profile->optics_handle);

            if (optics_visibility == nullptr || !optics_visibility->visible) {
                ++object_id;
                continue;
            }
            if (!diag_geometry_allowed(optics.geometry_handle)) {
                continue;
            }
            // ---- ��ȡ���α任��Ϣ���������ɣ�texture �� query_mesh_slots �ڲ������----
            EntityId transform_handle_a = 0;
            float          nlc_scale_a   = 1.0f;
            ktm::fvec3     nlc_offset_a  = {0.0f, 0.0f, 0.0f};
            if (auto geom_r = Corona::find_geometry(optics.geometry_handle)) {
                transform_handle_a = geom_r->transform_handle;
                nlc_scale_a        = geom_r->native_local_correction_scale;
                nlc_offset_a       = geom_r->native_local_correction_offset;
            }

            ktm::fmat4x4 model_matrix{ktm::fmat4x4::from_eye()};
            if (auto transform = Corona::find_transform(transform_handle_a)) {
                model_matrix = transform->compute_matrix();
                // ����ȡ�� nlc �������� apply_native_local_correction �߼�
                if (std::abs(nlc_scale_a - 1.0f) > 1e-6f ||
                    std::abs(nlc_offset_a.x) > 1e-6f ||
                    std::abs(nlc_offset_a.y) > 1e-6f ||
                    std::abs(nlc_offset_a.z) > 1e-6f) {
                    ktm::fmat4x4 correction = ktm::fmat4x4::from_eye();
                    correction[0][0] = nlc_scale_a;
                    correction[1][1] = nlc_scale_a;
                    correction[2][2] = nlc_scale_a;
                    correction[3][0] = nlc_offset_a.x;
                    correction[3][1] = nlc_offset_a.y;
                    correction[3][2] = nlc_offset_a.z;
                    model_matrix = multiply_ktm_mat4(model_matrix, correction);
                }
                model_matrix = multiply_ktm_mat4(camera_basis, model_matrix);
            }

            // ---- query_mesh_slots��ͳһ��ȡ���� mesh �� LOD ���� + ������Ϣ ----
            // �ڲ�ȡ����������ֵΪֵ������refcount ��ȫ���������ٳ��� geometry д����
            // valid=false ���� Actor �״μ���δ���ʱ���֣���Ψһ�Ϸ�����ԭ��
            const auto mesh_slots = geometry_system
                ? geometry_system->query_mesh_slots(optics.geometry_handle)
                : std::vector<Corona::Systems::GeometrySystem::MeshSlot>{};

            // CRITICAL FIX: Reserve space in caller's buffer vector.
            // Buffers must stay alive until after commit() in the caller.
            out_indirect_buffers.reserve(out_indirect_buffers.size() + mesh_slots.size());

            for (const auto& ms : mesh_slots) {
                if (!diag_mesh_allowed(ms.mesh_index)) continue;
                if (recorded_draws >= diag.draw_limit) return has_instances;

                const bool vertex_valid = static_cast<bool>(ms.geo.vertex);
                const bool index_valid = static_cast<bool>(ms.geo.index);
                const bool vertex_storage_valid = static_cast<bool>(ms.geo.vertex_storage);
                const bool index_storage_valid = static_cast<bool>(ms.geo.index_storage);
                const uint32_t texture_descriptor =
                    (diag.disable_textures || diag.force_solid_material || !ms.texture)
                        ? 0u
                        : ms.texture.store_descriptor();
                const uint32_t vertex_descriptor = vertex_storage_valid
                    ? ms.geo.vertex_storage.store_descriptor()
                    : 0u;
                const uint32_t index_descriptor = index_storage_valid
                    ? ms.geo.index_storage.store_descriptor()
                    : 0u;

                if (!ms.valid ||
                    !vertex_valid || !index_valid ||
                    vertex_descriptor == 0u || index_descriptor == 0u) {
                    log_invalid_optics_mesh_once(
                        actor_handle,
                        optics.geometry_handle,
                        ms.mesh_index,
                        ms.valid,
                        vertex_valid,
                        index_valid,
                        vertex_storage_valid,
                        index_storage_valid,
                        vertex_descriptor,
                        index_descriptor,
                        texture_descriptor);
                    continue;
                }

                const ktm::fmat4x4 mesh_model_matrix =
                    multiply_ktm_mat4(model_matrix, ms.instance_transform);
                auto material_id = static_cast<uint32_t>(batch.materials.size());
                batch.materials.push_back(pack_ui_material(ms, optics, texture_descriptor));
                log_optics_material_once(actor_handle,
                                         optics.geometry_handle,
                                         ms.mesh_index,
                                         material_id,
                                         texture_descriptor,
                                         ms.texture,
                                         ms.texture_ready,
                                         diag.disable_albedo_sample);
                batch.keep_mesh_resources(ms, texture_descriptor);

                auto instance_id = static_cast<uint32_t>(batch.instances.size());
                {
                    Hardware::InstanceInfo inst{};
                    inst.modelMatrix = mesh_model_matrix;
                    inst.vertexBufferIndex = vertex_descriptor;
                    inst.indexBufferIndex = index_descriptor;
                    inst.materialID = material_id;
                    inst.objectID = object_id;
                    inst.indexCount = ms.index_count;
                    inst.vertexCount = ms.vertex_count;
                    inst.maxIndex = ms.max_index;
                    inst.flags = 0u;
                    batch.instances.push_back(inst);
                    has_instances = true;
                }

                using Pipeline = Corona::Horizon::RasterizerPipeline<visibility_vert_glsl_t, visibility_frag_glsl_t>;
                auto& pc = static_cast<Pipeline::VertexResourceBindings&>(target_visibility).pushConsts;
                pc.modelMatrix = upload_value(mesh_model_matrix);
                pc.instanceID = instance_id + 1;

                // 新版 Horizon: 使用 indirect draw
                Corona::Horizon::DrawIndexedIndirectCommand draw_cmd{};
                draw_cmd.index_count = ms.index_count;
                draw_cmd.first_index = 0;
                draw_cmd.vertex_offset = 0;
                draw_cmd.instance_count = 1;
                draw_cmd.first_instance = 0;

                auto indirect_buffer =
                    Corona::Horizon::HardwareBuffer::from_bytes(
                        std::as_bytes(std::span(&draw_cmd, 1)),
                        sizeof(draw_cmd),
                        Corona::Horizon::BufferUsage_Indirect,
                        make_optics_draw_label(
                            "ui_visibility",
                            actor_handle,
                            optics.geometry_handle,
                            ms.mesh_index,
                            static_cast<std::uint32_t>(frame_index),
                            instance_id + 1,
                            material_id,
                            texture_descriptor,
                            vertex_descriptor,
                            index_descriptor,
                            ms.vertex_count,
                            ms.index_count,
                            ms.max_index));

                Corona::Horizon::DrawIndexedIndirectParams draw_params;
                draw_params.draw_count = 1;
                draw_params.indirect_offset = 0;
                draw_params.stride = sizeof(Corona::Horizon::DrawIndexedIndirectCommand);

                target_visibility.record_indirect(ms.geo.index, ms.geo.vertex, indirect_buffer, draw_params);

                // Keep buffer alive until after commit() (in caller's vector)
                out_indirect_buffers.push_back(std::move(indirect_buffer));
                ++recorded_draws;
            }
            ++object_id;
        }
    }
    return has_instances;
}

bool upload_instance_tables(const RenderInstanceBatch& batch,
                            Hardware& hardware,
                            Corona::Horizon::HardwareBuffer& instance_buffer,
                            std::uint64_t& instance_capacity,
                            Corona::Horizon::HardwareBuffer& material_buffer,
                            std::uint64_t& material_capacity,
                            std::string_view label) {
    const auto instance_count = static_cast<std::uint64_t>(batch.instances.size());
    const auto material_count = static_cast<std::uint64_t>(batch.materials.size());
    if (instance_count > instance_capacity || material_count > material_capacity ||
        !instance_buffer || !material_buffer) {
        if (hardware.last_receipt.serial != 0) {
            hardware.executor.wait_idle(hardware.last_receipt);
        }
    }

    const std::string instance_name = std::string(label) + ".instances";
    if (!ensure_storage_buffer_capacity<Hardware::InstanceInfo>(
            instance_buffer, instance_capacity, instance_count, instance_name)) {
        return false;
    }

    const std::string material_name = std::string(label) + ".materials";
    if (!ensure_storage_buffer_capacity<Hardware::MaterialInfo>(
            material_buffer, material_capacity, material_count, material_name)) {
        return false;
    }

    if (!batch.instances.empty()) {
        if (!write_array_bytes(instance_buffer, batch.instances.data(), batch.instances.size())) {
            CFW_LOG_ERROR("OpticsSystem: failed to upload {} instance table ({} entries, capacity={})",
                          label, instance_count, instance_capacity);
            return false;
        }
    }
    if (!batch.materials.empty()) {
        if (!write_array_bytes(material_buffer, batch.materials.data(), batch.materials.size())) {
            CFW_LOG_ERROR("OpticsSystem: failed to upload {} material table ({} entries, capacity={})",
                          label, material_count, material_capacity);
            return false;
        }
    }
    return true;
}

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
    // Engine-built scenes render camera contexts.
    // Keep the unused base renderer's path/history resources deferred.
    // Preparing it at the viewport resolution can duplicate gigabytes of state.
    project_desc.pipeline_desc.init(vision::ParameterSet{vision::DataWrap{
        {"param", {{"defer_base_compile", true}}}}});
    project_desc.renderer_desc.sampler_desc.init(empty_ps);
    project_desc.renderer_desc.spectrum_desc.init(empty_ps);
    // Corona-built scenes use the existing uniform, non-separated proposal.
    project_desc.renderer_desc.light_sampler_desc.init(vision::ParameterSet{vision::DataWrap{
        {"type", "uniform"}, {"param", {{"env_separate", false}}}}});
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
    const std::shared_ptr<Corona::Systems::Vision::VisionSceneResource>& scene_resource,
    bool participating_media = false)
    -> ocarina::SP<vision::Pipeline> {
    auto project_desc = make_default_vision_project_desc();
    project_desc.renderer_desc.integrator_desc.sub_type =
        mode == Corona::CameraVisionRenderMode::ReSTIR ? "rt" : "pt";
    if (mode == Corona::CameraVisionRenderMode::ReSTIR) {
        auto data = vision::DataWrap::object();
        Corona::Systems::Vision::configure_vision_scene_for_mode(data, mode);
        project_desc.renderer_desc.integrator_desc.init(
            vision::ParameterSet{data["render"]["integrator"]});
    } else if (participating_media) {
        project_desc.renderer_desc.integrator_desc.init(vision::ParameterSet{
            vision::DataWrap{{"type", "pt"}, {"param", {{"participating_media", true}}}}});
    }
    if (mode != Corona::CameraVisionRenderMode::SSAT) {
        project_desc.pipeline_desc.frame_buffer_desc.init(vision::ParameterSet{
            vision::DataWrap{{"type", "normal"},
                             {"param", {{"accumulation", mode == Corona::CameraVisionRenderMode::ProgressivePathTracing}}}}});
    }
    project_desc.output_desc.denoise =
        Corona::Systems::Vision::vision_render_mode_uses_denoise(mode);
    auto pipeline = vision::Node::create_shared<vision::Pipeline>(project_desc.pipeline_desc);
    if (!pipeline) {
        return {};
    }
    bind_pipeline_scene_resource_early(*pipeline, scene_resource);
    pipeline->init_project(project_desc);
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
    if (denoiser == nullptr || !denoiser->enabled() || denoiser->has_prepared_resources()) {
        return;
    }
    vision::Global::SceneGpuContextScope scene_gpu_context{
        pipeline.scene().geometry().bindless_array(),
        pipeline.scene().geometry().gpu_resource()->device()};
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

void apply_pending_default_surface_camera_update() {
    auto& hub = Corona::SharedDataHub::instance();
    const auto update = hub.take_default_surface_camera_update();
    if (!update) return;
    for (const auto& scene : Corona::all_scenes()) {
        if (!scene.enabled) continue;
        auto camera = Corona::find_camera(select_scene_camera_handle(scene));
        if (!camera || !camera->follows_default_surface ||
            (camera->surface && camera->surface != update->surface)) continue;
        camera->surface = update->surface;
        camera->width = update->width;
        camera->height = update->height;
        camera->aspect = static_cast<float>(update->width) / static_cast<float>(update->height);
    }
}

struct VisionModeSelection {
    bool has_visible_camera{false};
    Corona::CameraVisionRenderMode mode{Corona::CameraVisionRenderMode::PathTracing};
    std::uintptr_t selected_camera{0};
    bool conflict{false};
    std::size_t conflict_signature{0};
    std::string conflict_summary;

    void add(std::uintptr_t camera, Corona::CameraVisionRenderMode requested_mode) {
        if (!has_visible_camera) {
            has_visible_camera = true;
            mode = requested_mode;
            selected_camera = camera;
        } else if (requested_mode != mode) {
            conflict = true;
        }
        if (!conflict_summary.empty()) conflict_summary.append(", ");
        conflict_summary.append("camera ").append(std::to_string(camera)).append("=");
        conflict_summary.append(Corona::Systems::Vision::vision_render_mode_name(requested_mode));
        conflict_signature = std::hash<std::string>{}(conflict_summary);
    }
};

template<typename Views>
[[nodiscard]] VisionModeSelection select_vision_render_mode(const Views& views) {
    VisionModeSelection selection;
    for (const auto& view : views)
        if (!view.participating_media) selection.add(view.camera_handle, view.vision_mode);
    return selection;
}

[[nodiscard]] bool camera_uses_vision_media(const Corona::CameraDevice& camera) {
    using namespace Corona::Systems::Vision;
    if (!vision_media_enabled(camera.vision_render_mode, camera.vision_accumulation,
                              camera.vision_denoise, true)) return false;
    // Settings are typed JSON on the component (no wire-format round trip).
    const auto& settings = camera.vision_pt_settings;
    const auto requested = settings.find("participating_media");
    return requested != settings.end() && requested->is_boolean() && requested->get<bool>();
}

void add_vision_media_feedback(vision::DataWrap& settings, std::size_t imported_media_count,
                               std::size_t boundary_count, bool enabled) {
    settings["imported_media_count"] = imported_media_count;
    settings["medium_boundary_count"] = boundary_count;
    settings["media_enabled"] = enabled;
}

[[nodiscard]] std::optional<Corona::CameraDevice> active_scene_camera_values() {
    auto& hub = Corona::SharedDataHub::instance();
    for (const auto& scene : Corona::all_scenes()) {
        if (!scene.enabled) continue;
        const auto handle = select_scene_camera_handle(scene);
        if (auto camera = Corona::find_camera(handle)) return *camera;
    }
    return std::nullopt;
}

[[nodiscard]] Corona::EnvironmentDevice vision_environment_for_scene(const Corona::SceneDevice& scene) {
    if (scene.environment != 0) {
        if (const auto* env = Corona::SharedDataHub::instance().try_environment(scene.environment)) {
            return *env;
        }
    }
    Corona::EnvironmentDevice dark{};
    dark.sun_intensity = 0.0f;
    dark.sky_intensity = 0.0f;
    return dark;
}

// EngineBuilt currently represents the single enabled Corona scene. Never use
// EnvironmentDevice's bright defaults when that scene has no environment.
[[nodiscard]] Corona::EnvironmentDevice engine_vision_environment() {
    for (const auto& scene : Corona::all_scenes()) {
        if (scene.enabled) return vision_environment_for_scene(scene);
    }
    return vision_environment_for_scene(Corona::SceneDevice{});
}

[[nodiscard]] VisionModeSelection select_visible_vision_render_mode(
    const std::function<bool(std::uintptr_t)>& pending_capture = {}) {
    VisionModeSelection selection;

    for (const auto& scene : Corona::all_scenes()) {
        if (!scene.enabled) continue;
        for (const auto camera_handle : scene.camera_handles) {
            auto camera =
                Corona::find_camera(camera_handle);
            if (!camera ||
                (camera->surface == nullptr && (!pending_capture || !pending_capture(camera_handle)))) {
                continue;
            }

            selection.add(camera_handle, camera->vision_render_mode);
        }
    }

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
    auto name = std::string(Corona::Systems::Vision::vision_render_mode_name(key.mode));
    if (key.participating_media) name += " with participating media";
    return name;
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
    Corona::CameraVisionRenderMode mode) {
    ocarina::switch_profile::Scope profile{"scene.bind_gpu_resource", "geometry"};
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
            "(mode={})",
            std::string(Corona::Systems::Vision::vision_render_mode_name(mode)));
    } else {
        CFW_LOG_INFO(
            "OpticsSystem: reusing per-runtime Vision scene GPU view "
            "(mode={})",
            std::string(Corona::Systems::Vision::vision_render_mode_name(mode)));
    }
    pipeline.scene().bind_geometry_gpu_resource(std::move(scene_gpu_resource));
    if (scene_resource.logical_instance_count() == 0u) {
        sync_logical_instances_from_pipeline_scene(scene_resource, pipeline.scene());
    } else {
        apply_logical_instances_to_pipeline_scene(scene_resource, pipeline.scene());
    }
}

}  // namespace

namespace Corona::Systems {

Kernel::ComponentAccess OpticsSystem::component_access() const {
    Kernel::ComponentAccess access;
    access.write<OpticsDevice, CameraDevice, ImageDevice>()
          .read<Ecs::Transform, GeometryDevice, OpticsDevice, CameraDevice,
                SceneDevice, EnvironmentDevice, ActorDevice, ProfileDevice,
                AcousticsDevice, MechanicsDevice>();
    return access;
}

using RenderDetail::RetainSubmission;
using RenderDetail::bind_storage_image;
using RenderDetail::bind_storage_buffer;

namespace {
constexpr auto kScreenshotRequestTimeout = std::chrono::seconds(30);
}

struct OpticsSystem::UiViewResources {
    Horizon::HardwareImage ui_visibility;  ///< RG32_UINT StorageImage
    Horizon::HardwareImage ui_depth;       ///< D32_FLOAT DepthImage
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t last_used_frame = 0;
};

struct OpticsSystem::VisionPipelineRuntime {
    ocarina::SP<vision::Pipeline> pipeline;
    std::shared_ptr<VisionSceneResource> scene_resource;
    Corona::CameraVisionRenderMode mode{Corona::CameraVisionRenderMode::PathTracing};
    uint64_t last_used_frame{0};
    uint64_t scene_gpu_transform_version{0};
    bool engine_geometry_ready{false};
    bool engine_geometry_available{false};
    bool participating_media{false};
    std::size_t applied_signature{0};
    std::size_t pending_signature{0};
    uint32_t rebuild_retries{0};
    Vision::VisionGeometrySyncDebounce geometry_debounce;
    std::unique_ptr<Vision::VisionSkinningBridge> skinning;
    std::vector<Vision::VisionInstanceTransform> instance_transforms;
    bool transforms_pending{};
    bool skinning_ready{true};

    void bind_dynamic_geometry(Vision::VisionBuildResult& result) {
        instance_transforms = std::move(result.transforms);
        transforms_pending = false;
        auto& meshes = result.skinned_meshes;
        skinning = meshes.empty() && instance_transforms.empty() ? nullptr : std::make_unique<Vision::VisionSkinningBridge>(pipeline->device(), std::move(meshes));
        skinning_ready = true;
    }
    std::optional<Vision::VisionLightingState> engine_lighting;
    std::chrono::steady_clock::time_point next_geometry_retry{};

    // Per-view shared buffers carry Vision's resolved display pixels to Vulkan.
    std::unordered_map<std::uintptr_t, std::unique_ptr<Vision::VisionZeroCopyBridge>> bridges;
    std::unordered_map<std::uintptr_t, std::pair<std::string, bool>> incompatible_views;
    std::unordered_set<std::uintptr_t> retained_contexts;
    std::unordered_map<std::uintptr_t, bool> view_denoise_states;
    std::unordered_map<std::uintptr_t, CameraVisionDenoiser> view_denoiser_types;
    std::unordered_map<std::uintptr_t, Horizon::SubmitReceipt> interop_submissions;
    Horizon::HardwareExecutor* interop_executor{nullptr};

    void wait_for_interop_submission(EntityId camera_handle,
                                     std::string_view reason,
                                     bool log_wait = true) noexcept {
        auto receipt_it = interop_submissions.find(camera_handle);
        if (receipt_it == interop_submissions.end()) {
            return;
        }
        if (receipt_it->second.serial == 0) {
            interop_submissions.erase(receipt_it);
            return;
        }
        if (interop_executor == nullptr) {
            CFW_LOG_ERROR(
                "OpticsSystem: cannot wait for Vision interop submission before {} "
                "(camera={}, receipt_serial={})",
                reason,
                camera_handle,
                receipt_it->second.serial);
            return;
        }

        if (log_wait) {
            CFW_LOG_INFO(
                "OpticsSystem: waiting for Vision interop submission before {} "
                "(camera={}, receipt_serial={})",
                reason,
                camera_handle,
                receipt_it->second.serial);
        }
        try {
            interop_executor->wait_idle(receipt_it->second);
        } catch (const std::exception& e) {
            CFW_LOG_ERROR(
                "OpticsSystem: Vision interop wait failed before {} "
                "(camera={}, receipt_serial={}, error={})",
                reason,
                camera_handle,
                receipt_it->second.serial,
                e.what());
        }
        interop_submissions.erase(receipt_it);
    }

    void commit_and_clear_contexts() noexcept {
        CFW_LOG_INFO(
            "OpticsSystem: Vision runtime teardown begin "
            "(bridges={}, interop_submissions={}, contexts={})",
            bridges.size(),
            interop_submissions.size(),
            retained_contexts.size());
        if (pipeline) {
            CFW_LOG_INFO("OpticsSystem: Vision runtime teardown committing compute work");
            pipeline->commit_command();
            CFW_LOG_INFO("OpticsSystem: Vision runtime teardown compute work committed");
        }
        Vision::drain_vision_interop_submissions(
            interop_submissions,
            [&](EntityId camera_handle, const Horizon::SubmitReceipt& receipt) {
                if (interop_executor == nullptr) {
                    CFW_LOG_ERROR(
                        "OpticsSystem: missing executor while draining Vision interop submission "
                        "(camera={}, receipt_serial={})",
                        camera_handle,
                        receipt.serial);
                    return;
                }
                CFW_LOG_INFO(
                    "OpticsSystem: draining Vision interop submission "
                    "(camera={}, receipt_serial={})",
                    camera_handle,
                    receipt.serial);
                try {
                    interop_executor->wait_idle(receipt);
                } catch (const std::exception& e) {
                    CFW_LOG_ERROR(
                        "OpticsSystem: failed to drain Vision interop submission "
                        "(camera={}, receipt_serial={}, error={})",
                        camera_handle,
                        receipt.serial,
                        e.what());
                }
            },
            [&] {
                CFW_LOG_INFO("OpticsSystem: releasing Vision camera interop resources");
                clear_camera_runtime_state();
            });
        if (pipeline) {
            CFW_LOG_INFO("OpticsSystem: clearing Vision view contexts");
            pipeline->clear_view_contexts();
        }
        CFW_LOG_INFO("OpticsSystem: Vision runtime teardown complete");
    }

    void clear_camera_runtime_state() noexcept {
        bridges.clear();
        incompatible_views.clear();
        retained_contexts.clear();
        view_denoise_states.clear();
        view_denoiser_types.clear();
    }

    void bind_shared_scene_gpu_resource() {
        if (!pipeline || !scene_resource) {
            return;
        }
        const bool needs_geometry_prepare =
            !pipeline->scene().geometry().has_gpu_resource();
        bind_pipeline_scene_gpu_resource(
            *pipeline, *scene_resource, mode);
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

    void wait_for_geometry_users() {
        pipeline->commit_command();
        for (const auto& [camera, receipt] : interop_submissions) {
            if (receipt.serial == 0) continue;
            if (!interop_executor) throw std::runtime_error("missing geometry interop executor");
            interop_executor->wait_idle(receipt);
        }
        interop_submissions.clear();
    }



    void reset_pipeline(ocarina::SP<vision::Pipeline> next_pipeline,
                        Corona::CameraVisionRenderMode next_mode) {
        commit_and_clear_contexts();
        pipeline = std::move(next_pipeline);
        mode = next_mode;
        scene_gpu_transform_version = 0;
        engine_geometry_ready = false;
        engine_geometry_available = false;
        geometry_debounce.reset();
        engine_lighting.reset();
        next_geometry_retry = {};
        bind_shared_scene_gpu_resource();
    }
};

bool OpticsSystem::prepare_vision_camera_view(VisionPipelineRuntime& runtime,
                                            EntityId camera_handle,
                                            uint32_t width, uint32_t height,
                                            bool denoise, bool accumulation, bool stable_planes,
                                            CameraVisionDenoiser denoiser,
                                            const nlohmann::json& reblur_settings,
                                            const nlohmann::json& relax_settings,
                                            const nlohmann::json& pt_settings,
                                            const nlohmann::json& restir_settings,
                                            VisionCameraFeedback* deferred_feedback,
                                            std::size_t imported_media_count) {
    ocarina::switch_profile::Scope profile{"view.prepare", "view"};
    auto& pipeline = runtime.pipeline;
    if (!pipeline || camera_handle == 0) return false;
    VisionCameraFeedback feedback;
    pipeline->activate_global_context();
    const auto resolution = ocarina::make_uint2(std::max(width, 1u), std::max(height, 1u));
    bool view_created = false;
    if (runtime.mode == CameraVisionRenderMode::SSAT) denoiser = CameraVisionDenoiser::Scene;
    if (pipeline->has_view_context(camera_handle)) {
        if (!pipeline->activate_view_context(camera_handle)) return false;
        const auto* fb = pipeline->frame_buffer();
        const auto state = runtime.view_denoise_states.find(camera_handle);
        // PT enable toggles select its lobe producer; ReSTIR kernels are reused. Changing the denoiser type
        // rebuilds this view so REBLUR's lobe producers and history agree.
        // SSAT also needs recreation for its GBuffer callbacks.
        const bool recreate = !fb || fb->resolution().x != resolution.x ||
                              fb->resolution().y != resolution.y ||
                              state == runtime.view_denoise_states.end() ||
                              runtime.view_denoiser_types.at(camera_handle) != denoiser ||
                              (runtime.mode == CameraVisionRenderMode::SSAT &&
                               state->second != denoise);
        if (recreate) {
            pipeline->commit_command();
            runtime.wait_for_interop_submission(camera_handle, "view context recreation");
            runtime.bridges.erase(camera_handle);
            runtime.retained_contexts.erase(camera_handle);
            runtime.view_denoise_states.erase(camera_handle);
            runtime.view_denoiser_types.erase(camera_handle);
            pipeline->remove_view_context(camera_handle);
        }
    }
    if (!pipeline->has_view_context(camera_handle)) {
        // Renderer preparation must see the desired state so it allocates SVGF
        // buffers and registers the GBuffer callback before compiling kernels.
        pipeline->activate_view_context(0u);
        pipeline->set_output_denoise(denoise);
        const auto type = denoiser == CameraVisionDenoiser::ReBLUR ? "ReBLUR" :
                          denoiser == CameraVisionDenoiser::ReLAX ? "ReLAX" :
                          denoiser == CameraVisionDenoiser::SVGF ? "svgf" : "";
        const bool created = pipeline->create_view_context(camera_handle, resolution, type);
        pipeline->activate_view_context(0u);
        pipeline->set_output_denoise(Vision::vision_render_mode_uses_denoise(runtime.mode));
        if (!created) {
            CFW_LOG_ERROR("OpticsSystem: unable to allocate Vision view context for camera {}",
                          camera_handle);
            return false;
        }
        runtime.view_denoise_states[camera_handle] = denoise;
        runtime.view_denoiser_types[camera_handle] = denoiser;
        view_created = true;
    }
    if (!pipeline->activate_view_context(camera_handle)) return false;
    pipeline->renderer().integrator()->set_stable_planes_enabled(stable_planes);
    const bool is_restir = runtime.mode == CameraVisionRenderMode::ReSTIR;
    if (is_restir || runtime.mode == CameraVisionRenderMode::PathTracing ||
        runtime.mode == CameraVisionRenderMode::ProgressivePathTracing) {
        auto* integrator = pipeline->renderer().integrator().get();
        const auto& json = is_restir ? restir_settings : pt_settings;
        auto overrides = json.is_object() ? json : vision::DataWrap::object();
        if (!is_restir) {
            overrides["participating_media"] = runtime.participating_media;
            if (!runtime.participating_media && overrides.contains("medium_diagnostics"))
                overrides["medium_diagnostics"] = false;
        }
        const bool changed = integrator->set_settings(overrides);
        if (changed) pipeline->invalidate();
        if (is_restir) {
            if (Vision::validate_restir_settings(overrides).empty()) {
                // Media counts and the effective policy reach the editor every frame: the
                // request cannot know whether the runtime mode denied participating media.
                auto effective = integrator->settings();
                add_vision_media_feedback(effective, imported_media_count, 0, false);
                feedback.restir_effective = effective;
                feedback.restir_applied = json;
            }
        } else {
            auto effective = integrator->settings();
            add_vision_media_feedback(effective, imported_media_count,
                                      pipeline->scene().geometry().boundary_count(),
                                      runtime.participating_media);
            feedback.pt_effective = effective;
        }
    }
    if (auto* illumination = dynamic_cast<vision::IlluminationIntegrator*>(pipeline->renderer().integrator().get())) {
        if (auto* filter = illumination->denoiser(); filter && filter->set_reblur_settings(reblur_settings)) {
            pipeline->invalidate();
            feedback.reblur_effective = filter->reblur_settings();
        }
        feedback.reblur_contexts = illumination->reblur_context_settings();
        if (auto* filter = illumination->denoiser(); filter && filter->impl_type() == "ReLAX") {
            const bool changed = filter->set_relax_settings(relax_settings);
            if (changed) pipeline->invalidate();
            if (view_created || changed) {
                feedback.relax_effective = filter->relax_settings();
            }
        }
    }
    // Output settings are shared by the pipeline, while renderers and histories
    // belong to cameras. Restore the active camera's preference on every visit.
    pipeline->set_output_denoise(denoise);
    auto& denoise_state = runtime.view_denoise_states.at(camera_handle);
    if (denoise_state != denoise) {
        pipeline->commit_command();
        runtime.wait_for_interop_submission(camera_handle, "denoise toggle");
        if (denoise) {
            // Allocate and compile only the denoiser on its first use. Retain
            // these resources when disabled so subsequent switches are cheap.
            // SVGF's compute_GBuffer callback is empty; no geometry recompile
            // is required when registering it after the renderer was prepared.
            prepare_enabled_denoiser_for_runtime_switch(*pipeline);
        }
        denoise_state = denoise;
        pipeline->invalidate();
    }
    if (runtime.mode != CameraVisionRenderMode::SSAT &&
        pipeline->frame_buffer()->enable_accumulation() != accumulation) {
        // Accumulation belongs to the active camera, independently of its
        // integrator and denoiser. Drain GPU readers before reallocating buffers.
        pipeline->commit_command();
        runtime.wait_for_interop_submission(camera_handle, "accumulation toggle");
        pipeline->frame_buffer()->set_enable_accumulation(accumulation);
        pipeline->frame_buffer()->auto_manage_accumulation_buffer(accumulation);
        pipeline->invalidate();
    }
    // Publish runtime feedback separately from the frame's camera values.
    if (deferred_feedback) *deferred_feedback = std::move(feedback);
    else publish_vision_camera_feedback(camera_handle, feedback);
    return true;
}

void OpticsSystem::publish_vision_camera_feedback(EntityId camera_handle,
                                                 const VisionCameraFeedback& feedback) {
    if (!feedback.restir_effective && !feedback.pt_effective &&
        !feedback.reblur_effective && !feedback.relax_effective && !feedback.reblur_contexts) return;
    auto camera = Corona::find_camera(camera_handle);
    if (!camera) return;
    if (feedback.restir_effective) camera->vision_restir_effective_settings = *feedback.restir_effective;
    if (feedback.restir_applied) camera->vision_restir_applied_settings = *feedback.restir_applied;
    if (feedback.pt_effective) camera->vision_pt_effective_settings = *feedback.pt_effective;
    if (feedback.reblur_effective) camera->vision_reblur_effective_settings = *feedback.reblur_effective;
    if (feedback.reblur_contexts) camera->vision_reblur_context_settings = *feedback.reblur_contexts;
    if (feedback.relax_effective) camera->vision_relax_effective_settings = *feedback.relax_effective;
}

OpticsSystem::VisionPipelineKey OpticsSystem::make_vision_pipeline_key(CameraVisionRenderMode mode) const {
    return VisionPipelineKey{mode};
}

std::shared_ptr<OpticsSystem::VisionSceneResource> OpticsSystem::get_or_create_vision_scene_resource() {
    if (!vision_scene_resource_) vision_scene_resource_ = std::make_shared<VisionSceneResource>();
    return vision_scene_resource_;
}

void OpticsSystem::release_unused_vision_scene_resources() {
    const bool in_use = std::any_of(vision_runtimes_.begin(), vision_runtimes_.end(),
        [&](const auto& entry) { return entry.second->scene_resource == vision_scene_resource_; });
    if (!in_use) vision_scene_resource_.reset();
}

OpticsSystem::VisionPipelineRuntime& OpticsSystem::get_or_create_runtime(
    const VisionPipelineKey& key) {
    auto [it, inserted] = vision_runtimes_.try_emplace(key);
    if (inserted || !it->second) {
        it->second = std::make_unique<VisionPipelineRuntime>();
        it->second->scene_resource = key.participating_media
            ? std::make_shared<VisionSceneResource>() : get_or_create_vision_scene_resource();
        it->second->mode = key.mode;
        it->second->participating_media = key.participating_media;
        CFW_LOG_INFO("OpticsSystem: created Vision runtime ({})",
                     describe_vision_pipeline_key(key));
    } else if (!it->second->scene_resource) {
        it->second->scene_resource = key.participating_media
            ? std::make_shared<VisionSceneResource>() : get_or_create_vision_scene_resource();
    }
    it->second->interop_executor = hardware_ ? &hardware_->executor : nullptr;
    return *it->second;
}



void OpticsSystem::evict_idle_vision_runtimes(uint64_t frame_index) {
    for (auto it = vision_runtimes_.begin(); it != vision_runtimes_.end();) {
        if (active_vision_runtime_key_ && *active_vision_runtime_key_ == it->first) {
            ++it;
            continue;
        }
        auto& runtime = it->second;
        if (!runtime ||
            frame_index <= runtime->last_used_frame + kVisionRuntimeIdleEvictFrames) {
            ++it;
            continue;
        }
        // An algorithm can be inactive while its viewport remains open. Keep
        // the current scene's compiled variants so switching back after a few
        // seconds does not turn into another scene import and shader compile.
        // Closed views and previous scenes still follow normal idle eviction.
        const bool has_open_view = std::any_of(
            runtime->view_denoise_states.begin(), runtime->view_denoise_states.end(),
            [](const auto& view) {
                auto camera = Corona::find_camera(view.first);
                return camera && camera->surface != nullptr;
            });
        if (has_open_view) {
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
        if (it->first == key || it->first.participating_media) {
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
        active_vision_runtime_key_ = make_vision_pipeline_key(current_vision_render_mode_);
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
    vision_scene_resource_.reset();
    active_vision_runtime_key_.reset();
    vision_initialized_ = false;
}

void OpticsSystem::shutdown_vision_backend() {
    // Join stream workers and release shared imports, pipelines and scene GPU
    // resources before invoking the backend's Device deleter. The executable
    // ultimately calls TerminateProcess, so a function-static Device would
    // never run its cleanup callbacks or release its Vulkan context.
    clear_vision_runtimes();
    if (vision_device_) {
        CFW_LOG_INFO("OpticsSystem: destroying Vision compute device");
        auto& global = vision::Global::instance();
        global.set_pipeline(nullptr);
        vision::ImagePool::destroy_instance();
        // Material LUTs are process-global and outlive their scene bindless arrays.
        // Release their texture owners while the compute device is still valid.
        vision::MaterialLut::destroy_instance();
        // All pipeline and scene observers are gone. Retire hotfix versions
        // while their deferred GPU resources can still use the compute device.
        Corona::Hotfix::shutdown();
        vision_device_.reset();
        global.set_device(nullptr);
        CFW_LOG_INFO("OpticsSystem: Vision compute device destroyed");
    }
    vision_initialized_ = false;
}

OpticsSystem::OpticsSystem() {
    // Useful for bounded validation and machines with limited cooling.
    const auto explicit_fps = env_u64("CORONA_OPTICS_MAX_FPS");
    const auto fps = explicit_fps.value_or(60);
    set_target_fps(static_cast<int>(std::clamp<std::uint64_t>(fps, 1, 240)));
    if (explicit_fps) {
        explicit_render_interval_ = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / std::clamp<std::uint64_t>(fps, 1, 240)));
    }
}

OpticsSystem::~OpticsSystem() = default;

bool OpticsSystem::initialize_hardware_resources() {
    try {
        hardware_ = std::make_unique<Hardware>();

    } catch (const std::exception&) {
        CFW_LOG_CRITICAL("OpticsSystem: Failed to initialize hardware resources");
        return false;
    }

    return true;
}

bool OpticsSystem::initialize_render_pipelines() {
    try {
        // 新版 Horizon: RasterizerPipeline 构造需要 shader 类型（非实例）+ 描述符
        hardware_->uiVisibilityPipeline.emplace(
            visibility_vert_glsl_t{},
            visibility_frag_glsl_t{},
            make_ui_visibility_pipeline_desc());
        hardware_->screenshotReadbackPipeline.emplace(image_readback_comp_glsl, ktm::uvec3(8, 8, 1));
        hardware_->opticsOverlayPipeline.emplace(optics_overlay_comp_glsl, ktm::uvec3(8, 8, 1));

        hardware_->opticsUiWarpPipeline.emplace(optics_ui_warp_comp_glsl, ktm::uvec3(8, 8, 1));
        hardware_->opticsCompositePipeline.emplace(optics_composite_comp_glsl, ktm::uvec3(8, 8, 1));
        hardware_->visionDisplayExtent.as_push_constant();
        const auto write_display_image = [&] {
            using namespace EmbeddedShader;
            Uint2 pixel;
            pixel = dispatchThreadID()->xy();
            auto& extent = hardware_->visionDisplayExtent;
            $IF(pixel->x < extent->x && pixel->y < extent->y) {
                const Uint index = pixel->y * extent->x + pixel->x;
                hardware_->visionDisplayImage[pixel] = hardware_->visionDisplayPixels[index];
            }
        };
        const ktm::uvec3 threads(8, 8, 1);
        EmbeddedShader::CompilerOption compiler;
        compiler.compileHLSL = false;
        Horizon::ComputePipelineShaders shaders;
        shaders.object = std::make_shared<EmbeddedShader::ComputePipelineObject>(
            EmbeddedShader::ComputePipelineObject::compile(write_display_image, threads, compiler));
        shaders.compute = shaders.object->compute->getShaderCode(EmbeddedShader::ShaderLanguage::SpirV, true);
        shaders.auto_bind_entries = shaders.object->autoBindEntries;
        // Bindless resources use distinct push-constant offsets. Keep those
        // same identities in the command recorder for the buffer and image.
        for (auto& entry : shaders.auto_bind_entries) {
            if (entry.boundResourceRef) entry.location = static_cast<uint32_t>(entry.byteOffset);
        }
        hardware_->visionDisplayPipeline.emplace(Horizon::ComputePipelineDesc{threads}, std::move(shaders));
        hardware_->shaderHasInit = true;
    } catch (const std::exception& e) {
        CFW_LOG_CRITICAL("OpticsSystem: Failed to initialize typed pipelines: {}", e.what());
        return false;
    }

    return true;
}

void OpticsSystem::ensure_ui_view_resources(EntityId camera_handle,
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
        if (hardware_->last_receipt.serial != 0) {
            hardware_->executor.wait_idle(hardware_->last_receipt);
        }
        resources.ui_visibility =
            make_storage_image(width, height, Horizon::Format::RG32_UINT, "optics.ui_visibility");
        resources.ui_depth = make_depth_image(width, height, "optics.ui_depth");
        resources.width = width;
        resources.height = height;
    }
    resources.last_used_frame = frame_index;
    hardware_->uiVisibilityImage = resources.ui_visibility;
    hardware_->uiDepthImage = resources.ui_depth;
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

    // Allocate one ImageStorage slot when this surface first needs an output.
    if (target.image_handle == 0) {
        target.image_handle = Corona::SharedDataHub::instance().create_image();
        target.published_image = Detail::PublishedImage(target.image_handle);
        // Clear any image and GPU receipts retained from a previous allocation.
        if (auto accessor =
                Corona::SharedDataHub::instance().image_write(target.image_handle)) {
            *accessor = ImageDevice{};
        }
    }

    // Create or rebuild this surface's output images when its resolution changes.
    if (!target.final_output ||
        target.width != width || target.height != height) {
        if (target.image_handle != 0) {
            if (auto image_device =
                    Corona::SharedDataHub::instance().image_write(target.image_handle)) {
                if (image_device->consumed_receipt.serial != 0) {
                    hardware_->executor.wait_idle(image_device->consumed_receipt);
                }
            }
        }
        if (hardware_->last_receipt.serial != 0) {
            hardware_->executor.wait_idle(hardware_->last_receipt);
        }
        target.final_output =
            make_storage_image(width, height, Horizon::Format::RGBA16_FLOAT, "optics.surface_final");
        target.ui_overlay = {};
        target.ui_warped_overlay = {};
        target.width = width;
        target.height = height;
    }

    target.last_used_frame = frame_index;
    return target;
}

OpticsSystem::SurfaceRenderTarget& OpticsSystem::acquire_offscreen_screenshot_target(
    EntityId camera_handle,
    uint32_t width,
    uint32_t height,
    uint64_t frame_index) {
    width = std::max(width, 1u);
    height = std::max(height, 1u);

    auto& target = offscreen_screenshot_targets_[camera_handle];
    if (!target.final_output ||
        target.width != width || target.height != height) {
        if (hardware_->last_receipt.serial != 0) {
            hardware_->executor.wait_idle(hardware_->last_receipt);
        }
        target.final_output =
            make_storage_image(width, height, Horizon::Format::RGBA16_FLOAT, "optics.offscreen_final");
        target.ui_overlay = {};
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
                release_surface_target(it->second);
            }
            it = surface_targets_.erase(it);
        } else {
            ++it;
        }
    }
}

void OpticsSystem::release_surface_target(SurfaceRenderTarget& target) {
    if (target.image_handle == 0) {
        return;
    }
    // Invalidate cached Display layers and snapshots before waiting. An entered
    // frame keeps its image access until its final consumed receipt is written.
    target.published_image.retire().wait();
    auto& storage = Corona::SharedDataHub::instance();
    {
        auto image = storage.image_write(target.image_handle);
        if (image->submit_receipt.serial != 0) {
            hardware_->executor.wait_idle(image->submit_receipt);
        }
        if (image->consumed_receipt.serial != 0) {
            hardware_->executor.wait_idle(image->consumed_receipt);
        }
        *image = ImageDevice{};
    }
    storage.destroy_image(target.image_handle);
    target.image_handle = 0;
    target.published_image = {};
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
                    !Corona::find_camera(event.camera_handle)) {
                    if (event.completion_promise) {
                        event.completion_promise->set_value(false);
                    }
                    return;
                }
                const auto expires_at =
                    event.deadline == std::chrono::steady_clock::time_point{}
                        ? std::chrono::steady_clock::now() + kScreenshotRequestTimeout : event.deadline;
                std::lock_guard<std::mutex> lock(screenshot_mutex_);
                const auto conflicting_capture = [&](const PendingScreenshot& request) {
                    return event.render_frames != 0 && request.render_frames != 0 &&
                           request.camera_handle == event.camera_handle;
                };
                if (std::any_of(incoming_screenshots_.begin(), incoming_screenshots_.end(), conflicting_capture) ||
                    std::any_of(pending_screenshots_.begin(), pending_screenshots_.end(), conflicting_capture)) {
                    if (event.completion_promise) event.completion_promise->set_value(false);
                    return;
                }
                incoming_screenshots_.push_back(
                    {event.camera_handle,
                     event.file_path,
                     event.completion_promise,
                     expires_at, event.render_frames, event.warmup_frames, event.timings_path});
            });
    }

    return true;
}

void apply_pending_optics_updates() {
    auto& channel = Corona::OpticsCommandChannel::instance();
    auto commands = channel.commands().drain();
    if (commands.empty()) {
        return;
    }

    for (const auto& command : commands) {
        auto* device = Corona::find_optics(command.optics_handle);
        if (!device) {
            continue;
        }
        auto* visibility = Corona::find_visibility(command.optics_handle);
        if (!visibility) {
            continue;
        }
        Corona::apply_optics_update(*device, *visibility, command);
    }
}

void OpticsSystem::apply_camera_and_transform_updates() {
    // Explicit viewport/state commands take precedence over default surface size.
    apply_pending_default_surface_camera_update();
    apply_pending_camera_viewport_updates();
    const auto state_updates = apply_pending_camera_state_updates();
    apply_pending_camera_moves();
    apply_pending_transform_updates();
    apply_pending_camera_releases();
    SharedDataHub::instance().publish_camera_snapshot();
    // 变换快照：外部读者（get_position/get_matrix/面板）只读这一份投影，
    // 不再对 slot 加锁。必须在 transform 命令应用之后重建，才是"本 tick 的最终值"。
    SharedDataHub::instance().publish_transform_snapshot();
    acknowledge_camera_state_updates(state_updates);
}

void OpticsSystem::update() {
    // [P3/P5 合并保留] DAG 降级路径没有 tick 边界阶段：这里保证 Environment
    // 与 Optics 标量命令、相机姿态命令仍会被应用（DAG 模式下边界阶段已 drain，
    // 这些调用是幂等的空操作）。
    Corona::apply_pending_environment_updates();
    apply_pending_optics_updates();
    // Visibility/material getters must retain consumed values even when the
    // render cap below skips GPU work for this tick.
    Corona::publish_optics_snapshot();

    // Event-driven wakes may bypass SystemBase's periodic wait. An explicitly
    // requested render cap must still bound GPU submissions during such bursts.
    const auto now = std::chrono::steady_clock::now();
    if (explicit_render_interval_ != std::chrono::steady_clock::duration{}) {
        if (now < next_render_update_) return;
        next_render_update_ = now + explicit_render_interval_;
    }
    // This thread owns the GPU submit path; when it blocks everything that waits
    // on it (Scene/geometry holders included) blocks too, silently.
    Corona::Diagnostics::PhaseScope update_phase{"Optics", "update"};
    update_phase.set("camera_commands");
    // Setters enqueue camera changes before publishing their screenshot request.
    // Promote requests first, then drain those changes. Requests arriving later
    // must wait for the next frame instead of capturing this frame's old state.
    begin_screenshot_frame();
    apply_camera_and_transform_updates();

    // �ӳٻ�ȡ GeometrySystem ָ�루������ initialize() �л�ȡ����������
    // initialize_all() ��������ϵͳ��get_system() Ҳ��ͬ�� �� �ǵݹ� mutex �������
    if (!geometry_system_ && !geometry_system_queried_) {
        geometry_system_queried_ = true;
        auto& kernel = Kernel::KernelContext::instance();
        if (auto* sys_mgr = kernel.system_manager()) {
            auto sys = sys_mgr->get_system("Geometry");
            geometry_system_ = dynamic_cast<GeometrySystem*>(sys ? sys.get() : nullptr);
        }
    }

    if (!hardware_->shaderHasInit ||
        !hardware_->opticsOverlayPipeline ||
        !hardware_->opticsUiWarpPipeline ||
        !hardware_->opticsCompositePipeline) {
        return;
    }

    static uint64_t frame_index = 0;

    ++frame_index;

    update_phase.set("render_dispatch");
    optics_pipeline(frame_index);
}

OpticsSystem::RenderFramePlan OpticsSystem::build_render_frame_plan() {
    RenderFramePlan plan;
    std::unordered_set<std::uintptr_t> scheduled;
    auto scenes = Corona::all_scenes();
    for (auto scene_it = scenes.begin(); scene_it != scenes.end(); ++scene_it) {
        const auto& scene = *scene_it;
        if (!scene.enabled) continue;
        // 场景句柄直接来自实体句柄（entt 里引用地址不是实体 ID）。
        const auto scene_handle = scene_it.id();
        for (const auto handle : scene.camera_handles) {
            auto camera = Corona::find_camera(handle);
            if (!camera) continue;
            plan.has_cameras = true;
            if (!camera->surface && !has_pending_screenshot(handle)) continue;
            if (!scheduled.insert(handle).second) continue;
            plan.views.push_back({scene_handle, handle, camera->vision_render_mode, camera_uses_vision_media(*camera)});
        }
    }
    return plan;
}

std::optional<OpticsSystem::LockedRenderView> OpticsSystem::lock_render_view(
    const RenderView& view) {
    // Scene structure stays locked, while camera values are copied under a
    // short read lock. UI presentation and input must not wait for a GPU frame.
    auto scene = Corona::find_scene(view.scene_handle);
    if (!scene || !scene->enabled ||
        std::find(scene->camera_handles.begin(), scene->camera_handles.end(), view.camera_handle) ==
            scene->camera_handles.end()) return std::nullopt;
    auto camera = Corona::find_camera(view.camera_handle);
    if (!camera ||
        (!camera->surface && !has_pending_screenshot(view.camera_handle))) return std::nullopt;
    return LockedRenderView{scene, *camera};
}

void OpticsSystem::optics_pipeline(uint64_t frame_index) {
    fail_expired_pending_screenshots();
    fail_unrenderable_pending_screenshots();

    // Schedule each handle only once. Scene/assets stay in storage under locks;
    // only small camera/environment values are copied for a coherent frame.
    const auto plan = build_render_frame_plan();
    if (env_flag_enabled("CORONA_OPTICS_MEASURE_RENDER_TIME")) {
        CFW_LOG_INFO("OpticsFrameRoutes frame={} vision_views={}", frame_index, plan.views.size());
    }
    if (!plan.views.empty()) {
        if (!vision_initialized_ && std::chrono::steady_clock::now() >= next_vision_init_attempt_ &&
            !init_vision_lazy()) {
            next_vision_init_attempt_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
        if (vision_initialized_) {
            run_vision_frame(frame_index, plan.views);
        } else {
            for (const auto& view : plan.views) fail_pending_screenshots(view.camera_handle);
        }
    } else if (vision_initialized_) {
        // Keep on-demand capture resources until the next requested capture.
        if (!plan.has_cameras && active_vision_runtime_key_) {
            clear_vision_runtimes();
        } else {
            evict_idle_vision_runtimes(frame_index);
        }
    }
    evict_idle_surface_targets(frame_index);
    evict_idle_offscreen_screenshot_targets(frame_index);
    evict_idle_ui_view_resources(frame_index);
}

Horizon::HardwareImage* OpticsSystem::compose_surface_ui_overlay(
    Horizon::HardwareStream& stream,
    EntityId camera_handle,
    const CameraDevice& camera,
    const SceneDevice& scene,
    SurfaceRenderTarget& target,
    Horizon::HardwareImage& background,
    const ViewportUiState& ui_state,
    uint64_t frame_index) {
    auto& uiVisibility = *hardware_->uiVisibilityPipeline;
    auto& opticsOverlay = *hardware_->opticsOverlayPipeline;
    auto& opticsUiWarp = *hardware_->opticsUiWarpPipeline;
    auto& opticsComposite = *hardware_->opticsCompositePipeline;

    // follow-camera UI 使用正交投影并且跟随特定 actor 的世界空间位置而源自相机。
    const ktm::fmat4x4 camera_basis = make_camera_basis_matrix(camera);
    constexpr float kFollowCameraOrthoHeight = 2.0f;
    constexpr float kFollowCameraNear = -1000.0f;
    constexpr float kFollowCameraFar = 1000.0f;
    const float ortho_width = kFollowCameraOrthoHeight * camera.aspect;
    const ktm::fmat4x4 ortho_proj =
        make_orthographic_lh(ortho_width, kFollowCameraOrthoHeight,
                             kFollowCameraNear, kFollowCameraFar);

    Hardware::VPUniformBufferObject ui_vp;
    ui_vp.viewProjMatrix=multiply_ktm_mat4(ortho_proj,camera.compute_view_matrix());
    // UI overlay pass ͬ��ÿ�����д���� buffer���볡�� pass ͬһ��������Ϊ�����á�
    // keep_alive ���ڴ���� stream �ϣ��뱾 overlay �� dispatch ͬһ submission����
    auto uiVpLease = hardware_->uiVpUniformBufferPool.acquire(
        [] { return make_storage_buffer<Hardware::VPUniformBufferObject>(1, "optics.ui_vp_uniform.pool"); });
    Horizon::HardwareBuffer& uiVpBuffer = *uiVpLease.buffer;
    (void)write_object_bytes(uiVpBuffer,ui_vp);
    stream << RetainSubmission{uiVpLease.busy};


    RenderInstanceBatch uiBatch;
    std::vector<Corona::Horizon::HardwareBuffer> ui_indirect_buffers;
    const bool has_follow_camera_instances =
        collect_ui_instances(scene, uiVisibility, camera_basis, uiBatch, geometry_system_,
                             frame_index, ui_indirect_buffers);
    stream << RetainSubmission{uiBatch.resource_keep_alive};

    const auto& calibration = ui_state.calibration;
    const bool stereo_ui = ui_state.mode == ViewportUiMode::Stereo3D;

    const auto ui_instance_count = static_cast<std::uint32_t>(uiBatch.instances.size());
    auto& ui_log_state = ui_pass_log_states_[camera_handle];
    if (!ui_log_state.has_state ||
        ui_log_state.has_follow_camera_instances != has_follow_camera_instances ||
        ui_log_state.stereo_ui != stereo_ui ||
        ui_log_state.instance_count != ui_instance_count ||
        ui_log_state.width != target.width ||
        ui_log_state.height != target.height) {
        ui_log_state = UiPassLogState{
            .has_state = true,
            .has_follow_camera_instances = has_follow_camera_instances,
            .stereo_ui = stereo_ui,
            .instance_count = ui_instance_count,
            .width = target.width,
            .height = target.height,
        };
        CFW_LOG_INFO("Optics UI pass: camera={} mode={} follow_camera_instances={} output={}x{} warp={}",
                     camera_handle,
                     stereo_ui ? "stereo3d" : "flat2d",
                     ui_instance_count,
                     target.width,
                     target.height,
                     (stereo_ui && has_follow_camera_instances)
                         ? "submitted"
                         : "skipped");
    }

    if (!has_follow_camera_instances) {
        return &background;
    }

    ensure_ui_view_resources(camera_handle, target.width, target.height, frame_index);
    uiVisibility.visibilityData = hardware_->uiVisibilityImage;
    uiVisibility.bind_depth_target(hardware_->uiDepthImage);
    if (!target.ui_overlay) {
        target.ui_overlay = make_storage_image(target.width, target.height, Horizon::Format::RGBA16_FLOAT, "optics.surface_overlay");
    }
    if (stereo_ui && !target.ui_warped_overlay) {
        target.ui_warped_overlay = make_storage_image(target.width, target.height, Horizon::Format::RGBA16_FLOAT, "optics.surface_warped_overlay");
    }
    const auto ui_instance_capacity = grow_table_capacity(
        kInitialInstanceTableCapacity,
        static_cast<std::uint64_t>(uiBatch.instances.size()));
    const auto ui_material_capacity = grow_table_capacity(
        kInitialMaterialTableCapacity,
        static_cast<std::uint64_t>(uiBatch.materials.size()));
    auto uiInstLease = hardware_->uiInstanceInfoBufferPool.acquire(
        ui_instance_capacity,
        [ui_instance_capacity] {
            return make_storage_buffer<Hardware::InstanceInfo>(
                ui_instance_capacity, "optics.ui_instances.pool");
        });
    auto uiMatLease = hardware_->uiMaterialTableBufferPool.acquire(
        ui_material_capacity,
        [ui_material_capacity] {
            return make_storage_buffer<Hardware::MaterialInfo>(
                ui_material_capacity, "optics.ui_materials.pool");
        });
    Horizon::HardwareBuffer& uiInstanceBuffer = *uiInstLease.buffer;
    Horizon::HardwareBuffer& uiMaterialBuffer = *uiMatLease.buffer;
    auto uiInstanceCapacity = uiInstLease.capacity;
    auto uiMaterialCapacity = uiMatLease.capacity;
    const bool uploaded =
        upload_instance_tables(uiBatch,
                               *hardware_,
                               uiInstanceBuffer,
                               uiInstanceCapacity,
                               uiMaterialBuffer,
                               uiMaterialCapacity,
                               "optics.ui.pool");
    stream << RetainSubmission{uiInstLease.busy} << RetainSubmission{uiMatLease.busy};
    if (!uploaded) {
        uiVisibility.clear_records();
        return &background;
    }
    opticsOverlay.pushConsts.gbufferSize = upload_value(ktm::uvec2{target.width, target.height});
    bind_storage_image(opticsOverlay, opticsOverlay.pushConsts.visibilityImageIndex, hardware_->uiVisibilityImage);
    bind_storage_buffer(opticsOverlay, opticsOverlay.pushConsts.instanceInfoBufferIndex, uiInstanceBuffer);
    bind_storage_buffer(opticsOverlay, opticsOverlay.pushConsts.materialTableBufferIndex, uiMaterialBuffer);
    opticsOverlay.pushConsts.instanceCount =
        static_cast<std::uint32_t>(uiBatch.instances.size());
    opticsOverlay.pushConsts.materialCount =
        static_cast<std::uint32_t>(uiBatch.materials.size());
    bind_storage_buffer(opticsOverlay, opticsOverlay.pushConsts.vpBufferIndex, uiVpBuffer);
    bind_storage_image(opticsOverlay, opticsOverlay.pushConsts.outputImage, target.ui_overlay);

    if (stereo_ui) {
        bind_storage_image(opticsUiWarp, opticsUiWarp.pushConsts.inputImage, target.ui_overlay);
        bind_storage_image(opticsUiWarp, opticsUiWarp.pushConsts.outputImage, target.ui_warped_overlay);
        opticsUiWarp.pushConsts.outputWidth = target.width;
        opticsUiWarp.pushConsts.outputHeight = target.height;
        opticsUiWarp.pushConsts.lenticularPitch = calibration.lenticular_pitch;
        opticsUiWarp.pushConsts.slant = std::tan(calibration.slant_angle_radians);
        opticsUiWarp.pushConsts.phaseOffset = calibration.phase_offset;
        opticsUiWarp.pushConsts.parallaxScale = calibration.parallax_scale;
        opticsUiWarp.pushConsts.rgbSubpixelOffsets = upload_value(ktm::fvec4(
            calibration.rgb_subpixel_offsets[0],
            calibration.rgb_subpixel_offsets[1],
            calibration.rgb_subpixel_offsets[2],
            0.0f));
    }

    bind_storage_image(opticsComposite, opticsComposite.pushConsts.bgImage, background);
    bind_storage_image(opticsComposite, opticsComposite.pushConsts.fgImage, stereo_ui ? target.ui_warped_overlay : target.ui_overlay);
    bind_storage_image(opticsComposite, opticsComposite.pushConsts.outputImage, background);
    opticsComposite.pushConsts.outputWidth = target.width;
    opticsComposite.pushConsts.outputHeight = target.height;


    stream << uiVisibility.extent(target.width, target.height)
           << opticsOverlay.dispatch_extent(target.width, target.height);
    if (stereo_ui) {
        stream << opticsUiWarp.dispatch_extent(target.width, target.height);
    }
    stream << opticsComposite.dispatch_extent(target.width, target.height);
    // ע�⣺�˴��� commit���ɵ��÷��ں���ʱ��ͳһ�ύ��

    return &background;
}

void OpticsSystem::begin_screenshot_frame() {
    std::lock_guard<std::mutex> lock(screenshot_mutex_);
    pending_screenshots_.insert(pending_screenshots_.end(),
                               std::make_move_iterator(incoming_screenshots_.begin()),
                               std::make_move_iterator(incoming_screenshots_.end()));
    incoming_screenshots_.clear();
}

bool OpticsSystem::has_pending_screenshot(EntityId camera_handle) {
    std::lock_guard<std::mutex> lock(screenshot_mutex_);
    return std::any_of(pending_screenshots_.begin(), pending_screenshots_.end(),
                       [camera_handle](const PendingScreenshot& req) {
                           return req.camera_handle == camera_handle;
                       });
}

void OpticsSystem::fail_pending_screenshots(EntityId camera_handle) {
    std::vector<PendingScreenshot> matched;
    {
        std::lock_guard<std::mutex> lock(screenshot_mutex_);
        for (auto* queue : {&pending_screenshots_, &incoming_screenshots_}) {
            auto it = std::remove_if(queue->begin(), queue->end(),
                                    [camera_handle, &matched](PendingScreenshot& req) {
                                        if (camera_handle != 0 && req.camera_handle != camera_handle) {
                                            return false;
                                        }
                                        matched.push_back(std::move(req));
                                        return true;
                                    });
            queue->erase(it, queue->end());
        }
    }

    for (auto& req : matched) {
        if (req.completion_promise) {
            req.completion_promise->set_value(false);
        }
    }
}

void OpticsSystem::fail_expired_pending_screenshots() {
    std::vector<PendingScreenshot> expired;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(screenshot_mutex_);
        for (auto* queue : {&pending_screenshots_, &incoming_screenshots_}) {
            auto it = std::remove_if(queue->begin(), queue->end(),
                                    [now, &expired](PendingScreenshot& req) {
                                        if (req.expires_at == std::chrono::steady_clock::time_point{} ||
                                            req.expires_at > now) {
                                            return false;
                                        }
                                        expired.push_back(std::move(req));
                                        return true;
                                    });
            queue->erase(it, queue->end());
        }
    }

    for (auto& req : expired) {
        CFW_LOG_WARNING("OpticsSystem: dropping expired screenshot request for camera {}: {}",
                        req.camera_handle, req.file_path);
        if (req.completion_promise) {
            req.completion_promise->set_value(false);
        }
    }
}

void OpticsSystem::fail_unrenderable_pending_screenshots() {
    std::vector<std::uintptr_t> pending_handles;
    {
        std::lock_guard<std::mutex> lock(screenshot_mutex_);
        pending_handles.reserve(pending_screenshots_.size() + incoming_screenshots_.size());
        for (const auto* queue : {&pending_screenshots_, &incoming_screenshots_}) {
            for (const auto& req : *queue) {
                if (std::find(pending_handles.begin(), pending_handles.end(), req.camera_handle) ==
                    pending_handles.end()) {
                    pending_handles.push_back(req.camera_handle);
                }
            }
        }
    }

    for (const auto camera_handle : pending_handles) {
        if (!Corona::find_camera(camera_handle)) {
            fail_pending_screenshots(camera_handle);
            continue;
        }

        bool in_enabled_scene = false;
        bool in_any_scene = false;
        for (const auto& scene : Corona::all_scenes()) {
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

void OpticsSystem::process_pending_screenshots(EntityId camera_handle,
                                               Horizon::HardwareImage& render_target,
                                               uint32_t w, uint32_t h) {
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

    if (w == 0 || h == 0) {
        CFW_LOG_WARNING("OpticsSystem: Cannot take screenshot - zero render dimensions");
        for (auto& req : matched) {
            if (req.completion_promise) req.completion_promise->set_value(false);
        }
        return;
    }

    std::vector<std::uint32_t> pixels;
    bool read_ok = false;
    try {
        const uint64_t pixel_count = static_cast<uint64_t>(w) * h;
        // Capture-only staging: four bytes per pixel, never a persistent image.
        auto staging = make_storage_buffer<std::uint32_t>(
            pixel_count, "optics.screenshot_staging", Horizon::CpuAccessMode::Read);
        // Reuse the pipeline: destroying a temporary Horizon pipeline invokes
        // vkDeviceWaitIdle while the display thread may be submitting work.
        auto& readback = *hardware_->screenshotReadbackPipeline;
        bind_storage_image(readback, readback.pc.inputImage, render_target);
        bind_storage_buffer(readback, readback.pc.outputBuffer, staging);
        readback.pc.size = upload_value(ktm::uvec2{w,h});
        hardware_->executor.wait(hardware_->last_receipt);
        auto receipt = hardware_->executor << readback.dispatch_extent(w,h) << Horizon::commit();
        hardware_->executor.wait_completed(receipt);
        pixels.resize(static_cast<std::size_t>(pixel_count));
        read_ok = staging.read_bytes(std::as_writable_bytes(std::span(pixels)));
    } catch (const std::exception& error) {
        CFW_LOG_ERROR("OpticsSystem: Screenshot readback failed: {}", error.what());
    }
    for (auto& req : matched) {
        bool saved = false;
        if (read_ok && std::chrono::steady_clock::now() < req.expires_at) {
            try {
                auto path = std::filesystem::u8path(req.file_path);
                Resource::Image screenshot(path);
                screenshot.set_data(reinterpret_cast<unsigned char*>(pixels.data()), w, h, 4);
                Resource::ImageParser parser;
                saved = parser.export_to(screenshot, path);
            } catch (const std::exception& error) {
                CFW_LOG_ERROR("OpticsSystem: Screenshot export failed: {}", error.what());
            }
        }
        if (req.completion_promise) req.completion_promise->set_value(saved);
    }
}

void OpticsSystem::shutdown() {
    if (auto* event_bus = context()->event_bus()) {
        if (screenshot_request_sub_id_ != 0) {
            event_bus->unsubscribe(screenshot_request_sub_id_);
        }

    }

    fail_pending_screenshots(0);

    // Retire per-surface publications before releasing their storage and GPU images.
    for (auto& [surface, target] : surface_targets_) {
        release_surface_target(target);
    }
    surface_targets_.clear();
    offscreen_screenshot_targets_.clear();
    ui_view_resources_.clear();
    shutdown_vision_backend();
    hardware_.reset();

}

std::size_t OpticsSystem::compute_vision_scene_signature(const SceneDevice* locked_scene) const {
    // Lightweight per-frame change detector. Traverses the same hierarchy as
    // build_vision_geometry (enabled scene �� actor �� profile �� optics �� geometry)
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
    auto geom_storage = Corona::Ecs::all_components<Corona::GeometryDevice>();

    const auto mix_scene = [&](const SceneDevice& scene_dev) {
        if (!scene_dev.enabled) return;
        mix(scene_dev.authored_media_revision);
        mix(reinterpret_cast<std::size_t>(scene_dev.authored_media.get()));
        for (auto actor_handle : scene_dev.actor_handles) {
            auto actor = Corona::find_actor(actor_handle);
            const auto* actor_profiles = Corona::find_profiles(actor_handle);
            if (!actor) continue;
            mix(static_cast<std::size_t>(actor_handle));
            for (auto profile_handle : actor_profiles->handles) {
                auto profile = Corona::find_profile(profile_handle);
                if (!profile || profile->optics_handle == 0) continue;

                auto optics = Corona::find_optics(profile->optics_handle);
                if (!optics) continue;

                // visible toggle changes topology of the Vision scene.
                const auto* optics_visibility =
                    Corona::find_visibility(profile->optics_handle);
                const bool optics_visible =
                    optics_visibility != nullptr && optics_visibility->visible;
                mix(optics_visible ? 0x1u : 0x2u);
                if (!optics_visible) continue;

                // Material parameters bridged into the Vision principled BSDF.
                mix(optics->principled ? optics->principled->revision : 0u);
                mix(reinterpret_cast<std::size_t>(optics->material_overrides.get()));
                mix(reinterpret_cast<std::size_t>(optics->medium_interface.get()));
                mix_float(optics->metallic);
                mix_float(optics->roughness);
                mix_float(optics->subsurface);
                mix_float(optics->anisotropic);
                mix_float(optics->sheen);
                mix_float(optics->sheenTint);
                mix_float(optics->clearcoat);
                mix_float(optics->clearcoatGloss);

                auto geom = Corona::find_geometry(optics->geometry_handle);
                if (!geom) continue;
                mix(static_cast<std::size_t>(optics->geometry_handle));
                mix(static_cast<std::size_t>(geom->model_resource_handle));
                if (auto model = Corona::find_model_resource(geom->model_resource_handle)) {
                    mix(static_cast<std::size_t>(model->model_id));
                }
                mix(static_cast<std::size_t>(geom->gpu_build_state));
                mix(geom->mesh_handles.size());
                mix(geom->model_node_revision);
                mix(geom->model_node_overrides.size());
                for (const auto& override : geom->model_node_overrides) {
                    mix(override.node_index);
                    mix(std::hash<std::string>{}(override.node_path));
                    mix(override.visible ? (*override.visible ? 2u : 1u) : 0u);
                    for (const auto* property : {&override.position, &override.rotation, &override.scale}) {
                        mix(property->has_value());
                        if (*property) for (float value : **property) mix_float(value);
                    }
                }
                for (const auto& instance : geom->mesh_instances) {
                    mix(instance.mesh_index);
                    mix(instance.visible);
                    for (int column = 0; column < 4; ++column)
                        for (int row = 0; row < 4; ++row) mix_float(instance.transform[column][row]);
                }

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

                // The per-frame instance upload handles matrix changes. A different
                // source entity still requires rebuilding its logical binding.
                mix(static_cast<std::size_t>(geom->transform_handle));
            }
        }
    };
    if (locked_scene) {
        mix_scene(*locked_scene);
    } else {
        for (const auto& scene : Corona::all_scenes()) {
            mix_scene(scene);
        }
    }
    return sig;
}

Vision::VisionBuildResult OpticsSystem::rebuild_vision_scene(VisionPipelineRuntime& runtime) {
    Vision::VisionBuildResult result;
    auto& pipeline = runtime.pipeline;
    if (!pipeline) return result;
    try {
        pipeline->activate_global_context();
        runtime.wait_for_geometry_users();
        pipeline->activate_view_context(0u);
        auto& scene = pipeline->scene();
        result = Vision::build_vision_geometry(scene);
        if (!result.complete()) return result;
        if (runtime.scene_resource) {
            sync_logical_instances_from_pipeline_scene(*runtime.scene_resource, scene);
        }

        // Rebind the engine's infinite lighting after a complete topology
        // replacement; light buffers/sampler state must be prepared together.
        const auto env = engine_vision_environment();
        Vision::setup_vision_lights(*pipeline, env);
        runtime.engine_lighting = Vision::make_vision_lighting_state(env);

        // Keep framebuffer/sensor allocations intact during topology changes.
        // Forcing geometry_changed refreshes every view's light tables, uploads
        // both bindless arrays, then recompiles all consumers of the final data.
        scene.prepare();
        if (runtime.scene_resource) {
            runtime.scene_resource->mark_transforms_changed();
        }
        pipeline->prepare_geometry(true);
        runtime.bind_dynamic_geometry(result);
        if (runtime.scene_resource) {
            runtime.scene_resource->mark_scene_gpu_transforms_uploaded();
            runtime.scene_gpu_transform_version =
                runtime.scene_resource->logical_transform_version;
        }
    } catch (const std::exception& e) {
        runtime.engine_geometry_available = false;
        ++result.invalid_data;
        CFW_LOG_ERROR("OpticsSystem: Vision scene rebuild failed: {}", e.what());
    }
    return result;
}

void OpticsSystem::sync_vision_dynamic_scene(VisionPipelineRuntime& runtime) {
    sync_vision_scene_topology(runtime);
    if (!runtime.skinning || !runtime.engine_geometry_available) return;
    try {
        runtime.transforms_pending |= Vision::apply_vision_instance_transforms(runtime.pipeline->scene(),
                                                                               runtime.instance_transforms);
        runtime.skinning->update(*runtime.pipeline, runtime.transforms_pending);
        runtime.transforms_pending = false;
        runtime.skinning_ready = true;
    } catch (const std::exception& e) {
        if (runtime.skinning_ready) CFW_LOG_ERROR("Vision skinned geometry update failed: {}", e.what());
        runtime.skinning_ready = false;
    }
}

void OpticsSystem::sync_vision_scene_topology(VisionPipelineRuntime& runtime) {
    if (!vision_initialized_) return;
    const std::size_t sig = compute_vision_scene_signature();
    const bool revision_changed = sig != runtime.pending_signature;
    if (revision_changed) {
        runtime.pending_signature = sig;
        runtime.rebuild_retries = 0;
        runtime.engine_geometry_ready = false;
        runtime.next_geometry_retry = {};
    }
    if (runtime.engine_geometry_ready && sig == runtime.applied_signature) {
        runtime.geometry_debounce.reset();
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < runtime.next_geometry_retry || !runtime.geometry_debounce.poll(revision_changed, now)) return;
    runtime.geometry_debounce.reset();
    const auto result = rebuild_vision_scene(runtime);
    if (result.complete()) {
        runtime.applied_signature = sig;
        runtime.rebuild_retries = 0;
        runtime.engine_geometry_available = true;
        // Edits during CPU staging/GPU compilation still need a later snapshot.
        // Interactive views may display this complete snapshot; captures wait.
        runtime.engine_geometry_ready = compute_vision_scene_signature() == sig;
        CFW_LOG_INFO("Vision bridge synchronized: candidates={}, instances={}",
                     result.candidate_count, result.instance_count);
    } else {
        runtime.engine_geometry_ready = false;
        ++runtime.rebuild_retries;
        // Preserve the last complete scene; never accept a partial build as the
        // new baseline. Back off instead of recompiling indefinitely at frame rate.
        runtime.next_geometry_retry = now + std::chrono::seconds(
            runtime.rebuild_retries < kVisionRebuildMaxRetries ? 1 : 5);
        if (runtime.rebuild_retries == 1 || runtime.rebuild_retries == kVisionRebuildMaxRetries) {
            CFW_LOG_WARNING("Vision bridge waiting: candidates={}, pending={}, invalid={}; no partial frame published",
                            result.candidate_count, result.skipped_no_data, result.invalid_data);
        }
    }
}

bool OpticsSystem::sync_engine_vision_lighting(VisionPipelineRuntime& runtime, const EnvironmentDevice& env) {
    const auto lighting = Vision::make_vision_lighting_state(env);
    if (runtime.engine_lighting == lighting) return true;
    try {
        auto& pipeline = *runtime.pipeline;
        runtime.wait_for_geometry_users();
        pipeline.activate_view_context(0u);
        auto& scene = pipeline.scene();
        const bool supports_sun_update = runtime.mode == CameraVisionRenderMode::PathTracing ||
                                         runtime.mode == CameraVisionRenderMode::ProgressivePathTracing ||
                                         runtime.mode == CameraVisionRenderMode::ReSTIR;
        if (supports_sun_update && runtime.engine_lighting &&
            Vision::try_update_vision_sun_direction(pipeline, *runtime.engine_lighting, lighting)) {
            pipeline.invalidate();
            pipeline.invalidate_all_view_contexts();
        } else {
            Vision::setup_vision_lights(pipeline, env);
            scene.prepare();
            pipeline.refresh_world_bounds_dependents(true);
        }
        runtime.engine_lighting = lighting;
        return true;
    } catch (const std::exception& e) {
        // A partial upload must be rebuilt even if the next request restores the old direction.
        runtime.engine_lighting.reset();
        CFW_LOG_ERROR("Vision environment synchronization failed: {}", e.what());
        return false;
    }
}




bool OpticsSystem::init_vision_lazy() {
    if (vision_initialized_) return true;
    try {
        const auto selection = select_visible_vision_render_mode(
            [this](auto camera) { return has_pending_screenshot(camera); });
        if (selection.has_visible_camera) current_vision_render_mode_ = selection.mode;
        // Keep the device across scene changes, but release it explicitly in
        // shutdown instead of relying on static destruction after process exit.
        if (!vision_device_) {
            vision_device_ = std::make_unique<ocarina::Device>([] {
                // Engine deployment puts Vision plugins beside the executable. The
                // standalone hotfix setup may have registered ../bin first; its
                // missing DLL aborts loading before the runtime directory is tried.
                auto runtime_directory = std::filesystem::current_path();
#ifdef _WIN32
                std::vector<wchar_t> executable_path(32768);
                const auto length = GetModuleFileNameW(
                    nullptr, executable_path.data(), static_cast<DWORD>(executable_path.size()));
                if (length == 0 || length >= executable_path.size()) {
                    throw std::runtime_error("Cannot resolve the Vision runtime directory");
                }
                runtime_directory = std::filesystem::path(
                    std::wstring(executable_path.data(), length)).parent_path();
#endif
                auto& context = ocarina::RHIContext::instance();
                ocarina::DynamicModule::clear_search_path();
                context.init(runtime_directory);
                CFW_LOG_INFO("OpticsSystem: Vision compute backend = vulkan-compute");
                return context.create_device();
            }());
        }
        vision_device_->init_rtx();
        vision::Global::instance().set_device(vision_device_.get());
        vision::Global::instance().set_scene_path(std::filesystem::current_path());

        const auto key = make_vision_pipeline_key(current_vision_render_mode_);
        auto& runtime = get_or_create_runtime(key);
        if (!initialize_engine_built_vision_runtime(runtime, key)) return false;
        activate_single_vision_runtime_key(key);
        vision_initialized_ = true;
        return true;
    } catch (const std::exception& e) {
        CFW_LOG_ERROR("OpticsSystem: Vision init failed: {}", e.what());
        return false;
    }
}

void OpticsSystem::run_vision_frame(uint64_t frame_index,
                                   const std::vector<RenderView>& views) {
    if (views.empty() || !hardware_->visionDisplayPipeline) return;
    Corona::Diagnostics::PhaseScope vision_phase{"Optics", "vision:scene_sync"};

    auto cleanup_runtime_contexts =
        [&](VisionPipelineRuntime& runtime,
            const std::unordered_set<std::uintptr_t>& active_contexts) {
            auto& pipeline = runtime.pipeline;
            if (!pipeline) {
                return;
            }
            pipeline->activate_global_context();

            std::erase_if(runtime.incompatible_views, [&](const auto& entry) {
                return !active_contexts.contains(entry.first);
            });
            for (auto it = runtime.bridges.begin(); it != runtime.bridges.end();) {
                if (active_contexts.contains(it->first)) {
                    ++it;
                    continue;
                }
                const auto camera_handle = it->first;
                bool camera_exists = false;
                bool retain_bridge = false;
                if (auto camera = Corona::find_camera(
                        camera_handle)) {
                    camera_exists = true;
                    retain_bridge = camera->surface != nullptr;
                }

                // ѡ��������������������ʾ������� surface ר��Ŀ�꣬�����ù�������ͼ��
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
                runtime.wait_for_interop_submission(camera_handle, "inactive bridge release");
                it = runtime.bridges.erase(it);
                if (camera_exists) {
                    pipeline->invalidate_view_context(camera_handle);
                    runtime.retained_contexts.insert(camera_handle);
                } else {
                    pipeline->remove_view_context(camera_handle);
                    runtime.view_denoise_states.erase(camera_handle);
                    runtime.view_denoiser_types.erase(camera_handle);
                }
            }
            for (auto it = runtime.retained_contexts.begin();
                 it != runtime.retained_contexts.end();) {
                if (Corona::find_camera(*it) != nullptr) {
                    ++it;
                    continue;
                }
                pipeline->remove_view_context(*it);
                runtime.view_denoise_states.erase(*it);
                runtime.view_denoiser_types.erase(*it);
                it = runtime.retained_contexts.erase(it);
            }
            pipeline->activate_view_context(0u);
        };

    auto render_locked_vision_camera =
        [&](VisionPipelineRuntime& runtime,
            const RenderView& view,
            LockedRenderView& locked,
            std::unordered_set<std::uintptr_t>& active_contexts,
            VisionCameraFeedback& feedback, bool rendering_fallback,
            std::optional<std::string>& fallback_settings) {
            const bool profile_frame = env_flag_enabled("CORONA_VISION_FRAME_PROFILE");
            const auto profile_now = [&] {
                return profile_frame ? std::chrono::steady_clock::now()
                                     : std::chrono::steady_clock::time_point{};
            };
            const auto profile_ms = [](auto begin, auto end) {
                return std::chrono::duration<double, std::milli>(end - begin).count();
            };
            const auto profile_start = profile_now();
            const auto cam_handle = view.camera_handle;
            const auto& camera = locked.camera;
            const auto environment = vision_environment_for_scene(*locked.scene);
            const auto imported_media_count = locked.scene->authored_media
                ? locked.scene->authored_media->at("mediums").size() : 0u;
            auto& pipeline = runtime.pipeline;
            if (!pipeline) {
                return;
            }
            pipeline->activate_global_context();

            if ((!runtime.skinning_ready || !runtime.engine_geometry_available ||
                 (has_pending_screenshot(cam_handle) && !runtime.engine_geometry_ready))) return;
            active_contexts.insert(cam_handle);
            runtime.retained_contexts.erase(cam_handle);
            locked.scene = {};
            vision_phase.set("vision:lighting");
            if (!sync_engine_vision_lighting(runtime, environment)) return;
            try {
                const bool denoise = camera.vision_denoise ||
                    Vision::vision_render_mode_uses_denoise(runtime.mode);
                vision_phase.set("vision:view_prepare");
                if (!prepare_vision_camera_view(runtime, cam_handle,
                                               camera.width, camera.height, denoise,
                                               camera.vision_accumulation, camera.vision_stable_planes, camera.vision_denoiser,
                                               camera.vision_reblur_settings, camera.vision_relax_settings, camera.vision_pt_settings,
                                               camera.vision_restir_settings, &feedback, imported_media_count)) {
                    return;
                }

                const auto incompatible = [&] {
                    const auto& integrator = pipeline->renderer().integrator();
                    const char* reason = integrator->render_incompatibility();
                    if (!reason) return false;
                    const bool screenshot = has_pending_screenshot(cam_handle);
                    const bool fallback = runtime.mode == CameraVisionRenderMode::ReSTIR &&
                        view.vision_mode == runtime.mode && camera.surface &&
                        !screenshot &&
                        integrator->allows_path_tracing_fallback();
                    auto& last = runtime.incompatible_views[cam_handle];
                    if (screenshot || last.first != reason || last.second != fallback) {
                        if (fallback) {
                            CFW_LOG_WARNING("OpticsSystem: camera {} using path tracing for this viewport: {}", cam_handle, reason);
                        } else {
                            CFW_LOG_ERROR("OpticsSystem: camera {} cannot render the requested output: {}", cam_handle, reason);
                        }
                        last = {reason, fallback};
                    }
                    if (fallback) fallback_settings = camera.vision_restir_settings;
                    if (screenshot) fail_pending_screenshots(cam_handle);
                    return true;
                };
                if (incompatible()) return;

                auto& bridge = runtime.bridges[cam_handle];
                if (!bridge) {
                    bridge = std::make_unique<Vision::VisionZeroCopyBridge>();
                }
                // Keep failed bridges tracked for normal view cleanup. A resize,
                // scene reload or view recreation provides a fresh retry.
                if (bridge->failed()) {
                    return;
                }

                vision_phase.set("vision:camera_sync");
                Vision::sync_vision_camera(*pipeline, camera);
                vision_phase.set("vision:display");
                const auto render_start = profile_now();
                std::optional<PendingScreenshot> capture;
                {
                    std::lock_guard<std::mutex> lock(screenshot_mutex_);
                    const auto request = std::find_if(pending_screenshots_.begin(), pending_screenshots_.end(),
                        [&](const PendingScreenshot& request) {
                            return request.camera_handle == cam_handle && request.render_frames != 0;
                        });
                    if (request != pending_screenshots_.end()) capture = *request;
                }
                std::ofstream frame_timings, stage_timings;
                uint64_t capture_qpc_frequency = 0;
                const auto capture_profile_qpc = []() -> uint64_t {
#ifdef _WIN32
                    LARGE_INTEGER value{};
                    QueryPerformanceCounter(&value);
                    return static_cast<uint64_t>(value.QuadPart);
#else
                    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
                };
                if (capture) {
                    if (!capture->timings_path.empty()) {
                        frame_timings.open(std::filesystem::u8path(capture->timings_path));
                        if (!frame_timings) throw std::runtime_error("Cannot open screenshot timing CSV");
                        frame_timings << "frame,integrator_frame,render_including_denoise_ms,denoise_ms\n"
                                      << std::setprecision(12);
                        if (env_flag_enabled("VISION_STAGE_PROFILE") ||
                            env_flag_enabled("CORONA_VISION_CAPTURE_PROFILE")) {
                            const auto stage_path = std::filesystem::u8path(capture->timings_path).parent_path() / "stages.csv";
                            stage_timings.open(stage_path);
                            if (!stage_timings) throw std::runtime_error("Cannot open screenshot stage CSV");
#ifdef _WIN32
                            LARGE_INTEGER frequency{};
                            QueryPerformanceFrequency(&frequency);
                            capture_qpc_frequency = static_cast<uint64_t>(frequency.QuadPart);
#else
                            capture_qpc_frequency = 1000000000u;
#endif
                            stage_timings << "frame,integrator_frame,enabled,render_including_denoise_ms,denoise_ms,"
                                "gbuffer_ms,sampling_mask_ms,path_tracing_ms,stable_build_ms,stable_fill_ms,restir_di_ms,restir_gi_ms,"
                                "spatial_angular_ms,temporal_ms,combine_ms,postprocess_ms,render_final_ms,"
                                "frame_qpc_begin,frame_qpc_end,qpc_frequency\n" << std::setprecision(12);
                        }
                    }
                    pipeline->invalidate();
                }
                const auto render_frames = capture ? capture->render_frames : 1u;
                // BEGIN GI guide-reuse camera sequence parser
                struct DiagnosticCaptureCameraPose {
                    unsigned frame{};
                    std::array<float, 3> position{}, forward{}, world_up{};
                };
                const auto load_diagnostic_camera_sequence = [](const char* path, unsigned frame_count) {
                    std::ifstream input(std::filesystem::u8path(path));
                    if (!input) throw std::runtime_error("Cannot open GI guide-reuse camera sequence");
                    std::string row;
                    if (!std::getline(input, row)) throw std::runtime_error("Empty GI guide-reuse camera sequence");
                    if (!row.empty() && row.back() == '\r') row.pop_back();
                    if (row != "frame,pos_x,pos_y,pos_z,forward_x,forward_y,forward_z,up_x,up_y,up_z")
                        throw std::runtime_error("Invalid GI guide-reuse camera sequence header");
                    std::vector<DiagnosticCaptureCameraPose> poses;
                    while (std::getline(input, row)) {
                        if (!row.empty() && row.back() == '\r') row.pop_back();
                        if (row.empty()) continue;
                        if (std::count(row.begin(), row.end(), ',') != 9)
                            throw std::runtime_error("Invalid GI guide-reuse camera sequence column count");
                        std::istringstream fields(row);
                        std::string field;
                        std::getline(fields, field, ',');
                        if (field.empty() || !std::all_of(field.begin(), field.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
                            throw std::runtime_error("Invalid GI guide-reuse camera sequence frame");
                        const auto index = std::stoull(field);
                        if (index >= frame_count || (!poses.empty() && index <= poses.back().frame))
                            throw std::runtime_error("GI guide-reuse camera sequence frames must increase within the capture");
                        DiagnosticCaptureCameraPose pose;
                        pose.frame = static_cast<unsigned>(index);
                        std::array<float, 9> values{};
                        for (auto& value : values) {
                            std::getline(fields, field, ',');
                            std::istringstream number(field);
                            if (!(number >> value) || !std::isfinite(value))
                                throw std::runtime_error("Invalid GI guide-reuse camera sequence value");
                            number >> std::ws;
                            if (!number.eof()) throw std::runtime_error("Invalid GI guide-reuse camera sequence numeric suffix");
                        }
                        for (unsigned i = 0; i < 3; ++i) {
                            pose.position[i] = values[i];
                            pose.forward[i] = values[i + 3];
                            pose.world_up[i] = values[i + 6];
                        }
                        const auto& f = pose.forward;
                        const auto& u = pose.world_up;
                        const std::array<float, 3> cross{f[1] * u[2] - f[2] * u[1],
                            f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0]};
                        const auto cross2 = cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2];
                        if (!std::isfinite(cross2) || cross2 <= 1e-12f)
                            throw std::runtime_error("Degenerate GI guide-reuse camera sequence axes");
                        poses.push_back(pose);
                    }
                    if (!input.eof() || poses.empty()) throw std::runtime_error("Cannot read GI guide-reuse camera sequence");
                    return poses;
                };
                // END GI guide-reuse camera sequence parser
                std::vector<DiagnosticCaptureCameraPose> camera_sequence;
                std::ofstream camera_sequence_audit;
                auto sequence_camera = camera;
                size_t camera_sequence_index = 0;
                // The controlled diagnostic batch is192 frames. Its setup1 and
                // context-primer4 batches must not consume the same schedule.
                if (capture && render_frames == 192u) {
                    if (const char* path = std::getenv("VISION_GI_GUIDE_REUSE_CAMERA_SEQUENCE"); path && *path) {
                        camera_sequence = load_diagnostic_camera_sequence(path, render_frames);
                        const auto directory = std::filesystem::u8path(capture->timings_path.empty()
                            ? capture->file_path : capture->timings_path).parent_path();
                        camera_sequence_audit.open(directory / "camera-sequence-audit.csv");
                        if (!camera_sequence_audit) throw std::runtime_error("Cannot open GI guide-reuse camera sequence audit");
                        camera_sequence_audit << "frame,capture_frame,event,integrator_before_sync,integrator_before,integrator_after,"
                            "external_camera_revision,external_projection_revision,pos_x,pos_y,pos_z,forward_x,forward_y,forward_z,up_x,up_y,up_z\n"
                            << std::setprecision(std::numeric_limits<float>::max_digits10);
                    }
                }
                // Offline accumulation bypasses interaction/presentation pacing.
                // Only the final frame continues through the normal viewport
                // resolve, overlays and process_pending_screenshots below.
                for (unsigned frame = 1; frame <= render_frames; ++frame) {
                    if (capture && std::chrono::steady_clock::now() >= capture->expires_at) {
                        fail_pending_screenshots(cam_handle);
                        return;
                    }
                    const auto frame_qpc_begin = stage_timings.is_open() ? capture_profile_qpc() : 0u;
                    // display() advances the host's previous-camera matrices.
                    // Publish them before every offline subframe reprojects history.
                    const auto sequence_integrator_before_sync = pipeline->frame_index();
                    bool camera_sequence_event = false;
                    if (!camera_sequence.empty()) {
                        if (camera_sequence_index < camera_sequence.size() &&
                            camera_sequence[camera_sequence_index].frame == frame - 1u) {
                            const auto& pose = camera_sequence[camera_sequence_index++];
                            sequence_camera.position = ktm::fvec3{pose.position[0], pose.position[1], pose.position[2]};
                            sequence_camera.forward = ktm::fvec3{pose.forward[0], pose.forward[1], pose.forward[2]};
                            sequence_camera.world_up = ktm::fvec3{pose.world_up[0], pose.world_up[1], pose.world_up[2]};
                            camera_sequence_event = true;
                        }
                        // Pose-only synchronization preserves realtime history;
                        // projection, resolution and accumulation stay unchanged.
                        Vision::sync_vision_camera(*pipeline, sequence_camera);
                    }
                    const auto sequence_integrator_before = pipeline->frame_index();
                    pipeline->upload_data();
                    pipeline->display(1.0 / 60.0);
                    if (incompatible()) return;
                    runtime.incompatible_views.erase(cam_handle);
                    const auto frame_qpc_end = stage_timings.is_open() ? capture_profile_qpc() : 0u;
                    if (camera_sequence_audit.is_open()) {
                        if (sequence_integrator_before_sync != frame - 1u ||
                            sequence_integrator_before != frame - 1u || pipeline->frame_index() != frame)
                            throw std::runtime_error("GI guide-reuse camera sequence unexpectedly reset history");
                        const auto& sensor = pipeline->scene().sensor();
                        camera_sequence_audit << frame << ',' << frame - 1u << ',' << camera_sequence_event << ','
                            << sequence_integrator_before_sync << ',' << sequence_integrator_before << ',' << pipeline->frame_index() << ','
                            << sensor->external_camera_revision() << ',' << sensor->external_projection_revision() << ','
                            << sequence_camera.position.x << ',' << sequence_camera.position.y << ',' << sequence_camera.position.z << ','
                            << sequence_camera.forward.x << ',' << sequence_camera.forward.y << ',' << sequence_camera.forward.z << ','
                            << sequence_camera.world_up.x << ',' << sequence_camera.world_up.y << ',' << sequence_camera.world_up.z << '\n';
                    }
                    if (frame_timings.is_open()) {
                        frame_timings << frame << ',' << pipeline->frame_index() << ','
                                      << pipeline->cur_render_time() << ','
                                      << pipeline->renderer().integrator()->cur_denoise_time() << '\n';
                    }
                    if (stage_timings.is_open()) {
                        const auto& integrator = pipeline->renderer().integrator();
                        const auto& stages = integrator->cur_stage_profile();
                        stage_timings << frame << ',' << pipeline->frame_index() << ',' << stages.enabled << ','
                            << pipeline->cur_render_time() << ',' << integrator->cur_denoise_time() << ','
                            << stages.gbuffer_ms << ',' << stages.sampling_mask_ms << ',' << stages.path_tracing_ms << ','
                            << stages.stable_build_ms << ',' << stages.stable_fill_ms << ','
                            << stages.restir_di_ms << ',' << stages.restir_gi_ms << ','
                            << stages.spatial_angular_ms << ',' << stages.temporal_ms << ',' << stages.combine_ms << ','
                            << stages.postprocess_ms << ',' << stages.render_final_ms << ','
                            << frame_qpc_begin << ',' << frame_qpc_end << ',' << capture_qpc_frequency << '\n';
                    }
                }
                if (camera_sequence_audit.is_open()) {
                    camera_sequence_audit.flush();
                    if (!camera_sequence_audit) throw std::runtime_error("Cannot write GI guide-reuse camera sequence audit");
                }
                if (frame_timings.is_open()) {
                    frame_timings.flush();
                    if (!frame_timings) throw std::runtime_error("Cannot write screenshot timing CSV");
                }
                if (stage_timings.is_open()) {
                    stage_timings.flush();
                    if (!stage_timings) throw std::runtime_error("Cannot write screenshot stage CSV");
                }
                if (runtime.participating_media && capture) {
                    auto effective = pipeline->renderer().integrator()->settings();
                    add_vision_media_feedback(effective, imported_media_count,
                                              pipeline->scene().geometry().boundary_count(), true);
                    feedback.pt_effective = effective.dump();
                }
                const auto render_end = profile_now();
                if (env_flag_enabled("CORONA_OPTICS_MEASURE_RENDER_TIME")) {
                    CFW_LOG_INFO("VisionRenderTiming camera={} extent={}x{} accumulated_frames={} render_ms={}",
                                 cam_handle, camera.width, camera.height,
                                 pipeline->renderer().integrator()->frame_index(),
                                 pipeline->cur_render_time());
                }

                auto* fb = pipeline->frame_buffer();
                const auto res = fb->resolution();
                const uint32_t w = res.x;
                const uint32_t h = res.y;

                // The preceding image-store pass must finish before Vision reuses or
                // reallocates its shared buffer. Frame pixels never visit the CPU.
                vision_phase.set("vision:interop");
                const auto reuse_start = profile_now();
                runtime.wait_for_interop_submission(cam_handle, "shared buffer reuse", false);
                const auto reuse_end = profile_now();
                const auto& display = environment.vision_display_profile;
                if (!bridge->ensure(*pipeline, w, h) ||
                    !bridge->render_display(*pipeline, environment.exposure,
                                            display ? &*display : nullptr)) {
                    // The bridge logs and latches failures before this frame is published.
                    return;
                }
                const auto display_end = profile_now();

                void* surface = camera.surface;
                auto& target = surface
                    ? acquire_surface_target(surface, w, h, frame_index)
                    : acquire_offscreen_screenshot_target(cam_handle, w, h, frame_index);
                if (auto consumed_device =
                        Corona::SharedDataHub::instance().image_write(
                            target.image_handle)) {
                    if (consumed_device->consumed_receipt.serial != 0) {
                        hardware_->executor.wait(consumed_device->consumed_receipt);
                    }
                }

                const auto target_end = profile_now();
                auto stream = hardware_->executor.stream();
                // Keep the display image in GENERAL for the overlay and compositor.
                hardware_->visionDisplayPixels = bridge->imported();
                hardware_->visionDisplayImage = target.final_output;
                hardware_->visionDisplayExtent = ktm::uvec2{w, h};
                stream << hardware_->visionDisplayPipeline->dispatch_extent(w, h);

                const auto ui_state =
                    SharedDataHub::instance().viewport_ui_state(cam_handle);
                Horizon::HardwareImage* presented = &target.final_output;
                if (surface) {
                    vision_phase.set("vision:overlay");
                    // Overlay geometry is read directly under a short scene
                    // lock. Its GPU submission keeps the resource handles alive.
                    auto scene = Corona::find_scene(view.scene_handle);
                    if (!scene || !scene->enabled) return;
                    presented = compose_surface_ui_overlay(
                        stream, cam_handle, camera, *scene, target, target.final_output,
                        ui_state, frame_index);
                }

                vision_phase.set("vision:publish");
                const auto submit_start = profile_now();
                const Horizon::SubmitReceipt vision_submit_receipt =
                    stream << Horizon::commit();
                const auto submit_end = profile_now();
                hardware_->last_receipt = vision_submit_receipt;
                runtime.interop_submissions.insert_or_assign(cam_handle, vision_submit_receipt);

                if (runtime.participating_media && capture) {
                    // The screenshot response allows the caller to query these
                    // counters immediately, so publish before resolving it.
                    publish_vision_camera_feedback(cam_handle, feedback);
                }
                process_pending_screenshots(cam_handle, *presented, w, h);
                if (!surface) return;

                const ImagePixelExtent presented_extent{target.width, target.height};
                const auto viewport = optics_event_viewport(camera, presented_extent);
                if (auto image_device =
                        Corona::SharedDataHub::instance().image_write(
                            target.image_handle)) {
                    if (vision_submit_receipt.serial == 0) {
                        CFW_LOG_WARNING(
                            "OpticsSystem: publishing Vision frame with empty submit receipt "
                            "(camera={}, surface={}, image_handle={}, frame={}, extent={}x{})",
                            cam_handle,
                            surface,
                            target.image_handle,
                            frame_index,
                            camera.width,
                            camera.height);
                    }
                    image_device->image = *presented;
                    image_device->submit_receipt = vision_submit_receipt;
                    image_device->metadata = {frame_index,
                                              presented_extent.width,
                                              presented_extent.height,
                                              viewport.x,
                                              viewport.y,
                                              viewport.width,
                                              viewport.height};
                }

                if (auto* event_bus = context()->event_bus()) {
                    Corona::push_optics_frame(
                        {surface,
                         target.image_handle,
                         frame_index,
                         presented_extent.width,
                         presented_extent.height,
                         viewport.x,
                         viewport.y,
                         viewport.width,
                         viewport.height,
                         target.published_image});
                }
                if (profile_frame) {
                    const auto end = profile_now();
                    // Wall times at existing boundaries only: do not add GPU waits
                    // or decompose the renderer's fixed-cost algorithms.
                    CFW_LOG_INFO("VisionFrameProfile frame={} camera={} extent={}x{} prepare_ms={} render_ms={} bookkeeping_ms={} reuse_wait_ms={} display_sync_ms={} target_ms={} overlay_record_ms={} submit_ms={} publish_ms={} total_ms={}",
                        frame_index, cam_handle, w, h,
                        profile_ms(profile_start, render_start), profile_ms(render_start, render_end),
                        profile_ms(render_end, reuse_start), profile_ms(reuse_start, reuse_end),
                        profile_ms(reuse_end, display_end), profile_ms(display_end, target_end),
                        profile_ms(target_end, submit_start), profile_ms(submit_start, submit_end),
                        profile_ms(submit_end, end), profile_ms(profile_start, end));
                }
            } catch (const std::exception& error) {
                CFW_LOG_ERROR("OpticsSystem: Vision camera {} failed: {}",
                              cam_handle, error.what());
                fail_pending_screenshots(cam_handle);
            }
        };

    auto render_vision_camera = [&](VisionPipelineRuntime& runtime, const RenderView& view,
                                     std::unordered_set<std::uintptr_t>& active_contexts,
                                     const VisionPipelineKey& key,
                                     const std::string* previous_settings = nullptr) -> std::optional<std::string> {
        const bool profile_frame = env_flag_enabled("CORONA_VISION_FRAME_PROFILE");
        const auto start = profile_frame ? std::chrono::steady_clock::now()
                                         : std::chrono::steady_clock::time_point{};
        auto acquired = start;
        VisionCameraFeedback feedback;
        std::optional<std::string> fallback_settings;
        {
            auto locked = lock_render_view(view);
            if (profile_frame) acquired = std::chrono::steady_clock::now();
            if (!locked || locked->camera.vision_render_mode != view.vision_mode ||
                camera_uses_vision_media(locked->camera) != runtime.participating_media) return std::nullopt;
            if (previous_settings && (!locked->camera.surface || has_pending_screenshot(view.camera_handle) ||
                locked->camera.vision_restir_settings != *previous_settings)) return std::nullopt;
            if (key.participating_media &&
                (locked->scene->authored_media != key.authored_media ||
                 locked->scene->authored_media_revision != key.authored_media_revision)) return std::nullopt;
            render_locked_vision_camera(runtime, view, *locked, active_contexts, feedback,
                                        previous_settings != nullptr, fallback_settings);
        }
        const auto feedback_start = profile_frame ? std::chrono::steady_clock::now() : start;
        publish_vision_camera_feedback(view.camera_handle, feedback);
        if (profile_frame) {
            CFW_LOG_INFO("VisionAccessProfile frame={} camera={} lock_ms={} feedback_ms={} total_ms={}",
                frame_index, view.camera_handle,
                std::chrono::duration<double, std::milli>(acquired - start).count(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - feedback_start).count(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        return fallback_settings;
    };

    const auto mode_selection = select_vision_render_mode(views);
    if (mode_selection.has_visible_camera) {
        if (mode_selection.conflict &&
            mode_selection.conflict_signature != last_vision_mode_conflict_signature_) {
            last_vision_mode_conflict_signature_ = mode_selection.conflict_signature;
            CFW_LOG_WARNING(
                "OpticsSystem: multiple visible engine-built Vision cameras request different "
                "render modes ({}); camera {} selects the primary runtime '{}'",
                mode_selection.conflict_summary,
                mode_selection.selected_camera,
                std::string(Vision::vision_render_mode_name(mode_selection.mode)));
        } else if (!mode_selection.conflict) {
            last_vision_mode_conflict_signature_ = 0;
        }
        apply_vision_render_mode(mode_selection.mode);
    }

    auto& surface_runtime = active_vision_runtime();

    if (mode_selection.has_visible_camera) {
        sync_vision_dynamic_scene(surface_runtime);
    }

    std::unordered_map<VisionPipelineRuntime*, std::unordered_set<std::uintptr_t>> active_contexts;
    active_contexts.try_emplace(&surface_runtime);
    for (const auto& view : views) {
        auto* runtime = &surface_runtime;
        VisionPipelineKey key{view.vision_mode, view.participating_media};
        if (view.participating_media) {
            auto* scene = Corona::find_scene(view.scene_handle);
            if (!scene || !scene->enabled) continue;
            key.authored_media = scene->authored_media;
            key.authored_media_revision = scene->authored_media_revision;
        }
        // ReSTIR capability checks must run on the requested integrator, even
        // when another camera selected the primary runtime for this frame.
        const bool different_integrator = (view.vision_mode == CameraVisionRenderMode::ReSTIR) !=
                                          (surface_runtime.mode == CameraVisionRenderMode::ReSTIR);
        if (view.participating_media || different_integrator) {
            runtime = &get_or_create_runtime(key);
            if (!runtime->pipeline && !initialize_engine_built_vision_runtime(*runtime, key)) {
                fail_pending_screenshots(view.camera_handle);
                continue;
            }
            if (!active_contexts.contains(runtime)) sync_vision_dynamic_scene(*runtime);
        }
        runtime->last_used_frame = frame_index;
        if (const auto previous_settings = render_vision_camera(*runtime, view, active_contexts[runtime], key)) {
            const auto pt_key = make_vision_pipeline_key(CameraVisionRenderMode::PathTracing);
            auto& pt = get_or_create_runtime(pt_key);
            if (!pt.pipeline && !initialize_engine_built_vision_runtime(pt, pt_key)) continue;
            auto [contexts, first_use] = active_contexts.try_emplace(&pt);
            if (first_use) sync_vision_dynamic_scene(pt);
            pt.last_used_frame = frame_index;
            render_vision_camera(pt, view, contexts->second, pt_key, &*previous_settings);
        }
    }

    // Invalidate histories when their runtime falls idle after a camera mode
    // switch or viewport fallback. Its compiled contexts and bridges are reusable.
    for (const auto& [key, runtime] : vision_runtimes_) {
        if (!key.participating_media && runtime->last_used_frame + 1 == frame_index)
            active_contexts.try_emplace(runtime.get());
    }
    for (auto& [runtime, contexts] : active_contexts) cleanup_runtime_contexts(*runtime, contexts);
    // Old media revisions and disabled views must release their histories.
    for (auto it = vision_runtimes_.begin(); it != vision_runtimes_.end();) {
        if (it->first.participating_media && !active_contexts.contains(it->second.get())) {
            it->second->commit_and_clear_contexts();
            it = vision_runtimes_.erase(it);
        } else {
            ++it;
        }
    }
    if (surface_runtime.pipeline) surface_runtime.pipeline->activate_global_context();
    evict_idle_vision_runtimes(frame_index);
}


bool OpticsSystem::initialize_engine_built_vision_runtime(VisionPipelineRuntime& runtime,
                                                         const VisionPipelineKey& key) {
    auto previous_pipeline = vision::Global::instance().pipeline_shared();
    try {
        const auto& scene_resource = runtime.scene_resource;
        auto pipeline = create_vision_pipeline(key.mode, scene_resource, key.participating_media);
        if (!pipeline) {
            CFW_LOG_ERROR("OpticsSystem: failed to recreate engine-built Vision pipeline");
            if (previous_pipeline) previous_pipeline->activate_global_context();
            return false;
        }
        bind_pipeline_scene_gpu_resource(*pipeline, *scene_resource, key.mode);
        auto& scene = pipeline->scene();
        if (key.participating_media) {
            Vision::configure_vision_media(scene, key.authored_media.get(), true);
        }
        const auto build_signature = compute_vision_scene_signature();
        auto build_result = Vision::build_vision_geometry(scene);
        sync_logical_instances_from_pipeline_scene(*scene_resource, scene);

        const auto env = engine_vision_environment();
        Vision::setup_vision_lights(*pipeline, env);
        pipeline->prepare();
        // Camera GPU buffers exist only after prepare().
        if (const auto camera = active_scene_camera_values()) {
            Vision::sync_vision_camera(*pipeline, *camera);
        }

        runtime.reset_pipeline(std::move(pipeline), key.mode);
        runtime.bind_dynamic_geometry(build_result);
        runtime.pipeline->activate_global_context();
        const auto current_signature = compute_vision_scene_signature();
        runtime.engine_geometry_available = build_result.complete();
        runtime.engine_geometry_ready = build_result.complete() && current_signature == build_signature;
        runtime.engine_lighting = Vision::make_vision_lighting_state(env);
        runtime.applied_signature = build_result.complete() ? build_signature : 0;
        runtime.pending_signature = current_signature;
        runtime.rebuild_retries = 0;
        return true;
    } catch (const std::exception& e) {
        if (previous_pipeline) previous_pipeline->activate_global_context();
        CFW_LOG_ERROR("OpticsSystem: initializing engine-built Vision scene failed: {}", e.what());
        return false;
    }
}

bool OpticsSystem::load_engine_built_vision_scene(CameraVisionRenderMode requested_mode) {
    const auto key = make_vision_pipeline_key(requested_mode);
    auto& runtime = get_or_create_runtime(key);
    if (!initialize_engine_built_vision_runtime(runtime, key)) return false;
    activate_single_vision_runtime_key(key);
    runtime.pipeline->activate_global_context();
    current_vision_render_mode_ = requested_mode;
    if (requested_mode == CameraVisionRenderMode::SVGF ||
        requested_mode == CameraVisionRenderMode::SSAT) {
        CFW_LOG_WARNING(
            "OpticsSystem: engine-built Vision scene can only toggle denoise in "
            "Phase 2; requested mode '{}' does not change framebuffer or denoiser type",
            std::string(Vision::vision_render_mode_name(requested_mode)));
    }
    CFW_LOG_INFO("OpticsSystem: restored engine-built Vision scene");
    log_vision_pipeline_diagnostics(*runtime.pipeline, "engine-built mode switch");
    return true;
}

void OpticsSystem::apply_vision_render_mode(CameraVisionRenderMode mode) {
    auto& runtime = active_vision_runtime();
    auto& pipeline = runtime.pipeline;
    if (!pipeline) {
        return;
    }

    auto rekey_active_runtime = [&]() {
        const auto key = make_vision_pipeline_key(runtime.mode);
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
        return;
    }

    {
        // A different integrator needs a new renderer descriptor as well as new
        // base/view contexts. Rekeying the old runtime only changes its label.
        if ((mode == CameraVisionRenderMode::ReSTIR) !=
            (current_vision_render_mode_ == CameraVisionRenderMode::ReSTIR)) {
            (void)load_engine_built_vision_scene(mode);
            return;
        }
        const bool was_denoise_enabled =
            Vision::vision_render_mode_uses_denoise(current_vision_render_mode_);
        const bool denoise_enabled = Vision::vision_render_mode_uses_denoise(mode);
        runtime.commit_and_clear_contexts();
        pipeline->set_output_denoise(denoise_enabled);
        if (denoise_enabled && !was_denoise_enabled) {
            prepare_enabled_denoiser_for_runtime_switch(*pipeline);
        }
        if (mode != CameraVisionRenderMode::SSAT) {
            const bool accumulate = mode == CameraVisionRenderMode::ProgressivePathTracing;
            pipeline->frame_buffer()->set_enable_accumulation(accumulate);
            pipeline->frame_buffer()->auto_manage_accumulation_buffer(accumulate);
        }
        pipeline->invalidate();
        runtime.mode = mode;
        current_vision_render_mode_ = mode;
        rekey_active_runtime();
        if (mode == CameraVisionRenderMode::SVGF || mode == CameraVisionRenderMode::SSAT) {
            CFW_LOG_WARNING(
                "OpticsSystem: requested Vision mode '{}' on engine-built scene; "
                "Phase 2 only toggles denoise without changing framebuffer or denoiser type",
                std::string(Vision::vision_render_mode_name(mode)));
        }
        log_vision_pipeline_diagnostics(
            *pipeline,
            std::string("mode switch ") + std::string(Vision::vision_render_mode_name(mode)));
        return;
    }


}



}  // namespace Corona::Systems
