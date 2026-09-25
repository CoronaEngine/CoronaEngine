#include <corona/systems/ui/ui_frame_runner.h>

#include <corona/kernel/core/i_logger.h>
#include <corona/memory/gpu_mem_ledger.h>
#include <corona/systems/ui/camera_viewport_manager.h>
#include <corona/systems/ui/quad_compositor.h>
#include <corona/systems/ui/sdl_window_manager.h>
#include <corona/systems/ui/vulkan_backend.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "cef/browser_manager.h"
#include "cef/cef_client.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Corona::Systems::UI {

// ============================================================================
// SDL/UI lifecycle (replaces initialize_sdl_imgui / shutdown_sdl_imgui — no ImGui).
// ============================================================================

bool initialize_sdl_ui(SDL_Window*& window, std::unique_ptr<VulkanBackend>& vulkan_backend) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        CFW_LOG_ERROR("Failed to initialize SDL: {}", SDL_GetError());
        return false;
    }

    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");

    int initial_width = 1920;
    int initial_height = 1080;
    int initial_x = SDL_WINDOWPOS_CENTERED;
    int initial_y = SDL_WINDOWPOS_CENTERED;
    SDL_DisplayID primary_display = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode* desktop_mode = nullptr;
    if (primary_display != 0) {
        desktop_mode = SDL_GetDesktopDisplayMode(primary_display);
    }

    if (desktop_mode) {
        SDL_Rect usable_bounds{};
        if (SDL_GetDisplayUsableBounds(primary_display, &usable_bounds) &&
            usable_bounds.w > 0 && usable_bounds.h > 0) {
            initial_width = static_cast<int>(static_cast<float>(usable_bounds.w) * 0.8f);
            initial_height = static_cast<int>(static_cast<float>(usable_bounds.h) * 0.8f);
            initial_x = usable_bounds.x + static_cast<int>(
                                              (static_cast<float>(usable_bounds.w) -
                                               static_cast<float>(initial_width)) *
                                              0.5f);
            initial_y = usable_bounds.y + static_cast<int>(
                                              (static_cast<float>(usable_bounds.h) -
                                               static_cast<float>(initial_height)) *
                                              0.5f);
        } else {
            initial_width = static_cast<int>(static_cast<float>(desktop_mode->w) * 0.8f);
            initial_height = static_cast<int>(static_cast<float>(desktop_mode->h) * 0.8f);
        }
    }

    window = SDL_CreateWindow("Corona Engine (Horizon)", initial_width, initial_height,
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (window == nullptr) {
        CFW_LOG_ERROR("Failed to create window: {}", SDL_GetError());
        SDL_Quit();
        return false;
    }

    SDL_SetWindowPosition(window, initial_x, initial_y);
    SDL_StartTextInput(window);
    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "1");
    BrowserManager::instance().set_main_window(window);

    // 记录系统物理内存总量（MiB→字节），供 GeometrySystem 作为 RAM 预算分母。
    // 在此处获取：UI 本就链接 SDL，避免让 geometry 依赖 SDL，且保持跨平台。
    Corona::Memory::system_ram_bytes().store(
        static_cast<std::uint64_t>(SDL_GetSystemRAM()) * 1024ull * 1024ull,
        std::memory_order_relaxed);

    // Adopt the main window into the window manager singleton so the frame runner can iterate
    // all windows (main + detached) and detach/redock commands can mutate the set.
    SdlWindowManager::instance().adopt_main_window(window);

    vulkan_backend = std::make_unique<VulkanBackend>(window);
    if (!vulkan_backend->initialize()) {
        CFW_LOG_ERROR("Failed to initialize Corona UI backend");
        SDL_DestroyWindow(window);
        window = nullptr;
        SDL_Quit();
        return false;
    }

    return true;
}

void shutdown_sdl_ui(SDL_Window*& window, std::unique_ptr<VulkanBackend>& vulkan_backend) {
    // Retire secondary Display surfaces first, then release Vulkan image state, then destroy
    // their SDL windows. A failed acknowledgement leaves the window hidden and tracked.
    std::vector<void*> secondary_surfaces;
    SdlWindowManager::instance().for_each_window([&](const ManagedWindow& managed) {
        if (!managed.is_main && managed.surface != nullptr) secondary_surfaces.push_back(managed.surface);
    });
    bool all_drained = true;
    for (void* surface : secondary_surfaces) {
        if (SdlWindowManager::instance().request_remove_secondary_window(surface)) {
            if (vulkan_backend) vulkan_backend->unregister_surface(surface);
            all_drained = SdlWindowManager::instance().destroy_secondary_window(surface) && all_drained;
        } else {
            all_drained = false;
        }
    }
    void* main_surface = SdlWindowManager::instance().main_surface();
    if (main_surface != nullptr) {
        const bool ack = SdlWindowManager::instance().request_remove_surface(main_surface);
        all_drained = all_drained && ack;
        if (ack && vulkan_backend) vulkan_backend->unregister_surface(main_surface);
    }
    if (!all_drained) {
        CFW_LOG_ERROR("shutdown_sdl_ui: surface drain failed; retaining hidden windows and Vulkan resources");
        return;
    }

    if (vulkan_backend) {
        vulkan_backend->shutdown();
        vulkan_backend.reset();
    }

    if (window) {
        SDL_StopTextInput(window);
        SDL_DestroyWindow(window);
        window = nullptr;
    }

    SDL_Quit();
}

// ============================================================================
// UiFrameRunner
// ============================================================================

namespace {

// Build the layout inputs for one window: the tabs whose host_surface matches.
//   - host_filter == nullptr  -> the main window: all docked/main tabs (host_surface null).
//   - host_filter != nullptr  -> a secondary window: the single detached tab on that surface,
//     rewritten to docking_pos "main" so it fills the window.
std::vector<PanelLayoutInput> collect_layout_inputs(void* host_filter) {
    std::vector<PanelLayoutInput> inputs;
    for (const auto& [tab_id, tab] : BrowserManager::instance().get_tabs()) {
        if (!tab || !tab->open || tab->minimized) {
            continue;
        }
        if (tab->host_surface != host_filter) {
            continue;
        }
        if (host_filter == nullptr && tab->camera_view &&
            tab->detach_state != BrowserTab::DetachState::Docked) {
            CFW_LOG_DEBUG("CameraViewport: skip main layout tab={} while detach_state={}",
                          tab_id, static_cast<int>(tab->detach_state));
            continue;
        }
        PanelLayoutInput in;
        in.tab_id = tab_id;
        // A detached tab fills its own window regardless of its original docking_pos.
        in.docking_pos = (host_filter != nullptr) ? std::string("main") : tab->docking_pos;
        in.dock_width = tab->dock_width;
        in.dock_height = tab->dock_height;
        in.camera_view = tab->camera_view;
        in.floating = tab->floating;
        in.initial_x = tab->initial_x;
        in.initial_y = tab->initial_y;
        inputs.push_back(std::move(in));
    }
    // compute_panel_layout preserves input order, and both the compositor and hit-test use
    // placement order as z-order. Keep the main surface at the bottom, then sort floating
    // panels by their explicit priority and creation id for deterministic stacking.
    std::stable_sort(inputs.begin(), inputs.end(), [](const PanelLayoutInput& lhs, const PanelLayoutInput& rhs) {
        const auto* lhs_tab = BrowserManager::instance().get_tab(lhs.tab_id);
        const auto* rhs_tab = BrowserManager::instance().get_tab(rhs.tab_id);
        const int lhs_priority = lhs_tab ? lhs_tab->z_priority : 0;
        const int rhs_priority = rhs_tab ? rhs_tab->z_priority : 0;
        if (lhs_priority != rhs_priority) return lhs_priority < rhs_priority;
        return lhs.tab_id < rhs.tab_id;
    });
    return inputs;
}

enum FloatingResizeEdge {
    kResizeLeft = 1 << 0,
    kResizeRight = 1 << 1,
    kResizeTop = 1 << 2,
    kResizeBottom = 1 << 3,
};

int floating_resize_edges(const BrowserTab& tab, const HitResult& hit) {
    if (!tab.floating || hit.is_main) {
        return 0;
    }

    constexpr float kResizeHandle = 8.0f;
    constexpr float kTopResizeHandle = 6.0f;
    constexpr float kTitlebarHeight = 32.0f;
    constexpr float kTitlebarActionReserve = 80.0f;
    const float width = static_cast<float>(std::max(tab.width, tab.dock_width));
    const float height = static_cast<float>(std::max(tab.height, tab.dock_height));
    const bool over_titlebar_actions =
        hit.local_y >= 0.0f && hit.local_y <= kTitlebarHeight &&
        hit.local_x >= width - kTitlebarActionReserve && hit.local_x <= width;
    int edges = 0;

    if (over_titlebar_actions) {
        return 0;
    }

    if (hit.local_x >= 0.0f && hit.local_x <= kResizeHandle) {
        edges |= kResizeLeft;
    } else if (hit.local_x >= width - kResizeHandle && hit.local_x <= width) {
        edges |= kResizeRight;
    }

    if (hit.local_y >= 0.0f && hit.local_y <= kTopResizeHandle) {
        edges |= kResizeTop;
    } else if (hit.local_y >= height - kResizeHandle && hit.local_y <= height) {
        edges |= kResizeBottom;
    }

    return edges;
}

SDL_SystemCursor cursor_for_resize_edges(int edges) {
    const bool left = (edges & kResizeLeft) != 0;
    const bool right = (edges & kResizeRight) != 0;
    const bool top = (edges & kResizeTop) != 0;
    const bool bottom = (edges & kResizeBottom) != 0;

    if (left && top) {
        return SDL_SYSTEM_CURSOR_NW_RESIZE;
    }
    if (right && top) {
        return SDL_SYSTEM_CURSOR_NE_RESIZE;
    }
    if (right && bottom) {
        return SDL_SYSTEM_CURSOR_SE_RESIZE;
    }
    if (left && bottom) {
        return SDL_SYSTEM_CURSOR_SW_RESIZE;
    }
    if (left) {
        return SDL_SYSTEM_CURSOR_W_RESIZE;
    }
    if (right) {
        return SDL_SYSTEM_CURSOR_E_RESIZE;
    }
    if (top) {
        return SDL_SYSTEM_CURSOR_N_RESIZE;
    }
    if (bottom) {
        return SDL_SYSTEM_CURSOR_S_RESIZE;
    }
    return SDL_SYSTEM_CURSOR_DEFAULT;
}

SDL_Cursor* cached_system_cursor(SDL_SystemCursor cursor) {
    static std::array<SDL_Cursor*, SDL_SYSTEM_CURSOR_COUNT> cursors{};
    if (cursor == SDL_SYSTEM_CURSOR_DEFAULT) {
        return SDL_GetDefaultCursor();
    }

    const auto index = static_cast<std::size_t>(cursor);
    if (index >= cursors.size()) {
        return SDL_GetDefaultCursor();
    }
    if (cursors[index] == nullptr) {
        cursors[index] = SDL_CreateSystemCursor(cursor);
    }
    return cursors[index] ? cursors[index] : SDL_GetDefaultCursor();
}

void apply_floating_resize(BrowserTab& tab,
                           int tab_id,
                           int edges,
                           float start_x,
                           float start_y,
                           float start_w,
                           float start_h,
                           float dx,
                           float dy,
                           SDL_WindowID window_id) {
    constexpr float kMinWidth = 320.0f;
    constexpr float kMinHeight = 300.0f;

    float x = start_x;
    float y = start_y;
    float w = start_w;
    float h = start_h;

    if ((edges & kResizeLeft) != 0) {
        x = start_x + dx;
        w = start_w - dx;
        if (w < kMinWidth) {
            w = kMinWidth;
            x = start_x + start_w - kMinWidth;
        }
    } else if ((edges & kResizeRight) != 0) {
        w = std::max(kMinWidth, start_w + dx);
    }

    if ((edges & kResizeTop) != 0) {
        y = start_y + dy;
        h = start_h - dy;
        if (h < kMinHeight) {
            h = kMinHeight;
            y = start_y + start_h - kMinHeight;
        }
    } else if ((edges & kResizeBottom) != 0) {
        h = std::max(kMinHeight, start_h + dy);
    }

    int win_w = 0;
    int win_h = 0;
    if (SDL_Window* window = SdlWindowManager::instance().window_for_id(window_id)) {
        SDL_GetWindowSize(window, &win_w, &win_h);
    }
    if (win_w > 0) {
        if (x < 0.0f) {
            w += x;
            x = 0.0f;
        }
        if (x + w > static_cast<float>(win_w)) {
            w = std::max(kMinWidth, static_cast<float>(win_w) - x);
        }
    }
    if (win_h > 0) {
        if (y < 0.0f) {
            h += y;
            y = 0.0f;
        }
        if (y + h > static_cast<float>(win_h)) {
            h = std::max(kMinHeight, static_cast<float>(win_h) - y);
        }
    }

    tab.initial_x = static_cast<int>(std::lround(x));
    tab.initial_y = static_cast<int>(std::lround(y));
    tab.dock_width = static_cast<int>(std::lround(std::max(kMinWidth, w)));
    tab.dock_height = static_cast<int>(std::lround(std::max(kMinHeight, h)));
    tab.needs_reposition = true;
    tab.needs_resize = true;
    BrowserManager::instance().resize_tab(tab_id, tab.dock_width, tab.dock_height);
}

}  // namespace

void UiFrameRunner::dispatch_keyboard_to_active_tab(int active_tab_id) {
    if (active_tab_id != -1 && url_input_active_tab_ == -1) {
        auto* tab = BrowserManager::instance().get_tab(active_tab_id);
        if (tab && tab->client && tab->client->GetBrowser()) {
            tab->client->GetBrowser()->GetHost()->SetFocus(true);
            input_handler_.send_key_events_to_browser(tab->client->GetBrowser());
            return;
        }
    }
    input_handler_.clear_pending_events();
}

void UiFrameRunner::set_system_cursor(SDL_SystemCursor cursor) {
    if (active_system_cursor_ == cursor) {
        return;
    }

    SDL_Cursor* next = cached_system_cursor(cursor);
    if (next != nullptr && SDL_SetCursor(next)) {
        active_system_cursor_ = cursor;
    }
}

void UiFrameRunner::route_mouse_to_panels(SDL_WindowID window_id,
                                          const std::vector<PanelPlacement>& placements,
                                          int& active_tab_id) {
    // Build hit targets with an explicit z-order: the main panel is always bottom-most and
    // docked panels sit on top. hit_test() scans last-first (topmost wins), so we must push
    // the main panel FIRST regardless of the unordered_map iteration order of get_tabs() —
    // otherwise a click on a docked panel can fall through to the full-screen main panel.
    std::vector<HitTarget> targets;
    targets.reserve(placements.size());
    auto append_target = [&](const PanelPlacement& placement) {
        HitTarget target;
        target.tab_id = placement.tab_id;
        target.rect = placement.rect;
        target.is_main = placement.is_main;
        targets.push_back(target);
    };
    for (const PanelPlacement& placement : placements) {
        if (placement.is_main) {
            append_target(placement);
        }
    }
    for (const PanelPlacement& placement : placements) {
        if (!placement.is_main) {
            append_target(placement);
        }
    }

    const HitResult hit = input_router_.hit_test(window_id, targets);
    const InputState st = input_router_.state(window_id);
    SDL_Window* main_sdl_window = SdlWindowManager::instance().main_window();
    const bool is_main_window =
        main_sdl_window != nullptr && SDL_GetWindowID(main_sdl_window) == window_id;

    // Drain button transitions regardless of hit, so a release outside the panel still
    // closes a click that began inside it (mirrors the old was_down/is_active handling).
    std::vector<ButtonEvent> button_events = input_router_.drain_button_events(window_id);
    const float wheel = input_router_.consume_wheel(window_id);

    // ---- Phase 10: move/resize of an in-main-window floating panel -------------------------
    // Floating panels live only in the main window (host_surface == nullptr). A left-press on a
    // floating panel's edge/corner starts native resize. A left-press on its drag region starts
    // native move. In both modes, mouse events are consumed here so the operation keeps working
    // even after the cursor leaves the panel's previous CEF rectangle.

    bool ended_resize_this_frame = false;
    bool ended_drag_this_frame = false;
    for (const ButtonEvent& be : button_events) {
        if (!is_main_window || be.button != MouseButton::Left) {
            continue;
        }

        if (!be.pressed) {
            if (resizing_tab_id_ != -1) {
                resizing_tab_id_ = -1;
                resize_edges_ = 0;
                ended_resize_this_frame = true;
                continue;
            }
            if (dragging_tab_id_ != -1) {
                dragging_tab_id_ = -1;
                ended_drag_this_frame = true;
                continue;
            }
        }

        if (!be.pressed || resizing_tab_id_ != -1 || dragging_tab_id_ != -1 ||
            !hit.hit || hit.is_main) {
            continue;
        }

        auto* dtab = BrowserManager::instance().get_tab(hit.tab_id);
        if (!dtab || !dtab->floating) {
            continue;
        }

        const int resize_edges = floating_resize_edges(*dtab, hit);
        if (resize_edges != 0) {
            active_tab_id = hit.tab_id;
            url_input_active_tab_ = -1;
            focus_browser_tab_exclusively(hit.tab_id);
            resizing_tab_id_ = hit.tab_id;
            resize_edges_ = resize_edges;
            resize_mouse_start_x_ = be.mouse_x;
            resize_mouse_start_y_ = be.mouse_y;
            resize_rect_start_x_ = static_cast<float>(dtab->initial_x);
            resize_rect_start_y_ = static_cast<float>(dtab->initial_y);
            resize_rect_start_w_ = static_cast<float>(std::max(dtab->dock_width, dtab->width));
            resize_rect_start_h_ = static_cast<float>(std::max(dtab->dock_height, dtab->height));
            continue;
        }

        // The default in-main floating drag region covers the whole title bar.  Keep the
        // native move gesture away from the Vue title-bar actions, otherwise the native
        // handler consumes the button-down/up pair before CEF can deliver the click.
        constexpr float kTitlebarHeight = 32.0f;
        constexpr float kTitlebarActionReserve = 80.0f;
        const float panel_width = static_cast<float>(std::max(dtab->width, dtab->dock_width));
        const bool over_titlebar_actions =
            hit.local_y >= 0.0f && hit.local_y <= kTitlebarHeight &&
            hit.local_x >= panel_width - kTitlebarActionReserve && hit.local_x <= panel_width;
        if (hit.in_drag_region && !over_titlebar_actions) {
            active_tab_id = hit.tab_id;
            url_input_active_tab_ = -1;
            focus_browser_tab_exclusively(hit.tab_id);
            if (dtab && dtab->floating) {
                dragging_tab_id_ = hit.tab_id;
                drag_mouse_start_x_ = be.mouse_x;
                drag_mouse_start_y_ = be.mouse_y;
                drag_rect_start_x_ = static_cast<float>(dtab->initial_x);
                drag_rect_start_y_ = static_cast<float>(dtab->initial_y);
            }
        }
    }

    int cursor_edges = 0;
    if (is_main_window && resizing_tab_id_ != -1) {
        cursor_edges = resize_edges_;
    } else if (is_main_window && dragging_tab_id_ == -1 && hit.hit && !hit.is_main) {
        auto* hover_tab = BrowserManager::instance().get_tab(hit.tab_id);
        if (hover_tab && hover_tab->floating) {
            cursor_edges = floating_resize_edges(*hover_tab, hit);
        }
    }
    set_system_cursor(cursor_for_resize_edges(cursor_edges));

    if (is_main_window && resizing_tab_id_ != -1) {
        auto* dtab = BrowserManager::instance().get_tab(resizing_tab_id_);
        if (!dtab || !dtab->floating) {
            resizing_tab_id_ = -1;
            resize_edges_ = 0;
        } else {
            apply_floating_resize(*dtab,
                                  resizing_tab_id_,
                                  resize_edges_,
                                  resize_rect_start_x_,
                                  resize_rect_start_y_,
                                  resize_rect_start_w_,
                                  resize_rect_start_h_,
                                  st.mouse_x - resize_mouse_start_x_,
                                  st.mouse_y - resize_mouse_start_y_,
                                  window_id);
            return;
        }
    }
    if (ended_resize_this_frame) {
        return;
    }

    // While dragging: move the panel and consume all input (no CEF forwarding this frame).
    if (is_main_window && dragging_tab_id_ != -1) {
        auto* dtab = BrowserManager::instance().get_tab(dragging_tab_id_);
        if (!dtab || !dtab->floating) {
            dragging_tab_id_ = -1;
        } else {
            int win_w = 0;
            int win_h = 0;
            if (SDL_Window* w = SdlWindowManager::instance().window_for_id(window_id)) {
                SDL_GetWindowSize(w, &win_w, &win_h);
            }
            float nx = drag_rect_start_x_ + (st.mouse_x - drag_mouse_start_x_);
            float ny = drag_rect_start_y_ + (st.mouse_y - drag_mouse_start_y_);
            // Clamp so at least a strip of the panel stays on-screen (title bar grabbable).
            if (win_w > 0 && win_h > 0) {
                const float kMargin = 40.0f;
                nx = std::clamp(nx, -static_cast<float>(dtab->width) + kMargin,
                                static_cast<float>(win_w) - kMargin);
                ny = std::clamp(ny, 0.0f, static_cast<float>(win_h) - kMargin);
            }
            dtab->initial_x = static_cast<int>(std::lround(nx));
            dtab->initial_y = static_cast<int>(std::lround(ny));
            return;  // do not forward to CEF while dragging
        }
    }
    // The release that ended a drag must not also reach CEF as a stray click.
    if (ended_drag_this_frame) {
        return;
    }
    // ----------------------------------------------------------------------------------------

    // Determine which tab (if any) receives mouse events.
    auto* tab = (hit.hit && hit.tab_id != -1) ? BrowserManager::instance().get_tab(hit.tab_id) : nullptr;
    auto browser = (tab && tab->client) ? tab->client->GetBrowser() : nullptr;
    if (!browser) {
        return;
    }

    // Panel origin = global mouse - panel-local mouse (from hit_test).
    const float item_x = st.mouse_x - hit.local_x;
    const float item_y = st.mouse_y - hit.local_y;

    // On any click into this panel, make it the active tab and focus it exclusively.
    auto to_cef_button = [](MouseButton b) {
        switch (b) {
            case MouseButton::Right: return MBT_RIGHT;
            case MouseButton::Middle: return MBT_MIDDLE;
            case MouseButton::Left:
            default: return MBT_LEFT;
        }
    };

    for (const ButtonEvent& be : button_events) {
        if (be.pressed) {
            active_tab_id = hit.tab_id;
            url_input_active_tab_ = -1;
            focus_browser_tab_exclusively(hit.tab_id);
        }
        MouseUtils::send_mouse_click_ex(browser, be.mouse_x, be.mouse_y, item_x, item_y,
                                        to_cef_button(be.button), /*mouse_up=*/!be.pressed,
                                        be.click_count, st.shift, st.ctrl, st.alt);
    }

    // Mouse move (always treated as hovering the hit panel here).
    MouseUtils::send_mouse_move_ex(browser, st.mouse_x, st.mouse_y, item_x, item_y,
                                   st.left_down, st.right_down, st.shift, st.ctrl, st.alt,
                                   /*mouse_leave=*/false);

    // Wheel.
    if (wheel != 0.0f) {
        MouseUtils::send_mouse_wheel_ex(browser, st.mouse_x, st.mouse_y, item_x, item_y, wheel,
                                        st.left_down, st.right_down, st.shift, st.ctrl, st.alt);
    }
}

void UiFrameRunner::run_frame(UiFrameContext& context) {
    if (!context.running || !context.active_tab_id || !context.vulkan_backend) {
        return;
    }

    auto& window_manager = SdlWindowManager::instance();

    // 1) Pump SDL events ONCE for all windows. Mouse/wheel events carry a windowID and are
    //    bucketed per window by the input router; keyboard/text/IME go to the focused tab.
    input_router_.refresh_modifiers();
    auto result = event_handler_.process_events(
        context.window, url_input_active_tab_,
        [&](const SDL_Event& event) { input_handler_.process_sdl_key_event(event); },
        [&](const SDL_Event& event) { input_handler_.process_sdl_text_event(event); },
        [&](const SDL_Event& event) { input_handler_.process_sdl_ime_event(event); },
        [&](const SDL_Event& event) { input_router_.process_event(event); });

    if (result.should_quit) {
        *context.running = false;
    }

    if (result.window_resized && context.window && context.window_size_changed) {
        *context.window_size_changed = true;
        context.vulkan_backend->request_rebuild();
    }

    // A secondary (detached) window was closed by the user (its OS close button). Camera views
    // are standalone render targets, so closing their window closes the tab. A regular detached
    // panel still redocks. Camera tabs are marked closed here and are torn down by the common
    // close path below, before their surface can be rebound to the main window.
    for (const SDL_WindowID closed_id : result.closed_window_ids) {
        const ManagedWindow* mw = SdlWindowManager::instance().find_by_id(closed_id);
        if (mw == nullptr || mw->surface == nullptr) {
            continue;
        }
        for (auto& [tab_id, tab] : BrowserManager::instance().get_tabs()) {
            if (tab && tab->host_surface == mw->surface &&
                tab->detach_state == BrowserTab::DetachState::Detached) {
                if (detached_window_close_action(tab->camera_view) ==
                    DetachedWindowCloseAction::CloseTab) {
                    tab->open = false;
                    CFW_LOG_INFO("CameraViewport: OS close requested tab {}; closing camera view", tab_id);
                } else {
                    tab->detach_state = BrowserTab::DetachState::Redocking;
                }
                break;
            }
        }
    }

    // 2) Forward queued keyboard/text/IME to the active tab (whichever window it lives in).
    dispatch_keyboard_to_active_tab(*context.active_tab_id);

    // 3) Drive browser texture updates + collect closed tabs (once for all tabs). This also
    //    drains one queued main-thread task (e.g. a detachPanel/redockPanel state flip).
    BrowserManager::instance().update();

    // 4) Reconcile detach/redock intent to actual window + surface state (UI thread is the sole
    //    owner of the window set and the backend). Runs AFTER update() drained the state flip,
    //    and BEFORE the render loop so a newly-created window is rendered this frame and a
    //    redocked window is already gone.
    reconcile_detach_states(context);
    // printf("[FRAME_DEBUG] After reconcile_detach_states\n"); fflush(stdout);

    // printf("[FRAME_DEBUG] Before BrowserManager tabs loop\n"); fflush(stdout);
    std::vector<int> tabs_to_close;
    for (auto& [tab_id, tab] : BrowserManager::instance().get_tabs()) {
        if (!tab || !tab->open) {
            tabs_to_close.push_back(tab_id);
            continue;
        }
        if (tab->minimized) {
            continue;
        }
        BrowserManager::instance().update_texture(tab_id);
    }

    // 5) Render every window (main + detached). Snapshot the window list first, since
    //    render_window does not mutate it (detach/redock already reconciled above).
    // printf("[FRAME_DEBUG] After tabs loop\n"); fflush(stdout);
    // printf("[FRAME_DEBUG] Before window snapshot\n"); fflush(stdout);
    std::vector<ManagedWindow> windows;
    window_manager.for_each_window([&](const ManagedWindow& w) { windows.push_back(w); });
    for (const ManagedWindow& managed : windows) {
        // printf("[FRAME_DEBUG] Before render_window loop\n"); fflush(stdout);
        render_window(context, managed);
    }

    // Upload CEF paint buffers after all windows have routed this frame's input. The upload
    // executor may wait on the previous receipt; doing that before route_mouse_to_panels makes
    // Vue drag/click latency scale with the number of secondary surfaces.
    for (const auto& [tab_id, tab] : BrowserManager::instance().get_tabs()) {
        if (!tab || !tab->open || tab->minimized) {
            continue;
        }
        BrowserManager::instance().update_texture(tab_id);
    }

    // 6) Close tabs flagged closed. A closed tab that is currently detached owns an OS window
    //    + a registered surface; those must be torn down (promise-synced, same order as redock)
    //    BEFORE remove_tab destroys the tab, or we leak the window / present to a dead surface.
    auto& close_window_manager = SdlWindowManager::instance();
    for (int tab_id : tabs_to_close) {
        auto* tab = BrowserManager::instance().get_tab(tab_id);
        if (tab != nullptr && tab->host_surface != nullptr) {
            void* surface = tab->host_surface;
            tab->host_surface = nullptr;
            tab->platform_window_id = 0;
            tab->platform_handle_raw = nullptr;
            tab->detach_state = BrowserTab::DetachState::Docked;
            if (close_window_manager.request_remove_secondary_window(surface)) {
                context.vulkan_backend->unregister_surface(surface);
                close_window_manager.destroy_secondary_window(surface);
            }
        }
        BrowserManager::instance().remove_tab(tab_id);
        if (tab_id == *context.active_tab_id) {
            *context.active_tab_id = -1;
        }
        if (tab_id == url_input_active_tab_) {
            url_input_active_tab_ = -1;
        }
    }
}

void UiFrameRunner::reconcile_detach_states(UiFrameContext& context) {
    // The UI thread is the sole owner of the window set + the backend, so all window create/
    // destroy + surface register/unregister happens here. bridge commands only flipped the
    // per-tab detach_state (Detaching / Redocking); this turns that intent into reality.
    //
    // Collect target tab ids first: the reconcile actions mutate BrowserManager's tab map
    // indirectly (resize_tab) and the window set, so we avoid iterating the map while acting.
    std::vector<int> detaching;
    std::vector<int> redocking;
    for (const auto& [tab_id, tab] : BrowserManager::instance().get_tabs()) {
        if (!tab) {
            continue;
        }
        if (tab->detach_state == BrowserTab::DetachState::Detaching) {
            detaching.push_back(tab_id);
        } else if (tab->detach_state == BrowserTab::DetachState::Redocking) {
            redocking.push_back(tab_id);
        }
    }

    auto& window_manager = SdlWindowManager::instance();

    // --- Detaching: Docked->Detaching (by bridge) -> create window + register surface -> Detached.
    for (const int tab_id : detaching) {
        auto* tab = BrowserManager::instance().get_tab(tab_id);
        if (!tab) {
            continue;
        }

        if (tab->cef_creation_failed) {
            CFW_LOG_ERROR("reconcile: tab {} cannot detach because CEF creation failed", tab_id);
            tab->detach_state = BrowserTab::DetachState::Docked;
            tab->open = false;
            continue;
        }
        if (tab->client == nullptr || tab->client->GetBrowser() == nullptr) {
            // CEF creation is asynchronous. Keep Detaching pending until OnAfterCreated;
            // creating a secondary surface without a browser would produce an empty window.
            CFW_LOG_DEBUG("reconcile: tab {} waiting for CEF browser before detach", tab_id);
            continue;
        }

        if (tab->detach_maximized) {
            const SDL_DisplayID display_id = SDL_GetPrimaryDisplay();
            SDL_Rect usable_bounds{};
            if (display_id != 0 && SDL_GetDisplayUsableBounds(display_id, &usable_bounds)) {
                tab->detach_x = usable_bounds.x;
                tab->detach_y = usable_bounds.y;
                tab->detach_w = std::max(1, usable_bounds.w);
                tab->detach_h = std::max(1, usable_bounds.h);
            }
        }

        void* surface = window_manager.create_secondary_window(
            tab->detach_x, tab->detach_y, tab->detach_w, tab->detach_h);
        if (surface == nullptr) {
            CFW_LOG_ERROR("reconcile: failed to create secondary window for tab {}; reverting to Docked",
                          tab_id);
            tab->detach_state = BrowserTab::DetachState::Docked;
            continue;
        }

        CFW_LOG_INFO("reconcile: tab {} secondary SDL window created (surface={}, size={}x{}, pos=({}, {}))",
                     tab_id, surface, tab->detach_w, tab->detach_h, tab->detach_x, tab->detach_y);

        const ManagedWindow* mw = window_manager.find_by_surface(surface);
        SDL_Window* sdl_window = mw ? mw->window : nullptr;
        if (!context.vulkan_backend->register_surface(surface, sdl_window)) {
            CFW_LOG_ERROR("reconcile: failed to register surface for tab {}; tearing window back down",
                          tab_id);
            window_manager.request_remove_secondary_window(surface);
            window_manager.destroy_secondary_window(surface);
            tab->detach_state = BrowserTab::DetachState::Docked;
            continue;
        }

        CFW_LOG_INFO("reconcile: tab {} Vulkan surface registered (surface={})", tab_id, surface);

        tab->host_surface = surface;
        tab->platform_window_id = mw ? mw->window_id : 0;
        tab->platform_handle_raw = surface;
        tab->detach_state = BrowserTab::DetachState::Detached;

        // Enable title-bar drag + edge-resize on the borderless window. The callback reads
        // tab->drag_regions (the Vue-reported title-bar rects) keyed by this tab id.
        window_manager.enable_drag_hit_test(surface, tab_id);

        CFW_LOG_INFO("reconcile: tab {} detached to surface {} (state=Detached)", tab_id, surface);
    }

    // --- Redocking: Detached->Redocking (by bridge) -> clear host + unregister + destroy -> Docked.
    for (const int tab_id : redocking) {
        auto* tab = BrowserManager::instance().get_tab(tab_id);
        if (!tab) {
            continue;
        }

        void* surface = tab->host_surface;

        // Detach the tab from its window FIRST so render_window stops drawing into this surface
        // this frame; then release GPU resources, then the OS window (promise-synced).
        tab->host_surface = nullptr;
        tab->platform_window_id = 0;
        tab->platform_handle_raw = nullptr;
        if (tab->camera_view) {
            CameraViewportManager::instance().bind_surface(tab_id, nullptr, 0, 0,
                                                            tab->width, tab->height);
        }

        if (surface != nullptr) {
            if (window_manager.request_remove_secondary_window(surface)) {
                context.vulkan_backend->unregister_surface(surface);
                window_manager.destroy_secondary_window(surface);
            }
        }

        tab->detach_state = BrowserTab::DetachState::Docked;
        tab->detach_maximized = false;
        CFW_LOG_INFO("reconcile: tab {} redocked (surface {} destroyed)", tab_id, surface);
    }
}

void UiFrameRunner::render_window(UiFrameContext& context, const ManagedWindow& managed) {
    SDL_Window* const sdl_window = managed.window;
    void* const surface = managed.surface;
    if (sdl_window == nullptr || surface == nullptr) {
        return;
    }
    if (!context.vulkan_backend->has_surface(surface)) {
        return;  // surface not yet registered (e.g. mid-detach)
    }

    const SDL_WindowID window_id = managed.window_id;

    // Rebuild this window's render target on resize. The main window uses the shared
    // rebuild_needed_ flag; secondary windows always reconcile to their current pixel size.
    int pixel_iw = 0;
    int pixel_ih = 0;
    if (!SDL_GetWindowSizeInPixels(sdl_window, &pixel_iw, &pixel_ih) || pixel_iw <= 0 || pixel_ih <= 0) {
        SDL_GetWindowSize(sdl_window, &pixel_iw, &pixel_ih);
    }
    if (pixel_iw <= 0 || pixel_ih <= 0) {
        return;  // window not ready this frame
    }

    const bool main_window = managed.is_main;
    const bool needs_rebuild =
        main_window ? context.vulkan_backend->is_rebuild_needed()
                    : (context.vulkan_backend->surface_width(surface) != static_cast<uint32_t>(pixel_iw) ||
                       context.vulkan_backend->surface_height(surface) != static_cast<uint32_t>(pixel_ih));
    if (needs_rebuild) {
        context.vulkan_backend->rebuild(surface, static_cast<uint32_t>(pixel_iw),
                                        static_cast<uint32_t>(pixel_ih));
    }

    context.vulkan_backend->new_frame(surface);

    // Logical size for layout / CEF / hit-testing (see coordinate contract below).
    int logical_w = 0;
    int logical_h = 0;
    SDL_GetWindowSize(sdl_window, &logical_w, &logical_h);
    if (logical_w <= 0 || logical_h <= 0) {
        return;
    }

    const uint32_t pixel_w = context.vulkan_backend->surface_width(surface);
    const uint32_t pixel_h = context.vulkan_backend->surface_height(surface);
    const float scale_x = (pixel_w > 0) ? static_cast<float>(pixel_w) / static_cast<float>(logical_w) : 1.0f;
    const float scale_y = (pixel_h > 0) ? static_cast<float>(pixel_h) / static_cast<float>(logical_h) : 1.0f;

    // Coordinate contract (matches the former ImGui path):
    //   - Layout, hit-testing, CEF buffer sizes, and CEF-local mouse coords are LOGICAL.
    //   - Only the GPU render target and camera surface bounds are PHYSICAL pixels, obtained
    //     by scaling the logical rects with this window's own DPI scale.
    const WorkArea work = make_client_work_area(logical_w, logical_h);

    std::vector<PanelPlacement> placements;
    if (main_window) {
        // Main window: all tabs whose host_surface == nullptr, via the docking layout.
        const std::vector<PanelLayoutInput> inputs = collect_layout_inputs(nullptr);
        placements = compute_panel_layout(work, inputs);
    } else {
        // Secondary window: the single tab hosted here fills the whole client area.
        for (const auto& [tab_id, tab] : BrowserManager::instance().get_tabs()) {
            if (!tab || !tab->open || tab->minimized || tab->host_surface != surface ||
                tab->detach_state != BrowserTab::DetachState::Detached) {
                continue;
            }
            PanelPlacement p;
            p.tab_id = tab_id;
            p.is_main = false;
            p.rect = LayoutRect{work.x, work.y, work.width, work.height};
            placements.push_back(p);
            break;  // one panel per detached window
        }
    }

    // Resize each tab's CEF buffer to its LOGICAL panel size; bind camera viewports onto THIS
    // window's surface using PIXEL bounds.
    for (const PanelPlacement& placement : placements) {
        auto* tab = BrowserManager::instance().get_tab(placement.tab_id);
        if (!tab) {
            continue;
        }

        const int lw = std::max(1, static_cast<int>(std::lround(placement.rect.w)));
        const int lh = std::max(1, static_cast<int>(std::lround(placement.rect.h)));
        if (lw != tab->width || lh != tab->height) {
            BrowserManager::instance().resize_tab(placement.tab_id, lw, lh);
        }

        if (!tab->camera_view) {
            continue;
        }

        const bool valid_camera_binding =
            (main_window && tab->detach_state == BrowserTab::DetachState::Docked &&
             tab->host_surface == nullptr) ||
            (!main_window && tab->detach_state == BrowserTab::DetachState::Detached &&
             tab->host_surface == surface);
        if (!valid_camera_binding) {
            CFW_LOG_DEBUG(
                "CameraViewport: skip bind tab={} detach_state={} host_surface={} target_surface={}",
                placement.tab_id, static_cast<int>(tab->detach_state), tab->host_surface,
                surface);
            continue;
        }

        const int px = static_cast<int>(std::lround(placement.rect.x * scale_x));
        const int py = static_cast<int>(std::lround(placement.rect.y * scale_y));
        const int pw = std::max(1, static_cast<int>(std::lround(placement.rect.w * scale_x)));
        const int ph = std::max(1, static_cast<int>(std::lround(placement.rect.h * scale_y)));
        tab->platform_handle_raw = surface;
        tab->platform_window_id = window_id;
        CameraViewportManager::instance().bind_surface(placement.tab_id, surface, px, py, pw, ph);
        CameraViewportManager::instance().update_layout(placement.tab_id, px, py, pw, ph);
    }

    // Route this window's mouse input (per-window bucket) to the hit panel's browser.
    route_mouse_to_panels(window_id, placements, *context.active_tab_id);

    // Build quad list (PIXEL dest rects) and render + present this window's surface.
    // Draw order = z-order (compositor draws array order, no z-sort): the main panel is the
    // full-screen bottom layer, floating panels stack on top, and the panel being dragged is
    // topmost. Without this, the main full-screen quad (random unordered_map order) can paint
    // over a floating panel and make it "disappear".
    std::vector<QuadDraw> quads;
    quads.reserve(placements.size());
    auto append_quad = [&](const PanelPlacement& placement) {
        auto* tab = BrowserManager::instance().get_tab(placement.tab_id);
        if (!tab || !is_valid_texture_id(tab->texture_id)) {
            return;
        }
        const Horizon::HardwareImage* image =
            BrowserManager::instance().get_texture_image(tab->texture_id);
        if (image == nullptr) {
            return;
        }
        QuadDraw quad;
        quad.texture = image;
        quad.texture_ready =
            BrowserManager::instance().get_texture_upload_receipt(tab->texture_id);
        quad.dest_min = ktm::fvec2(placement.rect.x * scale_x, placement.rect.y * scale_y);
        quad.dest_max = ktm::fvec2((placement.rect.x + placement.rect.w) * scale_x,
                                   (placement.rect.y + placement.rect.h) * scale_y);
        quads.push_back(quad);
    };
    // 1) main (bottom), 2) non-main except active move/resize panel, 3) active panel (topmost).
    const int active_floating_tab_id =
        resizing_tab_id_ != -1 ? resizing_tab_id_ : dragging_tab_id_;
    for (const PanelPlacement& placement : placements) {
        if (placement.is_main) append_quad(placement);
    }
    for (const PanelPlacement& placement : placements) {
        if (!placement.is_main && placement.tab_id != active_floating_tab_id) append_quad(placement);
    }
    for (const PanelPlacement& placement : placements) {
        if (!placement.is_main && placement.tab_id == active_floating_tab_id) append_quad(placement);
    }

    context.vulkan_backend->render_quads(surface, quads, pixel_w, pixel_h);
    context.vulkan_backend->present_surface(surface);

    // Deferred show: reveal a freshly-detached window once its first frame is published.
    if (managed.pending_show && context.vulkan_backend->first_present_ready(surface)) {
        SdlWindowManager::instance().reveal_pending_window(surface);
    }
}

}  // namespace Corona::Systems::UI
