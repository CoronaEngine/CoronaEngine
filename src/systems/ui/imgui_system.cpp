#include <corona/kernel/core/i_logger.h>
#include <corona/shared_data_hub.h>
#include <corona/systems/script/script_system.h>
#include <corona/systems/ui/camera_viewport_manager.h>
#include <corona/systems/ui/imgui_system.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

#include "cef/browser_manager.h"
#include "cef/cef_client.h"
#include "imgui/imgui_ui.h"

namespace Corona::Systems {

bool ImguiSystem::initialize(Kernel::ISystemContext* ctx) {
    CFW_LOG_NOTICE("ImguiSystem: Initializing...");

    // 1. 初始化 CEF (必须在主线程)
    if (!UI::initialize_cef()) {
        CFW_LOG_ERROR("CEF initialization failed.");
        return false;
    }

    // 2. 初始化 SDL 和 ImGui (必须在主线程)
    CFW_LOG_NOTICE("ImguiSystem: Initializing SDL and ImGui in main thread...");
    if (!UI::initialize_sdl_imgui(window_, io_, vulkan_backend_)) {
        CFW_LOG_ERROR("SDL/ImGui initialization failed.");
        UI::shutdown_cef();
        return false;
    }

    sdl_initialized_ = true;
    running_ = true;
    active_tab_id_ = -1;

    SDL_ShowWindow(window_);

    CFW_LOG_NOTICE("ImguiSystem: Initialized successfully (main thread mode)");
    state_ = Kernel::SystemState::running;

    // 【订阅系统内部事件】使用 EventBus
    auto* event_bus = ctx->event_bus();
    if (event_bus) {
        sdl_start_id_ = event_bus->subscribe<Events::ScriptFinishStartEvent>(
            [this](const Events::ScriptFinishStartEvent& event) {
                // SDL_MaximizeWindow(window_);
                SDL_ShowWindow(window_);
            });
    } else {
        CFW_LOG_WARNING("ImguiSystem: No event bus available");
    }

    return true;
}

void ImguiSystem::start() {
    // 主线程系统不需要启动独立线程
    // ImguiSystem 由 Engine::tick() 在主线程中调用 update()
    state_ = Kernel::SystemState::running;
}

void ImguiSystem::stop() {
    // 主线程系统不需要停止线程
    CFW_LOG_INFO("ImguiSystem: Stop called (main thread mode)");
    auto& browser_manager = UI::BrowserManager::instance();
    std::vector<std::uintptr_t> camera_handles;
    for (const auto& [tab_id, tab] : browser_manager.get_tabs()) {
        if (tab->camera_view) {
            if (auto record = UI::CameraViewportManager::instance().find_by_tab(tab_id)) {
                camera_handles.push_back(record->camera_handle);
            }
        }
    }
    browser_manager.close_all_tabs();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!camera_handles.empty() && std::chrono::steady_clock::now() < deadline) {
        std::erase_if(camera_handles, [](const std::uintptr_t camera_handle) {
            auto camera =
                SharedDataHub::instance().camera_storage().try_acquire_read(camera_handle);
            return !camera || camera->surface == nullptr;
        });
        if (!camera_handles.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    ImGui::DestroyPlatformWindows();
    running_ = false;
    state_ = Kernel::SystemState::stopped;
}

void ImguiSystem::update() {
    if (!running_ || !sdl_initialized_) {
        return;
    }

    static UI::UiFrameRunner frame_runner;
    UI::UiFrameContext context{
        window_,
        io_,
        vulkan_backend_.get(),
        &active_tab_id_,
        &running_,
        &window_size_changed_};

    frame_runner.run_frame(context);
}

void ImguiSystem::shutdown() {
    CFW_LOG_NOTICE("ImGuiSystem: Shutting down...");
    running_ = false;

    // 关闭所有浏览器标签页
    CFW_LOG_INFO("ImGuiSystem: Closing all browser tabs...");
    UI::BrowserManager::instance().close_all_tabs();

    // 清理 SDL 和 ImGui (必须在主线程)
    if (sdl_initialized_) {
        CFW_LOG_INFO("ImGuiSystem: Shutting down SDL and ImGui...");
        UI::shutdown_sdl_imgui(window_, io_, vulkan_backend_);
        sdl_initialized_ = false;
    }

    // 清理 CEF
    CFW_LOG_INFO("ImGuiSystem: Shutting down CEF...");
    UI::shutdown_cef();
    CFW_LOG_INFO("ImGuiSystem: Shutdown complete");
}

}  // namespace Corona::Systems
