#include <corona/systems/ui/sdl_window_manager.h>

#include <corona/events/display_system_events.h>
#include <horizon/core/logging.h>
#include <corona/kernel/core/kernel_context.h>
#include <corona/kernel/event/i_event_bus.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>

#include "cef/browser_manager.h"

namespace Corona::Systems::UI {

namespace {

// Edge margin (logical px) for resize hit-testing on borderless secondary windows.
constexpr int kResizeBorder = 6;

// SDL hit-test callback for detached (borderless) secondary windows. Runs on the UI thread
// during SDL event processing. Must be allocation-free and efficient (SDL contract).
//
// Priority: window edges -> RESIZE_* (8-way); else Vue title-bar drag regions -> DRAGGABLE;
// else NORMAL (panel body stays interactive). `data` carries the tab id (intptr_t).
//
// `area` is in window logical coordinates; the detached panel fills the window single-bleed,
// and Vue reports drag regions in the same logical pixels, so no scaling is needed.
SDL_HitTestResult SDLCALL secondary_window_hit_test(SDL_Window* win, const SDL_Point* area,
                                                    void* data) {
    if (win == nullptr || area == nullptr) {
        return SDL_HITTEST_NORMAL;
    }

    int w = 0;
    int h = 0;
    SDL_GetWindowSize(win, &w, &h);
    if (w > 0 && h > 0) {
        const bool left = area->x < kResizeBorder;
        const bool right = area->x >= w - kResizeBorder;
        const bool top = area->y < kResizeBorder;
        const bool bottom = area->y >= h - kResizeBorder;
        if (top && left) return SDL_HITTEST_RESIZE_TOPLEFT;
        if (top && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
        if (bottom && left) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (top) return SDL_HITTEST_RESIZE_TOP;
        if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;
        if (left) return SDL_HITTEST_RESIZE_LEFT;
        if (right) return SDL_HITTEST_RESIZE_RIGHT;

        // The Vue title bar reports its whole header as draggable, but the right-side
        // float/close buttons must remain normal client hit-test targets so SDL/CEF receive
        // their mouse events instead of Windows consuming them as a native drag.
        constexpr int kTitlebarHeight = 32;
        constexpr int kTitlebarActionReserve = 80;
        if (area->y >= kResizeBorder && area->y < kTitlebarHeight &&
            area->x >= w - kTitlebarActionReserve) {
            return SDL_HITTEST_NORMAL;
        }
    }

    const int tab_id = static_cast<int>(reinterpret_cast<std::intptr_t>(data));
    auto* tab = BrowserManager::instance().get_tab(tab_id);
    if (tab == nullptr) {
        return SDL_HITTEST_NORMAL;
    }

    // drag_regions is written by setDragRegions on the CEF IPC thread; lock to read.
    std::lock_guard<std::mutex> lock(tab->drag_mutex);
    for (const DragRegion& region : tab->drag_regions) {
        if (static_cast<float>(area->x) >= region.x &&
            static_cast<float>(area->y) >= region.y &&
            static_cast<float>(area->x) < region.x + region.width &&
            static_cast<float>(area->y) < region.y + region.height) {
            return SDL_HITTEST_DRAGGABLE;
        }
    }
    return SDL_HITTEST_NORMAL;
}

}  // namespace

SdlWindowManager& SdlWindowManager::instance() {
    static SdlWindowManager manager;
    return manager;
}

void* native_surface_from_sdl_window(SDL_Window* window) {
    if (window == nullptr) {
        return nullptr;
    }
    void* native_handle = nullptr;
#if defined(_WIN32)
    native_handle = SDL_GetPointerProperty(
        SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER,
        nullptr);
#elif defined(__APPLE__)
    native_handle = SDL_GetPointerProperty(
        SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
        nullptr);
#endif
    return native_handle;
}

bool SdlWindowManager::adopt_main_window(SDL_Window* window) {
    if (window == nullptr) {
        CFW_LOG_ERROR("SdlWindowManager: adopt_main_window called with null window");
        return false;
    }
    const SDL_WindowID window_id = SDL_GetWindowID(window);
    if (const auto it = windows_.find(window_id);
        it != windows_.end() && it->second.removal_acknowledged) {
        return true;
    }

    void* surface = native_surface_from_sdl_window(window);
    if (surface == nullptr) {
        CFW_LOG_ERROR("SdlWindowManager: failed to resolve native surface for main window");
        return false;
    }

    main_window_ = window;
    main_window_id_ = window_id;

    ManagedWindow managed;
    managed.window = window;
    managed.window_id = main_window_id_;
    managed.surface = surface;
    managed.is_main = true;
    windows_[main_window_id_] = managed;

    CFW_LOG_INFO("SdlWindowManager: adopted main window (id={})", main_window_id_);
    return true;
}

void* SdlWindowManager::main_surface() const {
    const auto it = windows_.find(main_window_id_);
    return it != windows_.end() ? it->second.surface : nullptr;
}

SDL_Window* SdlWindowManager::window_for_id(SDL_WindowID window_id) const {
    const auto it = windows_.find(window_id);
    return it != windows_.end() ? it->second.window : nullptr;
}

const ManagedWindow* SdlWindowManager::find_by_id(SDL_WindowID window_id) const {
    const auto it = windows_.find(window_id);
    return it != windows_.end() ? &it->second : nullptr;
}

const ManagedWindow* SdlWindowManager::find_by_surface(void* surface) const {
    if (surface == nullptr) {
        return nullptr;
    }
    for (const auto& [id, managed] : windows_) {
        if (managed.surface == surface) {
            return &managed;
        }
    }
    return nullptr;
}

WindowClientRect SdlWindowManager::client_rect(SDL_Window* window) const {
    if (window == nullptr) {
        return {};
    }

    int width = 0;
    int height = 0;
    if (SDL_GetWindowSizeInPixels(window, &width, &height) && width > 0 && height > 0) {
        return {width, height};
    }

    width = 0;
    height = 0;
    if (SDL_GetWindowSize(window, &width, &height) && width > 0 && height > 0) {
        return {width, height};
    }

    return {};
}

void SdlWindowManager::for_each_window(const std::function<void(const ManagedWindow&)>& fn) const {
    if (!fn) {
        return;
    }
    for (const auto& [id, managed] : windows_) {
        fn(managed);
    }
}

// --- Secondary windows (Phase 7: detach / re-dock). ---

void* SdlWindowManager::create_secondary_window(int x, int y, int width, int height) {
    const int safe_w = std::max(width, 1);
    const int safe_h = std::max(height, 1);

    // Created HIDDEN; revealed by reveal_pending_window() after the first frame is published.
    // BORDERLESS: no OS title bar/border — Vue draws its own title bar, and SDL_SetWindowHitTest
    // (registered at detach time) makes the Vue title-bar region draggable and the window edges
    // resizable. RESIZABLE is kept so the RESIZE_* hit-test results take effect.
    SDL_Window* window = SDL_CreateWindow("Corona Panel", safe_w, safe_h,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIDDEN);
    if (window == nullptr) {
        CFW_LOG_ERROR("SdlWindowManager: failed to create secondary window: {}", SDL_GetError());
        return nullptr;
    }

    SDL_SetWindowPosition(window, x, y);
    SDL_StartTextInput(window);

    void* surface = native_surface_from_sdl_window(window);
    if (surface == nullptr) {
        CFW_LOG_ERROR("SdlWindowManager: failed to resolve native surface for secondary window");
        SDL_DestroyWindow(window);
        return nullptr;
    }

    const SDL_WindowID window_id = SDL_GetWindowID(window);
    ManagedWindow managed;
    managed.window = window;
    managed.window_id = window_id;
    managed.surface = surface;
    managed.is_main = false;
    managed.pending_show = true;
    windows_[window_id] = managed;

    // Register the surface so DisplaySystem creates the per-surface HardwareDisplayer that
    // owns this window's swapchain (UI composited over transparent optics).
    if (auto* event_bus = Kernel::KernelContext::instance().event_bus()) {
        Events::DisplaySurfaceChangedEvent changed;
        changed.surface = surface;
        changed.registration_ticket = Corona::Systems::UI::SurfaceCompletionTicket{};
        event_bus->publish<Events::DisplaySurfaceChangedEvent>(changed);
        if (!changed.registration_ticket->wait_until(
                Corona::Systems::UI::SurfaceCompletionTicket::Clock::now() +
                    std::chrono::seconds(5))) {
            CFW_LOG_ERROR("SdlWindowManager: registration timed out for surface {}", surface);
            SDL_HideWindow(window);
            windows_[window_id].removal_failed = true;
            return nullptr;
        }
        const auto registration = changed.registration_ticket->result();
        if (!registration || registration->status != DisplaySurfaceResult::Status::Succeeded) {
            CFW_LOG_ERROR("SdlWindowManager: registration failed for surface {}", surface);
            SDL_HideWindow(window);
            windows_[window_id].removal_failed = true;
            return nullptr;
        }
    }

    CFW_LOG_INFO("SdlWindowManager: secondary window created (id={}, surface={})",
                 window_id, surface);
    return surface;
}

void SdlWindowManager::enable_drag_hit_test(void* surface, int tab_id) {
    if (surface == nullptr) {
        return;
    }
    const ManagedWindow* mw = find_by_surface(surface);
    if (mw == nullptr || mw->window == nullptr) {
        CFW_LOG_WARNING("SdlWindowManager: enable_drag_hit_test: surface {} not found", surface);
        return;
    }
    // callback_data carries the tab id; the callback resolves drag regions via BrowserManager.
    if (!SDL_SetWindowHitTest(mw->window, &secondary_window_hit_test,
                              reinterpret_cast<void*>(static_cast<std::intptr_t>(tab_id)))) {
        CFW_LOG_WARNING("SdlWindowManager: SDL_SetWindowHitTest failed for surface {}: {}",
                        surface, SDL_GetError());
    }
}

bool SdlWindowManager::request_remove_surface(void* surface, std::chrono::milliseconds timeout) {
    if (surface == nullptr) {
        return false;
    }

    SDL_Window* window = nullptr;
    SDL_WindowID window_id = 0;
    for (const auto& [id, managed] : windows_) {
        if (managed.surface == surface) {
            window = managed.window;
            window_id = id;
            break;
        }
    }
    if (window == nullptr) {
        CFW_LOG_WARNING("SdlWindowManager: destroy_secondary_window: surface {} not found", surface);
        return false;
    }
    if (windows_[window_id].removal_acknowledged) return true;

    // DisplaySystem owns the swapchain + VkSurfaceKHR for this surface. It must tear that down
    // (GPU idle + destroy) BEFORE we destroy the OS window, or the Display thread could present
    // to a dead window. EventBus dispatch is synchronous on THIS (UI) thread, so the handler
    // only buffers the request; the actual teardown + promise fulfillment happens on the
    // Display thread's update(). Block on the promise so destruction is ordered. Bounded wait
    // avoids a hang if the Display thread has already stopped (e.g. during shutdown).
    if (auto* event_bus = Kernel::KernelContext::instance().event_bus()) {
        auto done = std::make_shared<std::promise<void>>();
        auto fut = done->get_future();
        event_bus->publish<Events::DisplaySurfaceRemovedEvent>({surface, done});
        if (fut.wait_for(timeout) != std::future_status::ready) {
            CFW_LOG_WARNING(
                "SdlWindowManager: surface {} teardown timed out; keeping window hidden",
                surface);
            SDL_HideWindow(window);
            windows_[window_id].removal_failed = true;
            return false;
        }
    }

    SDL_HideWindow(window);
    windows_[window_id].removal_acknowledged = true;
    return true;
}

bool SdlWindowManager::request_remove_secondary_window(void* surface, std::chrono::milliseconds timeout) {
    const auto* managed = find_by_surface(surface);
    if (managed == nullptr || managed->is_main) return false;
    return request_remove_surface(surface, timeout);
}

bool SdlWindowManager::destroy_secondary_window(void* surface) {
    SDL_Window* window = nullptr;
    SDL_WindowID window_id = 0;
    bool acknowledged = false;
    for (const auto& [id, managed] : windows_) {
        if (managed.surface == surface && !managed.is_main) {
            window = managed.window;
            window_id = id;
            acknowledged = managed.removal_acknowledged;
            break;
        }
    }
    if (window == nullptr) {
        return false;
    }
    if (!acknowledged && !request_remove_secondary_window(surface)) {
        return false;
    }

    SDL_StopTextInput(window);
    SDL_DestroyWindow(window);
    windows_.erase(window_id);

    CFW_LOG_INFO("SdlWindowManager: secondary window destroyed (id={}, surface={})",
                 window_id, surface);
    return true;
}

void SdlWindowManager::destroy_all_secondary() {
    // Collect first (destroy_secondary_window mutates windows_).
    std::vector<void*> surfaces;
    for (const auto& [id, managed] : windows_) {
        if (!managed.is_main) {
            surfaces.push_back(managed.surface);
        }
    }
    for (void* surface : surfaces) {
        destroy_secondary_window(surface);
    }
}

void SdlWindowManager::reveal_pending_window(void* surface) {
    if (surface == nullptr) {
        return;
    }
    for (auto& [id, managed] : windows_) {
        if (managed.surface == surface && managed.pending_show) {
            SDL_ShowWindow(managed.window);
            managed.pending_show = false;
            return;
        }
    }
}

}  // namespace Corona::Systems::UI
