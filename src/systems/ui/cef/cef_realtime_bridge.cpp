#include "cef_bridge_helpers.h"

#include <corona/kernel/core/i_logger.h>
#include <corona/resource/resource_manager.h>
#include <corona/resource/types/scene.h>
#include <corona/shared_data_hub.h>
#include <corona/systems/script/corona_engine_api.h>
#include <corona/systems/ui/camera_viewport_manager.h>
#include <corona/systems/ui/viewport_gizmo_manager.h>
#include <include/cef_values.h>
#include <nlohmann/json.hpp>
#include <SDL3/SDL.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "browser_manager.h"
#include "cef_client.h"

namespace Corona::Systems::UI {

namespace {

static std::mutex s_input_mutex;
static std::vector<InputEvent> s_input_queue;

struct GizmoDragSession {
    std::uintptr_t camera_handle{0};
    std::uintptr_t actor_handle{0};
    std::uintptr_t transform_handle{0};
    std::string scene_id;
    std::string request_id;
    std::string drag_id;
    std::string mode;
    std::string axis;
    ktm::fvec3 center{};
    ktm::fvec3 axis_vector{};
    ktm::fvec3 start_rotate_vector{};
    ktm::fvec3 start_position{};
    ktm::fvec3 start_rotation{};
    ktm::fvec3 start_scale{};
    ktm::fvec3 start_bounds_center_world{};
    ktm::fvec3 local_bounds_min{};
    ktm::fvec3 local_bounds_max{};
    double start_axis_t{0.0};
    double start_x{0.0};
    double start_y{0.0};
    bool has_local_bounds{false};
};

static std::mutex s_gizmo_drag_mutex;
static std::unordered_map<std::string, GizmoDragSession> s_gizmo_drags;

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

void append_geometry_handle(std::vector<std::uintptr_t>& handles, std::uintptr_t geometry_handle) {
    if (geometry_handle == 0) {
        return;
    }
    if (std::find(handles.begin(), handles.end(), geometry_handle) == handles.end()) {
        handles.push_back(geometry_handle);
    }
}

std::vector<std::uintptr_t> resolve_profile_handles(std::uintptr_t actor_handle) {
    std::vector<std::uintptr_t> profile_handles;
    if (auto actor = Corona::SharedDataHub::instance().actor_storage().try_acquire_read(actor_handle)) {
        profile_handles = actor->profile_handles;
    }
    return profile_handles;
}

std::vector<std::uintptr_t> resolve_actor_geometry_handles(std::uintptr_t actor_handle) {
    std::vector<std::uintptr_t> profile_handles;
    if (auto actor = Corona::SharedDataHub::instance().actor_storage().try_acquire_read(actor_handle)) {
        profile_handles = actor->profile_handles;
    }

    std::vector<std::uintptr_t> geometry_handles;
    auto& hub = Corona::SharedDataHub::instance();
    for (const auto profile_handle : profile_handles) {
        if (auto profile = hub.profile_storage().try_acquire_read(profile_handle)) {
            append_geometry_handle(geometry_handles, profile->geometry_handle);
            if (auto mechanics = hub.mechanics_storage().try_acquire_read(profile->mechanics_handle)) {
                append_geometry_handle(geometry_handles, mechanics->geometry_handle);
            }
            if (auto optics = hub.optics_storage().try_acquire_read(profile->optics_handle)) {
                append_geometry_handle(geometry_handles, optics->geometry_handle);
            }
            if (auto acoustics = hub.acoustics_storage().try_acquire_read(profile->acoustics_handle)) {
                append_geometry_handle(geometry_handles, acoustics->geometry_handle);
            }
        }
    }

    return geometry_handles;
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
    if (!frame || request_id.empty()) {
        return;
    }

    const std::string js = "window.__coronaFocusPoseResult&&window.__coronaFocusPoseResult(" +
                           nlohmann::json(request_id).dump() + "," + payload.dump() + ")";
    frame->ExecuteJavaScript(js, "", 0);
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

[[nodiscard]] ktm::fvec3 add_vec3(const ktm::fvec3& a, const ktm::fvec3& b) {
    return make_fvec3(a[0] + b[0], a[1] + b[1], a[2] + b[2]);
}

[[nodiscard]] ktm::fvec3 sub_vec3(const ktm::fvec3& a, const ktm::fvec3& b) {
    return make_fvec3(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

[[nodiscard]] ktm::fvec3 mul_vec3(const ktm::fvec3& v, float scale) {
    return make_fvec3(v[0] * scale, v[1] * scale, v[2] * scale);
}

[[nodiscard]] float dot_vec3(const ktm::fvec3& a, const ktm::fvec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

[[nodiscard]] ktm::fvec3 cross_vec3(const ktm::fvec3& a, const ktm::fvec3& b) {
    return make_fvec3(a[1] * b[2] - a[2] * b[1],
                      a[2] * b[0] - a[0] * b[2],
                      a[0] * b[1] - a[1] * b[0]);
}

[[nodiscard]] float length_vec3(const ktm::fvec3& v) {
    return std::sqrt(dot_vec3(v, v));
}

[[nodiscard]] ktm::fvec3 normalize_vec3(
    const ktm::fvec3& v,
    const ktm::fvec3& fallback = make_fvec3(0.0f, 0.0f, 1.0f)) {
    const float length = length_vec3(v);
    return length > 1.0e-6f ? mul_vec3(v, 1.0f / length) : fallback;
}

nlohmann::json json_vec3(const ktm::fvec3& value) {
    return {value[0], value[1], value[2]};
}

struct GizmoCameraSnapshot {
    ktm::fvec3 position{};
    ktm::fvec3 forward{};
    ktm::fvec3 up{};
    ktm::fvec3 right{};
    float fov{45.0f};
    double viewport_width{0.0};
    double viewport_height{0.0};
};

struct GizmoActorSnapshot {
    std::uintptr_t transform_handle{0};
    bool has_local_bounds{false};
    ktm::fvec3 position{};
    ktm::fvec3 rotation{};
    ktm::fvec3 scale{};
    ktm::fvec3 local_bounds_min{};
    ktm::fvec3 local_bounds_max{};
    ktm::fvec3 bounds_center_world{};
};

struct GizmoActorSource {
    std::uintptr_t transform_handle{0};
    std::uintptr_t model_resource_handle{0};
    bool has_local_bounds{false};
    ktm::fvec3 local_bounds_min{};
    ktm::fvec3 local_bounds_max{};
};

bool fill_model_resource_bounds_nowait(std::uintptr_t model_resource_handle,
                                       ktm::fvec3& out_min,
                                       ktm::fvec3& out_max) {
    if (model_resource_handle == 0) {
        return false;
    }

    std::uint64_t model_id = 0;
    auto& hub = Corona::SharedDataHub::instance();
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
    out_min = make_fvec3(aabb.min[0], aabb.min[1], aabb.min[2]);
    out_max = make_fvec3(aabb.max[0], aabb.max[1], aabb.max[2]);
    return true;
}

ktm::fvec3 compute_bounds_center_world(const ktm::fvec3& position,
                                       const ktm::fvec3& rotation,
                                       const ktm::fvec3& scale,
                                       const ktm::fvec3& local_min,
                                       const ktm::fvec3& local_max) {
    Corona::ModelTransform transform;
    transform.position = position;
    transform.euler_rotation = rotation;
    transform.scale = scale;
    const auto local_center = make_fvec3((local_min[0] + local_max[0]) * 0.5f,
                                         (local_min[1] + local_max[1]) * 0.5f,
                                         (local_min[2] + local_max[2]) * 0.5f);
    return transform_local_point_to_world(transform.compute_matrix(), local_center);
}

GizmoActorSource resolve_actor_gizmo_source_nowait(std::uintptr_t actor_handle) {
    struct GeometryCandidate {
        std::uintptr_t geometry_handle{0};
        bool has_local_bounds{false};
        ktm::fvec3 local_bounds_min{};
        ktm::fvec3 local_bounds_max{};
    };

    auto append_candidate = [](std::vector<GeometryCandidate>& candidates,
                               std::uintptr_t geometry_handle,
                               bool has_local_bounds,
                               const ktm::fvec3& local_min,
                               const ktm::fvec3& local_max) {
        if (geometry_handle == 0) {
            return;
        }
        for (auto& candidate : candidates) {
            if (candidate.geometry_handle != geometry_handle) {
                continue;
            }
            if (!candidate.has_local_bounds && has_local_bounds) {
                candidate.has_local_bounds = true;
                candidate.local_bounds_min = local_min;
                candidate.local_bounds_max = local_max;
            }
            return;
        }
        GeometryCandidate candidate;
        candidate.geometry_handle = geometry_handle;
        candidate.has_local_bounds = has_local_bounds;
        candidate.local_bounds_min = local_min;
        candidate.local_bounds_max = local_max;
        candidates.push_back(candidate);
    };

    std::vector<std::uintptr_t> profile_handles;
    auto& hub = Corona::SharedDataHub::instance();
    if (auto actor = hub.actor_storage().try_acquire_read_nowait(actor_handle)) {
        profile_handles = actor->profile_handles;
    }

    std::vector<GeometryCandidate> candidates;
    const auto zero = make_fvec3(0.0f, 0.0f, 0.0f);
    for (const auto profile_handle : profile_handles) {
        std::uintptr_t profile_geometry = 0;
        std::uintptr_t mechanics_handle = 0;
        std::uintptr_t optics_handle = 0;
        std::uintptr_t acoustics_handle = 0;
        if (auto profile = hub.profile_storage().try_acquire_read_nowait(profile_handle)) {
            profile_geometry = profile->geometry_handle;
            mechanics_handle = profile->mechanics_handle;
            optics_handle = profile->optics_handle;
            acoustics_handle = profile->acoustics_handle;
        }
        append_candidate(candidates, profile_geometry, false, zero, zero);
        if (auto mechanics = hub.mechanics_storage().try_acquire_read_nowait(mechanics_handle)) {
            append_candidate(candidates,
                             mechanics->geometry_handle,
                             true,
                             mechanics->min_xyz,
                             mechanics->max_xyz);
        }
        if (auto optics = hub.optics_storage().try_acquire_read_nowait(optics_handle)) {
            append_candidate(candidates, optics->geometry_handle, false, zero, zero);
        }
        if (auto acoustics = hub.acoustics_storage().try_acquire_read_nowait(acoustics_handle)) {
            append_candidate(candidates, acoustics->geometry_handle, false, zero, zero);
        }
    }

    for (const auto& candidate : candidates) {
        GizmoActorSource source;
        source.has_local_bounds = candidate.has_local_bounds;
        source.local_bounds_min = candidate.local_bounds_min;
        source.local_bounds_max = candidate.local_bounds_max;
        if (auto geometry = hub.geometry_storage().try_acquire_read_nowait(candidate.geometry_handle)) {
            source.transform_handle = geometry->transform_handle;
            source.model_resource_handle = geometry->model_resource_handle;
        }
        if (source.transform_handle == 0) {
            continue;
        }
        if (!source.has_local_bounds) {
            source.has_local_bounds = fill_model_resource_bounds_nowait(source.model_resource_handle,
                                                                        source.local_bounds_min,
                                                                        source.local_bounds_max);
        }
        return source;
    }
    return {};
}

bool read_gizmo_actor_snapshot(std::uintptr_t actor_handle, GizmoActorSnapshot& out) {
    auto& hub = Corona::SharedDataHub::instance();
    const auto source = resolve_actor_gizmo_source_nowait(actor_handle);
    out.transform_handle = source.transform_handle;
    out.has_local_bounds = source.has_local_bounds;
    out.local_bounds_min = source.local_bounds_min;
    out.local_bounds_max = source.local_bounds_max;
    if (out.transform_handle == 0) {
        return false;
    }
    if (auto transform = hub.model_transform_storage().try_acquire_read_nowait(out.transform_handle)) {
        out.position = transform->position;
        out.rotation = transform->euler_rotation;
        out.scale = transform->scale;
        out.bounds_center_world = out.has_local_bounds
            ? compute_bounds_center_world(out.position,
                                          out.rotation,
                                          out.scale,
                                          out.local_bounds_min,
                                          out.local_bounds_max)
            : out.position;
        if (!out.has_local_bounds) {
            CFW_LOG_DEBUG("ActorGizmo: local bounds unavailable for actor={}, using transform position as scale center",
                          actor_handle);
        }
        return true;
    }
    return false;
}

bool read_gizmo_camera_snapshot(std::uintptr_t camera_handle,
                                double viewport_width,
                                double viewport_height,
                                GizmoCameraSnapshot& out) {
    if (viewport_width <= 0.0 || viewport_height <= 0.0) {
        return false;
    }

    if (auto camera = Corona::SharedDataHub::instance().camera_storage().try_acquire_read_nowait(camera_handle)) {
        out.position = camera->position;
        out.forward = normalize_vec3(camera->forward);
        out.up = normalize_vec3(camera->world_up, make_fvec3(0.0f, 1.0f, 0.0f));
        out.right = normalize_vec3(cross_vec3(out.up, out.forward), make_fvec3(1.0f, 0.0f, 0.0f));
        out.up = normalize_vec3(cross_vec3(out.forward, out.right), make_fvec3(0.0f, 1.0f, 0.0f));
        out.fov = camera->fov;
        out.viewport_width = viewport_width;
        out.viewport_height = viewport_height;
        return true;
    }
    return false;
}

bool project_world_to_screen(const GizmoCameraSnapshot& camera,
                             const ktm::fvec3& world,
                             std::array<double, 2>& out) {
    const auto rel = sub_vec3(world, camera.position);
    const float view_z = dot_vec3(rel, camera.forward);
    if (view_z <= 1.0e-4f) {
        return false;
    }
    const float view_x = dot_vec3(rel, camera.right);
    const float view_y = dot_vec3(rel, camera.up);
    const double half_tan = std::tan(static_cast<double>(camera.fov) * 3.14159265358979323846 / 360.0);
    const double aspect = camera.viewport_width / camera.viewport_height;
    if (half_tan <= 0.0 || aspect <= 0.0) {
        return false;
    }
    const double ndc_x = static_cast<double>(view_x) / (static_cast<double>(view_z) * half_tan * aspect);
    const double ndc_y = static_cast<double>(view_y) / (static_cast<double>(view_z) * half_tan);
    out = {
        (ndc_x * 0.5 + 0.5) * camera.viewport_width,
        (0.5 - ndc_y * 0.5) * camera.viewport_height,
    };
    return std::isfinite(out[0]) && std::isfinite(out[1]);
}

ktm::fvec3 make_gizmo_axis_vector(const std::string& axis) {
    if (axis == "x") return make_fvec3(1.0f, 0.0f, 0.0f);
    if (axis == "y") return make_fvec3(0.0f, 1.0f, 0.0f);
    if (axis == "z") return make_fvec3(0.0f, 0.0f, 1.0f);
    return make_fvec3(0.0f, 0.0f, 0.0f);
}

bool make_gizmo_ring_basis(const std::string& axis,
                           ktm::fvec3& out_u,
                           ktm::fvec3& out_v) {
    if (axis == "x") {
        out_u = make_fvec3(0.0f, 1.0f, 0.0f);
        out_v = make_fvec3(0.0f, 0.0f, 1.0f);
        return true;
    }
    if (axis == "y") {
        out_u = make_fvec3(0.0f, 0.0f, 1.0f);
        out_v = make_fvec3(1.0f, 0.0f, 0.0f);
        return true;
    }
    if (axis == "z") {
        out_u = make_fvec3(1.0f, 0.0f, 0.0f);
        out_v = make_fvec3(0.0f, 1.0f, 0.0f);
        return true;
    }
    return false;
}

ktm::fvec3 screen_ray_direction(const GizmoCameraSnapshot& camera, double x, double y) {
    const double nx = (x / camera.viewport_width) * 2.0 - 1.0;
    const double sy = 1.0 - (y / camera.viewport_height) * 2.0;
    const double half_tan = std::tan(static_cast<double>(camera.fov) * 3.14159265358979323846 / 360.0);
    const double aspect = camera.viewport_width / camera.viewport_height;
    auto dir = add_vec3(camera.forward,
                        add_vec3(mul_vec3(camera.right, static_cast<float>(nx * half_tan * aspect)),
                                 mul_vec3(camera.up, static_cast<float>(sy * half_tan))));
    return normalize_vec3(dir);
}

bool intersect_axis_drag_plane(const GizmoCameraSnapshot& camera,
                               const ktm::fvec3& center,
                               const ktm::fvec3& axis_vector,
                               double x,
                               double y,
                               double& out_axis_t) {
    const auto ray_dir = screen_ray_direction(camera, x, y);
    auto plane_normal = cross_vec3(axis_vector, cross_vec3(camera.forward, axis_vector));
    if (length_vec3(plane_normal) <= 1.0e-5f) {
        plane_normal = cross_vec3(axis_vector, camera.up);
    }
    plane_normal = normalize_vec3(plane_normal, camera.forward);
    const float denom = dot_vec3(ray_dir, plane_normal);
    if (std::abs(denom) <= 1.0e-5f) {
        return false;
    }
    const float t = dot_vec3(sub_vec3(center, camera.position), plane_normal) / denom;
    const auto hit = add_vec3(camera.position, mul_vec3(ray_dir, t));
    out_axis_t = dot_vec3(sub_vec3(hit, center), axis_vector);
    return std::isfinite(out_axis_t);
}

bool intersect_rotation_plane(const GizmoCameraSnapshot& camera,
                              const ktm::fvec3& center,
                              const ktm::fvec3& axis_vector,
                              double x,
                              double y,
                              ktm::fvec3& out_vector) {
    const auto ray_dir = screen_ray_direction(camera, x, y);
    const float denom = dot_vec3(ray_dir, axis_vector);
    if (std::abs(denom) <= 1.0e-5f) {
        return false;
    }
    const float t = dot_vec3(sub_vec3(center, camera.position), axis_vector) / denom;
    if (t <= 0.0f || !std::isfinite(t)) {
        return false;
    }
    const auto hit = add_vec3(camera.position, mul_vec3(ray_dir, t));
    const auto relative = sub_vec3(hit, center);
    const auto projected = sub_vec3(relative, mul_vec3(axis_vector, dot_vec3(relative, axis_vector)));
    if (length_vec3(projected) <= 1.0e-5f) {
        return false;
    }
    out_vector = normalize_vec3(projected);
    return true;
}

float signed_rotation_delta(const ktm::fvec3& start_vector,
                            const ktm::fvec3& current_vector,
                            const ktm::fvec3& axis_vector) {
    const float sin_value = dot_vec3(cross_vec3(start_vector, current_vector), axis_vector);
    const float cos_value = dot_vec3(start_vector, current_vector);
    return static_cast<float>(std::atan2(static_cast<double>(sin_value),
                                         static_cast<double>(cos_value)));
}

nlohmann::json build_gizmo_rings(const GizmoCameraSnapshot& camera,
                                 const ktm::fvec3& center,
                                 float radius) {
    nlohmann::json rings;
    constexpr int kSegments = 48;
    constexpr double kPi = 3.14159265358979323846;
    for (const std::string axis_name : {"x", "y", "z"}) {
        ktm::fvec3 u{};
        ktm::fvec3 v{};
        if (!make_gizmo_ring_basis(axis_name, u, v)) {
            continue;
        }

        nlohmann::json points = nlohmann::json::array();
        bool projectable = true;
        for (int i = 0; i < kSegments; ++i) {
            const double angle = (static_cast<double>(i) / static_cast<double>(kSegments)) * kPi * 2.0;
            const auto world = add_vec3(center,
                                        add_vec3(mul_vec3(u, static_cast<float>(std::cos(angle) * radius)),
                                                 mul_vec3(v, static_cast<float>(std::sin(angle) * radius))));
            std::array<double, 2> screen{};
            if (!project_world_to_screen(camera, world, screen)) {
                projectable = false;
                break;
            }
            points.push_back({screen[0], screen[1]});
        }
        if (projectable && points.size() >= 3) {
            rings[axis_name]["points"] = points;
        }
    }
    return rings;
}

void send_gizmo_event(const CefRefPtr<CefFrame>& frame,
                      const char* event_name,
                      const nlohmann::json& payload) {
    if (!frame) {
        return;
    }
    const std::string js = "window.__coronaEmit&&window.__coronaEmit(" +
                           nlohmann::json(event_name).dump() + "," + payload.dump() + ")";
    frame->ExecuteJavaScript(js, "", 0);
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
    for (const auto geometry_handle : resolve_actor_geometry_handles(actor_handle)) {
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
    if (!frame) {
        return;
    }

    const std::string js = "window.__coronaEmit&&window.__coronaEmit(\"actor-pick-result\"," +
                           payload.dump() + ")";
    frame->ExecuteJavaScript(js, "", 0);
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

    auto emit = [&](const std::string& status,
                    std::uintptr_t actor_handle,
                    std::uint32_t result_x,
                    std::uint32_t result_y,
                    const char* message_text = nullptr) {
        nlohmann::json payload;
        payload["status"] = status;
        payload["sceneId"] = scene_id;
        payload["cameraHandle"] = static_cast<std::uint64_t>(camera_handle);
        payload["requestId"] = request_id;
        payload["actorHandle"] = static_cast<std::uint64_t>(actor_handle);
        payload["x"] = result_x;
        payload["y"] = result_y;
        if (message_text) {
            payload["message"] = message_text;
        }
        send_viewport_pick_result(frame, payload);
    };

    if (camera_handle == 0 || request_id.empty() || scene_id.empty() ||
        vp_w <= 0.0 || vp_h <= 0.0 || !std::isfinite(x) || !std::isfinite(y)) {
        CFW_LOG_WARNING("ViewportPick: invalid params (camera={}, scene='{}', request='{}', vp={}x{})",
                        camera_handle, scene_id, request_id, vp_w, vp_h);
        emit("error", 0, 0, 0, "invalid params");
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
        emit("error", 0, 0, 0, "camera unavailable");
        return true;
    }

    const double scaled_x = x * cam_w / vp_w;
    const double scaled_y = y * cam_h / vp_h;
    if (scaled_x < 0.0 || scaled_y < 0.0 ||
        scaled_x >= cam_w || scaled_y >= cam_h) {
        CFW_LOG_DEBUG("ViewportPick miss: camera={} request={} pos=({},{}) -> scaled=({},{})",
                      camera_handle, request_id, x, y, scaled_x, scaled_y);
        emit("miss", 0, 0, 0);
        return true;
    }

    const auto pick_x = static_cast<std::uint32_t>(scaled_x);
    const auto pick_y = static_cast<std::uint32_t>(scaled_y);

    if (actor_pick_handle == 0) {
        CFW_LOG_WARNING("ViewportPick: camera {} has no actor pick storage", camera_handle);
        emit("error", 0, pick_x, pick_y, "actor pick unavailable");
        return true;
    }

    auto pick = hub.actor_pick_storage().try_acquire_write(actor_pick_handle);
    if (!pick) {
        CFW_LOG_WARNING("ViewportPick: pick storage {} is unavailable", actor_pick_handle);
        emit("error", 0, pick_x, pick_y, "actor pick storage unavailable");
        return true;
    }

    if (pick->result_ready && pick->result_request_id == request_id &&
        pick->result_x == pick_x && pick->result_y == pick_y) {
        const auto picked_actor = pick->actor_handle;
        emit(picked_actor != 0 ? "success" : "miss", picked_actor, pick_x, pick_y);
        CFW_LOG_DEBUG("ViewportPick result: camera={} request={} cam_px=({},{}) -> handle=0x{:x}",
                      camera_handle, request_id, pick_x, pick_y, picked_actor);
        return true;
    }

    if (pick->request_id == request_id &&
        pick->x == pick_x &&
        pick->y == pick_y &&
        !pick->result_ready) {
        emit("pending", 0, pick_x, pick_y);
        return true;
    }

    pick->request_id = request_id;
    pick->x = pick_x;
    pick->y = pick_y;
    pick->pending = true;
    pick->result_ready = false;
    emit("pending", 0, pick_x, pick_y);
    CFW_LOG_DEBUG("ViewportPick pending: camera={} scene='{}' request={} pos=({},{}) -> cam_px=({},{})",
                  camera_handle, scene_id, request_id, x, y, pick_x, pick_y);

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

bool handle_viewport_gizmo_mode(const CefRefPtr<CefProcessMessage>& message) {
    auto args = message->GetArgumentList();
    if (!args || args->GetSize() < 1 || args->GetType(0) != VTYPE_STRING) {
        CFW_LOG_WARNING("ViewportGizmoMode dropped: expected (mode)");
        return true;
    }

    const std::string mode = args->GetString(0).ToString();
    ViewportGizmoManager::instance().set_mode(mode);
    CFW_LOG_DEBUG("ViewportGizmoMode set: mode={}", mode);
    return true;
}

bool handle_viewport_gizmo_selection(const CefRefPtr<CefProcessMessage>& message) {
    auto args = message->GetArgumentList();
    double camera_value = 0.0;
    double actor_value = 0.0;
    if (!args || args->GetSize() < 3 ||
        args->GetType(0) != VTYPE_STRING ||
        !get_numeric_arg(args, 1, camera_value) ||
        !get_numeric_arg(args, 2, actor_value)) {
        CFW_LOG_WARNING("ViewportGizmoSelection dropped: expected (sceneId, cameraHandle, actorHandle)");
        return true;
    }

    const std::string scene_id = args->GetString(0).ToString();
    const auto camera_handle = static_cast<std::uintptr_t>(camera_value);
    const auto actor_handle = static_cast<std::uintptr_t>(actor_value);
    if (camera_handle == 0) {
        CFW_LOG_WARNING("ViewportGizmoSelection dropped: camera handle is 0");
        return true;
    }

    ViewportGizmoManager::instance().set_selection(scene_id, camera_handle, actor_handle);
    CFW_LOG_DEBUG("ViewportGizmoSelection set: scene='{}' camera={} actor={}",
                  scene_id,
                  camera_handle,
                  actor_handle);
    return true;
}

bool handle_viewport_gizmo_clear_selection(const CefRefPtr<CefProcessMessage>& message) {
    auto args = message->GetArgumentList();
    double camera_value = 0.0;
    if (args && args->GetSize() >= 1 && get_numeric_arg(args, 0, camera_value) &&
        static_cast<std::uintptr_t>(camera_value) != 0) {
        ViewportGizmoManager::instance().clear_camera(static_cast<std::uintptr_t>(camera_value));
    } else {
        ViewportGizmoManager::instance().clear_selection();
    }
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

bool handle_actor_gizmo_state(const CefRefPtr<CefFrame>& frame,
                              const CefRefPtr<CefProcessMessage>& message) {
    auto args = message->GetArgumentList();
    double camera_value = 0.0;
    double actor_value = 0.0;
    double vp_w = 0.0;
    double vp_h = 0.0;
    if (!args || args->GetSize() < 6 ||
        !get_numeric_arg(args, 0, camera_value) ||
        args->GetType(1) != VTYPE_STRING ||
        !get_numeric_arg(args, 2, actor_value) ||
        args->GetType(3) != VTYPE_STRING ||
        !get_numeric_arg(args, 4, vp_w) ||
        !get_numeric_arg(args, 5, vp_h)) {
        CFW_LOG_WARNING("ActorGizmoState dropped: invalid args");
        return true;
    }

    const auto camera_handle = static_cast<std::uintptr_t>(camera_value);
    const std::string scene_id = args->GetString(1).ToString();
    const auto actor_handle = static_cast<std::uintptr_t>(actor_value);
    const std::string request_id = args->GetString(3).ToString();

    auto make_payload = [&](const std::string& status, const char* message_text = nullptr) {
        nlohmann::json payload;
        payload["status"] = status;
        payload["sceneId"] = scene_id;
        payload["cameraHandle"] = static_cast<std::uint64_t>(camera_handle);
        payload["actorHandle"] = static_cast<std::uint64_t>(actor_handle);
        payload["requestId"] = request_id;
        if (message_text) payload["message"] = message_text;
        return payload;
    };

    if (camera_handle == 0 || actor_handle == 0 || scene_id.empty() || request_id.empty() ||
        vp_w <= 0.0 || vp_h <= 0.0 || !std::isfinite(vp_w) || !std::isfinite(vp_h)) {
        send_gizmo_event(frame, "actor-gizmo-state", make_payload("error", "invalid params"));
        return true;
    }

    GizmoCameraSnapshot camera;
    if (!read_gizmo_camera_snapshot(camera_handle, vp_w, vp_h, camera)) {
        send_gizmo_event(frame, "actor-gizmo-state", make_payload("error", "camera unavailable"));
        return true;
    }

    GizmoActorSnapshot actor;
    if (!read_gizmo_actor_snapshot(actor_handle, actor)) {
        send_gizmo_event(frame, "actor-gizmo-state", make_payload("error", "actor transform unavailable"));
        return true;
    }

    const auto gizmo_center_world = actor.bounds_center_world;
    std::array<double, 2> center_screen{};
    if (!project_world_to_screen(camera, gizmo_center_world, center_screen)) {
        auto payload = make_payload("error", "actor is not projectable");
        payload["visible"] = false;
        send_gizmo_event(frame, "actor-gizmo-state", payload);
        return true;
    }

    const float axis_length = std::max(0.75f, length_vec3(actor.scale) * 0.35f);
    const float ring_radius = std::max(axis_length * 0.82f, 0.6f);
    nlohmann::json axes;
    for (const std::string axis_name : {"x", "y", "z"}) {
        const auto axis = make_gizmo_axis_vector(axis_name);
        const auto end_world = add_vec3(gizmo_center_world, mul_vec3(axis, axis_length));
        std::array<double, 2> end_screen{};
        if (!project_world_to_screen(camera, end_world, end_screen)) {
            continue;
        }
        axes[axis_name]["screenStart"] = {center_screen[0], center_screen[1]};
        axes[axis_name]["screenEnd"] = {end_screen[0], end_screen[1]};
    }

    auto payload = make_payload("success");
    payload["visible"] = true;
    payload["center"]["world"] = json_vec3(gizmo_center_world);
    payload["center"]["screen"] = {center_screen[0], center_screen[1]};
    payload["scaleCenter"]["world"] = json_vec3(gizmo_center_world);
    payload["scaleCenter"]["screen"] = {center_screen[0], center_screen[1]};
    payload["axes"] = axes;
    payload["scaleAxes"] = axes;
    payload["rings"] = build_gizmo_rings(camera, gizmo_center_world, ring_radius);
    payload["transform"]["position"] = json_vec3(actor.position);
    payload["transform"]["rotation"] = json_vec3(actor.rotation);
    payload["transform"]["scale"] = json_vec3(actor.scale);
    send_gizmo_event(frame, "actor-gizmo-state", payload);
    return true;
}

bool handle_actor_gizmo_drag(const CefRefPtr<CefFrame>& frame,
                             const CefRefPtr<CefProcessMessage>& message) {
    auto args = message->GetArgumentList();
    double camera_value = 0.0;
    double actor_value = 0.0;
    double x = 0.0;
    double y = 0.0;
    double vp_w = 0.0;
    double vp_h = 0.0;
    if (!args || args->GetSize() < 12 ||
        !get_numeric_arg(args, 0, camera_value) ||
        args->GetType(1) != VTYPE_STRING ||
        !get_numeric_arg(args, 2, actor_value) ||
        args->GetType(3) != VTYPE_STRING ||
        args->GetType(4) != VTYPE_STRING ||
        args->GetType(5) != VTYPE_STRING ||
        args->GetType(6) != VTYPE_STRING ||
        args->GetType(7) != VTYPE_STRING ||
        !get_numeric_arg(args, 8, x) ||
        !get_numeric_arg(args, 9, y) ||
        !get_numeric_arg(args, 10, vp_w) ||
        !get_numeric_arg(args, 11, vp_h)) {
        CFW_LOG_WARNING("ActorGizmoDrag dropped: invalid args");
        return true;
    }

    const auto camera_handle = static_cast<std::uintptr_t>(camera_value);
    const std::string scene_id = args->GetString(1).ToString();
    const auto actor_handle = static_cast<std::uintptr_t>(actor_value);
    const std::string request_id = args->GetString(3).ToString();
    const std::string drag_id = args->GetString(4).ToString();
    const std::string phase = args->GetString(5).ToString();
    const std::string mode = args->GetString(6).ToString();
    const std::string axis = args->GetString(7).ToString();

    auto make_payload = [&](const std::string& status, const char* message_text = nullptr) {
        nlohmann::json payload;
        payload["status"] = status;
        payload["sceneId"] = scene_id;
        payload["cameraHandle"] = static_cast<std::uint64_t>(camera_handle);
        payload["actorHandle"] = static_cast<std::uint64_t>(actor_handle);
        payload["requestId"] = request_id;
        payload["dragId"] = drag_id;
        payload["phase"] = phase;
        payload["mode"] = mode;
        payload["axis"] = axis;
        if (message_text) payload["message"] = message_text;
        return payload;
    };

    if (camera_handle == 0 || actor_handle == 0 || scene_id.empty() ||
        request_id.empty() || drag_id.empty() ||
        (phase != "start" && phase != "move" && phase != "end") ||
        (mode != "move" && mode != "scale" && mode != "rotate") ||
        (axis != "x" && axis != "y" && axis != "z" && axis != "uniform") ||
        (axis == "uniform" && mode != "scale") ||
        vp_w <= 0.0 || vp_h <= 0.0 || !std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(vp_w) || !std::isfinite(vp_h) ||
        x < 0.0 || y < 0.0 || x >= vp_w || y >= vp_h) {
        send_gizmo_event(frame, "actor-gizmo-transform", make_payload("error", "invalid params"));
        return true;
    }

    GizmoCameraSnapshot camera;
    if (!read_gizmo_camera_snapshot(camera_handle, vp_w, vp_h, camera)) {
        send_gizmo_event(frame, "actor-gizmo-transform", make_payload("error", "camera unavailable"));
        return true;
    }

    auto make_result = [&](const GizmoDragSession& session,
                           const ktm::fvec3& position,
                           const ktm::fvec3& rotation,
                           const ktm::fvec3& scale) {
        auto payload = make_payload("success");
        payload["transform"]["position"] = json_vec3(position);
        payload["transform"]["rotation"] = json_vec3(rotation);
        payload["transform"]["scale"] = json_vec3(scale);
        send_gizmo_event(frame, "actor-gizmo-transform", payload);
        (void)session;
    };

    if (phase == "start") {
        GizmoActorSnapshot actor;
        if (!read_gizmo_actor_snapshot(actor_handle, actor)) {
            send_gizmo_event(frame, "actor-gizmo-transform", make_payload("error", "actor transform unavailable"));
            return true;
        }

        GizmoDragSession session;
        session.camera_handle = camera_handle;
        session.actor_handle = actor_handle;
        session.transform_handle = actor.transform_handle;
        session.scene_id = scene_id;
        session.request_id = request_id;
        session.drag_id = drag_id;
        session.mode = mode;
        session.axis = axis;
        session.center = actor.bounds_center_world;
        session.axis_vector = axis == "uniform" ? make_fvec3(0.0f, 1.0f, 0.0f) : make_gizmo_axis_vector(axis);
        session.start_position = actor.position;
        session.start_rotation = actor.rotation;
        session.start_scale = actor.scale;
        session.start_bounds_center_world = actor.bounds_center_world;
        session.local_bounds_min = actor.local_bounds_min;
        session.local_bounds_max = actor.local_bounds_max;
        session.has_local_bounds = actor.has_local_bounds;
        session.start_x = x;
        session.start_y = y;
        if (mode == "rotate" &&
            !intersect_rotation_plane(camera, session.center, session.axis_vector, x, y, session.start_rotate_vector)) {
            send_gizmo_event(frame, "actor-gizmo-transform", make_payload("error", "rotation plane unavailable"));
            return true;
        }
        if (mode != "rotate" && axis != "uniform" &&
            !intersect_axis_drag_plane(camera, session.center, session.axis_vector, x, y, session.start_axis_t)) {
            send_gizmo_event(frame, "actor-gizmo-transform", make_payload("error", "drag plane unavailable"));
            return true;
        }

        {
            std::lock_guard<std::mutex> lock(s_gizmo_drag_mutex);
            s_gizmo_drags[drag_id] = session;
        }
        make_result(session, actor.position, actor.rotation, actor.scale);
        return true;
    }

    GizmoDragSession session;
    bool missing_session = false;
    {
        std::lock_guard<std::mutex> lock(s_gizmo_drag_mutex);
        auto it = s_gizmo_drags.find(drag_id);
        if (it == s_gizmo_drags.end()) {
            missing_session = true;
        } else {
            session = it->second;
        }
    }
    if (missing_session) {
        send_gizmo_event(frame, "actor-gizmo-transform", make_payload("stale", "drag session is stale"));
        return true;
    }

    if (session.actor_handle != actor_handle ||
        session.camera_handle != camera_handle ||
        session.request_id != request_id ||
        session.scene_id != scene_id ||
        session.mode != mode ||
        session.axis != axis) {
        send_gizmo_event(frame, "actor-gizmo-transform", make_payload("stale", "drag session mismatch"));
        return true;
    }

    ktm::fvec3 next_position = session.start_position;
    ktm::fvec3 next_rotation = session.start_rotation;
    ktm::fvec3 next_scale = session.start_scale;
    if (session.mode == "move") {
        double current_axis_t = session.start_axis_t;
        if (!intersect_axis_drag_plane(camera, session.center, session.axis_vector, x, y, current_axis_t)) {
            send_gizmo_event(frame, "actor-gizmo-transform", make_payload("error", "drag plane unavailable"));
            return true;
        }
        const float delta = static_cast<float>(current_axis_t - session.start_axis_t);
        next_position = add_vec3(session.start_position, mul_vec3(session.axis_vector, delta));
    } else if (session.mode == "rotate") {
        ktm::fvec3 current_vector{};
        if (!intersect_rotation_plane(camera, session.center, session.axis_vector, x, y, current_vector)) {
            send_gizmo_event(frame, "actor-gizmo-transform", make_payload("error", "rotation plane unavailable"));
            return true;
        }
        const float delta = signed_rotation_delta(session.start_rotate_vector, current_vector, session.axis_vector);
        const int component = session.axis == "x" ? 0 : (session.axis == "y" ? 1 : 2);
        next_rotation[component] = session.start_rotation[component] + delta;
    } else if (session.axis == "uniform") {
        const float factor = std::max(0.001f, 1.0f + static_cast<float>((session.start_y - y) * 0.01));
        next_scale = mul_vec3(session.start_scale, factor);
    } else {
        double current_axis_t = session.start_axis_t;
        if (!intersect_axis_drag_plane(camera, session.center, session.axis_vector, x, y, current_axis_t)) {
            send_gizmo_event(frame, "actor-gizmo-transform", make_payload("error", "drag plane unavailable"));
            return true;
        }
        const float delta = static_cast<float>(current_axis_t - session.start_axis_t);
        int component = session.axis == "x" ? 0 : (session.axis == "y" ? 1 : 2);
        next_scale[component] = std::max(0.001f, session.start_scale[component] + delta);
    }

    if (session.mode == "scale" && session.has_local_bounds) {
        const auto new_center_without_compensation =
            compute_bounds_center_world(session.start_position,
                                        session.start_rotation,
                                        next_scale,
                                        session.local_bounds_min,
                                        session.local_bounds_max);
        const auto compensation = sub_vec3(session.start_bounds_center_world,
                                           new_center_without_compensation);
        next_position = add_vec3(session.start_position, compensation);
    }

    if (auto transform = Corona::SharedDataHub::instance()
                             .model_transform_storage()
                             .try_acquire_write(session.transform_handle)) {
        if (session.mode == "move") {
            transform->position = next_position;
        } else if (session.mode == "scale") {
            transform->position = next_position;
            transform->scale = next_scale;
        } else if (session.mode == "rotate") {
            transform->euler_rotation = next_rotation;
        }
    } else {
        send_gizmo_event(frame, "actor-gizmo-transform", make_payload("error", "transform unavailable"));
        return true;
    }

    make_result(session, next_position, next_rotation, next_scale);
    if (phase == "end") {
        std::lock_guard<std::mutex> lock(s_gizmo_drag_mutex);
        s_gizmo_drags.erase(drag_id);
    }
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
                        mech->bEnableCollision = (value != 0.0);
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

    {
        std::lock_guard<std::mutex> lock(s_input_mutex);
        s_input_queue.push_back(std::move(evt));
    }

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
            nlohmann::json result;
            result["tab_id"] = tab_id;
            result["panel_id"] = panel_id;
            send_dock_callback(frame, request_id, nullptr, result);
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
                bm.enqueue_main_thread_task([tab_id] {
                    BrowserManager::instance().remove_tab(tab_id);
                });
            }
            return true;
        }

        if (cmd == "closePanelTab") {
            int tab_id = command.value("tabId", -1);
            std::string panel_id = command.value("panelId", "");
            if (tab_id >= 0) {
                bm.enqueue_main_thread_task([tab_id] {
                    BrowserManager::instance().remove_tab(tab_id);
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

    if (message->GetName() == "ActorGizmoState") {
        return handle_actor_gizmo_state(frame, message);
    }

    if (message->GetName() == "ActorGizmoDrag") {
        return handle_actor_gizmo_drag(frame, message);
    }

    if (message->GetName() == "ViewportGizmoMode") {
        return handle_viewport_gizmo_mode(message);
    }

    if (message->GetName() == "ViewportGizmoSelection") {
        return handle_viewport_gizmo_selection(message);
    }

    if (message->GetName() == "ViewportGizmoClearSelection") {
        return handle_viewport_gizmo_clear_selection(message);
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
