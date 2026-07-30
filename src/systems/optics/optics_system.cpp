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

#include "shadow_culling.h"

#include <array>
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

#ifdef _WIN32
#include <windows.h>
#endif

#include <oneapi/tbb/task_group.h>

#include "hardware.h"
#include "native_diagnostics.h"
#include "optics_debug_labels.h"
#include "storage_snapshot.h"
#include "../common/diagnostic_env.h"

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
#include "vision/vision_interop_lifetime.h"
#include "vision/vision_zero_copy_bridge.h"
#endif

namespace {

constexpr uint32_t kShadowCascadeCount = 4;
constexpr uint32_t kShadowMapSize = 1024;
constexpr float kShadowMaxDistance = 100.0f;
constexpr float kShadowSplitLambda = 0.95f;
constexpr float kShadowBias = 0.0015f;
constexpr uint32_t kSsaoSampleCount = 16;
constexpr float kSsaoRadius = 0.6f;
constexpr float kSsaoBias = 0.025f;
constexpr float kSsaoStrength = 1.0f;
constexpr float kSsaoPower = 1.5f;
constexpr uint32_t kSsaoAtrousPasses = 2;
constexpr uint32_t kShadowAtrousPasses = 1;
constexpr float kSsaoAtrousValueSigma = 0.25f;
constexpr float kShadowAtrousValueSigma = 0.35f;
constexpr float kAtrousNormalPower = 32.0f;
constexpr float kAtrousNormalThreshold = 0.85f;
constexpr float kAtrousDepthSigmaScale = 0.02f;
constexpr float kAtrousDepthSigmaMin = 0.05f;
constexpr std::uint64_t kInitialInstanceTableCapacity = 4096;
constexpr std::uint64_t kInitialMaterialTableCapacity = 1024;

std::mutex g_invalid_optics_mesh_log_mutex;
std::unordered_set<std::string> g_invalid_optics_mesh_logs;
std::mutex g_optics_material_log_mutex;
std::unordered_set<std::string> g_optics_material_logs;

[[nodiscard]] std::filesystem::path current_executable_directory() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(MAX_PATH);
    while (true) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            break;
        }
        if (length < buffer.size() - 1) {
            return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
    return std::filesystem::current_path();
}

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
        c.skip_scene_visibility = env_flag_enabled("CORONA_OPTICS_DIAG_SKIP_SCENE_VISIBILITY");
        c.skip_shadows = env_flag_enabled("CORONA_OPTICS_DIAG_SKIP_SHADOWS");
        c.skip_deferred_compute = env_flag_enabled("CORONA_OPTICS_DIAG_SKIP_DEFERRED_COMPUTE");
        c.disable_textures = env_flag_enabled("CORONA_OPTICS_DIAG_DISABLE_TEXTURES");
        c.disable_albedo_sample = env_flag_enabled("CORONA_OPTICS_DIAG_DISABLE_ALBEDO_SAMPLE");
        c.force_solid_material = env_flag_enabled("CORONA_OPTICS_DIAG_FORCE_SOLID_MATERIAL");
        c.profile = env_flag_enabled("CORONA_OPTICS_DIAG_PROFILE");
        c.debug_labels = env_flag_enabled("CORONA_OPTICS_DIAG_DEBUG_LABELS");
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

    Corona::Horizon::ImageExtent extent{};
    if (texture_valid) {
        extent = texture.extent();
    }

    CFW_LOG_INFO("OpticsSystem: material table entry "
                 "(actor={}, geometry={}, mesh={}, material={}, texture_desc={}, texture_image={}, "
                 "texture_ready={}, texture_valid={}, extent={}x{}x{}, albedo_sample_disabled={})",
                 actor_handle,
                 geometry_handle,
                 mesh_index,
                 material_id,
                 texture_descriptor,
                 texture_valid ? texture.get_image_id() : 0u,
                 texture_ready,
                 texture_valid,
                 extent.width,
                 extent.height,
                 extent.depth,
                 disable_albedo_sample);
}

// Perf toggle: sky-driven SH9 ambient. Set to false to skip both the sky→SH
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
    };

    std::vector<Hardware::InstanceInfo> instances;
    std::vector<Hardware::MaterialInfo> materials;
    std::vector<std::uintptr_t> actorHandles;
    std::shared_ptr<ResourceKeepAlive> resource_keep_alive = std::make_shared<ResourceKeepAlive>();

    void clear() {
        instances.clear();
        materials.clear();
        actorHandles.clear();
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

using ShadowSceneBounds = Corona::Systems::OpticsDetail::ShadowSceneBounds;

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
    // desc.depth_attachment =
    //     Corona::Horizon::DepthAttachmentDesc::with_format(Corona::Horizon::Format::D32);
    return desc;
}

[[nodiscard]] Corona::Horizon::RasterizerPipelineDesc make_shadow_pipeline_desc() {
    Corona::Horizon::RasterizerPipelineDesc desc;
    auto vertex_shader = Corona::Horizon::PipelineShaderDesc::from_slang_module(
        Corona::Horizon::PipelineShaderStage::Vertex,
        shadow_vert_glsl_t::slangModule);
    auto fragment_shader = Corona::Horizon::PipelineShaderDesc::from_slang_module(
        Corona::Horizon::PipelineShaderStage::Fragment,
        shadow_frag_glsl_t::slangModule);
    desc.set_shaders(std::move(vertex_shader), std::move(fragment_shader));
    desc.depth_stencil.depth_test_enabled = true;
    desc.depth_stencil.depth_write_enabled = true;
    desc.depth_stencil.depth_compare_op = Corona::Horizon::CompareOp::LessOrEqual;
    desc.rasterizer.cull_mode = Corona::Horizon::CullMode::None;
    // desc.depth_attachment =
    //     Corona::Horizon::DepthAttachmentDesc::with_format(Corona::Horizon::Format::D32);
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

void mix_hash(std::size_t& sig, std::size_t value) {
    sig ^= value + 0x9e3779b97f4a7c15ULL + (sig << 6) + (sig >> 2);
}

void mix_hash_float(std::size_t& sig, float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float must be 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    mix_hash(sig, static_cast<std::size_t>(bits));
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

[[nodiscard]] std::size_t external_live_hidden_transform_signature(int shape_index) {
    std::size_t sig = 0;
    mix_hash(sig, static_cast<std::size_t>(shape_index));
    mix_hash(sig, 0x76497369626c6548ULL);
    return sig;
}

[[nodiscard]] ::vision::float4x4 hidden_external_live_o2w() {
    auto o2w = ::vision::make_float4x4(1.f);
    o2w[0][0] = 0.f;
    o2w[1][1] = 0.f;
    o2w[2][2] = 0.f;
    return o2w;
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

[[nodiscard]] std::string external_live_json_path_for_shape(int shape_index) {
    return shape_index < 0 ? std::string{} : std::string{"/scene/shapes/"} + std::to_string(shape_index);
}

void write_external_live_binding_shape_index(
    std::uintptr_t actor_handle,
    int shape_index) {
    if (actor_handle == 0 || shape_index < 0) {
        return;
    }
    auto& hub = Corona::SharedDataHub::instance();
    auto binding = hub.external_vision_binding(actor_handle);
    if (!binding) {
        return;
    }
    binding->shape_index = shape_index;
    binding->json_path = external_live_json_path_for_shape(shape_index);
    hub.set_external_vision_binding(actor_handle, std::move(*binding));
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

// Engine-native variant: resolves a NON-bound engine actor's renderable transform
// for mixed rendering into an ExternalLive scene. Unlike resolve_external_live_transform
// it is driven by the engine geometry's own OpticsDevice/ModelTransform (no binding).
// Returns std::nullopt when the actor is not currently renderable (no visible optics
// with a geometry + transform) — the caller treats that as "remove from mix".
// `shape_index` is the actor's current Vision group index, folded into the signature
// so a remap-induced index change forces a transform re-apply.
[[nodiscard]] std::optional<ExternalLiveResolvedTransform> resolve_engine_native_transform(
    std::uintptr_t actor_handle,
    int shape_index) {
    if (actor_handle == 0) {
        return std::nullopt;
    }

    auto& hub = Corona::SharedDataHub::instance();
    auto actor = hub.actor_storage().try_acquire_read(actor_handle);
    if (!actor) {
        return std::nullopt;
    }

    for (auto profile_handle : actor->profile_handles) {
        auto profile = hub.profile_storage().try_acquire_read(profile_handle);
        if (!profile || profile->optics_handle == 0) {
            continue;
        }

        auto optics = hub.optics_storage().try_acquire_read(profile->optics_handle);
        if (!optics || !optics->visible || optics->geometry_handle == 0) {
            continue;
        }

        auto geometry = hub.geometry_storage().try_acquire_read(optics->geometry_handle);
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
                    update.fields, Corona::CameraStateUpdateField::ShadowCascadeDebug)) {
                camera->shadow_cascade_debug = update.shadow_cascade_debug;
            }
            if (Corona::has_camera_state_field(
                    update.fields, Corona::CameraStateUpdateField::SsaoEnabled)) {
                camera->ssao_enabled = update.ssao_enabled;
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
        hub.clear_ssat_view_viewer_state(release.camera_handle);
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

[[nodiscard]] ktm::fmat4x4 make_inverse_projection_matrix(
    const Corona::CameraDevice& camera) {
    const ktm::fmat4x4 proj = camera.compute_projection_matrix();
    const float sx = proj[0][0];
    const float sy = proj[1][1];
    const float depth_scale = proj[2][2];
    const float depth_bias = proj[3][2];
    if (std::abs(sx) < 1e-6f || std::abs(sy) < 1e-6f ||
        std::abs(depth_bias) < 1e-6f) {
        return ktm::fmat4x4::from_eye();
    }

    ktm::fmat4x4 inv{};
    inv[0][0] = 1.0f / sx;
    inv[1][1] = 1.0f / sy;
    inv[3][2] = 1.0f;
    inv[2][3] = 1.0f / depth_bias;
    inv[3][3] = -depth_scale / depth_bias;
    return inv;
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
    return Corona::Horizon::ImageUsageFlags::Storage |
           Corona::Horizon::ImageUsageFlags::ColorAttachment |
           Corona::Horizon::ImageUsageFlags::Sampled |
           Corona::Horizon::ImageUsageFlags::TransferSrc |
           Corona::Horizon::ImageUsageFlags::TransferDst;
}

[[nodiscard]] Corona::Horizon::ImageUsageFlags optics_compute_image_usage() {
    return Corona::Horizon::ImageUsageFlags::Storage |
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

[[nodiscard]] Corona::Horizon::HardwareImage make_compute_image(
    uint32_t width,
    uint32_t height,
    Corona::Horizon::Format format,
    std::string_view name) {
    return Corona::Horizon::HardwareImage(Corona::Horizon::HardwareImageDesc::texture_2d(
        width,
        height,
        format,
        optics_compute_image_usage(),
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
    std::uint64_t frame_index) {
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

        // 续期资源访问时间（驱动 ResourceManager LRU）
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
            // ---- 获取几何变换信息（读锁即可，texture 由 query_mesh_slots 内部处理）----
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
                // 用提取的 nlc 参数重现 apply_native_local_correction 逻辑
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

            // ---- query_mesh_slots：统一获取所有 mesh 的 LOD 缓冲 + 材质信息 ----
            // 内部取读锁，返回值为值副本（refcount 安全），无需再持有 geometry 写锁。
            // valid=false 仅在 Actor 首次加载未完成时出现，是唯一合法跳过原因。
            const auto mesh_slots = geometry_system
                ? geometry_system->query_mesh_slots(optics.geometry_handle)
                : std::vector<Corona::Systems::GeometrySystem::MeshSlot>{};

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
                        : ms.texture.storeSampledDescriptor();
                const uint32_t vertex_descriptor = vertex_storage_valid
                    ? ms.geo.vertex_storage.storeDescriptor()
                    : 0u;
                const uint32_t index_descriptor = index_storage_valid
                    ? ms.geo.index_storage.storeDescriptor()
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

                auto material_id = static_cast<uint32_t>(batch.materials.size());
                {
                    Hardware::MaterialInfo mat_info{};

                    const bool lighting_active =
                        optics.bEnableLighting && !diag.force_solid_material;
                    const float lighting_enabled = lighting_active ? 1.0f : 0.0f;
                    mat_info.textureDescriptor = texture_descriptor;

                    if (lighting_active) {
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
                        ms.material_color[0], ms.material_color[1],
                        ms.material_color[2], ms.material_color[3]};
                    batch.materials.push_back(mat_info);
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
                    inst.modelMatrix = model_matrix;
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
                    has_instances = true;
                }

                target_visibility[visibility_vert_glsl_t::pushConsts::modelMatrix] =
                    upload_value(model_matrix);
                target_visibility[visibility_vert_glsl_t::pushConsts::uniformBufferIndex] =
                    target_vp_descriptor;
                target_visibility[visibility_vert_glsl_t::pushConsts::instanceID] =
                    instance_id + 1;
                target_visibility[visibility_frag_glsl_t::pushConsts::textureIndex] =
                    texture_descriptor;
                Corona::Horizon::DrawIndexedParams draw_params;
                draw_params.debug_label = make_optics_draw_label(
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
                    ms.max_index);
                target_visibility.record(ms.geo.index, ms.geo.vertex, draw_params);
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
        hardware.executor.wait_idle(hardware.executor.last_receipt());
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

void apply_ssat_view_viewer_state(vision::Pipeline& pipeline,
                                  std::uintptr_t camera_handle,
                                  const Corona::CameraDevice& camera) {
    auto& hub = Corona::SharedDataHub::instance();
    const auto requested = hub.ssat_view_viewer_state(camera_handle);

    auto* fb = pipeline.frame_buffer();
    auto* lightfield_fb =
        fb == nullptr ? nullptr : dynamic_cast<vision::ILightFieldFrameBuffer*>(fb);

    vision::Denoiser* denoiser = nullptr;
    std::string denoiser_type;
    bool denoiser_enabled = false;
    bool denoiser_supports_lightfield = false;
    if (auto* integrator = pipeline.renderer().integrator().get()) {
        if (auto* illum = dynamic_cast<vision::IlluminationIntegrator*>(integrator)) {
            denoiser = illum->denoiser();
            if (denoiser != nullptr) {
                denoiser_type = std::string(denoiser->impl_type());
                denoiser_enabled = denoiser->enabled();
                denoiser_supports_lightfield = denoiser->supports_lightfield();
            }
        }
    }

    const bool ssat_active = camera.vision_render_mode == Corona::CameraVisionRenderMode::SSAT &&
                             lightfield_fb != nullptr &&
                             pipeline.output_desc().denoise &&
                             denoiser != nullptr &&
                             denoiser_type == "SSAT" &&
                             denoiser_enabled &&
                             denoiser_supports_lightfield;

    if (ssat_active) {
        const auto view_count =
            vision::lightfield_view_count(lightfield_fb->lenticular_params().num_views);
        const auto effective_index = vision::lightfield_effective_view_index(
            requested.requested_view_index, view_count);
        const auto viewer_mode = requested.mode == Corona::SsatViewViewerMode::FinalView
                                     ? vision::LightFieldViewerMode::FinalView
                                     : vision::LightFieldViewerMode::Interlaced;
        lightfield_fb->set_viewer_state(viewer_mode, effective_index);
        hub.update_ssat_view_viewer_status(
            camera_handle, true, false, view_count, effective_index);
        return;
    }

    if (lightfield_fb != nullptr) {
        lightfield_fb->set_viewer_state(vision::LightFieldViewerMode::Interlaced, 0u);
    }

    if (camera.vision_render_mode != Corona::CameraVisionRenderMode::SSAT) {
        hub.clear_ssat_view_viewer_state(camera_handle);
        return;
    }

    const bool pending = camera.vision_render_mode == Corona::CameraVisionRenderMode::SSAT &&
                         fb != nullptr &&
                         (denoiser == nullptr ||
                          (denoiser_type == "SSAT" && !denoiser_enabled));
    hub.update_ssat_view_viewer_status(camera_handle, false, pending, 0u, 0u);
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

[[nodiscard]] std::string vision_framebuffer_type_from_project_data(
    const vision::DataWrap& data) {
    if (!data.contains("pipeline") || !data["pipeline"].is_object()) {
        return "<missing>";
    }
    const auto& pipeline = data["pipeline"];
    if (!pipeline.contains("param") || !pipeline["param"].is_object()) {
        return "<missing>";
    }
    const auto& pipeline_param = pipeline["param"];
    if (!pipeline_param.contains("frame_buffer") ||
        !pipeline_param["frame_buffer"].is_object()) {
        return "<missing>";
    }
    const auto& frame_buffer = pipeline_param["frame_buffer"];
    if (!frame_buffer.contains("type") || !frame_buffer["type"].is_string()) {
        return "<missing>";
    }
    return frame_buffer["type"].get<std::string>();
}

// Loads a Vision scene description and brings it to a renderable state,
// mirroring the reference snippet (ProjectDesc -> init -> prepare).
// Resolves relative texture/mesh references against base_dir.
[[nodiscard]] auto import_vision_scene_from_data(vision::DataWrap project_data,
                                                const std::filesystem::path& base_dir,
                                                const std::string& scene_label,
                                                Corona::CameraVisionRenderMode mode,
                                                const std::shared_ptr<
                                                    Corona::Systems::Vision::VisionSceneResource>&
                                                    scene_resource,
                                                Corona::Systems::Vision::VisionPipelineSource source)
    -> ocarina::SP<vision::Pipeline> {
    const auto source_framebuffer_type =
        vision_framebuffer_type_from_project_data(project_data);
    Corona::Systems::Vision::configure_vision_scene_for_mode(project_data, mode);
    const auto configured_framebuffer_type =
        vision_framebuffer_type_from_project_data(project_data);
    CFW_LOG_INFO(
        "OpticsSystem: Vision framebuffer config (scene={}, mode={}, source={}, configured={})",
        scene_label,
        std::string(Corona::Systems::Vision::vision_render_mode_name(mode)),
        source_framebuffer_type,
        configured_framebuffer_type);

    vision::Global::instance().set_scene_path(base_dir);

    vision::ProjectDesc project_desc;
    project_desc.scene_path = base_dir;
    project_desc.init(project_data);

    auto pipeline = vision::Node::create_shared<vision::Pipeline>(project_desc.pipeline_desc);
    if (!pipeline) {
        CFW_LOG_ERROR("OpticsSystem: Vision pipeline creation returned null for {}",
                      scene_label);
        return {};
    }
    bind_pipeline_scene_resource_early(*pipeline, scene_resource);
    pipeline->init_project(project_desc);
    if (scene_resource) {
        bind_pipeline_scene_gpu_resource(
            *pipeline, *scene_resource, source, mode, scene_label);
    }
    pipeline->init_postprocessor(project_desc.renderer_desc.denoiser_desc);
    pipeline->init();
    pipeline->set_output_denoise(project_desc.output_desc.denoise);
    pipeline->prepare();
    // prepare() does not create FrameBuffer::view_texture_; the render path tone
    // maps into it and we later read it back, so create it explicitly here.
    pipeline->frame_buffer()->prepare_view_texture();
    CFW_LOG_INFO(
        "OpticsSystem: Vision framebuffer realized (scene={}, configured={}, lightfield={})",
        scene_label,
        configured_framebuffer_type,
        dynamic_cast<const vision::ILightFieldFrameBuffer*>(pipeline->frame_buffer()) != nullptr);
    return pipeline;
}

// Loads a Vision scene from disk and brings it to a renderable state.
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

    return import_vision_scene_from_data(vision::create_json_from_file(scene_path),
                                         scene_path.parent_path(),
                                         scene_path.string(),
                                         mode,
                                         scene_resource,
                                         source);
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

namespace {
constexpr auto kScreenshotRequestTimeout = std::chrono::seconds(10);
}

struct OpticsSystem::NativeViewResources {
    Horizon::HardwareImage visibility;
    Horizon::HardwareImage depth;
    Horizon::HardwareImage surface_guide;
    Horizon::HardwareImage ssao_raw;
    Horizon::HardwareImage ssao_temp;
    Horizon::HardwareImage ssao_filtered;
    Horizon::HardwareImage shadow_raw;
    Horizon::HardwareImage shadow_filtered;
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
[[nodiscard]] bool vision_zero_copy_forced_disabled() {
    static const bool disabled =
        Vision::vision_zero_copy_disabled_from_value(
            std::getenv("CORONA_VISION_DISABLE_ZERO_COPY"));
    return disabled;
}

struct OpticsSystem::VisionPipelineRuntime {
    ocarina::SP<vision::Pipeline> pipeline;
    std::shared_ptr<VisionSceneResource> scene_resource;
    VisionPipelineSource source{VisionPipelineSource::EngineBuilt};
    std::string scene_path;
    std::string scene_json;
    std::string base_dir;
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
    std::unordered_map<std::uintptr_t, Horizon::SubmitReceipt> interop_submissions;
    Horizon::HardwareExecutor* interop_executor{nullptr};

    void wait_for_interop_submission(std::uintptr_t camera_handle,
                                     std::string_view reason,
                                     bool log_wait = true) noexcept {
        auto receipt_it = interop_submissions.find(camera_handle);
        if (receipt_it == interop_submissions.end()) {
            return;
        }
        if (receipt_it->second.empty()) {
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
            "(scene={}, bridges={}, interop_submissions={}, contexts={})",
            scene_path,
            bridges.size(),
            interop_submissions.size(),
            retained_contexts.size());
        if (pipeline) {
            CFW_LOG_INFO("OpticsSystem: Vision runtime teardown committing CUDA work");
            pipeline->commit_command();
            CFW_LOG_INFO("OpticsSystem: Vision runtime teardown CUDA work committed");
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
        CFW_LOG_INFO("OpticsSystem: Vision runtime teardown complete (scene={})", scene_path);
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
        scene_json.clear();
        base_dir.clear();
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
    it->second->interop_executor = hardware_ ? &hardware_->executor : nullptr;
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

        // Embedded Vision scenes use a logical scene key such as
        // `scene.ini.embedded`; it is not a file on disk. When a camera changes
        // render mode, the new mode gets a new runtime key, so carry the JSON
        // payload from any already-loaded runtime for the same scene/source.
        // Capture it before a forced resource reload erases the old runtimes.
        std::string embedded_scene_json;
        std::string embedded_base_dir;
        for (const auto& [existing_key, existing_runtime] : vision_runtimes_) {
            if (existing_key.scene_path != key.scene_path ||
                existing_key.source != key.source ||
                !existing_runtime || existing_runtime->scene_json.empty()) {
                continue;
            }
            embedded_scene_json = existing_runtime->scene_json;
            embedded_base_dir = existing_runtime->base_dir;
            break;
        }

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

        ocarina::SP<vision::Pipeline> pipeline;
        if (!embedded_scene_json.empty()) {
            const auto base_dir = embedded_base_dir.empty()
                                      ? std::filesystem::current_path()
                                      : std::filesystem::u8path(embedded_base_dir);
            pipeline = import_vision_scene_from_data(
                vision::DataWrap::parse(embedded_scene_json),
                base_dir,
                key.scene_path,
                key.mode,
                scene_resource,
                key.source);
        } else {
            pipeline = import_vision_scene_from_file(
                std::filesystem::u8path(key.scene_path),
                key.mode,
                scene_resource,
                key.source);
        }
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
        if (!embedded_scene_json.empty()) {
            runtime.scene_json = std::move(embedded_scene_json);
            runtime.base_dir = std::move(embedded_base_dir);
        }
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
        hardware_->shadowColorImage =
            make_storage_image(kShadowMapSize, kShadowMapSize,
                               Horizon::Format::RGBA32_UINT, "optics.shadow_dummy_color");
        for (uint32_t i = 0; i < kShadowCascadeCount; ++i) {
            hardware_->shadowCascadeImages[i] =
                make_depth_image(kShadowMapSize, kShadowMapSize,
                                 "optics.shadow_cascade_" + std::to_string(i));
        }

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
        hardware_->actorPickBuffer =
            make_storage_buffer<std::uint32_t>(1, "optics.actor_pick");
        hardware_->shadowInfoBuffer =
            make_storage_buffer<Hardware::ShadowInfoBufferObject>(1, "optics.shadow_info");

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
        const auto shadow_desc = make_shadow_pipeline_desc();
        for (auto& shadow_pipeline : hardware_->shadowPipelines) {
            shadow_pipeline.emplace(shadow_desc);
        }
        hardware_->ssaoPipeline.emplace(ssao_comp_glsl, ktm::uvec3(8, 8, 1));
        hardware_->surfaceGuidePipeline.emplace(surface_guide_comp_glsl, ktm::uvec3(8, 8, 1));
        hardware_->shadowMaskPipeline.emplace(shadow_mask_comp_glsl, ktm::uvec3(8, 8, 1));
        hardware_->atrousScalarPipeline.emplace(atrous_scalar_filter_comp_glsl, ktm::uvec3(8, 8, 1));
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
        !resources.visibility || !resources.depth ||
        !resources.surface_guide ||
        !resources.ssao_raw || !resources.ssao_temp || !resources.ssao_filtered ||
        !resources.shadow_raw || !resources.shadow_filtered ||
        !resources.visibility_pipeline) {
        hardware_->executor.wait_for_completion(hardware_->executor.last_receipt());
        resources.visibility =
            make_storage_image(width, height, Horizon::Format::RGBA32_UINT, "optics.native_visibility");
        resources.depth = make_depth_image(width, height, "optics.native_depth");
        resources.surface_guide =
            make_compute_image(width, height, Horizon::Format::RGBA16_FLOAT, "optics.native_surface_guide");
        resources.ssao_raw =
            make_compute_image(width, height, Horizon::Format::R16_FLOAT, "optics.native_ssao_raw");
        resources.ssao_temp =
            make_compute_image(width, height, Horizon::Format::R16_FLOAT, "optics.native_ssao_temp");
        resources.ssao_filtered =
            make_compute_image(width, height, Horizon::Format::R16_FLOAT, "optics.native_ssao_filtered");
        resources.shadow_raw =
            make_compute_image(width, height, Horizon::Format::R16_FLOAT, "optics.native_shadow_raw");
        resources.shadow_filtered =
            make_compute_image(width, height, Horizon::Format::R16_FLOAT, "optics.native_shadow_filtered");
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
                const auto expires_at =
                    std::chrono::steady_clock::now() + kScreenshotRequestTimeout;
                std::lock_guard<std::mutex> lock(screenshot_mutex_);
                pending_screenshots_.push_back(
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
                // Only stash the request here (any thread). The actual import touches
                // the CUDA pipeline and MUST run on the render thread, so it is
                // deferred to apply_pending_vision_scene_load() in update().
                std::lock_guard<std::mutex> lock(vision_scene_load_mutex_);
                pending_vision_scene_load_ = VisionSceneLoadRequest{
                    event.scene_path,
                    event.scene_json,
                    event.base_dir,
                    event.scene_key,
                    event.external_live,
                };
            });
#endif

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

void OpticsSystem::update() {
    if (hardware_) {
        const auto consumed_serial =
            pending_native_consumed_serial_.exchange(0, std::memory_order_acq_rel);
        if (consumed_serial != 0) {
            hardware_->native_frame_throttle.acknowledge_consumed(consumed_serial);
        }
        // Never block the Optics update thread on an in-flight native receipt.
        // The display/driver may retire it later; skipping this frame keeps the
        // UI and event loop responsive when the GPU queue is stalled.
        if (!hardware_->native_frame_throttle.try_acquire_slot()) {
            return;
        }
    }
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

    const bool shadow_pipelines_ready = std::all_of(
        hardware_->shadowPipelines.begin(), hardware_->shadowPipelines.end(),
        [](const auto& pipeline) { return pipeline.has_value(); });
    if (!hardware_->shaderHasInit || !hardware_->lightingPipeline ||
        !shadow_pipelines_ready ||
        !hardware_->ssaoPipeline || !hardware_->surfaceGuidePipeline ||
        !hardware_->shadowMaskPipeline || !hardware_->atrousScalarPipeline ||
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
    auto& shadow_pipelines = hardware_->shadowPipelines;
    auto& ssao = *hardware_->ssaoPipeline;
    auto& surfaceGuide = *hardware_->surfaceGuidePipeline;
    auto& shadowMask = *hardware_->shadowMaskPipeline;
    auto& atrousScalar = *hardware_->atrousScalarPipeline;
    auto& sky = *hardware_->skyPipeline;
    auto& tonemap = *hardware_->tonemapPipeline;
    // UI overlay/warp/composite 管线现由 compose_surface_ui_overlay() 内部使用。

    fail_expired_pending_screenshots();
    fail_unrenderable_pending_screenshots();

    const auto scene_snapshots =
        OpticsDetail::snapshot_storage(SharedDataHub::instance().scene_storage());
    for (const auto& scene : scene_snapshots) {
        if (!scene.enabled)
            continue;

        for (auto cam_handle : scene.camera_handles) {
            auto camera_snapshot = OpticsDetail::snapshot_storage_value(
                SharedDataHub::instance().camera_storage(), cam_handle);
            if (camera_snapshot) {
                const CameraDevice* camera = &*camera_snapshot;
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
                if (hardware_) {
                    if (!hardware_->native_frame_throttle.try_acquire_slot()) {
                        continue;
                    }
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
                        hardware_->executor.wait(consumed_device->consumed_receipt);
                    }
                }
                bind_native_view_resources(cam_handle, camera->width, camera->height,
                                           frame_index);
                auto& native_resources = *native_view_resources_.at(cam_handle);
                auto& visibility = *native_resources.visibility_pipeline;

                // ================================================================
                // 1. Update camera uniform buffers
                // ================================================================
                hardware_->uniformBufferObjects.eyePosition = make_vec4(camera->position, 1.0f);
                hardware_->uniformBufferObjects.eyeDir = make_vec4(camera->forward, 0.0f);
                hardware_->uniformBufferObjects.eyeViewMatrix = camera->compute_view_matrix();
                hardware_->uniformBufferObjects.eyeProjMatrix = camera->compute_projection_matrix();
                hardware_->uniformBufferObjects.eyeInvProjMatrix =
                    make_inverse_projection_matrix(*camera);
                hardware_->vpUniformBufferObjects.viewProjMatrix = camera->compute_view_proj_matrix();

                // ---- Per-submission buffer 租用（消除跨帧/跨相机共享 buffer 覆盖竞争）----
                // 从池各租一份当前无 GPU 在用的 buffer，本相机 pass 全程改用这些 lease，
                // 不再触碰 hardware_->*Buffer 单例（单例仅留给已 wait_idle 的冷路径）。
                // commit 前会 stream << keep_alive(busy)，GPU 用完自动回池复用。
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

                auto collect_actor_instances_for_pass =
                    [&](auto& target_visibility,
                        const ktm::fmat4x4& view_proj_matrix,
                        bool follow_camera_pass,
                        const ktm::fmat4x4* camera_basis,
                        RenderInstanceBatch& batch) -> bool {
                    batch.clear();

                    // ---- Native 视锥剔除（Step 5）----
                    // 消费几何线程算好的 visible_actor_handles（多相机视锥并集）。
                    // 仅用于场景 pass（!follow_camera_pass）：UI/跟随相机 actor 不在世界
                    // 视锥可见集内，不可据此剔除。空集 → 回退全量（与 Vision 一致），
                    // 避免可见集尚未算出时整屏消失。
                    // 注意：object_id 是 actor_handles 的 1-based 全量下标，作为 V-buffer
                    // objectID 用。剔除采用"跳过绘制但仍 ++object_id"，保证每个被渲染
                    // 物体的 objectID 与剔除前完全一致（纯减少 draw call，不改编号语义）。
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

                        // 跳过未加载的 actor（GPU 资源已释放，无法渲染）
                        {
                            std::shared_lock lock(residency_mtx_);
                            if (!resident_actors_.count(actor_handle)) {
                                ++object_id;
                                continue;
                            }
                        }

                        // 续期资源访问时间（驱动 ResourceManager LRU）
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
                                break;  // 一个 actor 只续期一次
                            }
                        }

                        if (actor->follow_camera != follow_camera_pass) {
                            ++object_id;
                            continue;
                        }

                        // ---- 视锥剔除：不在可见集内 → 跳过绘制（仍 ++object_id 保编号）----
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
                            // ---- 提取几何变换信息（读锁）----
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

                            // ---- 计算包围半径（LOD 选级用）----
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

                            // ---- query_mesh_slots：统一接口，LOD 已解析，无需手动 fallback ----
                            const auto mesh_slots_b = (geometry_system_ && have_bounds)
                                ? geometry_system_->query_mesh_slots(
                                      optics.geometry_handle,
                                      camera->position, camera->fov,
                                      world_center, bounding_radius)
                                : (geometry_system_
                                      ? geometry_system_->query_mesh_slots(optics.geometry_handle)
                                      : std::vector<GeometrySystem::MeshSlot>{});

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
                                        : ms.texture.storeSampledDescriptor();
                                const uint32_t vertex_descriptor = vertex_storage_valid
                                    ? ms.geo.vertex_storage.storeDescriptor()
                                    : 0u;
                                const uint32_t index_descriptor = index_storage_valid
                                    ? ms.geo.index_storage.storeDescriptor()
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

                                auto materialID = static_cast<uint32_t>(batch.materials.size());
                                {
                                    Hardware::MaterialInfo mat_info{};
                                    const bool lighting_active =
                                        optics.bEnableLighting && !diag.force_solid_material;
                                    float lighting_enabled = lighting_active ? 1.0f : 0.0f;
                                    mat_info.textureDescriptor = texture_descriptor;
                                    if (lighting_active) {
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
                                        mat_info.metallic = 0.0f; mat_info.roughness = 1.0f;
                                        mat_info.subsurface = 0.0f; mat_info.specular = 0.0f;
                                        mat_info.specularTint = 0.0f; mat_info.anisotropic = 0.0f;
                                        mat_info.sheen = 0.0f; mat_info.sheenTint = 0.0f;
                                        mat_info.clearcoat = 0.0f; mat_info.clearcoatGloss = 0.0f;
                                    }
                                    mat_info.lightingEnabled = lighting_enabled;
                                    mat_info.materialColor = ktm::fvec4{
                                        ms.material_color[0], ms.material_color[1],
                                        ms.material_color[2], ms.material_color[3]};
                                    batch.materials.push_back(mat_info);
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
                                    inst.modelMatrix = model_matrix;
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
                                    has_instances = true;
                                }

                                const ktm::fmat4x4 clip_matrix =
                                    multiply_ktm_mat4(view_proj_matrix, model_matrix);
                                target_visibility[visibility_vert_glsl_t::pushConsts::modelMatrix] =
                                    upload_value(clip_matrix);
                                target_visibility[visibility_vert_glsl_t::pushConsts::uniformBufferIndex] =
                                    0u;
                                target_visibility[visibility_vert_glsl_t::pushConsts::instanceID] =
                                    instanceID + 1;
                                target_visibility[visibility_frag_glsl_t::pushConsts::textureIndex] =
                                    texture_descriptor;
                                Horizon::DrawIndexedParams draw_params;
                                draw_params.debug_label = make_optics_draw_label(
                                    follow_camera_pass ? "follow_visibility" : "scene_visibility",
                                    actor_handle,
                                    optics.geometry_handle,
                                    ms.mesh_index,
                                    static_cast<std::uint32_t>(frame_index),
                                    instanceID + 1,
                                    materialID,
                                    texture_descriptor,
                                    vertex_descriptor,
                                    index_descriptor,
                                    ms.vertex_count,
                                    ms.index_count,
                                    ms.max_index);
                                target_visibility.record(ms.geo.index, ms.geo.vertex, draw_params);
                                ++recorded_draws;
                            }
                            ++object_id;
                        }
                    }
                    return has_instances;
                };

                struct ShadowCasterSnapshot {
                    std::uintptr_t actor_handle = 0;
                    std::uintptr_t profile_handle = 0;
                    std::uintptr_t geometry_handle = 0;
                    ktm::fmat4x4 model_matrix{ktm::fmat4x4::from_eye()};
                    Corona::Systems::OpticsDetail::ShadowCasterBoundsSnapshot bounds{};
                    float max_abs_scale = 1.0f;
                    std::uint32_t cascade_visibility_mask = 0;
                    std::array<std::vector<GeometrySystem::ShadowMeshSlot>,
                               kShadowCascadeCount> cascade_slots{};
                };

                ShadowSceneBounds shadow_scene_bounds;
                std::vector<ShadowCasterSnapshot> shadow_casters;
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
                            snapshot.model_matrix = apply_native_local_correction(
                                transform->compute_matrix(), *geom);
                            snapshot.max_abs_scale = std::max({
                                std::abs(transform->scale.x),
                                std::abs(transform->scale.y),
                                std::abs(transform->scale.z),
                                std::abs(geom->native_local_correction_scale),
                                1.0e-6f});

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
                build_shadow_caster_snapshots();

                std::array<Corona::Systems::OpticsDetail::ShadowCascadeView,
                           kShadowCascadeCount> shadow_cascade_views{};

                auto prepare_shadow_caster_meshes = [&]() {
                    const std::uint32_t enabled_mask =
                        (!is_debug_mode && !diag.skip_shadows && native_shadows_enabled)
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
                        }
                    }
                };

                auto record_shadow_cascade =
                    [&](const ktm::fmat4x4& light_view_proj,
                        Horizon::HardwareImage& shadow_depth,
                        uint32_t cascade_index) {
                    auto& shadow = *shadow_pipelines[cascade_index];
                    shadow.clear_records();
                    shadow.bind_render_target(0, hardware_->shadowColorImage);
                    shadow.bind_depth_target(shadow_depth);
                    uint32_t shadow_draws = 0;
                    for (const auto& caster : shadow_casters) {
                        if ((caster.cascade_visibility_mask & (1u << cascade_index)) == 0u) {
                            continue;
                        }
                        for (const auto& slot : caster.cascade_slots[cascade_index]) {
                            if (!diag_mesh_allowed(slot.mesh_index)) continue;
                            if (shadow_draws >= diag.draw_limit) return;
                            if (!slot.valid) continue;

                            const ktm::fmat4x4 clip_matrix = multiply_ktm_mat4(
                                light_view_proj, caster.model_matrix);
                            shadow[shadow_vert_glsl_t::pushConsts::lightViewProjModel] =
                                upload_value(clip_matrix);
                            Horizon::DrawIndexedParams draw_params;
                            draw_params.debug_label = make_optics_draw_label(
                                "shadow",
                                caster.actor_handle,
                                caster.geometry_handle,
                                slot.mesh_index,
                                static_cast<std::uint32_t>(frame_index),
                                0u,
                                0u,
                                0u,
                                0u,
                                0u,
                                slot.vertex_count,
                                slot.index_count,
                                slot.max_index);
                            shadow.record(slot.geo.index, slot.geo.vertex, draw_params);
                            ++shadow_draws;
                            ++native_cascade_draws[cascade_index];
                            native_cascade_indices[cascade_index] += slot.index_count;
                        }
                    }
                };

                const uint32_t sceneVpDescriptor = sceneVpBuffer.storeDescriptor();
                (void)sceneVpDescriptor;
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
                if (ktm::length(sun_dir) < 1e-5f) {
                    sun_dir = make_vec3(1.0f, 1.0f, 1.0f);
                }
                sun_dir = ktm::normalize(sun_dir);

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
                        hardware_->shadowCascadeImages[cascade].storeSampledDescriptor();
                    cascade_near = cascade_far;
                }
                hardware_->shadowInfoBufferObjects.cascadeSplits = ktm::fvec4{
                    shadow_splits[0], shadow_splits[1], shadow_splits[2], shadow_splits[3]};
                hardware_->shadowInfoBufferObjects.shadowMapSize =
                    static_cast<float>(kShadowMapSize);
                hardware_->shadowInfoBufferObjects.shadowBias = kShadowBias;
                hardware_->shadowInfoBufferObjects.shadowEnabled =
                    (!diag.skip_shadows && diag.shadow_cascade_mask != 0u &&
                     sun_intensity > 0.0f) ? 1u : 0u;
                native_shadows_enabled = hardware_->shadowInfoBufferObjects.shadowEnabled != 0u;
                prepare_shadow_caster_meshes();
                (void)write_object_bytes(sceneShadowBuffer,
                                         hardware_->shadowInfoBufferObjects);

                (void)write_object_bytes(sceneUboBuffer,
                                         hardware_->uniformBufferObjects);
                const uint32_t uboDescriptor = sceneUboBuffer.storeDescriptor();
                const uint32_t depthSampledDescriptor =
                    hardware_->depthImage.storeSampledDescriptor();

                // Offscreen cameras (no surface) render to a dedicated image so
                // they never collide with the display pipeline's per-surface output.
                // Display cameras render into their own surface target (改造1).
                Horizon::HardwareImage& render_target = target.final_output;
                Horizon::HardwareImage* presented_target = &render_target;
                const uint32_t finalOutputDescriptor = render_target.storeStorageDescriptor();
                const uint32_t visibilityStorageDescriptor =
                    hardware_->visibilityImage.storeStorageDescriptor();
                const uint32_t surfaceGuideStorageDescriptor =
                    native_resources.surface_guide.storeStorageDescriptor();
                const uint32_t surfaceGuideSampledDescriptor =
                    native_resources.surface_guide.storeSampledDescriptor();
                const uint32_t ssaoRawStorageDescriptor =
                    native_resources.ssao_raw.storeStorageDescriptor();
                const uint32_t ssaoRawSampledDescriptor =
                    native_resources.ssao_raw.storeSampledDescriptor();
                const uint32_t ssaoTempStorageDescriptor =
                    native_resources.ssao_temp.storeStorageDescriptor();
                const uint32_t ssaoTempSampledDescriptor =
                    native_resources.ssao_temp.storeSampledDescriptor();
                const uint32_t ssaoFilteredStorageDescriptor =
                    native_resources.ssao_filtered.storeStorageDescriptor();
                const uint32_t ssaoFilteredSampledDescriptor =
                    native_resources.ssao_filtered.storeSampledDescriptor();
                const uint32_t shadowRawStorageDescriptor =
                    native_resources.shadow_raw.storeStorageDescriptor();
                const uint32_t shadowRawSampledDescriptor =
                    native_resources.shadow_raw.storeSampledDescriptor();
                const uint32_t shadowFilteredStorageDescriptor =
                    native_resources.shadow_filtered.storeStorageDescriptor();
                const uint32_t shadowFilteredSampledDescriptor =
                    native_resources.shadow_filtered.storeSampledDescriptor();

                const bool is_ssao_debug_mode =
                    camera->output_mode == CameraOutputMode::SSAO ||
                    camera->output_mode == CameraOutputMode::SSAORaw;
                const bool is_shadow_mask_debug_mode =
                    camera->output_mode == CameraOutputMode::ShadowMaskRaw ||
                    camera->output_mode == CameraOutputMode::ShadowMask;
                const bool should_run_ssao = camera->ssao_enabled || is_ssao_debug_mode;
                const bool should_run_shadow_mask =
                    hardware_->shadowInfoBufferObjects.shadowEnabled != 0u ||
                    is_shadow_mask_debug_mode;
                const uint32_t sceneMaterialCount =
                    static_cast<uint32_t>(sceneBatch.materials.size());

                // ================================================================
                // 4c. SSAO and shadow masks: guide + raw scalar maps.
                // ================================================================
                surfaceGuide.pushConsts.gbufferSize = upload_value(hardware_->gbufferSize);
                surfaceGuide.pushConsts.visibilityImageIndex = visibilityStorageDescriptor;
                surfaceGuide.pushConsts.depthImageIndex = depthSampledDescriptor;
                surfaceGuide.pushConsts.instanceInfoBufferIndex =
                    hardware_->instanceInfoBuffer.storeDescriptor();
                surfaceGuide.pushConsts.vpBufferIndex =
                    hardware_->vpUniformBuffer.storeDescriptor();
                surfaceGuide.pushConsts.uniformBufferIndex = uboDescriptor;
                surfaceGuide.pushConsts.outputImageIndex = surfaceGuideStorageDescriptor;
                surfaceGuide.bind_storage_image(0, hardware_->visibilityImage);
                surfaceGuide.bind_storage_image(1, native_resources.surface_guide);

                ssao.pushConsts.gbufferSize = upload_value(hardware_->gbufferSize);
                ssao.pushConsts.visibilityImageIndex = visibilityStorageDescriptor;
                ssao.pushConsts.depthImageIndex = depthSampledDescriptor;
                ssao.pushConsts.instanceInfoBufferIndex =
                    sceneInstanceBuffer.storeDescriptor();
                ssao.pushConsts.instanceCount = native_instance_count;
                ssao.pushConsts.materialCount = sceneMaterialCount;
                ssao.pushConsts.vpBufferIndex =
                    sceneVpBuffer.storeDescriptor();
                ssao.pushConsts.uniformBufferIndex = uboDescriptor;
                ssao.pushConsts.outputImageIndex = ssaoRawStorageDescriptor;
                ssao.pushConsts.radius = kSsaoRadius;
                ssao.pushConsts.bias = kSsaoBias;
                ssao.pushConsts.power = kSsaoPower;
                ssao.pushConsts.sampleCount = kSsaoSampleCount;
                ssao.bind_storage_image(0, hardware_->visibilityImage);
                ssao.bind_storage_image(1, native_resources.ssao_raw);

                shadowMask.pushConsts.gbufferSize = upload_value(hardware_->gbufferSize);
                shadowMask.pushConsts.visibilityImageIndex = visibilityStorageDescriptor;
                shadowMask.pushConsts.depthImageIndex = depthSampledDescriptor;
                shadowMask.pushConsts.instanceInfoBufferIndex =
                    hardware_->instanceInfoBuffer.storeDescriptor();
                shadowMask.pushConsts.vpBufferIndex =
                    hardware_->vpUniformBuffer.storeDescriptor();
                shadowMask.pushConsts.uniformBufferIndex = uboDescriptor;
                shadowMask.pushConsts.shadowInfoBufferIndex =
                    hardware_->shadowInfoBuffer.storeDescriptor();
                shadowMask.pushConsts.outputImageIndex = shadowRawStorageDescriptor;
                shadowMask.pushConsts.sun_dir = upload_value(sun_dir);
                shadowMask.bind_storage_image(0, hardware_->visibilityImage);
                shadowMask.bind_storage_image(1, native_resources.shadow_raw);

                // ================================================================
                // 5. Lighting pass: VBuffer decode + PBR direct illumination
                // ================================================================
                lighting.pushConsts.gbufferSize = upload_value(hardware_->gbufferSize);
                lighting.pushConsts.visibilityImageIndex =
                    hardware_->visibilityImage.storeStorageDescriptor();
                lighting.pushConsts.depthImageIndex = depthSampledDescriptor;
                lighting.pushConsts.instanceInfoBufferIndex =
                    sceneInstanceBuffer.storeDescriptor();
                lighting.pushConsts.materialTableBufferIndex =
                    sceneMaterialBuffer.storeDescriptor();
                lighting.pushConsts.instanceCount = native_instance_count;
                lighting.pushConsts.materialCount = sceneMaterialCount;
                lighting.pushConsts.vpBufferIndex =
                    sceneVpBuffer.storeDescriptor();
                lighting.pushConsts.finalOutputImage = finalOutputDescriptor;
                lighting.pushConsts.uniformBufferIndex = uboDescriptor;
                lighting.bind_storage_image(0, hardware_->visibilityImage);
                lighting.bind_storage_image(1, render_target);
                lighting.pushConsts.skyIrradianceSHBufferIndex =
                    hardware_->skyIrradianceSHBuffer.storeDescriptor();
                lighting.pushConsts.ambientIntensity = 0.0f;
                lighting.pushConsts.sun_dir = upload_value(sun_dir);
                lighting.pushConsts.shadowEnabled =
                    hardware_->shadowInfoBufferObjects.shadowEnabled;
                lighting.pushConsts.shadowInfoBufferIndex =
                    sceneShadowBuffer.storeDescriptor();
                lighting.pushConsts.shadowCascadeDebug =
                    camera->shadow_cascade_debug ? 1u : 0u;
                lighting.pushConsts.shadowMaskImageIndex = shadowFilteredSampledDescriptor;
                lighting.pushConsts.ssaoImageIndex = ssaoFilteredSampledDescriptor;
                lighting.pushConsts.ssaoEnabled = camera->ssao_enabled ? 1u : 0u;
                lighting.pushConsts.ssaoStrength = kSsaoStrength;
                lighting.pushConsts.skyAmbientEnabled = kSkyAmbientEnabled ? 1u : 0u;
                lighting.pushConsts.disableAlbedoSample =
                    diag.disable_albedo_sample ? 1u : 0u;
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
                    kSkyAmbientEnabled &&
                    (!sky_sh_initialized_ || sky_sig != sky_sh_signature_);
                if (sky_sh_needs_update) {
                    auto& skySH = *hardware_->skySHProjectPipeline;
                    skySH.pushConsts.outputBufferIndex =
                        hardware_->skyIrradianceSHBuffer.storeDescriptor();
                    skySH.pushConsts.sampleCount = 64u;
                    skySH.pushConsts.sun_dir = upload_value(sun_dir);
                    skySH.pushConsts.sky_intensity = sky_intensity;
                    skySH.set_debug_label(make_optics_dispatch_label(
                        "sky_sh_project",
                        static_cast<std::uint32_t>(frame_index),
                        native_instance_count,
                        sceneMaterialCount,
                        hardware_->gbufferSize.x,
                        hardware_->gbufferSize.y));
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
                // dispatch 参数是 workgroup 组数(原样进 vkCmdDispatch)。这些延迟通道
                // 全是 local_size 8x8, 用 dispatch_groups 从像素换算 ceil(w/8)xceil(h/8);
                // 除数取自管线反射的真实 local size(已由 Horizon SPIR-V patch 修正)。
                // 曾经直接传裸像素 gbufferSize 会导致 64x 超发(越界 guard 使画面仍对但极浪费)。
                const auto [dispatchX, dispatchY] =
                    lighting.dispatch_groups(hardware_->gbufferSize.x, hardware_->gbufferSize.y);
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
                    actorPick.set_debug_label(make_optics_dispatch_label(
                        "actor_pick",
                        static_cast<std::uint32_t>(frame_index),
                        native_instance_count,
                        sceneMaterialCount,
                        hardware_->gbufferSize.x,
                        hardware_->gbufferSize.y));
                }

                ssao.set_debug_label(make_optics_dispatch_label(
                    "ssao",
                    static_cast<std::uint32_t>(frame_index),
                    native_instance_count,
                    sceneMaterialCount,
                    hardware_->gbufferSize.x,
                    hardware_->gbufferSize.y));
                lighting.set_debug_label(make_optics_dispatch_label(
                    "lighting",
                    static_cast<std::uint32_t>(frame_index),
                    native_instance_count,
                    sceneMaterialCount,
                    hardware_->gbufferSize.x,
                    hardware_->gbufferSize.y));
                sky.set_debug_label(make_optics_dispatch_label(
                    "sky",
                    static_cast<std::uint32_t>(frame_index),
                    native_instance_count,
                    sceneMaterialCount,
                    hardware_->gbufferSize.x,
                    hardware_->gbufferSize.y));
                tonemap.set_debug_label(make_optics_dispatch_label(
                    "tonemap",
                    static_cast<std::uint32_t>(frame_index),
                    native_instance_count,
                    sceneMaterialCount,
                    hardware_->gbufferSize.x,
                    hardware_->gbufferSize.y));

                Horizon::SubmitReceipt latest_submit_receipt;
                {
                    const auto native_submit_start = PerfClock::now();
                    auto stream = hardware_->executor.stream();
                    bool native_submission_cancelled = false;
                    auto dispatch_atrous = [&](uint32_t inputSampledDescriptor,
                                               uint32_t outputStorageDescriptor,
                                               Horizon::HardwareImage& outputImage,
                                               uint32_t filterStep,
                                               float valueSigma) {
                        atrousScalar.pushConsts.gbufferSize = upload_value(hardware_->gbufferSize);
                        atrousScalar.pushConsts.inputImageIndex = inputSampledDescriptor;
                        atrousScalar.pushConsts.outputImageIndex = outputStorageDescriptor;
                        atrousScalar.pushConsts.guideImageIndex = surfaceGuideSampledDescriptor;
                        atrousScalar.pushConsts.visibilityImageIndex = visibilityStorageDescriptor;
                        atrousScalar.pushConsts.filterStep = filterStep;
                        atrousScalar.pushConsts.valueSigma = valueSigma;
                        atrousScalar.pushConsts.normalPower = kAtrousNormalPower;
                        atrousScalar.pushConsts.normalThreshold = kAtrousNormalThreshold;
                        atrousScalar.pushConsts.depthSigmaScale = kAtrousDepthSigmaScale;
                        atrousScalar.pushConsts.depthSigmaMin = kAtrousDepthSigmaMin;
                        atrousScalar.bind_storage_image(0, hardware_->visibilityImage);
                        atrousScalar.bind_storage_image(1, outputImage);
                        stream << atrousScalar(dispatchX, dispatchY, 1);
                    };
                    auto dispatch_ssao_atrous = [&]() {
                        uint32_t inputSampledDescriptor = ssaoRawSampledDescriptor;
                        for (uint32_t pass = 0; pass < kSsaoAtrousPasses; ++pass) {
                            const bool is_last = pass + 1u == kSsaoAtrousPasses;
                            Horizon::HardwareImage& outputImage =
                                is_last ? native_resources.ssao_filtered : native_resources.ssao_temp;
                            const uint32_t outputStorageDescriptor =
                                is_last ? ssaoFilteredStorageDescriptor : ssaoTempStorageDescriptor;
                            dispatch_atrous(inputSampledDescriptor,
                                            outputStorageDescriptor,
                                            outputImage,
                                            1u << pass,
                                            kSsaoAtrousValueSigma);
                            inputSampledDescriptor =
                                is_last ? ssaoFilteredSampledDescriptor : ssaoTempSampledDescriptor;
                        }
                    };
                    auto dispatch_shadow_atrous = [&]() {
                        uint32_t inputSampledDescriptor = shadowRawSampledDescriptor;
                        for (uint32_t pass = 0; pass < kShadowAtrousPasses; ++pass) {
                            dispatch_atrous(inputSampledDescriptor,
                                            shadowFilteredStorageDescriptor,
                                            native_resources.shadow_filtered,
                                            1u << pass,
                                            kShadowAtrousValueSigma);
                            inputSampledDescriptor = shadowFilteredSampledDescriptor;
                        }
                    };
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
                            case CameraOutputMode::SSAO:
                                debugMode = 5;
                                break;
                            case CameraOutputMode::SSAORaw:
                                debugMode = 6;
                                break;
                            case CameraOutputMode::ShadowMaskRaw:
                                debugMode = 7;
                                break;
                            case CameraOutputMode::ShadowMask:
                                debugMode = 8;
                                break;
                            default:
                                debugMode = 0;
                                break;
                        }

                        visibility(hardware_->gbufferSize.x, hardware_->gbufferSize.y);
                        stream.append_consuming(visibility);
                        if (!diag.skip_deferred_compute) {
                            if (debugMode == 5u || debugMode == 6u ||
                                debugMode == 7u || debugMode == 8u) {
                                if ((debugMode == 7u || debugMode == 8u) &&
                                    !diag.skip_shadows &&
                                    hardware_->shadowInfoBufferObjects.shadowEnabled != 0u) {
                                    Corona::Systems::OpticsDetail::for_each_enabled_shadow_cascade(
                                        diag.shadow_cascade_mask,
                                        kShadowCascadeCount,
                                        [&](std::uint32_t cascade) {
                                            record_shadow_cascade(
                                                hardware_->shadowInfoBufferObjects
                                                    .lightViewProj[cascade],
                                                hardware_->shadowCascadeImages[cascade],
                                                cascade);
                                            auto& cascade_pipeline =
                                                *shadow_pipelines[cascade];
                                            cascade_pipeline(kShadowMapSize, kShadowMapSize);
                                            stream.append_consuming(cascade_pipeline);
                                        });
                                }
                                stream << surfaceGuide(dispatchX, dispatchY, 1);
                            }
                            if (debugMode == 5u || debugMode == 6u) {
                                stream << ssao(dispatchX, dispatchY, 1);
                                if (debugMode == 5u) {
                                    dispatch_ssao_atrous();
                                }
                            }
                            if (debugMode == 7u || debugMode == 8u) {
                                stream << shadowMask(dispatchX, dispatchY, 1);
                                if (debugMode == 8u) {
                                    dispatch_shadow_atrous();
                                }
                            }
                            if (debugMode == 4u) {
                                auto& visibilityDebugResolve = *hardware_->visibilityDebugResolvePipeline;
                                visibilityDebugResolve.pushConsts.gbufferSize =
                                    upload_value(hardware_->gbufferSize);
                                visibilityDebugResolve.pushConsts.visibilityImageIndex =
                                    hardware_->visibilityImage.storeSampledDescriptor();
                                visibilityDebugResolve.pushConsts.outputImageIndex =
                                    render_target.storeStorageDescriptor();
                                visibilityDebugResolve.bind_storage_image(0, render_target);
                                visibilityDebugResolve.set_debug_label(make_optics_dispatch_label(
                                    "visibility_debug_resolve",
                                    static_cast<std::uint32_t>(frame_index),
                                    native_instance_count,
                                    sceneMaterialCount,
                                    hardware_->gbufferSize.x,
                                    hardware_->gbufferSize.y));
                                stream << visibilityDebugResolve(dispatchX, dispatchY, 1);
                            } else {
                                auto& debugResolve = *hardware_->debugResolvePipeline;

                                debugResolve.pushConsts.gbufferSize = upload_value(hardware_->gbufferSize);
                                debugResolve.pushConsts.visibilityImageIndex =
                                    hardware_->visibilityImage.storeSampledDescriptor();
                                debugResolve.pushConsts.depthImageIndex = depthSampledDescriptor;
                                debugResolve.pushConsts.instanceInfoBufferIndex =
                                    sceneInstanceBuffer.storeDescriptor();
                                debugResolve.pushConsts.materialTableBufferIndex =
                                    sceneMaterialBuffer.storeDescriptor();
                                debugResolve.pushConsts.instanceCount = native_instance_count;
                                debugResolve.pushConsts.materialCount = sceneMaterialCount;
                                debugResolve.pushConsts.vpBufferIndex =
                                    sceneVpBuffer.storeDescriptor();
                                debugResolve.pushConsts.outputImageIndex = finalOutputDescriptor;
                                debugResolve.pushConsts.debugMode = debugMode;
                                debugResolve.pushConsts.uniformBufferIndex = uboDescriptor;
                                debugResolve.pushConsts.shadowInfoBufferIndex =
                                    sceneShadowBuffer.storeDescriptor();
                                debugResolve.pushConsts.shadowCascadeDebug =
                                    camera->shadow_cascade_debug ? 1u : 0u;
                                debugResolve.pushConsts.ssaoImageIndex =
                                    ssaoFilteredSampledDescriptor;
                                debugResolve.pushConsts.ssaoRawImageIndex =
                                    ssaoRawSampledDescriptor;
                                debugResolve.pushConsts.shadowMaskRawImageIndex =
                                    shadowRawSampledDescriptor;
                                debugResolve.pushConsts.shadowMaskImageIndex =
                                    shadowFilteredSampledDescriptor;
                                debugResolve.pushConsts.disableAlbedoSample =
                                    diag.disable_albedo_sample ? 1u : 0u;
                                debugResolve.bind_storage_image(0, render_target);
                                debugResolve.set_debug_label(make_optics_dispatch_label(
                                    "debug_resolve",
                                    static_cast<std::uint32_t>(frame_index),
                                    native_instance_count,
                                    sceneMaterialCount,
                                    hardware_->gbufferSize.x,
                                    hardware_->gbufferSize.y));

                                stream << debugResolve(dispatchX, dispatchY, 1);
                            }
                        }
                    } else {
                        // ============================================================
                        // Normal rendering path: full pipeline
                        // ============================================================
                        visibility(hardware_->gbufferSize.x, hardware_->gbufferSize.y);
                        stream.append_consuming(visibility);
                        if (!diag.skip_shadows &&
                            hardware_->shadowInfoBufferObjects.shadowEnabled != 0u) {
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
                                        cascade_pipeline(kShadowMapSize, kShadowMapSize);
                                        stream.append_consuming(cascade_pipeline);
                                    });
                            }
                            native_shadow_ms =
                                elapsed_ms(native_shadow_start, PerfClock::now());
                        }
                        if (!diag.skip_deferred_compute) {
                            if (should_run_ssao) {
                                stream << surfaceGuide(dispatchX, dispatchY, 1)
                                       << ssao(dispatchX, dispatchY, 1);
                                dispatch_ssao_atrous();
                            } else if (should_run_shadow_mask) {
                                stream << surfaceGuide(dispatchX, dispatchY, 1);
                            }
                            if (should_run_shadow_mask) {
                                stream << shadowMask(dispatchX, dispatchY, 1);
                                dispatch_shadow_atrous();
                            }
                            if (sky_sh_needs_update) {
                                stream << (*hardware_->skySHProjectPipeline)(1, 1, 1);
                            }
                            stream << lighting(dispatchX, dispatchY, 1)
                                   << sky(dispatchX, dispatchY, 1)
                                   << tonemap(dispatchX, dispatchY, 1);
                        }
                    }

                    if (native_submission_cancelled) {
                        continue;
                    }

                    if (actor_pick_request && !diag.skip_deferred_compute) {
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

                    const auto native_commit_start = PerfClock::now();
                    // 保活本相机租用的 buffer 至 GPU 完成：executor 持有这些 busy 哨兵，
                    // 直到本 submission 的 timeline semaphore 完成才 retire 掉，届时 busy
                    // use_count 落回 1，池方可复用该 buffer。这是消除覆盖竞争的关键。
                    stream << Horizon::keep_alive(vpLease.busy)
                           << Horizon::keep_alive(uboLease.busy)
                           << Horizon::keep_alive(shadowLease.busy)
                           << Horizon::keep_alive(instLease.busy)
                           << Horizon::keep_alive(matLease.busy)
                           << Horizon::keep_alive(sceneBatch.resource_keep_alive);
                    latest_submit_receipt = stream << Horizon::commit();
                    native_commit_ms = elapsed_ms(native_commit_start, PerfClock::now());
                    native_submit_ms = elapsed_ms(native_submit_start, PerfClock::now());
                }
                if (hardware_) {
                    hardware_->native_frame_throttle.submitted(latest_submit_receipt);
                }

                if (!is_debug_mode && sky_sh_needs_update) {
                    sky_sh_signature_ = sky_sig;
                    sky_sh_initialized_ = true;
                }

                if (actor_pick_request && !diag.skip_deferred_compute) {
                    complete_actor_pick(*actor_pick_request, sceneBatch.actorHandles);
                }


                    // 仅在真正发生合成时 commit，保持无 follow-actor 时的原有行为
                    // （此时 render_target 已在上方 scene pass 提交）。



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

                // 显示相机把自己 surface 的输出发布给 DisplaySystem（按 surface 区分）。
                if (auto image_device =
                        SharedDataHub::instance().image_storage().acquire_write(target.image_handle)) {
                    if (latest_submit_receipt.empty()) {
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
    auto& uiVisibility = *hardware_->uiVisibilityPipeline;
    auto& opticsOverlay = *hardware_->opticsOverlayPipeline;
    auto& opticsCursor = *hardware_->opticsCursorPipeline;
    auto& opticsUiWarp = *hardware_->opticsUiWarpPipeline;
    auto& opticsComposite = *hardware_->opticsCompositePipeline;

    // 组数换算用管线反射的真实 local size(经 Horizon SPIR-V patch, 均为 8x8)。
    const auto [dispatchX, dispatchY] =
        opticsComposite.dispatch_groups(hardware_->gbufferSize.x, hardware_->gbufferSize.y);
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
    // UI overlay pass 同样每相机重写共享 buffer，与场景 pass 同一竞争；改为池租用。
    // keep_alive 挂在传入的 stream 上（与本 overlay 的 dispatch 同一 submission）。
    auto uiVpLease = hardware_->uiVpUniformBufferPool.acquire(
        [] { return make_storage_buffer<Hardware::VPUniformBufferObject>(1, "optics.ui_vp_uniform.pool"); });
    Horizon::HardwareBuffer& uiVpBuffer = *uiVpLease.buffer;
    (void)write_object_bytes(uiVpBuffer,
                             hardware_->vpUniformBufferObjects);
    const uint32_t uiVpDescriptor = uiVpBuffer.storeDescriptor();
    stream << Horizon::keep_alive(uiVpLease.busy);

    uiVisibility.visibilityData = hardware_->uiVisibilityImage;
    uiVisibility.bind_depth_target(hardware_->uiDepthImage);

    RenderInstanceBatch uiBatch;
    const bool has_follow_camera_instances =
        collect_actor_instances_for_visibility(scene, uiVisibility, uiVpDescriptor,
                                               /*follow_camera_pass=*/true,
                                               &camera_basis, uiBatch, geometry_system_, frame_index);

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
        stream << Horizon::keep_alive(uiInstLease.busy)
               << Horizon::keep_alive(uiMatLease.busy)
               << Horizon::keep_alive(uiBatch.resource_keep_alive);

        if (follow_camera_overlay_ready) {
            opticsOverlay.pushConsts.gbufferSize = upload_value(hardware_->gbufferSize);
            opticsOverlay.pushConsts.visibilityImageIndex =
                hardware_->uiVisibilityImage.storeStorageDescriptor();
            opticsOverlay.pushConsts.instanceInfoBufferIndex =
                uiInstanceBuffer.storeDescriptor();
            opticsOverlay.pushConsts.materialTableBufferIndex =
                uiMaterialBuffer.storeDescriptor();
            opticsOverlay.pushConsts.instanceCount =
                static_cast<std::uint32_t>(uiBatch.instances.size());
            opticsOverlay.pushConsts.materialCount =
                static_cast<std::uint32_t>(uiBatch.materials.size());
            opticsOverlay.pushConsts.vpBufferIndex = uiVpDescriptor;
            opticsOverlay.pushConsts.outputImage = overlayDescriptor;
            opticsOverlay.bind_storage_image(0, hardware_->uiVisibilityImage);
            opticsOverlay.bind_storage_image(1, target.ui_overlay);
            opticsOverlay.set_debug_label(make_optics_dispatch_label(
                "ui_overlay",
                static_cast<std::uint32_t>(frame_index),
                static_cast<std::uint32_t>(uiBatch.instances.size()),
                static_cast<std::uint32_t>(uiBatch.materials.size()),
                hardware_->gbufferSize.x,
                hardware_->gbufferSize.y));
        }
    }

    if (!follow_camera_overlay_ready && !cursor_visible) {
        return &background;
    }

    if (cursor_visible && cursor_state != nullptr) {
        const bool preserve_existing_overlay = follow_camera_overlay_ready;
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
        opticsCursor.set_debug_label(make_optics_dispatch_label(
            "ui_cursor",
            static_cast<std::uint32_t>(frame_index),
            static_cast<std::uint32_t>(uiBatch.instances.size()),
            static_cast<std::uint32_t>(uiBatch.materials.size()),
            hardware_->gbufferSize.x,
            hardware_->gbufferSize.y));
        const auto [cw, ch] = opticsCursor.dispatch_groups(cursor_width, cursor_height);
        cursorDispatchX = cw;
        cursorDispatchY = ch;
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
        opticsUiWarp.set_debug_label(make_optics_dispatch_label(
            "ui_warp",
            static_cast<std::uint32_t>(frame_index),
            static_cast<std::uint32_t>(uiBatch.instances.size()),
            static_cast<std::uint32_t>(uiBatch.materials.size()),
            hardware_->gbufferSize.x,
            hardware_->gbufferSize.y));
    }

    opticsComposite.pushConsts.bgImage = background.storeStorageDescriptor();
    opticsComposite.pushConsts.fgImage = compositeOverlayDescriptor;
    opticsComposite.pushConsts.outputImage = target.composite_output.storeStorageDescriptor();
    opticsComposite.pushConsts.outputWidth = hardware_->gbufferSize.x;
    opticsComposite.pushConsts.outputHeight = hardware_->gbufferSize.y;
    opticsComposite.bind_storage_image(0, background);
    opticsComposite.bind_storage_image(1, stereo_ui ? target.ui_warped_overlay : target.ui_overlay);
    opticsComposite.bind_storage_image(2, target.composite_output);
    opticsComposite.set_debug_label(make_optics_dispatch_label(
        "ui_composite",
        static_cast<std::uint32_t>(frame_index),
        static_cast<std::uint32_t>(uiBatch.instances.size()),
        static_cast<std::uint32_t>(uiBatch.materials.size()),
        hardware_->gbufferSize.x,
        hardware_->gbufferSize.y));

    if (follow_camera_overlay_ready) {
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
                                           scene_batch,
                                           geometry_system_,
                                           frame_index);
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
    actor_pick.pushConsts.visibilityImageIndex =
        hardware_->visibilityImage.storeStorageDescriptor();
    actor_pick.pushConsts.outputBufferIndex = hardware_->actorPickBuffer.storeDescriptor();
    actor_pick.bind_storage_image(0, hardware_->visibilityImage);

    auto actor_pick_stream = hardware_->executor.stream();
    actor_pick_stream << Horizon::keep_alive(scene_batch.resource_keep_alive);
    visibility(hardware_->gbufferSize.x, hardware_->gbufferSize.y);
    actor_pick_stream.append_consuming(visibility);
    const Horizon::SubmitReceipt actor_pick_receipt =
        actor_pick_stream << actor_pick(1, 1, 1) << Horizon::commit();
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

void OpticsSystem::fail_expired_pending_screenshots() {
    std::vector<PendingScreenshot> expired;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(screenshot_mutex_);
        auto it = std::remove_if(pending_screenshots_.begin(), pending_screenshots_.end(),
                                 [now, &expired](PendingScreenshot& req) {
                                     if (req.expires_at == std::chrono::steady_clock::time_point{} ||
                                         req.expires_at > now) {
                                         return false;
                                     }
                                     expired.push_back(std::move(req));
                                     return true;
                                 });
        pending_screenshots_.erase(it, pending_screenshots_.end());
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
        if (residency_sub_id_ != 0) {
            event_bus->unsubscribe(residency_sub_id_);
        }
        if (native_frame_consumed_sub_id_ != 0) {
            event_bus->unsubscribe(native_frame_consumed_sub_id_);
        }
    }

    if (hardware_) {
        hardware_->native_frame_throttle.drain([this](const Horizon::SubmitReceipt& receipt) {
            hardware_->executor.wait_for_completion(receipt);
        });
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

    // "数据未就绪"判定：本帧仍有候选物体的网格数据没加载好。包含两种情况：
    //   1) 有候选但 0 实例（首个/全部物体数据未就绪）；
    //   2) 已有物体撑起 instance_count>0，但本次新加入的物体被 skipped_no_data 跳过。
    // 情况 2 正是 Vision 模式下"添加物体不显示"的根因：旧逻辑只要 instance_count>0
    // 就锁定签名，导致刚添加、mesh 资源仍在异步加载的物体被永久丢弃——资源稍后就绪
    // 并不改变场景签名（签名只折入句柄与设备 buffer 元素数，不查询 ResourceManager
    // 加载状态），因此再也不会触发重建。把 skipped_no_data>0 一并纳入重试条件即可修复。
    const bool data_not_ready =
        result.skipped_no_data > 0 ||
        (result.candidate_count > 0 && result.instance_count == 0);

    if (!data_not_ready) {
        // 全部候选物体都已建成（含 candidate_count==0 的真正空场景）：接受签名，
        // 停止重试，避免对空场景每帧空转重建。
        vision_applied_signature_ = sig;
        vision_rebuild_retries_ = 0;
    } else {
        // 仍有候选物体数据未就绪：不锁定签名，去抖每隔几帧继续重试，直到数据落地或达上限。
        if (++vision_rebuild_retries_ >= kVisionRebuildMaxRetries) {
            CFW_LOG_ERROR(
                "OpticsSystem: Vision rebuild left {} candidate mesh(es) unrealized "
                "({} instances built) after {} retries; accepting to avoid busy-loop",
                result.skipped_no_data, result.instance_count, vision_rebuild_retries_);
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

    std::unordered_map<std::uintptr_t, Corona::ExternalVisionBindingDevice> active_bindings;
    std::unordered_set<std::uintptr_t> active_bound_actors;
    std::unordered_set<std::uintptr_t> hidden_bound_actors;
    for (auto scene_it = hub.scene_storage().cbegin(); scene_it != hub.scene_storage().cend(); ++scene_it) {
        const auto& scene_dev = *scene_it;
        if (!scene_dev.enabled) {
            continue;
        }
        for (auto actor_handle : scene_dev.actor_handles) {
            // 跳过未加载的 actor — 无 Vision shapes 可同步
            {
                std::shared_lock lock(residency_mtx_);
                if (!resident_actors_.count(actor_handle)) continue;
            }
            const auto binding = hub.external_vision_binding(actor_handle);
            if (!binding) {
                continue;
            }
            if (normalize_scene_path_key(binding->source_path) != current_scene_key) {
                continue;
            }
            active_bound_actors.insert(actor_handle);
            active_bindings.emplace(actor_handle, *binding);
            if (!binding->visible) {
                hidden_bound_actors.insert(actor_handle);
            }
        }
    }

    std::vector<std::uintptr_t> actors_to_remove;
    for (const auto& [actor_handle, record] : scene_resource->external_live_shapes_by_actor) {
        (void)record;
        if (!active_bound_actors.contains(actor_handle)) {
            actors_to_remove.push_back(actor_handle);
        }
    }

    std::vector<std::uintptr_t> actors_to_add;
    std::vector<std::uintptr_t> actors_to_replace;
    for (const auto& [actor_handle, binding] : active_bindings) {
        const auto* record = scene_resource->find_external_live_shape(actor_handle);
        if (record == nullptr) {
            const int binding_shape_index = external_live_shape_index(binding);
            const auto group_index = static_cast<std::size_t>(binding_shape_index);
            if (binding_shape_index >= 0 &&
                group_index < groups.size() &&
                groups[group_index]) {
                scene_resource->upsert_external_live_shape({
                    .actor_handle = actor_handle,
                    .shape_index = binding_shape_index,
                    .shape_guid = binding.shape_guid,
                    .shape_identity_key = binding.shape_identity_key,
                    .dynamically_added = false,
                });
            } else {
                actors_to_add.push_back(actor_handle);
            }
            continue;
        }
        const int binding_shape_index = external_live_shape_index(binding);
        const bool shape_identity_changed =
            binding.shape_guid != record->shape_guid ||
            binding.shape_identity_key != record->shape_identity_key;
        if ((binding_shape_index >= 0 && binding_shape_index != record->shape_index) ||
            shape_identity_changed) {
            actors_to_replace.push_back(actor_handle);
        }
    }

    bool geometry_changed = false;
    bool material_registry_changed = false;
    bool needs_compile = false;
    bool removal_transform_changed = false;
    std::size_t tombstoned_instance_count = 0;
    const bool embedded_runtime = !runtime.scene_json.empty();

    auto remove_actor_shape = [&](std::uintptr_t actor_handle) {
        const auto* record = scene_resource->find_external_live_shape(actor_handle);
        if (record == nullptr || record->shape_index < 0) {
            scene_resource->erase_external_live_shape(actor_handle);
            return;
        }

        const auto record_copy = *record;
        const auto removal_action =
            Vision::external_live_shape_removal_action(record_copy, embedded_runtime);
        if (removal_action == Vision::ExternalLiveShapeRemovalAction::ForgetTracking) {
            scene_resource->erase_external_live_shape(actor_handle);
            return;
        }

        if (removal_action == Vision::ExternalLiveShapeRemovalAction::HideOriginal) {
            const auto group_index = static_cast<std::size_t>(record_copy.shape_index);
            if (group_index >= groups.size() || !groups[group_index]) {
                scene_resource->erase_external_live_shape(actor_handle);
                return;
            }

            const auto hidden_o2w = hidden_external_live_o2w();
            const auto hidden_signature =
                external_live_hidden_transform_signature(record_copy.shape_index);
            auto& group = groups[group_index];
            group->aabb = {};
            group->for_each([&](::vision::SP<::vision::ShapeInstance> instance,
                                std::uint32_t instance_index) {
                if (!instance) {
                    return;
                }
                instance->set_o2w(hidden_o2w);
                instance->init_aabb();
                group->aabb.extend(instance->aabb);
                scene_resource->upsert_logical_instance({
                    .key = {.shape_index = record_copy.shape_index,
                            .instance_index = static_cast<int>(instance_index)},
                    .actor_handle = 0,
                    .transform_signature = hidden_signature,
                    .object_to_world = flatten_vision_matrix(hidden_o2w),
                });
                removal_transform_changed = true;
                ++tombstoned_instance_count;
            });
            scene_resource->erase_external_live_shape(actor_handle);

            std::size_t mesh_count = 0;
            if (auto* geometry_data = vision_scene.geometry().data()) {
                geometry_data->for_each_mesh(
                    [&](const ::vision::Mesh*, std::uint32_t) { ++mesh_count; });
            }
            CFW_LOG_INFO(
                "OpticsSystem: embedded Vision shape removal action={} actor={} guid={} "
                "shape_index={} groups={} instances={} materials={} meshes={}",
                Vision::external_live_shape_removal_action_name(removal_action),
                actor_handle,
                record_copy.shape_guid,
                record_copy.shape_index,
                groups.size(),
                vision_scene.instances().size(),
                vision_scene.materials().all_instance_num(),
                mesh_count);
            return;
        }

        const auto shape_index = static_cast<unsigned int>(record_copy.shape_index);
        if (!Vision::remove_vision_shape_for_actor(vision_scene, shape_index)) {
            scene_resource->erase_external_live_shape(actor_handle);
            return;
        }
        auto actors_to_rewrite =
            scene_resource->remap_external_live_shape_indices_after_remove(record_copy.shape_index);
        for (auto rewrite_actor : actors_to_rewrite) {
            if (const auto* rewritten = scene_resource->find_external_live_shape(rewrite_actor)) {
                write_external_live_binding_shape_index(rewrite_actor, rewritten->shape_index);
            }
        }
        geometry_changed = true;
    };

    std::sort(actors_to_remove.begin(), actors_to_remove.end(), [&](auto lhs, auto rhs) {
        const auto* l = scene_resource->find_external_live_shape(lhs);
        const auto* r = scene_resource->find_external_live_shape(rhs);
        const int li = l == nullptr ? -1 : l->shape_index;
        const int ri = r == nullptr ? -1 : r->shape_index;
        return li > ri;
    });
    for (auto actor_handle : actors_to_remove) {
        remove_actor_shape(actor_handle);
    }

    std::sort(actors_to_replace.begin(), actors_to_replace.end(), [&](auto lhs, auto rhs) {
        const auto* l = scene_resource->find_external_live_shape(lhs);
        const auto* r = scene_resource->find_external_live_shape(rhs);
        const int li = l == nullptr ? -1 : l->shape_index;
        const int ri = r == nullptr ? -1 : r->shape_index;
        return li > ri;
    });
    for (auto actor_handle : actors_to_replace) {
        remove_actor_shape(actor_handle);
        actors_to_add.push_back(actor_handle);
    }

    for (auto actor_handle : actors_to_add) {
        const auto binding_it = active_bindings.find(actor_handle);
        if (binding_it == active_bindings.end()) {
            continue;
        }
        const auto result =
            Vision::add_vision_shape_for_actor(vision_scene, actor_handle, binding_it->second);
        if (!result.added) {
            continue;
        }
        write_external_live_binding_shape_index(actor_handle, result.shape_index);
        scene_resource->upsert_external_live_shape({
            .actor_handle = actor_handle,
            .shape_index = result.shape_index,
            .shape_guid = binding_it->second.shape_guid,
            .shape_identity_key = binding_it->second.shape_identity_key,
            .dynamically_added = true,
        });
        geometry_changed = true;
        material_registry_changed = true;
        needs_compile = needs_compile ||
            result.material_topology_after != result.material_topology_before;
    }

    if (geometry_changed) {
        try {
            pipeline->activate_view_context(0u);
            vision_scene.register_instance_meshes();
            vision_scene.tidy_up();
            if (material_registry_changed) {
                vision_scene.prepare_materials();
            }
            vision_scene.fill_instances();
            pipeline->rebuild_geometry_gpu();
            if (needs_compile) {
                pipeline->compile();
            }
            scene_resource->mark_transforms_changed();
            scene_resource->mark_scene_gpu_transforms_uploaded();
            runtime.scene_gpu_transform_version = scene_resource->logical_transform_version;
            pipeline->invalidate_all_view_contexts();
        } catch (const std::exception& e) {
            CFW_LOG_ERROR("OpticsSystem: external_live geometry sync failed: {}", e.what());
        }
    }

    bool changed = removal_transform_changed;
    std::size_t updated_actors = tombstoned_instance_count;

    for (const auto& [actor_handle, active_binding] : active_bindings) {
        auto binding = active_binding;
        const auto* existing_shape_record =
            scene_resource->find_external_live_shape(actor_handle);
        if (existing_shape_record != nullptr && existing_shape_record->shape_index >= 0) {
            binding.shape_index = existing_shape_record->shape_index;
            binding.json_path = external_live_json_path_for_shape(existing_shape_record->shape_index);
        }

        auto resolved = resolve_external_live_transform(actor_handle, binding);
        if (!resolved) {
            continue;
        }
        const auto normal_signature = resolved->signature;
        const bool actor_hidden = hidden_bound_actors.contains(actor_handle);
        const bool original_external_shape =
            existing_shape_record != nullptr && !existing_shape_record->dynamically_added;

        const auto group_index = static_cast<std::size_t>(resolved->shape_index);
        if (group_index >= groups.size() || !groups[group_index]) {
            continue;
        }

        auto& group = groups[group_index];
        const bool first_time_actor_sync =
            scene_resource->external_live_transform_signatures.find(actor_handle) ==
            scene_resource->external_live_transform_signatures.end();

        if (original_external_shape) {
            group->for_each([&](::vision::SP<::vision::ShapeInstance> instance,
                                std::uint32_t instance_index) {
                if (!instance) {
                    return;
                }
                scene_resource->cache_external_live_original_instance({
                    .key = {.shape_index = resolved->shape_index,
                            .instance_index = static_cast<int>(instance_index)},
                    .actor_handle = actor_handle,
                    .transform_signature = normal_signature,
                    .object_to_world = flatten_vision_matrix(instance->o2w()),
                });
            });
            scene_resource->external_live_original_transform_signatures.try_emplace(
                actor_handle,
                normal_signature);
        }

        const auto original_signature =
            scene_resource->external_live_original_transform_signatures.find(actor_handle);
        const bool actor_transform_changed_from_original =
            original_external_shape &&
            original_signature != scene_resource->external_live_original_transform_signatures.end() &&
            original_signature->second != normal_signature;

        scene_resource->upsert_external_live_shape({
            .actor_handle = actor_handle,
            .shape_index = resolved->shape_index,
            .shape_guid = binding.shape_guid,
            .shape_identity_key = binding.shape_identity_key,
            .dynamically_added = existing_shape_record != nullptr
                                   ? existing_shape_record->dynamically_added
                                   : false,
        });

        if (original_external_shape && !actor_hidden && first_time_actor_sync) {
            scene_resource->external_live_transform_signatures[actor_handle] =
                normal_signature;
            continue;
        }

        std::size_t target_signature = normal_signature;
        auto target_o2w = resolved->o2w;
        if (actor_hidden) {
            target_signature = external_live_hidden_transform_signature(resolved->shape_index);
            target_o2w = hidden_external_live_o2w();
        }
        const bool restore_original =
            original_external_shape && !actor_hidden && !actor_transform_changed_from_original;

        const auto cached =
            scene_resource->external_live_transform_signatures.find(actor_handle);
        const bool actor_signature_changed =
            cached == scene_resource->external_live_transform_signatures.end() ||
            cached->second != target_signature;

        group->aabb = ::vision::Box3f{};
        bool logical_instance_changed = false;
        if (restore_original) {
            const auto original_instances =
                scene_resource->restore_external_live_original_instances(resolved->shape_index);
            group->for_each([&](::vision::SP<::vision::ShapeInstance> instance,
                                std::uint32_t instance_index) {
                if (!instance) {
                    return;
                }
                const auto original =
                    std::find_if(original_instances.begin(),
                                 original_instances.end(),
                                 [&](const auto& record) {
                                     return record.key.instance_index ==
                                            static_cast<int>(instance_index);
                                 });
                if (original == original_instances.end()) {
                    return;
                }
                const auto original_o2w = unflatten_vision_matrix(original->object_to_world);
                logical_instance_changed |= scene_resource->upsert_logical_instance({
                    .key = original->key,
                    .actor_handle = actor_handle,
                    .transform_signature = target_signature,
                    .object_to_world = original->object_to_world,
                });
                instance->set_o2w(original_o2w);
                instance->init_aabb();
                group->aabb.extend(instance->aabb);
            });
        } else {
            const auto object_to_world = flatten_vision_matrix(target_o2w);
            group->for_each([&](::vision::SP<::vision::ShapeInstance> instance,
                                std::uint32_t instance_index) {
                if (!instance) {
                    return;
                }
                logical_instance_changed |= scene_resource->upsert_logical_instance({
                    .key = {.shape_index = resolved->shape_index,
                            .instance_index = static_cast<int>(instance_index)},
                    .actor_handle = actor_handle,
                    .transform_signature = target_signature,
                    .object_to_world = object_to_world,
                });
                instance->set_o2w(target_o2w);
                instance->init_aabb();
                group->aabb.extend(instance->aabb);
            });
        }

        scene_resource->external_live_transform_signatures[actor_handle] =
            target_signature;
        if ((actor_hidden || restore_original || !first_time_actor_sync) &&
            (actor_signature_changed || logical_instance_changed)) {
            changed = true;
            ++updated_actors;
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

void OpticsSystem::sync_engine_native_mixed_shapes(VisionPipelineRuntime& runtime) {
    auto& pipeline = runtime.pipeline;
    auto scene_resource = runtime.scene_resource;
    if (!vision_initialized_ || !pipeline || !scene_resource ||
        runtime.source != VisionPipelineSource::ExternalLive ||
        runtime.scene_path.empty()) {
        return;
    }

    auto& hub = SharedDataHub::instance();
    auto& vision_scene = pipeline->scene();
    auto& groups = vision_scene.groups();

    // 1. Enumerate engine-native candidate actors: present in an enabled scene and
    //    WITHOUT an external_vision_binding (bound proxies are handled by
    //    sync_external_live_vision_transforms, which must have run first).
    //    只收集 Loaded 状态的 actor — Unloaded 的 GPU 资源已释放，无 mesh 可构建。
    std::unordered_set<std::uintptr_t> active_native_actors;
    for (auto scene_it = hub.scene_storage().cbegin(); scene_it != hub.scene_storage().cend(); ++scene_it) {
        const auto& scene_dev = *scene_it;
        if (!scene_dev.enabled) {
            continue;
        }
        for (auto actor_handle : scene_dev.actor_handles) {
            if (actor_handle == 0 || hub.has_external_vision_binding(actor_handle)) {
                continue;
            }
            {
                std::shared_lock lock(residency_mtx_);
                if (!resident_actors_.count(actor_handle)) continue;
            }
            active_native_actors.insert(actor_handle);
        }
    }

    // 2. Diff against the tracked set: remove actors that left the scene or are no
    //    longer renderable (invisible / geometry cleared); add newly-renderable ones.
    std::vector<std::uintptr_t> actors_to_remove;
    std::vector<std::uintptr_t> actors_to_add;
    for (const auto& [actor_handle, record] : scene_resource->engine_mixed_shapes_by_actor) {
        (void)record;
        if (!active_native_actors.contains(actor_handle)) {
            actors_to_remove.push_back(actor_handle);
        }
    }
    for (auto actor_handle : active_native_actors) {
        const auto* record = scene_resource->find_engine_mixed_shape(actor_handle);
        const auto resolved = resolve_engine_native_transform(
            actor_handle, record != nullptr ? record->shape_index : -1);
        if (record == nullptr) {
            // Only attempt to add a renderable (visible + transform) actor. Mesh
            // readiness is checked inside add_vision_shape_for_actor.
            if (resolved) {
                actors_to_add.push_back(actor_handle);
            }
        } else if (!resolved) {
            // Tracked but no longer renderable -> remove from the mix.
            actors_to_remove.push_back(actor_handle);
        }
    }

    bool geometry_changed = false;
    bool material_registry_changed = false;
    bool needs_compile = false;

    auto remove_native = [&](std::uintptr_t actor_handle) {
        const auto* record = scene_resource->find_engine_mixed_shape(actor_handle);
        if (record == nullptr || record->shape_index < 0) {
            scene_resource->erase_engine_mixed_shape(actor_handle);
            return;
        }
        const int removed_index = record->shape_index;
        if (!Vision::remove_vision_shape_for_actor(
                vision_scene, static_cast<unsigned int>(removed_index))) {
            scene_resource->erase_engine_mixed_shape(actor_handle);
            return;
        }
        // Erase BEFORE remap so this record is not double-decremented.
        scene_resource->erase_engine_mixed_shape(actor_handle);
        auto actors_to_rewrite =
            scene_resource->remap_external_live_shape_indices_after_remove(removed_index);
        for (auto rewrite_actor : actors_to_rewrite) {
            if (const auto* rewritten = scene_resource->find_external_live_shape(rewrite_actor)) {
                write_external_live_binding_shape_index(rewrite_actor, rewritten->shape_index);
            }
        }
        geometry_changed = true;
    };

    // Remove in descending shape_index order so each erase only shifts strictly
    // higher, not-yet-processed indices (mirrors the bound-proxy removal pass).
    std::sort(actors_to_remove.begin(), actors_to_remove.end(), [&](auto lhs, auto rhs) {
        const auto* l = scene_resource->find_engine_mixed_shape(lhs);
        const auto* r = scene_resource->find_engine_mixed_shape(rhs);
        const int li = l == nullptr ? -1 : l->shape_index;
        const int ri = r == nullptr ? -1 : r->shape_index;
        return li > ri;
    });
    for (auto actor_handle : actors_to_remove) {
        remove_native(actor_handle);
    }

    // Append new engine-native shapes (always land at the high end of groups_).
    std::unordered_set<std::uintptr_t> just_added_actors;
    for (auto actor_handle : actors_to_add) {
        const auto result = Vision::add_vision_shape_for_actor(
            vision_scene, actor_handle, Corona::ExternalVisionBindingDevice{});
        if (!result.added) {
            continue;  // mesh data not ready yet -> retry next frame
        }
        scene_resource->engine_mixed_shapes_by_actor[actor_handle] =
            Vision::EngineMixedShapeRecord{
                .actor_handle = actor_handle,
                .shape_index = result.shape_index,
                .transform_signature = 0,
            };
        just_added_actors.insert(actor_handle);
        geometry_changed = true;
        material_registry_changed = true;
        needs_compile = needs_compile ||
            result.material_topology_after != result.material_topology_before;
    }

    if (geometry_changed) {
        try {
            pipeline->activate_view_context(0u);
            if (needs_compile) {
                // A new material TYPE was introduced (e.g. the first engine-native
                // PrincipledBSDF in a cbox scene that had none). Recompiling the
                // integrator requires the renderer's light sampler + scene to be
                // fully re-prepared first: PathTracingIntegrator::compile walks the
                // light sampler during codegen, and a stale sampler (after tidy_up
                // re-indexed lights) faults. So mirror rebuild_vision_scene's proven
                // post-build sequence exactly (scene.prepare → prepare_geometry →
                // prepare_lights → bindless → compile → per-view-context recompile)
                // instead of the lighter incremental path below.
                vision_scene.prepare();
                pipeline->prepare_geometry();
                pipeline->renderer().prepare_lights(vision_scene);
                pipeline->upload_scene_bindless_array();
                pipeline->upload_bindless_array();
                pipeline->compile();
                pipeline->rebuild_view_context_renderers();
            } else {
                // Same material topology (e.g. another PrincipledBSDF object):
                // cheap incremental geometry update, no integrator recompile. New
                // material texture slots still need their bindless handles published
                // (update_slotSOA + upload_handles) or the kernel reads unpublished
                // slots for the new material instance.
                vision_scene.register_instance_meshes();
                vision_scene.tidy_up();
                if (material_registry_changed) {
                    vision_scene.prepare_materials();
                }
                vision_scene.fill_instances();
                pipeline->rebuild_geometry_gpu();
                pipeline->upload_scene_bindless_array();
                pipeline->upload_bindless_array();
            }
            scene_resource->mark_transforms_changed();
            scene_resource->mark_scene_gpu_transforms_uploaded();
            runtime.scene_gpu_transform_version = scene_resource->logical_transform_version;
            pipeline->invalidate_all_view_contexts();
        } catch (const std::exception& e) {
            CFW_LOG_ERROR("OpticsSystem: engine-native mixed geometry sync failed: {}", e.what());
        }
    }

    // Transform sync for tracked engine-native shapes (driven by the engine
    // geometry's ModelTransform; no binding). Newly-added shapes have
    // transform_signature==0 so they also pass through here once to register their
    // logical instances.
    bool changed = false;
    std::size_t updated_actors = 0;
    for (auto& [actor_handle, record] : scene_resource->engine_mixed_shapes_by_actor) {
        const auto resolved = resolve_engine_native_transform(actor_handle, record.shape_index);
        if (!resolved) {
            continue;
        }
        const auto group_index = static_cast<std::size_t>(resolved->shape_index);
        if (group_index >= groups.size() || !groups[group_index]) {
            continue;
        }
        if (record.transform_signature == resolved->signature) {
            continue;
        }

        auto& group = groups[group_index];
        group->aabb = ::vision::Box3f{};
        const auto object_to_world = flatten_vision_matrix(resolved->o2w);
        group->for_each([&](::vision::SP<::vision::ShapeInstance> instance,
                            std::uint32_t instance_index) {
            if (!instance) {
                return;
            }
            scene_resource->upsert_logical_instance({
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
        record.transform_signature = resolved->signature;
        // Mirror sync_external_live_vision_transforms' first_time_actor_sync guard:
        // an actor ADDED this frame was already fully built+uploaded by the geometry
        // block above (prepare_geometry/rebuild_geometry_gpu), so it must NOT also
        // trigger the transform-block update_geometry() — issuing an update_accel
        // (TLAS refit) right after a full build_accel + compile corrupts the
        // accel/SBT and faults the render kernel. We still apply o2w + register the
        // logical instance above (needed for shared-resource transform tracking);
        // we just skip flagging a GPU transform flush this frame. Subsequent real
        // moves (not in just_added_actors) flush normally.
        if (just_added_actors.contains(actor_handle)) {
            continue;
        }
        changed = true;
        ++updated_actors;
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
        CFW_LOG_DEBUG("OpticsSystem: engine-native mixed updated {} actor transform(s)",
                      updated_actors);
    } catch (const std::exception& e) {
        CFW_LOG_ERROR("OpticsSystem: engine-native mixed transform sync failed: {}", e.what());
    }
}

bool OpticsSystem::init_vision_lazy() {
    if (vision_initialized_) return true;
    try {
        auto& rhi_context = ocarina::RHIContext::instance();
        const auto runtime_directory = current_executable_directory();
        ocarina::DynamicModule::clear_search_path();
        ocarina::DynamicModule::add_search_path(runtime_directory);

        const auto* cuda_backend = rhi_context.obtain_module("ocarina-backend-cuda.dll");
        if (cuda_backend == nullptr || cuda_backend->handle() == nullptr) {
            CFW_LOG_ERROR(
                "OpticsSystem: CUDA Vision backend could not be loaded from {}",
                runtime_directory.string());
            return false;
        }
        if (cuda_backend->function_ptr("create_device") == nullptr ||
            cuda_backend->function_ptr("destroy") == nullptr) {
            CFW_LOG_ERROR("OpticsSystem: CUDA Vision backend is missing device entry points");
            return false;
        }

        // ocarina::Device is non-default-constructible; use auto so the type is
        // deduced from create_device(). Function-local static ensures single init.
        static auto s_device = rhi_context.create_device("cuda");
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
        std::optional<VisionSceneLoadRequest> pending_external_scene;
        {
            std::lock_guard<std::mutex> lock(vision_scene_load_mutex_);
            if (pending_vision_scene_load_ &&
                (!pending_vision_scene_load_->scene_path.empty() ||
                 !pending_vision_scene_load_->scene_json.empty())) {
                pending_external_scene.swap(pending_vision_scene_load_);
            }
        }
        if (pending_external_scene) {
            const auto mode_selection = select_visible_vision_render_mode();
            const auto requested_mode = mode_selection.has_visible_camera
                                            ? mode_selection.mode
                                            : current_vision_render_mode_;
            const bool loaded = !pending_external_scene->scene_json.empty()
                                    ? load_external_vision_scene_from_json(
                                          *pending_external_scene, requested_mode, true)
                                    : load_external_vision_scene(pending_external_scene->scene_path,
                                                                 requested_mode,
                                                                 std::nullopt,
                                                                 true);
            if (!loaded) {
                CFW_LOG_ERROR("OpticsSystem: failed to initialize Vision from external scene: {}",
                              pending_external_scene->scene_json.empty()
                                  ? pending_external_scene->scene_path
                                  : pending_external_scene->scene_key);
                return false;
            }
            vision_initialized_ = true;
            const bool external_live =
                pending_external_scene->scene_json.empty() &&
                has_external_live_bindings_for_scene(pending_external_scene->scene_path);
            vision_applied_signature_ = 0;
            vision_pending_signature_ = 0;
            vision_stable_frames_ = 0;
            vision_rebuild_retries_ = 0;
            CFW_LOG_INFO("OpticsSystem: initialized Vision from {} scene: {}",
                         !pending_external_scene->scene_json.empty()
                             ? "embedded"
                             : (external_live ? "external_live" : "external"),
                         pending_external_scene->scene_json.empty()
                             ? pending_external_scene->scene_path
                             : pending_external_scene->scene_key);
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
                runtime.wait_for_interop_submission(camera_handle, "inactive bridge release");
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
                        runtime.wait_for_interop_submission(cam_handle, "view context recreation");
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
                auto* fb = pipeline->frame_buffer();
                if (fb == nullptr) {
                    SharedDataHub::instance().update_ssat_view_viewer_status(
                        cam_handle, false, camera.vision_render_mode == CameraVisionRenderMode::SSAT,
                        0u, 0u);
                    return;
                }
                apply_ssat_view_viewer_state(*pipeline, cam_handle, camera);
                pipeline->display(1.0 / 60.0);

                const auto res = fb->resolution();
                const uint32_t w = res.x;
                const uint32_t h = res.y;

                const uint64_t pixel_count = static_cast<uint64_t>(w) * static_cast<uint64_t>(h);
                std::optional<uint32_t> vision_src_descriptor;
                bool used_zero_copy = false;
                if (!vision_zero_copy_forced_disabled() &&
                    !runtime.zero_copy_disabled.contains(cam_handle)) {
                    // CUDA is about to write the shared allocation. The preceding Vulkan
                    // resolve must finish first until external timeline semaphores replace
                    // this conservative CPU-side synchronization.
                    runtime.wait_for_interop_submission(cam_handle,
                                                        "zero-copy buffer reuse",
                                                        false);
                    auto& bridge = runtime.bridges[cam_handle];
                    if (!bridge) {
                        bridge = std::make_unique<Vision::VisionZeroCopyBridge>();
                    }
                    if (bridge->ensure(*pipeline, w, h) &&
                        bridge->copy_from_framebuffer(*pipeline)) {
                        vision_src_descriptor = bridge->imported().storeDescriptor();
                        used_zero_copy = true;
                    } else {
                        runtime.wait_for_interop_submission(cam_handle, "failed bridge release");
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

                    const auto [dispatchX, dispatchY] = visionResolve.dispatch_groups(w, h);
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
                if (used_zero_copy) {
                    runtime.interop_submissions.insert_or_assign(cam_handle,
                                                                 vision_submit_receipt);
                }

                process_pending_screenshots(cam_handle, *presented);

                if (auto image_device =
                        SharedDataHub::instance().image_storage().acquire_write(
                            target.image_handle)) {
                    if (vision_submit_receipt.empty()) {
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
                // Mix engine-native (unbound) actors into the same external scene.
                // Must run AFTER the bound-proxy sync so bound shapes are at their
                // final indices and engine shapes append after them.
                sync_engine_native_mixed_shapes(*runtime);
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
        sync_engine_native_mixed_shapes(runtime);
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
                SharedDataHub::instance().clear_ssat_view_viewer_state(cam_handle);
                continue;
            }
            if (camera->surface == nullptr) {
                SharedDataHub::instance().clear_ssat_view_viewer_state(cam_handle);
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
    std::optional<VisionSceneLoadRequest> request;
    {
        std::lock_guard<std::mutex> lock(vision_scene_load_mutex_);
        if (!pending_vision_scene_load_) return;
        request.swap(pending_vision_scene_load_);
    }

    const auto mode_selection = select_visible_vision_render_mode();
    const auto requested_mode = mode_selection.has_visible_camera
                                    ? mode_selection.mode
                                    : current_vision_render_mode_;
    if (!request->scene_json.empty()) {
        if (load_external_vision_scene_from_json(*request, requested_mode, true)) {
            vision_applied_signature_ = 0;
            vision_pending_signature_ = 0;
            vision_stable_frames_ = 0;
            vision_rebuild_retries_ = 0;
            CFW_LOG_INFO("OpticsSystem: embedded Vision scene loaded: {}",
                         request->scene_key);
        }
        return;
    }

    const std::string& path = request->scene_path;
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
    if (!runtime.scene_json.empty()) {
        VisionSceneLoadRequest request;
        request.scene_json = runtime.scene_json;
        request.base_dir = runtime.base_dir;
        request.scene_key = runtime.scene_path;
        request.external_live = runtime.source == VisionPipelineSource::ExternalLive;
        if (!load_external_vision_scene_from_json(request, mode)) {
            CFW_LOG_WARNING(
                "OpticsSystem: failed to switch embedded Vision scene '{}' to mode '{}'; "
                "continuing with previous pipeline mode '{}'",
                source_path,
                std::string(Vision::vision_render_mode_name(mode)),
                std::string(Vision::vision_render_mode_name(current_vision_render_mode_)));
            return;
        }
        return;
    }

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

bool OpticsSystem::load_external_vision_scene_from_json(const VisionSceneLoadRequest& request,
                                                        CameraVisionRenderMode mode,
                                                        bool force_reload_scene_resource) {
    if (request.scene_json.empty()) {
        return false;
    }

    const auto source = request.external_live
        ? VisionPipelineSource::ExternalLive
        : VisionPipelineSource::ExternalFile;
    auto scene_key = request.scene_key;
    if (scene_key.empty()) {
        scene_key = std::string("embedded_vision_") +
                    std::to_string(std::hash<std::string>{}(request.scene_json));
    }
    const auto key = make_vision_pipeline_key(scene_key, mode, source);

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
                        "OpticsSystem: releasing embedded Vision runtime before shared scene reload ({})",
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
            CFW_LOG_INFO("OpticsSystem: reloading embedded Vision scene resource ({})",
                         describe_vision_scene_resource_key(scene_resource->key));
            scene_resource->reset_loaded_scene();
        }

        if (runtime.pipeline && !force_reload_scene_resource) {
            runtime.pipeline->set_output_denoise(
                Vision::vision_render_mode_uses_denoise(key.mode));
            active_vision_runtime_key_ = key;
            current_vision_render_mode_ = mode;
            return true;
        }

        const auto base_dir = request.base_dir.empty()
                                  ? std::filesystem::current_path()
                                  : std::filesystem::u8path(request.base_dir);
        auto pipeline = import_vision_scene_from_data(
            vision::DataWrap::parse(request.scene_json),
            base_dir,
            key.scene_path,
            key.mode,
            scene_resource,
            key.source);
        if (!pipeline) {
            CFW_LOG_ERROR("OpticsSystem: Embedded Vision scene import failed: {}",
                          key.scene_path);
            release_unused_vision_scene_resources();
            return false;
        }

        log_vision_pipeline_diagnostics(
            *pipeline,
            std::string("embedded import mode=") +
                std::string(Vision::vision_render_mode_name(key.mode)));
        runtime.reset_pipeline(std::move(pipeline), key.source, key.scene_path, key.mode);
        runtime.scene_json = request.scene_json;
        runtime.base_dir = request.base_dir;
        active_vision_runtime_key_ = key;
        current_vision_render_mode_ = mode;
        CFW_LOG_INFO("OpticsSystem: loaded embedded Vision runtime ({})",
                     describe_vision_pipeline_key(key));
        return true;
    } catch (const std::exception& e) {
        CFW_LOG_ERROR("OpticsSystem: Embedded Vision scene import threw: {}", e.what());
        return false;
    }
}
#endif  // CORONA_ENABLE_VISION

}  // namespace Corona::Systems
