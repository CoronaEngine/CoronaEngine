#include "cef_bridge_helpers.h"
#include "cef_editor_api.h"

#include <corona/kernel/core/i_logger.h>
#include <corona/resource/resource_manager.h>
#include <corona/resource/types/image.h>
#include <corona/resource/types/scene.h>
#include <corona/shared_data_hub.h>
#include <corona/engine/engine_runtime_api.h>
#include <corona/systems/ui/camera_viewport_manager.h>
#include <include/cef_values.h>
#include <nlohmann/json.hpp>
#include <SDL3/SDL.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "browser_manager.h"
#include "cef_client.h"
#include "request_response_broker.h"

namespace Corona::Systems::UI {

namespace {

static std::mutex s_input_mutex;
static std::vector<InputEvent> s_input_queue;

#ifdef _WIN32
struct WindowedPlacement {
    RECT rect{};
    LONG_PTR style{0};
};

static std::unordered_map<HWND, WindowedPlacement> s_camera_windowed_placements;
#endif

struct CameraWindowModeState {
    int mode{0};
    int x{120};
    int y{120};
    int width{960};
    int height{540};
    bool saved{false};
    bool saved_maximized{false};
};

static std::unordered_map<int, CameraWindowModeState> s_camera_window_modes;

struct ViewportGizmoDragRuntime {
    std::uintptr_t actor_handle{};
    Corona::ViewportGizmoAxis axis{Corona::ViewportGizmoAxis::None};
    ktm::fvec3 initial_position{};
    ktm::fvec3 last_position{};
    ktm::fvec2 initial_pointer{};
    ktm::fvec2 fallback_screen_direction{};
    std::optional<float> initial_axis_parameter;
    bool fallback_logged{false};
    bool write_failure_logged{false};
    std::string scene_id;
    std::string actor_name;
};

static std::mutex s_viewport_gizmo_mutex;
static std::unordered_map<std::uintptr_t, ViewportGizmoDragRuntime>
    s_viewport_gizmo_drags;

struct ViewportGizmoCpuMasks {
    std::array<std::vector<std::uint8_t>, 3> alpha;
    std::array<Corona::Systems::OpticsDetail::ViewportGizmoSpriteMask, 3> views;
    bool valid{false};
};

const ViewportGizmoCpuMasks& viewport_gizmo_cpu_masks() {
    static ViewportGizmoCpuMasks masks;
    static std::once_flag load_once;
    std::call_once(load_once, [] {
        constexpr std::array<const char*, 3> paths{{
            "assets/icon/axis_x.png",
            "assets/icon/axis_y.png",
            "assets/icon/axis_z.png",
        }};
        std::error_code ec;
        auto root = std::filesystem::current_path(ec);
        for (std::size_t i = 0; i < paths.size(); ++i) {
            auto path = std::filesystem::path(paths[i]);
            if (!ec) {
                for (auto dir = root; !dir.empty(); dir = dir.parent_path()) {
                    const auto candidate = dir / paths[i];
                    if (std::filesystem::exists(candidate, ec) && !ec) {
                        path = candidate;
                        break;
                    }
                    ec.clear();
                    if (dir == dir.parent_path()) break;
                }
            }
            const auto image_id =
                Corona::Resource::ResourceManager::get_instance().import_sync(path);
            if (image_id == Corona::Resource::IResource::INVALID_UID) {
                CFW_LOG_WARNING("Viewport gizmo alpha mask load failed: {}", path.string());
                return;
            }
            auto image =
                Corona::Resource::ResourceManager::get_instance()
                    .acquire_read<Corona::Resource::Image>(image_id);
            if (!image || image->get_width() <= 0 || image->get_height() <= 0 ||
                image->get_data() == nullptr) {
                CFW_LOG_WARNING("Viewport gizmo alpha mask is invalid: {}", path.string());
                return;
            }
            const auto width = static_cast<std::uint32_t>(image->get_width());
            const auto height = static_cast<std::uint32_t>(image->get_height());
            const int channels = image->get_channels();
            if (channels != 4) {
                CFW_LOG_WARNING("Viewport gizmo image has no alpha channel: {}", path.string());
                return;
            }
            const auto pixel_count =
                static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
            masks.alpha[i].resize(pixel_count);
            const auto* pixels = image->get_data();
            for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
                masks.alpha[i][pixel] = pixels[pixel * 4 + 3];
            }
            const auto& metadata =
                Corona::Systems::OpticsDetail::kViewportGizmoSpriteMetadata[i];
            masks.views[i] = {
                .alpha = masks.alpha[i],
                .width = width,
                .height = height,
                .anchor = metadata.anchor,
                .tip = metadata.tip,
            };
        }
        masks.valid = true;
    });
    return masks;
}

bool set_browser_tab_system_cursor_state(CefRefPtr<CefBrowser> browser, bool hidden, bool custom) {
    if (!browser) {
        return false;
    }

    const int browser_id = browser->GetIdentifier();
    for (const auto& [tab_id, tab] : BrowserManager::instance().get_tabs()) {
        if (!tab || !tab->client) {
            continue;
        }
        auto tab_browser = tab->client->GetBrowser();
        if (tab_browser && tab_browser->GetIdentifier() == browser_id) {
            tab->hide_system_cursor.store(hidden, std::memory_order_relaxed);
            tab->use_custom_system_cursor.store(custom, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

void request_camera_window_rect(int tab_id, BrowserTab* tab, int x, int y, int width, int height) {
    tab->initial_x = x;
    tab->initial_y = y;
    tab->dock_width = std::max(width, 64);
    tab->dock_height = std::max(height, 64);
    tab->needs_reposition = true;
    tab->needs_resize = true;
    BrowserManager::instance().resize_tab(tab_id, tab->dock_width, tab->dock_height);
}

#ifdef _WIN32
void save_windowed_placement(HWND hwnd) {
    if (!hwnd || IsZoomed(hwnd)) {
        return;
    }

    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) {
        return;
    }

    const LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    if ((style & WS_OVERLAPPEDWINDOW) == 0) {
        return;
    }

    s_camera_windowed_placements[hwnd] = {
        .rect = rect,
        .style = style,
    };
}

bool restore_windowed_placement(HWND hwnd) {
    auto it = s_camera_windowed_placements.find(hwnd);
    if (it == s_camera_windowed_placements.end()) {
        return false;
    }

    const auto placement = it->second;
    s_camera_windowed_placements.erase(it);
    SetWindowLongPtr(hwnd, GWL_STYLE, placement.style);
    ShowWindow(hwnd, SW_RESTORE);
    SetWindowPos(hwnd, nullptr,
                 placement.rect.left,
                 placement.rect.top,
                 placement.rect.right - placement.rect.left,
                 placement.rect.bottom - placement.rect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    return true;
}
#endif

[[nodiscard]] std::uintptr_t select_scene_camera_handle(const Corona::SceneDevice& scene) {
    if (scene.active_camera_handle != 0 &&
        std::find(scene.camera_handles.begin(),
                  scene.camera_handles.end(),
                  scene.active_camera_handle) != scene.camera_handles.end()) {
        return scene.active_camera_handle;
    }
    return scene.camera_handles.empty() ? 0 : scene.camera_handles.front();
}

[[nodiscard]] ktm::fvec3 make_fvec3(float x, float y, float z) {
    ktm::fvec3 value;
    value[0] = x;
    value[1] = y;
    value[2] = z;
    return value;
}

[[nodiscard]] ktm::fvec4 make_fvec4(float x, float y, float z, float w) {
    ktm::fvec4 value;
    value[0] = x;
    value[1] = y;
    value[2] = z;
    value[3] = w;
    return value;
}

[[nodiscard]] ktm::fvec3 transform_local_point_to_world(const ktm::fmat4x4& matrix,
                                                        const ktm::fvec3& local_point) {
    const ktm::fvec4 local_h = make_fvec4(local_point[0], local_point[1], local_point[2], 1.0f);
    const ktm::fvec4 world_h = matrix * local_h;
    return make_fvec3(world_h[0], world_h[1], world_h[2]);
}

}  // namespace (input queue)

// ── drain_input_events: 消费所有积攒的输入事件 ──
// 开放给 ScriptSystem 调用（通过头文件声明），每帧由 Python show_log_on_js 消费
void enqueue_input_event(InputEvent event) {
    std::lock_guard<std::mutex> lock(s_input_mutex);
    s_input_queue.push_back(std::move(event));
}

std::vector<InputEvent> drain_input_events() {
    std::lock_guard<std::mutex> lock(s_input_mutex);
    std::vector<InputEvent> events;
    events.swap(s_input_queue);
    return events;
}

namespace {

bool parse_vec3_list(const CefRefPtr<CefListValue>& list, ktm::fvec3& out) {
    if (!list || list->GetSize() != 3) {
        return false;
    }

    auto read_value = [list](size_t index, float& value) -> bool {
        const auto type = list->GetType(index);
        if (type == VTYPE_INT) {
            value = static_cast<float>(list->GetInt(index));
            return true;
        }
        if (type == VTYPE_DOUBLE) {
            value = static_cast<float>(list->GetDouble(index));
            return true;
        }
        return false;
    };

    return read_value(0, out[0]) && read_value(1, out[1]) && read_value(2, out[2]);
}

bool handle_camera_move_fast(const CefRefPtr<CefProcessMessage>& message) {
    // CameraMoveFast is the editor camera-control channel. Blockly/node-graph
    // input must still reach Scratch, but while the runtime owns input we must
    // not forward any pose updates produced by the editor camera controllers.
    if (!Corona::API::is_editor_camera_input_enabled()) {
        return true;
    }

    auto args = message->GetArgumentList();
    if (!args || args->GetSize() < 5) {
        return true;
    }

    const auto handle_type = args->GetType(0);
    if (handle_type != VTYPE_INT && handle_type != VTYPE_DOUBLE) {
        return true;
    }

    const auto handle_value =
        handle_type == VTYPE_INT ? static_cast<double>(args->GetInt(0)) : args->GetDouble(0);
    const auto camera_handle = static_cast<std::uintptr_t>(handle_value);
    if (camera_handle == 0) {
        return true;
    }

    ktm::fvec3 position{};
    ktm::fvec3 forward{};
    ktm::fvec3 world_up{};
    if (!parse_vec3_list(args->GetList(1), position) ||
        !parse_vec3_list(args->GetList(2), forward) ||
        !parse_vec3_list(args->GetList(3), world_up)) {
        return true;
    }

    float fov = 45.0f;
    const auto fov_type = args->GetType(4);
    if (fov_type == VTYPE_INT) {
        fov = static_cast<float>(args->GetInt(4));
    } else if (fov_type == VTYPE_DOUBLE) {
        fov = static_cast<float>(args->GetDouble(4));
    }

    Corona::CameraMoveCommand move;
    move.camera_handle = camera_handle;
    move.position = position;
    move.forward = forward;
    move.world_up = world_up;
    move.fov = fov;
    Corona::SharedDataHub::instance().enqueue_camera_move(move);

    return true;
}

bool handle_camera_viewport_fast(const CefRefPtr<CefProcessMessage>& message) {
    auto args = message->GetArgumentList();
    if (!args || args->GetSize() < 7) {
        return true;
    }

    auto read_number = [args](int index, double& value) -> bool {
        const auto type = args->GetType(index);
        if (type == VTYPE_INT) {
            value = static_cast<double>(args->GetInt(index));
            return true;
        }
        if (type == VTYPE_DOUBLE) {
            value = args->GetDouble(index);
            return true;
        }
        return false;
    };

    double handle_value = 0.0;
    if (!read_number(0, handle_value)) {
        return true;
    }

    const auto camera_handle = static_cast<std::uintptr_t>(handle_value);
    if (camera_handle == 0) {
        return true;
    }

    std::array<double, 6> values{};
    for (int i = 0; i < 6; ++i) {
        if (!read_number(i + 1, values[static_cast<size_t>(i)])) {
            return true;
        }
    }

    void* surface = nullptr;
    if (auto camera = Corona::SharedDataHub::instance().camera_storage().try_acquire_read(
            camera_handle)) {
        surface = camera->surface;
    }
    if (surface == nullptr) {
        surface = Corona::API::get_default_surface();
    }

    Corona::SharedDataHub::instance().enqueue_camera_viewport_update({
        .camera_handle = camera_handle,
        .surface = surface,
        .view_open = false,
        .x = std::max(static_cast<int>(std::lround(values[0])), 0),
        .y = std::max(static_cast<int>(std::lround(values[1])), 0),
        .width = std::max(static_cast<int>(std::lround(values[2])), 1),
        .height = std::max(static_cast<int>(std::lround(values[3])), 1),
        .render_width = std::max(static_cast<int>(std::lround(values[4])), 1),
        .render_height = std::max(static_cast<int>(std::lround(values[5])), 1),
    });
    return true;
}

std::vector<std::uintptr_t> resolve_profile_handles(std::uintptr_t actor_handle) {
    std::vector<std::uintptr_t> profile_handles;
    if (auto actor = Corona::SharedDataHub::instance().actor_storage().try_acquire_read(actor_handle)) {
        profile_handles = actor->profile_handles;
    }
    return profile_handles;
}

struct FocusBounds {
    ktm::fvec3 min{};
    ktm::fvec3 max{};
    bool valid{false};
};

struct FocusGeometrySource {
    std::uintptr_t geometry_handle{};
    bool has_local_bounds{false};
    ktm::fvec3 local_min{};
    ktm::fvec3 local_max{};
};

void append_focus_geometry_source(std::vector<FocusGeometrySource>& sources,
                                  std::uintptr_t geometry_handle,
                                  bool has_local_bounds,
                                  const ktm::fvec3& local_min,
                                  const ktm::fvec3& local_max) {
    if (geometry_handle == 0) {
        return;
    }

    for (auto& source : sources) {
        if (source.geometry_handle != geometry_handle) {
            continue;
        }
        if (!source.has_local_bounds && has_local_bounds) {
            source.has_local_bounds = true;
            source.local_min = local_min;
            source.local_max = local_max;
        }
        return;
    }

    FocusGeometrySource source;
    source.geometry_handle = geometry_handle;
    source.has_local_bounds = has_local_bounds;
    source.local_min = local_min;
    source.local_max = local_max;
    sources.push_back(source);
}

std::vector<FocusGeometrySource> resolve_actor_focus_geometry_sources(std::uintptr_t actor_handle) {
    std::vector<std::uintptr_t> profile_handles;
    if (auto actor = Corona::SharedDataHub::instance().actor_storage().try_acquire_read_nowait(actor_handle)) {
        profile_handles = actor->profile_handles;
    }

    std::vector<FocusGeometrySource> sources;
    auto& hub = Corona::SharedDataHub::instance();
    const ktm::fvec3 zero = make_fvec3(0.0f, 0.0f, 0.0f);

    for (const auto profile_handle : profile_handles) {
        if (auto profile = hub.profile_storage().try_acquire_read_nowait(profile_handle)) {
            append_focus_geometry_source(sources, profile->geometry_handle, false, zero, zero);

            if (auto mechanics = hub.mechanics_storage().try_acquire_read_nowait(profile->mechanics_handle)) {
                append_focus_geometry_source(sources,
                                             mechanics->geometry_handle,
                                             true,
                                             mechanics->min_xyz,
                                             mechanics->max_xyz);
            }
            if (auto optics = hub.optics_storage().try_acquire_read_nowait(profile->optics_handle)) {
                append_focus_geometry_source(sources, optics->geometry_handle, false, zero, zero);
            }
            if (auto acoustics = hub.acoustics_storage().try_acquire_read_nowait(profile->acoustics_handle)) {
                append_focus_geometry_source(sources, acoustics->geometry_handle, false, zero, zero);
            }
        }
    }

    return sources;
}

void expand_focus_bounds(FocusBounds& bounds, const ktm::fvec3& point) {
    if (!bounds.valid) {
        bounds.min = point;
        bounds.max = point;
        bounds.valid = true;
        return;
    }

    bounds.min[0] = std::min(bounds.min[0], point[0]);
    bounds.min[1] = std::min(bounds.min[1], point[1]);
    bounds.min[2] = std::min(bounds.min[2], point[2]);
    bounds.max[0] = std::max(bounds.max[0], point[0]);
    bounds.max[1] = std::max(bounds.max[1], point[1]);
    bounds.max[2] = std::max(bounds.max[2], point[2]);
}

bool append_geometry_focus_bounds(const FocusGeometrySource& source, FocusBounds& bounds) {
    auto& hub = Corona::SharedDataHub::instance();
    std::uintptr_t transform_handle = 0;
    std::uintptr_t model_resource_handle = 0;
    if (auto geometry = hub.geometry_storage().try_acquire_read_nowait(source.geometry_handle)) {
        transform_handle = geometry->transform_handle;
        model_resource_handle = geometry->model_resource_handle;
    }

    if (transform_handle == 0) {
        return false;
    }

    ktm::fvec3 local_min{};
    ktm::fvec3 local_max{};
    if (source.has_local_bounds) {
        local_min = source.local_min;
        local_max = source.local_max;
    } else {
        if (model_resource_handle == 0) {
            return false;
        }

        std::uint64_t model_id = 0;
        if (auto resource = hub.model_resource_storage().try_acquire_read_nowait(model_resource_handle)) {
            model_id = resource->model_id;
        }
        if (model_id == 0) {
            return false;
        }

        auto scene_resource =
            Corona::Resource::ResourceManager::get_instance()
                .acquire_read<Corona::Resource::Scene>(model_id);
        if (!scene_resource) {
            return false;
        }

        const auto aabb = scene_resource->get_scene_aabb();
        local_min = make_fvec3(aabb.min[0], aabb.min[1], aabb.min[2]);
        local_max = make_fvec3(aabb.max[0], aabb.max[1], aabb.max[2]);
    }

    auto transform = hub.model_transform_storage().try_acquire_read_nowait(transform_handle);
    if (!transform) {
        return false;
    }

    const ktm::fvec3 corners[8] = {
        make_fvec3(local_min[0], local_min[1], local_min[2]),
        make_fvec3(local_max[0], local_min[1], local_min[2]),
        make_fvec3(local_min[0], local_max[1], local_min[2]),
        make_fvec3(local_max[0], local_max[1], local_min[2]),
        make_fvec3(local_min[0], local_min[1], local_max[2]),
        make_fvec3(local_max[0], local_min[1], local_max[2]),
        make_fvec3(local_min[0], local_max[1], local_max[2]),
        make_fvec3(local_max[0], local_max[1], local_max[2]),
    };

    const ktm::fmat4x4 matrix = transform->compute_matrix();
    for (const auto& corner : corners) {
        expand_focus_bounds(bounds, transform_local_point_to_world(matrix, corner));
    }

    return true;
}

void send_focus_pose_result(const CefRefPtr<CefFrame>& frame,
                            const std::string& request_id,
                            const nlohmann::json& payload) {
    (void)frame;
    if (request_id.empty()) {
        return;
    }

    auto event_payload = payload.is_object()
                             ? payload
                             : nlohmann::json::object({{"payload", payload}});
    event_payload["request_id"] = request_id;
    emit_editor_api_event("SceneTools.focusPoseResult", event_payload);
}

bool get_numeric_arg(const CefRefPtr<CefListValue>& args, size_t index, double& out) {
    if (!args || index >= args->GetSize()) {
        return false;
    }

    const auto type = args->GetType(index);
    if (type == VTYPE_INT) {
        out = static_cast<double>(args->GetInt(index));
        return true;
    }
    if (type == VTYPE_DOUBLE) {
        out = args->GetDouble(index);
        return true;
    }
    return false;
}

bool handle_compute_actor_focus_pose_fast(const CefRefPtr<CefFrame>& frame,
                                          const CefRefPtr<CefProcessMessage>& message) {
    auto args = message->GetArgumentList();
    if (!args || args->GetSize() < 2 || args->GetType(1) != VTYPE_STRING) {
        return true;
    }

    const std::string request_id = args->GetString(1).ToString();
    auto fail = [&](const std::string& message_text) {
        nlohmann::json payload;
        payload["status"] = "error";
        payload["message"] = message_text;
        send_focus_pose_result(frame, request_id, payload);
        return true;
    };

    double actor_handle_value = 0.0;
    if (!get_numeric_arg(args, 0, actor_handle_value)) {
        return fail("actor handle is invalid");
    }

    const auto actor_handle = static_cast<std::uintptr_t>(actor_handle_value);
    if (actor_handle == 0) {
        return fail("actor handle is empty");
    }

    auto& hub = Corona::SharedDataHub::instance();
    bool actor_found = false;
    std::size_t profile_count = 0;
    if (auto actor = hub.actor_storage().try_acquire_read_nowait(actor_handle)) {
        actor_found = true;
        profile_count = actor->profile_handles.size();
    }
    const bool has_external_vision_binding = hub.has_external_vision_binding(actor_handle);

    FocusBounds bounds;
    const auto focus_sources = resolve_actor_focus_geometry_sources(actor_handle);
    for (const auto& source : focus_sources) {
        append_geometry_focus_bounds(source, bounds);
    }

    if (!bounds.valid) {
        return fail(
            "actor bounds are unavailable (handle=" + std::to_string(actor_handle) +
            ", actor_found=" + (actor_found ? "true" : "false") +
            ", profiles=" + std::to_string(profile_count) +
            ", geometry_sources=" + std::to_string(focus_sources.size()) +
            ", external_vision_binding=" + (has_external_vision_binding ? "true" : "false") + ")");
    }

    const ktm::fvec3 center = make_fvec3(
        (bounds.min[0] + bounds.max[0]) * 0.5f,
        (bounds.min[1] + bounds.max[1]) * 0.5f,
        (bounds.min[2] + bounds.max[2]) * 0.5f);
    const float dx = bounds.max[0] - bounds.min[0];
    const float dy = bounds.max[1] - bounds.min[1];
    const float dz = bounds.max[2] - bounds.min[2];
    const float diagonal = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float distance = std::max(diagonal * 2.0f, 1.0f);

    nlohmann::json payload;
    payload["status"] = "success";
    payload["position"] = {center[0], center[1], center[2] - distance};
    payload["forward"] = {0.0f, 0.0f, 1.0f};
    payload["up"] = {0.0f, 1.0f, 0.0f};
    payload["center"] = {center[0], center[1], center[2]};
    payload["distance"] = distance;
    send_focus_pose_result(frame, request_id, payload);
    return true;
}

bool handle_actor_transform_fast(const CefRefPtr<CefProcessMessage>& message) {
    auto args = message->GetArgumentList();
    if (!args || args->GetSize() < 3) {
        CFW_LOG_WARNING("ActorTransformFast dropped: expected 3 args");
        return true;
    }

    const auto handle_type = args->GetType(0);
    if (handle_type != VTYPE_INT && handle_type != VTYPE_DOUBLE) {
        CFW_LOG_WARNING("ActorTransformFast dropped: actor handle type is invalid");
        return true;
    }

    const auto handle_value =
        handle_type == VTYPE_INT ? static_cast<double>(args->GetInt(0)) : args->GetDouble(0);
    const auto actor_handle = static_cast<std::uintptr_t>(handle_value);
    if (actor_handle == 0 || args->GetType(1) != VTYPE_INT) {
        CFW_LOG_WARNING("ActorTransformFast dropped: actor handle={}, operation type={}", actor_handle, static_cast<int>(args->GetType(1)));
        return true;
    }

    ktm::fvec3 value{};
    if (!parse_vec3_list(args->GetList(2), value)) {
        CFW_LOG_WARNING("ActorTransformFast dropped: vector is invalid for actor {}", actor_handle);
        return true;
    }

    const auto operation = args->GetInt(1);
    auto& hub = Corona::SharedDataHub::instance();
    for (const auto geometry_handle :
         hub.resolve_actor_geometry_handles(actor_handle)) {
        auto geometry = hub.geometry_storage().try_acquire_read(geometry_handle);
        if (!geometry || geometry->transform_handle == 0) {
            continue;
        }
        if (auto transform = hub.model_transform_storage().try_acquire_write(geometry->transform_handle)) {
            switch (operation) {
                case 0:
                    transform->position = value;
                    break;
                case 1:
                    transform->euler_rotation = value;
                    break;
                case 2:
                    transform->scale = value;
                    break;
                default:
                    break;
            }
        }
    }

    return true;
}

void send_viewport_pick_result(const CefRefPtr<CefFrame>& frame,
                               const nlohmann::json& payload) {
    emit_editor_api_event_to_frame("SceneTools.actorPickResult", payload, frame);
}

void drain_actor_pick_completion_events_impl() {
    for (const auto& completion : Corona::SharedDataHub::instance().drain_actor_pick_completions()) {
        CFW_LOG_DEBUG("ViewportPick completion: camera={} scene='{}' request={} status={} actor=0x{:x} cam_px=({}, {})",
                      completion.camera_handle,
                      completion.scene_id,
                      completion.request_id,
                      completion.status,
                      completion.actor_handle,
                      completion.x,
                      completion.y);
        nlohmann::json payload = {
            {"status", completion.status},
            {"sceneId", completion.scene_id},
            {"cameraHandle", static_cast<std::uint64_t>(completion.camera_handle)},
            {"requestId", completion.request_id},
            {"actorHandle", static_cast<std::uint64_t>(completion.actor_handle)},
            {"x", completion.x},
            {"y", completion.y},
        };
        cef_request_response_broker().complete(
            completion.request_id, completion.camera_handle, completion.scene_id,
            CefResponse{completion.status, std::move(payload)});
    }
    auto& broker = cef_request_response_broker();
    broker.expire();
    broker.dispatch();
}

bool handle_viewport_pick(const CefRefPtr<CefFrame>& frame,
                          const CefRefPtr<CefProcessMessage>& message) {
    auto args = message->GetArgumentList();
    if (!args || args->GetSize() < 7) {
        CFW_LOG_WARNING("ViewportPick dropped: expected 7 args");
        return true;
    }

    const auto read_double = [&](int index) -> double {
        const auto type = args->GetType(index);
        if (type == VTYPE_INT) return static_cast<double>(args->GetInt(index));
        if (type == VTYPE_DOUBLE) return args->GetDouble(index);
        return 0.0;
    };

    const auto camera_handle = static_cast<std::uintptr_t>(read_double(0));
    const std::string scene_id =
        args->GetType(1) == VTYPE_STRING ? args->GetString(1).ToString() : std::string{};
    const std::string request_id =
        args->GetType(2) == VTYPE_STRING ? args->GetString(2).ToString() : std::string{};
    const double x = read_double(3);
    const double y = read_double(4);
    const double vp_w = read_double(5);
    const double vp_h = read_double(6);

    auto emit_error = [&](const char* message_text, std::uint32_t result_x = 0,
                          std::uint32_t result_y = 0) {
        nlohmann::json payload = {
            {"status", "error"},
            {"sceneId", scene_id},
            {"cameraHandle", static_cast<std::uint64_t>(camera_handle)},
            {"requestId", request_id},
            {"actorHandle", 0},
            {"x", result_x},
            {"y", result_y},
        };
        if (message_text) payload["message"] = message_text;
        send_viewport_pick_result(frame, payload);
    };

    if (camera_handle == 0 || request_id.empty() || scene_id.empty() ||
        vp_w <= 0.0 || vp_h <= 0.0 || !std::isfinite(x) || !std::isfinite(y)) {
        CFW_LOG_WARNING("ViewportPick: invalid params (camera={}, scene='{}', request='{}', vp={}x{})",
                        camera_handle, scene_id, request_id, vp_w, vp_h);
        emit_error("invalid params");
        return true;
    }

    auto& hub = Corona::SharedDataHub::instance();

    std::uintptr_t actor_pick_handle = 0;
    double cam_w = 1920.0;
    double cam_h = 1080.0;
    if (auto cam = hub.camera_storage().try_acquire_read(camera_handle)) {
        cam_w = static_cast<double>(cam->width);
        cam_h = static_cast<double>(cam->height);
        actor_pick_handle = cam->actor_pick_handle;
    } else {
        CFW_LOG_WARNING("ViewportPick: camera {} is unavailable", camera_handle);
        emit_error("camera unavailable");
        return true;
    }

    const auto browser = frame ? frame->GetBrowser() : nullptr;
    const int browser_id = browser ? browser->GetIdentifier() : 0;
    const std::string frame_id = frame ? frame->GetIdentifier().ToString() : std::string{};
    if (!browser || browser_id <= 0 || frame_id.empty()) {
        emit_error("invalid CEF frame");
        return true;
    }

    const bool registered = cef_request_response_broker().register_request(
        CefRequestContext{request_id, browser_id, frame_id, camera_handle, scene_id},
        [frame](const CefRequestContext& context, const CefResponse& response) {
            auto payload = response.payload.is_object()
                               ? response.payload
                               : nlohmann::json::object();
            payload["status"] = response.status;
            payload["sceneId"] = context.scene_id;
            payload["cameraHandle"] = static_cast<std::uint64_t>(context.camera_handle);
            payload["requestId"] = context.request_id;
            if (!payload.contains("actorHandle")) payload["actorHandle"] = 0;
            if (!payload.contains("x")) payload["x"] = 0;
            if (!payload.contains("y")) payload["y"] = 0;
            send_viewport_pick_result(frame, payload);
        });
    if (!registered) {
        emit_error("duplicate requestId");
        return true;
    }

    const double scaled_x = x * cam_w / vp_w;
    const double scaled_y = y * cam_h / vp_h;
    if (scaled_x < 0.0 || scaled_y < 0.0 ||
        scaled_x >= cam_w || scaled_y >= cam_h) {
        CFW_LOG_DEBUG("ViewportPick miss: camera={} request={} pos=({},{}) -> scaled=({},{})",
                      camera_handle, request_id, x, y, scaled_x, scaled_y);
        hub.enqueue_actor_pick_completion({camera_handle, scene_id, request_id, "miss", 0, 0, 0});
        return true;
    }

    const auto pick_x = static_cast<std::uint32_t>(scaled_x);
    const auto pick_y = static_cast<std::uint32_t>(scaled_y);

    if (actor_pick_handle == 0) {
        cef_request_response_broker().cancel(request_id);
        CFW_LOG_WARNING("ViewportPick: camera {} has no actor pick storage", camera_handle);
        emit_error("actor pick unavailable", pick_x, pick_y);
        return true;
    }

    auto pick = hub.actor_pick_storage().try_acquire_write(actor_pick_handle);
    if (!pick) {
        cef_request_response_broker().cancel(request_id);
        CFW_LOG_WARNING("ViewportPick: pick storage {} is unavailable", actor_pick_handle);
        emit_error("actor pick storage unavailable", pick_x, pick_y);
        return true;
    }

    (void)pick;
    hub.enqueue_actor_pick_request({camera_handle, scene_id, request_id, pick_x, pick_y});
    CFW_LOG_DEBUG("ViewportPick enqueue: camera={} scene='{}' request={} pos=({},{}) vp={}x{} cam={}x{} -> cam_px=({},{})",
                  camera_handle, scene_id, request_id, x, y, vp_w, vp_h, cam_w, cam_h, pick_x, pick_y);

    return true;
}

Corona::ViewportUiMode parse_viewport_ui_mode(const std::string& mode) {
    return mode == "stereo3d"
        ? Corona::ViewportUiMode::Stereo3D
        : Corona::ViewportUiMode::Flat2D;
}

Corona::ViewportUiCursorShape parse_viewport_ui_cursor_shape(const std::string& shape) {
    if (shape == "hand" || shape == "pointer") return Corona::ViewportUiCursorShape::Hand;
    if (shape == "crosshair") return Corona::ViewportUiCursorShape::Crosshair;
    if (shape == "grab") return Corona::ViewportUiCursorShape::Grab;
    if (shape == "grabbing") return Corona::ViewportUiCursorShape::Grabbing;
    if (shape == "hidden" || shape == "none") return Corona::ViewportUiCursorShape::Hidden;
    return Corona::ViewportUiCursorShape::Arrow;
}

const char* viewport_gizmo_axis_name(Corona::ViewportGizmoAxis axis) {
    switch (axis) {
        case Corona::ViewportGizmoAxis::X: return "x";
        case Corona::ViewportGizmoAxis::Y: return "y";
        case Corona::ViewportGizmoAxis::Z: return "z";
        default: return "none";
    }
}

ktm::fvec3 viewport_gizmo_axis_vector(Corona::ViewportGizmoAxis axis) {
    switch (axis) {
        case Corona::ViewportGizmoAxis::X: return make_fvec3(1.0f, 0.0f, 0.0f);
        case Corona::ViewportGizmoAxis::Y: return make_fvec3(0.0f, 1.0f, 0.0f);
        case Corona::ViewportGizmoAxis::Z: return make_fvec3(0.0f, 0.0f, 1.0f);
        default: return make_fvec3(0.0f, 0.0f, 0.0f);
    }
}

std::optional<ktm::fvec3> actor_gizmo_position(std::uintptr_t actor_handle) {
    auto& hub = Corona::SharedDataHub::instance();
    for (const auto geometry_handle :
         hub.resolve_actor_geometry_handles(actor_handle)) {
        auto geometry = hub.geometry_storage().try_acquire_read(geometry_handle);
        if (!geometry || geometry->transform_handle == 0) {
            continue;
        }
        if (auto transform =
                hub.model_transform_storage().try_acquire_read(geometry->transform_handle)) {
            return transform->position;
        }
    }
    return std::nullopt;
}

bool write_actor_gizmo_position(std::uintptr_t actor_handle,
                                const ktm::fvec3& position) {
    bool wrote = false;
    auto& hub = Corona::SharedDataHub::instance();
    for (const auto geometry_handle :
         hub.resolve_actor_geometry_handles(actor_handle)) {
        auto geometry = hub.geometry_storage().try_acquire_read(geometry_handle);
        if (!geometry || geometry->transform_handle == 0) {
            continue;
        }
        if (auto transform =
                hub.model_transform_storage().try_acquire_write(geometry->transform_handle)) {
            transform->position = position;
            wrote = true;
        }
    }
    return wrote;
}

ktm::fvec3 viewport_gizmo_pointer_ray(const Corona::CameraDevice& camera,
                                      ktm::fvec2 pixel,
                                      float width,
                                      float height) {
    const auto forward = ktm::normalize(camera.forward);
    auto right = ktm::cross(camera.world_up, forward);
    right = ktm::length(right) > 1.0e-5f
                ? ktm::normalize(right)
                : make_fvec3(1.0f, 0.0f, 0.0f);
    const auto up = ktm::normalize(ktm::cross(forward, right));
    const float ndc_x = 2.0f * pixel.x / std::max(width, 1.0f) - 1.0f;
    const float ndc_y = 2.0f * pixel.y / std::max(height, 1.0f) - 1.0f;
    const float tan_half_fov = std::tan(ktm::radians(camera.fov) * 0.5f);
    return ktm::normalize(
        forward +
        right * (ndc_x * tan_half_fov * camera.aspect) -
        up * (ndc_y * tan_half_fov));
}

void emit_viewport_gizmo_result(const Corona::ViewportGizmoPointerResult& result,
                                const Corona::ViewportGizmoTarget& target) {
    nlohmann::json payload{
        {"cameraHandle", static_cast<std::uint64_t>(result.camera_handle)},
        {"requestId", result.request_id},
        {"axis", viewport_gizmo_axis_name(result.axis)},
        {"consumed", result.consumed},
        {"dragging", result.dragging},
        {"ended", result.ended},
        {"cancelled", result.cancelled},
        {"sceneId", target.scene_id},
        {"actor", target.actor_name},
        {"actorHandle", static_cast<std::uint64_t>(target.actor_handle)},
        {"position", {result.position.x, result.position.y, result.position.z}},
    };
    emit_editor_api_event("SceneTools.viewportGizmoPointerResult", payload);
}

void emit_viewport_gizmo_transform(const Corona::ViewportGizmoTarget& target,
                                   const ktm::fvec3& position) {
    emit_editor_api_event(
        "SceneTools.actorTransformUpdated",
        nlohmann::json{
            {"scene", target.scene_id},
            {"actor", target.actor_name},
            {"position", {position.x, position.y, position.z}},
        });
}

bool handle_viewport_gizmo_target(const CefRefPtr<CefProcessMessage>& message) {
    auto args = message->GetArgumentList();
    double camera_value = 0.0;
    double actor_value = 0.0;
    if (!args || args->GetSize() < 4 ||
        !get_numeric_arg(args, 0, camera_value) ||
        args->GetType(1) != VTYPE_STRING ||
        args->GetType(2) != VTYPE_STRING ||
        !get_numeric_arg(args, 3, actor_value)) {
        CFW_LOG_WARNING(
            "ViewportGizmoTarget dropped: expected (cameraHandle, sceneId, actorName, actorHandle)");
        return true;
    }
    const auto camera_handle = static_cast<std::uintptr_t>(camera_value);
    const auto actor_handle = static_cast<std::uintptr_t>(actor_value);
    auto& hub = Corona::SharedDataHub::instance();
    if (actor_handle == 0) {
        hub.clear_viewport_gizmo_target(camera_handle);
        std::lock_guard lock(s_viewport_gizmo_mutex);
        s_viewport_gizmo_drags.erase(camera_handle);
        CFW_LOG_INFO("Viewport gizmo target cleared: camera={}", camera_handle);
        return true;
    }
    hub.set_viewport_gizmo_target({
        .camera_handle = camera_handle,
        .scene_id = args->GetString(1).ToString(),
        .actor_name = args->GetString(2).ToString(),
        .actor_handle = actor_handle,
    });
    CFW_LOG_INFO("Viewport gizmo target set: camera={} scene='{}' actor='{}' handle=0x{:x}",
                 camera_handle,
                 args->GetString(1).ToString(),
                 args->GetString(2).ToString(),
                 actor_handle);
    return true;
}

bool handle_viewport_gizmo_pointer(const CefRefPtr<CefProcessMessage>& message) {
    auto args = message->GetArgumentList();
    double camera_value = 0.0;
    double x_value = 0.0;
    double y_value = 0.0;
    double width_value = 0.0;
    double height_value = 0.0;
    double button_value = 0.0;
    if (!args || args->GetSize() < 10 ||
        !get_numeric_arg(args, 0, camera_value) ||
        args->GetType(1) != VTYPE_STRING ||
        args->GetType(2) != VTYPE_STRING ||
        !get_numeric_arg(args, 3, x_value) ||
        !get_numeric_arg(args, 4, y_value) ||
        !get_numeric_arg(args, 5, width_value) ||
        !get_numeric_arg(args, 6, height_value) ||
        !get_numeric_arg(args, 7, button_value)) {
        CFW_LOG_WARNING("ViewportGizmoPointer dropped: invalid arguments");
        return true;
    }

    const auto camera_handle = static_cast<std::uintptr_t>(camera_value);
    const std::string request_id = args->GetString(1).ToString();
    std::string event_type = args->GetString(2).ToString();
    std::transform(event_type.begin(), event_type.end(), event_type.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    auto& hub = Corona::SharedDataHub::instance();
    const auto state = hub.viewport_gizmo_state(camera_handle);
    Corona::ViewportGizmoPointerResult result;
    result.camera_handle = camera_handle;
    result.request_id = request_id;
    if (state.target.actor_handle == 0 || width_value <= 0.0 || height_value <= 0.0) {
        emit_viewport_gizmo_result(result, state.target);
        return true;
    }

    auto camera = hub.camera_storage().try_acquire_read(camera_handle);
    const auto current_position = actor_gizmo_position(state.target.actor_handle);
    if (!camera || !current_position) {
        hub.clear_viewport_gizmo_target(camera_handle);
        emit_viewport_gizmo_result(result, state.target);
        return true;
    }

    const float camera_width = static_cast<float>(std::max(camera->width, 1u));
    const float camera_height = static_cast<float>(std::max(camera->height, 1u));
    ktm::fvec2 pointer{
        static_cast<float>(x_value / width_value) * camera_width,
        static_cast<float>(y_value / height_value) * camera_height,
    };
    const auto ui_state = hub.viewport_ui_state(camera_handle);
    if (ui_state.mode == Corona::ViewportUiMode::Stereo3D) {
        pointer = Corona::Systems::OpticsDetail::viewport_ui_prewarp_point(
            pointer,
            {
                .lenticular_pitch = ui_state.calibration.lenticular_pitch,
                .slant_angle_radians = ui_state.calibration.slant_angle_radians,
                .phase_offset = ui_state.calibration.phase_offset,
                .green_subpixel_offset = ui_state.calibration.rgb_subpixel_offsets[1],
                .parallax_scale = ui_state.calibration.parallax_scale,
            });
    }

    const auto layout = Corona::Systems::OpticsDetail::make_viewport_gizmo_layout(
        camera->compute_view_proj_matrix(), *current_position,
        camera->width, camera->height, 128.0f);
    const auto& cpu_masks = viewport_gizmo_cpu_masks();
    const auto hover_axis =
        cpu_masks.valid
            ? Corona::Systems::OpticsDetail::hit_test_viewport_gizmo_alpha(
                  layout, pointer, cpu_masks.views, 8.0f, 128.0f)
            : Corona::ViewportGizmoAxis::None;

    std::lock_guard lock(s_viewport_gizmo_mutex);
    auto drag_it = s_viewport_gizmo_drags.find(camera_handle);
    const bool cancel_event =
        event_type == "cancel" || event_type == "pointercancel" ||
        event_type == "blur" || event_type == "escape";
    const bool up_event = event_type == "up" || event_type == "pointerup";
    const bool down_event = event_type == "down" || event_type == "pointerdown";

    if (cancel_event && drag_it != s_viewport_gizmo_drags.end()) {
        const bool restored = write_actor_gizmo_position(
            drag_it->second.actor_handle, drag_it->second.initial_position);
        CFW_LOG_WARNING("Viewport gizmo drag cancelled: camera={} actor='{}' axis={} event='{}' restored={}",
                        camera_handle,
                        drag_it->second.actor_name,
                        static_cast<int>(drag_it->second.axis),
                        event_type,
                        restored);
        result.axis = drag_it->second.axis;
        result.consumed = true;
        result.cancelled = true;
        result.position = drag_it->second.initial_position;
        emit_viewport_gizmo_transform(state.target, result.position);
        s_viewport_gizmo_drags.erase(drag_it);
        hub.update_viewport_gizmo_interaction(
            camera_handle, Corona::ViewportGizmoAxis::None, false);
        emit_viewport_gizmo_result(result, state.target);
        return true;
    }

    if (down_event && static_cast<int>(button_value) == 0 &&
        drag_it == s_viewport_gizmo_drags.end() &&
        hover_axis != Corona::ViewportGizmoAxis::None) {
        const auto axis_vector = viewport_gizmo_axis_vector(hover_axis);
        const auto ray =
            viewport_gizmo_pointer_ray(*camera, pointer, camera_width, camera_height);
        ViewportGizmoDragRuntime drag;
        drag.actor_handle = state.target.actor_handle;
        drag.axis = hover_axis;
        drag.initial_position = *current_position;
        drag.last_position = *current_position;
        drag.initial_pointer = pointer;
        const std::size_t axis_index =
            hover_axis == Corona::ViewportGizmoAxis::X ? 0u :
            (hover_axis == Corona::ViewportGizmoAxis::Y ? 1u : 2u);
        drag.fallback_screen_direction = layout.axes[axis_index].direction;
        drag.initial_axis_parameter =
            Corona::Systems::OpticsDetail::closest_axis_parameter(
                camera->position, ray, *current_position, axis_vector);
        drag.scene_id = state.target.scene_id;
        drag.actor_name = state.target.actor_name;
        s_viewport_gizmo_drags[camera_handle] = drag;
        CFW_LOG_INFO("Viewport gizmo drag start: camera={} actor='{}' axis={} initial_parameter={}",
                     camera_handle,
                     state.target.actor_name,
                     static_cast<int>(hover_axis),
                     drag.initial_axis_parameter
                         ? std::to_string(*drag.initial_axis_parameter)
                         : std::string("degenerate"));
        hub.update_viewport_gizmo_interaction(camera_handle, hover_axis, true);
        result.axis = hover_axis;
        result.consumed = true;
        result.dragging = true;
        result.position = *current_position;
        emit_viewport_gizmo_result(result, state.target);
        return true;
    }

    drag_it = s_viewport_gizmo_drags.find(camera_handle);
    if (drag_it != s_viewport_gizmo_drags.end()) {
        auto& drag = drag_it->second;
        const auto axis_vector = viewport_gizmo_axis_vector(drag.axis);
        const auto ray =
            viewport_gizmo_pointer_ray(*camera, pointer, camera_width, camera_height);
        const auto parameter =
            Corona::Systems::OpticsDetail::closest_axis_parameter(
                camera->position, ray, drag.initial_position, axis_vector);
        const float depth = std::max(
            ktm::dot(drag.initial_position - camera->position,
                     ktm::normalize(camera->forward)),
            camera->near_plane);
        const float world_per_pixel =
            2.0f * depth * std::tan(ktm::radians(camera->fov) * 0.5f) /
            camera_height;
        float delta = 0.0f;
        if (parameter && drag.initial_axis_parameter) {
            delta = *parameter - *drag.initial_axis_parameter;
        } else {
            if (!drag.fallback_logged) {
                CFW_LOG_WARNING("Viewport gizmo drag fallback: camera={} actor='{}' axis={} parameter_valid={} initial_parameter_valid={} screen_direction=({}, {}) world_per_pixel={}",
                                camera_handle,
                                drag.actor_name,
                                static_cast<int>(drag.axis),
                                parameter.has_value(),
                                drag.initial_axis_parameter.has_value(),
                                drag.fallback_screen_direction.x,
                                drag.fallback_screen_direction.y,
                                world_per_pixel);
                drag.fallback_logged = true;
            }
            delta = ktm::dot(pointer - drag.initial_pointer,
                             drag.fallback_screen_direction) * world_per_pixel;
        }
        delta = std::clamp(delta, -10000.0f, 10000.0f);
        const float previous_delta =
            ktm::dot(drag.last_position - drag.initial_position, axis_vector);
        const float max_frame_delta = std::max(world_per_pixel * 256.0f, 0.25f);
        delta = std::clamp(
            delta,
            previous_delta - max_frame_delta,
            previous_delta + max_frame_delta);
        const auto next_position = drag.initial_position + axis_vector * delta;
        const bool wrote = write_actor_gizmo_position(drag.actor_handle, next_position);
        if (wrote) {
            drag.last_position = next_position;
            emit_viewport_gizmo_transform(state.target, next_position);
        } else if (!drag.write_failure_logged) {
            CFW_LOG_WARNING("Viewport gizmo transform write failed: camera={} actor='{}' axis={} handle={}",
                            camera_handle,
                            drag.actor_name,
                            static_cast<int>(drag.axis),
                            drag.actor_handle);
            drag.write_failure_logged = true;
        }
        result.axis = drag.axis;
        result.consumed = true;
        result.position = next_position;
        if (up_event) {
            CFW_LOG_INFO("Viewport gizmo drag ended: camera={} actor='{}' axis={} position=({}, {}, {})",
                         camera_handle,
                         drag.actor_name,
                         static_cast<int>(drag.axis),
                         next_position.x,
                         next_position.y,
                         next_position.z);
            result.ended = true;
            result.dragging = false;
            s_viewport_gizmo_drags.erase(drag_it);
            hub.update_viewport_gizmo_interaction(
                camera_handle, Corona::ViewportGizmoAxis::None, false);
        } else {
            result.dragging = true;
            hub.update_viewport_gizmo_interaction(camera_handle, drag.axis, true);
        }
        emit_viewport_gizmo_result(result, state.target);
        return true;
    }

    hub.update_viewport_gizmo_interaction(
        camera_handle, Corona::ViewportGizmoAxis::None, false, hover_axis);
    result.axis = hover_axis;
    result.position = *current_position;
    emit_viewport_gizmo_result(result, state.target);
    return true;
}

// 把 Vision 光场语义参数 (pe/angle/offset, 子像素单位) 转成 warp 消费的
// ViewportUiCalibration (像素单位)。推导见计划文档：
//   pitch = pe/3   (子像素周期→像素周期)
//   slant_angle_radians = -angle  (Vision 相位场 +tan·y vs shader -tan·y)
//   phase_offset = offset/pe      (子像素偏移→周期数)
// parallax_scale 是 UI 专有视差增益，无 Vision 对应，直接透传。
Corona::ViewportUiCalibration calibration_from_lenticular(double pe,
                                                          double angle,
                                                          double offset,
                                                          double parallax_scale) {
    Corona::ViewportUiCalibration calibration;
    const double safe_pe = std::abs(pe) > 1.0e-5 ? pe : 1.0e-5;
    calibration.lenticular_pitch = static_cast<float>(safe_pe / 3.0);
    calibration.slant_angle_radians = static_cast<float>(-angle);
    calibration.phase_offset = static_cast<float>(offset / safe_pe);
    calibration.rgb_subpixel_offsets = {0.0f, 1.0f / 3.0f, 2.0f / 3.0f};
    calibration.parallax_scale = static_cast<float>(parallax_scale);
    return calibration;
}

bool handle_viewport_ui_mode(const CefRefPtr<CefProcessMessage>& message) {
    auto args = message->GetArgumentList();
    double camera_value = 0.0;
    if (!args || args->GetSize() < 2 ||
        !get_numeric_arg(args, 0, camera_value) ||
        args->GetType(1) != VTYPE_STRING) {
        CFW_LOG_WARNING("ViewportUiMode dropped: expected (cameraHandle, mode)");
        return true;
    }

    const auto camera_handle = static_cast<std::uintptr_t>(camera_value);
    const std::string mode_name = args->GetString(1).ToString();
    const auto mode = parse_viewport_ui_mode(mode_name);
    Corona::SharedDataHub::instance().set_viewport_ui_mode(camera_handle, mode);
    CFW_LOG_INFO("ViewportUiMode set: camera={} mode={}",
                 camera_handle,
                 mode == Corona::ViewportUiMode::Stereo3D ? "stereo3d" : "flat2d");
    return true;
}

bool handle_viewport_system_cursor(CefRefPtr<CefBrowser> browser,
                                   const CefRefPtr<CefProcessMessage>& message) {
    auto args = message->GetArgumentList();
    if (!args || args->GetSize() < 1) {
        CFW_LOG_WARNING("ViewportSystemCursor dropped: expected (hidden[, custom])");
        return true;
    }

    bool hidden = false;
    if (args->GetType(0) == VTYPE_BOOL) {
        hidden = args->GetBool(0);
    } else {
        double numeric = 0.0;
        hidden = get_numeric_arg(args, 0, numeric) && numeric != 0.0;
    }

    bool custom = hidden;
    if (args->GetSize() > 1) {
        if (args->GetType(1) == VTYPE_BOOL) {
            custom = args->GetBool(1);
        } else {
            double numeric = 0.0;
            custom = get_numeric_arg(args, 1, numeric) && numeric != 0.0;
        }
    }

    if (!set_browser_tab_system_cursor_state(browser, hidden, custom)) {
        CFW_LOG_WARNING("ViewportSystemCursor dropped: browser tab not found");
    }
    return true;
}

bool handle_viewport_ui_pointer(const CefRefPtr<CefProcessMessage>& message) {
    auto args = message->GetArgumentList();
    double camera_value = 0.0;
    double x = 0.0;
    double y = 0.0;
    if (!args || args->GetSize() < 4 ||
        !get_numeric_arg(args, 0, camera_value) ||
        args->GetType(1) != VTYPE_STRING ||
        !get_numeric_arg(args, 2, x) ||
        !get_numeric_arg(args, 3, y)) {
        CFW_LOG_WARNING("ViewportUiPointer dropped: expected (cameraHandle, type, x, y, ...)");
        return true;
    }

    Corona::ViewportUiPointerCommand command;
    command.camera_handle = static_cast<std::uintptr_t>(camera_value);
    command.event_type = args->GetString(1).ToString();
    command.x = static_cast<float>(x);
    command.y = static_cast<float>(y);

    double buttons = 0.0;
    double modifiers = 0.0;
    if (get_numeric_arg(args, 4, buttons)) {
        command.buttons = static_cast<std::uint32_t>(std::max(0.0, buttons));
    }
    if (get_numeric_arg(args, 5, modifiers)) {
        command.modifiers = static_cast<std::uint32_t>(std::max(0.0, modifiers));
    }
    if (args->GetSize() > 6 && args->GetType(6) == VTYPE_STRING) {
        command.cursor_shape = parse_viewport_ui_cursor_shape(args->GetString(6).ToString());
    }

    Corona::SharedDataHub::instance().enqueue_viewport_ui_pointer(std::move(command));
    return true;
}

bool handle_viewport_ui_calibration(const CefRefPtr<CefProcessMessage>& message) {
    auto args = message->GetArgumentList();
    double camera_value = 0.0;
    double pe = 0.0;
    double angle = 0.0;
    double offset = 0.0;
    double parallax_scale = 0.0;
    if (!args || args->GetSize() < 5 ||
        !get_numeric_arg(args, 0, camera_value) ||
        !get_numeric_arg(args, 1, pe) ||
        !get_numeric_arg(args, 2, angle) ||
        !get_numeric_arg(args, 3, offset) ||
        !get_numeric_arg(args, 4, parallax_scale)) {
        CFW_LOG_WARNING(
            "ViewportUiCalibration dropped: expected (cameraHandle, pe, angle, offset, parallaxScale)");
        return true;
    }

    const auto camera_handle = static_cast<std::uintptr_t>(camera_value);
    const auto calibration =
        calibration_from_lenticular(pe, angle, offset, parallax_scale);
    Corona::SharedDataHub::instance().set_viewport_ui_calibration(camera_handle, calibration);
    CFW_LOG_INFO(
        "ViewportUiCalibration set: camera={} pe={:.4f} angle={:.4f} offset={:.4f} parallax={:.4f}",
        camera_handle, pe, angle, offset, parallax_scale);
    return true;
}

bool handle_property_fast(const CefRefPtr<CefProcessMessage>& message) {
    auto args = message->GetArgumentList();
    if (!args || args->GetSize() < 3) {
        CFW_LOG_WARNING("PropertyFast dropped: expected 3 args");
        return true;
    }

    const auto handle_type = args->GetType(0);
    if (handle_type != VTYPE_INT && handle_type != VTYPE_DOUBLE) {
        CFW_LOG_WARNING("PropertyFast dropped: actor handle type is invalid");
        return true;
    }

    const auto handle_value =
        handle_type == VTYPE_INT ? static_cast<double>(args->GetInt(0)) : args->GetDouble(0);
    const auto actor_handle = static_cast<std::uintptr_t>(handle_value);
    if (actor_handle == 0 || args->GetType(1) != VTYPE_INT) {
        CFW_LOG_WARNING("PropertyFast dropped: actor handle={}, propertyType type={}",
                        actor_handle, static_cast<int>(args->GetType(1)));
        return true;
    }

    const auto property_type = args->GetInt(1);
    double value = 0.0;
    const auto value_type = args->GetType(2);
    if (value_type == VTYPE_INT) {
        value = static_cast<double>(args->GetInt(2));
    } else if (value_type == VTYPE_DOUBLE) {
        value = args->GetDouble(2);
    } else {
        CFW_LOG_WARNING("PropertyFast dropped: value type is invalid");
        return true;
    }

    auto& hub = Corona::SharedDataHub::instance();
    for (const auto profile_handle : resolve_profile_handles(actor_handle)) {
        auto profile = hub.profile_storage().try_acquire_read(profile_handle);
        if (!profile) continue;

        switch (property_type) {
            case 0:  // Mass
                if (profile->mechanics_handle != 0) {
                    if (auto mech = hub.mechanics_storage().try_acquire_write(profile->mechanics_handle)) {
                        mech->mass = static_cast<float>(value);
                    }
                }
                break;
            case 1:  // Restitution
                if (profile->mechanics_handle != 0) {
                    if (auto mech = hub.mechanics_storage().try_acquire_write(profile->mechanics_handle)) {
                        mech->restitution = static_cast<float>(value);
                    }
                }
                break;
            case 2:  // Damping
                if (profile->mechanics_handle != 0) {
                    if (auto mech = hub.mechanics_storage().try_acquire_write(profile->mechanics_handle)) {
                        mech->damping = static_cast<float>(value);
                    }
                }
                break;
            case 3:  // Visible
                if (profile->optics_handle != 0) {
                    if (auto opt = hub.optics_storage().try_acquire_write(profile->optics_handle)) {
                        opt->visible = (value != 0.0);
                    }
                }
                break;
            case 4:  // CollisionEnabled
                if (profile->mechanics_handle != 0) {
                    if (auto mech = hub.mechanics_storage().try_acquire_write(profile->mechanics_handle)) {
                        mech->collision_shape = (value != 0.0)
                            ? CollisionShape::Box : CollisionShape::None;
                    }
                }
                break;
            case 5:  // PhysicsEnabled
                if (profile->mechanics_handle != 0) {
                    if (auto mech = hub.mechanics_storage().try_acquire_write(profile->mechanics_handle)) {
                        mech->physics_enabled = (value != 0.0);
                    }
                }
                break;
            case 8:  // CollisionShape: 0=None, 1=Box, 2=Mesh
                if (profile->mechanics_handle != 0) {
                    if (auto mech = hub.mechanics_storage().try_acquire_write(profile->mechanics_handle)) {
                        const int shape = static_cast<int>(value);
                        mech->collision_shape = shape == 0 ? CollisionShape::None
                            : (shape == 2 ? CollisionShape::Mesh : CollisionShape::Box);
                    }
                }
                break;
            case 6:  // LinearLockMask (bit0=X, bit1=Y, bit2=Z)
                if (profile->mechanics_handle != 0) {
                    if (auto mech = hub.mechanics_storage().try_acquire_write(profile->mechanics_handle)) {
                        mech->linear_lock_mask = static_cast<uint8_t>(value);
                    }
                }
                break;
            case 7:  // AngularLockMask (bit0=X, bit1=Y, bit2=Z)
                if (profile->mechanics_handle != 0) {
                    if (auto mech = hub.mechanics_storage().try_acquire_write(profile->mechanics_handle)) {
                        mech->angular_lock_mask = static_cast<uint8_t>(value);
                    }
                }
                break;
            default:
                CFW_LOG_WARNING("PropertyFast: unknown propertyType {}", property_type);
                break;
        }
    }

    return true;
}

bool handle_input_inject(const CefRefPtr<CefProcessMessage>& message) {
    auto args = message->GetArgumentList();
    if (!args || args->GetSize() < 1) {
        CFW_LOG_WARNING("InputInject dropped: expected at least 1 arg");
        return true;
    }

    InputEvent evt{};
    evt.type = args->GetInt(0);

    switch (evt.type) {
        case 0:  // keyDown(code, modifiers?, displayKey?)
            evt.arg0 = args->GetSize() > 1 ? args->GetString(1).ToString() : "";
            evt.arg1 = args->GetSize() > 2 ? args->GetString(2).ToString() : "";
            evt.arg2 = args->GetSize() > 3 ? args->GetString(3).ToString() : evt.arg0;
            break;
        case 1:  // keyUp(code, displayKey?)
            evt.arg0 = args->GetSize() > 1 ? args->GetString(1).ToString() : "";
            evt.arg1 = args->GetSize() > 2 ? args->GetString(2).ToString() : evt.arg0;
            break;
        case 2:  // mouseEvent(eventType, button?, x?, y?)
            evt.arg0 = args->GetSize() > 1 ? args->GetString(1).ToString() : "";
            evt.arg1 = args->GetSize() > 2 ? args->GetString(2).ToString() : "";
            evt.arg3 = args->GetSize() > 3 ? (args->GetType(3) == VTYPE_INT ? static_cast<double>(args->GetInt(3)) : args->GetDouble(3)) : 0.0;
            evt.arg4 = args->GetSize() > 4 ? (args->GetType(4) == VTYPE_INT ? static_cast<double>(args->GetInt(4)) : args->GetDouble(4)) : 0.0;
            break;
        default:
            CFW_LOG_WARNING("InputInject: unknown type {}", evt.type);
            return true;
    }

    enqueue_input_event(std::move(evt));
    return true;
}

int find_tab_id_for_browser(const CefRefPtr<CefBrowser>& browser) {
    if (!browser) {
        return -1;
    }

    const int browser_id = browser->GetIdentifier();
    for (auto& [tab_id, tab] : BrowserManager::instance().get_tabs()) {
        if (tab->client && tab->client->GetBrowser() &&
            tab->client->GetBrowser()->GetIdentifier() == browser_id) {
            return tab_id;
        }
    }

    return -1;
}

int resolve_camera_tab_id(const nlohmann::json& command,
                          const CefRefPtr<CefBrowser>& browser) {
    const std::string scene_id = command.value("sceneId", "");
    const std::string camera_id = command.value("cameraId", "");
    if (!scene_id.empty() && !camera_id.empty()) {
        if (auto existing = CameraViewportManager::instance().find_by_camera(scene_id, camera_id)) {
            return existing->tab_id;
        }
    }
    if (!camera_id.empty()) {
        for (auto& [tab_id, tab] : BrowserManager::instance().get_tabs()) {
            if (tab && tab->camera_view && tab->url.find(camera_id) != std::string::npos) {
                return tab_id;
            }
        }
    }
    return find_tab_id_for_browser(browser);
}

std::string source_base_url(const CefRefPtr<CefBrowser>& browser) {
    if (!browser || !browser->GetMainFrame()) {
        return {};
    }

    std::string url = browser->GetMainFrame()->GetURL().ToString();
    const auto hash_pos = url.find('#');
    if (hash_pos != std::string::npos) {
        url = url.substr(0, hash_pos);
    }
    return url;
}

void execute_tab_javascript(BrowserTab* tab, const std::string& js) {
    if (tab && tab->client && tab->client->GetBrowser()) {
        tab->client->GetBrowser()->GetMainFrame()->ExecuteJavaScript(js, "", 0);
    }
}

void send_dock_callback(const CefRefPtr<CefFrame>& frame,
                        const std::string& request_id,
                        const nlohmann::json& error,
                        const nlohmann::json& result) {
    if (!frame || request_id.empty()) {
        return;
    }

    std::string js = "window.__dockCallback&&window.__dockCallback(" +
                     nlohmann::json(request_id).dump() + "," +
                     (error.is_null() ? "null" : error.dump()) + "," +
                     (result.is_null() ? "null" : result.dump()) + ")";
    frame->ExecuteJavaScript(js, "", 0);
}

void broadcast_dock_event(const std::string& event, const nlohmann::json& payload) {
    std::string args_js;
    if (payload.is_array()) {
        for (size_t i = 0; i < payload.size(); ++i) {
            if (i > 0) {
                args_js += ",";
            }
            args_js += payload[i].dump();
        }
        if (!args_js.empty()) {
            args_js += ",";
        }
    } else {
        args_js = payload.dump();
        args_js += ",";
    }

    args_js += "{\"_fromCross\":1}";
    std::string js = "if(window.__coronaEmit)window.__coronaEmit(" +
                     nlohmann::json(event).dump() + "," + args_js + ")";

    for (auto& [tab_id, tab] : BrowserManager::instance().get_tabs()) {
        if (!tab->minimized) {
            execute_tab_javascript(tab.get(), js);
        }
    }
}

bool handle_dock_command(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame,
                         const CefRefPtr<CefProcessMessage>& message) {
    auto args = message->GetArgumentList();
    if (!args || args->GetSize() < 1 || args->GetType(0) != VTYPE_STRING) {
        return true;
    }

    std::string request_id;
    try {
        auto command = nlohmann::json::parse(args->GetString(0).ToString());
        request_id = command.value("requestId", "");
        const std::string cmd = command.value("cmd", "");
        auto& bm = BrowserManager::instance();

        if (cmd == "createDetachedPanel") {
            CFW_LOG_INFO("DockCommand createDetachedPanel received: panel_id={}, route={}, size={}x{}, pos=({}, {})",
                         command.value("panelId", ""), command.value("routePath", ""),
                         command.value("width", 400), command.value("height", 600),
                         command.value("x", 120), command.value("y", 120));
        }

        if (cmd == "createCameraView") {
            const std::string scene_id = command.value("sceneId", "");
            const std::string camera_id = command.value("cameraId", "");
            const auto camera_handle =
                command.value("cameraHandle", static_cast<std::uintptr_t>(0));
            std::string route = command.value("routePath", "");
            const int width = command.value("width", 960);
            const int height = command.value("height", 540);
            const int x = command.value("x", 120);
            const int y = command.value("y", 120);

            if (scene_id.empty() || camera_id.empty() || camera_id == "undefined" ||
                camera_id == "null" || camera_handle == 0) {
                nlohmann::json error;
                error["message"] = "createCameraView requires a valid sceneId, cameraId, and cameraHandle";
                send_dock_callback(frame, request_id, error, nullptr);
                return true;
            }

            if (auto existing = CameraViewportManager::instance().find_by_camera(
                    scene_id, camera_id)) {
                nlohmann::json result;
                result["tab_id"] = existing->tab_id;
                result["existing"] = true;
                send_dock_callback(frame, request_id, nullptr, result);
                return true;
            }

            if (!route.empty() && route[0] == '#') {
                route = route.substr(1);
            }
            route += (route.find('?') == std::string::npos) ? "?standalone=1" : "&standalone=1";

            const std::string base_url = source_base_url(browser);
            bm.enqueue_main_thread_task(
                [base_url, route, scene_id, camera_id, camera_handle, width, height, x, y] {
                    auto& browser_manager = BrowserManager::instance();
                    if (CameraViewportManager::instance().find_by_camera(
                            scene_id, camera_id)) {
                        return;
                    }
                    const int tab_id = browser_manager.create_tab(
                        base_url, route, "camera", width, height, false, true, x, y);
                    if (auto* tab = browser_manager.get_tab(tab_id)) {
                        // Camera views are standalone secondary surfaces.  Mark the tab for
                        // detachment before it can be laid out in the main window, so the
                        // frame runner creates the SDL child window and routes this camera's
                        // image to its own surface.
                        tab->detach_x = x;
                        tab->detach_y = y;
                        tab->detach_w = (width > 0) ? width : std::max(1, tab->width);
                        tab->detach_h = (height > 0) ? height : std::max(1, tab->height);
                        tab->detach_state = BrowserTab::DetachState::Detaching;
                    }
                    if (!CameraViewportManager::instance().register_view(
                            scene_id, camera_id, camera_handle, tab_id)) {
                        browser_manager.remove_tab(tab_id);
                        return;
                    }
                    if (auto* tab = browser_manager.get_tab(tab_id)) {
                        tab->name = "Camera " + camera_id;
                    }
                });

            nlohmann::json result;
            result["queued"] = true;
            result["existing"] = false;
            send_dock_callback(frame, request_id, nullptr, result);
            return true;
        }

        if (cmd == "closeCameraView") {
            const std::string scene_id = command.value("sceneId", "");
            const std::string camera_id = command.value("cameraId", "");
            bool closed = false;
            if (auto existing = CameraViewportManager::instance().find_by_camera(
                    scene_id, camera_id)) {
                const int tab_id = existing->tab_id;
                bm.enqueue_main_thread_task([tab_id] {
                    BrowserManager::instance().remove_tab(tab_id);
                });
                closed = true;
            }
            nlohmann::json result;
            result["closed"] = closed;
            send_dock_callback(frame, request_id, nullptr, result);
            return true;
        }

        if (cmd == "toggleMaximizeThisCameraView") {
            const int tab_id = resolve_camera_tab_id(command, browser);
            bm.enqueue_main_thread_task([tab_id] {
                auto* tab = BrowserManager::instance().get_tab(tab_id);
                if (!tab || !tab->camera_view) {
                    CFW_LOG_WARNING("toggleMaximizeThisCameraView skipped: tab_id={}, tab={}, camera_view={}, window_id={}",
                                    tab_id, tab != nullptr, tab ? tab->camera_view : false,
                                    tab ? tab->platform_window_id : 0);
                    return;
                }
                SDL_Window* window = tab->platform_window_id != 0
                                         ? SDL_GetWindowFromID(tab->platform_window_id)
                                         : nullptr;
                if (!window) {
                    auto* hwnd = static_cast<HWND>(tab->platform_handle_raw);
                    if (!hwnd) {
                        CFW_LOG_WARNING("toggleMaximizeThisCameraView skipped: no SDL window or HWND, tab_id={}, window_id={}",
                                        tab_id, tab->platform_window_id);
                        return;
                    }
                if (IsZoomed(hwnd)) {
                    CFW_LOG_DEBUG("Restoring camera viewport HWND: tab_id={}, hwnd={}",
                                  tab_id, tab->platform_handle_raw);
                    if (!restore_windowed_placement(hwnd)) {
                        ShowWindow(hwnd, SW_RESTORE);
                    }
                } else {
                    CFW_LOG_DEBUG("Maximizing camera viewport HWND: tab_id={}, hwnd={}",
                                  tab_id, tab->platform_handle_raw);
                    save_windowed_placement(hwnd);
                    ShowWindow(hwnd, SW_MAXIMIZE);
                }
                    return;
                }
                const auto flags = SDL_GetWindowFlags(window);
                if ((flags & SDL_WINDOW_MAXIMIZED) != 0) {
                    CFW_LOG_DEBUG("Restoring camera viewport window: tab_id={}, window_id={}",
                                  tab_id, tab->platform_window_id);
                    SDL_RestoreWindow(window);
                } else {
                    CFW_LOG_DEBUG("Maximizing camera viewport window: tab_id={}, window_id={}",
                                  tab_id, tab->platform_window_id);
                    SDL_MaximizeWindow(window);
                }
            });

            nlohmann::json result;
            result["queued"] = true;
            send_dock_callback(frame, request_id, nullptr, result);
            return true;
        }

        if (cmd == "cycleThisCameraViewWindowMode") {
            const int tab_id = resolve_camera_tab_id(command, browser);
            bm.enqueue_main_thread_task([tab_id] {
                auto* tab = BrowserManager::instance().get_tab(tab_id);
                if (!tab || !tab->camera_view) {
                    CFW_LOG_WARNING("cycleThisCameraViewWindowMode skipped: tab_id={}, tab={}, camera_view={}, window_id={}",
                                    tab_id, tab != nullptr, tab ? tab->camera_view : false,
                                    tab ? tab->platform_window_id : 0);
                    return;
                }
                SDL_Window* window = tab->platform_window_id != 0
                                         ? SDL_GetWindowFromID(tab->platform_window_id)
                                         : nullptr;
                if (window) {
                    auto& state = s_camera_window_modes[tab_id];
                    if (state.mode == 2) {
                        CFW_LOG_DEBUG("Restoring camera viewport from borderless before window toggle: tab_id={}, window_id={}",
                                      tab_id, tab->platform_window_id);
                        SDL_SetWindowFullscreen(window, false);
                        SDL_SetWindowBordered(window, true);
                        SDL_RestoreWindow(window);
                        request_camera_window_rect(tab_id, tab, state.x, state.y, state.width, state.height);
                        state.mode = state.saved_maximized ? 1 : 0;
                        if (state.saved_maximized) {
                            SDL_MaximizeWindow(window);
                        }
                        return;
                    }
                    if (state.mode == 0 && !state.saved) {
                        SDL_GetWindowPosition(window, &state.x, &state.y);
                        SDL_GetWindowSize(window, &state.width, &state.height);
                        state.width = std::max(state.width, tab->dock_width);
                        state.height = std::max(state.height, tab->dock_height);
                        state.saved = true;
                        state.saved_maximized = false;
                    }

                    const SDL_DisplayID display_id = SDL_GetDisplayForWindow(window);
                    SDL_Rect usable{};
                    if (!SDL_GetDisplayUsableBounds(display_id, &usable)) {
                        usable = SDL_Rect{state.x, state.y, state.width, state.height};
                    }

                    if (state.mode == 1 || (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0) {
                        CFW_LOG_DEBUG("Restoring camera viewport window: tab_id={}, window_id={}",
                                      tab_id, tab->platform_window_id);
                        SDL_SetWindowBordered(window, true);
                        SDL_RestoreWindow(window);
                        request_camera_window_rect(tab_id, tab, state.x, state.y, state.width, state.height);
                        state.mode = 0;
                    } else {
                        CFW_LOG_DEBUG("Maximizing camera viewport window: tab_id={}, window_id={}",
                                      tab_id, tab->platform_window_id);
                        SDL_SetWindowFullscreen(window, false);
                        SDL_SetWindowBordered(window, true);
                        request_camera_window_rect(tab_id, tab, usable.x, usable.y, usable.w, usable.h);
                        SDL_MaximizeWindow(window);
                        state.mode = 1;
                    }
                    return;
                }

                auto* hwnd = static_cast<HWND>(tab->platform_handle_raw);
                if (!hwnd) {
                    CFW_LOG_WARNING("cycleThisCameraViewWindowMode skipped: no SDL window or HWND, tab_id={}, window_id={}",
                                    tab_id, tab->platform_window_id);
                    return;
                }

                const LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
                const bool borderless = (style & WS_OVERLAPPEDWINDOW) == 0;
                if (borderless) {
                    CFW_LOG_DEBUG("Restoring camera viewport HWND from borderless fallback: tab_id={}, hwnd={}",
                                  tab_id, tab->platform_handle_raw);
                    if (!restore_windowed_placement(hwnd)) {
                        SetWindowLongPtr(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
                        ShowWindow(hwnd, SW_RESTORE);
                    }
                    return;
                }
                if (IsZoomed(hwnd)) {
                    CFW_LOG_DEBUG("Restoring camera viewport HWND from maximized fallback: tab_id={}, hwnd={}",
                                  tab_id, tab->platform_handle_raw);
                    if (!restore_windowed_placement(hwnd)) {
                        ShowWindow(hwnd, SW_RESTORE);
                    }
                    request_camera_window_rect(tab_id, tab, tab->initial_x, tab->initial_y,
                                               tab->dock_width, tab->dock_height);
                    s_camera_window_modes[tab_id].mode = 0;
                    return;
                }

                auto& state = s_camera_window_modes[tab_id];
                state.mode = 1;
                state.saved = true;
                state.x = tab->initial_x;
                state.y = tab->initial_y;
                state.width = tab->dock_width;
                state.height = tab->dock_height;
                CFW_LOG_DEBUG("Maximizing camera viewport HWND fallback: tab_id={}, hwnd={}",
                              tab_id, tab->platform_handle_raw);
                save_windowed_placement(hwnd);
                ShowWindow(hwnd, SW_MAXIMIZE);
            });

            nlohmann::json result;
            result["queued"] = true;
            send_dock_callback(frame, request_id, nullptr, result);
            return true;
        }

        if (cmd == "toggleBorderlessThisCameraView") {
            const int tab_id = resolve_camera_tab_id(command, browser);
            bm.enqueue_main_thread_task([tab_id] {
                auto* tab = BrowserManager::instance().get_tab(tab_id);
                if (!tab || !tab->camera_view) {
                    CFW_LOG_WARNING("toggleBorderlessThisCameraView skipped: tab_id={}, tab={}, camera_view={}, window_id={}",
                                    tab_id, tab != nullptr, tab ? tab->camera_view : false,
                                    tab ? tab->platform_window_id : 0);
                    return;
                }

                SDL_Window* window = tab->platform_window_id != 0
                                         ? SDL_GetWindowFromID(tab->platform_window_id)
                                         : nullptr;
                auto& state = s_camera_window_modes[tab_id];
                if (window) {
                    if (state.mode == 2) {
                        CFW_LOG_DEBUG("Restoring camera viewport from borderless fullscreen: tab_id={}, window_id={}",
                                      tab_id, tab->platform_window_id);
                        SDL_SetWindowFullscreen(window, false);
                        SDL_SetWindowBordered(window, true);
                        SDL_RestoreWindow(window);
                        request_camera_window_rect(tab_id, tab, state.x, state.y, state.width, state.height);
                        if (state.saved_maximized) {
                            SDL_MaximizeWindow(window);
                            state.mode = 1;
                        } else {
                            state.mode = 0;
                        }
                        return;
                    }

                    SDL_GetWindowPosition(window, &state.x, &state.y);
                    SDL_GetWindowSize(window, &state.width, &state.height);
                    state.width = std::max(state.width, tab->dock_width);
                    state.height = std::max(state.height, tab->dock_height);
                    state.saved = true;
                    state.saved_maximized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;

                    const SDL_DisplayID display_id = SDL_GetDisplayForWindow(window);
                    SDL_Rect bounds{};
                    if (!SDL_GetDisplayBounds(display_id, &bounds)) {
                        bounds = SDL_Rect{state.x, state.y, state.width, state.height};
                    }

                    CFW_LOG_DEBUG("Setting camera viewport borderless fullscreen: tab_id={}, window_id={}, x={}, y={}, w={}, h={}",
                                  tab_id, tab->platform_window_id, bounds.x, bounds.y, bounds.w, bounds.h);
                    SDL_SetWindowFullscreen(window, false);
                    SDL_RestoreWindow(window);
                    SDL_SetWindowBordered(window, false);
                    request_camera_window_rect(tab_id, tab, bounds.x, bounds.y, bounds.w, bounds.h);
                    SDL_SetWindowPosition(window, bounds.x, bounds.y);
                    SDL_SetWindowSize(window, bounds.w, bounds.h);
                    SDL_RaiseWindow(window);
                    state.mode = 2;
                    return;
                }

                auto* hwnd = static_cast<HWND>(tab->platform_handle_raw);
                if (!hwnd) {
                    CFW_LOG_WARNING("toggleBorderlessThisCameraView skipped: no SDL window or HWND, tab_id={}, window_id={}",
                                    tab_id, tab->platform_window_id);
                    return;
                }

                const LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
                const bool borderless = (style & WS_OVERLAPPEDWINDOW) == 0;
                if (borderless || state.mode == 2) {
                    CFW_LOG_DEBUG("Restoring camera viewport HWND from borderless fullscreen: tab_id={}, hwnd={}",
                                  tab_id, tab->platform_handle_raw);
                    if (!restore_windowed_placement(hwnd)) {
                        SetWindowLongPtr(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
                        ShowWindow(hwnd, SW_RESTORE);
                    }
                    request_camera_window_rect(tab_id, tab, state.x, state.y, state.width, state.height);
                    state.mode = 0;
                    return;
                }

                RECT rect{};
                if (GetWindowRect(hwnd, &rect)) {
                    state.x = rect.left;
                    state.y = rect.top;
                    state.width = std::max(static_cast<int>(rect.right - rect.left), tab->dock_width);
                    state.height = std::max(static_cast<int>(rect.bottom - rect.top), tab->dock_height);
                    state.saved = true;
                }
                state.saved_maximized = IsZoomed(hwnd);
                save_windowed_placement(hwnd);

                HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO monitor_info{};
                monitor_info.cbSize = sizeof(monitor_info);
                RECT monitor_rect = rect;
                if (monitor && GetMonitorInfo(monitor, &monitor_info)) {
                    monitor_rect = monitor_info.rcMonitor;
                }
                const int x = monitor_rect.left;
                const int y = monitor_rect.top;
                const int width = monitor_rect.right - monitor_rect.left;
                const int height = monitor_rect.bottom - monitor_rect.top;
                CFW_LOG_DEBUG("Setting camera viewport HWND borderless fullscreen: tab_id={}, hwnd={}, x={}, y={}, w={}, h={}",
                              tab_id, tab->platform_handle_raw, x, y, width, height);
                SetWindowLongPtr(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
                SetWindowPos(hwnd, HWND_TOP, x, y, width, height,
                             SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                request_camera_window_rect(tab_id, tab, x, y, width, height);
                state.mode = 2;
            });

            nlohmann::json result;
            result["queued"] = true;
            send_dock_callback(frame, request_id, nullptr, result);
            return true;
        }

        if (cmd == "resizeThisCameraView") {
            const int tab_id = resolve_camera_tab_id(command, browser);
            const int width = std::max(command.value("width", 960), 64);
            const int height = std::max(command.value("height", 540), 64);
            bm.enqueue_main_thread_task([tab_id, width, height] {
                auto* tab = BrowserManager::instance().get_tab(tab_id);
                if (!tab || !tab->camera_view) {
                    CFW_LOG_WARNING("resizeThisCameraView skipped: tab_id={}, tab={}, camera_view={}, window_id={}",
                                    tab_id, tab != nullptr, tab ? tab->camera_view : false,
                                    tab ? tab->platform_window_id : 0);
                    return;
                }
                SDL_Window* window = tab->platform_window_id != 0
                                         ? SDL_GetWindowFromID(tab->platform_window_id)
                                         : nullptr;
                if (!window) {
                    auto* hwnd = static_cast<HWND>(tab->platform_handle_raw);
                    if (!hwnd) {
                        CFW_LOG_WARNING("resizeThisCameraView skipped: no SDL window or HWND, tab_id={}, window_id={}",
                                        tab_id, tab->platform_window_id);
                        return;
                    }
                    tab->dock_width = width;
                    tab->dock_height = height;
                    tab->needs_resize = true;
                    BrowserManager::instance().resize_tab(tab_id, width, height);
                    CFW_LOG_DEBUG("Resizing camera viewport HWND: tab_id={}, hwnd={}, size={}x{}",
                                  tab_id, tab->platform_handle_raw, width, height);
                    SetWindowPos(hwnd, nullptr, 0, 0, width, height,
                                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                    return;
                }
                CFW_LOG_DEBUG("Resizing camera viewport window: tab_id={}, window_id={}, size={}x{}",
                              tab_id, tab->platform_window_id, width, height);
                tab->dock_width = width;
                tab->dock_height = height;
                tab->needs_resize = true;
                BrowserManager::instance().resize_tab(tab_id, width, height);
                SDL_SetWindowSize(window, width, height);
            });

            nlohmann::json result;
            result["queued"] = true;
            result["width"] = width;
            result["height"] = height;
            send_dock_callback(frame, request_id, nullptr, result);
            return true;
        }

        if (cmd == "suspendCameraViews") {
            const std::string scene_id = command.value("sceneId", "");
            const auto tab_ids = CameraViewportManager::instance().tabs_for_scene(scene_id);
            bm.enqueue_main_thread_task([tab_ids, frame, request_id] {
                auto& browser_manager = BrowserManager::instance();
                for (const int tab_id : tab_ids) {
                    if (auto* tab = browser_manager.get_tab(tab_id)) {
                        tab->preserve_camera_open_on_close = true;
                    }
                    browser_manager.remove_tab(tab_id);
                }
                browser_manager.enqueue_main_thread_task([frame, request_id, closed = tab_ids.size()] {
                    nlohmann::json result;
                    result["closed"] = closed;
                    send_dock_callback(frame, request_id, nullptr, result);
                });
            });
            return true;
        }

        if (cmd == "createPanelTab") {
            std::string panel_id = command.value("panelId", "");
            std::string route = command.value("routePath", "");
            int width = command.value("width", 400);
            int height = command.value("height", 600);
            int z_priority = command.value("zPriority", 0);
            std::string docking_pos = command.value("dockingPos", "right_top");
            if (docking_pos != "right_top" &&
                docking_pos != "right_bottom" &&
                docking_pos != "left_bottom" &&
                docking_pos != "center") {
                docking_pos = "right_top";
            }

            if (!route.empty() && route[0] == '#') {
                route = route.substr(1);
            }
            std::string standalone_route = route;
            standalone_route += (standalone_route.find('?') == std::string::npos) ? "?standalone=1" : "&standalone=1";

            int tab_id = bm.create_tab(source_base_url(browser), standalone_route,
                                       docking_pos, width, height, false);
            // Phase 10: a popped-out panel is an in-main-window floating, draggable rectangle.
            // Seed an initial position from its anchor so multiple pop-outs don't stack at 0,0;
            // the user can then drag it by its title bar. Done on the UI thread.
            bm.enqueue_main_thread_task([tab_id, docking_pos, width, height, z_priority] {
                auto* tab = BrowserManager::instance().get_tab(tab_id);
                if (!tab) {
                    return;
                }
                tab->floating = true;
                tab->z_priority = z_priority;
                // Spread anchors a bit so default panels are visible and not overlapping.
                int ix = 80;
                int iy = 80;
                if (docking_pos == "right_top") { ix = 1100; iy = 80; }
                else if (docking_pos == "right_bottom") { ix = 1100; iy = 560; }
                else if (docking_pos == "left_bottom") { ix = 80; iy = 560; }
                else if (docking_pos == "center") { ix = 600; iy = 300; }
                tab->initial_x = ix;
                tab->initial_y = iy;
            });
            nlohmann::json result;
            result["tab_id"] = tab_id;
            result["panel_id"] = panel_id;
            send_dock_callback(frame, request_id, nullptr, result);
            return true;
        }

        if (cmd == "createDetachedPanel") {
            // Like createPanelTab, but the tab is born detached: created and immediately flagged
            // Detaching in the SAME main-thread task, so the frame runner's reconcile turns it
            // into its own borderless OS window THIS frame — it never renders as a main-window
            // rectangle, so there is no 1-frame flash. The DockTitleBar it renders (standalone
            // mode) reports drag regions, and reconcile registers the hit-test, so the window is
            // draggable by its title bar and resizable at its edges.
            std::string panel_id = command.value("panelId", "");
            std::string route = command.value("routePath", "");
            int width = command.value("width", 400);
            int height = command.value("height", 600);
            const int x = command.value("x", 120);
            const int y = command.value("y", 120);

            if (!route.empty() && route[0] == '#') {
                route = route.substr(1);
            }
            std::string standalone_route = route;
            standalone_route += (standalone_route.find('?') == std::string::npos) ? "?standalone=1" : "&standalone=1";

            const std::string base_url = source_base_url(browser);
            bm.enqueue_main_thread_task([base_url, standalone_route, panel_id, width, height, x, y,
                                         frame, request_id] {
                auto& bmgr = BrowserManager::instance();
                // docking_pos "right_top" is only a placeholder; the tab is detached below before
                // it can ever be laid out in the main window.
                int tab_id = bmgr.create_tab(base_url, standalone_route, "right_top",
                                             width, height, false);
                auto* tab = bmgr.get_tab(tab_id);
                if (tab) {
                    tab->detach_x = x;
                    tab->detach_y = y;
                    tab->detach_w = (width > 0) ? width : std::max(1, tab->width);
                    tab->detach_h = (height > 0) ? height : std::max(1, tab->height);
                    tab->detach_state = BrowserTab::DetachState::Detaching;
                    CFW_LOG_INFO("createDetachedPanel: tab {} created and marked Detaching (panel_id={})",
                                 tab_id, panel_id);
                } else {
                    CFW_LOG_ERROR("createDetachedPanel: tab {} was not found after create_tab (panel_id={})",
                                  tab_id, panel_id);
                }
                nlohmann::json result;
                result["tab_id"] = tab_id;
                result["panel_id"] = panel_id;
                send_dock_callback(frame, request_id, nullptr, result);
            });
            return true;
        }

        if (cmd == "closeThisTab") {
            std::string panel_id = command.value("panelId", "");
            nlohmann::json result;
            result["panel_id"] = panel_id;
            send_dock_callback(frame, request_id, nullptr, result);

            nlohmann::json payload;
            payload["panelId"] = panel_id;
            broadcast_dock_event("panel-closed", payload);

            int tab_id = find_tab_id_for_browser(browser);
            if (tab_id >= 0) {
                // Mark closed rather than removing directly: the frame loop's close path
                // tears down a detached tab's secondary OS window (promise-synced) BEFORE
                // destroying the tab. Direct remove_tab here would leak/credit-crash the window.
                bm.enqueue_main_thread_task([tab_id] {
                    if (auto* tab = BrowserManager::instance().get_tab(tab_id)) {
                        tab->open = false;
                    }
                });
            }
            return true;
        }

        if (cmd == "closePanelTab") {
            int tab_id = command.value("tabId", -1);
            std::string panel_id = command.value("panelId", "");
            if (tab_id >= 0) {
                bm.enqueue_main_thread_task([tab_id] {
                    if (auto* tab = BrowserManager::instance().get_tab(tab_id)) {
                        tab->open = false;
                    }
                });
            }

            nlohmann::json payload;
            payload["panelId"] = panel_id;
            broadcast_dock_event("panel-closed", payload);
            send_dock_callback(frame, request_id, nullptr, payload);
            return true;
        }

        if (cmd == "broadcast") {
            std::string event = command.value("event", "");
            nlohmann::json payload = command.value("payload", nlohmann::json::object());
            broadcast_dock_event(event, payload);
            send_dock_callback(frame, request_id, nullptr, event);
            return true;
        }

        if (cmd == "setDragRegions") {
            int tab_id = find_tab_id_for_browser(browser);
            if (command.contains("tabId") && command["tabId"].is_number_integer()) {
                tab_id = command.value("tabId", -1);
            }
            std::vector<DragRegion> regions;
            for (const auto& region : command.value("regions", nlohmann::json::array())) {
                regions.push_back({
                    region.value("x", 0.0f),
                    region.value("y", 0.0f),
                    region.value("w", 0.0f),
                    region.value("h", 0.0f),
                });
            }
            if (tab_id >= 0) {
                bm.set_tab_drag_regions(tab_id, regions);
            }
            nlohmann::json result;
            result["ok"] = tab_id >= 0;
            send_dock_callback(frame, request_id, nullptr, result);
            return true;
        }

        if (cmd == "detachPanel") {
            int tab_id = find_tab_id_for_browser(browser);
            if (command.contains("tabId") && command["tabId"].is_number_integer()) {
                tab_id = command.value("tabId", -1);
            }
            // Optional desired secondary-window geometry (logical px).
            const int x = command.value("x", 120);
            const int y = command.value("y", 120);
            const int w = command.value("width", 0);
            const int h = command.value("height", 0);
            const bool maximized = command.value("maximized", false);

            // Desired-state only: flip Docked -> Detaching on the UI thread. The frame runner's
            // reconcile step does the actual window create + surface register next frame. All
            // mutation of detach_state goes through enqueue_main_thread_task so the field stays
            // single-threaded (UI thread), needing no lock. See Phase 7d design notes.
            bm.enqueue_main_thread_task([tab_id, x, y, w, h, maximized] {
                auto* tab = BrowserManager::instance().get_tab(tab_id);
                if (!tab || tab->detach_state != BrowserTab::DetachState::Docked) {
                    return;  // unknown tab or mid-transition: reject (guards ABA / double-detach)
                }
                tab->detach_x = x;
                tab->detach_y = y;
                tab->detach_w = (w > 0) ? w : std::max(1, tab->width);
                tab->detach_h = (h > 0) ? h : std::max(1, tab->height);
                tab->detach_maximized = maximized;
                tab->detach_state = BrowserTab::DetachState::Detaching;
            });

            nlohmann::json result;
            result["queued"] = tab_id >= 0;
            send_dock_callback(frame, request_id, nullptr, result);
            return true;
        }

        if (cmd == "togglePanelWindowMode") {
            int tab_id = find_tab_id_for_browser(browser);
            if (command.contains("tabId") && command["tabId"].is_number_integer()) {
                tab_id = command.value("tabId", -1);
            }
            const int x = command.value("x", 120);
            const int y = command.value("y", 120);
            const int w = command.value("width", 0);
            const int h = command.value("height", 0);

            bm.enqueue_main_thread_task([tab_id, x, y, w, h] {
                auto* tab = BrowserManager::instance().get_tab(tab_id);
                if (!tab) {
                    return;
                }
                if (tab->detach_state == BrowserTab::DetachState::Docked) {
                    tab->detach_x = x;
                    tab->detach_y = y;
                    tab->detach_w = (w > 0) ? w : std::max(1, tab->width);
                    tab->detach_h = (h > 0) ? h : std::max(1, tab->height);
                    tab->detach_state = BrowserTab::DetachState::Detaching;
                    CFW_LOG_INFO("togglePanelWindowMode: tab {} -> Detaching", tab_id);
                } else if (tab->detach_state == BrowserTab::DetachState::Detached) {
                    tab->detach_state = BrowserTab::DetachState::Redocking;
                    CFW_LOG_INFO("togglePanelWindowMode: tab {} -> Redocking", tab_id);
                }
            });

            nlohmann::json result;
            result["queued"] = tab_id >= 0;
            send_dock_callback(frame, request_id, nullptr, result);
            return true;
        }

        if (cmd == "redockPanel") {
            int tab_id = find_tab_id_for_browser(browser);
            if (command.contains("tabId") && command["tabId"].is_number_integer()) {
                tab_id = command.value("tabId", -1);
            }
            // Desired-state only: flip Detached -> Redocking on the UI thread. The frame runner
            // tears the window down (promise-synced) next frame.
            bm.enqueue_main_thread_task([tab_id] {
                auto* tab = BrowserManager::instance().get_tab(tab_id);
                if (!tab || tab->detach_state != BrowserTab::DetachState::Detached) {
                    return;  // only a fully Detached panel can be redocked (guards ABA)
                }
                tab->detach_state = BrowserTab::DetachState::Redocking;
            });

            nlohmann::json result;
            result["queued"] = tab_id >= 0;
            send_dock_callback(frame, request_id, nullptr, result);
            return true;
        }

        nlohmann::json error;
        error["message"] = "Unknown DockCommand: " + cmd;
        send_dock_callback(frame, request_id, error, nullptr);
        return true;
    } catch (const std::exception& e) {
        nlohmann::json error;
        error["message"] = e.what();
        send_dock_callback(frame, request_id, error, nullptr);
        return true;
    }
}

}  // namespace

void drain_actor_pick_completion_events() {
    drain_actor_pick_completion_events_impl();
}

bool handle_realtime_process_message(CefRefPtr<CefBrowser> browser,
                                     CefRefPtr<CefFrame> frame,
                                     const CefRefPtr<CefProcessMessage>& message) {
    if (!message) {
        return false;
    }

    if (message->GetName() == "DockCommand") {
        return handle_dock_command(browser, frame, message);
    }

    if (message->GetName() == "CameraMoveFast") {
        return handle_camera_move_fast(message);
    }

    if (message->GetName() == "CameraViewportFast") {
        return handle_camera_viewport_fast(message);
    }

    if (message->GetName() == "ComputeActorFocusPoseFast") {
        return handle_compute_actor_focus_pose_fast(frame, message);
    }

    if (message->GetName() == "ActorTransformFast") {
        return handle_actor_transform_fast(message);
    }

    if (message->GetName() == "PropertyFast") {
        return handle_property_fast(message);
    }

    if (message->GetName() == "ViewportPick") {
        return handle_viewport_pick(frame, message);
    }

    if (message->GetName() == "ViewportGizmoTarget") {
        return handle_viewport_gizmo_target(message);
    }

    if (message->GetName() == "ViewportGizmoPointer") {
        return handle_viewport_gizmo_pointer(message);
    }

    if (message->GetName() == "ViewportUiMode") {
        return handle_viewport_ui_mode(message);
    }

    if (message->GetName() == "ViewportUiPointer") {
        return handle_viewport_ui_pointer(message);
    }

    if (message->GetName() == "ViewportSystemCursor") {
        return handle_viewport_system_cursor(browser, message);
    }

    if (message->GetName() == "ViewportUiCalibration") {
        return handle_viewport_ui_calibration(message);
    }

    if (message->GetName() == "InputInject") {
        return handle_input_inject(message);
    }

    return false;
}

bool forward_process_message_to_router(const CefRefPtr<CefMessageRouterBrowserSide>& browser_side_router,
                                       CefRefPtr<CefBrowser> browser,
                                       CefRefPtr<CefFrame> frame,
                                       CefProcessId source_process,
                                       CefRefPtr<CefProcessMessage> message) {
    if (!browser_side_router) {
        return false;
    }

    return browser_side_router->OnProcessMessageReceived(browser, frame, source_process, message);
}

}  // namespace Corona::Systems::UI
