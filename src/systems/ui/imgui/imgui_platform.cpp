#include "imgui_platform.h"
#include <corona/systems/ui/vulkan_backend.h>
#include <corona/systems/ui/sdl_window_manager.h>
#include <imgui.h>
#include <memory>
#include <vector>

namespace Corona::Systems::UI {
namespace {
VulkanBackend* renderer = nullptr;
void (*sdl_destroy)(ImGuiViewport*) = nullptr;
void (*sdl_show)(ImGuiViewport*) = nullptr;
std::vector<std::unique_ptr<ImGuiViewport>> retired;
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
    if (SdlWindowManager::instance().adopt_platform_window(window) &&
        renderer->register_surface(native_surface_from_sdl_window(window), window))
        viewport->RendererUserData = renderer;
    else
        viewport->PlatformRequestClose = true;
}
}

void install_imgui_platform(VulkanBackend& backend) {
    renderer = &backend;
    auto& platform = ImGui::GetPlatformIO();
    sdl_destroy = platform.Platform_DestroyWindow;
    sdl_show = platform.Platform_ShowWindow;
    platform.Platform_DestroyWindow = destroy_platform;
    platform.Platform_ShowWindow = [](ImGuiViewport*) {}; // wait for actual first present
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
    for (auto* viewport : ImGui::GetPlatformIO().Viewports) {
        auto* window = window_of(viewport);
        const auto surface = native_surface_from_sdl_window(window);
        if (!window || !viewport->DrawData || !backend.has_surface(surface)) continue;
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) continue;
        int width = 0, height = 0;
        if (!SDL_GetWindowSizeInPixels(window, &width, &height) || width <= 0 || height <= 0) continue;
        backend.render_imgui(surface, *viewport->DrawData, width, height);
        backend.present_surface(surface);
        const auto* managed = SdlWindowManager::instance().find_by_surface(surface);
        if (managed && managed->pending_show && backend.first_present_ready(surface)) {
            sdl_show(viewport);
            SdlWindowManager::instance().reveal_pending_window(surface);
        }
    }
}
}
