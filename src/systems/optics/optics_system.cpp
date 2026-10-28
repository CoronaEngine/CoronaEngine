#include "native_material.h"
#include "native_camera.h"
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
#include <corona/systems/geometry/native_render_load_guard.h>
#include <corona/systems/optics/optics_system.h>

#include "shadow_culling.h"

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
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <span>
#include <system_error>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <oneapi/tbb/task_group.h>

#include "hardware.h"
#include "../common/render_resource_bindings.h"
#include "actor_pick_result.h"
#include "native_diagnostics.h"
#include "optics_debug_labels.h"
#include "../common/diagnostic_env.h"
#include "../common/vision_restir_settings.h"


#ifdef _WIN32
#include <Windows.h>
#endif
#include "base/import/parameter_set.h"
#include "base/import/json_util.h"
#include "base/import/project_desc.h"
#include "base/mgr/global.h"
#include "base/mgr/pipeline.h"
#include "base/mgr/switch_profile.h"
#include "base/mgr/scene.h"
#include "base/scattering/material.h"
#include "base/sensor/frame_buffer.h"
#include "base/sensor/light_field_types.h"
#include "base/sensor/sensor.h"
#include "rhi/context.h"
#include "vision/vision_geometry_adapter.h"
#include "vision/vision_geometry_sync_policy.h"
#include "vision/vision_camera_adapter.h"
#include "vision/vision_diagnostic_adapter.h"
#include "vision/vision_light_adapter.h"
#include "vision/vision_render_mode_config.h"
#include "vision/vision_interop_lifetime.h"
#include "vision/vision_zero_copy_bridge.h"

namespace {

constexpr uint32_t kShadowCascadeCount = NATIVE_SHADOW_CASCADES;
constexpr uint32_t kShadowMapSize = NATIVE_SHADOW_SIZE;
constexpr float kShadowMaxDistance = NATIVE_SHADOW_DISTANCE;
constexpr float kShadowSplitLambda = NATIVE_SHADOW_SPLIT;
constexpr float kShadowBias = NATIVE_SHADOW_BIAS;
constexpr float kSsaoStrength = NATIVE_SSAO_STRENGTH;
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
    bool skip_scene_visibility = false;
    bool skip_shadows = false;
    bool skip_deferred_compute = false;
    bool disable_textures = false;
    bool disable_albedo_sample = false;
    bool force_solid_material = false;
    bool profile = false;
    bool debug_labels = false;
    std::optional<std::uintptr_t> only_actor;
    std::optional<std::uintptr_t> only_geometry;
    std::optional<std::uint32_t> only_mesh;
    std::uint32_t draw_limit = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t shadow_cascade_mask = (1u << kShadowCascadeCount) - 1u;
};

[[nodiscard]] const OpticsDiagConfig& optics_diag_config() {
    static const OpticsDiagConfig cfg = [] {
        OpticsDiagConfig c;
        c.profile = env_flag_enabled("CORONA_OPTICS_DIAG_PROFILE");
        c.debug_labels = env_flag_enabled("CORONA_OPTICS_DIAG_DEBUG_LABELS");
#ifdef CORONA_NATIVE_DEVELOPER_DIAGNOSTICS
        c.skip_scene_visibility = env_flag_enabled("CORONA_OPTICS_DIAG_SKIP_SCENE_VISIBILITY");
        c.skip_shadows = env_flag_enabled("CORONA_OPTICS_DIAG_SKIP_SHADOWS");
        c.skip_deferred_compute = env_flag_enabled("CORONA_OPTICS_DIAG_SKIP_DEFERRED_COMPUTE");
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
        c.shadow_cascade_mask =
            Corona::Systems::OpticsDetail::parse_shadow_cascade_mask(
                env_u64("CORONA_OPTICS_DIAG_SHADOW_CASCADE_MASK"),
                kShadowCascadeCount);
#endif
        if (c.skip_scene_visibility || c.skip_shadows || c.skip_deferred_compute ||
            c.disable_textures || c.disable_albedo_sample || c.force_solid_material || c.profile ||
            c.debug_labels ||
            c.only_actor || c.only_geometry || c.only_mesh ||
            c.draw_limit != std::numeric_limits<std::uint32_t>::max() ||
            c.shadow_cascade_mask != ((1u << kShadowCascadeCount) - 1u)) {
            CFW_LOG_WARNING("OpticsSystem: diagnostic mode enabled "
                            "(skip_visibility={}, skip_shadows={}, skip_deferred={}, disable_textures={}, "
                            "disable_albedo_sample={}, force_solid={}, profile={}, debug_labels={}, only_actor={}, only_geometry={}, "
                            "only_mesh={}, draw_limit={}, shadow_cascade_mask=0x{:x})",
                            c.skip_scene_visibility, c.skip_shadows, c.skip_deferred_compute,
                            c.disable_textures, c.disable_albedo_sample, c.force_solid_material, c.profile,
                            c.debug_labels,
                            c.only_actor.value_or(0), c.only_geometry.value_or(0),
                            c.only_mesh.value_or(std::numeric_limits<std::uint32_t>::max()),
                            c.draw_limit, c.shadow_cascade_mask);
        }
        return c;
    }();
    return cfg;
}

[[nodiscard]] bool diag_actor_allowed(std::uintptr_t actor_handle) {
    const auto& diag = optics_diag_config();
    return !diag.only_actor || *diag.only_actor == actor_handle;
}

[[nodiscard]] bool diag_geometry_allowed(std::uintptr_t geometry_handle) {
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

[[nodiscard]] std::string make_optics_dispatch_label(std::string_view pass,
                                                     std::uint32_t frame,
                                                     std::uint32_t instances,
                                                     std::uint32_t materials,
                                                     std::uint32_t width,
                                                     std::uint32_t height) {
    if (!Corona::Systems::OpticsDetail::debug_labels_enabled(
            optics_diag_config().profile, optics_diag_config().debug_labels)) {
        return {};
    }
    std::ostringstream out;
    out << "Optics/" << pass
        << " frame=" << frame
        << " instances=" << instances
        << " materials=" << materials
        << " extent=" << width << "x" << height;
    return out.str();
}

void log_invalid_optics_mesh_once(std::uintptr_t actor_handle,
                                  std::uintptr_t geometry_handle,
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

void log_optics_material_once(std::uintptr_t actor_handle,
                              std::uintptr_t geometry_handle,
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

// Perf toggle: sky-driven SH9 ambient. Set to false to skip both the sky��SH
// projection dispatch and the per-pixel evalSkySH in lighting, so the SH
// ambient's cost can be measured in isolation. Compile-time single point.
constexpr bool kSkyAmbientEnabled = true;

struct RenderInstanceBatch {
    struct ResourceKeepAlive {
        std::vector<Corona::Horizon::HardwareBuffer> vertex_draw_buffers;
        std::vector<Corona::Horizon::HardwareBuffer> index_draw_buffers;
        std::vector<Corona::Horizon::HardwareBuffer> vertex_storage_buffers;
        std::vector<Corona::Horizon::HardwareBuffer> index_storage_buffers;
        std::vector<Corona::Horizon::HardwareImage> sampled_textures;
        std::vector<std::shared_ptr<const Corona::MeshMaterialDevice>> model_materials;
        std::vector<std::shared_ptr<Corona::Systems::OpticsDetail::NativeAppearance>> appearances;
        std::shared_ptr<Corona::Systems::OpticsDetail::NativeLighting> direct_lighting;
    };

    std::vector<Hardware::InstanceInfo> instances;
    std::vector<Hardware::MaterialInfo> materials;
    std::vector<std::uintptr_t> actorHandles;
    std::vector<std::uint32_t> nodeIndices;
    std::shared_ptr<ResourceKeepAlive> resource_keep_alive = std::make_shared<ResourceKeepAlive>();

    void clear() {
        instances.clear();
        materials.clear();
        actorHandles.clear();
        nodeIndices.clear();
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
        if (slot.material) resource_keep_alive->model_materials.push_back(slot.material);
        if (texture_descriptor != 0u && slot.texture) {
            resource_keep_alive->sampled_textures.push_back(slot.texture);
        }
    }
};

using ShadowSceneBounds = Corona::Systems::OpticsDetail::ShadowSceneBounds;

struct UVec2Upload {
    uint32_t x;
    uint32_t y;
};

struct FVec2Upload {
    float x;
    float y;
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

using PerfClock = std::chrono::steady_clock;

[[nodiscard]] double elapsed_ms(PerfClock::time_point start, PerfClock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void record_optics_native_perf(
    const Corona::Systems::OpticsDetail::NativePerfSample& sample) {
    if (!optics_diag_config().profile) {
        return;
    }
    static Corona::Systems::OpticsDetail::NativePerfWindow window;
    static PerfClock::time_point window_start = PerfClock::now();
    window.add(sample);

    const auto now = PerfClock::now();
    if (elapsed_ms(window_start, now) < 1000.0) {
        return;
    }
    const auto stats = window.snapshot();
    CFW_LOG_INFO(
        "OpticsNativePerf samples={} avg_total_ms={:.2f} p95_total_ms={:.2f} max_total_ms={:.2f} "
        "avg_throttle_wait_ms={:.2f} max_throttle_wait_ms={:.2f} "
        "avg_collect_ms={:.2f} avg_submit_ms={:.2f} max_submit_ms={:.2f} "
        "avg_shadow_record_ms={:.2f} max_shadow_record_ms={:.2f} "
        "avg_commit_ms={:.2f} max_commit_ms={:.2f} "
        "avg_visibility_draws={:.1f} avg_visibility_indices={:.1f} "
        "cascade_draws=[{:.1f},{:.1f},{:.1f},{:.1f}] "
        "cascade_indices=[{:.1f},{:.1f},{:.1f},{:.1f}] "
        "max_output={}x{} max_instances={} shadows={} debug={} "
        "sky_ambient={} sky_sh_updates={}",
        stats.samples,
        stats.avg_total_ms,
        stats.p95_total_ms,
        stats.max_total_ms,
        stats.avg_throttle_wait_ms,
        stats.max_throttle_wait_ms,
        stats.avg_collect_ms,
        stats.avg_submit_ms,
        stats.max_submit_ms,
        stats.avg_shadow_record_ms,
        stats.max_shadow_record_ms,
        stats.avg_commit_ms,
        stats.max_commit_ms,
        stats.avg_visibility_draws,
        stats.avg_visibility_indices,
        stats.avg_cascade_draws[0], stats.avg_cascade_draws[1],
        stats.avg_cascade_draws[2], stats.avg_cascade_draws[3],
        stats.avg_cascade_indices[0], stats.avg_cascade_indices[1],
        stats.avg_cascade_indices[2], stats.avg_cascade_indices[3],
        stats.max_output_width,
        stats.max_output_height,
        stats.max_instances,
        stats.shadow_samples,
        stats.debug_samples,
        stats.sky_ambient_samples,
        stats.sky_sh_update_samples);

    window.reset();
    window_start = now;
}

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

[[nodiscard]] FVec2Upload upload_value(const ktm::fvec2& value) {
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


[[nodiscard]] Corona::Horizon::RasterizerPipelineDesc make_visibility_pipeline_desc(bool reversed_depth = true) {
    Corona::Horizon::RasterizerPipelineDesc desc;
    // 新版 Horizon: shader 在构造 RasterizerPipeline 时传入，不在 desc 中设置
    desc.depth_test_enabled = true;
    desc.depth_write_enabled = true;
    desc.depth_compare_op = reversed_depth ? Corona::Horizon::CompareOp::GreaterOrEqual
                                          : Corona::Horizon::CompareOp::LessOrEqual;
    // 新版没有 cull_mode 字段，默认行为
    desc.blend_enabled = false;  // opaque
    desc.clear_depth_target = true;
    desc.debug_name = "corona.visibility";
    return desc;
}

[[nodiscard]] Corona::Horizon::RasterizerPipelineDesc make_shadow_pipeline_desc() {
    Corona::Horizon::RasterizerPipelineDesc desc;
    // 新版 Horizon: shader 在构造 RasterizerPipeline 时传入，不在 desc 中设置
    desc.depth_test_enabled = true;
    desc.depth_write_enabled = true;
    desc.depth_compare_op = Corona::Horizon::CompareOp::LessOrEqual;
    // 新版没有 cull_mode 字段，默认行为
    desc.clear_depth_target = true;
    desc.debug_name = "corona.solar_rsm";
    desc.blend_enabled = false; // RSM flux is the nearest solar-visible surface.
    return desc;
}
constexpr char kMouseIconRelativePath[] = "assets/icon/mouse_icon.png";
constexpr std::array<const char*, 3> kGizmoAxisRelativePaths{{
    "assets/icon/axis_x.png",
    "assets/icon/axis_y.png",
    "assets/icon/axis_z.png",
}};

struct CursorIconPixels {
    std::vector<unsigned char> rgba;
    int width = 0;
    int height = 0;
};

std::filesystem::path find_mouse_icon_path() {
    std::error_code ec;
    auto current = Corona::Utils::executable_directory();
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

std::filesystem::path find_relative_asset_path(const std::filesystem::path& relative_path) {
    std::error_code ec;
    auto current = Corona::Utils::executable_directory();
    if (!ec) {
        for (auto dir = current; !dir.empty(); dir = dir.parent_path()) {
            auto candidate = dir / relative_path;
            if (std::filesystem::exists(candidate, ec) && !ec) {
                return candidate;
            }
            ec.clear();
            if (dir == dir.parent_path()) {
                break;
            }
        }
    }
    return relative_path;
}

std::optional<CursorIconPixels> load_rgba_icon_pixels(
    const std::filesystem::path& relative_path,
    std::string_view label) {
    const auto icon_path = find_relative_asset_path(relative_path);
    const auto image_id = Corona::Resource::ResourceManager::get_instance().import_sync(icon_path);
    if (image_id == Corona::Resource::IResource::INVALID_UID) {
        CFW_LOG_WARNING("{} load failed: {}", label, icon_path.string());
        return std::nullopt;
    }
    auto image =
        Corona::Resource::ResourceManager::get_instance().acquire_read<Corona::Resource::Image>(image_id);
    if (!image || image->get_width() <= 0 || image->get_height() <= 0 ||
        image->get_data() == nullptr) {
        CFW_LOG_WARNING("{} data invalid: {}", label, icon_path.string());
        return std::nullopt;
    }

    CursorIconPixels pixels;
    pixels.width = image->get_width();
    pixels.height = image->get_height();
    const int channels = image->get_channels();
    const auto pixel_count =
        static_cast<std::size_t>(pixels.width) * static_cast<std::size_t>(pixels.height);
    pixels.rgba.resize(pixel_count * 4);
    const unsigned char* src = image->get_data();
    if (channels == 4) {
        std::copy(src, src + pixel_count * 4, pixels.rgba.begin());
    } else if (channels == 3 || channels == 1) {
        for (std::size_t i = 0; i < pixel_count; ++i) {
            const auto source = i * static_cast<std::size_t>(channels);
            pixels.rgba[i * 4 + 0] = src[source + 0];
            pixels.rgba[i * 4 + 1] = channels == 1 ? src[source] : src[source + 1];
            pixels.rgba[i * 4 + 2] = channels == 1 ? src[source] : src[source + 2];
            pixels.rgba[i * 4 + 3] = 255;
        }
    } else {
        CFW_LOG_WARNING("{} has unsupported channel count: {}", label, channels);
        return std::nullopt;
    }
    return pixels;
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

constexpr uint64_t kVisionRuntimeIdleEvictFrames = 240;

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

    CFW_LOG_INFO("Optics: applying {} camera viewport update(s)", updates.size());

    auto& camera_storage = hub.camera_storage();
    for (const auto& update : updates) {
        if (auto camera = camera_storage.acquire_write(update.camera_handle)) {
            CFW_LOG_INFO("Optics: camera viewport handle={} surface={} open={} rect={}x{}+{},{} render={}x{}",
                         update.camera_handle, update.surface, update.view_open, update.width,
                         update.height, update.x, update.y, update.render_width,
                         update.render_height);
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
        if(update.surface && !update.view_open) {
            // One embedded viewport owns a surface. Explicit floating camera
            // windows remain independent; old saved cameras must not render
            // an additional backend behind the active story viewport.
            std::vector<std::uintptr_t> obsolete;
            for(const auto& camera:std::as_const(camera_storage)) {
                const auto handle=reinterpret_cast<std::uintptr_t>(&camera);
                if(handle!=update.camera_handle && camera.surface==update.surface && !camera.view_open)
                    obsolete.push_back(handle);
            }
            for(auto handle:obsolete)if(auto camera=camera_storage.try_acquire_write(handle)) {
                if(camera->surface==update.surface && !camera->view_open){camera->surface=nullptr;camera->follows_default_surface=false;}
            }
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
                camera->view_x = update.view_x;
                camera->view_y = update.view_y;
                camera->view_width = update.view_width;
                camera->view_height = update.view_height;
                camera->move_speed = update.move_speed;
            }
        }
        // Retain the accepted preference until the camera write is committed.
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

void apply_pending_camera_releases() {
    auto& hub = Corona::SharedDataHub::instance();
    for (const auto& release : hub.drain_camera_releases()) {
        hub.clear_camera_state_updates(release.camera_handle);
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

[[nodiscard]] ktm::fmat4x4 make_orthographic_off_center_lh(float left,
                                                           float right,
                                                           float bottom,
                                                           float top,
                                                           float near_plane,
                                                           float far_plane) {
    ktm::fmat4x4 proj = ktm::fmat4x4::from_eye();
    const float width = std::max(right - left, 1e-4f);
    const float height = std::max(top - bottom, 1e-4f);
    const float depth = std::max(far_plane - near_plane, 1e-4f);

    proj[0][0] = 2.0f / width;
    proj[1][1] = -2.0f / height;
    proj[2][2] = 1.0f / depth;
    proj[3][0] = -(right + left) / width;
    proj[3][1] = (top + bottom) / height;
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

[[nodiscard]] ktm::fvec3 make_vec3(float x, float y, float z) {
    return ktm::fvec3{x, y, z};
}

[[nodiscard]] ktm::fvec4 make_vec4(const ktm::fvec3& v, float w) {
    return ktm::fvec4{v.x, v.y, v.z, w};
}

[[nodiscard]] ktm::fvec3 add_vec3(const ktm::fvec3& a, const ktm::fvec3& b) {
    return make_vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}

[[nodiscard]] ktm::fvec3 sub_vec3(const ktm::fvec3& a, const ktm::fvec3& b) {
    return make_vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

[[nodiscard]] ktm::fvec3 mul_vec3(const ktm::fvec3& v, float s) {
    return make_vec3(v.x * s, v.y * s, v.z * s);
}

[[nodiscard]] ktm::fvec3 transform_point(const ktm::fmat4x4& matrix,
                                         const ktm::fvec3& point) {
    const float x = matrix[0][0] * point.x + matrix[1][0] * point.y +
                    matrix[2][0] * point.z + matrix[3][0];
    const float y = matrix[0][1] * point.x + matrix[1][1] * point.y +
                    matrix[2][1] * point.z + matrix[3][1];
    const float z = matrix[0][2] * point.x + matrix[1][2] * point.y +
                    matrix[2][2] * point.z + matrix[3][2];
    const float w = matrix[0][3] * point.x + matrix[1][3] * point.y +
                    matrix[2][3] * point.z + matrix[3][3];
    if (std::abs(w) > 1e-5f) {
        return make_vec3(x / w, y / w, z / w);
    }
    return make_vec3(x, y, z);
}

[[nodiscard]] float lerp_float(float a, float b, float t) {
    return a + (b - a) * t;
}

[[nodiscard]] std::array<ktm::fvec3, 8> camera_frustum_corners(
    const Corona::CameraDevice& camera,
    float near_distance,
    float far_distance) {
    const ktm::fvec3 forward = ktm::normalize(camera.forward);
    ktm::fvec3 right = ktm::cross(camera.world_up, forward);
    if (ktm::length(right) < 1e-5f) {
        right = make_vec3(1.0f, 0.0f, 0.0f);
    } else {
        right = ktm::normalize(right);
    }
    const ktm::fvec3 up = ktm::normalize(ktm::cross(forward, right));

    const float tan_half_fov = std::tan(ktm::radians(camera.fov) * 0.5f);
    const float near_half_h = tan_half_fov * near_distance;
    const float near_half_w = near_half_h * camera.aspect;
    const float far_half_h = tan_half_fov * far_distance;
    const float far_half_w = far_half_h * camera.aspect;
    const ktm::fvec3 near_center = add_vec3(camera.position, mul_vec3(forward, near_distance));
    const ktm::fvec3 far_center = add_vec3(camera.position, mul_vec3(forward, far_distance));

    return {
        add_vec3(add_vec3(near_center, mul_vec3(right, -near_half_w)), mul_vec3(up, -near_half_h)),
        add_vec3(add_vec3(near_center, mul_vec3(right, near_half_w)), mul_vec3(up, -near_half_h)),
        add_vec3(add_vec3(near_center, mul_vec3(right, -near_half_w)), mul_vec3(up, near_half_h)),
        add_vec3(add_vec3(near_center, mul_vec3(right, near_half_w)), mul_vec3(up, near_half_h)),
        add_vec3(add_vec3(far_center, mul_vec3(right, -far_half_w)), mul_vec3(up, -far_half_h)),
        add_vec3(add_vec3(far_center, mul_vec3(right, far_half_w)), mul_vec3(up, -far_half_h)),
        add_vec3(add_vec3(far_center, mul_vec3(right, -far_half_w)), mul_vec3(up, far_half_h)),
        add_vec3(add_vec3(far_center, mul_vec3(right, far_half_w)), mul_vec3(up, far_half_h)),
    };
}

[[nodiscard]] bool scene_bounds_valid(
    const ktm::fvec3& scene_min_world,
    const ktm::fvec3& scene_max_world) {
    return scene_min_world.x <= scene_max_world.x &&
           scene_min_world.y <= scene_max_world.y &&
           scene_min_world.z <= scene_max_world.z;
}

[[nodiscard]] std::array<ktm::fvec3, 8> bounds_corners(
    const ktm::fvec3& min_world,
    const ktm::fvec3& max_world) {
    return {
        make_vec3(min_world.x, min_world.y, min_world.z),
        make_vec3(max_world.x, min_world.y, min_world.z),
        make_vec3(min_world.x, max_world.y, min_world.z),
        make_vec3(max_world.x, max_world.y, min_world.z),
        make_vec3(min_world.x, min_world.y, max_world.z),
        make_vec3(max_world.x, min_world.y, max_world.z),
        make_vec3(min_world.x, max_world.y, max_world.z),
        make_vec3(max_world.x, max_world.y, max_world.z),
    };
}

void include_shadow_bounds_point(ShadowSceneBounds& bounds, const ktm::fvec3& point) {
    if (!bounds.valid) {
        bounds.min_world = point;
        bounds.max_world = point;
        bounds.valid = true;
        return;
    }

    bounds.min_world.x = std::min(bounds.min_world.x, point.x);
    bounds.min_world.y = std::min(bounds.min_world.y, point.y);
    bounds.min_world.z = std::min(bounds.min_world.z, point.z);
    bounds.max_world.x = std::max(bounds.max_world.x, point.x);
    bounds.max_world.y = std::max(bounds.max_world.y, point.y);
    bounds.max_world.z = std::max(bounds.max_world.z, point.z);
}

void include_shadow_bounds(
    ShadowSceneBounds& bounds,
    const ktm::fmat4x4& model_matrix,
    const ktm::fvec3& local_min,
    const ktm::fvec3& local_max) {
    for (const auto& corner : bounds_corners(local_min, local_max)) {
        include_shadow_bounds_point(bounds, transform_point(model_matrix, corner));
    }
}

[[nodiscard]] float compute_shadow_far_plane(const Corona::CameraDevice& camera) {
    const float near_plane = std::max(camera.near_plane, 0.01f);
    return std::max(std::min(camera.far_plane, kShadowMaxDistance), near_plane + 0.01f);
}

[[nodiscard]] std::array<float, kShadowCascadeCount> compute_shadow_splits(
    const Corona::CameraDevice& camera,
    float shadow_far_plane) {
    std::array<float, kShadowCascadeCount> splits{};
    const float near_plane = std::max(camera.near_plane, 0.01f);
    const float far_plane = std::max(shadow_far_plane, near_plane + 0.01f);
    for (uint32_t i = 0; i < kShadowCascadeCount; ++i) {
        const float p = static_cast<float>(i + 1u) / static_cast<float>(kShadowCascadeCount);
        const float logarithmic = near_plane * std::pow(far_plane / near_plane, p);
        const float uniform = near_plane + (far_plane - near_plane) * p;
        splits[i] = lerp_float(uniform, logarithmic, kShadowSplitLambda);
    }
    return splits;
}

[[nodiscard]] Corona::Systems::OpticsDetail::ShadowCascadeView compute_shadow_light_view_proj(
    const Corona::CameraDevice& camera,
    const ktm::fvec3& sun_dir,
    float cascade_near,
    float cascade_far,
    const ktm::fvec3& scene_min_world,
    const ktm::fvec3& scene_max_world) {
    const auto corners = camera_frustum_corners(camera, cascade_near, cascade_far);
    ktm::fvec3 center = make_vec3(0.0f, 0.0f, 0.0f);
    for (const auto& corner : corners) {
        center = add_vec3(center, corner);
    }
    center = mul_vec3(center, 1.0f / static_cast<float>(corners.size()));

    float radius = 0.0f;
    for (const auto& corner : corners) {
        radius = std::max(radius, ktm::length(sub_vec3(corner, center)));
    }
    radius = std::max(radius, 1.0f);

    ktm::fvec3 light_forward = mul_vec3(ktm::normalize(sun_dir), -1.0f);
    ktm::fvec3 light_up = make_vec3(0.0f, 1.0f, 0.0f);
    if (std::abs(ktm::dot(light_forward, light_up)) > 0.95f) {
        light_up = make_vec3(0.0f, 0.0f, 1.0f);
    }
    const ktm::fvec3 light_position = sub_vec3(center, mul_vec3(light_forward, radius * 2.0f));
    const ktm::fmat4x4 light_view = ktm::look_to_lh(light_position, light_forward, light_up);

    ktm::fvec3 first = transform_point(light_view, corners[0]);
    float min_x = first.x;
    float max_x = first.x;
    float min_y = first.y;
    float max_y = first.y;
    float min_z = first.z;
    float max_z = first.z;
    for (const auto& corner : corners) {
        const ktm::fvec3 light_corner = transform_point(light_view, corner);
        min_x = std::min(min_x, light_corner.x);
        max_x = std::max(max_x, light_corner.x);
        min_y = std::min(min_y, light_corner.y);
        max_y = std::max(max_y, light_corner.y);
        min_z = std::min(min_z, light_corner.z);
        max_z = std::max(max_z, light_corner.z);
    }

    if (scene_bounds_valid(scene_min_world, scene_max_world)) {
        for (const auto& scene_corner : bounds_corners(scene_min_world, scene_max_world)) {
            const ktm::fvec3 light_corner = transform_point(light_view, scene_corner);
            min_z = std::min(min_z, light_corner.z);
            max_z = std::max(max_z, light_corner.z);
        }
    }

    const float xy_padding = std::max(radius * 0.02f, 0.01f);
    const float z_padding = std::max(radius * 0.25f, 0.01f);
    const float orthographic_width = std::max(max_x - min_x, 0.001f) + xy_padding * 2.0f;
    const float orthographic_height = std::max(max_y - min_y, 0.001f) + xy_padding * 2.0f;
    const ktm::fmat4x4 light_proj = make_orthographic_off_center_lh(
        min_x - xy_padding,
        max_x + xy_padding,
        min_y - xy_padding,
        max_y + xy_padding,
        min_z - z_padding,
        max_z + z_padding);
    return Corona::Systems::OpticsDetail::make_shadow_cascade_view(
        multiply_ktm_mat4(light_proj, light_view),
        orthographic_width, orthographic_height,
        static_cast<float>(kShadowMapSize));
}

[[nodiscard]] Corona::Horizon::ImageUsageFlags optics_storage_image_usage() {
    return Corona::Horizon::ImageUsage_Storage |
           Corona::Horizon::ImageUsage_ColorAttachment |
           Corona::Horizon::ImageUsage_Sampled |
           Corona::Horizon::ImageUsage_TransferSrc |
           Corona::Horizon::ImageUsage_TransferDst;
}

[[nodiscard]] Corona::Horizon::ImageUsageFlags optics_compute_image_usage() {
    return Corona::Horizon::ImageUsage_Storage |
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
    RenderInstanceBatch& batch,
    const Corona::Systems::GeometrySystem* geometry_system,
    std::uint64_t frame_index,
    std::vector<Corona::Horizon::HardwareBuffer>& out_indirect_buffers) {
    batch.clear();
    const auto& diag = optics_diag_config();

    auto& hub = Corona::SharedDataHub::instance();
    auto& actor_storage = hub.actor_storage();
    auto& profile_storage = hub.profile_storage();
    auto& optics_storage = hub.optics_storage();
    auto& geom_storage = hub.geometry_storage();
    auto& transform_storage = hub.model_transform_storage();

    bool has_instances = false;
    uint32_t recorded_draws = 0;
    uint32_t object_id = 1;
    for (auto actor_handle : scene.actor_handles) {
        auto actor = actor_storage.try_acquire_read(actor_handle);
        if (!actor) {
            ++object_id;
            continue;
        }
        if (!diag_actor_allowed(actor_handle)) {
            ++object_id;
            continue;
        }

        // ������Դ����ʱ�䣨���� ResourceManager LRU��
        {
            auto& model_res_storage = hub.model_resource_storage();
            for (auto ph : actor->profile_handles) {
                auto prof = profile_storage.try_acquire_read(ph);
                if (!prof || !prof->geometry_handle) continue;
                auto g = geom_storage.try_acquire_read(prof->geometry_handle);
                if (!g || !g->model_resource_handle) continue;
                auto mr = model_res_storage.try_acquire_read(g->model_resource_handle);
                if (mr && mr->model_id) {
                    Corona::Resource::ResourceManager::get_instance().touch(mr->model_id);
                }
                break;
            }
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
            if (!diag_geometry_allowed(optics.geometry_handle)) {
                continue;
            }
            // ---- ��ȡ���α任��Ϣ���������ɣ�texture �� query_mesh_slots �ڲ������----
            std::uintptr_t transform_handle_a = 0;
            float          nlc_scale_a   = 1.0f;
            ktm::fvec3     nlc_offset_a  = {0.0f, 0.0f, 0.0f};
            if (auto geom_r = geom_storage.try_acquire_read(optics.geometry_handle)) {
                transform_handle_a = geom_r->transform_handle;
                nlc_scale_a        = geom_r->native_local_correction_scale;
                nlc_offset_a       = geom_r->native_local_correction_offset;
            }

            ktm::fmat4x4 model_matrix{ktm::fmat4x4::from_eye()};
            if (auto transform = transform_storage.try_acquire_read(transform_handle_a)) {
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
                if (camera_basis != nullptr) {
                    model_matrix = multiply_ktm_mat4(*camera_basis, model_matrix);
                }
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
                {
                    batch.materials.push_back(Corona::Systems::OpticsDetail::pack_native_material(
                        ms, optics, texture_descriptor, diag.disable_textures || diag.force_solid_material,
                        optics.bEnableLighting && !diag.force_solid_material));
                }
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
                    batch.actorHandles.push_back(actor_handle);
                    batch.nodeIndices.push_back(ms.node_index);
                    has_instances = true;
                }

                using Pipeline = Corona::Horizon::RasterizerPipeline<visibility_vert_glsl_t, visibility_frag_glsl_t>;
                auto& pc = static_cast<Pipeline::VertexResourceBindings&>(target_visibility).pushConsts;
                pc.modelMatrix = upload_value(mesh_model_matrix);
                pc.uniformBufferIndex = target_vp_descriptor;
                pc.instanceID = instance_id + 1;
                pc.textureIndex = texture_descriptor;

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
                            follow_camera_pass ? "ui_visibility" : "visibility",
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
    const std::shared_ptr<Corona::Systems::Vision::VisionSceneResource>& scene_resource = {})
    -> ocarina::SP<vision::Pipeline> {
    auto project_desc = make_default_vision_project_desc();
    project_desc.renderer_desc.integrator_desc.sub_type =
        mode == Corona::CameraVisionRenderMode::ReSTIR ? "rt" : "pt";
    if (mode == Corona::CameraVisionRenderMode::ReSTIR) {
        auto data = vision::DataWrap::object();
        Corona::Systems::Vision::configure_vision_scene_for_mode(data, mode);
        project_desc.renderer_desc.integrator_desc.init(
            vision::ParameterSet{data["render"]["integrator"]});
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
    for (const auto& scene : std::as_const(hub.scene_storage())) {
        if (!scene.enabled) continue;
        auto camera = hub.camera_storage().try_acquire_write(select_scene_camera_handle(scene));
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
    for (const auto& view : views) selection.add(view.camera_handle, view.vision_mode);
    return selection;
}

[[nodiscard]] std::optional<Corona::CameraDevice> active_scene_camera_values() {
    auto& hub = Corona::SharedDataHub::instance();
    for (const auto& scene : std::as_const(hub.scene_storage())) {
        if (!scene.enabled) continue;
        const auto handle = select_scene_camera_handle(scene);
        if (auto camera = hub.camera_storage().try_acquire_read(handle)) return *camera;
    }
    return std::nullopt;
}

[[nodiscard]] Corona::EnvironmentDevice vision_environment_for_scene(const Corona::SceneDevice& scene) {
    if (scene.environment != 0) {
        if (auto env = Corona::SharedDataHub::instance().environment_storage().try_acquire_read(scene.environment)) {
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
    for (const auto& scene : std::as_const(Corona::SharedDataHub::instance().scene_storage())) {
        if (scene.enabled) return vision_environment_for_scene(scene);
    }
    return vision_environment_for_scene(Corona::SceneDevice{});
}

// The Vision path tracer keeps an acceleration structure and material/device state
// in addition to the native renderer's resources.  It is intended for previewing
// modest scenes; expanding a highly-instanced glTF scene here can exhaust memory
// and starve the editor frame loop.  Native rendering still supports picking and
// model-node editing, so it is the safe interactive fallback above this limit.
constexpr std::size_t kVisionInteractiveInstanceLimit = 1024;

void fallback_large_scenes_to_native_renderer() {
    auto& hub = Corona::SharedDataHub::instance();
    auto& actor_storage = hub.actor_storage();
    auto& profile_storage = hub.profile_storage();
    auto& optics_storage = hub.optics_storage();
    auto& geometry_storage = hub.geometry_storage();
    auto& camera_storage = hub.camera_storage();

    for (const auto& scene : std::as_const(hub.scene_storage())) {
        if (!scene.enabled) continue;
        const bool has_vision_camera = std::any_of(scene.camera_handles.begin(), scene.camera_handles.end(),
            [&](auto handle) {
                const auto camera = camera_storage.try_acquire_read(handle);
                return camera && camera->render_backend == Corona::CameraRenderBackend::Vision;
            });
        if (!has_vision_camera) continue;

        std::size_t instance_count = 0;
        for (const auto actor_handle : scene.actor_handles) {
            const auto actor = actor_storage.try_acquire_read(actor_handle);
            if (!actor) continue;
            for (const auto profile_handle : actor->profile_handles) {
                const auto profile = profile_storage.try_acquire_read(profile_handle);
            if (!profile || profile->optics_handle == 0) continue;
                const auto optics = optics_storage.try_acquire_read(profile->optics_handle);
                const auto geometry = optics ? geometry_storage.try_acquire_read(optics->geometry_handle)
                                             : decltype(geometry_storage.try_acquire_read(0)){};
                if (!optics || !optics->visible || !geometry) continue;
                if (!geometry->mesh_instances.empty()) {
                    instance_count += geometry->mesh_instances.size();
                } else if (!geometry->mesh_handles.empty()) {
                    instance_count += geometry->mesh_handles.size();
                } else if (auto model = hub.model_resource_storage().try_acquire_read(geometry->model_resource_handle)) {
                    if (auto resource = Corona::Resource::ResourceManager::get_instance()
                            .acquire_read<Corona::Resource::Scene>(model->model_id)) {
                        instance_count += resource->data.mesh_instances.empty()
                            ? resource->data.meshes.size() : resource->data.mesh_instances.size();
                    }
                }
                if (instance_count > kVisionInteractiveInstanceLimit) break;
            }
            if (instance_count > kVisionInteractiveInstanceLimit) break;
        }
        if (instance_count <= kVisionInteractiveInstanceLimit) continue;

        for (const auto camera_handle : scene.camera_handles) {
            const auto camera = camera_storage.try_acquire_write(camera_handle);
            if (!camera || camera->render_backend != Corona::CameraRenderBackend::Vision) continue;
            camera->render_backend = Corona::CameraRenderBackend::Native;
            CFW_LOG_WARNING(
                "OpticsSystem: switched camera {} to Native rendering because scene has more than {} instances (observed={})",
                camera_handle, kVisionInteractiveInstanceLimit, instance_count);
        }
    }
}

[[nodiscard]] VisionModeSelection select_visible_vision_render_mode(
    const std::function<bool(std::uintptr_t)>& pending_capture = {}) {
    VisionModeSelection selection;

    for (const auto& scene : std::as_const(Corona::SharedDataHub::instance().scene_storage())) {
        if (!scene.enabled) continue;
        for (const auto camera_handle : scene.camera_handles) {
            auto camera =
                Corona::SharedDataHub::instance().camera_storage().try_acquire_read(camera_handle);
            if (!camera || camera->render_backend != Corona::CameraRenderBackend::Vision ||
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
    return std::string(Corona::Systems::Vision::vision_render_mode_name(key.mode));
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
    vision::switch_profile::Scope profile{"scene.bind_gpu_resource", "geometry"};
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
using RenderDetail::RetainSubmission;
using RenderDetail::bind_storage_image;
using RenderDetail::bind_sampled_image;
using RenderDetail::bind_storage_buffer;

namespace {
// A cold native capture may first build solar RSMs and compile shading pipelines.
constexpr auto kScreenshotRequestTimeout = std::chrono::seconds(30);
}

struct OpticsSystem::NativeViewResources {
    Horizon::HardwareImage visibility;
    Horizon::HardwareImage depth;
    Horizon::HardwareImage opaque_visibility, opaque_depth, exit_visibility, exit_depth, opaque_color, front_snapshot;
    Memory::GpuMemToken refraction_memory;
    Horizon::HardwareImage ssgi_correction, ssgi_filtered;
    std::array<Horizon::HardwareImage,2> ssgi_history, ssgi_validation;
    Horizon::HardwareBuffer screen_depth;
    OpticsDetail::ScreenSpaceLayout screen_layout{};
    Memory::GpuMemToken ssgi_memory;
    ktm::fmat4x4 previous_view_projection{}, previous_view{};
    uint32_t screen_frame=0;
    bool screen_history_valid=false;
    bool screen_had_refraction=false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t last_used_frame = 0;
};

// per-camera UI overlay �м���Native �� Vision ���ã���һ������Դ����
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
    Vision::VisionGeometrySyncDebounce geometry_debounce;
    std::optional<Vision::VisionLightingState> engine_lighting;
    std::chrono::steady_clock::time_point next_geometry_retry{};

    // Zero-copy path: shares Vision's pre-tonemap linear color buffer with Vulkan
    // and resolves it via the vision_resolve compute pass.
    std::unordered_map<std::uintptr_t, std::unique_ptr<Vision::VisionZeroCopyBridge>> bridges;
    std::unordered_map<std::uintptr_t, std::unique_ptr<Vision::VisionDiagnosticAdapter>> diagnostics;
    std::unordered_set<std::uintptr_t> retained_contexts;
    std::unordered_map<std::uintptr_t, bool> view_denoise_states;
    std::unordered_map<std::uintptr_t, CameraVisionDenoiser> view_denoiser_types;
    std::unordered_map<std::uintptr_t, Horizon::SubmitReceipt> interop_submissions;
    Horizon::HardwareExecutor* interop_executor{nullptr};

    void wait_for_interop_submission(std::uintptr_t camera_handle,
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
            [&](std::uintptr_t camera_handle, const Horizon::SubmitReceipt& receipt) {
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
        diagnostics.clear();
        bridges.clear();
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
                                            std::uintptr_t camera_handle,
                                            uint32_t width, uint32_t height,
                                            bool denoise, bool accumulation, bool stable_planes,
                                            CameraVisionDenoiser denoiser,
                                            const std::string& reblur_settings,
                                            const std::string& relax_settings,
                                            const std::string& pt_settings,
                                            const std::string& restir_settings,
                                            VisionCameraFeedback* deferred_feedback) {
    vision::switch_profile::Scope profile{"view.prepare", "view"};
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
        // Enable toggles reuse PT/ReSTIR kernels. Changing the denoiser type
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
            runtime.diagnostics.erase(camera_handle);
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
        const auto &json = is_restir ? restir_settings : pt_settings;
        const auto overrides = vision::DataWrap::parse(json, nullptr, false);
        const bool changed = integrator->set_settings(overrides);
        if (changed) pipeline->invalidate();
        if (is_restir) {
            if (Vision::validate_restir_settings(overrides).empty()) {
                feedback.restir_effective = integrator->settings().dump();
                feedback.restir_applied = json;
            }
        } else if (view_created || changed) {
            feedback.pt_effective = integrator->settings().dump();
        }
    }
    if (auto* illumination = dynamic_cast<vision::IlluminationIntegrator*>(pipeline->renderer().integrator().get())) {
        if (auto* filter = illumination->denoiser(); filter && filter->set_reblur_settings(reblur_settings)) {
            pipeline->invalidate();
            feedback.reblur_effective = filter->reblur_settings_json();
        }
        if (auto* filter = illumination->denoiser(); filter && filter->impl_type() == "ReLAX") {
            const bool changed = filter->set_relax_settings(relax_settings);
            if (changed) pipeline->invalidate();
            if (view_created || changed) {
                feedback.relax_effective = filter->relax_settings_json();
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

void OpticsSystem::publish_vision_camera_feedback(std::uintptr_t camera_handle,
                                                 const VisionCameraFeedback& feedback) {
    if (!feedback.restir_effective && !feedback.pt_effective &&
        !feedback.reblur_effective && !feedback.relax_effective) return;
    auto camera = SharedDataHub::instance().camera_storage().try_acquire_write(camera_handle);
    if (!camera) return;
    if (feedback.restir_effective) camera->vision_restir_effective_settings = *feedback.restir_effective;
    if (feedback.restir_applied) camera->vision_restir_applied_settings = *feedback.restir_applied;
    if (feedback.pt_effective) camera->vision_pt_effective_settings = *feedback.pt_effective;
    if (feedback.reblur_effective) camera->vision_reblur_effective_settings = *feedback.reblur_effective;
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
        it->second->scene_resource = get_or_create_vision_scene_resource();
        it->second->mode = key.mode;
        CFW_LOG_INFO("OpticsSystem: created Vision runtime ({})",
                     describe_vision_pipeline_key(key));
    } else if (!it->second->scene_resource) {
        it->second->scene_resource = get_or_create_vision_scene_resource();
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
                auto camera = SharedDataHub::instance().camera_storage().try_acquire_read(view.first);
                return camera && camera->surface != nullptr &&
                       camera->render_backend == CameraRenderBackend::Vision;
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

bool OpticsSystem::initialize_vision_backend_if_enabled() {
    // Vision backend is lazily initialized on first switch to Vision mode.
    return true;
}

bool OpticsSystem::initialize_hardware_resources() {
    try {
        hardware_ = std::make_unique<Hardware>();

        hardware_->gbufferSize.x = 1920;
        hardware_->gbufferSize.y = 1080;

        // --- Uniform buffers ---
        hardware_->uniformBuffer =
            make_storage_buffer<Hardware::UniformBufferObject>(1, "optics.uniform");
        hardware_->vpUniformBuffer =
            make_storage_buffer<Hardware::VPUniformBufferObject>(1, "optics.vp_uniform");
        hardware_->uiVpUniformBuffer =
            make_storage_buffer<Hardware::VPUniformBufferObject>(1, "optics.ui_vp_uniform");

        // --- Instance & Material table buffers ---
        hardware_->instanceInfoCapacity = kInitialInstanceTableCapacity;
        hardware_->uiInstanceInfoCapacity = kInitialInstanceTableCapacity;
        hardware_->materialTableCapacity = kInitialMaterialTableCapacity;
        hardware_->uiMaterialTableCapacity = kInitialMaterialTableCapacity;
        hardware_->instanceInfoBuffer =
            make_storage_buffer<Hardware::InstanceInfo>(hardware_->instanceInfoCapacity,
                                                        "optics.instances");
        hardware_->uiInstanceInfoBuffer =
            make_storage_buffer<Hardware::InstanceInfo>(hardware_->uiInstanceInfoCapacity,
                                                        "optics.ui_instances");
        hardware_->materialTableBuffer =
            make_storage_buffer<Hardware::MaterialInfo>(hardware_->materialTableCapacity,
                                                        "optics.materials");
        hardware_->uiMaterialTableBuffer =
            make_storage_buffer<Hardware::MaterialInfo>(hardware_->uiMaterialTableCapacity,
                                                        "optics.ui_materials");
        hardware_->shadowInfoBuffer =
            make_storage_buffer<Hardware::ShadowInfoBufferObject>(1, "optics.shadow_info");

        // Sky-driven ambient: 9 SH coefficients �� vec3 = 27 floats. Written by
        // sky_sh_project.comp each environment change, read by lighting.comp.
        hardware_->skyIrradianceSHBuffer =
            make_storage_buffer<float>(27, "optics.sky_irradiance_sh");

        // finalOutputImage �����ڴ˴�����ÿ�� surface �����������
        // acquire_surface_target() ���贴��������1: per-surface �������
    } catch (const std::exception&) {
        CFW_LOG_CRITICAL("OpticsSystem: Failed to initialize hardware resources");
        return false;
    }

    return true;
}

bool OpticsSystem::initialize_render_pipelines() {
    try {
        // 新版 Horizon: RasterizerPipeline 构造需要 shader 类型（非实例）+ 描述符
        hardware_->visibilityPipeline.emplace(
            visibility_vert_glsl_t{},
            visibility_frag_glsl_t{},
            make_visibility_pipeline_desc());
        hardware_->uiVisibilityPipeline.emplace(
            visibility_vert_glsl_t{},
            visibility_frag_glsl_t{},
            make_visibility_pipeline_desc(false));
        hardware_->actorPickPipeline.emplace(actor_pick_comp_glsl, ktm::uvec3(1, 1, 1));
        hardware_->screenshotReadbackPipeline.emplace(image_readback_comp_glsl, ktm::uvec3(8, 8, 1));
        hardware_->opticsOverlayPipeline.emplace(optics_overlay_comp_glsl, ktm::uvec3(8, 8, 1));
        hardware_->opticsGizmoPipeline.emplace(optics_gizmo_comp_glsl, ktm::uvec3(8, 8, 1));
        hardware_->opticsCursorPipeline.emplace(optics_cursor_comp_glsl, ktm::uvec3(8, 8, 1));
        hardware_->opticsUiWarpPipeline.emplace(optics_ui_warp_comp_glsl, ktm::uvec3(8, 8, 1));
        hardware_->opticsCompositePipeline.emplace(optics_composite_comp_glsl, ktm::uvec3(8, 8, 1));
        hardware_->visionResolvePipeline.emplace(vision_resolve_comp_glsl, ktm::uvec3(8, 8, 1));
        hardware_->shaderHasInit = true;
    } catch (const std::exception& e) {
        CFW_LOG_CRITICAL("OpticsSystem: Failed to initialize typed pipelines: {}", e.what());
        return false;
    }

    return true;
}

bool OpticsSystem::ensure_native_render_resources() {
    if (hardware_->native_resources_ready) return true;
    try {
        for (uint32_t i = 0; i < kShadowCascadeCount; ++i) {
            hardware_->shadowCascadeImages[i] =
                make_depth_image(kShadowMapSize, kShadowMapSize,
                                 "optics.shadow_cascade_" + std::to_string(i));
            hardware_->rsmPositions[i]=make_storage_image(kShadowMapSize,kShadowMapSize,Horizon::Format::RGBA32_FLOAT,"optics.rsm.position");
            hardware_->rsmNormals[i]=make_storage_image(kShadowMapSize,kShadowMapSize,Horizon::Format::RGBA16_FLOAT,"optics.rsm.normal");
            hardware_->rsmFluxes[i]=make_storage_image(kShadowMapSize,kShadowMapSize,Horizon::Format::RGBA16_FLOAT,"optics.rsm.flux");
            hardware_->rsmPositions[i].set_clear_color(0,0,0,0);
            hardware_->rsmNormals[i].set_clear_color(0,0,0,0);
            hardware_->rsmFluxes[i].set_clear_color(0,0,0,0);
        }

        hardware_->rsmMemory=Memory::GpuMemToken(Memory::ResKind::Texture,
            kShadowCascadeCount*uint64_t(kShadowMapSize)*kShadowMapSize*32);


        hardware_->refractionOpaquePipeline.emplace(visibility_vert_glsl_t{},visibility_frag_glsl_t{},make_visibility_pipeline_desc());
        hardware_->refractionExitPipeline.emplace(visibility_vert_glsl_t{},visibility_frag_glsl_t{},make_visibility_pipeline_desc(false));
        hardware_->refractionFrontPipeline.emplace(refraction_front_comp_glsl,ktm::uvec3(16,16,1));
        const auto shadow_desc = make_shadow_pipeline_desc();
        for (auto& shadow_pipeline : hardware_->shadowPipelines) {
            shadow_pipeline.emplace(
                shadow_vert_glsl_t{},
                shadow_frag_glsl_t{},
                shadow_desc);
        }
        hardware_->lightingPipeline.emplace(lighting_comp_glsl, ktm::uvec3(8, 8, 1));
        hardware_->screenGiPipeline.emplace(screen_gi_comp_glsl, ktm::uvec3(8,8,1));
        hardware_->screenDepthPipeline.emplace(screen_depth_comp_glsl, ktm::uvec3(8,8,1));
        hardware_->screenFilterPipeline.emplace(screen_filter_comp_glsl, ktm::uvec3(8,8,1));
        hardware_->skyPipeline.emplace(sky_comp_glsl, ktm::uvec3(8, 8, 1));
        hardware_->skySHProjectPipeline.emplace(sky_sh_project_comp_glsl, ktm::uvec3(64, 1, 1));
        hardware_->tonemapPipeline.emplace(tonemap_comp_glsl, ktm::uvec3(8, 8, 1));
        hardware_->native_resources_ready = true;
        CFW_LOG_INFO("NativeResourceLifecycle allocated: RSM payload=128 MiB, CSM depth=16 MiB, LPV=0 MiB");
        return true;
    } catch (const std::exception& error) {
        CFW_LOG_ERROR("OpticsSystem: native resource allocation failed: {}", error.what());
        // Include partial allocations in the same teardown path.
        hardware_->native_resources_ready = true;
        release_native_render_resources();
        return false;
    }
}

void OpticsSystem::release_native_render_resources() {
    if (!hardware_ || !hardware_->native_resources_ready) return;
    // Called on the Optics thread, outside scene/camera locks and before Vision
    // initialization. Wait only for our submissions; shared output/UI survive.
    hardware_->native_frame_throttle.drain([this](const Horizon::SubmitReceipt& receipt) {
        hardware_->executor.wait_completed(receipt);
    });
    if (hardware_->last_receipt.serial != 0)
        hardware_->executor.wait_completed(hardware_->last_receipt);

    // Pipelines retain bound images and buffers. Erasing the view map alone
    // does not release those resources, even after all GPU work has completed.
    hardware_->visibilityPipeline.reset();
    hardware_->actorPickPipeline.reset();
    hardware_->refractionOpaquePipeline.reset();
    hardware_->refractionExitPipeline.reset();
    hardware_->refractionFrontPipeline.reset();
    for (auto& pipeline : hardware_->shadowPipelines) pipeline.reset();
    hardware_->lightingPipeline.reset();
    hardware_->screenGiPipeline.reset();
    hardware_->screenDepthPipeline.reset();
    hardware_->screenFilterPipeline.reset();
    hardware_->skyPipeline.reset();
    hardware_->skySHProjectPipeline.reset();
    hardware_->tonemapPipeline.reset();
    hardware_->visibilityImage = {};
    hardware_->depthImage = {};
    native_view_resources_.clear();
    hardware_->appearance_cache = {};
    hardware_->lighting_cache = {};
    hardware_->lightFieldCache.invalidate();
    hardware_->shadowCascadeImages = {};
    hardware_->rsmPositions = {};
    hardware_->rsmNormals = {};
    hardware_->rsmFluxes = {};
    hardware_->rsmMemory = {};
    hardware_->vpUniformBufferPool = {};
    hardware_->uniformBufferPool = {};
    hardware_->shadowInfoBufferPool = {};
    hardware_->instanceInfoBufferPool = {};
    hardware_->materialTableBufferPool = {};
    hardware_->shadowInfoBufferObjects = {};
    sky_sh_initialized_ = false;
    sky_sh_signature_ = 0;
    hardware_->native_resources_ready = false;
    // Picking shares visibility shaders, but needs no Native lighting resources.
    hardware_->visibilityPipeline.emplace(visibility_vert_glsl_t{}, visibility_frag_glsl_t{},
                                          make_visibility_pipeline_desc());
    hardware_->actorPickPipeline.emplace(actor_pick_comp_glsl, ktm::uvec3(1, 1, 1));
    CFW_LOG_INFO("NativeResourceLifecycle released: lighting, material, view resources and bindings");
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
        Horizon::ImageUsage_Sampled | Horizon::ImageUsage_TransferDst,
        "optics.cursor_icon"));
    if (!icon) {
        CFW_LOG_WARNING("Optics cursor icon GPU image creation failed");
        return false;
    }

    Horizon::HardwareExecutor executor;
    const auto upload_pixels = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(pixels->rgba.data()),
        pixels->rgba.size());
    // Horizon �Ƴ��� HardwareImage::upload()����Ϊ staging buffer + copy_from()��
    Horizon::HardwareBufferDesc staging_desc;
    staging_desc.element_count = upload_pixels.size_bytes();
    staging_desc.element_size = 1;
    staging_desc.usage = Horizon::BufferUsage_TransferSrc;
    staging_desc.cpu_access = Horizon::CpuAccessMode::Write;
    const Horizon::HardwareBuffer staging(staging_desc, upload_pixels);

    const Horizon::SubmitReceipt upload_receipt =
        executor.stream()
            << icon.copy_from(staging)
            << Horizon::commit();
    executor.wait_idle(upload_receipt);
    hardware_->cursorIconImage = std::move(icon);
    CFW_LOG_INFO("Optics cursor icon uploaded ({}x{})", pixels->width, pixels->height);
    return true;
}

bool OpticsSystem::ensure_gizmo_axis_textures() {
    if (!hardware_) {
        return false;
    }
    if (std::all_of(hardware_->gizmoAxisImages.begin(),
                    hardware_->gizmoAxisImages.end(),
                    [](const Horizon::HardwareImage& image) {
                        return static_cast<bool>(image);
                    })) {
        return true;
    }
    if (hardware_->gizmoAxisLoadAttempted) {
        return false;
    }
    hardware_->gizmoAxisLoadAttempted = true;

    std::array<CursorIconPixels, 3> pixels;
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        auto loaded = load_rgba_icon_pixels(kGizmoAxisRelativePaths[i], "Optics gizmo axis");
        if (!loaded) {
            return false;
        }
        pixels[i] = std::move(*loaded);
    }

    Horizon::HardwareExecutor executor;
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        Horizon::HardwareImage image(Horizon::HardwareImageDesc::texture_2d(
            static_cast<std::uint32_t>(pixels[i].width),
            static_cast<std::uint32_t>(pixels[i].height),
            Horizon::Format::SRGBA8_UNORM,
            Horizon::ImageUsage_Sampled | Horizon::ImageUsage_TransferDst,
            i == 0 ? "optics.gizmo_axis_x"
                   : (i == 1 ? "optics.gizmo_axis_y" : "optics.gizmo_axis_z")));
        if (!image) {
            CFW_LOG_WARNING("Optics gizmo axis GPU image creation failed at {}", i);
            return false;
        }
        const auto bytes = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(pixels[i].rgba.data()),
            pixels[i].rgba.size());

        // Use staging buffer + copy_from() like cursor icon (new Horizon API)
        Horizon::HardwareBufferDesc staging_desc;
        staging_desc.element_count = bytes.size_bytes();
        staging_desc.element_size = 1;
        staging_desc.usage = Horizon::BufferUsage_TransferSrc;
        staging_desc.cpu_access = Horizon::CpuAccessMode::Write;
        const Horizon::HardwareBuffer staging(staging_desc, bytes);

        const auto receipt = executor.stream()
            << image.copy_from(staging)
            << Horizon::commit();
        executor.wait_idle(receipt);
        hardware_->gizmoAxisImages[i] = std::move(image);
    }
    CFW_LOG_INFO("Optics gizmo axis textures uploaded");
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
        !resources.visibility || !resources.depth) {
        if (hardware_->last_receipt.serial != 0) {
            hardware_->executor.wait_idle(hardware_->last_receipt);
        }
        resources.visibility =
            make_storage_image(width, height, Horizon::Format::RG32_UINT, "optics.native_visibility");
        resources.depth = make_depth_image(width, height, "optics.native_depth");
        resources.depth.set_clear_depth(0.0f, 0);
        resources.opaque_visibility={};resources.opaque_depth={};resources.exit_visibility={};
        resources.exit_depth={};resources.opaque_color={};resources.refraction_memory={};
        resources.front_snapshot={};
        resources.ssgi_correction={};resources.ssgi_filtered={};resources.ssgi_history={};resources.ssgi_validation={};
        resources.screen_depth={};resources.ssgi_memory={};resources.screen_history_valid=false;resources.screen_frame=0;
        resources.width = width;
        resources.height = height;
    }
    resources.last_used_frame = frame_index;

    hardware_->gbufferSize.x = width;
    hardware_->gbufferSize.y = height;
    hardware_->visibilityImage = resources.visibility;
    hardware_->depthImage = resources.depth;
    // Draw recording snapshots attachments and resources into each submission.
    // Cameras need distinct images, but share the same immutable pipeline state.
    // Keeping it alive also avoids Horizon's device-wide idle on pipeline
    // destruction racing the display queue when a camera closes or is evicted.
    hardware_->visibilityPipeline->visibilityData = resources.visibility;
    hardware_->visibilityPipeline->bind_depth_target(resources.depth);

    // UI visibility/depth �ɹ��� helper ͳһ���䲢�󶨣�Native �� Vision ͬԴ����

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

    // ���޸� gbufferSize�����÷����𣩡����󶨹�������������� UI ͼ��
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

    // Allocate one ImageStorage slot when this surface first needs an output.
    if (target.image_handle == 0) {
        target.image_handle = SharedDataHub::instance().image_storage().allocate();
        target.published_image = Detail::PublishedImage(target.image_handle);
        // Clear any image and GPU receipts retained from a previous allocation.
        if (auto accessor =
                SharedDataHub::instance().image_storage().acquire_write(target.image_handle)) {
            *accessor = ImageDevice{};
        }
    }

    // Create or rebuild this surface's output images when its resolution changes.
    if (!target.final_output ||
        target.width != width || target.height != height) {
        if (target.image_handle != 0) {
            if (auto image_device =
                    SharedDataHub::instance().image_storage().acquire_write(target.image_handle)) {
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
    std::uintptr_t camera_handle,
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
    auto& storage = SharedDataHub::instance().image_storage();
    {
        auto image = storage.acquire_write(target.image_handle);
        if (image->submit_receipt.serial != 0) {
            hardware_->executor.wait_idle(image->submit_receipt);
        }
        if (image->consumed_receipt.serial != 0) {
            hardware_->executor.wait_idle(image->consumed_receipt);
        }
        *image = ImageDevice{};
    }
    storage.deallocate(target.image_handle);
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

    if (!initialize_vision_backend_if_enabled()) {
        return false;
    }

    if (!initialize_hardware_resources()) {
        return false;
    }

    if (!initialize_render_pipelines()) {
        return false;
    }

    try {
        actor_pick_readback_worker_ =
            std::make_unique<OpticsDetail::ActorPickReadbackWorker>(64);
        actor_pick_readback_worker_->start();
    } catch (const std::exception& error) {
        CFW_LOG_CRITICAL("OpticsSystem: Failed to start actor pick readback worker: {}",
                         error.what());
        actor_pick_readback_worker_.reset();
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
                const auto expires_at =
                    event.deadline == std::chrono::steady_clock::time_point{}
                        ? std::chrono::steady_clock::now() + kScreenshotRequestTimeout : event.deadline;
                std::lock_guard<std::mutex> lock(screenshot_mutex_);
                incoming_screenshots_.push_back(
                    {event.camera_handle,
                     event.file_path,
                     event.completion_promise,
                     expires_at});
            });

        backend_switch_sub_id_ = event_bus->subscribe<Events::RenderBackendSwitchEvent>(
            [this](const Events::RenderBackendSwitchEvent& event) {
                if (event.camera_handle == 0) {
                    return;
                }
                CameraStateUpdateCommand command{};
                command.camera_handle = event.camera_handle;
                command.fields = CameraStateUpdateField::RenderBackend;
                command.render_backend =
                    event.backend == static_cast<int>(RenderBackend::Vision)
                        ? CameraRenderBackend::Vision
                        : CameraRenderBackend::Native;
                SharedDataHub::instance().enqueue_camera_state_update(command);
            });

        residency_sub_id_ = event_bus->subscribe<Events::ActorResidencyChangedEvent>(
            [this](const Events::ActorResidencyChangedEvent& e) {
                std::unique_lock lock(residency_mtx_);
                if (e.loaded) {
                    resident_actors_.insert(e.actor);
                } else {
                    resident_actors_.erase(e.actor);
                }
            });
        native_frame_consumed_sub_id_ =
            event_bus->subscribe<Events::OpticsFrameConsumedEvent>(
                [this](const Events::OpticsFrameConsumedEvent& event) {
                    if (event.submit_serial == 0) return;
                    auto observed = pending_native_consumed_serial_.load(std::memory_order_relaxed);
                    while (observed < event.submit_serial &&
                           !pending_native_consumed_serial_.compare_exchange_weak(
                               observed, event.submit_serial,
                               std::memory_order_release,
                               std::memory_order_relaxed)) {
                    }
                });
    }

    return true;
}

bool OpticsSystem::ensure_native_frame_slot() {
    // A saturated throttle is normal: Optics records faster than the display
    // presents, so the throttle is what keeps it from running ahead. In that case
    // the right answer is still "skip this frame" (the display acknowledges within
    // a frame or two, and its acknowledgement releases every older entry too).
    //
    // What must never happen is a throttle that stays full because *nothing*
    // acknowledges it: Display only publishes OpticsFrameConsumedEvent for surfaces
    // it actually presented with an optics layer, so a removed/failed/lost surface
    // drops its acknowledgement. Two dropped acknowledgements then stopped native
    // rendering for every camera, silently, until the engine was restarted. Past
    // this grace period Optics therefore takes over and retires the oldest
    // submission by waiting for its own GPU completion.
    constexpr std::uint64_t kNativeThrottleStuckMs = 500;
    // Retiring a submission that the display never consumed should be immediate;
    // anything slower is reported.
    constexpr double kNativeThrottleSlowRetireMs = 100.0;

    if (!hardware_) {
        return true;
    }
    auto& throttle = hardware_->native_frame_throttle;
    if (throttle.try_acquire_slot()) {
        return true;
    }

    throttle.note_saturated_frame();
    const auto now = PerfClock::now();
    const auto oldest_age_ms = throttle.oldest_age_ms(now);
    if (oldest_age_ms < kNativeThrottleStuckMs) {
        // Display is simply behind: drop this frame, as the throttle intends.
        return false;
    }

    const bool report_due = last_native_throttle_report_.time_since_epoch().count() == 0 ||
                            now - last_native_throttle_report_ >= std::chrono::seconds(1);
    if (report_due) {
        last_native_throttle_report_ = now;
        CFW_LOG_WARNING(
            "[OpticsThrottle] stuck for {}ms: in_flight={} oldest_serial={} "
            "saturated_frames={} retired_without_ack={} forgotten={} — no consumer "
            "acknowledged these frames; retiring the oldest by GPU completion",
            oldest_age_ms, throttle.in_flight_count(), throttle.oldest_serial(),
            throttle.saturated_frames(), throttle.released_without_ack(),
            throttle.forgotten_for_capacity());
    }

    const auto wait_start = PerfClock::now();
    const auto retired_serial = throttle.oldest_serial();
    const bool retired = throttle.retire_oldest([this](const Horizon::SubmitReceipt& receipt) {
        hardware_->executor.wait_completed(receipt);
    });
    const auto wait_ms = elapsed_ms(wait_start, PerfClock::now());
    pending_native_throttle_wait_ms_ += wait_ms;
    if (retired && wait_ms >= kNativeThrottleSlowRetireMs) {
        CFW_LOG_WARNING(
            "[OpticsThrottle] retiring serial={} cost {:.1f}ms (nothing acknowledged it; "
            "the GPU queue is behind)",
            retired_serial, wait_ms);
    }
    return throttle.try_acquire_slot();
}

void OpticsSystem::update() {
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
    if (hardware_) {
        const auto consumed_serial =
            pending_native_consumed_serial_.exchange(0, std::memory_order_acq_rel);
        if (consumed_serial != 0) {
            hardware_->native_frame_throttle.acknowledge_consumed(consumed_serial);
        }
        // Native backpressure belongs to native view submission only. It must
        // not delay camera commands (including backend switches) or Vision.
    }
    update_phase.set("camera_commands");
    // Setters enqueue camera changes before publishing their screenshot request.
    // Promote requests first, then drain those changes. Requests arriving later
    // must wait for the next frame instead of capturing this frame's old state.
    begin_screenshot_frame();
    apply_pending_camera_moves();
    // Explicit camera viewport/state commands below take precedence over the
    // default surface size, matching the previous follows_default_surface rule.
    apply_pending_default_surface_camera_update();
    apply_pending_camera_viewport_updates();
    apply_pending_camera_state_updates();
    apply_pending_camera_releases();

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

    fallback_large_scenes_to_native_renderer();
    std::vector<std::uintptr_t> requested_vision_cameras;
    bool native_view_requested = false;
    for (const auto& scene : std::as_const(SharedDataHub::instance().scene_storage())) {
        if (!scene.enabled) {
            continue;
        }
        for (const auto camera_handle : scene.camera_handles) {
            if (auto camera =
                    SharedDataHub::instance().camera_storage().try_acquire_read(camera_handle);
                camera) {
                if (camera->render_backend == CameraRenderBackend::Vision &&
                    (camera->surface != nullptr || has_pending_screenshot(camera_handle))) {
                    requested_vision_cameras.push_back(camera_handle);
                }
                native_view_requested |= camera->render_backend == CameraRenderBackend::Native &&
                    (camera->surface != nullptr || has_pending_screenshot(camera_handle));
            }
        }
    }
    if (!requested_vision_cameras.empty() && !native_view_requested) {
        update_phase.set("native_resource_release");
        release_native_render_resources();
    }

    // Another camera waiting for its native window must not stall a ready
    // Vision view. Requested cameras already have a surface or capture request.
    if (!requested_vision_cameras.empty() && !vision_initialized_) {
        update_phase.set("vision_initialize");
        if (!init_vision_lazy()) {
            CFW_LOG_WARNING("OpticsSystem: Vision init failed, falling back affected cameras to Native");
            for (const auto camera_handle : requested_vision_cameras) {
                if (auto camera =
                        SharedDataHub::instance().camera_storage().acquire_write(camera_handle)) {
                    camera->render_backend = CameraRenderBackend::Native;
                }
            }
        }
    }

    if (!hardware_->shaderHasInit ||
        !hardware_->opticsOverlayPipeline ||
        !hardware_->opticsCursorPipeline || !hardware_->opticsUiWarpPipeline ||
        !hardware_->opticsCompositePipeline) {
        return;
    }

    static float frame_count = 0.0f;
    static uint64_t frame_index = 0;

    float dt = delta_time();
    frame_count += dt;
    ++frame_index;

    update_phase.set("render_dispatch");
    optics_pipeline(frame_count, frame_index);
}

OpticsSystem::RenderFramePlan OpticsSystem::build_render_frame_plan() {
    RenderFramePlan plan;
    std::unordered_set<std::uintptr_t> scheduled;
    for (const auto& scene : std::as_const(SharedDataHub::instance().scene_storage())) {
        if (!scene.enabled) continue;
        const auto scene_handle = reinterpret_cast<std::uintptr_t>(&scene);
        for (const auto handle : scene.camera_handles) {
            auto camera = SharedDataHub::instance().camera_storage().try_acquire_read(handle);
            if (!camera) continue;
            plan.has_vision_cameras |= camera->render_backend == CameraRenderBackend::Vision;
            if (!camera->surface && !has_pending_screenshot(handle)) continue;
            if (!scheduled.insert(handle).second) continue;
            auto& views = camera->render_backend == CameraRenderBackend::Vision
                ? plan.vision_views : plan.native_views;
            views.push_back({scene_handle, handle, camera->vision_render_mode});
        }
    }
    return plan;
}

std::optional<OpticsSystem::LockedRenderView> OpticsSystem::lock_render_view(
    const RenderView& view, CameraRenderBackend backend) {
    // Scene structure stays locked, while camera values are copied under a
    // short read lock. UI presentation and input must not wait for a GPU frame.
    auto scene = SharedDataHub::instance().scene_storage().try_acquire_read(view.scene_handle);
    if (!scene || !scene->enabled ||
        std::find(scene->camera_handles.begin(), scene->camera_handles.end(), view.camera_handle) ==
            scene->camera_handles.end()) return std::nullopt;
    auto camera = SharedDataHub::instance().camera_storage().try_acquire_read(view.camera_handle);
    if (!camera || camera->render_backend != backend ||
        (!camera->surface && !has_pending_screenshot(view.camera_handle))) return std::nullopt;
    return LockedRenderView{std::move(scene), *camera};
}

void OpticsSystem::optics_pipeline(float frame_count, uint64_t frame_index) {
    drain_viewport_ui_pointer_commands();
    fail_expired_pending_screenshots();
    fail_unrenderable_pending_screenshots();

    // Schedule each handle only once. Scene/assets stay in storage under locks;
    // only small camera/environment values are copied for a coherent frame.
    const auto plan = build_render_frame_plan();
    if (env_flag_enabled("CORONA_OPTICS_MEASURE_RENDER_TIME")) {
        CFW_LOG_INFO("OpticsFrameRoutes frame={} native_views={} vision_views={}",
                     frame_index, plan.native_views.size(), plan.vision_views.size());
    }
    if (!plan.native_views.empty()) run_native_frame(frame_count, frame_index, plan.native_views);
    if (!plan.vision_views.empty() && vision_initialized_) {
        run_vision_frame(frame_count, frame_index, plan.vision_views);
    } else if (vision_initialized_) {
        // Native-only frames must not load, synchronize or render a Vision scene.
        // Keep on-demand capture resources until the next requested capture.
        if (!plan.has_vision_cameras && active_vision_runtime_key_) {
            clear_vision_runtimes();
        } else {
            evict_idle_vision_runtimes(frame_index);
        }
    }
    evict_idle_surface_targets(frame_index);
    evict_idle_offscreen_screenshot_targets(frame_index);
    evict_idle_native_view_resources(frame_index);
    evict_idle_ui_view_resources(frame_index);
}

void OpticsSystem::run_native_frame(float frame_count, uint64_t frame_index,
                                    const std::vector<RenderView>& views) {
    if (views.empty() || !ensure_native_render_resources()) return;
    const bool shadow_pipelines_ready = std::all_of(
        hardware_->shadowPipelines.begin(), hardware_->shadowPipelines.end(),
        [](const auto& pipeline) { return pipeline.has_value(); });
    if (!hardware_->visibilityPipeline || !hardware_->lightingPipeline ||
        !shadow_pipelines_ready ||
        !hardware_->skyPipeline || !hardware_->tonemapPipeline || !hardware_->skySHProjectPipeline) return;
    auto& lighting = *hardware_->lightingPipeline;
    auto& shadow_pipelines = hardware_->shadowPipelines;
    auto& sky = *hardware_->skyPipeline;
    auto& tonemap = *hardware_->tonemapPipeline;
    for (const auto& view : views) {
        if (!ensure_native_frame_slot()) continue;
        auto locked = lock_render_view(view, CameraRenderBackend::Native);
        if (!locked) continue;
        const auto cam_handle = view.camera_handle;
        const CameraDevice* camera = &locked->camera;
        // No scene data is read while allocating targets or waiting on Display.
        locked->scene = {};
        void* surface = camera->surface;
        const bool offscreen_screenshot = surface == nullptr;
        if (offscreen_screenshot && !has_pending_screenshot(cam_handle)) {
            continue;
        }
        const double native_throttle_wait_ms =
            std::exchange(pending_native_throttle_wait_ms_, 0.0);
        const auto native_frame_start = PerfClock::now();
        double native_collect_ms = 0.0;
        double native_submit_ms = 0.0;
        double native_shadow_ms = 0.0;
        double native_commit_ms = 0.0;
        uint32_t native_instance_count = 0;
        uint32_t native_visibility_draws = 0;
        uint64_t native_visibility_indices = 0;
        std::array<uint32_t, kShadowCascadeCount> native_cascade_draws{};
        std::array<uint64_t, kShadowCascadeCount> native_cascade_indices{};
        bool native_shadows_enabled = false;
        const bool is_debug_mode =
            camera->output_mode != CameraOutputMode::FinalColor;
        bool native_debug_mode = is_debug_mode;

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
                if (consumed_device->consumed_receipt.serial != 0) {
                    hardware_->executor.wait(consumed_device->consumed_receipt);
                }
            }
        }
        bind_native_view_resources(cam_handle, camera->width, camera->height,
                                   frame_index);
        locked->scene = SharedDataHub::instance().scene_storage().try_acquire_read(view.scene_handle);
        if (!locked->scene || !locked->scene->enabled ||
            std::find(locked->scene->camera_handles.begin(), locked->scene->camera_handles.end(), cam_handle) ==
                locked->scene->camera_handles.end()) continue;
        const auto& scene = *locked->scene;
        auto& visibility = *hardware_->visibilityPipeline;
        struct RefractionDraw {
            Horizon::HardwareBuffer index,vertex,indirect;
            Horizon::DrawIndexedIndirectParams params;
            ktm::fmat4x4 clip;
            uint32_t instance,material;
        };
        std::vector<RefractionDraw> refraction_draws;

        // ================================================================
        // 1. Update camera uniform buffers
        // ================================================================
        hardware_->uniformBufferObjects.eyePosition = make_vec4(camera->position, 1.0f);
        hardware_->uniformBufferObjects.eyeDir = make_vec4(camera->forward, 0.0f);
        hardware_->uniformBufferObjects.eyeViewMatrix = camera->compute_view_matrix();
        hardware_->uniformBufferObjects.eyeProjMatrix = OpticsDetail::native_projection(*camera);
        hardware_->uniformBufferObjects.eyeInvProjMatrix =
            OpticsDetail::native_inverse_projection(*camera);
        hardware_->vpUniformBufferObjects.viewProjMatrix =
            multiply_ktm_mat4(OpticsDetail::native_projection(*camera), camera->compute_view_matrix());

        // ---- Per-submission buffer ���ã�������֡/��������� buffer ���Ǿ�����----
        // �ӳظ���һ�ݵ�ǰ�� GPU ���õ� buffer������� pass ȫ�̸�����Щ lease��
        // ���ٴ��� hardware_->*Buffer ����������������� wait_idle ����·������
        // commit ǰ�� stream << keep_alive(busy)��GPU �����Զ��سظ��á�
        auto vpLease = hardware_->vpUniformBufferPool.acquire(
            [] { return make_storage_buffer<Hardware::VPUniformBufferObject>(1, "optics.vp_uniform.pool"); });
        auto uboLease = hardware_->uniformBufferPool.acquire(
            [] { return make_storage_buffer<Hardware::UniformBufferObject>(1, "optics.uniform.pool"); });
        auto shadowLease = hardware_->shadowInfoBufferPool.acquire(
            [] { return make_storage_buffer<Hardware::ShadowInfoBufferObject>(1, "optics.shadow_info.pool"); });
        FramePlaceBufferPool::Lease instLease;
        FramePlaceBufferPool::Lease matLease;
        Horizon::HardwareBuffer& sceneVpBuffer = *vpLease.buffer;
        Horizon::HardwareBuffer& sceneUboBuffer = *uboLease.buffer;
        Horizon::HardwareBuffer& sceneShadowBuffer = *shadowLease.buffer;

        (void)write_object_bytes(sceneVpBuffer,
                                 hardware_->vpUniformBufferObjects);

        // Configure visibility pipeline render targets
        visibility.visibilityData = hardware_->visibilityImage;
        visibility.bind_depth_target(hardware_->depthImage);

        auto& actor_storage = SharedDataHub::instance().actor_storage();
        auto& profile_storage = SharedDataHub::instance().profile_storage();
        auto& optics_storage = SharedDataHub::instance().optics_storage();
        auto& geom_storage = SharedDataHub::instance().geometry_storage();
        auto& transform_storage = SharedDataHub::instance().model_transform_storage();
        const auto& diag = optics_diag_config();

        RenderInstanceBatch sceneBatch;
        std::vector<Horizon::HardwareBuffer> scene_indirect_buffers;  // CRITICAL: Must outlive commit()

        auto collect_actor_instances_for_pass =
            [&](auto& target_visibility,
                const ktm::fmat4x4& view_proj_matrix,
                bool follow_camera_pass,
                const ktm::fmat4x4* camera_basis,
                RenderInstanceBatch& batch) -> bool {
            batch.clear();

            // ---- Native ��׶�޳���Step 5��----
            // ���Ѽ����߳���õ� visible_actor_handles���������׶��������
            // �����ڳ��� pass��!follow_camera_pass����UI/������� actor ��������
            // ��׶�ɼ����ڣ����ɾݴ��޳����ռ� �� ����ȫ������ Vision һ�£���
            // ����ɼ�����δ���ʱ������ʧ��
            // ע�⣺object_id �� actor_handles �� 1-based ȫ���±꣬��Ϊ V-buffer
            // objectID �á��޳�����"�������Ƶ��� ++object_id"����֤ÿ������Ⱦ
            // ����� objectID ���޳�ǰ��ȫһ�£������� draw call�����ı�����壩��
            std::unordered_set<std::uintptr_t> visible_set;
            const bool use_visible_cull =
                !follow_camera_pass && !scene.visible_actor_handles.empty();
            if (use_visible_cull) {
                visible_set.reserve(scene.visible_actor_handles.size());
                for (auto h : scene.visible_actor_handles) visible_set.insert(h);
            }

            bool has_instances = false;
            uint32_t recorded_draws = 0;
            uint32_t object_id = 1;
            for (auto actor_handle : scene.actor_handles) {
                auto actor = actor_storage.try_acquire_read(actor_handle);
                if (!actor) {
                    ++object_id;
                    continue;
                }
                if (!diag_actor_allowed(actor_handle)) {
                    ++object_id;
                    continue;
                }

                // ����δ���ص� actor��GPU ��Դ���ͷţ��޷���Ⱦ��
                {
                    std::shared_lock lock(residency_mtx_);
                    if (!resident_actors_.count(actor_handle)) {
                        ++object_id;
                        continue;
                    }
                }

                // ������Դ����ʱ�䣨���� ResourceManager LRU��
                {
                    auto& model_res_storage =
                        SharedDataHub::instance().model_resource_storage();
                    for (auto ph : actor->profile_handles) {
                        auto prof = profile_storage.try_acquire_read(ph);
                        if (!prof || !prof->geometry_handle) continue;
                        auto g = geom_storage.try_acquire_read(prof->geometry_handle);
                        if (!g || !g->model_resource_handle) continue;
                        auto mr = model_res_storage.try_acquire_read(g->model_resource_handle);
                        if (mr && mr->model_id) {
                            Corona::Resource::ResourceManager::get_instance().touch(mr->model_id);
                        }
                        break;  // һ�� actor ֻ����һ��
                    }
                }

                if (actor->follow_camera != follow_camera_pass) {
                    ++object_id;
                    continue;
                }

                // ---- ��׶�޳������ڿɼ����� �� �������ƣ��� ++object_id ����ţ�----
                if (use_visible_cull && !visible_set.count(actor_handle)) {
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
                    if (!diag_geometry_allowed(optics.geometry_handle)) {
                        continue;
                    }
                    // ---- ��ȡ���α任��Ϣ��������----
                    std::uintptr_t transform_handle_b = 0;
                    float          nlc_scale_b  = 1.0f;
                    ktm::fvec3     nlc_offset_b = {0.0f, 0.0f, 0.0f};
                    if (auto geom_r = geom_storage.try_acquire_read(optics.geometry_handle)) {
                        transform_handle_b = geom_r->transform_handle;
                        nlc_scale_b        = geom_r->native_local_correction_scale;
                        nlc_offset_b       = geom_r->native_local_correction_offset;
                    }

                    ktm::fmat4x4 model_matrix{ktm::fmat4x4::from_eye()};
                    ktm::fvec3   world_center{0.0f, 0.0f, 0.0f};
                    if (auto transform = transform_storage.try_acquire_read(transform_handle_b)) {
                        model_matrix = transform->compute_matrix();
                        world_center = transform->position;
                        if (std::abs(nlc_scale_b - 1.0f) > 1e-6f ||
                            std::abs(nlc_offset_b.x) > 1e-6f ||
                            std::abs(nlc_offset_b.y) > 1e-6f ||
                            std::abs(nlc_offset_b.z) > 1e-6f) {
                            ktm::fmat4x4 correction = ktm::fmat4x4::from_eye();
                            correction[0][0] = nlc_scale_b;
                            correction[1][1] = nlc_scale_b;
                            correction[2][2] = nlc_scale_b;
                            correction[3][0] = nlc_offset_b.x;
                            correction[3][1] = nlc_offset_b.y;
                            correction[3][2] = nlc_offset_b.z;
                            model_matrix = multiply_ktm_mat4(model_matrix, correction);
                        }
                        if (camera_basis != nullptr) {
                            model_matrix = multiply_ktm_mat4(*camera_basis, model_matrix);
                        }
                    }

                    // ---- �����Χ�뾶��LOD ѡ���ã�----
                    float bounding_radius = 1.0f;
                    bool  have_bounds     = false;
                    if (geometry_system_) {
                        auto& ms = SharedDataHub::instance().mechanics_storage();
                        if (auto mech = ms.try_acquire_read(profile->mechanics_handle)) {
                            float dx = mech->max_xyz.x - mech->min_xyz.x;
                            float dy = mech->max_xyz.y - mech->min_xyz.y;
                            float dz = mech->max_xyz.z - mech->min_xyz.z;
                            bounding_radius = std::sqrt(dx*dx + dy*dy + dz*dz) * 0.5f;
                            have_bounds = true;
                        }
                    }

                    // ---- query_mesh_slots��ͳһ�ӿڣ�LOD �ѽ����������ֶ� fallback ----
                    const auto mesh_slots_b = (geometry_system_ && have_bounds)
                        ? geometry_system_->query_mesh_slots(
                              optics.geometry_handle,
                              camera->position, camera->fov,
                              world_center, bounding_radius,
                              &model_matrix)
                        : (geometry_system_
                              ? geometry_system_->query_mesh_slots(optics.geometry_handle)
                              : std::vector<GeometrySystem::MeshSlot>{});

                    // CRITICAL FIX: Reserve space in outer scope's buffer vector
                    // Buffers must stay alive until after commit() (line 4633)
                    scene_indirect_buffers.reserve(scene_indirect_buffers.size() + mesh_slots_b.size());

                    struct PendingVisibilityDraw {
                        Horizon::HardwareBuffer index_buffer;
                        Horizon::HardwareBuffer vertex_buffer;
                        Horizon::DrawIndexedIndirectCommand command{};
                        ktm::fmat4x4 clip_matrix{ktm::fmat4x4::from_eye()};
                        uint32_t instance_id = 0;
                        uint32_t material_id = 0;
                        uint32_t texture_descriptor = 0;
                        uint32_t mesh_index = 0;
                        uint32_t vertex_count = 0;
                        uint32_t index_count = 0;
                        uint32_t max_index = 0;
                    };
                    std::vector<PendingVisibilityDraw> pending_visibility_draws;
                    pending_visibility_draws.reserve(mesh_slots_b.size());

                    for (const auto& ms : mesh_slots_b) {
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
                        auto materialID = static_cast<uint32_t>(batch.materials.size());
                        batch.materials.push_back(Corona::Systems::OpticsDetail::pack_native_material(
                            ms, optics, texture_descriptor, diag.disable_textures || diag.force_solid_material,
                            optics.bEnableLighting && !diag.force_solid_material));
                        if (optics.principled && !diag.disable_textures && !diag.force_solid_material) {
                            auto appearance=hardware_->appearance_cache.obtain(optics.principled);
                            if (appearance) {
                                auto& packed=batch.materials.back();
                                // Bit 6 changes occlusionDescriptor into an expression-program SSBO.
                                packed.flags|=64u;
                                packed.occlusionDescriptor=appearance->program.store_descriptor();
                                batch.resource_keep_alive->appearances.push_back(std::move(appearance));
                            }
                        }
                        log_optics_material_once(actor_handle,
                                                 optics.geometry_handle,
                                                 ms.mesh_index,
                                                 materialID,
                                                 texture_descriptor,
                                                 ms.texture,
                                                 ms.texture_ready,
                                                 diag.disable_albedo_sample);
                        batch.keep_mesh_resources(ms, texture_descriptor);

                        auto instanceID = static_cast<uint32_t>(batch.instances.size());
                        {
                            Hardware::InstanceInfo inst{};
                            inst.modelMatrix = mesh_model_matrix;
                            inst.vertexBufferIndex = vertex_descriptor;
                            inst.indexBufferIndex = index_descriptor;
                            inst.materialID = materialID;
                            inst.objectID = object_id;
                            inst.indexCount = ms.index_count;
                            inst.vertexCount = ms.vertex_count;
                            inst.maxIndex = ms.max_index;
                            inst.flags = 0u;
                            batch.instances.push_back(inst);
                            batch.actorHandles.push_back(actor_handle);
            batch.nodeIndices.push_back(ms.node_index);
                            has_instances = true;
                        }

                        const ktm::fmat4x4 clip_matrix =
                            multiply_ktm_mat4(view_proj_matrix, mesh_model_matrix);

                        using Pipeline = Corona::Horizon::RasterizerPipeline<visibility_vert_glsl_t, visibility_frag_glsl_t>;
                        auto& pc = static_cast<Pipeline::VertexResourceBindings&>(target_visibility).pushConsts;
                        pc.modelMatrix = upload_value(clip_matrix);
                        pc.uniformBufferIndex = 0u;
                            pc.padding0 = 0u;
                        pc.instanceID = instanceID + 1;
                        pc.textureIndex = texture_descriptor;

                        // 新版 Horizon: 使用 indirect draw
                        Horizon::DrawIndexedIndirectCommand draw_cmd{};
                        draw_cmd.index_count = ms.index_count;
                        draw_cmd.first_index = 0;
                        draw_cmd.vertex_offset = 0;
                        draw_cmd.instance_count = 1;
                        draw_cmd.first_instance = 0;

                        PendingVisibilityDraw pending{};
                        pending.index_buffer = ms.geo.index;
                        pending.vertex_buffer = ms.geo.vertex;
                        pending.command = draw_cmd;
                        pending.clip_matrix = clip_matrix;
                        pending.instance_id = instanceID + 1;
                        pending.material_id = materialID;
                        pending.texture_descriptor = texture_descriptor;
                        pending.mesh_index = ms.mesh_index;
                        pending.vertex_count = ms.vertex_count;
                        pending.index_count = ms.index_count;
                        pending.max_index = ms.max_index;
                        pending_visibility_draws.push_back(std::move(pending));
                        ++recorded_draws;
                    }

                    if (!pending_visibility_draws.empty()) {
                        std::vector<Horizon::DrawIndexedIndirectCommand> commands;
                        commands.reserve(pending_visibility_draws.size());
                        for (const auto& pending : pending_visibility_draws) {
                            commands.push_back(pending.command);
                        }

                        auto indirect_buffer =
                            Horizon::HardwareBuffer::from_bytes(
                                std::as_bytes(std::span<const Horizon::DrawIndexedIndirectCommand>(
                                    commands.data(), commands.size())),
                                commands.size() * sizeof(Horizon::DrawIndexedIndirectCommand),
                                Horizon::BufferUsage_Indirect,
                                follow_camera_pass ? "follow_visibility.batch"
                                                   : "scene_visibility.batch");

                        using Pipeline = Corona::Horizon::RasterizerPipeline<visibility_vert_glsl_t, visibility_frag_glsl_t>;
                        auto& pc = static_cast<Pipeline::VertexResourceBindings&>(target_visibility).pushConsts;
                        for (std::size_t draw_index = 0;
                             draw_index < pending_visibility_draws.size();
                             ++draw_index) {
                            const auto& pending = pending_visibility_draws[draw_index];
                            pc.modelMatrix = upload_value(pending.clip_matrix);
                            pc.uniformBufferIndex = 0u;
                            pc.padding0 = 0u;
                            pc.instanceID = pending.instance_id;
                            pc.textureIndex = pending.texture_descriptor;

                            Horizon::DrawIndexedIndirectParams draw_params;
                            draw_params.draw_count = 1;
                            draw_params.indirect_offset =
                                draw_index * sizeof(Horizon::DrawIndexedIndirectCommand);
                            draw_params.stride = sizeof(Horizon::DrawIndexedIndirectCommand);

                            if (!follow_camera_pass) refraction_draws.push_back({pending.index_buffer,pending.vertex_buffer,
                                indirect_buffer,draw_params,pending.clip_matrix,pending.instance_id,pending.material_id});
                            target_visibility.record_indirect(
                                pending.index_buffer,
                                pending.vertex_buffer,
                                indirect_buffer,
                                draw_params);
                        }
                        scene_indirect_buffers.push_back(std::move(indirect_buffer));
                    }
                    ++object_id;
                }
            }
            return has_instances;
        };

        std::shared_ptr<OpticsDetail::NativeLighting> direct_lighting;
        Horizon::HardwareBuffer rsm_material_buffer;
        std::vector<Hardware::MaterialInfo> rsm_materials;
        std::vector<Horizon::HardwareImage> rsm_material_images;
        std::vector<Horizon::HardwareBuffer> rsm_material_programs;
        struct ShadowCasterSnapshot {
            std::unordered_map<uint32_t,uint32_t> material_ids;
            std::size_t material_revision{};
            std::uintptr_t actor_handle = 0;
            std::uintptr_t profile_handle = 0;
            std::uintptr_t geometry_handle = 0;
            std::uint64_t pose_revision = 0;
            ktm::fmat4x4 model_matrix{ktm::fmat4x4::from_eye()};
            Corona::Systems::OpticsDetail::ShadowCasterBoundsSnapshot bounds{};
            float max_abs_scale = 1.0f;
            std::uint32_t cascade_visibility_mask = 0;
            std::array<std::vector<GeometrySystem::ShadowMeshSlot>,
                       kShadowCascadeCount> cascade_slots{};
        };

        ShadowSceneBounds shadow_scene_bounds;
        std::vector<ShadowCasterSnapshot> shadow_casters;
        std::vector<Horizon::HardwareBuffer> shadow_indirect_buffers;  // CRITICAL: Must outlive commit()
        std::mutex shadow_indirect_buffers_mutex;  // Thread-safe for parallel shadow recording

        auto build_shadow_caster_snapshots = [&]() {
            std::unordered_set<std::uintptr_t> resident_snapshot;
            {
                std::shared_lock lock(residency_mtx_);
                resident_snapshot = resident_actors_;
            }
            shadow_casters.reserve(scene.actor_handles.size());
            for (auto actor_handle : scene.actor_handles) {
                auto actor = actor_storage.try_acquire_read(actor_handle);
                if (!actor || actor->follow_camera ||
                    !diag_actor_allowed(actor_handle) ||
                    !resident_snapshot.contains(actor_handle)) {
                    continue;
                }
                for (auto profile_handle : actor->profile_handles) {
                    auto profile = profile_storage.try_acquire_read(profile_handle);
                    if (!profile || profile->optics_handle == 0 ||
                        profile->mechanics_handle == 0) {
                        continue;
                    }
                    auto optics_acc = optics_storage.try_acquire_read(profile->optics_handle);
                    if (!optics_acc || !optics_acc->visible ||
                        !diag_geometry_allowed(optics_acc->geometry_handle)) {
                        continue;
                    }
                    // Clear glass transmits the solar field approximately;
                    // colored shadows and caustics require a separate model.
                    if(optics_acc->principled && optics_acc->principled->transmission>=.999f)continue;
                    auto geom = geom_storage.try_acquire_read(optics_acc->geometry_handle);
                    if (!geom || geom->transform_handle == 0) continue;
                    auto transform =
                        transform_storage.try_acquire_read(geom->transform_handle);
                    auto mech = SharedDataHub::instance()
                                    .mechanics_storage()
                                    .try_acquire_read(profile->mechanics_handle);
                    if (!transform || !mech) continue;

                    ShadowCasterSnapshot snapshot;
                    snapshot.actor_handle = actor_handle;
                    snapshot.profile_handle = profile_handle;
                    snapshot.geometry_handle = optics_acc->geometry_handle;
                    snapshot.pose_revision = geom->skin_pose_revision;
                    snapshot.model_matrix = apply_native_local_correction(
                        transform->compute_matrix(), *geom);
                    snapshot.max_abs_scale = std::max({
                        std::abs(transform->scale.x),
                        std::abs(transform->scale.y),
                        std::abs(transform->scale.z),
                        1.0e-6f}) * std::max(
                            std::abs(geom->native_local_correction_scale), 1.0e-6f);

                    for (const auto& corner :
                         bounds_corners(mech->min_xyz, mech->max_xyz)) {
                        const auto point = transform_point(snapshot.model_matrix, corner);
                        if (!snapshot.bounds.valid) {
                            snapshot.bounds.world_bounds.min = point;
                            snapshot.bounds.world_bounds.max = point;
                            snapshot.bounds.valid = true;
                        } else {
                            snapshot.bounds.world_bounds.min.x = std::min(
                                snapshot.bounds.world_bounds.min.x, point.x);
                            snapshot.bounds.world_bounds.min.y = std::min(
                                snapshot.bounds.world_bounds.min.y, point.y);
                            snapshot.bounds.world_bounds.min.z = std::min(
                                snapshot.bounds.world_bounds.min.z, point.z);
                            snapshot.bounds.world_bounds.max.x = std::max(
                                snapshot.bounds.world_bounds.max.x, point.x);
                            snapshot.bounds.world_bounds.max.y = std::max(
                                snapshot.bounds.world_bounds.max.y, point.y);
                            snapshot.bounds.world_bounds.max.z = std::max(
                                snapshot.bounds.world_bounds.max.z, point.z);
                        }
                    }
                    snapshot.bounds.valid = snapshot.bounds.valid &&
                                            snapshot.bounds.world_bounds.valid();
                    Corona::Systems::OpticsDetail::include_shadow_scene_bounds(
                        shadow_scene_bounds, snapshot.bounds);
                    shadow_casters.push_back(std::move(snapshot));
                }
            }
        };

        std::array<Corona::Systems::OpticsDetail::ShadowCascadeView,
                   kShadowCascadeCount> shadow_cascade_views{};

        auto prepare_shadow_caster_meshes = [&]() {
            const std::uint32_t enabled_mask =
                (!diag.skip_shadows && native_shadows_enabled)
                    ? diag.shadow_cascade_mask
                    : 0u;
            for (auto& caster : shadow_casters) {
                caster.cascade_visibility_mask =
                    Corona::Systems::OpticsDetail::shadow_cascade_visibility_mask(
                        caster.bounds, shadow_cascade_views, enabled_mask);
                if (caster.cascade_visibility_mask == 0u) continue;

                if (geometry_system_) {
                    std::array<GeometrySystem::ShadowLodQuery,
                               kShadowCascadeCount> queries{};
                    for (std::uint32_t cascade = 0;
                         cascade < kShadowCascadeCount;
                         ++cascade) {
                        queries[cascade].enabled =
                            (caster.cascade_visibility_mask & (1u << cascade)) != 0u;
                        queries[cascade].world_units_per_texel =
                            shadow_cascade_views[cascade].world_units_per_texel;
                    }
                    auto slots = geometry_system_->query_shadow_mesh_slots_batch(
                        caster.geometry_handle,
                        queries,
                        caster.max_abs_scale,
                        frame_index);
                    for (std::size_t cascade = 0;
                         cascade < std::min(slots.size(), caster.cascade_slots.size());
                         ++cascade) {
                        caster.cascade_slots[cascade] = std::move(slots[cascade]);
                    }
                    auto profile=SharedDataHub::instance().profile_storage().acquire_read(caster.profile_handle);
                    auto optics=SharedDataHub::instance().optics_storage().acquire_read(profile->optics_handle);
                    caster.material_revision=optics->principled?optics->principled->revision:0;
                    for(const auto& cascade_slots:caster.cascade_slots)for(const auto& slot:cascade_slots) {
                        if(caster.material_ids.contains(slot.mesh_index))continue;
                        caster.material_ids.emplace(slot.mesh_index,static_cast<uint32_t>(rsm_materials.size()));
                        const auto descriptor=slot.texture?slot.texture.store_descriptor():0u;
                        rsm_materials.push_back(OpticsDetail::pack_native_material(slot,*optics,descriptor,false,optics->bEnableLighting));
                        if(slot.texture)rsm_material_images.push_back(slot.texture);
                        if(slot.material) {
                            sceneBatch.resource_keep_alive->model_materials.push_back(slot.material);
                            for(const auto& image:slot.material->textures)if(image)rsm_material_images.push_back(image);
                        }
                        if(auto appearance=hardware_->appearance_cache.obtain(optics->principled)) {
                            rsm_materials.back().flags|=64u;
                            rsm_materials.back().occlusionDescriptor=appearance->program.store_descriptor();
                            rsm_material_programs.push_back(appearance->program);
                            for(const auto& image:appearance->images)rsm_material_images.push_back(image->image);
                            sceneBatch.resource_keep_alive->appearances.push_back(std::move(appearance));
                        }
                    }
                }
            }
        };

        auto record_shadow_cascade =
            [&](const ktm::fmat4x4& light_view_proj,
                Horizon::HardwareImage& shadow_depth,
                uint32_t cascade_index) {
            auto& shadow = *shadow_pipelines[cascade_index];
            shadow.clear_records();
            shadow.bind_depth_target(shadow_depth);
            shadow.rsmPosition=hardware_->rsmPositions[cascade_index];
            shadow.rsmNormal=hardware_->rsmNormals[cascade_index];
            shadow.rsmFlux=hardware_->rsmFluxes[cascade_index];
            bind_storage_buffer(shadow,shadow.rsmConsts.materialTableBufferIndex,rsm_material_buffer);
            bind_storage_buffer(shadow,shadow.rsmConsts.shadowInfo,sceneShadowBuffer);
            shadow.rsmConsts.cascade=cascade_index;
            uint32_t shadow_draws = 0;

            struct PendingShadowDraw {
                uint32_t material_id{};
                Horizon::HardwareBuffer index_buffer;
                Horizon::HardwareBuffer vertex_buffer;
                std::uint32_t first_command{}, command_count{};
                ktm::fmat4x4 clip_matrix{ktm::fmat4x4::from_eye()};
                ktm::fmat4x4 model_matrix{ktm::fmat4x4::from_eye()};
            };
            std::vector<PendingShadowDraw> pending_shadow_draws;
            std::vector<Horizon::DrawIndexedIndirectCommand> commands;

            for (const auto& caster : shadow_casters) {
                if ((caster.cascade_visibility_mask & (1u << cascade_index)) == 0u) {
                    continue;
                }
                for (const auto& slot : caster.cascade_slots[cascade_index]) {
                    if (!diag_mesh_allowed(slot.mesh_index)) continue;
                    if (shadow_draws >= diag.draw_limit) return;
                    if (!slot.valid) continue;

                    const ktm::fmat4x4 shadow_model_matrix =
                        multiply_ktm_mat4(caster.model_matrix, slot.instance_transform);
                    const ktm::fmat4x4 clip_matrix = multiply_ktm_mat4(
                        light_view_proj, shadow_model_matrix);
                    const Corona::Systems::OpticsDetail::ShadowMeshFrustum mesh_frustum(clip_matrix);
                    if (!mesh_frustum.visible(slot.bounds_min, slot.bounds_max, slot.bounds_valid)) continue;
                    shadow.pushConsts.lightViewProjModel =
                        upload_value(clip_matrix);

                    // 新版 Horizon: 使用 indirect draw
                    Horizon::DrawIndexedIndirectCommand draw_cmd{};
                    draw_cmd.index_count = slot.index_count;
                    draw_cmd.first_index = 0;
                    draw_cmd.vertex_offset = 0;
                    draw_cmd.instance_count = 1;
                    draw_cmd.first_instance = 0;

                    PendingShadowDraw pending{};
                    pending.material_id=caster.material_ids.at(slot.mesh_index);
                    pending.index_buffer = slot.geo.index;
                    pending.vertex_buffer = slot.geo.vertex;
                    pending.clip_matrix = clip_matrix;
                    pending.model_matrix = shadow_model_matrix;
                    pending.first_command = static_cast<std::uint32_t>(commands.size());
                    if (slot.draw_clusters && !slot.draw_clusters->empty()) {
                        for (const auto& cluster : *slot.draw_clusters) {
                            if (!mesh_frustum.visible(cluster.minimum, cluster.maximum, cluster.valid)) continue;
                            if (commands.size() > pending.first_command &&
                                commands.back().first_index + commands.back().index_count == cluster.first_index) {
                                commands.back().index_count += cluster.index_count;
                            } else {
                                draw_cmd.first_index = cluster.first_index;
                                draw_cmd.index_count = cluster.index_count;
                                commands.push_back(draw_cmd);
                            }
                            native_cascade_indices[cascade_index] += cluster.index_count;
                        }
                    } else {
                        commands.push_back(draw_cmd);
                        native_cascade_indices[cascade_index] += slot.index_count;
                    }
                    pending.command_count = static_cast<std::uint32_t>(commands.size()) - pending.first_command;
                    if (pending.command_count == 0) continue;
                    pending_shadow_draws.push_back(std::move(pending));
                    ++shadow_draws;
                    ++native_cascade_draws[cascade_index];
                }
            }

            if (!pending_shadow_draws.empty()) {
                auto indirect_buffer =
                    Horizon::HardwareBuffer::from_bytes(
                        std::as_bytes(std::span<const Horizon::DrawIndexedIndirectCommand>(
                            commands.data(), commands.size())),
                        commands.size() * sizeof(Horizon::DrawIndexedIndirectCommand),
                        Horizon::BufferUsage_Indirect,
                        "shadow.batch");

                for (std::size_t draw_index = 0;
                     draw_index < pending_shadow_draws.size();
                     ++draw_index) {
                    const auto& pending = pending_shadow_draws[draw_index];
                    shadow.rsmConsts.materialID=pending.material_id;
                    shadow.pushConsts.lightViewProjModel =
                        upload_value(pending.clip_matrix);
                    const auto& model=pending.model_matrix;
                    shadow.pushConsts.modelRow0=upload_value(ktm::fvec4{model[0][0],model[1][0],model[2][0],model[3][0]});
                    shadow.pushConsts.modelRow1=upload_value(ktm::fvec4{model[0][1],model[1][1],model[2][1],model[3][1]});
                    shadow.pushConsts.modelRow2=upload_value(ktm::fvec4{model[0][2],model[1][2],model[2][2],model[3][2]});

                    Horizon::DrawIndexedIndirectParams draw_params;
                    draw_params.stride = sizeof(Horizon::DrawIndexedIndirectCommand);
                    // Horizon requires multiDrawIndirect. Stay within
                    // Vulkan's required 65535-command minimum limit.
                    for (std::uint32_t first = 0; first < pending.command_count;) {
                        draw_params.draw_count = std::min(65535u, pending.command_count - first);
                        draw_params.indirect_offset =
                            (std::uint64_t(pending.first_command) + first) * sizeof(Horizon::DrawIndexedIndirectCommand);
                        shadow.record_indirect(pending.index_buffer, pending.vertex_buffer,
                                               indirect_buffer, draw_params);
                        first += draw_params.draw_count;
                    }
                }

                std::lock_guard<std::mutex> lock(shadow_indirect_buffers_mutex);
                shadow_indirect_buffers.push_back(std::move(indirect_buffer));
            }
        };

        {
            const auto native_collect_start = PerfClock::now();
            visibility.clear_records();
            if (!diag.skip_scene_visibility) {
                collect_actor_instances_for_pass(visibility,
                                                 hardware_->vpUniformBufferObjects.viewProjMatrix,
                                                 false,
                                                 nullptr,
                                                 sceneBatch);
            } else {
                sceneBatch.clear();
            }
            const auto scene_instance_capacity = grow_table_capacity(
                kInitialInstanceTableCapacity,
                static_cast<std::uint64_t>(sceneBatch.instances.size()));
            const auto scene_material_capacity = grow_table_capacity(
                kInitialMaterialTableCapacity,
                static_cast<std::uint64_t>(sceneBatch.materials.size()));
            instLease = hardware_->instanceInfoBufferPool.acquire(
                scene_instance_capacity,
                [scene_instance_capacity] {
                    return make_storage_buffer<Hardware::InstanceInfo>(
                        scene_instance_capacity, "optics.instances.pool");
                });
            matLease = hardware_->materialTableBufferPool.acquire(
                scene_material_capacity,
                [scene_material_capacity] {
                    return make_storage_buffer<Hardware::MaterialInfo>(
                        scene_material_capacity, "optics.materials.pool");
                });
            auto sceneInstanceCapacity = instLease.capacity;
            auto sceneMaterialCapacity = matLease.capacity;
            if (!upload_instance_tables(sceneBatch,
                                        *hardware_,
                                        *instLease.buffer,
                                        sceneInstanceCapacity,
                                        *matLease.buffer,
                                        sceneMaterialCapacity,
                                        "optics.scene.pool")) {
                visibility.clear_records();
                sceneBatch.clear();
            }
            native_collect_ms = elapsed_ms(native_collect_start, PerfClock::now());
            native_instance_count = static_cast<uint32_t>(sceneBatch.instances.size());
            native_visibility_draws = native_instance_count;
            for (const auto& instance : sceneBatch.instances) {
                native_visibility_indices += instance.indexCount;
            }
        }
        Horizon::HardwareBuffer& sceneInstanceBuffer = *instLease.buffer;
        Horizon::HardwareBuffer& sceneMaterialBuffer = *matLease.buffer;

        // ================================================================
        // 4. Environment parameters
        // ================================================================
        ktm::fvec3 sun_dir;
        sun_dir.x = 1.0f;
        sun_dir.y = 1.0f;
        sun_dir.z = 1.0f;
        std::uint32_t floor_grid_enabled = 1;
        ktm::fvec3 sun_color{1.0f, 0.949f, 0.853f};
        float sun_intensity = 0.0f;
        float sky_intensity = 0.0f;
        float exposure = 1.0f;
        EnvironmentDevice native_environment;
        native_environment.sun_intensity=0.f;
        native_environment.sky_intensity=0.f;
        if (scene.environment != 0) {
            if (auto env = SharedDataHub::instance().environment_storage().try_acquire_read(
                    scene.environment)) {
                sun_dir = env->sun_position;
                floor_grid_enabled = env->floor_grid_enabled;
                sun_color = env->sun_color;
                sun_intensity = env->sun_intensity;
                sky_intensity = env->sky_intensity;
                exposure = env->exposure;
                native_environment=*env;
            }
        }
        if (ktm::length(sun_dir) < 1e-5f) {
            sun_dir = make_vec3(1.0f, 1.0f, 1.0f);
        }
        sun_dir = ktm::normalize(sun_dir);
        bool geometry_loading = false;
        auto& shared_hub = SharedDataHub::instance();
        // During async import the actor may not yet have a visible
        // scene-batch instance. Inspect the storage globally so the
        // loading guard is active from the first import frame.
        for (auto it = shared_hub.geometry_storage().cbegin();
             it != shared_hub.geometry_storage().cend(); ++it) {
            if (it->gpu_build_state == GeometryDevice::GpuBuildState::PendingImport ||
                it->gpu_build_state == GeometryDevice::GpuBuildState::PendingBuild) {
                geometry_loading = true;
                break;
            }
        }
        direct_lighting=hardware_->lighting_cache.obtain(native_environment);
        hardware_->executor.wait(direct_lighting->ready);
        sceneBatch.resource_keep_alive->direct_lighting=direct_lighting;
        if (direct_lighting->authored_environment) {
            // The first authored directional is the solar RSM source.
            // Authored HDR has its own irradiance projection, so legacy sky
            // must not be added a second time.
            sky_intensity=0.f;
        }

        const bool needs_shadow = !is_debug_mode ||
            camera->output_mode == CameraOutputMode::ShadowMask ||
            camera->output_mode == CameraOutputMode::ShadowMaskRaw;
        if (needs_shadow &&
            !diag.skip_shadows && sun_intensity > 0.0f) {
            build_shadow_caster_snapshots();
        }
        const ktm::fvec3 shadow_min_world =
            shadow_scene_bounds.valid ? shadow_scene_bounds.min_world : scene.min_world;
        const ktm::fvec3 shadow_max_world =
            shadow_scene_bounds.valid ? shadow_scene_bounds.max_world : scene.max_world;
        const float shadow_far_plane = compute_shadow_far_plane(*camera);
        const auto shadow_splits = compute_shadow_splits(*camera, shadow_far_plane);
        float cascade_near = std::max(camera->near_plane, 0.01f);
        for (uint32_t cascade = 0; cascade < kShadowCascadeCount; ++cascade) {
            const float cascade_far = shadow_splits[cascade];
            shadow_cascade_views[cascade] =
                compute_shadow_light_view_proj(*camera,
                                               sun_dir,
                                               cascade_near,
                                               cascade_far,
                                               shadow_min_world,
                                               shadow_max_world);
            hardware_->shadowInfoBufferObjects.lightViewProj[cascade] =
                shadow_cascade_views[cascade].light_view_proj;
            hardware_->shadowInfoBufferObjects.shadowMapDescriptors[cascade] =
                hardware_->shadowCascadeImages[cascade].store_descriptor();
            auto& rsm=hardware_->shadowInfoBufferObjects.rsmDescriptors;
            rsm[cascade*4]=hardware_->rsmPositions[cascade].store_descriptor();
            rsm[cascade*4+1]=hardware_->rsmNormals[cascade].store_descriptor();
            rsm[cascade*4+2]=hardware_->rsmFluxes[cascade].store_descriptor();
            rsm[cascade*4+3]=std::bit_cast<uint32_t>(shadow_cascade_views[cascade].orthographic_width*
                shadow_cascade_views[cascade].orthographic_height/float(kShadowMapSize*kShadowMapSize));
            cascade_near = cascade_far;
        }
        hardware_->shadowInfoBufferObjects.cascadeSplits = ktm::fvec4{
            shadow_splits[0], shadow_splits[1], shadow_splits[2], shadow_splits[3]};
        hardware_->shadowInfoBufferObjects.shadowMapSize =
            static_cast<float>(kShadowMapSize);
        hardware_->shadowInfoBufferObjects.shadowBias = kShadowBias;
        hardware_->shadowInfoBufferObjects.shadowEnabled =
            (needs_shadow && !geometry_loading &&
             !diag.skip_shadows && diag.shadow_cascade_mask != 0u &&
             sun_intensity > 0.0f) ? 1u : 0u;
        native_shadows_enabled = hardware_->shadowInfoBufferObjects.shadowEnabled != 0u;
        prepare_shadow_caster_meshes();
        rsm_material_buffer=make_storage_buffer<Hardware::MaterialInfo>(
            std::max<std::size_t>(1,rsm_materials.size()),"optics.rsm.materials");
        if(!rsm_materials.empty())write_array_bytes(rsm_material_buffer,rsm_materials.data(),rsm_materials.size());
        hardware_->shadowInfoBufferObjects.sunColor=ktm::fvec4{sun_color.x*sun_intensity,sun_color.y*sun_intensity,sun_color.z*sun_intensity,0};
        hardware_->shadowInfoBufferObjects.sunDirection=ktm::fvec4{sun_dir.x,sun_dir.y,sun_dir.z,0};
        (void)write_object_bytes(sceneShadowBuffer,
                                 hardware_->shadowInfoBufferObjects);

        (void)write_object_bytes(sceneUboBuffer,
                                 hardware_->uniformBufferObjects);

        // Offscreen cameras (no surface) render to a dedicated image so
        // they never collide with the display pipeline's per-surface output.
        // Display cameras render into their own surface target (����1).
        Horizon::HardwareImage& render_target = target.final_output;
        Horizon::HardwareImage* presented_target = &render_target;
        const uint32_t visibilityStorageDescriptor =
            hardware_->visibilityImage.store_descriptor();
        const bool is_ssao_debug_mode =
            camera->output_mode == CameraOutputMode::SSAO ||
            camera->output_mode == CameraOutputMode::SSAORaw;
        const bool loading_guard = Corona::Geometry::NativeRenderLoadGuard::is_loading(
            geometry_loading, false);
        const bool should_run_ssao = Corona::Geometry::NativeRenderLoadGuard::effective_ssao(
            loading_guard, true, is_ssao_debug_mode);
        const uint32_t sceneMaterialCount = static_cast<uint32_t>(sceneBatch.materials.size());
        const bool has_refraction=!is_debug_mode && !loading_guard && true &&
            std::any_of(sceneBatch.materials.begin(),sceneBatch.materials.end(),[](const auto& m){return (m.flags&256u)!=0u;});
        auto& refraction=*native_view_resources_.at(cam_handle);
        const bool has_ssgi=!is_debug_mode && !loading_guard && true;
        const bool has_screen_trace=!is_debug_mode && !loading_guard &&
            (has_ssgi || true);
        if(has_screen_trace && !refraction.screen_depth) {
            refraction.screen_layout=OpticsDetail::ScreenSpaceLayout::make(hardware_->gbufferSize.x,hardware_->gbufferSize.y);
            refraction.screen_depth=make_storage_buffer<float>(refraction.screen_layout.hierarchy_words,
                "screen.depth_hierarchy",Horizon::CpuAccessMode::None);
        }
        if(has_ssgi && !refraction.ssgi_correction) {
            const auto layout=refraction.screen_layout;
            refraction.ssgi_correction=make_storage_image(layout.width,layout.height,Horizon::Format::RGBA16_FLOAT,"ssgi.raw");
            refraction.ssgi_filtered=make_storage_image(layout.width,layout.height,Horizon::Format::RGBA16_FLOAT,"ssgi.filtered");
            for(unsigned i=0;i<2;++i) {
                refraction.ssgi_history[i]=make_storage_image(layout.width,layout.height,Horizon::Format::RGBA16_FLOAT,"ssgi.history");
                refraction.ssgi_validation[i]=make_storage_image(layout.width,layout.height,Horizon::Format::RG32_UINT,"ssgi.validation");
            }
            refraction.ssgi_memory=Memory::GpuMemToken(Memory::ResKind::Texture,layout.bytes());
        }
        const uint32_t screen_current=refraction.screen_frame&1u,screen_previous=screen_current^1u;
        if(!has_ssgi)refraction.screen_history_valid=false;
        if(has_refraction) {
            const auto w=hardware_->gbufferSize.x,h=hardware_->gbufferSize.y;
            if(!refraction.opaque_visibility) {
                refraction.opaque_visibility=make_storage_image(w,h,Horizon::Format::RG32_UINT,"refraction.opaque_visibility");
                refraction.opaque_depth=make_depth_image(w,h,"refraction.opaque_depth");refraction.opaque_depth.set_clear_depth(0,0);
                refraction.exit_visibility=make_storage_image(w,h,Horizon::Format::RG32_UINT,"refraction.exit_visibility");
                refraction.exit_depth=make_depth_image(w,h,"refraction.exit_depth");refraction.exit_depth.set_clear_depth(1,0);
                refraction.opaque_color=make_storage_image(w,h,Horizon::Format::RGBA16_FLOAT,"refraction.opaque_color");
                refraction.front_snapshot=make_storage_image(w,h,Horizon::Format::RG32_UINT,"refraction.front_snapshot");
                refraction.refraction_memory=Memory::GpuMemToken(Memory::ResKind::Texture,uint64_t(w)*h*40);
            }
            auto& front=*hardware_->refractionFrontPipeline;
            bind_storage_image(front,front.pushConsts.visibilityIndex,hardware_->visibilityImage);
            bind_sampled_image(front,front.pushConsts.depthIndex,hardware_->depthImage);
            bind_storage_buffer(front,front.pushConsts.instances,sceneInstanceBuffer);
            bind_storage_image(front,front.pushConsts.outputIndex,refraction.front_snapshot);
            front.pushConsts.size=upload_value(hardware_->gbufferSize);
            front.pushConsts.instanceCount=native_instance_count;
            auto& opaque=*hardware_->refractionOpaquePipeline;
            auto& exit=*hardware_->refractionExitPipeline;
            opaque.clear_records();exit.clear_records();
            opaque.visibilityData=refraction.opaque_visibility;opaque.bind_depth_target(refraction.opaque_depth);
            exit.visibilityData=refraction.exit_visibility;exit.bind_depth_target(refraction.exit_depth);
            using Pipeline=Horizon::RasterizerPipeline<visibility_vert_glsl_t,visibility_frag_glsl_t>;
            for(const auto& draw:refraction_draws) {
                bool glass=(sceneBatch.materials[draw.material].flags&256u)!=0u;
                auto& pass=glass?exit:opaque;
                auto& pc=static_cast<Pipeline::VertexResourceBindings&>(pass).pushConsts;
                pc.modelMatrix=upload_value(draw.clip);pc.instanceID=draw.instance;
                pc.padding0=0u;pc.textureIndex=0u;pc.uniformBufferIndex=0u;
                if(glass) {
                    bind_storage_image(pass,pc.textureIndex,refraction.front_snapshot);
                    pc.uniformBufferIndex=sceneBatch.instances.at(draw.instance-1u).objectID;
                    pc.padding0=1u;
                }
                pass.record_indirect(draw.index,draw.vertex,draw.indirect,draw.params);
            }
        }
        const uint32_t native_lighting_flags = (should_run_ssao ? 1u : 0u) |
            ((!loading_guard && true) ? 2u : 0u) |
            (kSkyAmbientEnabled ? 4u : 0u) | 0u |
            (direct_lighting->authored_environment ? 16u : 0u) | (has_refraction ? 32u : 0u) | (has_ssgi ? 64u : 0u);
        const auto configure_lighting = [&](auto& lighting) {
        lighting.pushConsts.gbufferSize = upload_value(hardware_->gbufferSize);
        bind_storage_image(lighting, lighting.pushConsts.visibilityImageIndex, hardware_->visibilityImage);
        bind_sampled_image(lighting, lighting.pushConsts.depthImageIndex, hardware_->depthImage);
        bind_storage_buffer(lighting, lighting.pushConsts.instanceInfoBufferIndex, sceneInstanceBuffer);
        bind_storage_buffer(lighting, lighting.pushConsts.materialTableBufferIndex, sceneMaterialBuffer);
        bind_storage_buffer(lighting, lighting.pushConsts.directLightingBufferIndex, direct_lighting->data);
        bind_storage_buffer(lighting, lighting.pushConsts.uniformBufferIndex, sceneUboBuffer);
        bind_storage_buffer(lighting, lighting.pushConsts.shadowInfoBufferIndex, sceneShadowBuffer);
        bind_storage_buffer(lighting, lighting.pushConsts.skyIrradianceSHBufferIndex, hardware_->skyIrradianceSHBuffer);
        bind_storage_image(lighting, lighting.pushConsts.finalOutputImage, render_target);
        lighting.pushConsts.instanceCount = native_instance_count;
        lighting.pushConsts.materialCount = sceneMaterialCount;
        lighting.pushConsts.featureFlags=native_lighting_flags;
        bind_sampled_image(lighting, lighting.pushConsts.shadowMap0, hardware_->shadowCascadeImages[0]);
        bind_sampled_image(lighting, lighting.pushConsts.shadowMap1, hardware_->shadowCascadeImages[1]);
        bind_sampled_image(lighting, lighting.pushConsts.shadowMap2, hardware_->shadowCascadeImages[2]);
        bind_sampled_image(lighting, lighting.pushConsts.shadowMap3, hardware_->shadowCascadeImages[3]);
        lighting.pushConsts.sun_dir = upload_value(sun_dir);
        lighting.pushConsts.lightColor = upload_value(sun_color * sun_intensity);
        lighting.pushConsts.shadowEnabled = hardware_->shadowInfoBufferObjects.shadowEnabled;
        lighting.pushConsts.outputMode = static_cast<uint32_t>(camera->output_mode);
        lighting.pushConsts.ssaoStrength = kSsaoStrength;
        lighting.pushConsts.disableAlbedoSample = diag.disable_albedo_sample ? 1u : 0u;
        lighting.pushConsts.ssrMaxDistance = NATIVE_TRACE_DISTANCE;
        lighting.pushConsts.ssrThickness = NATIVE_TRACE_THICKNESS;
        lighting.pushConsts.ssrMaxRoughness = NATIVE_SSR_ROUGHNESS;
        lighting.pushConsts.ssrMaxSteps = NATIVE_REFRACTION_STEPS;
        };
        configure_lighting(lighting);
        if(has_ssgi)configure_lighting(*hardware_->screenGiPipeline);
        hardware_->shadowInfoBufferObjects.refractionDescriptors={};
        if(has_refraction) {
            hardware_->shadowInfoBufferObjects.refractionDescriptors={refraction.opaque_visibility.store_descriptor(),
                refraction.opaque_depth.store_descriptor(),refraction.exit_visibility.store_descriptor(),
                refraction.exit_depth.store_descriptor(),refraction.opaque_color.store_descriptor(),0,0,0};
        }
        if(has_ssgi)hardware_->shadowInfoBufferObjects.refractionDescriptors[5]=refraction.ssgi_correction.store_descriptor();
        auto& screen=hardware_->shadowInfoBufferObjects.screenDescriptors;
        screen={};
        if(has_screen_trace) {
            const auto layout=refraction.screen_layout;
            screen[0]=refraction.screen_depth.store_descriptor();
            screen[1]=layout.width;screen[2]=layout.height;screen[3]=layout.stride;
            screen[4]=layout.levels;screen[5]=refraction.screen_frame;
            if(has_ssgi) {
                screen[7]=refraction.ssgi_history[screen_previous].store_descriptor();
                screen[8]=refraction.ssgi_validation[screen_current].store_descriptor();
                screen[9]=refraction.ssgi_validation[screen_previous].store_descriptor();
                hardware_->shadowInfoBufferObjects.previousViewProjection=refraction.previous_view_projection;
                hardware_->shadowInfoBufferObjects.previousView=refraction.previous_view;
                hardware_->shadowInfoBufferObjects.inverseView=ktm::inverse(hardware_->uniformBufferObjects.eyeViewMatrix);
            }
        }

        // ================================================================
        // 5b. Sky �� SH9 projection (sky-driven ambient).
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
            kSkyAmbientEnabled &&
            (!sky_sh_initialized_ || sky_sig != sky_sh_signature_);
        if (sky_sh_needs_update) {
            auto& skySH = *hardware_->skySHProjectPipeline;
            bind_storage_buffer(skySH, skySH.pushConsts.outputBufferIndex, hardware_->skyIrradianceSHBuffer);
            skySH.pushConsts.sampleCount = 64u;
            skySH.pushConsts.sun_dir = upload_value(sun_dir);
            skySH.pushConsts.sky_intensity = sky_intensity;
        }

        // ================================================================
        // 6. Sky pass: atmospheric scattering + floor grid
        // ================================================================
        sky.pushConsts.gbufferSize = upload_value(hardware_->gbufferSize);
        bind_sampled_image(sky, sky.pushConsts.gbufferDepthImage, hardware_->depthImage);
        bind_storage_image(sky, sky.pushConsts.finalOutputImage, render_target);
        bind_storage_buffer(sky, sky.pushConsts.uniformBufferIndex, sceneUboBuffer);
        sky.pushConsts.sun_dir = upload_value(sun_dir);
        sky.pushConsts.floor_grid_enabled = floor_grid_enabled;
        sky.pushConsts.cameraFov = camera->fov;
        sky.pushConsts.sky_intensity = sky_intensity;
        sky.pushConsts.authoredEnvironment = direct_lighting->authored_environment ? 1u : 0u;
        bind_sampled_image(sky, sky.pushConsts.environmentImage, direct_lighting->environment);

        // ================================================================
        // 7. Tonemap pass: ACES filmic HDR �� LDR
        // ================================================================
        tonemap.pushConsts.gbufferSize = upload_value(hardware_->gbufferSize);
        bind_storage_image(tonemap, tonemap.pushConsts.inputImage, render_target);
        bind_storage_image(tonemap, tonemap.pushConsts.outputImage, render_target);
        tonemap.pushConsts.exposure = exposure;

        // ================================================================
        // 8. GPU sync & dispatch
        // ================================================================
        OpticsDetail::NativeLightFieldInputs light_field_inputs;
        light_field_inputs.add(native_shadows_enabled ? diag.shadow_cascade_mask : 0u);
        if (!direct_lighting->authored_environment || native_shadows_enabled) {
            for (float value : {sun_dir.x, sun_dir.y, sun_dir.z, sky_intensity})
                light_field_inputs.add(value);
        }
        if (native_shadows_enabled) {
            // RSM flux depends on solar radiance, material, geometry and cascade projection.
            for (float value : {sun_color.x * sun_intensity, sun_color.y * sun_intensity,
                                sun_color.z * sun_intensity, camera->position.x,
                                camera->position.y, camera->position.z,
                                camera->forward.x, camera->forward.y, camera->forward.z})
                light_field_inputs.add(value);
            for (unsigned cascade = 0; cascade < kShadowCascadeCount; ++cascade) {
                for (unsigned c = 0; c < 4; ++c)
                    for (unsigned r = 0; r < 4; ++r)
                        light_field_inputs.add(shadow_cascade_views[cascade].light_view_proj[c][r]);
                light_field_inputs.add(shadow_splits[cascade]);
                light_field_inputs.add(shadow_cascade_views[cascade].orthographic_width);
                light_field_inputs.add(shadow_cascade_views[cascade].orthographic_height);
            }
            // Resident shadow casters and asynchronous shadow LODs can
            // change without replacing the environment image.
            light_field_inputs.add(static_cast<uint32_t>(shadow_casters.size()));
            for (const auto& caster : shadow_casters) {
                light_field_inputs.add(static_cast<uint32_t>(caster.material_revision));
                light_field_inputs.add(static_cast<uint32_t>(caster.material_revision>>32));
                light_field_inputs.add(static_cast<uint32_t>(caster.pose_revision));
                light_field_inputs.add(static_cast<uint32_t>(caster.pose_revision>>32));
                light_field_inputs.add(caster.cascade_visibility_mask);
                for (unsigned c = 0; c < 4; ++c)
                    for (unsigned r = 0; r < 4; ++r)
                        light_field_inputs.add(caster.model_matrix[c][r]);
                for (const auto& slots : caster.cascade_slots) {
                    light_field_inputs.add(static_cast<uint32_t>(slots.size()));
                    for (const auto& slot : slots) {
                        light_field_inputs.add(slot.valid ? 1u : 0u);
                        light_field_inputs.add(slot.mesh_index);
                        light_field_inputs.add(slot.index_count);
                        light_field_inputs.add(slot.bounds_valid ? 1u : 0u);
                        for (unsigned k = 0; k < 3; ++k) {
                            light_field_inputs.add(slot.bounds_min[k]);
                            light_field_inputs.add(slot.bounds_max[k]);
                        }
                        for (unsigned c = 0; c < 4; ++c)
                            for (unsigned r = 0; r < 4; ++r)
                                light_field_inputs.add(slot.instance_transform[c][r]);
                        light_field_inputs.meshes.push_back(slot.geo.vertex);
                        light_field_inputs.meshes.push_back(slot.geo.index);
                    }
                }
            }
        }
        for(const auto& material:rsm_materials) {
            std::array<uint32_t,32> words;
            std::memcpy(words.data(),&material,sizeof(material));
            for(auto word:words)light_field_inputs.add(word);
        }
        for(const auto& program:rsm_material_programs)light_field_inputs.meshes.push_back(program);
        light_field_inputs.images=rsm_material_images;
        const bool reset_rsm=!hardware_->lightFieldCache.owned_by(view.scene_handle,view.camera_handle);
        const bool light_field_changed=reset_rsm ||
            !hardware_->lightFieldCache.matches(direct_lighting, light_field_inputs);
        const bool update_shadow_maps = native_shadows_enabled && !geometry_loading && light_field_changed;
        // Receiver validation rejects changed surfaces locally. Updating the
        // light field does not discard every static receiver's temporal history.
        screen[6]=(has_ssgi && !geometry_loading && refraction.screen_history_valid &&
            refraction.screen_had_refraction==has_refraction) ? 1u : 0u;
        (void)write_object_bytes(sceneShadowBuffer,hardware_->shadowInfoBufferObjects);
        // dispatch ������ workgroup ����(ԭ���� vkCmdDispatch)����Щ�ӳ�ͨ��
        // ȫ�� local_size 8x8, �� dispatch_groups �����ػ��� ceil(w/8)xceil(h/8);
        // ����ȡ�Թ��߷������ʵ local size(���� Horizon SPIR-V patch ����)��
        // ����ֱ�Ӵ������� gbufferSize �ᵼ�� 64x ����(Խ�� guard ʹ�����ԶԵ����˷�)��
        // dispatch_extent replaces dispatch_groups in new Horizon
        auto actor_pick_request = take_pending_actor_pick(cam_handle, *camera);
        std::optional<OpticsDetail::ActorPickReadbackWorker::Reservation>
            actor_pick_reservation;
        std::optional<Horizon::HardwareBuffer> actor_pick_result_buffer;

        const auto reject_actor_pick = [&](std::string_view reason) {
            if (actor_pick_request) {
                CFW_LOG_WARNING(
                    "OpticsSystem: Actor pick request {} was not submitted: {}",
                    actor_pick_request->request_id,
                    reason);
                fail_actor_pick(*actor_pick_request);
                actor_pick_request.reset();
            }
            actor_pick_result_buffer.reset();
            actor_pick_reservation.reset();
        };

        if (actor_pick_request) {
            if (diag.skip_deferred_compute) {
                reject_actor_pick("deferred compute is disabled");
            } else if (!actor_pick_readback_worker_ ||
                       !(actor_pick_reservation =
                             actor_pick_readback_worker_->reserve())) {
                reject_actor_pick("readback queue is full or unavailable");
            } else {
                try {
                    actor_pick_result_buffer.emplace(
                        make_storage_buffer<std::uint32_t>(
                            1, "optics.actor_pick.request",
                            Horizon::CpuAccessMode::ReadWrite));
                    auto& actorPick = *hardware_->actorPickPipeline;
                    actorPick.pushConsts.pixel = upload_value(
                        ktm::uvec2{actor_pick_request->x, actor_pick_request->y});
                    bind_storage_image(actorPick, actorPick.pushConsts.visibilityImageIndex, hardware_->visibilityImage);
                    bind_storage_buffer(actorPick, actorPick.pushConsts.outputBufferIndex, *actor_pick_result_buffer);
                } catch (const std::exception& error) {
                    CFW_LOG_ERROR(
                        "OpticsSystem: Failed to create actor pick result buffer: {}",
                        error.what());
                    reject_actor_pick("result buffer creation failed");
                }
            }
        }


        Horizon::SubmitReceipt latest_submit_receipt;
        {
            const auto native_submit_start = PerfClock::now();
            double measurement_pause_ms=0;
            std::optional<Horizon::HardwareStream> stream{std::in_place,hardware_->executor};
            bool native_submission_cancelled = false;

            if (update_shadow_maps) hardware_->lightFieldCache.begin_update();

            *stream << visibility.extent(hardware_->gbufferSize.x, hardware_->gbufferSize.y);
            if(has_refraction) {
                *stream << hardware_->refractionFrontPipeline->dispatch_extent(hardware_->gbufferSize.x,hardware_->gbufferSize.y);
                *stream << hardware_->refractionOpaquePipeline->extent(hardware_->gbufferSize.x,hardware_->gbufferSize.y);
                hardware_->refractionExitPipeline->extent(hardware_->gbufferSize.x,hardware_->gbufferSize.y);
                *stream << RenderDetail::IndirectRaster{&*hardware_->refractionExitPipeline,{refraction.front_snapshot}};
            }
                if (update_shadow_maps) {
                    const auto native_shadow_start = PerfClock::now();
                    std::array<std::exception_ptr, kShadowCascadeCount>
                        cascade_errors{};
                    oneapi::tbb::task_group cascade_tasks;
                    Corona::Systems::OpticsDetail::for_each_enabled_shadow_cascade(
                        diag.shadow_cascade_mask,
                        kShadowCascadeCount,
                        [&](std::uint32_t cascade) {
                            cascade_tasks.run([&, cascade] {
                                try {
                                    record_shadow_cascade(
                                        hardware_->shadowInfoBufferObjects
                                            .lightViewProj[cascade],
                                        hardware_->shadowCascadeImages[cascade],
                                        cascade);
                                } catch (...) {
                                    cascade_errors[cascade] =
                                        std::current_exception();
                                }
                            });
                        });
                    cascade_tasks.wait();

                    Corona::Systems::OpticsDetail::for_each_enabled_shadow_cascade(
                        diag.shadow_cascade_mask,
                        kShadowCascadeCount,
                        [&](std::uint32_t cascade) {
                            if (!cascade_errors[cascade]) return;
                            native_submission_cancelled = true;
                            try {
                                std::rethrow_exception(cascade_errors[cascade]);
                            } catch (const std::exception& error) {
                                CFW_LOG_ERROR(
                                    "OpticsSystem: shadow cascade recording failed "
                                    "(cascade={}, error={}); cancelling Native frame",
                                    cascade,
                                    error.what());
                            } catch (...) {
                                CFW_LOG_ERROR(
                                    "OpticsSystem: shadow cascade recording failed "
                                    "(cascade={}, unknown error); cancelling Native frame",
                                    cascade);
                            }
                        });

                    if (!native_submission_cancelled) {
                        Corona::Systems::OpticsDetail::for_each_enabled_shadow_cascade(
                            diag.shadow_cascade_mask,
                            kShadowCascadeCount,
                            [&](std::uint32_t cascade) {
                                auto& cascade_pipeline =
                                    *shadow_pipelines[cascade];
                                cascade_pipeline.extent(kShadowMapSize, kShadowMapSize);
                                *stream << RenderDetail::IndirectRaster{&cascade_pipeline,rsm_material_images,rsm_material_programs};
                            });
                    }
                    native_shadow_ms =
                        elapsed_ms(native_shadow_start, PerfClock::now());
                }
            if (!native_submission_cancelled && !diag.skip_deferred_compute) {
                if (!is_debug_mode && sky_sh_needs_update) {
                    *stream << hardware_->skySHProjectPipeline->dispatch_extent(1, 1);
                }
                lighting.dispatch_extent(hardware_->gbufferSize.x, hardware_->gbufferSize.y);
                RenderDetail::IndirectDispatch material_dispatch{&lighting};
                if(native_shadows_enabled)for(uint32_t cascade=0;cascade<kShadowCascadeCount;++cascade) {
                    material_dispatch.images.push_back(hardware_->rsmPositions[cascade]);
                    material_dispatch.images.push_back(hardware_->rsmNormals[cascade]);
                    material_dispatch.images.push_back(hardware_->rsmFluxes[cascade]);
                }
                material_dispatch.images.push_back(direct_lighting->environment);
                std::unordered_set<uint32_t> material_programs, material_images;
                for (const auto& appearance : sceneBatch.resource_keep_alive->appearances) {
                    if (material_programs.insert(appearance->program.store_descriptor()).second)
                        material_dispatch.buffers.push_back(appearance->program);
                    for (const auto& image : appearance->images)
                        if (material_images.insert(image->image.store_descriptor()).second)
                            material_dispatch.images.push_back(image->image);
                }
                if(has_screen_trace) {
                    auto& depth=*hardware_->screenDepthPipeline;
                    depth.pc.size=upload_value(hardware_->gbufferSize);
                    bind_sampled_image(depth,depth.pc.depthIndex,has_refraction?refraction.opaque_depth:hardware_->depthImage);
                    bind_storage_buffer(depth,depth.pc.pyramidIndex,refraction.screen_depth);
                    bind_storage_buffer(depth,depth.pc.uniformIndex,sceneUboBuffer);
                    for(uint32_t level=1;level<refraction.screen_layout.levels;++level) {
                        depth.pc.level=level;
                        const auto scale=1u<<level;
                        depth.dispatch_extent((hardware_->gbufferSize.x+scale-1)/scale,(hardware_->gbufferSize.y+scale-1)/scale);
                        RenderDetail::IndirectDispatch dispatch{&depth};
                        *stream << dispatch;
                    }
                    material_dispatch.buffers.push_back(refraction.screen_depth);
                }
                if(has_ssgi) {
                    const auto layout=refraction.screen_layout;
                    auto& gi=*hardware_->screenGiPipeline;
                    if(has_refraction) {
                        bind_storage_image(gi,gi.pushConsts.visibilityImageIndex,refraction.opaque_visibility);
                        bind_sampled_image(gi,gi.pushConsts.depthImageIndex,refraction.opaque_depth);
                    }
                    bind_storage_image(gi,gi.pushConsts.finalOutputImage,refraction.ssgi_correction);
                    gi.dispatch_extent(layout.width,layout.height);
                    auto dispatch=material_dispatch;
                    dispatch.pipeline=&gi;
                    dispatch.writable_images={refraction.ssgi_validation[screen_current]};
                    *stream << dispatch;

                    auto& filter=*hardware_->screenFilterPipeline;
                    filter.pushConsts.gbufferSize=upload_value(hardware_->gbufferSize);
                    bind_storage_buffer(filter,filter.pushConsts.uniformBufferIndex,sceneUboBuffer);
                    bind_storage_buffer(filter,filter.pushConsts.shadowInfoBufferIndex,sceneShadowBuffer);
                    filter.dispatch_extent(layout.width,layout.height);
                    RenderDetail::IndirectDispatch filtering{&filter};
                    filtering.images={refraction.ssgi_validation[screen_current],refraction.ssgi_validation[screen_previous],
                        refraction.ssgi_history[screen_previous]};
                    const auto run_filter=[&](uint32_t mode,const Horizon::HardwareImage& input,const Horizon::HardwareImage& output) {
                        filter.pushConsts.mode=mode;
                        bind_storage_image(filter,filter.pushConsts.inputIndex,input);
                        bind_storage_image(filter,filter.pushConsts.outputIndex,output);
                        *stream << filtering;
                    };
                    run_filter(0,refraction.ssgi_correction,refraction.ssgi_history[screen_current]);
                    run_filter(1,refraction.ssgi_history[screen_current],refraction.ssgi_filtered);
                    run_filter(2,refraction.ssgi_filtered,refraction.ssgi_correction);
                    material_dispatch.images.push_back(refraction.ssgi_correction);
                    material_dispatch.images.push_back(refraction.ssgi_validation[screen_current]);
                }
                if(has_refraction) {
                    // Immutable opaque background: refraction never reads its own output.
                    const uint32_t flags=(should_run_ssao?1u:0u) |
                        ((!loading_guard && true)?2u:0u) |
                        (kSkyAmbientEnabled?4u:0u) | (direct_lighting->authored_environment?16u:0u) | (has_ssgi?64u:0u);
                    lighting.pushConsts.featureFlags=flags;
                    bind_storage_image(lighting,lighting.pushConsts.visibilityImageIndex,refraction.opaque_visibility);
                    bind_sampled_image(lighting,lighting.pushConsts.depthImageIndex,refraction.opaque_depth);
                    bind_storage_image(lighting,lighting.pushConsts.finalOutputImage,refraction.opaque_color);
                    *stream << material_dispatch;
                    lighting.pushConsts.featureFlags=flags|32u|0u;
                    bind_storage_image(lighting,lighting.pushConsts.visibilityImageIndex,hardware_->visibilityImage);
                    bind_sampled_image(lighting,lighting.pushConsts.depthImageIndex,hardware_->depthImage);
                    bind_storage_image(lighting,lighting.pushConsts.finalOutputImage,render_target);
                    for(const auto& image:{refraction.opaque_visibility,refraction.opaque_depth,refraction.exit_visibility,refraction.exit_depth,refraction.opaque_color})
                        material_dispatch.images.push_back(image);
                }
                *stream << material_dispatch;
                if (!is_debug_mode) {
                    *stream << sky.dispatch_extent(hardware_->gbufferSize.x, hardware_->gbufferSize.y)
                           << tonemap.dispatch_extent(hardware_->gbufferSize.x, hardware_->gbufferSize.y);
                }
            }

            if (native_submission_cancelled) {
                reject_actor_pick("native frame submission was cancelled");
                continue;
            }

            if (actor_pick_request) {
                *stream << hardware_->actorPickPipeline->dispatch_extent(1, 1);
            }

            if (!is_debug_mode) {
                const auto ui_state =
                    SharedDataHub::instance().viewport_ui_state(cam_handle);
                presented_target = compose_surface_ui_overlay(
                    *stream,
                    cam_handle, *camera, scene, target, render_target,
                    ui_state.mode, ui_state.calibration, frame_index);
            }

            // Recorded draws retain resource handles; no scene/asset contents
            // are read by the CPU after this point. Let editors/Geometry write
            // while the GPU executes, without making a scene snapshot.
            locked->scene = {};
            const auto native_commit_start = PerfClock::now();
            // ���������õ� buffer �� GPU ��ɣ�executor ������Щ busy �ڱ���
            // ֱ���� submission �� timeline semaphore ��ɲ� retire �����ʱ busy
            // use_count ��� 1���ط��ɸ��ø� buffer�������������Ǿ����Ĺؼ���
            *stream << RetainSubmission{vpLease.busy}
                   << RetainSubmission{uboLease.busy}
                   << RetainSubmission{shadowLease.busy}
                   << RetainSubmission{instLease.busy}
                   << RetainSubmission{matLease.busy}
                   << RetainSubmission{sceneBatch.resource_keep_alive};
            latest_submit_receipt = *stream << Horizon::commit();
            hardware_->last_receipt = latest_submit_receipt;
            if(has_ssgi && !diag.skip_deferred_compute) {
                refraction.previous_view_projection=hardware_->vpUniformBufferObjects.viewProjMatrix;
                refraction.previous_view=hardware_->uniformBufferObjects.eyeViewMatrix;
                refraction.screen_history_valid=true;
                refraction.screen_had_refraction=has_refraction;
                ++refraction.screen_frame;
            } else refraction.screen_history_valid=false;
            if (update_shadow_maps) {
                hardware_->lightFieldCache.submitted(direct_lighting, std::move(light_field_inputs),
                    view.scene_handle,view.camera_handle);
            }
            native_commit_ms = elapsed_ms(native_commit_start, PerfClock::now());
            native_submit_ms = elapsed_ms(native_submit_start, PerfClock::now());
            if (env_flag_enabled("CORONA_OPTICS_MEASURE_RENDER_TIME")) {
                // Opt-in regression measurement: command recording through GPU
                // completion, excluding scene loading, frame pacing and PNG IO.
                hardware_->executor.wait_completed(latest_submit_receipt);
                CFW_LOG_INFO("NativeRenderTiming camera={} extent={}x{} render_ms={:.3f} gi=RSM+SSGI rsm_updated={}",
                    cam_handle, camera->width, camera->height,
                    elapsed_ms(native_submit_start, PerfClock::now())-measurement_pause_ms,
                    update_shadow_maps ? 1 : 0);
            }
        }
        if (hardware_) {
            hardware_->native_frame_throttle.submitted(latest_submit_receipt);
        }

        if (!is_debug_mode && sky_sh_needs_update) {
            sky_sh_signature_ = sky_sig;
            sky_sh_initialized_ = true;
        }

        if (actor_pick_request && actor_pick_result_buffer && actor_pick_reservation) {
            enqueue_actor_pick_readback(*actor_pick_request,
                                        std::move(*actor_pick_result_buffer),
                                        latest_submit_receipt,
                                        sceneBatch.actorHandles,
                                        sceneBatch.nodeIndices,
                                        std::move(*actor_pick_reservation));
        }


            // �������������ϳ�ʱ commit�������� follow-actor ʱ��ԭ����Ϊ
            // ����ʱ render_target �����Ϸ� scene pass �ύ����



        Corona::Systems::OpticsDetail::NativePerfSample native_perf_sample;
        native_perf_sample.total_ms = elapsed_ms(native_frame_start, PerfClock::now());
        native_perf_sample.throttle_wait_ms = native_throttle_wait_ms;
        native_perf_sample.collect_ms = native_collect_ms;
        native_perf_sample.submit_ms = native_submit_ms;
        native_perf_sample.shadow_record_ms = native_shadow_ms;
        native_perf_sample.commit_ms = native_commit_ms;
        native_perf_sample.visibility_draws = native_visibility_draws;
        native_perf_sample.visibility_indices = native_visibility_indices;
        native_perf_sample.cascade_draws = native_cascade_draws;
        native_perf_sample.cascade_indices = native_cascade_indices;
        native_perf_sample.output_width = camera->width;
        native_perf_sample.output_height = camera->height;
        native_perf_sample.instance_count = native_instance_count;
        native_perf_sample.shadows_enabled = native_shadows_enabled;
        native_perf_sample.debug_mode = native_debug_mode;
        native_perf_sample.sky_ambient_enabled =
            !native_debug_mode && kSkyAmbientEnabled;
        native_perf_sample.sky_sh_updated =
            !native_debug_mode && sky_sh_needs_update;
        record_optics_native_perf(native_perf_sample);

        if (offscreen_screenshot) {
            process_pending_screenshots(cam_handle, *presented_target);
            continue;
        }

        process_pending_screenshots(cam_handle, *presented_target);

        // ��ʾ������Լ� surface ����������� DisplaySystem���� surface ���֣���
        const ImagePixelExtent presented_extent{target.width, target.height};
        const auto viewport = optics_event_viewport(*camera, presented_extent);
        if (auto image_device =
                SharedDataHub::instance().image_storage().acquire_write(target.image_handle)) {
            if (latest_submit_receipt.serial == 0) {
                CFW_LOG_WARNING(
                    "OpticsSystem: publishing native frame with empty submit receipt "
                    "(camera={}, surface={}, image_handle={}, frame={}, extent={}x{})",
                    cam_handle,
                    surface,
                    target.image_handle,
                    frame_index,
                    camera->width,
                    camera->height);
            }
            image_device->image = *presented_target;
            image_device->submit_receipt = latest_submit_receipt;
            image_device->metadata = {frame_index,
                                      presented_extent.width,
                                      presented_extent.height,
                                      viewport.x,
                                      viewport.y,
                                      viewport.width,
                                      viewport.height};
        }

        if (auto* event_bus = context()->event_bus()) {
            event_bus->publish<Events::OpticsFrameReadyEvent>({surface,
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
    }
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
    auto& uiVisibility = *hardware_->uiVisibilityPipeline;
    auto& opticsOverlay = *hardware_->opticsOverlayPipeline;
    auto& opticsGizmo = *hardware_->opticsGizmoPipeline;
    auto& opticsCursor = *hardware_->opticsCursorPipeline;
    auto& opticsUiWarp = *hardware_->opticsUiWarpPipeline;
    auto& opticsComposite = *hardware_->opticsCompositePipeline;

    // 计算光标渲染工作组数量（本地大小为 8x8，根据 Horizon SPIR-V patch）
    uint32_t cursorDispatchWidth = hardware_->gbufferSize.x;
    uint32_t cursorDispatchHeight = hardware_->gbufferSize.y;

    // follow-camera UI 使用正交投影并且跟随特定 actor 的世界空间位置而源自相机。
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
    // UI overlay pass ͬ��ÿ�����д���� buffer���볡�� pass ͬһ��������Ϊ�����á�
    // keep_alive ���ڴ���� stream �ϣ��뱾 overlay �� dispatch ͬһ submission����
    auto uiVpLease = hardware_->uiVpUniformBufferPool.acquire(
        [] { return make_storage_buffer<Hardware::VPUniformBufferObject>(1, "optics.ui_vp_uniform.pool"); });
    Horizon::HardwareBuffer& uiVpBuffer = *uiVpLease.buffer;
    (void)write_object_bytes(uiVpBuffer,
                             hardware_->vpUniformBufferObjects);
    const uint32_t uiVpDescriptor = FramePlaceBufferPool::store_descriptor(uiVpBuffer);
    stream << RetainSubmission{uiVpLease.busy};


    RenderInstanceBatch uiBatch;
    std::vector<Corona::Horizon::HardwareBuffer> ui_indirect_buffers;
    const bool has_follow_camera_instances =
        collect_actor_instances_for_visibility(scene, uiVisibility, uiVpDescriptor,
                                               /*follow_camera_pass=*/true,
                                               &camera_basis, uiBatch, geometry_system_, frame_index,
                                               ui_indirect_buffers);
    stream << RetainSubmission{uiBatch.resource_keep_alive};

    const bool stereo_ui = mode == ViewportUiMode::Stereo3D;
    const bool cursor_icon_ready = stereo_ui && ensure_cursor_icon_texture();
    const auto cursor_it = viewport_cursor_states_.find(camera_handle);
    const bool cursor_visible =
        stereo_ui && cursor_icon_ready && cursor_it != viewport_cursor_states_.end() && cursor_it->second.visible &&
        cursor_it->second.cursor_shape != ViewportUiCursorShape::Hidden &&
        std::isfinite(cursor_it->second.x) && std::isfinite(cursor_it->second.y);
    const ViewportCursorState* cursor_state =
        cursor_visible ? &cursor_it->second : nullptr;

    const auto gizmo_state =
        SharedDataHub::instance().viewport_gizmo_state(camera_handle);
    OpticsDetail::ViewportGizmoLayout gizmo_layout;
    bool gizmo_visible = false;
    if (gizmo_state.target.actor_handle != 0 && ensure_gizmo_axis_textures()) {
        auto& hub = SharedDataHub::instance();
        const auto transform_handle =
            hub.resolve_actor_primary_transform_handle(
                gizmo_state.target.actor_handle);
        if (transform_handle) {
            if (auto transform =
                    hub.model_transform_storage().try_acquire_read(
                        *transform_handle)) {
                gizmo_layout = OpticsDetail::make_viewport_gizmo_layout(
                    camera.compute_view_proj_matrix(),
                    transform->position,
                    hardware_->gbufferSize.x,
                    hardware_->gbufferSize.y,
                    128.0f);
                gizmo_visible = gizmo_layout.visible;
            }
        }
    }

    const auto ui_instance_count = static_cast<std::uint32_t>(uiBatch.instances.size());
    auto& ui_log_state = ui_pass_log_states_[camera_handle];
    if (!ui_log_state.has_state ||
        ui_log_state.has_follow_camera_instances != has_follow_camera_instances ||
        ui_log_state.stereo_ui != stereo_ui ||
        ui_log_state.cursor_visible != cursor_visible ||
        ui_log_state.gizmo_target != (gizmo_state.target.actor_handle != 0) ||
        ui_log_state.gizmo_visible != gizmo_visible ||
        ui_log_state.instance_count != ui_instance_count ||
        ui_log_state.width != hardware_->gbufferSize.x ||
        ui_log_state.height != hardware_->gbufferSize.y) {
        ui_log_state = UiPassLogState{
            .has_state = true,
            .has_follow_camera_instances = has_follow_camera_instances,
            .stereo_ui = stereo_ui,
            .cursor_visible = cursor_visible,
            .gizmo_target = gizmo_state.target.actor_handle != 0,
            .gizmo_visible = gizmo_visible,
            .instance_count = ui_instance_count,
            .width = hardware_->gbufferSize.x,
            .height = hardware_->gbufferSize.y,
        };
        CFW_LOG_INFO("Optics UI pass: camera={} mode={} follow_camera_instances={} gizmo_target={} gizmo={} cursor={} output={}x{} warp={}",
                     camera_handle,
                     stereo_ui ? "stereo3d" : "flat2d",
                     ui_instance_count,
                     gizmo_state.target.actor_handle != 0 ? "set" : "none",
                     gizmo_visible ? "visible" : "hidden",
                     cursor_visible ? "visible" : "hidden",
                     hardware_->gbufferSize.x,
                     hardware_->gbufferSize.y,
                     (stereo_ui &&
                      (has_follow_camera_instances || gizmo_visible || cursor_visible))
                         ? "submitted"
                         : "skipped");
    }

    if (!has_follow_camera_instances && !gizmo_visible && !cursor_visible) {
        return &background;
    }

    if (has_follow_camera_instances) {
        ensure_ui_view_resources(camera_handle, target.width, target.height, frame_index);
        uiVisibility.visibilityData = hardware_->uiVisibilityImage;
        uiVisibility.bind_depth_target(hardware_->uiDepthImage);
    }
    if (!target.ui_overlay) {
        target.ui_overlay = make_storage_image(target.width, target.height, Horizon::Format::RGBA16_FLOAT, "optics.surface_overlay");
    }
    if (stereo_ui && !target.ui_warped_overlay) {
        target.ui_warped_overlay = make_storage_image(target.width, target.height, Horizon::Format::RGBA16_FLOAT, "optics.surface_warped_overlay");
    }
    bool follow_camera_overlay_ready = has_follow_camera_instances;
    if (has_follow_camera_instances) {
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
        follow_camera_overlay_ready =
            upload_instance_tables(uiBatch,
                                   *hardware_,
                                   uiInstanceBuffer,
                                   uiInstanceCapacity,
                                   uiMaterialBuffer,
                                   uiMaterialCapacity,
                                   "optics.ui.pool");
        if (!follow_camera_overlay_ready) {
            uiVisibility.clear_records();
            uiBatch.clear();
        }
        stream << RetainSubmission{uiInstLease.busy} << RetainSubmission{uiMatLease.busy};

        if (follow_camera_overlay_ready) {
            opticsOverlay.pushConsts.gbufferSize = upload_value(hardware_->gbufferSize);
            bind_storage_image(opticsOverlay, opticsOverlay.pushConsts.visibilityImageIndex, hardware_->uiVisibilityImage);
            bind_storage_buffer(opticsOverlay, opticsOverlay.pushConsts.instanceInfoBufferIndex, uiInstanceBuffer);
            bind_storage_buffer(opticsOverlay, opticsOverlay.pushConsts.materialTableBufferIndex, uiMaterialBuffer);
            opticsOverlay.pushConsts.instanceCount =
                static_cast<std::uint32_t>(uiBatch.instances.size());
            opticsOverlay.pushConsts.materialCount =
                static_cast<std::uint32_t>(uiBatch.materials.size());
            bind_storage_buffer(opticsOverlay, opticsOverlay.pushConsts.vpBufferIndex, uiVpBuffer);
            bind_storage_image(opticsOverlay, opticsOverlay.pushConsts.outputImage, target.ui_overlay);
        }
    }

    if (!follow_camera_overlay_ready && !gizmo_visible && !cursor_visible) {
        return &background;
    }

    if (gizmo_visible) {
        bind_storage_image(opticsGizmo, opticsGizmo.pushConsts.outputImage, target.ui_overlay);
        opticsGizmo.pushConsts.outputWidth = hardware_->gbufferSize.x;
        opticsGizmo.pushConsts.outputHeight = hardware_->gbufferSize.y;
        opticsGizmo.pushConsts.originX = 0u;
        opticsGizmo.pushConsts.originY = 0u;
        opticsGizmo.pushConsts.gizmoOrigin = upload_value(gizmo_layout.origin);
        opticsGizmo.pushConsts.xDirection =
            upload_value(gizmo_layout.axes[0].direction);
        opticsGizmo.pushConsts.yDirection =
            upload_value(gizmo_layout.axes[1].direction);
        opticsGizmo.pushConsts.zDirection =
            upload_value(gizmo_layout.axes[2].direction);
        bind_sampled_image(opticsGizmo, opticsGizmo.pushConsts.xImage, hardware_->gizmoAxisImages[0]);
        bind_sampled_image(opticsGizmo, opticsGizmo.pushConsts.yImage, hardware_->gizmoAxisImages[1]);
        bind_sampled_image(opticsGizmo, opticsGizmo.pushConsts.zImage, hardware_->gizmoAxisImages[2]);
        opticsGizmo.pushConsts.activeAxis =
            static_cast<std::uint32_t>(gizmo_state.active_axis);
        opticsGizmo.pushConsts.hoverAxis =
            static_cast<std::uint32_t>(gizmo_state.hover_axis);
        opticsGizmo.pushConsts.preserveExisting =
            follow_camera_overlay_ready ? 1u : 0u;
        opticsGizmo.pushConsts.axisLength = 128.0f;
        const auto& sprite_metadata =
            OpticsDetail::kViewportGizmoSpriteMetadata;
        opticsGizmo.pushConsts.xSourceAnchorTip =
            upload_value(ktm::fvec4{
                sprite_metadata[0].anchor.x, sprite_metadata[0].anchor.y,
                sprite_metadata[0].tip.x, sprite_metadata[0].tip.y});
        opticsGizmo.pushConsts.ySourceAnchorTip =
            upload_value(ktm::fvec4{
                sprite_metadata[1].anchor.x, sprite_metadata[1].anchor.y,
                sprite_metadata[1].tip.x, sprite_metadata[1].tip.y});
        opticsGizmo.pushConsts.zSourceAnchorTip =
            upload_value(ktm::fvec4{
                sprite_metadata[2].anchor.x, sprite_metadata[2].anchor.y,
                sprite_metadata[2].tip.x, sprite_metadata[2].tip.y});
    }

    if (cursor_visible && cursor_state != nullptr) {
        const bool preserve_existing_overlay =
            follow_camera_overlay_ready || gizmo_visible;
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

        bind_storage_image(opticsCursor, opticsCursor.pushConsts.outputImage, target.ui_overlay);
        opticsCursor.pushConsts.outputWidth = hardware_->gbufferSize.x;
        opticsCursor.pushConsts.outputHeight = hardware_->gbufferSize.y;
        opticsCursor.pushConsts.originX = cursor_origin_x;
        opticsCursor.pushConsts.originY = cursor_origin_y;
        opticsCursor.pushConsts.cursorX = cursor_state->x;
        opticsCursor.pushConsts.cursorY = cursor_state->y;
        opticsCursor.pushConsts.cursorShape =
            static_cast<std::uint32_t>(cursor_state->cursor_shape);
        bind_sampled_image(opticsCursor, opticsCursor.pushConsts.cursorImage, hardware_->cursorIconImage);
        opticsCursor.pushConsts.cursorSize = 48.0f;
        opticsCursor.pushConsts.preserveExisting = preserve_existing_overlay ? 1u : 0u;
        cursorDispatchWidth = cursor_width;
        cursorDispatchHeight = cursor_height;
    }

    if (stereo_ui) {
        bind_storage_image(opticsUiWarp, opticsUiWarp.pushConsts.inputImage, target.ui_overlay);
        bind_storage_image(opticsUiWarp, opticsUiWarp.pushConsts.outputImage, target.ui_warped_overlay);
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
    }

    bind_storage_image(opticsComposite, opticsComposite.pushConsts.bgImage, background);
    bind_storage_image(opticsComposite, opticsComposite.pushConsts.fgImage, stereo_ui ? target.ui_warped_overlay : target.ui_overlay);
    bind_storage_image(opticsComposite, opticsComposite.pushConsts.outputImage, background);
    opticsComposite.pushConsts.outputWidth = hardware_->gbufferSize.x;
    opticsComposite.pushConsts.outputHeight = hardware_->gbufferSize.y;


    if (follow_camera_overlay_ready) {
        stream << uiVisibility.extent(hardware_->gbufferSize.x, hardware_->gbufferSize.y)
               << opticsOverlay.dispatch_extent(hardware_->gbufferSize.x, hardware_->gbufferSize.y);
    }
    if (gizmo_visible) {
        stream << opticsGizmo.dispatch_extent(hardware_->gbufferSize.x, hardware_->gbufferSize.y);
    }
    if (cursor_visible) {
        stream << opticsCursor.dispatch_extent(cursorDispatchWidth, cursorDispatchHeight);
    }
    if (stereo_ui) {
        stream << opticsUiWarp.dispatch_extent(hardware_->gbufferSize.x, hardware_->gbufferSize.y);
    }
    stream << opticsComposite.dispatch_extent(hardware_->gbufferSize.x, hardware_->gbufferSize.y);
    // ע�⣺�˴��� commit���ɵ��÷��ں���ʱ��ͳһ�ύ��

    return &background;
}

std::optional<OpticsSystem::ActorPickRequest> OpticsSystem::take_pending_actor_pick(
    std::uintptr_t camera_handle, const CameraDevice& camera) {
    auto& hub = SharedDataHub::instance();
    const auto command = hub.take_actor_pick_request(camera_handle);
    if (!command) {
        return std::nullopt;
    }

    const auto pick_handle = camera.actor_pick_handle;
    const auto camera_width = camera.width;
    const auto camera_height = camera.height;
    if (pick_handle == 0 || camera_width == 0 || camera_height == 0) {
        hub.enqueue_actor_pick_completion({camera_handle, command->scene_id, command->request_id,
                                           "error", 0, command->x, command->y});
        return std::nullopt;
    }

    auto pick = hub.actor_pick_storage().try_acquire_write(pick_handle);
    if (!pick) {
        hub.enqueue_actor_pick_completion({camera_handle, command->scene_id, command->request_id,
                                           "error", 0, command->x, command->y});
        return std::nullopt;
    }

    ActorPickRequest request;
    request.pick_handle = pick_handle;
    request.camera_handle = camera_handle;
    request.scene_id = command->scene_id;
    request.request_id = command->request_id;
    request.x = command->x;
    request.y = command->y;
    pick->pending = false;

    if (request.x >= camera_width || request.y >= camera_height) {
        hub.enqueue_actor_pick_completion({camera_handle, request.scene_id, request.request_id,
                                           "miss", 0, request.x, request.y});
        return std::nullopt;
    }

    pick->result_ready = false;
    return request;
}

void OpticsSystem::complete_actor_pick(const ActorPickRequest& request,
                                       const std::vector<std::uintptr_t>& scene_actor_handles,
                                       const std::vector<std::uint32_t>& node_indices,
                                       bool read_ok,
                                       std::uint32_t instance_id) {
    const auto resolved =
        OpticsDetail::resolve_actor_pick_result(read_ok, instance_id, scene_actor_handles, node_indices);

    auto& hub = SharedDataHub::instance();
    if (auto pick = hub.actor_pick_storage().try_acquire_write(request.pick_handle)) {
        pick->actor_handle = resolved.actor_handle;
        pick->node_index = resolved.node_index;
        pick->result_x = request.x;
        pick->result_y = request.y;
        pick->result_request_id = request.request_id;
        pick->result_ready = true;
    }
    hub.enqueue_actor_pick_completion({request.camera_handle, request.scene_id, request.request_id,
                                       std::string(resolved.status), resolved.actor_handle,
                                       request.x, request.y, resolved.node_index});
}

void OpticsSystem::fail_actor_pick(const ActorPickRequest& request) {
    complete_actor_pick(request, {}, {}, false, 0);
}

void OpticsSystem::enqueue_actor_pick_readback(
    ActorPickRequest request,
    Horizon::HardwareBuffer result_buffer,
    Horizon::SubmitReceipt receipt,
    std::vector<std::uintptr_t> scene_actor_handles,
    std::vector<std::uint32_t> node_indices,
    OpticsDetail::ActorPickReadbackWorker::Reservation reservation) {
    if (!actor_pick_readback_worker_ || receipt.serial == 0) {
        fail_actor_pick(request);
        return;
    }

    const auto fallback_buffer = result_buffer;
    const bool enqueued = actor_pick_readback_worker_->submit(
        std::move(reservation),
        [this, request, result_buffer, receipt, scene_actor_handles, node_indices]() mutable {
            std::uint32_t instance_id = 0;
            bool read_ok = false;
            try {
                // executor.wait() only queues the receipt's tokens for the *next*
                // commit on the render thread; the host must instead wait for this
                // submission's own timeline value, otherwise the read below can
                // observe the previous frame's instance id (wrong actor picked) and
                // the worker mutates the render thread's executor from a foreign
                // thread for no reason.
                hardware_->executor.wait_completed(receipt);
                std::uint32_t value = 0;
                if (result_buffer.read_bytes(std::span<std::byte>(
                        reinterpret_cast<std::byte*>(&value), sizeof(value)))) {
                    instance_id = value;
                    read_ok = true;
                } else {
                    CFW_LOG_ERROR("OpticsSystem: Failed to read actor pick result from GPU");
                }
            } catch (const std::exception& error) {
                CFW_LOG_ERROR("OpticsSystem: Actor pick receipt wait/read failed: {}",
                              error.what());
            } catch (...) {
                CFW_LOG_ERROR("OpticsSystem: Actor pick receipt wait/read failed");
            }
            complete_actor_pick(request, scene_actor_handles, node_indices, read_ok, instance_id);
        });

    if (!enqueued) {
        // This is only expected during an abnormal lifecycle race. Preserve the
        // GPU buffer until its receipt completes before reporting the failure.
        try {
            hardware_->executor.wait_completed(receipt);
            (void)fallback_buffer;
        } catch (...) {
        }
        fail_actor_pick(request);
    }
}

void OpticsSystem::process_vision_actor_pick(std::uintptr_t camera_handle,
                                             const CameraDevice& camera,
                                             const SceneDevice& scene,
                                             uint64_t frame_index) {
    const auto actor_pick_request = take_pending_actor_pick(camera_handle, camera);
    if (!actor_pick_request) {
        return;
    }

    if (!actor_pick_readback_worker_) {
        fail_actor_pick(*actor_pick_request);
        return;
    }
    auto actor_pick_reservation = actor_pick_readback_worker_->reserve();
    if (!actor_pick_reservation) {
        CFW_LOG_WARNING("OpticsSystem: Vision actor pick readback queue is full");
        fail_actor_pick(*actor_pick_request);
        return;
    }

    std::optional<Horizon::HardwareBuffer> actor_pick_result_buffer;
    try {
        actor_pick_result_buffer.emplace(
            make_storage_buffer<std::uint32_t>(
                1, "optics.actor_pick.vision_request",
                Horizon::CpuAccessMode::ReadWrite));
    } catch (const std::exception& error) {
        CFW_LOG_ERROR("OpticsSystem: Failed to create Vision actor pick result buffer: {}",
                      error.what());
        fail_actor_pick(*actor_pick_request);
        return;
    }

    bind_native_view_resources(camera_handle, camera.width, camera.height, frame_index);
    auto& visibility = *hardware_->visibilityPipeline;

    hardware_->vpUniformBufferObjects.viewProjMatrix =
        multiply_ktm_mat4(OpticsDetail::native_projection(camera), camera.compute_view_matrix());
    (void)write_object_bytes(hardware_->vpUniformBuffer,
                             hardware_->vpUniformBufferObjects);
    const uint32_t scene_vp_descriptor = hardware_->vpUniformBuffer.store_descriptor();

    RenderInstanceBatch scene_batch;
    std::vector<Corona::Horizon::HardwareBuffer> actor_pick_indirect_buffers;
    collect_actor_instances_for_visibility(scene,
                                           visibility,
                                           scene_vp_descriptor,
                                           false,
                                           nullptr,
                                           scene_batch,
                                           geometry_system_,
                                           frame_index,
                                           actor_pick_indirect_buffers);
    if (!upload_instance_tables(scene_batch,
                                *hardware_,
                                hardware_->instanceInfoBuffer,
                                hardware_->instanceInfoCapacity,
                                hardware_->materialTableBuffer,
                                hardware_->materialTableCapacity,
                                "optics.actor_pick")) {
        visibility.clear_records();
        scene_batch.clear();
    }

    auto& actor_pick = *hardware_->actorPickPipeline;
    actor_pick.pushConsts.pixel =
        upload_value(ktm::uvec2{actor_pick_request->x, actor_pick_request->y});
    bind_storage_image(actor_pick, actor_pick.pushConsts.visibilityImageIndex, hardware_->visibilityImage);
    bind_storage_buffer(actor_pick, actor_pick.pushConsts.outputBufferIndex, *actor_pick_result_buffer);

    auto actor_pick_stream = hardware_->executor.stream();
    actor_pick_stream << RetainSubmission{scene_batch.resource_keep_alive};
    actor_pick_stream << visibility.extent(hardware_->gbufferSize.x, hardware_->gbufferSize.y);
    const Horizon::SubmitReceipt actor_pick_receipt =
        actor_pick_stream << actor_pick.dispatch_extent(1, 1) << Horizon::commit();
    hardware_->last_receipt = actor_pick_receipt;
    enqueue_actor_pick_readback(*actor_pick_request,
                                std::move(*actor_pick_result_buffer),
                                actor_pick_receipt,
                                scene_batch.actorHandles,
                                scene_batch.nodeIndices,
                                std::move(*actor_pick_reservation));
}

void OpticsSystem::begin_screenshot_frame() {
    std::lock_guard<std::mutex> lock(screenshot_mutex_);
    pending_screenshots_.insert(pending_screenshots_.end(),
                               std::make_move_iterator(incoming_screenshots_.begin()),
                               std::make_move_iterator(incoming_screenshots_.end()));
    incoming_screenshots_.clear();
}

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
        if (!SharedDataHub::instance().camera_storage().try_acquire_read(camera_handle)) {
            fail_pending_screenshots(camera_handle);
            continue;
        }

        bool in_enabled_scene = false;
        bool in_any_scene = false;
        for (const auto& scene : std::as_const(SharedDataHub::instance().scene_storage())) {
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
        if (read_ok) {
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
        if (backend_switch_sub_id_ != 0) {
            event_bus->unsubscribe(backend_switch_sub_id_);
        }

        if (residency_sub_id_ != 0) {
            event_bus->unsubscribe(residency_sub_id_);
        }
        if (native_frame_consumed_sub_id_ != 0) {
            event_bus->unsubscribe(native_frame_consumed_sub_id_);
        }
    }

    fail_pending_screenshots(0);

    if (actor_pick_readback_worker_) {
        actor_pick_readback_worker_->shutdown();
        actor_pick_readback_worker_.reset();
    }

    if (hardware_) {
        // Note: executor.wait() enqueues the receipt's tokens for the *next*
        // commit, so this drain does not block on the GPU. That is intentional
        // here: Horizon resources are reference counted and the device retires
        // in-flight work on its own, while a blocking wait_idle() during shutdown
        // would hang the engine whenever the queue is wedged (device lost).
        hardware_->native_frame_throttle.drain([this](const Horizon::SubmitReceipt& receipt) {
            hardware_->executor.wait(receipt);
        });
    }

    // Retire per-surface publications before releasing their storage and GPU images.
    for (auto& [surface, target] : surface_targets_) {
        release_surface_target(target);
    }
    surface_targets_.clear();
    offscreen_screenshot_targets_.clear();
    native_view_resources_.clear();
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
    auto& actor_storage = hub.actor_storage();
    auto& profile_storage = hub.profile_storage();
    auto& optics_storage = hub.optics_storage();
    auto& geom_storage = hub.geometry_storage();
    auto& transform_storage = hub.model_transform_storage();

    const auto mix_scene = [&](const SceneDevice& scene_dev) {
        if (!scene_dev.enabled) return;
        for (auto actor_handle : scene_dev.actor_handles) {
            auto actor = actor_storage.try_acquire_read(actor_handle);
            if (!actor) continue;
            mix(static_cast<std::size_t>(actor_handle));
            for (auto profile_handle : actor->profile_handles) {
                auto profile = profile_storage.try_acquire_read(profile_handle);
                if (!profile || profile->optics_handle == 0) continue;

                auto optics = optics_storage.try_acquire_read(profile->optics_handle);
                if (!optics) continue;

                // visible toggle changes topology of the Vision scene.
                mix(optics->visible ? 0x1u : 0x2u);
                if (!optics->visible) continue;

                // Material parameters bridged into the Vision principled BSDF.
                mix(optics->principled ? optics->principled->revision : 0u);
                mix(reinterpret_cast<std::size_t>(optics->material_overrides.get()));
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
                if (auto model = hub.model_resource_storage().try_acquire_read(geom->model_resource_handle)) {
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

                // Hash the actual affine transform, including authored shear and
                // reflection. TRS alone misses matrix-only external-scene edits.
                if (auto transform = transform_storage.try_acquire_read(geom->transform_handle)) {
                    const auto matrix = transform->compute_matrix();
                    for (int column = 0; column < 4; ++column)
                        for (int row = 0; row < 4; ++row) mix_float(matrix[column][row]);
                }
            }
        }
    };
    if (locked_scene) {
        mix_scene(*locked_scene);
    } else {
        for (const auto& scene : std::as_const(hub.scene_storage())) {
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
        runtime.wait_for_geometry_users();
        pipeline->activate_view_context(0u);
        auto& scene = pipeline->scene();
        result = Vision::build_vision_geometry(scene);
        if (!result.complete()) return result;
        runtime.diagnostics.clear();
        if (runtime.scene_resource) {
            sync_logical_instances_from_pipeline_scene(*runtime.scene_resource, scene);
        }

        // Rebind the engine's infinite lighting after a complete topology
        // replacement; light buffers/sampler state must be prepared together.
        const auto env = engine_vision_environment();
        Vision::setup_vision_lights(scene, env);
        runtime.engine_lighting = Vision::make_vision_lighting_state(env);

        // Keep framebuffer/sensor allocations intact during topology changes.
        // Forcing geometry_changed refreshes every view's light tables, uploads
        // both bindless arrays, then recompiles all consumers of the final data.
        scene.prepare();
        if (runtime.scene_resource) {
            runtime.scene_resource->mark_transforms_changed();
        }
        pipeline->prepare_geometry(true);
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
    if (!vision_initialized_) return;
    const std::size_t sig = compute_vision_scene_signature();
    const bool revision_changed = sig != vision_pending_signature_;
    if (revision_changed) {
        vision_pending_signature_ = sig;
        vision_stable_frames_ = 0;
        vision_rebuild_retries_ = 0;
        runtime.engine_geometry_ready = false;
        runtime.next_geometry_retry = {};
    }
    if (runtime.engine_geometry_ready && sig == vision_applied_signature_) {
        runtime.geometry_debounce.reset();
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < runtime.next_geometry_retry || !runtime.geometry_debounce.poll(revision_changed, now)) return;
    runtime.geometry_debounce.reset();
    vision_stable_frames_ = 0;
    const auto result = rebuild_vision_scene(runtime);
    if (result.complete()) {
        vision_applied_signature_ = sig;
        vision_rebuild_retries_ = 0;
        runtime.engine_geometry_available = true;
        // Edits during CPU staging/GPU compilation still need a later snapshot.
        // Interactive views may display this complete snapshot; captures wait.
        runtime.engine_geometry_ready = compute_vision_scene_signature() == sig;
        CFW_LOG_INFO("Vision bridge synchronized: candidates={}, instances={}",
                     result.candidate_count, result.instance_count);
    } else {
        runtime.engine_geometry_ready = false;
        ++vision_rebuild_retries_;
        // Preserve the last complete scene; never accept a partial build as the
        // new baseline. Back off instead of recompiling indefinitely at frame rate.
        runtime.next_geometry_retry = now + std::chrono::seconds(
            vision_rebuild_retries_ < kVisionRebuildMaxRetries ? 1 : 5);
        if (vision_rebuild_retries_ == 1 || vision_rebuild_retries_ == kVisionRebuildMaxRetries) {
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
        Vision::setup_vision_lights(scene, env);
        scene.prepare();
        // Force the existing all-view resource refresh even when bounds did not
        // change: new light tables must be uploaded before any view recompiles.
        pipeline.refresh_world_bounds_dependents(true);
        runtime.engine_lighting = lighting;
        return true;
    } catch (const std::exception& e) {
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
        auto scene_resource = get_or_create_vision_scene_resource();
        auto pipeline = create_vision_pipeline(current_vision_render_mode_, scene_resource);
        if (!pipeline) {
            CFW_LOG_ERROR("OpticsSystem: Failed to create Vision pipeline without external scene import");
            return false;
        }
        bind_pipeline_scene_gpu_resource(*pipeline,
                                         *scene_resource,
                                         current_vision_render_mode_);

        // Populate Vision scene directly from CoronaEngine scene data.
        auto& scene = pipeline->scene();
        const auto build_signature = compute_vision_scene_signature();
        const auto build_result = Vision::build_vision_geometry(scene);
        sync_logical_instances_from_pipeline_scene(*scene_resource, scene);

        // The adapter keeps a valid background entry even when it is black.
        // No environment in Corona must never imply bright default lighting.
        const auto env = engine_vision_environment();
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
        // Vulkan device deterministically.
        if (const auto camera = active_scene_camera_values()) {
            Vision::sync_vision_camera(*pipeline, *camera);
        }

        activate_single_vision_runtime_key(key);
        auto& runtime = active_vision_runtime();
        runtime.reset_pipeline(std::move(pipeline),
                               current_vision_render_mode_);
        vision_initialized_ = true;

        // Establish the dynamic-scene signature baseline so subsequent edits are
        // detected as changes against the initially-built scene.
        const auto current_signature = compute_vision_scene_signature();
        runtime.engine_geometry_available = build_result.complete();
        runtime.engine_geometry_ready = build_result.complete() && current_signature == build_signature;
        runtime.engine_lighting = Vision::make_vision_lighting_state(env);
        vision_applied_signature_ = build_result.complete() ? build_signature : 0;
        vision_pending_signature_ = current_signature;
        vision_stable_frames_ = 0;

        return true;
    } catch (const std::exception& e) {
        CFW_LOG_ERROR("OpticsSystem: Vision init failed: {}", e.what());
        return false;
    }
}

void OpticsSystem::run_vision_frame(float frame_count, uint64_t frame_index,
                                   const std::vector<RenderView>& views) {
    if (views.empty() || !hardware_->visionResolvePipeline) return;
    Corona::Diagnostics::PhaseScope vision_phase{"Optics", "vision:scene_sync"};
    (void)frame_count;

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
                runtime.diagnostics.erase(camera_handle);
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
                if (SharedDataHub::instance().camera_storage().try_acquire_read(*it)) {
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
            VisionCameraFeedback& feedback) {
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
            auto& pipeline = runtime.pipeline;
            if (!pipeline) {
                return;
            }

            if ((!runtime.engine_geometry_available ||
                 (has_pending_screenshot(cam_handle) && !runtime.engine_geometry_ready))) return;
            active_contexts.insert(cam_handle);
            runtime.retained_contexts.erase(cam_handle);
            vision_phase.set("vision:pick");
            process_vision_actor_pick(cam_handle, camera, *locked.scene, frame_index);
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
                                               camera.vision_restir_settings, &feedback)) {
                    return;
                }

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
                pipeline->upload_data();
                vision_phase.set("vision:display");
                const bool diagnostic = Vision::VisionDiagnosticAdapter::supports(camera.output_mode);
                const auto render_start = profile_now();
                if (diagnostic) {
                    auto& adapter = runtime.diagnostics[cam_handle];
                    if (!adapter) adapter = std::make_unique<Vision::VisionDiagnosticAdapter>();
                    if (!adapter->render(*pipeline, camera.output_mode)) return;
                } else {
                    pipeline->display(1.0 / 60.0);
                }
                const auto render_end = profile_now();
                // display() already waits for GPU completion. Reuse its timing without
                // adding a synchronization or including cross-API presentation work.
                SharedDataHub::instance().publish_camera_frame_timing(
                    cam_handle, pipeline->cur_render_time(),
                    pipeline->renderer().integrator()->cur_denoise_time(), runtime.mode);
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

                // The preceding resolve must finish before Vision reuses or
                // reallocates its shared buffer. Frame pixels never visit the CPU.
                vision_phase.set("vision:interop");
                const auto reuse_start = profile_now();
                runtime.wait_for_interop_submission(cam_handle, "shared buffer reuse", false);
                const auto reuse_end = profile_now();
                if (!bridge->ensure(*pipeline, w, h) ||
                    !bridge->copy_from_framebuffer(*pipeline)) {
                    // The bridge logs and latches the failure. Do not resolve an
                    // invalid descriptor or publish stale pixels as a new frame.
                    return;
                }
                const auto copy_end = profile_now();
                const uint32_t vision_src_descriptor = bridge->imported().store_descriptor();

                void* surface = camera.surface;
                hardware_->gbufferSize = ktm::uvec2{w, h};
                auto& target = surface
                    ? acquire_surface_target(surface, w, h, frame_index)
                    : acquire_offscreen_screenshot_target(cam_handle, w, h, frame_index);
                if (auto consumed_device =
                        SharedDataHub::instance().image_storage().try_acquire_read(
                            target.image_handle)) {
                    if (consumed_device->consumed_receipt.serial != 0) {
                        hardware_->executor.wait(consumed_device->consumed_receipt);
                    }
                }

                const auto target_end = profile_now();
                auto stream = hardware_->executor.stream();
                {
                    auto& visionResolve = *hardware_->visionResolvePipeline;
                    visionResolve.pushConsts.gbufferSize = upload_value(hardware_->gbufferSize);
                    visionResolve.pushConsts.srcBufferIndex = vision_src_descriptor;
                    visionResolve.pushConsts.outputImage =
                        target.final_output.store_descriptor();
                    visionResolve.pushConsts.exposure = environment.exposure;
                    visionResolve.pushConsts.diagnostic = diagnostic ? 1u : 0u;

                    // dispatch_extent replaces dispatch_groups
                    // ���ڴ� commit��UI overlay pass �������� final_output ��Ϊ������
                    // ��֡��ͬһ executor �ϰ��������¼��ĩβͳһ�ύһ�Ρ�
                    stream << visionResolve.dispatch_extent(hardware_->gbufferSize.x, hardware_->gbufferSize.y);
                }

                // �� Native ���õ� UI overlay �㣺gbufferSize �����Ϸ���Ϊ {w,h}��

                const auto ui_state =
                    SharedDataHub::instance().viewport_ui_state(cam_handle);
                Horizon::HardwareImage* presented = &target.final_output;
                if (surface) {
                    vision_phase.set("vision:overlay");
                    // Overlay geometry is read directly under a short scene
                    // lock. Its GPU submission keeps the resource handles alive.
                    auto scene = SharedDataHub::instance().scene_storage().try_acquire_read(view.scene_handle);
                    if (!scene || !scene->enabled) return;
                    presented = compose_surface_ui_overlay(
                        stream, cam_handle, camera, *scene, target, target.final_output,
                        ui_state.mode, ui_state.calibration, frame_index);
                }

                vision_phase.set("vision:publish");
                const auto submit_start = profile_now();
                const Horizon::SubmitReceipt vision_submit_receipt =
                    stream << Horizon::commit();
                const auto submit_end = profile_now();
                hardware_->last_receipt = vision_submit_receipt;
                runtime.interop_submissions.insert_or_assign(cam_handle, vision_submit_receipt);

                process_pending_screenshots(cam_handle, *presented);
                if (!surface) return;

                const ImagePixelExtent presented_extent{target.width, target.height};
                const auto viewport = optics_event_viewport(camera, presented_extent);
                if (auto image_device =
                        SharedDataHub::instance().image_storage().acquire_write(
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
                    event_bus->publish<Events::OpticsFrameReadyEvent>(
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
                    CFW_LOG_INFO("VisionFrameProfile frame={} camera={} extent={}x{} prepare_ms={} render_ms={} bookkeeping_ms={} reuse_wait_ms={} copy_sync_ms={} target_ms={} overlay_record_ms={} submit_ms={} publish_ms={} total_ms={}",
                        frame_index, cam_handle, w, h,
                        profile_ms(profile_start, render_start), profile_ms(render_start, render_end),
                        profile_ms(render_end, reuse_start), profile_ms(reuse_start, reuse_end),
                        profile_ms(reuse_end, copy_end), profile_ms(copy_end, target_end),
                        profile_ms(target_end, submit_start), profile_ms(submit_start, submit_end),
                        profile_ms(submit_end, end), profile_ms(profile_start, end));
                }
            } catch (const std::exception& error) {
                CFW_LOG_ERROR("OpticsSystem: Vision camera {} failed: {}",
                              cam_handle, error.what());
            }
        };

    auto render_vision_camera = [&](VisionPipelineRuntime& runtime, const RenderView& view,
                                     std::unordered_set<std::uintptr_t>& active_contexts) {
        const bool profile_frame = env_flag_enabled("CORONA_VISION_FRAME_PROFILE");
        const auto start = profile_frame ? std::chrono::steady_clock::now()
                                         : std::chrono::steady_clock::time_point{};
        auto acquired = start;
        VisionCameraFeedback feedback;
        {
            auto locked = lock_render_view(view, CameraRenderBackend::Vision);
            if (profile_frame) acquired = std::chrono::steady_clock::now();
            if (!locked || locked->camera.vision_render_mode != view.vision_mode) return;
            render_locked_vision_camera(runtime, view, *locked, active_contexts, feedback);
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
    };

    const auto mode_selection = select_vision_render_mode(views);
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

    auto& runtime = active_vision_runtime();
    auto& pipeline = runtime.pipeline;
    if (!pipeline) return;

    if (mode_selection.has_visible_camera) {
        sync_vision_dynamic_scene(runtime);
    }

    runtime.last_used_frame = frame_index;
    std::unordered_set<std::uintptr_t> active_contexts;
    for (const auto& view : views) {
        render_vision_camera(runtime, view, active_contexts);
    }

    cleanup_runtime_contexts(runtime, active_contexts);
    evict_idle_vision_runtimes(frame_index);
}


bool OpticsSystem::load_engine_built_vision_scene(CameraVisionRenderMode requested_mode) {
    auto previous_pipeline = vision::Global::instance().pipeline_shared();
    try {
        const auto key = make_vision_pipeline_key(requested_mode);
        auto scene_resource = get_or_create_vision_scene_resource();
        auto pipeline = create_vision_pipeline(requested_mode, scene_resource);
        if (!pipeline) {
            CFW_LOG_ERROR("OpticsSystem: failed to recreate engine-built Vision pipeline");
            if (previous_pipeline) previous_pipeline->activate_global_context();
            return false;
        }
        bind_pipeline_scene_gpu_resource(*pipeline,
                                         *scene_resource,
                                         requested_mode);
        auto& scene = pipeline->scene();
        const auto build_signature = compute_vision_scene_signature();
        const auto build_result = Vision::build_vision_geometry(scene);
        sync_logical_instances_from_pipeline_scene(*scene_resource, scene);

        const auto env = engine_vision_environment();
        Vision::setup_vision_lights(scene, env);
        pipeline->prepare();
        pipeline->frame_buffer()->prepare_view_texture();
        pipeline->set_output_denoise(
            Vision::vision_render_mode_uses_denoise(requested_mode));

        if (const auto camera = active_scene_camera_values()) {
            Vision::sync_vision_camera(*pipeline, *camera);
        }

        activate_single_vision_runtime_key(key);
        auto& runtime = active_vision_runtime();
        runtime.reset_pipeline(std::move(pipeline),
                               requested_mode);
        runtime.pipeline->activate_global_context();
        current_vision_render_mode_ = requested_mode;
        const auto current_signature = compute_vision_scene_signature();
        runtime.engine_geometry_available = build_result.complete();
        runtime.engine_geometry_ready = build_result.complete() && current_signature == build_signature;
        runtime.engine_lighting = Vision::make_vision_lighting_state(env);
        vision_applied_signature_ = build_result.complete() ? build_signature : 0;
        vision_pending_signature_ = current_signature;
        vision_stable_frames_ = 0;
        vision_rebuild_retries_ = 0;
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
    } catch (const std::exception& e) {
        if (previous_pipeline) previous_pipeline->activate_global_context();
        CFW_LOG_ERROR("OpticsSystem: restoring engine-built Vision scene failed: {}", e.what());
        return false;
    }
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
