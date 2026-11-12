#include <corona/systems/ui/sdl_window_manager.h>
#include <corona/systems/display/display_surface_commands.h>

#include <corona/events/display_system_events.h>
#include <horizon/core/logging.h>
#include <corona/kernel/core/kernel_context.h>
#include <corona/kernel/event/i_event_bus.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>

#include "cef/browser_manager.h"

namespace Corona::Systems::UI {

namespace {

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

bool SdlWindowManager::adopt_platform_window(SDL_Window* window) {
    const auto surface = native_surface_from_sdl_window(window);
    if (!window || !surface) return false;
    const auto id = SDL_GetWindowID(window);
    ManagedWindow managed;
    managed.window = window;
    managed.window_id = id;
    managed.surface = surface;
    managed.pending_show = true;
    windows_[id] = managed;
    if (auto* bus = Kernel::KernelContext::instance().event_bus()) {
        // Platform creation runs on the UI thread. Display may be busy compiling
        // a scene, and native surface creation may need this thread to dispatch
        // OS messages. Enqueue registration without blocking it. Display drains
        // surface commands before UI frames; the existing first-present ticket
        // keeps this window hidden until registration and presentation succeed.
        Corona::DisplaySurfaceCommand surface_command{};
        surface_command.kind = Corona::DisplaySurfaceCommand::Kind::Changed;
        surface_command.surface = surface;
        return Corona::DisplaySurfaceCommandChannel::instance().commands().push(std::move(surface_command));
    }
    return false;
}

void SdlWindowManager::forget_platform_window(SDL_WindowID id) {
    if (id != main_window_id_) windows_.erase(id);
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

void SdlWindowManager::for_each_window(const std::function<void(const ManagedWindow&)>& fn) const {
    if (!fn) {
        return;
    }
    for (const auto& [id, managed] : windows_) {
        fn(managed);
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
        Corona::DisplaySurfaceCommand surface_command{};
        surface_command.kind = Corona::DisplaySurfaceCommand::Kind::Removed;
        surface_command.surface = surface;
        surface_command.done = done;
        Corona::DisplaySurfaceCommandChannel::instance().commands().push(std::move(surface_command));
        if (fut.wait_for(timeout) != std::future_status::ready) {
            CFW_LOG_WARNING(
                "SdlWindowManager: surface {} teardown timed out; keeping window hidden",
                surface);
            SDL_HideWindow(window);
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
