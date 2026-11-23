#pragma once

#include <corona/kernel/entity_id.h>
#include <cstdint>

namespace Corona::Events {

/**
 * @brief 声学系统内部事件（单线程使用 EventBus）
 */
struct AcousticsSystemDemoEvent {
    int demo_value;
};

/**
 * @brief 引擎到声学系统的跨线程事件（使用 EventStream）
 */
struct EngineToAcousticsDemoEvent {
    float delta_time;
};

/**
 * @brief 声学系统到引擎的跨线程事件（使用 EventStream）
 */
struct AcousticsToEngineDemoEvent {
    float delta_time;
};

// ============================================================================
// 音频播放事件（单线程 EventBus）
// ============================================================================

/// 请求播放音频资源
struct PlayAudioEvent {
    std::uint64_t resource_id{0};
    bool loop{false};
    /// 关联的声学组件句柄（AcousticsStorage handle）。
    /// 非 0 时为空间音频：声学线程据此每帧解析 actor 的世界坐标。
    /// 0 表示旧的全局非空间播放路径。
    EntityId acoustics_handle{0};
};

/// 请求停止音频资源
struct StopAudioEvent {
    std::uint64_t resource_id{0};
    /// 非 0 时按声学组件句柄停止对应的空间播放；0 时按 resource_id 停止。
    EntityId acoustics_handle{0};
};

}  // namespace Corona::Events
