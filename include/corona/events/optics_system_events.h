#pragma once

#include <corona/kernel/entity_id.h>
#include <horizon/core/storage.h>

#include <future>
#include <chrono>
#include <memory>
#include <string>

namespace Corona {
class Model;
}

namespace Corona::Events {

/**
 * @brief 光学系统内部事件（单线程使用 EventBus）
 */
struct OpticsSystemDemoEvent {
    int demo_value;
};

/**
 * @brief 引擎到光学系统的跨线程事件（使用 EventStream）
 */
struct EngineToOpticsDemoEvent {
    float delta_time;
};

/**
 * @brief 光学系统到引擎的跨线程事件（使用 EventStream）
 */
struct OpticsToEngineDemoEvent {
    float delta_time;
};

/**
 * @brief Screenshot request (published by Camera/Viewport API, consumed by OpticsSystem)
 *
 * Uses camera_handle as the matching key so that offscreen cameras (without a
 * display surface) can still take screenshots.
 */
struct ScreenshotRequestEvent {
    EntityId camera_handle = 0;
    std::string file_path;
    std::shared_ptr<std::promise<bool>> completion_promise;
    std::chrono::steady_clock::time_point deadline{};
    // Zero preserves the normal next-frame screenshot. Fixed-frame captures
    // reset Vision history and render continuously before the same readback.
    unsigned render_frames{};
    unsigned warmup_frames{};
    std::string timings_path;
};

/**
 * @brief Request to switch the optics render backend (published by script API,
 *        consumed by OpticsSystem).
 *
 * backend: 0 = Native (Vulkan rasterization), 1 = Vision (Vulkan path tracing).
 * Vision initializes its device lazily when rendering is needed.
 */
struct RenderBackendSwitchEvent {
    int backend = 0;
    EntityId camera_handle = 0;
};

}  // namespace Corona::Events
