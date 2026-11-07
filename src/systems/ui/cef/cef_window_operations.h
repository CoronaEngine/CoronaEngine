#pragma once

#include "browser_manager.h"
#include "camera_window_lifetime.h"
#include <optional>

namespace Corona::Systems::UI {

enum class WindowMethod { GetCurrent, SetDragRegions, Close, CloseTab, FocusTab, CreatePanel, DetachPanel, RedockPanel,
                          SuspendCameraViews, CloseCameraView, ToggleCameraMaximized, CycleCameraWindowMode,
                          ToggleCameraBorderless, ResizeCameraView, ToggleMainBorderless, CreateCameraView, ConfigureViewport };

struct ViewportBounds {
    int x = 0, y = 0, width = 0, height = 0, render_width = 0, render_height = 0;
};
struct ViewportLenticularCalibration {
    double pe = 19.1813, angle = 0.2305, offset = 14.1171, parallax_scale = 19.1849;
};
struct ViewportPointer {
    double x = 0, y = 0;
    std::string cursor = "hidden";
};
struct ViewportConfiguration {
    std::optional<std::string> mode;
    std::optional<ViewportLenticularCalibration> calibration;
    std::optional<ViewportBounds> bounds;
    std::optional<ViewportPointer> pointer;
};
struct ViewportConfigurationSnapshot {
    std::string archive_session, scene, camera_id, mode;
    ViewportLenticularCalibration calibration;
    std::optional<ViewportBounds> bounds;
    ViewportPointer pointer;
    bool system_cursor_hidden = false;
};

struct PanelOptions {
    std::string panel_id;
    std::string title;
    std::string route;
    std::string docking_pos = "right_top";
    int width = 0, height = 0, x = 120, y = 120, z_priority = 0;
    bool native_docked = false, detached = false, maximized = false;
};

// Thread-boundary DTOs. No V8 handles or legacy JSON requests belong here.
struct WindowSnapshot {
    int tab_id = -1;
    int width = 0;
    int height = 0;
    bool minimized = false;
    bool detached = false;
    std::string dock_state;
    std::string panel_id;
    std::vector<DragRegion> drag_regions;
};

struct WindowRequest {
    WindowMethod method;
    int browser_id;
    int tab_id = -1;
    std::optional<std::vector<DragRegion>> regions;
    std::optional<PanelOptions> panel;
    std::string base_url;
    std::string scene_id;
    std::string camera_id;
    std::string archive_session;
    std::uintptr_t camera_handle = 0; // Resolved by the native scene owner, never supplied by Web.
    std::shared_ptr<CameraWindowAuthority> camera_authority;
    std::shared_ptr<CameraWindowState> camera_state;
    int width = 0, height = 0;
    std::optional<ViewportConfiguration> viewport;
    std::uint64_t context_id = 0; // Native endpoint identity; never supplied by Web.
};

struct WindowReply {
    WindowSnapshot snapshot;
    bool accepted = false;
    bool existing = false;
    int close_self_tab_id = -1;
    int closed_count = 0;
    std::string error_code;
    std::string error_message;
    std::optional<ViewportConfigurationSnapshot> viewport;
};

// Invoked only by the BrowserManager main-thread task queue.
WindowReply execute_window_operation(const WindowRequest& request);
WindowReply apply_viewport_configuration(const WindowRequest& request, BrowserTab& source);
void release_viewport_pointer(BrowserTab& source, std::uint64_t context_id);
void refresh_viewport_state(BrowserTab& source);
void release_viewport_pointer_context(int browser_id, std::uint64_t context_id);

// Invoked asynchronously on the native editor scene owner (CEF UI) thread.
// Resolves exact IDs and fills only immutable DTO values plus a native lease.
WindowReply prepare_camera_window_request(WindowRequest& request);

}  // namespace Corona::Systems::UI
