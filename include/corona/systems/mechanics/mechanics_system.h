#pragma once

#include <corona/events/mechanics_system_events.h>
#include <corona/events/scene_system_events.h>
#include <corona/kernel/event/i_event_bus.h>
#include <corona/kernel/event/i_event_stream.h>
#include <corona/kernel/system/system_base.h>

#include <memory>

namespace Corona::Systems {

// 前向声明，避免每帧通过 ISystemContext::get_system() 获取
class GeometrySystem;

/**
 * @brief 力学系统 (Mechanics System)
 *
 * 负责物理模拟、刚体动力学、碰撞检测和响应。
 * 运行在独立线程，以 60 FPS 更新物理状态。
 */
class MechanicsSystem : public Kernel::SystemBase {
   public:
    MechanicsSystem();

    ~MechanicsSystem() override;

    // ========================================
    // ISystem 接口实现
    // ========================================

    std::string_view get_name() const override {
        return "Mechanics";
    }

    int get_priority() const override {
        return 75;  // 中高优先级，在几何系统之后
    }

    /**
     * @brief 初始化力学系统
     * @param ctx 系统上下文
     * @return 初始化成功返回 true
     */
    bool initialize(Kernel::ISystemContext* ctx) override;

    /**
     * @brief 每帧更新物理
     *
     * 在独立线程中调用，执行物理模拟
     */
    void update() override;

    /**
     * @brief 关闭力学系统
     *
     * 清理所有物理资源
     */
    void stop() override;

    void shutdown() override;

   private:
    // 力学系统私有成员
    void update_physics(float fixed_dt);

    /// 骨骼动画 CPU 蒙皮（P2，自 GeometrySystem 迁入）。每真实帧遍历所有 GeometryDevice，
    /// 对蒙皮模型（Scene::skeleton 有值）：推进 anim_time → compute_pose 算 final 骨骼矩阵
    /// → 对每个 mesh 做 CPU 蒙皮 → write_bytes 重传到 GPU 顶点缓冲（含已驻留 LOD 级别，
    /// LOD1..N 句柄经 GeometrySystem::get_skinning_targets 借出）。蒙皮后顶点 + 动态 AABB
    /// 写回 GeometryDevice（结果槽：所有 buffer/CPU 数据仍归 GeometrySystem 持有，便于
    /// 流式加载 LRU 管理），供 Vision / 物理消费。在 update() 的固定步进循环之前调用，
    /// 使物理帧消费同帧蒙皮 AABB，而非上一帧数据。
    /// @param dt 本真实帧经过时间（秒），由 update() 测量并传入；首帧传 0。
    void update_skinned_geometry(float dt);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace Corona::Systems
