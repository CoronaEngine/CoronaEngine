#pragma once

// Phase 6 of the ImGui-removal plan: the ImGui-free main-window frame loop. It replaces
// UI::UiFrameRunner (imgui/imgui_ui.cpp), which drove the frame via ImGui NewFrame/Render
// plus a transparent dockspace. This runner instead:
//   1. pumps SDL events (quit/resize/focus/text + windowID routing), feeding the input
//      router (mouse/wheel) and BrowserInputHandler (keyboard/text/IME);
//   2. computes fixed-rect panel placement via compute_panel_layout();
//   3. for camera_view panels, binds the surface + layout to CameraViewportManager using the
//      computed rect and the main window's native HWND;
//   4. forwards mouse input to the hit panel's CEF browser via the MouseUtils *_ex overloads;
//   5. builds a QuadDraw list (one quad per visible panel texture) and renders it through
//      VulkanBackend::render_quads(), then present_frame().
//
// Phase 6 is main-window only: panels are laid out inside the single main window; there is
// no detach/secondary-window handling (that is Phase 7). Multi-viewport is gone.

#include <SDL3/SDL.h>

#include <cstdint>

#include <corona/systems/ui/panel_layout.h>
#include <corona/systems/ui/sdl_input_router.h>

#include "cef/browser_ui.h"
#include "sdl/sdl_utils.h"

namespace Corona::Systems {
class VulkanBackend;
}

namespace Corona::Systems::UI {

// Lifecycle: create the SDL window + Vulkan backend (replaces initialize_sdl_imgui), and
// tear them down (replaces shutdown_sdl_imgui). No ImGui context is created.
bool initialize_sdl_ui(SDL_Window*& window, std::unique_ptr<VulkanBackend>& vulkan_backend);
void shutdown_sdl_ui(SDL_Window*& window, std::unique_ptr<VulkanBackend>& vulkan_backend);

// Per-frame context, mirroring the old UiFrameContext minus the ImGuiIO pointer.
struct UiFrameContext {
    SDL_Window* window = nullptr;
    VulkanBackend* vulkan_backend = nullptr;
    int* active_tab_id = nullptr;
    bool* running = nullptr;
    bool* window_size_changed = nullptr;
};

// Forward declaration: the manager that owns the main + secondary OS windows.
struct ManagedWindow;

class UiFrameRunner {
   public:
    UiFrameRunner() = default;

    void run_frame(UiFrameContext& context);

   private:
    // Forward queued keyboard/text/IME events to the active tab's browser (unchanged logic).
    void dispatch_keyboard_to_active_tab(int active_tab_id);

    // Render one window (main or secondary): compute layout, resize CEF, bind cameras, route
    // mouse, build quads, render + present that window's surface. Phase 7c: the main window
    // renders all tabs with host_surface == nullptr via the docking layout; a secondary window
    // renders the single tab whose host_surface == that window's surface, filling it.
    void render_window(UiFrameContext& context, const ManagedWindow& managed);

    // Phase 7d: reconcile each tab's detach intent (set by the detachPanel/redockPanel
    // DockCommands) to actual OS-window + surface state. Runs once per frame on the UI thread,
    // before the window-render loop, so window creation/destruction + surface register/unregister
    // all happen single-threaded here (no cross-thread GPU access). Detaching -> create secondary
    // window, register its surface, set host_surface -> Detached. Redocking -> clear host_surface,
    // unregister surface, destroy the window (promise-synced) -> Docked.
    void reconcile_detach_states(UiFrameContext& context);

    // Build hit targets for one window's placements and forward mouse to the hit panel's
    // browser, using that window's per-window input bucket (window_id) and surface.
    void route_mouse_to_panels(SDL_WindowID window_id,
                               const std::vector<PanelPlacement>& placements,
                               int& active_tab_id);

    void set_system_cursor(SDL_SystemCursor cursor);

    // Diagnostics for a lag report about dragging a panel border. Floating panels are drawn as
    // quads on the main window's surface, and their border drag is handled natively (never in the
    // page), so nothing in the frontend can report where a panel is or whether a drag is active.
    // Logs each floating panel's rectangle plus the active drag/resize ids: immediately whenever
    // those ids change, otherwise at most once per second.
    void log_floating_layout();

    int url_input_active_tab_ = -1;

    // Phase 10: title-bar drag of an in-main-window floating panel rectangle. While a drag is
    // active, dragging_tab_id_ holds the tab being moved; its quad is drawn topmost and its
    // mouse events are NOT forwarded to CEF. The panel's logical rect is updated each move by
    // (start_rect + mouse delta), clamped to the work area. -1 = no drag.
    int dragging_tab_id_ = -1;
    float drag_mouse_start_x_ = 0.0f;
    float drag_mouse_start_y_ = 0.0f;
    float drag_rect_start_x_ = 0.0f;
    float drag_rect_start_y_ = 0.0f;

    // Edge/corner resize for in-main-window floating panels. This mirrors the title-bar
    // drag path above: while active, mouse input is consumed by native so resizing continues
    // even after the cursor leaves the panel's previous CEF rectangle.
    int resizing_tab_id_ = -1;
    int resize_edges_ = 0;
    float resize_mouse_start_x_ = 0.0f;
    float resize_mouse_start_y_ = 0.0f;
    float resize_rect_start_x_ = 0.0f;
    float resize_rect_start_y_ = 0.0f;
    float resize_rect_start_w_ = 0.0f;
    float resize_rect_start_h_ = 0.0f;
    SDL_SystemCursor active_system_cursor_ = SDL_SYSTEM_CURSOR_DEFAULT;

    std::uint64_t diag_frame_counter_ = 0;
    int diag_last_dragging_tab_id_ = -1;
    int diag_last_resizing_tab_id_ = -1;

    SdlInputRouter input_router_{};
    BrowserInputHandler input_handler_{};
    SDLEventHandler event_handler_{};
};

}  // namespace Corona::Systems::UI
