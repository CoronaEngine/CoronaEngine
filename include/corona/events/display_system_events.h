#pragma once

#include "horizon.h"

#include <cstdint>
#include <future>
#include <memory>

namespace Corona::Events {
/**
 * @brief 显示系统内部事件（单线程使用 EventBus）
 */
struct DisplaySystemDemoEvent {
    int demo_value;
};

/**
 * @brief 引擎到显示系统的跨线程事件（使用 EventStream）
 */
struct EngineToDisplayDemoEvent {
    float delta_time;
};

/**
 * @brief 显示系统到引擎的跨线程事件（使用 EventStream）
 */
struct DisplayToEngineDemoEvent {
    float delta_time;
};

/**
 * @brief 显示表面变化事件（使用 EventBus）
 */
struct DisplaySurfaceChangedEvent {
    void* surface;
};

/**
 * @brief 显示表面移除事件（使用 EventBus，主线程发布）
 *
 * 当 ImGui 次级视口子窗口销毁时发布。DisplaySystem 拥有该 surface 的 swapchain，
 * 必须在主线程销毁 OS 窗口之前完成 displayer 的拆除（GPU idle + 释放），否则
 * Display 线程可能向已销毁的窗口呈现。`done` 由 DisplaySystem 在其 update() 线程
 * 完成拆除后兑现，发布方（主线程）阻塞等待该 future 以保证拆除顺序。
 *
 * 注意：EventBus 为同步分发，订阅回调运行在发布方线程（主线程），因此回调中
 * 只能把请求压入缓冲并立即返回；真正的拆除与 done 兑现发生在 Display 线程。
 */
struct DisplaySurfaceRemovedEvent {
    void* surface = nullptr;
    std::shared_ptr<std::promise<void>> done;  ///< Display 线程拆除完成后 set_value()
};

/**
 * @brief Optics layer frame ready (published by OpticsSystem, consumed by DisplaySystem)
 */
struct OpticsFrameReadyEvent {
    void* surface = nullptr;
    std::uintptr_t image_handle = 0;
    uint64_t frame_index = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t viewport_x = 0;
    uint32_t viewport_y = 0;
    uint32_t viewport_width = 0;
    uint32_t viewport_height = 0;
};

/**
 * @brief UI layer frame ready (published by VulkanBackend, consumed by DisplaySystem)
 */
struct UIFrameReadyEvent {
    void* surface = nullptr;
    std::uintptr_t image_handle = 0;
    uint64_t frame_index = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

}  // namespace Corona::Events
