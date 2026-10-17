#include <horizon/core/logging.h>
#include <corona/diagnostics/thread_phase.h>
#include <corona/systems/geometry/geometry_system.h>
#include <corona/systems/mechanics/mechanics_system.h>

#include <algorithm>
#include <chrono>
#include <charconv>
#include <cstdlib>
#include <memory>
#include <string_view>

#include "mechanics_internal.h"
#include "gpu_performance.h"

namespace Corona::Systems {

using namespace MechanicsInternal;

namespace {
constexpr float kDefaultFixedDt = 1.0f / 60.0f;
constexpr float kMinFixedDt = 1.0f / 120.0f;
constexpr float kMaxFixedDt = 1.0f / 30.0f;
constexpr int kMaxCatchUpSteps = 4;

// Horizon creates native Vulkan pipelines on the first submission. Exercise
// every kernel with tiny private inputs on the preparation worker, then clear
// identities/mesh caches before publishing the pipeline. No scene data, events
// or physical time are involved. Empty dispatches would skip native creation.
GpuRigidVelocityView warmup_velocities() {
    const std::array<float, 24> zeroes{};
    auto desc = Horizon::HardwareBufferDesc::typed<float>(zeroes.size(),
        Horizon::BufferUsage_Storage | Horizon::BufferUsage_TransferSrc | Horizon::BufferUsage_TransferDst,
        "mechanics.startup.velocities");
    return {Horizon::HardwareBuffer(desc, std::as_bytes(std::span(zeroes))), 4};
}

void warmup_pipeline(GpuSkinningPipeline& pipeline) {
    const std::array<Resource::Vertex, 2> vertices{};
    const std::array<Resource::BoneWeights, 2> weights{};
    GpuSkinningPipeline::Request request;
    request.key = 1; request.revision = 1;
    request.vertices = vertices; request.weights = weights; request.readback = false;
    (void)pipeline.update(std::span(&request, 1), {});
    pipeline.reset();
}

void warmup_pipeline(GpuCollisionPipeline& pipeline) {
    CollisionMesh mesh;
    mesh.vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    mesh.triangles = {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}};
    mesh.total_vertex_count = 4;
    TriangleOctree tree;
    tree.build(mesh.vertices, mesh);
    pipeline.set_mesh(1, 1, mesh, tree);
    pipeline.set_mesh(2, 1, mesh, tree);
    const auto identity = gpu_matrix(ktm::fmat4x4::from_eye());
    const GpuCollisionPipeline::Pair pair{{1, identity}, {2, identity}};
    (void)pipeline.contacts(std::span(&pair, 1));
    const GpuCollisionPipeline::VertexQuery vertex{{1, identity}, {0, 0, 0}};
    (void)pipeline.sample_vertices(std::span(&vertex, 1));
    const GpuCollisionPipeline::GroundQuery ground{{1, identity}, {0.1f, 2, 0.1f}, 3};
    (void)pipeline.ground(std::span(&ground, 1));
    pipeline.retain({});
}

void warmup_pipeline(GpuBroadphasePipeline& pipeline) {
    const std::array<GpuBroadphasePipeline::Body, 2> bodies{{
        {{0, 0, 0}, {1, 1, 1}}, {{0, 0, 0}, {1, 1, 1}}}};
    const std::array<std::uint32_t, 2> indices{0, 1};
    const GpuBroadphasePipeline::Scene scene{indices};
    (void)pipeline.pairs(bodies, std::span(&scene, 1));
}

void warmup_pipeline(GpuRigidIntegrationPipeline& pipeline) {
    const auto source = warmup_velocities();
    const auto destination = warmup_velocities();
    const std::array<GpuRigidIntegrationPipeline::VelocityInput, 4> velocities{};
    (void)pipeline.update_velocities(velocities, {});
    (void)pipeline.update_velocities_device(velocities, source, {});
    const std::array<GpuRigidIntegrationPipeline::PoseInput, 1> poses{};
    const std::array<GpuRigidIntegrationPipeline::ResidentPoseInput, 1> resident{};
    (void)pipeline.integrate_poses(poses, 0);
    (void)pipeline.integrate_poses_from_velocities(resident, source, 0);
    (void)pipeline.predict_poses_from_velocities(resident, source, 0);
    const std::array<std::uint32_t, 1> indices{0};
    (void)pipeline.integrate_poses_to_velocities(resident, source,
        {destination.buffer, destination.count}, indices, 0);
}

void warmup_pipeline(GpuContactSolverPipeline& pipeline) {
    const auto source = warmup_velocities();
    const std::array<GpuContactSolverPipeline::Body, 4> bodies{};
    std::array<GpuContactSolverPipeline::Contact, 2> contacts{};
    contacts[0].a = 0; contacts[0].b = 1; contacts[0].normal = {0, 1, 0};
    contacts[1].a = 2; contacts[1].b = 3; contacts[1].normal = {0, 1, 0};
    const std::array<std::uint32_t, 4> indices{0, 1, 2, 3};
    (void)pipeline.solve_from_velocities(bodies, contacts, {}, source, indices);
    (void)pipeline.solve_device({}, {}, {});
}

void warmup_pipeline(GpuFloorSleepPipeline& pipeline) {
    const std::array<GpuFloorSleepPipeline::Velocity, 1> velocities{};
    const std::array<GpuFloorSleepPipeline::SleepInput, 1> sleep{};
    pipeline.begin(velocities);
    (void)pipeline.finish_sleep(sleep, {});
    const std::array<std::uint64_t, 1> identities{1};
    pipeline.begin_resident(identities);
    const std::array<GpuFloorSleepPipeline::FloorInput, 1> floor{};
    (void)pipeline.evaluate_floor(floor, {});
    const std::array<std::uint8_t, 1> accepted{1};
    pipeline.commit_floor(accepted);
    const std::array<GpuFloorSleepPipeline::ResidentSleepInput, 1> resident{};
    (void)pipeline.finish_sleep_resident(resident, {});
    pipeline.begin_resident({});
}

std::uint64_t performance_report_interval() {
    static const auto interval = [] {
        std::uint64_t value = 120;
        if (const char* setting = std::getenv("CORONA_MECHANICS_GPU_PROFILE_INTERVAL")) {
            const std::string_view text(setting);
            std::uint64_t parsed{};
            const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (result.ec == std::errc{} && result.ptr == text.data() + text.size() && parsed >= 1 && parsed <= 1000000)
                value = parsed;
        }
        return value;
    }();
    return interval;
}

// Construct before the frame timer: all nested timings must close before
// snapshot/reset, including first-frame and shutdown early returns.
struct PerformanceReport {
    ~PerformanceReport() noexcept {
        if (!GpuPerformance::enabled()) return;
        try {
            static thread_local std::uint64_t updates{};
            if (++updates < performance_report_interval()) return;
            CFW_LOG_INFO("Mechanics GPU profile ({} updates; host times, recorded transfer bytes):\n{}",
                         updates, GpuPerformance::format(GpuPerformance::snapshot()));
            GpuPerformance::reset();
            updates = 0;
        } catch (...) {
            // Optional diagnostics must not interrupt the simulation.
        }
    }
};

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

void MechanicsSystem::Impl::start_gpu_preparation() {
    if (!gpu_preparation_workers.empty()) return;
    struct Jobs {
        std::vector<std::packaged_task<void()>> tasks;
        std::atomic<std::size_t> next{};
    };
    auto jobs = std::make_shared<Jobs>();
    jobs->tasks.push_back(gpu_collision.prepare("collision", warmup_pipeline));
    jobs->tasks.push_back(gpu_rigid_integration.prepare("integration", warmup_pipeline));
    jobs->tasks.push_back(gpu_floor_sleep.prepare("floor/sleep", warmup_pipeline));
    jobs->tasks.push_back(gpu_contact_solver.prepare("contacts", warmup_pipeline));
    jobs->tasks.push_back(gpu_broadphase.prepare("broad phase", warmup_pipeline));
    jobs->tasks.push_back(gpu_skinning.prepare("skinning", warmup_pipeline));
    // Bound compiler concurrency instead of starting a compiler for every
    // kernel. Workers capture only their job queue, never Impl or SystemContext.
    for (unsigned i = 0; i < 2; ++i) {
        gpu_preparation_workers.emplace_back([jobs](std::stop_token stop) {
            while (!stop.stop_requested()) {
                const auto index = jobs->next.fetch_add(1, std::memory_order_relaxed);
                if (index >= jobs->tasks.size()) break;
                if (jobs->tasks[index].valid()) jobs->tasks[index]();
            }
        });
    }
}

bool MechanicsSystem::initialize(Kernel::ISystemContext* ctx) {
    impl_->ctx = ctx;
    impl_->shutdown_requested.store(false, std::memory_order_release);
    impl_->start_gpu_preparation();

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

    CFW_LOG_INFO("MechanicsSystem initialized (Vulkan/Helicon GPU skinning, triangle collision and resident rigid-body solver)");
    return true;
}

void MechanicsSystem::update() {
    if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
        return;
    }
    // Stage marker: this loop holds Scene read handles for the whole skinning pass
    // (frame_scenes) and blocks on scene/geometry slot locks, so a stall here also
    // stalls every writer that wants the same Scene entry.
    Corona::Diagnostics::PhaseScope update_phase{"Mechanics", "update"};
    PerformanceReport performance_report;
    GpuPerformance::Scope frame_performance{"mechanics.frame"};

    // 用高精度计时器测量真实 dt
    auto now = std::chrono::steady_clock::now();
    if (impl_->first_update) {
        impl_->last_update_time = now;
        impl_->first_update = false;
        // 首帧：先蒙皮（dt=0，建立初始姿态 AABB），再跑一步物理
        update_phase.set("skinned_geometry(first)");
        update_skinned_geometry(0.0f);
        update_phase.set("physics(first)");
        update_physics(resolve_fixed_dt());
        return;
    }

    float actual_dt = std::chrono::duration<float>(now - impl_->last_update_time).count();
    impl_->last_update_time = now;

    // 钳制防止巨幅跳帧
    const float max_frame_time = 0.1f;
    actual_dt = std::min(actual_dt, max_frame_time);

    // ---- 骨骼动画蒙皮（Vulkan EDSL，物理步进前完成）----
    // 先蒙皮生成本帧正确 AABB 和蒙皮顶点，物理帧随后消费同帧数据，
    // 消除之前 update_physics 使用上一帧 AABB 的一帧延迟。
    // 蒙皮模型即使未开物理（simulation_enabled=false）也照常播放。
    if (!impl_->shutdown_requested.load(std::memory_order_acquire)) {
        update_phase.set("skinned_geometry");
        update_skinned_geometry(actual_dt);
    }

    impl_->time_accumulator += actual_dt;

    const float fixed_dt = resolve_fixed_dt();
    int catch_up_steps = 0;
    while (impl_->time_accumulator >= fixed_dt &&
           catch_up_steps < kMaxCatchUpSteps &&
           !impl_->shutdown_requested.load(std::memory_order_acquire)) {
        update_phase.set("physics");
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

