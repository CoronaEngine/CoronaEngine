#include <horizon/core/logging.h>
#include <corona/systems/geometry/geometry_system.h>
#include <corona/systems/mechanics/mechanics_system.h>

#include <algorithm>
#include <chrono>
#include <memory>

#include "mechanics_internal.h"

namespace Corona::Systems {

using namespace MechanicsInternal;

namespace {
constexpr float kDefaultFixedDt = 1.0f / 60.0f;
constexpr float kMinFixedDt = 1.0f / 120.0f;
constexpr float kMaxFixedDt = 1.0f / 30.0f;
constexpr int kMaxCatchUpSteps = 4;

float resolve_fixed_dt() {
    auto& hub = SharedDataHub::instance();
    const auto& scenes = hub.scene_storage();
    auto& environments = hub.environment_storage();
    for (const auto& scene : scenes) {
        if (!scene.enabled || !scene.simulation_enabled || scene.environment == 0) continue;
        if (auto environment = environments.try_acquire_read(scene.environment)) {
            return std::clamp(environment->fixed_dt, kMinFixedDt, kMaxFixedDt);
        }
    }
    return kDefaultFixedDt;
}
}  // namespace

MechanicsSystem::MechanicsSystem() : impl_(std::make_unique<Impl>()) {
    set_target_fps(60);
}

MechanicsSystem::~MechanicsSystem() = default;

bool MechanicsSystem::initialize(Kernel::ISystemContext* ctx) {
    impl_->ctx = ctx;
    impl_->shutdown_requested.store(false, std::memory_order_release);

    // GeometrySystem 指针缓存移到 update_physics() 首次调用时完成，
    // 因为 initialize() 在 SystemManager::initialize_all() 的锁内调用，
    // 此时 get_system() 会尝试重入同一把非递归 mutex，导致未定义行为/崩溃。

    if (ctx && ctx->event_bus()) {
        impl_->residency_sub_id_ =
            ctx->event_bus()->subscribe<Events::ActorResidencyChangedEvent>(
                [this](const Events::ActorResidencyChangedEvent& e) {
                    std::unique_lock lock(impl_->residency_mtx_);
                    if (e.loaded) {
                        impl_->resident_actors_.insert(e.actor);
                    } else {
                        impl_->resident_actors_.erase(e.actor);
                    }
                });
    }

    CFW_LOG_INFO("MechanicsSystem initialized");
    return true;
}

void MechanicsSystem::update() {
    if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
        return;
    }

    // 用高精度计时器测量真实 dt
    auto now = std::chrono::steady_clock::now();
    if (impl_->first_update) {
        impl_->last_update_time = now;
        impl_->first_update = false;
        // 首帧：先蒙皮（dt=0，建立初始姿态 AABB），再跑一步物理
        update_skinned_geometry(0.0f);
        update_physics(resolve_fixed_dt());
        return;
    }

    float actual_dt = std::chrono::duration<float>(now - impl_->last_update_time).count();
    impl_->last_update_time = now;

    // 钳制防止巨幅跳帧
    const float max_frame_time = 0.1f;
    actual_dt = std::min(actual_dt, max_frame_time);

    // ---- 骨骼动画 CPU 蒙皮（Phase 1：移至物理步进之前）----
    // 先蒙皮生成本帧正确 AABB 和蒙皮顶点，物理帧随后消费同帧数据，
    // 消除之前 update_physics 使用上一帧 AABB 的一帧延迟。
    // 蒙皮模型即使未开物理（simulation_enabled=false）也照常播放。
    if (!impl_->shutdown_requested.load(std::memory_order_acquire)) {
        update_skinned_geometry(actual_dt);
    }

    impl_->time_accumulator += actual_dt;

    const float fixed_dt = resolve_fixed_dt();
    int catch_up_steps = 0;
    while (impl_->time_accumulator >= fixed_dt &&
           catch_up_steps < kMaxCatchUpSteps &&
           !impl_->shutdown_requested.load(std::memory_order_acquire)) {
        update_physics(fixed_dt);
        impl_->time_accumulator -= fixed_dt;
        ++catch_up_steps;
    }
}

void MechanicsSystem::stop() {
    impl_->shutdown_requested.store(true, std::memory_order_release);
    Kernel::SystemBase::stop();
}

void MechanicsSystem::shutdown() {
    // 标记关闭请求，不再接受新的回调任务
    impl_->shutdown_requested.store(true, std::memory_order_release);

    if (impl_->residency_sub_id_ != 0 && impl_->ctx && impl_->ctx->event_bus()) {
        impl_->ctx->event_bus()->unsubscribe(impl_->residency_sub_id_);
    }

    impl_->clear_runtime_state();
    CFW_LOG_INFO("MechanicsSystem shutdown, all caches cleared");
}

// 物理主循环（单帧）：搜集物体 → 积分外力(重力/阻尼) → 建世界 AABB → 粗/细碰撞改速度 → 积分位姿 → 地板 → 休眠 → 清理缓存

}  // namespace Corona::Systems

