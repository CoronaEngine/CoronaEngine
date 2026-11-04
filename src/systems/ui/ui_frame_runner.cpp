#include <corona/systems/ui/ui_frame_runner.h>

#include <horizon/core/logging.h>
#include <corona/memory/gpu_mem_ledger.h>
#include <corona/systems/ui/camera_viewport_manager.h>
#include <corona/systems/ui/panel_resize_policy.h>
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
#include "imgui/imgui_platform.h"
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Corona::Systems::UI {

// ============================================================================
// SDL, ImGui platform and Horizon renderer lifecycle.
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

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    static std::string layout_path;
    if (char* preferences = SDL_GetPrefPath("CoronaEngine", "Editor")) {
        layout_path = std::string(preferences) + "corona-docking.ini";
        SDL_free(preferences);
        io.IniFilename = layout_path.c_str();
    }
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 0;
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = 1;
#ifdef _WIN32
    // Native window titles use the same language as the Web panel metadata.
    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\msyh.ttc") != INVALID_FILE_ATTRIBUTES)
        io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc", 18, nullptr, io.Fonts->GetGlyphRangesChineseFull());
#endif
    if (!ImGui_ImplSDL3_InitForOther(window) || !vulkan_backend->initialize_imgui_renderer()) {
        CFW_LOG_ERROR("Failed to initialize ImGui platform/renderer");
        return false;
    }
    install_imgui_platform(*vulkan_backend);
    return true;
}

void shutdown_sdl_ui(SDL_Window*& window, std::unique_ptr<VulkanBackend>& vulkan_backend) {
    if (ImGui::GetCurrentContext()) {
        ImGui::DestroyPlatformWindows();
        retry_imgui_platform_retirement();
        if (imgui_platform_retirement_pending()) {
            CFW_LOG_ERROR("ImGui surface retirement pending; retaining SDL and renderer resources");
            return;
        }
        vulkan_backend->shutdown_imgui_renderer();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
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


}  // namespace Corona::Systems::UI
