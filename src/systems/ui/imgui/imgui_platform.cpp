#include "imgui_platform.h"
#include <corona/systems/ui/vulkan_backend.h>
#include <corona/systems/ui/sdl_window_manager.h>
#include <horizon/core/logging.h>
#include <imgui.h>
#include <chrono>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Corona::Systems::UI {
namespace {
VulkanBackend* renderer = nullptr;
void (*sdl_destroy)(ImGuiViewport*) = nullptr;
void (*sdl_show)(ImGuiViewport*) = nullptr;
std::vector<std::unique_ptr<ImGuiViewport>> retired;

// A secondary window is created hidden so it never flashes a stale/empty swapchain, and
// revealed once its first frame is on screen. That wait has to be bounded: a hidden window
// is not presented by Display, so "wait for the first present" is a deadlock when nothing
// presents the surface before the window exists. Past this deadline the window is shown
// unconditionally and its first present follows on the normal path.
constexpr auto kPendingShowDeadline = std::chrono::milliseconds(100);
std::unordered_map<void*, std::chrono::steady_clock::time_point> pending_show_since;
std::unordered_map<void*, long long> pending_show_frames;

SDL_Window* window_of(ImGuiViewport* viewport) {
    return SDL_GetWindowFromID(static_cast<SDL_WindowID>(reinterpret_cast<std::uintptr_t>(viewport->PlatformHandle)));
}
bool retire(ImGuiViewport* viewport) {
    auto& manager = SdlWindowManager::instance();
    auto* window = window_of(viewport);
    if (!window) return true;
    const auto surface = native_surface_from_sdl_window(window);
    if (surface == manager.main_surface()) return true;
    if (!manager.request_remove_secondary_window(surface, std::chrono::milliseconds(50))) return false;
    renderer->unregister_surface(surface);
    manager.forget_platform_window(SDL_GetWindowID(window));
    return true;
}
void destroy_platform(ImGuiViewport* viewport) {
    if (retire(viewport)) {
        sdl_destroy(viewport);
    } else {
        // ImGui frees its viewport after this callback. Transfer SDL userdata to a
        // detached public viewport until Display acknowledges retirement; never free
        // an HWND while its swapchain can still be in use.
        auto pending = std::make_unique<ImGuiViewport>();
        pending->PlatformUserData = viewport->PlatformUserData;
        pending->PlatformHandle = viewport->PlatformHandle;
        pending->PlatformHandleRaw = viewport->PlatformHandleRaw;
        retired.push_back(std::move(pending));
        viewport->PlatformUserData = viewport->PlatformHandle = viewport->PlatformHandleRaw = nullptr;
    }
}
void create_renderer(ImGuiViewport* viewport) {
    auto* window = window_of(viewport);
    const auto surface = native_surface_from_sdl_window(window);
    if (SdlWindowManager::instance().adopt_platform_window(window) &&
        renderer->register_surface(surface, window)) {
        viewport->RendererUserData = renderer;
    } else
        viewport->PlatformRequestClose = true;
}
}

void install_imgui_platform(VulkanBackend& backend) {
    renderer = &backend;
    auto& platform = ImGui::GetPlatformIO();
    sdl_destroy = platform.Platform_DestroyWindow;
    sdl_show = platform.Platform_ShowWindow;
    platform.Platform_DestroyWindow = destroy_platform;
    // The window stays hidden until render_imgui_platform_windows reveals it on first
    // present, or at kPendingShowDeadline at the latest.
    platform.Platform_ShowWindow = [](ImGuiViewport*) {};
    platform.Renderer_CreateWindow = create_renderer;
    platform.Renderer_DestroyWindow = [](ImGuiViewport* viewport) { viewport->RendererUserData = nullptr; };
    platform.Renderer_SetWindowSize = [](ImGuiViewport*, ImVec2) {};
}

void retry_imgui_platform_retirement() {
    std::erase_if(retired, [](const auto& viewport) {
        if (!retire(viewport.get())) return false;
        sdl_destroy(viewport.get());
        return true;
    });
}
bool imgui_platform_retirement_pending() { return !retired.empty(); }

void render_imgui_platform_windows(VulkanBackend& backend) {
    const auto now = std::chrono::steady_clock::now();
    for (auto* viewport : ImGui::GetPlatformIO().Viewports) {
        auto* window = window_of(viewport);
        const auto surface = native_surface_from_sdl_window(window);
        // This is the only place a secondary window can become visible, so the decision
        // lives here and is taken before the frame is drawn: either its first frame is
        // already on screen, or the wait has run out.
        if (window && surface) {
            const auto* managed = SdlWindowManager::instance().find_by_surface(surface);
            if (managed && managed->pending_show) {
                const auto entry = pending_show_since.try_emplace(surface, now).first;
                const long long frames = ++pending_show_frames[surface];
                const bool first_present = backend.first_present_ready(surface);
                const bool deadline_passed = now - entry->second >= kPendingShowDeadline;
                if (first_present || deadline_passed) {
                    if (sdl_show) sdl_show(viewport);
                    SdlWindowManager::instance().reveal_pending_window(surface);
                    pending_show_since.erase(surface);
                    pending_show_frames.erase(surface);
                    CFW_LOG_INFO(
                        "[UI/PlatformWindow] revealed surface {} after {} frames ({})", surface,
                        frames,
                        first_present ? "first present" : "show deadline, no first present");
                } else if (frames == 1) {
                    // Pending windows used to fail silently (no draw data, no surface, no
                    // first-present receipt); keep one line so a stuck window is visible.
                    CFW_LOG_INFO(
                        "[UI/PlatformWindow] surface {} created hidden: draw_data={} "
                        "has_surface={} minimized={} extent={}x{}",
                        surface, viewport->DrawData != nullptr, backend.has_surface(surface),
                        (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0,
                        static_cast<int>(viewport->Size.x), static_cast<int>(viewport->Size.y));
                }
            }
        }
        if (!window || !viewport->DrawData || !backend.has_surface(surface)) continue;
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) continue;
        int width = 0, height = 0;
        if (!SDL_GetWindowSizeInPixels(window, &width, &height) || width <= 0 || height <= 0) continue;
        backend.render_imgui(surface, *viewport->DrawData, width, height);
        backend.present_surface(surface);
    }
}
}
