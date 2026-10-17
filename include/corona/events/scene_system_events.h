#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace Corona::Events {

/**
 * @brief 场景系统：actor 进入某相机视锥
 */
struct ActorEnteredFrustumEvent {
    std::uintptr_t scene{};
    std::uintptr_t actor{};
    std::uintptr_t camera{};
};

/**
 * @brief 场景系统：actor 离开某相机视锥
 */
struct ActorLeftFrustumEvent {
    std::uintptr_t scene{};
    std::uintptr_t actor{};
    std::uintptr_t camera{};
};

/**
 * @brief 触发 LRU 卸载请求（M3 起接入 ActorCache）
 */
struct ActorEvictRequestedEvent {
    std::uintptr_t scene{};
    std::uintptr_t actor{};
    // gpu_only=true：仅释放 GPU 资源（快照存 ActorCache + 延迟释放显存），
    //   **不**级联清理底层 Scene/Image 的 CPU 内存。用于 VRAM 压力淘汰——
    //   GPU 压力绝不误伤 CPU，且 Scene CPU 保留使后续可快速从 CPU 重建 GPU。
    // gpu_only=false（默认）：GPU 释放 + 级联 try_evict 底层资源 CPU。
    //   用于不可见帧淘汰、RAM 压力淘汰（整体下线）。
    bool gpu_only{false};
};

/**
 * @brief 触发 LRU 唤醒请求（M3 起接入 ActorCache）
 */
struct ActorRestoreRequestedEvent {
    std::uintptr_t scene{};
    std::uintptr_t actor{};
};

/**
 * @brief 触发 Actor 的距离预加载请求事件
 */
struct ActorLoadRequestedEvent {
    std::uintptr_t scene{};
    std::uintptr_t actor{};
};

/**
 * @brief Actor 资源加载完成，GPU 资源已就绪（由 GeometrySystem 发布）
 *
 * 外部系统（Optics/Mechanics/Vision）应监听此事件以开始消费该 actor。
 * 与 ActorLoadRequestedEvent 配对：request → 异步加载 → finished。
 */
struct ActorLoadFinishedEvent {
    std::uintptr_t scene{};
    std::uintptr_t actor{};
};

/**
 * @brief 触发 Actor 的距离卸载请求事件
 */
struct ActorUnloadRequestedEvent {
    std::uintptr_t scene{};
    std::uintptr_t actor{};
};

/**
 * @brief Actor 资源卸载完成，GPU 资源已释放（由 GeometrySystem 发布）
 *
 * 外部系统应监听此事件以停止消费该 actor。
 * 与 ActorUnloadRequestedEvent 配对：request → 异步卸载 → finished。
 */
struct ActorUnloadFinishedEvent {
    std::uintptr_t scene{};
    std::uintptr_t actor{};
};

/**
 * @brief Actor 驻留状态变更通知（Loaded ↔ Unloaded）
 *
 * 每当 actor 的驻留状态发生确定性的变化时发布：
 * - loaded=true  → 资源已加载，GPU 数据就绪，外部系统可以消费
 * - loaded=false → 资源已卸载，GPU 数据已释放，外部系统应停止消费
 *
 * 外部系统（Optics / Mechanics / Vision）应统一监听此事件，
 * 而不是分别监听 ActorLoadFinishedEvent / ActorUnloadFinishedEvent。
 *
 * 发布方：GeometrySystem（on_load_finished / on_unload_finished）
 */
struct ActorResidencyChangedEvent {
    std::uintptr_t scene{};
    std::uintptr_t actor{};
    bool           loaded{};   // true=已加载(Loaded), false=已卸载(Unloaded)
};

/**
 * @brief 旧版八叉树粗筛碰撞候选对事件，保留类型兼容。
 *
 * GeometrySystem 不再逐帧生成 CPU 候选对或自动发布本事件。
 * MechanicsSystem 直接批量提交 GPU 粗筛，使用结果进行三角窄相检测。
 * 需要 CPU 八叉树候选对的调用方仍可显式调用 GeometrySystem::query_pairs()。
 */
struct BroadphasePairsEvent {
    std::uintptr_t scene{};
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> pairs;
};
}  // namespace Corona::Events
