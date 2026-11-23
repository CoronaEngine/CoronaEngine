#pragma once

// Tracks native windows and coordinates Display surface registration/retirement.
// ImGui's SDL backend owns every platform window; this manager adopts them and never
// creates one itself.

#include <SDL3/SDL.h>

#include <chrono>
#include <functional>
#include <unordered_map>

namespace Corona::Systems::UI {

// A window tracked by the manager. All of them are adopted: the main window from the UI
// bootstrap, secondary windows from ImGui's platform backend.
struct ManagedWindow {
    SDL_Window* window = nullptr;
    SDL_WindowID window_id = 0;
    void* surface = nullptr;  // native handle (HWND/NSWindow*); DisplaySystem surface key
    bool is_main = false;
    // A secondary window is adopted HIDDEN and revealed only after its first frame has been
    // published to the DisplaySystem, to avoid a 1-frame white/black flash when it opens.
    bool pending_show = false;
    bool removal_acknowledged = false;
};

// Extract the native window handle (HWND/NSWindow*) from an SDL window, mirroring the
// existing extraction in vulkan_backend.cpp. Returns nullptr on unsupported platforms or
// failure.
[[nodiscard]] void* native_surface_from_sdl_window(SDL_Window* window);

class SdlWindowManager {
   public:
    // Process-wide instance. The UI runs single-threaded on the main thread, so the window
    // set is shared (the frame runner reads it; adoption and retirement mutate it) via this
    // singleton, mirroring BrowserManager / CameraViewportManager.
    static SdlWindowManager& instance();

    SdlWindowManager() = default;

    // Adopt the already-created main window (today: imgui_ui.cpp's SDL_CreateWindow). The
    // window's native surface is resolved and stored. Returns false if the handle is null
    // or the surface cannot be resolved.
    bool adopt_main_window(SDL_Window* window);
    // ImGui's SDL backend owns these windows; this manager owns only Display registration.
    bool adopt_platform_window(SDL_Window* window);
    void forget_platform_window(SDL_WindowID id);

    // Lookups.
    [[nodiscard]] SDL_Window* main_window() const { return main_window_; }
    [[nodiscard]] void* main_surface() const;
    [[nodiscard]] const ManagedWindow* find_by_surface(void* surface) const;

    // Iterate all tracked windows (main + secondaries).
    void for_each_window(const std::function<void(const ManagedWindow&)>& fn) const;

    // --- Retirement of an adopted window. ---
    // Request Display retirement and hide the native window. Returns true only when the
    // Display acknowledgement was received; a timeout leaves the hidden window tracked.
    bool request_remove_secondary_window(void* surface, std::chrono::milliseconds timeout = std::chrono::seconds(5));
    bool request_remove_surface(void* surface, std::chrono::milliseconds timeout = std::chrono::seconds(5));
    // Final destruction after Vulkan image state has been unregistered. Refuses to destroy
    // a window whose Display retirement was not acknowledged.
    bool destroy_secondary_window(void* surface);

    // Deferred show: if `surface`'s window is still hidden awaiting its first frame, show it
    // now (called after that surface's first UIFrameReadyEvent is published, eliminating the
    // 1-frame white flash when a window opens). No-op if already shown or unknown surface.
    void reveal_pending_window(void* surface);


   private:
    SDL_Window* main_window_ = nullptr;
    SDL_WindowID main_window_id_ = 0;
    std::unordered_map<SDL_WindowID, ManagedWindow> windows_;
};

}  // namespace Corona::Systems::UI
