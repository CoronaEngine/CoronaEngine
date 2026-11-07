#include "cef_window_operations.h"
#include "cef_client.h"
#include <corona/systems/ui/camera_viewport_manager.h>
#include <corona/shared_data_hub.h>
#include <cmath>
#include <limits>

#include <algorithm>
#include <unordered_map>
#include <SDL3/SDL.h>
#include <windows.h>
#include <horizon/core/logging.h>

namespace Corona::Systems::UI {
namespace {

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

void toggle_camera_maximized(int tab_id) {
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
}

void cycle_camera_window_mode(int tab_id) {
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
}

void toggle_camera_borderless(int tab_id) {
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
}

void toggle_main_borderless() {
    SDL_Window* window = BrowserManager::instance().main_window();
    if (!window) {
        CFW_LOG_WARNING("toggleMainWindowBorderless skipped: no main window");
        return;
    }
    static bool s_main_borderless = false;
    static int s_saved_x = 0, s_saved_y = 0, s_saved_w = 0, s_saved_h = 0;
    static bool s_saved_maximized = false;
    if (s_main_borderless) {
        CFW_LOG_DEBUG("Restoring main window from borderless fullscreen");
        SDL_SetWindowFullscreen(window, false);
        SDL_SetWindowBordered(window, true);
        SDL_RestoreWindow(window);
        SDL_SetWindowPosition(window, s_saved_x, s_saved_y);
        SDL_SetWindowSize(window, s_saved_w, s_saved_h);
        if (s_saved_maximized) {
            SDL_MaximizeWindow(window);
        }
        s_main_borderless = false;
    } else {
        SDL_GetWindowPosition(window, &s_saved_x, &s_saved_y);
        SDL_GetWindowSize(window, &s_saved_w, &s_saved_h);
        s_saved_maximized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
        const SDL_DisplayID display_id = SDL_GetDisplayForWindow(window);
        SDL_Rect bounds{};
        if (!SDL_GetDisplayBounds(display_id, &bounds)) {
            bounds = SDL_Rect{s_saved_x, s_saved_y, s_saved_w, s_saved_h};
        }
        CFW_LOG_DEBUG("Setting main window borderless fullscreen: x={}, y={}, w={}, h={}",
                      bounds.x, bounds.y, bounds.w, bounds.h);
        SDL_SetWindowFullscreen(window, false);
        SDL_RestoreWindow(window);
        SDL_SetWindowBordered(window, false);
        SDL_SetWindowPosition(window, bounds.x, bounds.y);
        SDL_SetWindowSize(window, bounds.w, bounds.h);
        SDL_RaiseWindow(window);
        s_main_borderless = true;
    }
}

void resize_camera_view(int tab_id, int width, int height) {
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
}

}  // namespace

namespace {
constexpr const char* viewport_cursors[] = {"arrow", "hand", "crosshair", "grab", "grabbing", "hidden"};
std::optional<Corona::ViewportUiCursorShape> viewport_cursor_shape(const std::string& name) {
    for (std::size_t i = 0; i < std::size(viewport_cursors); ++i)
        if (name == viewport_cursors[i]) return static_cast<Corona::ViewportUiCursorShape>(i);
    return std::nullopt;
}
}

void release_viewport_pointer(BrowserTab& source, std::uint64_t context_id) {
    if (!context_id || source.viewport_pointer_owner != context_id) return;
    Corona::SharedDataHub::instance().clear_viewport_ui_pointer(source.viewport_pointer_camera, context_id);
    source.hide_system_cursor.store(false, std::memory_order_relaxed);
    source.viewport_pointer_owner = 0;
    source.viewport_pointer_camera = 0;
    source.viewport_pointer_authority.reset();
}

void refresh_viewport_state(BrowserTab& source) {
    if (source.web_camera_handle) {
        const auto authority = source.web_camera_authority;
        std::unique_lock<std::mutex> lease;
        if (authority) lease = std::unique_lock(authority->mutex);
        if (!source.open || !authority || !authority->active) {
            source.web_camera_handle = 0;
            source.web_camera_authority.reset();
            source.web_camera_width = source.web_camera_height = 0;
            source.web_camera_render_width = source.web_camera_render_height = 0;
        }
    }
    if (!source.viewport_pointer_owner) return;
    const auto authority = source.viewport_pointer_authority.lock();
    std::unique_lock<std::mutex> lease;
    if (authority) lease = std::unique_lock(authority->mutex);
    const auto state = Corona::SharedDataHub::instance().viewport_ui_state(source.viewport_pointer_camera);
    if (!source.open || !authority || !authority->active || state.mode != Corona::ViewportUiMode::Stereo3D ||
        state.pointer.owner_context != source.viewport_pointer_owner || state.pointer.cursor_shape == Corona::ViewportUiCursorShape::Hidden)
        release_viewport_pointer(source, source.viewport_pointer_owner);
}

void release_viewport_pointer_context(int browser_id, std::uint64_t context_id) {
    for (const auto& entry : BrowserManager::instance().tabs_snapshot()) {
        auto* tab = entry.tab;
        if (!tab || !tab->client || tab->viewport_pointer_owner != context_id) continue;
        const auto browser = tab->client->GetBrowser();
        if (browser && browser->GetIdentifier() == browser_id) release_viewport_pointer(*tab, context_id);
    }
}

WindowReply apply_viewport_configuration(const WindowRequest& request, BrowserTab& source) {
    WindowReply result;
    const auto fail = [&](const char* code, const char* message) {
        result.error_code = code; result.error_message = message; return result;
    };
    if (!source.open) return fail("WINDOW_CLOSED", "Viewport window is no longer available");
    const auto authority = request.camera_authority;
    std::unique_lock<std::mutex> lease;
    if (authority) lease = std::unique_lock(authority->mutex);
    if (!authority || !authority->active || !request.camera_handle)
        return fail("PROJECT_CONTEXT_CHANGED", "Viewport camera identity was retired");
    if (!request.viewport || (!request.viewport->mode && !request.viewport->calibration && !request.viewport->bounds && !request.viewport->pointer))
        return fail("INVALID_ARGUMENT", "Viewport configuration requires at least one property");
    const auto& patch = *request.viewport;
    if (patch.mode && *patch.mode != "flat2d" && *patch.mode != "stereo3d")
        return fail("INVALID_ARGUMENT", "Invalid viewport UI mode");
    std::optional<Corona::ViewportUiCalibration> converted;
    if (patch.calibration) {
        const auto& c = *patch.calibration;
        const auto representable = [](double value) {
            return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
        };
        if (!representable(c.pe) || c.pe <= 1e-5 || !representable(c.angle) ||
            !representable(c.offset) || !representable(c.parallax_scale) || !representable(c.offset / c.pe))
            return fail("INVALID_ARGUMENT", "Invalid viewport lenticular calibration");
        converted.emplace();
        converted->lenticular_pitch = static_cast<float>(c.pe / 3);
        converted->slant_angle_radians = static_cast<float>(-c.angle);
        converted->phase_offset = static_cast<float>(c.offset / c.pe);
        converted->parallax_scale = static_cast<float>(c.parallax_scale);
    }
    if (patch.bounds) {
        const auto& bounds = *patch.bounds;
        if (bounds.x < 0 || bounds.y < 0 || bounds.width <= 0 || bounds.height <= 0 || bounds.render_width <= 0 || bounds.render_height <= 0)
            return fail("INVALID_ARGUMENT", "Invalid viewport bounds");
    }
    if ((patch.bounds || patch.pointer) && source.camera_view) {
        const auto record = CameraViewportManager::instance().find_by_tab(source.tab_id);
        if (!record || record->camera_handle != request.camera_handle)
            return fail("PERMISSION_DENIED", "CameraView cannot bind another camera surface");
    }
    std::optional<Corona::ViewportUiCursorShape> cursor;
    if (patch.pointer) {
        const auto& pointer = *patch.pointer;
        cursor = viewport_cursor_shape(pointer.cursor);
        if (!request.context_id || !cursor || !std::isfinite(pointer.x) || !std::isfinite(pointer.y) ||
            pointer.x < 0 || pointer.y < 0 || pointer.x > std::numeric_limits<float>::max() || pointer.y > std::numeric_limits<float>::max())
            return fail("INVALID_ARGUMENT", "Invalid viewport pointer state");
        if (!source.camera_view && (source.docking_pos != "main" || (!patch.bounds && source.web_camera_handle != request.camera_handle)))
            return fail("PERMISSION_DENIED", "Pointer requires the caller's bound camera surface");
    }
    // All fields and the native identity lease are valid before any mutation.
    auto& hub = Corona::SharedDataHub::instance();
    if (patch.bounds && source.viewport_pointer_camera && source.viewport_pointer_camera != request.camera_handle)
        release_viewport_pointer(source, source.viewport_pointer_owner);
    if (patch.mode) hub.set_viewport_ui_mode(request.camera_handle,
        *patch.mode == "stereo3d" ? Corona::ViewportUiMode::Stereo3D : Corona::ViewportUiMode::Flat2D);
    if (converted) hub.set_viewport_ui_calibration(request.camera_handle, *converted);
    if (patch.bounds) {
        const auto& bounds = *patch.bounds;
        source.web_camera_handle = request.camera_handle;
        source.web_camera_authority = authority;
        source.web_camera_x = bounds.x; source.web_camera_y = bounds.y;
        source.web_camera_width = bounds.width; source.web_camera_height = bounds.height;
        source.web_camera_render_width = bounds.render_width; source.web_camera_render_height = bounds.render_height;
    }
    if (patch.pointer) {
        if (*cursor == Corona::ViewportUiCursorShape::Hidden) release_viewport_pointer(source, request.context_id);
        else {
            if (source.viewport_pointer_owner != request.context_id || source.viewport_pointer_camera != request.camera_handle)
                release_viewport_pointer(source, source.viewport_pointer_owner);
            const auto& pointer = *patch.pointer;
            hub.set_viewport_ui_pointer(request.camera_handle, {static_cast<float>(pointer.x), static_cast<float>(pointer.y), *cursor, request.context_id});
            source.viewport_pointer_owner = request.context_id;
            source.viewport_pointer_camera = request.camera_handle;
            source.viewport_pointer_authority = authority;
        }
    }
    const auto state = hub.viewport_ui_state(request.camera_handle);
    const bool hide_cursor = source.viewport_pointer_owner && state.pointer.owner_context == source.viewport_pointer_owner &&
        state.mode == Corona::ViewportUiMode::Stereo3D && state.pointer.cursor_shape != Corona::ViewportUiCursorShape::Hidden;
    if (source.viewport_pointer_camera == request.camera_handle || !source.viewport_pointer_owner)
        source.hide_system_cursor.store(hide_cursor, std::memory_order_relaxed);
    const auto& c = state.calibration;
    result.viewport = ViewportConfigurationSnapshot{request.archive_session, request.scene_id, request.camera_id,
        state.mode == Corona::ViewportUiMode::Stereo3D ? "stereo3d" : "flat2d",
        {double(c.lenticular_pitch) * 3, -double(c.slant_angle_radians), double(c.phase_offset) * double(c.lenticular_pitch) * 3, c.parallax_scale}};
    result.viewport->pointer = {state.pointer.x, state.pointer.y, viewport_cursors[static_cast<std::size_t>(state.pointer.cursor_shape)]};
    result.viewport->system_cursor_hidden = source.hide_system_cursor.load(std::memory_order_relaxed);
    if (source.web_camera_handle == request.camera_handle)
        result.viewport->bounds = ViewportBounds{source.web_camera_x, source.web_camera_y,
            source.web_camera_width, source.web_camera_height, source.web_camera_render_width, source.web_camera_render_height};
    result.accepted = true;
    return result;
}

WindowReply execute_window_operation(const WindowRequest& request) {
    auto& manager = BrowserManager::instance();
    BrowserTab* source = nullptr;
    for (const auto& entry : manager.tabs_snapshot()) {
        auto* tab = entry.tab;
        if (!tab || !tab->open || !tab->client) continue;
        const auto browser = tab->client->GetBrowser();
        if (browser && browser->GetIdentifier() == request.browser_id) { source = tab; break; }
    }
    WindowReply result;
    if (!source) {
        result.error_code = "WINDOW_CLOSED";
        result.error_message = "Native window is no longer available";
        return result;
    }
    const bool camera_control = request.method == WindowMethod::ToggleCameraMaximized ||
        request.method == WindowMethod::CycleCameraWindowMode || request.method == WindowMethod::ToggleCameraBorderless ||
        request.method == WindowMethod::ResizeCameraView;
    if (camera_control) {
        // Explicit scene/camera identity is authoritative. Never guess from a URL
        // substring or apply a missing camera's command to the caller instead.
        auto* camera_tab = source;
        if (!request.scene_id.empty()) {
            const auto camera = CameraViewportManager::instance().find_by_camera(request.scene_id, request.camera_id);
            camera_tab = camera ? manager.get_tab(camera->tab_id) : nullptr;
        }
        if (!camera_tab || !camera_tab->open || !camera_tab->camera_view) return result;
        if (source->docking_pos != "main" && camera_tab != source) {
            result.error_code = "PERMISSION_DENIED";
            result.error_message = "Only the main window can control another camera window";
            return result;
        }
        const auto* window = camera_tab->platform_window_id ? SDL_GetWindowFromID(camera_tab->platform_window_id) : nullptr;
        if (!window && !camera_tab->platform_handle_raw) return result;
        switch (request.method) {
        case WindowMethod::ToggleCameraMaximized: toggle_camera_maximized(camera_tab->tab_id); break;
        case WindowMethod::CycleCameraWindowMode: cycle_camera_window_mode(camera_tab->tab_id); break;
        case WindowMethod::ToggleCameraBorderless: toggle_camera_borderless(camera_tab->tab_id); break;
        case WindowMethod::ResizeCameraView: resize_camera_view(camera_tab->tab_id, std::max(64, request.width), std::max(64, request.height)); break;
        default: break;
        }
        result.accepted = true;
        return result;
    }
    const bool panel_transition = request.method == WindowMethod::DetachPanel || request.method == WindowMethod::RedockPanel;
    const bool targeted = request.method == WindowMethod::CloseTab || request.method == WindowMethod::FocusTab ||
                          (panel_transition && request.tab_id >= 0);
    auto* target = targeted ? manager.get_tab(request.tab_id) : source;
    if ((targeted && source->docking_pos != "main" && (!panel_transition || target != source)) ||
        ((request.method == WindowMethod::Close || request.method == WindowMethod::CloseTab || panel_transition) &&
         target && target->docking_pos == "main")) {
        result.error_code = "PERMISSION_DENIED";
        result.error_message = "Only the main window can manage other panels; the main window cannot be closed as a panel";
        return result;
    }
    // Closing an already removed tab is an idempotent cleanup, not a retry.
    if (!target || !target->open) return result;
    switch (request.method) {
    case WindowMethod::ConfigureViewport:
        return apply_viewport_configuration(request, *source);
    case WindowMethod::ToggleMainBorderless:
        if (source->docking_pos != "main") {
            result.error_code = "PERMISSION_DENIED";
            result.error_message = "Only the main window can change its window mode";
        } else if (manager.main_window()) {
            toggle_main_borderless();
            result.accepted = true;
        }
        break;
    case WindowMethod::ToggleCameraMaximized:
    case WindowMethod::CycleCameraWindowMode:
    case WindowMethod::ToggleCameraBorderless:
    case WindowMethod::ResizeCameraView:
        break; // Handled above with exact camera identity and native permissions.
    case WindowMethod::CloseCameraView: {
        const auto camera = CameraViewportManager::instance().find_by_camera(request.scene_id, request.camera_id);
        if (!camera) break;
        if (source->docking_pos != "main" && camera->tab_id != source->tab_id) {
            result.error_code = "PERMISSION_DENIED";
            result.error_message = "Only the main window can close another camera view";
            break;
        }
        if (camera->tab_id == source->tab_id) result.close_self_tab_id = source->tab_id;
        else manager.remove_tab(camera->tab_id);
        result.accepted = true;
        break;
    }
    case WindowMethod::SuspendCameraViews: {
        const auto ids = CameraViewportManager::instance().tabs_for_scene(request.scene_id);
        if (source->docking_pos != "main" && std::any_of(ids.begin(), ids.end(), [&](int id) { return id != source->tab_id; })) {
            result.error_code = "PERMISSION_DENIED";
            result.error_message = "Only the main window can suspend other camera views";
            break;
        }
        for (const int id : ids) {
            if (auto* tab = manager.get_tab(id)) tab->preserve_camera_open_on_close = true;
            if (id == source->tab_id) result.close_self_tab_id = id;
            else manager.remove_tab(id);
        }
        result.closed_count = static_cast<int>(ids.size());
        break;
    }
    case WindowMethod::CreateCameraView: {
        // SceneTools also runs in its own native Dock context. Preserve that
        // existing caller path; scene identity and the native lease authorize
        // the camera rather than a main-window-only UI policy.
        const auto authority = request.camera_authority;
        std::unique_lock<std::mutex> lease;
        if (authority) lease = std::unique_lock(authority->mutex);
        if (!authority || !authority->active || !request.camera_handle) {
            result.error_code = "PROJECT_CONTEXT_CHANGED";
            result.error_message = "Camera window request was retired before creation";
            break;
        }
        auto& cameras = CameraViewportManager::instance();
        if (const auto existing = cameras.find_by_camera(request.scene_id, request.camera_id)) {
            const auto* tab = manager.get_tab(existing->tab_id);
            if (existing->camera_handle != request.camera_handle || !tab || !tab->open) {
                result.error_code = "PROJECT_CONTEXT_CHANGED";
                result.error_message = "Previous camera surface has not finished retiring";
                break;
            }
            result.snapshot.tab_id = existing->tab_id;
            result.existing = true;
            break;
        }
        const auto& options = *request.panel;
        auto route = options.route;
        if (route.starts_with('#')) route.erase(0, 1);
        route += route.find('?') == std::string::npos ? "?standalone=1" : "&standalone=1";
        const int id = manager.create_tab(request.base_url, route, "camera", options.width, options.height,
                                          false, true, options.x, options.y);
        auto* tab = manager.get_tab(id);
        if (!tab) {
            result.error_code = "CREATE_FAILED";
            result.error_message = "Native camera window could not be created";
            break;
        }
        tab->detach_x = options.x;
        tab->detach_y = options.y;
        tab->detach_w = options.width;
        tab->detach_h = options.height;
        tab->detach_state = BrowserTab::DetachState::Detaching;
        if (!cameras.register_view(request.scene_id, request.camera_id, request.camera_handle, id, request.camera_state, request.camera_authority)) {
            manager.remove_tab(id);
            result.error_code = "CREATE_FAILED";
            result.error_message = "Native camera surface could not be registered";
            break;
        }
        tab->name = "Camera " + request.camera_id;
        result.snapshot.tab_id = id;
        break;
    }
    case WindowMethod::CreatePanel: {
        const auto& options = *request.panel;
        auto route = options.route;
        if (route.starts_with('#')) route.erase(0, 1);
        route += route.find('?') == std::string::npos ? "?standalone=1&nativeHost=1" : "&standalone=1&nativeHost=1";
        const auto docking_pos = options.detached ? std::string("right_top") : options.docking_pos;
        const int id = manager.create_tab(request.base_url, route, docking_pos, options.width, options.height, false);
        auto* tab = manager.get_tab(id);
        if (!tab) {
            result.error_code = "CREATE_FAILED";
            result.error_message = "Native panel could not be created";
            break;
        }
        tab->panel_id = options.panel_id;
        tab->panel_title = options.title;
        if (options.detached) {
            // Preserve the existing same-frame desired-state transition.
            tab->detach_x = options.x;
            tab->detach_y = options.y;
            tab->detach_w = options.width;
            tab->detach_h = options.height;
            tab->detach_state = BrowserTab::DetachState::Detaching;
        } else {
            tab->native_docked = options.native_docked;
            tab->floating = true;
            tab->z_priority = options.z_priority;
            tab->initial_x = 80;
            tab->initial_y = 80;
            if (docking_pos == "right_top") { tab->initial_x = 1100; tab->initial_y = 80; }
            else if (docking_pos == "right_bottom") { tab->initial_x = 1100; tab->initial_y = 560; }
            else if (docking_pos == "left_bottom") { tab->initial_x = 80; tab->initial_y = 560; }
            else if (docking_pos == "center") { tab->initial_x = 600; tab->initial_y = 300; }
        }
        result.snapshot.tab_id = id;
        result.snapshot.panel_id = tab->panel_id;
        break;
    }
    case WindowMethod::DetachPanel: {
        if (target->detach_state != BrowserTab::DetachState::Docked) break;
        const auto& options = *request.panel;
        target->detach_x = options.x;
        target->detach_y = options.y;
        target->detach_w = options.width > 0 ? options.width : std::max(1, target->width);
        target->detach_h = options.height > 0 ? options.height : std::max(1, target->height);
        target->detach_maximized = options.maximized;
        target->detach_state = BrowserTab::DetachState::Detaching;
        result.accepted = true;
        break;
    }
    case WindowMethod::RedockPanel:
        if (target->detach_state == BrowserTab::DetachState::Detached) {
            target->detach_state = BrowserTab::DetachState::Redocking;
            result.accepted = true;
        }
        break;
    case WindowMethod::Close:
        // The binding settles this context's Promise before queuing retirement.
        result.close_self_tab_id = source->tab_id;
        result.accepted = true;
        break;
    case WindowMethod::CloseTab:
        target->open = false;
        result.accepted = true;
        break;
    case WindowMethod::FocusTab:
        target->minimized = false;
        target->native_focus_requested = true;
        result.accepted = true;
        break;
    case WindowMethod::GetCurrent:
    case WindowMethod::SetDragRegions: {
        if (request.regions) manager.set_tab_drag_regions(source->tab_id, *request.regions);
        auto& snapshot = result.snapshot;
        snapshot.tab_id = source->tab_id;
        snapshot.width = source->width;
        snapshot.height = source->height;
        snapshot.minimized = source->minimized;
        snapshot.detached = source->detach_state == BrowserTab::DetachState::Detached;
        switch (source->detach_state) {
        case BrowserTab::DetachState::Docked: snapshot.dock_state = "docked"; break;
        case BrowserTab::DetachState::Detaching: snapshot.dock_state = "detaching"; break;
        case BrowserTab::DetachState::Detached: snapshot.dock_state = "detached"; break;
        case BrowserTab::DetachState::Redocking: snapshot.dock_state = "redocking"; break;
        }
        snapshot.panel_id = source->panel_id;
        { std::lock_guard lock(source->drag_mutex); snapshot.drag_regions = source->drag_regions; }
        break;
    }
    }
    return result;
}

}  // namespace Corona::Systems::UI
