#pragma once

#include <horizon/core/storage.h>

#include <future>
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
    std::uintptr_t camera_handle = 0;
    std::string file_path;
    std::shared_ptr<std::promise<bool>> completion_promise;
};

/**
 * @brief Request to switch the optics render backend (published by script API,
 *        consumed by OpticsSystem).
 *
 * backend: 0 = Native (Vulkan rasterization), 1 = Vision (CUDA path tracing).
 * The switch is only meaningful when the engine is compiled with
 * CORONA_ENABLE_VISION; otherwise OpticsSystem keeps the Native backend.
 */
struct RenderBackendSwitchEvent {
    int backend = 0;
    std::uintptr_t camera_handle = 0;
};

/**
 * @brief Request to load an external Vision scene (published by script API,
 *        consumed by OpticsSystem).
 *
 * scene_path: absolute path to a Vision *.json scene.
 * scene_json/base_dir/scene_key: in-memory Vision scene JSON, the directory
 * used to resolve relative resources, and a stable cache/log key.
 * external_live: use the ExternalLive Vision runtime so native proxy actors can
 * update bound Vision shapes without rebuilding the whole scene.
 * Empty path and empty json means "unload the external scene and rebuild the
 * engine-driven scene from SharedDataHub".
 *
 * Only meaningful when the engine is compiled with CORONA_ENABLE_VISION and the
 * Vision backend is active. The actual import runs on the OpticsSystem render
 * thread; this event only carries the path across the thread boundary.
 */
struct VisionSceneLoadEvent {
    std::string scene_path;
    std::string scene_json;
    std::string base_dir;
    std::string scene_key;
    bool external_live = false;
};

}  // namespace Corona::Events
