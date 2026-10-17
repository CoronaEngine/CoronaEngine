#include <corona/events/engine_events.h>
#include <corona/diagnostics/thread_phase.h>
#include <corona/events/mechanics_system_events.h>
#include <horizon/core/logging.h>
#include <corona/kernel/event/i_event_bus.h>
#include <corona/kernel/event/i_event_stream.h>
#include <corona/resource/resource_manager.h>
#include <corona/systems/mechanics/mechanics_system.h>
#include <corona/systems/geometry/geometry_system.h>

#include <algorithm>      // min,max,clamp,sort,unique
#include <array>          // std::array（八叉树子节点）
#include <atomic>         // g_shutdown_requested
#include <chrono>         // steady_clock
#include <cmath>          // asin,atan2,fabs,abs
#include <cstddef>        // size_t
#include <cstdint>        // 固定宽度整数
#include <exception>      // std::exception
#include <functional>     // std::function（回调）
#include <limits>         // numeric_limits（SAT）
#include <memory>         // unique_ptr,make_unique
#include <set>
#include <stdexcept>
#include <unordered_map>  // 各 handle→数据 映射
#include <unordered_set>  // alive_handles
#include <utility>        // pair, move
#include <vector>         // mechanics_data, collision_pairs 等

#include "corona/shared_data_hub.h"  // 场景/几何/transform 集中存储
#include "ktm/ktm.h"                 // 向量矩阵四元数

// Resource layer — 用于加载 LOD 碰撞网格
#include <corona/resource/resource_manager.h>
#include <corona/resource/types/scene.h>
#include <corona/resource/types/animation_pose.h>  // 骨骼动画求值（compute_pose / advance_anim_time）
// Note: do not depend on nanobind in the mechanics system. Callbacks provided
// from the scripting layer are expected to manage GIL acquisition themselves.


#include "mechanics_internal.h"
#include "gpu_performance.h"
#include "ik_runtime.h"

namespace Corona::Systems {

using namespace MechanicsInternal;

void MechanicsSystem::update_physics(float fixed_dt) {
    GpuPerformance::Scope performance{"physics.step"};
    std::optional<GpuPerformance::Scope> phase;
    phase.emplace("physics.collect");
    // 如果正在关闭，不再处理新的物理更新
    if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
        return;
    }

    // Replace the complete contact sample once per fixed step, including a step
    // with no contacts or disabled simulation. Animation may read this sample
    // multiple times between steps; an empty render-frame queue is not separation.
    impl_->deferred_ik_target_updates.clear();

    // 首次调用时懒缓存 GeometrySystem 指针（不在 initialize() 中做，
    // 因为 initialize() 在 SystemManager::initialize_all() 的锁内执行，
    // get_system() 会重入同一把非递归 mutex）。
    if (!impl_->geometry_sys && impl_->ctx) {
        impl_->geometry_sys = dynamic_cast<GeometrySystem*>(impl_->ctx->get_system("Geometry"));
    }

    // 常量：时间步、摩擦、休眠、惯量下限等（可按手感调参）
    const float floor_eps = 0.01f;                                       // 地板碰撞容差
    const float low_vel_threshold = 0.05f;                               // 低速衰减阈值
    const float zero_vel_threshold = 0.01f;                              // 速度归零阈值
    const float friction_coeff = 0.35f;                                  // 动态（滑动）摩擦系数
    const float static_friction_coeff = 0.55f;                           // 静摩擦系数（坡面防微滑）
    const float sleep_threshold = 0.05f;                                 // 休眠速度阈值
    const float sleep_time_needed = 0.4f;                                // 静止多久后休眠
    const float min_inertia = 0.0001f;                                   // 最小转动惯量，防止除零
    const float rot_damping_factor = 0.97f;                              // 基础旋转阻尼系数
    const float max_linear_speed = 80.0f;                                // 线速度上限 m/s，防碰飞
    const float max_angular_speed = 30.0f;                               // 角速度上限 rad/s
    const float max_impulse_per_contact = 500.0f;                        // 单次接触最大冲量 N·s
    const float max_position_correction = 0.5f;                          // 单帧最大位置修正距离 m
    const float positional_slop = 0.0005f;

    // 本帧临时表：质量/阻尼/恢复系数/碰撞开关
    std::unordered_map<std::uintptr_t, BodyFrameParams> frame_params;

    // --- 从 SharedDataHub 取各存储的引用（几何、变换、场景、环境等）---
    auto& mechanics_storage = SharedDataHub::instance().mechanics_storage();            // mechanics 组件数据
    auto& geometry_storage = SharedDataHub::instance().geometry_storage();              // 网格/包围体句柄
    auto& transform_storage = SharedDataHub::instance().model_transform_storage();      // 位姿写回目标
    auto& model_resource_storage = SharedDataHub::instance().model_resource_storage();  // 模型资源数据
    const auto& scene_storage = SharedDataHub::instance().scene_storage();              // 场景与 actor 列表（const → cbegin/cend 读锁遍历）
    auto& actor_storage = SharedDataHub::instance().actor_storage();
    auto& profile_storage = SharedDataHub::instance().profile_storage();          // actor→mechanics 映射
    auto& environment_storage = SharedDataHub::instance().environment_storage();  // 全局 dt/重力等

    ktm::fvec3 gravity = make_fvec3(0.0f, -9.8f, 0.0f);  // m/s²
    float floor_restitution = 0.6f;                      // 地板法向弹性系数 0..1
    float floor_y = 0.0f;                                // 无穷大水平面高度

    std::vector<std::uintptr_t> mechanics_handles;  // 本帧参与物理的 mechanics 去重列表
    mechanics_handles.reserve(64);
    // Keep membership separate from the deduplicated body list: a body may
    // belong to several scenes, but each physical pair must be solved once.
    std::vector<std::vector<std::uintptr_t>> scene_mechanics;
    scene_mechanics.reserve(4);

    // --- 阶段 1：遍历场景 → 读环境(gravity/floor/dt) → 展开 Actor/Profile → 收集 mechanics_handle ---
    for (const auto& scene : scene_storage) {
        if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
            return;
        }
        if (!scene.enabled)
            continue;

        if (!scene.simulation_enabled)
            continue;
        const auto scene_handle = reinterpret_cast<std::uintptr_t>(&scene);
        auto& scene_bodies = scene_mechanics.emplace_back();

        // 若绑定了 environment：覆盖重力、地板参数，并钳制 fixed_dt
        if (scene.environment != 0) {
            if (auto env = environment_storage.try_acquire_read(scene.environment)) {
                gravity = env->gravity;
                floor_y = env->floor_y;
                floor_restitution = env->floor_restitution;
            }
        }

        for (auto actor_handle : scene.actor_handles) {
            if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
                return;
            }
            // 跳过未加载的 actor — 无 GPU 资源 / 无全量物理数据
            // TODO: 后续实现 offline physics proxy —— Unloaded + body_type=Dynamic
            //       的 actor 用简化 AABB 碰撞体继续参与物理
            {
                std::shared_lock lock(impl_->residency_mtx_);
                if (!impl_->resident_actors_.count(actor_handle)) continue;
            }
            // Residency is scene-specific. A shared actor loaded elsewhere
            // must not enter collision pairs for an unloaded scene.
            const bool broadphase_loaded = impl_->geometry_sys &&
                impl_->geometry_sys->get_actor_load_state(actor_handle, scene_handle) == ActorLoadState::Loaded;
            if (auto actor = actor_storage.try_acquire_read(actor_handle)) {
                // 续期资源访问时间（驱动 ResourceManager LRU）
                {
                    auto& mrs = SharedDataHub::instance().model_resource_storage();
                    auto& ps = SharedDataHub::instance().profile_storage();
                    auto& gs = SharedDataHub::instance().geometry_storage();
                    for (auto ph : actor->profile_handles) {
                        auto prof = ps.try_acquire_read(ph);
                        if (!prof || !prof->geometry_handle) continue;
                        auto g = gs.try_acquire_read(prof->geometry_handle);
                        if (!g || !g->model_resource_handle) continue;
                        auto mr = mrs.try_acquire_read(g->model_resource_handle);
                        if (mr && mr->model_id) {
                            Corona::Resource::ResourceManager::get_instance().touch(mr->model_id);
                        }
                        break;
                    }
                }

                for (auto profile_handle : actor->profile_handles) {
                    if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
                        return;
                    }
                    if (auto profile = profile_storage.try_acquire_read(profile_handle)) {
                        if (auto h = profile->mechanics_handle) {
                            // 读 MechanicsDevice：检查物理开关 + 质量/阻尼/恢复；读失败则用默认值
                            // 轴锁变化检测变量（需在 if/else 外声明，供后续唤醒逻辑使用）
                            auto& body = impl_->body(h);
                            auto& params = frame_params[h];
                            params.actor = actor_handle;
                            uint8_t old_linear = body.linear_lock;
                            uint8_t old_angular = body.angular_lock;
                            uint8_t new_linear = 0;
                            uint8_t new_angular = 0;
                            if (auto m_acc = mechanics_storage.try_acquire_read(h)) {
                                params.mass = m_acc->mass;
                                params.damping = m_acc->damping;
                                params.restitution = m_acc->restitution;
                                params.body_type = m_acc->body_type;
                                new_linear = m_acc->linear_lock_mask;
                                new_angular = m_acc->angular_lock_mask;
                                body.linear_lock = new_linear;
                                body.angular_lock = new_angular;
                            } else {
                                params = BodyFrameParams{};
                                params.actor = actor_handle;
                                body.linear_lock = 0;
                                body.angular_lock = 0;
                            }

                            // Phantom 完全不参与碰撞检测和物理模拟，直接跳过
                            if (params.body_type == BodyType::Phantom) continue;

                            mechanics_handles.push_back(h);
                            if (broadphase_loaded) scene_bodies.push_back(h);

                            // 轴锁解除时唤醒休眠体：若锁定位从 1→0（解锁），休眠体需恢复物理响应
                            if (((old_linear & ~new_linear) != 0 || (old_angular & ~new_angular) != 0) && body.sleeping) {
                                body.sleeping = false;
                                body.reset_sleep_timer = true;
                            }

                            // 质量防护：避免0质量导致碰撞冲量计算异常
                            if (params.mass < 0.0001f) {
                                params.mass = 1.0f;
                            }
                        }
                    }
                }
            }
        }
    }

    std::sort(mechanics_handles.begin(), mechanics_handles.end());                                                      // 排序使 unique 有效
    mechanics_handles.erase(std::unique(mechanics_handles.begin(), mechanics_handles.end()), mechanics_handles.end());  // 去重

    if (mechanics_handles.empty()) {
        return;  // 无物体则整帧跳过
    }

    impl_->global_simulation_time += fixed_dt;

    // Take one pose snapshot before forces/sleep flags are uploaded. An editor
    // move is an explicit wake command; stale contact impulses belong to the
    // previous placement and must not survive it.
    std::unordered_map<std::uintptr_t, ModelTransform> step_transforms;
    std::unordered_set<std::uintptr_t> edited_bodies;
    for (auto h : mechanics_handles) {
        auto& body = impl_->body(h);
        const auto type = frame_params.at(h).body_type;
        if (body.body_type != type) {
            body.body_type = type;
            body.sleeping = false;
            body.reset_sleep_timer = true;
            edited_bodies.insert(h);
        }
        auto mechanics = mechanics_storage.try_acquire_read(h);
        if (!mechanics) continue;
        auto geometry = geometry_storage.try_acquire_read(mechanics->geometry_handle);
        if (!geometry) continue;
        const auto transform_handle = geometry->transform_handle;
        if (!step_transforms.contains(transform_handle)) {
            auto transform = transform_storage.try_acquire_read(transform_handle);
            if (!transform) continue;
            step_transforms.emplace(transform_handle, *transform);
        }
        if (impl_->body(h).observe_transform(transform_handle, step_transforms.at(transform_handle)))
            edited_bodies.insert(h);
    }
    std::erase_if(impl_->warm_start_cache, [&](const auto& item) {
        return edited_bodies.contains(item.first.first) || edited_bodies.contains(item.first.second);
    });

    phase.emplace("physics.prepare_forces");
    // Stage 2/2b: one GPU batch preserves damping -> gravity -> speed limits ->
    // axis locks. Include handles without valid geometry, as the old integrator
    // also updated their velocities before resolving the transform below.
    impl_->gpu_rigid_integration.ensure();
    impl_->gpu_floor_sleep.ensure();
    if (!impl_->gpu_submission_batch) impl_->gpu_submission_batch = std::make_unique<GpuSubmissionBatch>();
    auto& gpu_batch = *impl_->gpu_submission_batch;
    // A shutdown/exception must invalidate a recorded but unexecuted state.
    // Normal paths explicitly flush at host readback boundaries below.
    struct BatchScope {
        GpuSubmissionBatch& batch;
        ~BatchScope() { batch.abandon(); }
    } batch_scope{gpu_batch};
    std::vector<std::uint64_t> body_identities(mechanics_handles.begin(), mechanics_handles.end());
    std::unordered_map<std::uintptr_t, std::uint32_t> body_state_indices;
    for (std::size_t i = 0; i < mechanics_handles.size(); ++i)
        body_state_indices.emplace(mechanics_handles[i], static_cast<std::uint32_t>(i));
    // An empty simulation returns above, retaining state while fully paused.
    // Nonempty steps remap surviving identities and discard absent bodies, just
    // like the CPU runtime cache cleanup at the end of the step.
    impl_->gpu_floor_sleep->begin_resident(body_identities, &gpu_batch);
    GpuRigidVelocityView integrated_velocities;
    {
        std::vector<GpuRigidIntegrationPipeline::VelocityInput> inputs;
        inputs.reserve(mechanics_handles.size());
        for (auto h : mechanics_handles) {
            if (impl_->shutdown_requested.load(std::memory_order_acquire)) return;
            const auto& body = impl_->body(h);
            const auto& params = frame_params.at(h);
            inputs.push_back({{}, {},
                              params.damping, params.body_type == BodyType::Dynamic, body.sleeping,
                              body.linear_lock, body.angular_lock, params.body_type != BodyType::Dynamic});
        }
        integrated_velocities = impl_->gpu_rigid_integration->update_velocities_device(inputs,
            impl_->gpu_floor_sleep->initial_velocities(),
            {fixed_dt, {gravity.x, gravity.y, gravity.z}, rot_damping_factor, max_linear_speed, max_angular_speed}, &gpu_batch);
    }

    // --- 阶段 3：为每个 mechanics 读几何/变换 → 首遇则建四元数朝向 → 预测位姿 → 世界 AABB + 长方体对角惯量（世界系冲量用）---
    phase.emplace("physics.prepare_geometry");
    std::vector<MechanicsWorldAABB> mechanics_data;
    mechanics_data.reserve(mechanics_handles.size());
    std::unordered_map<std::uintptr_t, std::size_t> handle_to_index;
    std::vector<ModelTransform> collision_transforms;
    std::vector<GpuRigidIntegrationPipeline::ResidentPoseInput> predicted_poses;
    collision_transforms.reserve(mechanics_handles.size());
    predicted_poses.reserve(mechanics_handles.size());

    for (std::uintptr_t h : mechanics_handles) {         // 为每个力学体准备碰撞与惯量数据
        if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
            return;
        }
        auto m_acc = mechanics_storage.try_acquire_read(h);  // mechanics 组件读锁
        if (!m_acc) continue;                            // 无数据则跳过
        const auto& m = *m_acc;                          // 其 min/max、geometry_handle

        auto geom_acc = geometry_storage.try_acquire_read(m.geometry_handle);
        if (!geom_acc) continue;

        const auto snapshot = step_transforms.find(geom_acc->transform_handle);
        if (snapshot == step_transforms.end() ||
            impl_->body(h).last_transform_handle != geom_acc->transform_handle) continue;
        const auto& t = snapshot->second;

        ktm::fvec3 e_local = make_fvec3(  // mechanics 局部半棱长
            (m.max_xyz.x - m.min_xyz.x) * 0.5f,
            (m.max_xyz.y - m.min_xyz.y) * 0.5f,
            (m.max_xyz.z - m.min_xyz.z) * 0.5f);

        // 获取 model_id 用于碰撞网格查找
        std::uint64_t entry_model_id = 0;
        if (auto res_acc = model_resource_storage.try_acquire_read(geom_acc->model_resource_handle)) {
            entry_model_id = res_acc->model_id;
        }

        MechanicsWorldAABB entry;  // 本物体本帧用的缓存结构
        entry.handle = h;
        entry.geom_handle = m.geometry_handle;             // geometry actor 句柄（蒙皮缓存键）
        entry.transform_handle = geom_acc->transform_handle;  // 之后写位置修正用同一 handle
        entry.model_id = entry_model_id;
        entry.local_min = m.min_xyz;
        entry.local_max = m.max_xyz;

        // 蒙皮物体（P4）：用每帧蒙皮顶点算出的动态 local AABB 覆盖静态 min/max，
        // 使碰撞包围盒跟随当前动画姿态（如挥臂时 AABB 变大）。AABB 由 GeometrySystem
        // 的 update_skinned_geometry 每帧在蒙皮循环中顺手累积（模型空间，已含首帧归一化），
        // 与静态 min_xyz 同空间，后续 world_aabb_from_local_bounds 逻辑完全复用。
        if (geom_acc->is_skinned && geom_acc->skinned_aabb_valid) {
            entry.local_min = geom_acc->skinned_aabb_min;
            entry.local_max = geom_acc->skinned_aabb_max;
            entry.is_skinned = true;
            // S4：同步更新 e_local，使惯量张量与实际碰撞 AABB 尺寸一致（姿态变化时同步）
            e_local = make_fvec3(
                (geom_acc->skinned_aabb_max.x - geom_acc->skinned_aabb_min.x) * 0.5f,
                (geom_acc->skinned_aabb_max.y - geom_acc->skinned_aabb_min.y) * 0.5f,
                (geom_acc->skinned_aabb_max.z - geom_acc->skinned_aabb_min.z) * 0.5f);
        }
        auto& body = impl_->body(h);
        // Only physics-owned dynamic poses are extrapolated. Static/kinematic
        // collision geometry must match the transform authored by the caller.
        GpuRigidIntegrationPipeline::PoseInput predicted;
        predicted.position = {t.position.x, t.position.y, t.position.z};
        predicted.orientation = {body.orientation_quat.i, body.orientation_quat.j,
                                 body.orientation_quat.k, body.orientation_quat.r};
        predicted.linear_lock = body.linear_lock;
        predicted.angular_lock = body.angular_lock;
        predicted.advance = !body.sleeping && frame_params[h].body_type == BodyType::Dynamic;
        predicted_poses.push_back({predicted, body_state_indices.at(h), false});
        collision_transforms.push_back(t);
        if (frame_params[h].body_type != BodyType::Dynamic) {
            // Static/Kinematic：无限质量，逆惯量为零，不接收冲量速度修正
            entry.inertia_inv_body = make_fvec3(0.0f, 0.0f, 0.0f);
        } else {
            const float mass = frame_params[h].mass;                    // kg
            const float w = std::abs(e_local.x * t.scale.x) * 2.0f;  // 世界系盒子 X 向全长（缩放后）
            const float hh = std::abs(e_local.y * t.scale.y) * 2.0f;
            const float d = std::abs(e_local.z * t.scale.z) * 2.0f;
            float Ix = mass * (hh * hh + d * d) / 12.0f;  // 长方体主轴惯量（近似；均质 box）
            float Iy = mass * (w * w + d * d) / 12.0f;
            float Iz = mass * (w * w + hh * hh) / 12.0f;
            Ix = std::max(Ix, min_inertia);  // 下限防止除零
            Iy = std::max(Iy, min_inertia);
            Iz = std::max(Iz, min_inertia);
            entry.inertia_inv_body = make_fvec3(1.0f / Ix, 1.0f / Iy, 1.0f / Iz);  // 体系逆惯量对角
        }

        handle_to_index[h] = mechanics_data.size();  // 句柄→本轮 mechanics_data 下标
        mechanics_data.push_back(entry);
    }

    phase.emplace("physics.prediction_bounds");
    // Storage read handles above are released before waiting for the GPU.
    const auto predictions = impl_->gpu_rigid_integration->predict_poses_from_velocities(
        predicted_poses, integrated_velocities, fixed_dt, &gpu_batch);
    (void)gpu_batch.flush();  // Also execute forces when no body has readable geometry.
    for (std::size_t i = 0; i < predictions.size(); ++i) {
        auto& entry = mechanics_data[i];
        auto& transform = collision_transforms[i];
        const auto& pose = predictions[i];
        auto q = ktm::fquat::real_imag(pose.orientation[3], make_fvec3(pose.orientation[0], pose.orientation[1], pose.orientation[2]));
        transform.position = make_fvec3(pose.position[0], pose.position[1], pose.position[2]);
        sync_euler_from_orientation_quat(q, transform.euler_rotation);
        world_aabb_from_local_bounds(transform, entry.local_min, entry.local_max,
                                    entry.min_world, entry.max_world, entry.center_world);
        entry.half_extents = make_fvec3((entry.max_world.x - entry.min_world.x) * .5f,
                                      (entry.max_world.y - entry.min_world.y) * .5f,
                                      (entry.max_world.z - entry.min_world.z) * .5f);
        entry.rot_body_to_world = q.matrix3x3();
    }

    phase.emplace("physics.collision_assets");
    // Preload immutable triangle topology before GPU broad/narrow phase.
    for (const auto& entry : mechanics_data) {
        if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
            return;
        }
        if (entry.model_id == 0) continue;

        if (entry.is_skinned) {
            // 蒙皮物体：确保固定拓扑已解析；运行期变形顶点驻留 GPU。
            if (!impl_->static_triangle_index_cache.count(entry.model_id)) {
                ensure_collision_mesh(entry.model_id,
                                      impl_->collision_mesh_cache,
                                      &impl_->static_triangle_index_cache,
                                      &impl_->static_triangle_bone_cache);
            }
        } else {
            // 非蒙皮物体：静态碰撞网格，只建一次
            ensure_collision_mesh(entry.model_id,
                                  impl_->collision_mesh_cache,
                                  &impl_->static_triangle_index_cache,
                                  &impl_->static_triangle_bone_cache);
        }
    }

    // 临时校正表：记录 Phase 5 迭代后处理（每子步一次）的位置校正量，在 Phase 6 积分后统一应用
    std::unordered_map<std::uintptr_t, ktm::fvec3> position_correction;
    std::unordered_set<std::uintptr_t> supported_bodies;
    struct PenetrationConstraint {
        std::size_t a, b;
        ktm::fvec3 normal;
        float depth;
    };
    std::vector<PenetrationConstraint> penetration_constraints;
    GpuRigidVelocityView solved_velocities;

    // Only keys successfully registered for this step's model may be queried.
    // A cached GPU key can otherwise still refer to a model that was replaced.
    std::unordered_set<std::uintptr_t> ready_collision_keys;
    for (const auto& entry : mechanics_data) {
        if (!entry.model_id || !entry.geom_handle) continue;
        {
            auto geometry = geometry_storage.try_acquire_read(entry.geom_handle);
            if (!geometry || geometry->gpu_build_state != GeometryDevice::GpuBuildState::Ready ||
                geometry->mesh_handles.empty()) continue;
            auto resource = model_resource_storage.try_acquire_read(geometry->model_resource_handle);
            if (!resource || resource->model_id != entry.model_id) continue;
        }
        const auto bind = impl_->collision_mesh_cache.find(entry.model_id);
        if (bind == impl_->collision_mesh_cache.end()) continue;
        const CollisionMesh* mesh = nullptr;
        TriangleOctree* tree = nullptr;
        if (bind->second.requires_skinning) {
            const auto model = impl_->skinned_collision_model_ids.find(entry.geom_handle);
            const auto found = impl_->skinned_collision_cache.find(entry.geom_handle);
            if (!entry.is_skinned || model == impl_->skinned_collision_model_ids.end() ||
                model->second != entry.model_id || found == impl_->skinned_collision_cache.end() ||
                !impl_->gpu_collision || !impl_->gpu_collision->contains(entry.geom_handle)) continue;
            mesh = &found->second;
            tree = &impl_->skinned_octree_cache[entry.geom_handle];
        } else {
            mesh = &bind->second;
            tree = &impl_->triangle_octree_cache[entry.model_id];
        }
        if (mesh->triangles.empty()) continue;
        if (tree->empty()) tree->build(mesh->vertices, *mesh);
        impl_->gpu_collision.ensure();
        impl_->gpu_collision->set_mesh(entry.geom_handle, entry.model_id, *mesh, *tree);
        ready_collision_keys.insert(entry.geom_handle);
    }

    phase.emplace("physics.contacts");
    // Stage 5: GPU scene-scoped AABB sweep -> GPU triangle contacts -> GPU
    // sequential impulses. Use this step's predicted, deformed bounds directly;
    // the Geometry tree's static/current bounds can miss moving or skinned bodies.

    // 构建 actor_handle → vector<mechanics_handle> 反向映射
    std::unordered_map<std::uintptr_t, std::vector<std::uintptr_t>> actor_to_mech;
    for (const auto& [mh, params] : frame_params) {
        if (params.actor != 0) {
            actor_to_mech[params.actor].push_back(mh);
        }
    }
    auto actor_for_mechanics = [&](std::uintptr_t mechanics_handle) {
        auto it = frame_params.find(mechanics_handle);
        return (it != frame_params.end() && it->second.actor != 0) ? it->second.actor : mechanics_handle;
    };
    auto first_mechanics_for_actor = [&](std::uintptr_t actor_handle) {
        auto it = actor_to_mech.find(actor_handle);
        return (it != actor_to_mech.end() && !it->second.empty()) ? it->second.front() : std::uintptr_t{0};
    };

    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> collision_pairs;
    collision_pairs.reserve(mechanics_data.size() * 4);

    if (mechanics_data.size() >= 2) {
        if (mechanics_data.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("Too many mechanics bodies for GPU broad phase");
        std::vector<GpuBroadphasePipeline::Body> bounds;
        bounds.reserve(mechanics_data.size());
        constexpr float margin = GpuCollisionPipeline::contact_distance * 0.5f;
        for (const auto& body : mechanics_data)
            bounds.push_back({{body.min_world.x - margin, body.min_world.y - margin, body.min_world.z - margin},
                              {body.max_world.x + margin, body.max_world.y + margin, body.max_world.z + margin}});
        std::vector<std::vector<std::uint32_t>> members(scene_mechanics.size());
        std::vector<GpuBroadphasePipeline::Scene> scenes;
        scenes.reserve(members.size());
        for (std::size_t si = 0; si < scene_mechanics.size(); ++si) {
            for (auto handle : scene_mechanics[si]) {
                const auto found = handle_to_index.find(handle);
                if (found != handle_to_index.end()) members[si].push_back(static_cast<std::uint32_t>(found->second));
            }
            scenes.push_back({members[si]});
        }
        if (impl_->shutdown_requested.load(std::memory_order_acquire)) return;
        impl_->gpu_broadphase.ensure();
        const auto pairs = impl_->gpu_broadphase->pairs(bounds, scenes);
        collision_pairs.reserve(pairs.size());
        // mechanics_data follows sorted mechanics handles. Canonical GPU pairs
        // therefore give a stable solver order across scenes and tree layouts.
        for (const auto& pair : pairs)
            collision_pairs.emplace_back(mechanics_data[pair[0]].handle, mechanics_data[pair[1]].handle);
    }

    if (mechanics_data.size() >= 2) {
        // 碰撞对跟踪（用于回调通知 collision start/end）
        std::unordered_set<std::pair<std::uintptr_t, std::uintptr_t>, PairHash> curr_active_collisions;

        // 5.4 对候选对做法向/切向冲量（半隐式 GS：多轮依次解每对约束近似同时满足）
        constexpr float eps = 1e-8f;                   // 分母稳定项，非物理
        constexpr float k_positional_percent = 0.35f;  // 迭代结束后按穿透拆分平移（每子步一次），且只推一部分，防过冲
        constexpr int k_impulse_iterations = 8;        // 轮数↑ 堆叠更稳；提升到8以改善坡面接触收敛
        constexpr float k_early_exit_vel_eps = 0.002f; // 本轮最大速度修正低于此值则提前退出

        // E1：每个候选对的本子步接触记录。迭代内只写记录，迭代结束后统一执行一次后处理
        // （位置校正 / 唤醒 / 活跃对 / 开始回调），与是否收敛早退无关。
        struct PairContactRecord {
            bool in_contact = false;  // 本子步窄相判定接触（与轮次、接近/分离状态无关）
            std::size_t index_a = 0;  // mechanics_data 下标
            std::size_t index_b = 0;
            ktm::fvec3 normal{};      // A→B
            float penetration = 0.f;
            ktm::fvec3 contact_pt{};  // S2：预计算 pass 填写的接触点（冲量迭代内直接读取）
            float inv_ma = 0.f;
            float inv_mb = 0.f;
            // E2：累积冲量（跨迭代轮次，本子步内累加，warm start 用）
            float accumulated_j  = 0.f;  // 法向累积冲量（≥ 0，不允许负值即拉力）
            // 反弹：迭代前记录的初始法向接近速度（v_n_initial > 0 表示接近）
            float v_n_initial = 0.f;
            ktm::fvec3 surface_a{}, surface_b{};
            int triangle_a{-1}, triangle_b{-1};
        };
        std::vector<PairContactRecord> pair_records(collision_pairs.size());

        // GPU triangle narrow phase runs once for the whole broad-phase batch.
        // Only compact contacts and sampled surface velocities return to the solver.
        {
            std::set<std::pair<std::uintptr_t, int>> ik_queued_nodes;
            std::vector<GpuCollisionPipeline::Pair> queries;
            std::vector<std::size_t> query_pairs;
            for (std::size_t pair_idx = 0; pair_idx < collision_pairs.size(); ++pair_idx) {
                if (impl_->shutdown_requested.load(std::memory_order_acquire)) return;
                const auto [ha, hb] = collision_pairs[pair_idx];
                auto it_a = handle_to_index.find(ha), it_b = handle_to_index.find(hb);
                if (it_a == handle_to_index.end() || it_b == handle_to_index.end()) continue;
                const auto& a = mechanics_data[it_a->second];
                const auto& b = mechanics_data[it_b->second];
                if (!impl_->gpu_collision || a.geom_handle == b.geom_handle ||
                    !ready_collision_keys.contains(a.geom_handle) ||
                    !ready_collision_keys.contains(b.geom_handle)) continue;
                GpuCollisionPipeline::Pair query;
                query.a.key = a.geom_handle;
                query.b.key = b.geom_handle;
                query.a.transform = gpu_matrix(collision_transforms[it_a->second].compute_matrix());
                query.b.transform = gpu_matrix(collision_transforms[it_b->second].compute_matrix());
                pair_records[pair_idx].index_a = it_a->second;
                pair_records[pair_idx].index_b = it_b->second;
                queries.push_back(query);
                query_pairs.push_back(pair_idx);
            }
            const auto hits = queries.empty() ? std::vector<GpuCollisionPipeline::Contact>{}
                : impl_->gpu_collision->contacts(queries);
            std::vector<GpuCollisionPipeline::VertexQuery> velocity_queries;
            std::vector<std::pair<std::size_t, bool>> velocity_targets;
            for (std::size_t qi = 0; qi < hits.size(); ++qi) {
                const auto& hit = hits[qi];
                if (!hit.has_contact) continue;
                auto& rec = pair_records[query_pairs[qi]];
                const auto& a = mechanics_data[rec.index_a];
                const auto& b = mechanics_data[rec.index_b];
                rec.in_contact = true;
                rec.normal = make_fvec3(hit.normal[0], hit.normal[1], hit.normal[2]);
                rec.penetration = hit.penetration;
                rec.contact_pt = make_fvec3(hit.point[0], hit.point[1], hit.point[2]);
                rec.triangle_a = hit.triangle_a; rec.triangle_b = hit.triangle_b;
                const auto cached = impl_->warm_start_cache.find(collision_pairs[query_pairs[qi]]);
                if (cached != impl_->warm_start_cache.end()) {
                    const auto& old = cached->second;
                    const auto delta = old.point - rec.contact_pt;
                    if (old.triangle_a == rec.triangle_a && old.triangle_b == rec.triangle_b &&
                        ktm::dot(old.normal, rec.normal) > 0.999f && ktm::dot(delta, delta) < 0.0004f)
                        rec.accumulated_j = old.normal_impulse;
                }
                // Sleeping bodies do not extrapolate gravity. Their support
                // therefore uses the same contact margin as narrow phase, not
                // just the penetration slop of an awake predicted pose.
                if (rec.normal.y < -0.25f) supported_bodies.insert(a.handle);
                if (rec.normal.y > 0.25f) supported_bodies.insert(b.handle);
                penetration_constraints.push_back({rec.index_a, rec.index_b, rec.normal, rec.penetration});
                // Use hysteresis around the correction slop. Awake bodies are
                // checked at their final pose below: prediction includes gravity
                // that the solver may already have cancelled.
                if (rec.penetration > positional_slop * 2.0f) {
                    for (auto h : {a.handle, b.handle}) {
                        if (frame_params.at(h).body_type != BodyType::Dynamic) continue;
                        auto& body = impl_->body(h);
                        if (body.sleeping) {
                            body.sleeping = false; body.reset_sleep_timer = true;
                        }
                    }
                }
                const auto append_velocity = [&](const MechanicsWorldAABB& body,
                                                 const GpuCollisionPipeline::Instance& instance,
                                                 bool is_b) {
                    if (!body.is_skinned) return;
                    const auto inverse = ktm::inverse(make_fmat4x4_colmajor(instance.transform));
                    const auto p = inverse * make_fvec4(hit.point[0], hit.point[1], hit.point[2], 1);
                    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
                        !std::isfinite(p.w) || std::abs(p.w) < 1e-8f) return;
                    velocity_queries.push_back({instance, {p.x / p.w, p.y / p.w, p.z / p.w}});
                    velocity_targets.emplace_back(query_pairs[qi], is_b);
                };
                append_velocity(a, queries[qi].a, false);
                append_velocity(b, queries[qi].b, true);
                // Keep raw world contacts; each matching chain applies its own offset before CCD.
                auto enqueue_ik = [&](int tri_idx, const MechanicsWorldAABB& body,
                                      const ktm::fvec3& contact_normal) {
                    if (!body.is_skinned || tri_idx < 0) return;
                    auto sit = impl_->skinned_collision_cache.find(body.geom_handle);
                    if (sit == impl_->skinned_collision_cache.end()) return;
                    const auto& sc = sit->second;
                    if (tri_idx >= static_cast<int>(sc.triangle_bone_ids.size())) return;
                    int node = sc.triangle_bone_ids[static_cast<std::size_t>(tri_idx)];
                    if (node < 0) return;
                    const auto gh = body.geom_handle;
                    if (!gh) return;
                    bool needs_contact = false;
                    std::uint64_t revision = 0;
                    {
                        auto gr = geometry_storage.try_acquire_read(gh);
                        if (gr) {
                            revision = gr->ik_chains_revision;
                            for (const auto& ch : gr->ik_chains) {
                                if (ch.enabled && ch.weight > 0 && ch.contact_driven &&
                                    ch.mode != Resource::IkChain::Mode::FootPlant) {
                                    needs_contact = true;
                                    break;
                                }
                            }
                        }
                    }
                    if (!needs_contact || !ik_queued_nodes.emplace(gh, node).second) return;
                    impl_->deferred_ik_target_updates.push_back(
                        {gh, body.transform_handle, node, rec.contact_pt, contact_normal, revision});
                };
                enqueue_ik(hit.triangle_a, a, rec.normal);
                enqueue_ik(hit.triangle_b, b, make_fvec3(-rec.normal.x, -rec.normal.y, -rec.normal.z));
            }
            if (!velocity_queries.empty()) {
                const auto velocities = impl_->gpu_collision->sample_vertices(velocity_queries);
                for (std::size_t vi = 0; vi < velocities.size(); ++vi) {
                    const auto [pair_index, is_b] = velocity_targets[vi];
                    const auto& v = velocities[vi].surface_velocity;
                    (is_b ? pair_records[pair_index].surface_b : pair_records[pair_index].surface_a)
                        = make_fvec3(v[0], v[1], v[2]);
                }
            }
        }

        // Wake connected dynamic bodies before computing effective masses.
        // A sleeping endpoint used to have zero inverse mass, so trying to wake
        // it from its resulting impulse could never succeed. Fixed bodies only
        // propagate an explicit edit, not wakefulness from unrelated contacts.
        std::vector<std::vector<std::size_t>> contact_neighbors(mechanics_data.size());
        for (const auto& rec : pair_records) {
            if (!rec.in_contact) continue;
            contact_neighbors[rec.index_a].push_back(rec.index_b);
            contact_neighbors[rec.index_b].push_back(rec.index_a);
        }
        std::vector<std::size_t> wake_queue;
        std::vector<bool> visited(mechanics_data.size(), false);
        for (std::size_t i = 0; i < mechanics_data.size(); ++i) {
            const auto h = mechanics_data[i].handle;
            if (edited_bodies.contains(h) ||
                (frame_params.at(h).body_type == BodyType::Dynamic && !impl_->body(h).sleeping)) {
                visited[i] = true;
                wake_queue.push_back(i);
            }
        }
        for (std::size_t next = 0; next < wake_queue.size(); ++next) {
            for (auto neighbor : contact_neighbors[wake_queue[next]]) {
                const auto h = mechanics_data[neighbor].handle;
                if (visited[neighbor] || frame_params.at(h).body_type != BodyType::Dynamic) continue;
                visited[neighbor] = true;
                auto& body = impl_->body(h);
                body.sleeping = false;
                // The GPU speed test resets timers on a real impact. Preserving
                // the timer here lets quiet connected bodies settle together.
                wake_queue.push_back(neighbor);
            }
        }

        // Upload compact contacts once; all GS iterations and the global
        // convergence decision execute on the GPU in a single submission.
        std::vector<GpuContactSolverPipeline::Contact> solver_contacts;
        std::vector<std::size_t> solver_pairs;
        for (std::size_t pi = 0; pi < pair_records.size(); ++pi) {
            const auto& rec = pair_records[pi];
            if (!rec.in_contact) continue;
            GpuContactSolverPipeline::Contact contact;
            contact.a = static_cast<std::uint32_t>(rec.index_a);
            contact.b = static_cast<std::uint32_t>(rec.index_b);
            contact.normal = {rec.normal.x, rec.normal.y, rec.normal.z};
            contact.point = {rec.contact_pt.x, rec.contact_pt.y, rec.contact_pt.z};
            contact.surface_a = {rec.surface_a.x, rec.surface_a.y, rec.surface_a.z};
            contact.surface_b = {rec.surface_b.x, rec.surface_b.y, rec.surface_b.z};
            contact.penetration = rec.penetration;
            contact.accumulated_j = rec.accumulated_j;
            solver_contacts.push_back(contact);
            solver_pairs.push_back(pi);
        }
        if (!solver_contacts.empty()) {
            std::vector<GpuContactSolverPipeline::Body> solver_bodies;
            std::vector<std::uint32_t> solver_source_indices;
            solver_bodies.reserve(mechanics_data.size());
            solver_source_indices.reserve(mechanics_data.size());
            for (const auto& data : mechanics_data) {
                const auto& body = impl_->body(data.handle);
                const auto& params = frame_params.at(data.handle);
                GpuContactSolverPipeline::Body input;
                input.center = {data.center_world.x, data.center_world.y, data.center_world.z};
                input.inertia_inv = {data.inertia_inv_body.x, data.inertia_inv_body.y, data.inertia_inv_body.z};
                for (std::size_t col = 0; col < 3; ++col)
                    for (std::size_t row = 0; row < 3; ++row)
                        input.rotation[col * 3 + row] = data.rot_body_to_world[col][row];
                input.mass = params.mass;
                input.restitution = params.restitution;
                input.dynamic = params.body_type == BodyType::Dynamic;
                input.sleeping = body.sleeping;
                input.linear_lock = body.linear_lock;
                input.angular_lock = body.angular_lock;
                solver_bodies.push_back(input);
                solver_source_indices.push_back(body_state_indices.at(data.handle));
            }
            if (impl_->shutdown_requested.load(std::memory_order_acquire)) return;
            impl_->gpu_contact_solver.ensure();
            GpuContactSolverPipeline::Params params;
            params.dt = fixed_dt;
            params.restitution_threshold = 2.0f * std::sqrt(ktm::dot(gravity, gravity)) * fixed_dt + sleep_threshold;
            params.max_linear_speed = max_linear_speed;
            params.max_angular_speed = max_angular_speed;
            params.max_impulse = max_impulse_per_contact;
            params.friction = friction_coeff;
            params.static_friction = static_friction_coeff;
            params.early_exit_velocity = k_early_exit_vel_eps;
            params.iterations = k_impulse_iterations;
            const auto solved = impl_->gpu_contact_solver->solve_from_velocities(
                solver_bodies, solver_contacts, params, integrated_velocities, solver_source_indices);
            solved_velocities = impl_->gpu_contact_solver->velocity_view();
            for (std::size_t ci = 0; ci < solved.contacts.size(); ++ci) {
                auto& rec = pair_records[solver_pairs[ci]];
                const auto& state = solved.contacts[ci];
                rec.inv_ma = state.inv_ma;
                rec.inv_mb = state.inv_mb;
                rec.accumulated_j = state.accumulated_j;
                rec.v_n_initial = state.v_n_initial;
            }
        }
        phase.emplace("physics.contact_postprocess");
        // E2 Bug-3 修复：把本子步最终的 accumulated_j 写回跨子步缓存，供下一子步 warm start 使用
        for (std::size_t pair_idx = 0; pair_idx < collision_pairs.size(); ++pair_idx) {
            const auto& pair = collision_pairs[pair_idx];
            const PairContactRecord& rec = pair_records[pair_idx];
            const auto sorted = (pair.first < pair.second)
                ? std::make_pair(pair.first, pair.second)
                : std::make_pair(pair.second, pair.first);
            if (rec.in_contact && rec.accumulated_j > 0.f) {
                impl_->warm_start_cache[sorted] = {rec.normal, rec.contact_pt, rec.accumulated_j, rec.triangle_a, rec.triangle_b};
            } else {
                // 本子步无接触：清除缓存，防止下一子步因过期数据注入无效冲量
                impl_->warm_start_cache.erase(sorted);
            }
        }

        // ===== E1：迭代后处理（每子步恰好一次，与是否收敛早退无关）=====
        // 原先这四项放在 impulse_iter == 末轮 的分支里，而收敛早退在其后：静止接触通常
        // 1–2 轮即满足早退，于是位置校正 / 唤醒 / 活跃对 / 开始回调几乎从不执行。
        // 「是否接触」以本子步窄相结论为准，不再取决于末轮恰好处于接近还是分离。
        for (std::size_t pair_idx = 0; pair_idx < collision_pairs.size(); ++pair_idx) {
            if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
                return;
            }
            const PairContactRecord& rec = pair_records[pair_idx];
            if (!rec.in_contact) {
                continue;
            }
            const std::uintptr_t ha = collision_pairs[pair_idx].first;
            const std::uintptr_t hb = collision_pairs[pair_idx].second;
            const MechanicsWorldAABB& a = mechanics_data[rec.index_a];
            const MechanicsWorldAABB& b = mechanics_data[rec.index_b];
            const ktm::fvec3& normal = rec.normal;
            const float inv_ma = rec.inv_ma;
            const float inv_mb = rec.inv_mb;

            // 按穿透深度记录软位置校正（延迟到 Phase 6 积分后统一应用，避免抖动）
            const float pen = std::max(0.f, rec.penetration - positional_slop);
            // 钳制穿透深度以防止大穿透时的位置校正过大（如物体生成在碰撞体内部）
            const float pen_clamped = std::min(pen, max_position_correction / k_positional_percent);
            if (pen_clamped > 0.f) {
                const auto unlocked_normal = [&](std::uintptr_t h) {
                    auto n = normal;
                    for (int axis = 0; axis < 3; ++axis)
                        if (impl_->body(h).linear_lock & (1u << axis)) n[axis] = 0;
                    return n;
                };
                const auto na = unlocked_normal(ha), nb = unlocked_normal(hb);
                const float inv_sum = inv_ma * ktm::dot(normal, na) + inv_mb * ktm::dot(normal, nb);
                if (inv_sum > eps) {
                    const float corr_scale = k_positional_percent * pen_clamped / inv_sum;
                    const auto record_corr = [&](std::uintptr_t handle, float inv_eff, float sign) {
                        if (inv_eff <= eps) return;
                        auto& corr = position_correction[handle];  // 默认初始化为 {0,0,0}
                        const auto n = unlocked_normal(handle);
                        corr.x += sign * n.x * corr_scale * inv_eff;
                        corr.y += sign * n.y * corr_scale * inv_eff;
                        corr.z += sign * n.z * corr_scale * inv_eff;
                    };
                    record_corr(ha, inv_ma, -1.f);
                    record_corr(hb, inv_mb, +1.f);
                }
            }

            // 记录活跃碰撞对（窄相判定接触即记录，末轮恰好分离的接触对也不再漏记，
            // 避免持续接触在相邻子步间反复触发 end/begin）
            auto actor_a = actor_for_mechanics(ha);
            auto actor_b = actor_for_mechanics(hb);
            auto sorted_pair = (actor_a < actor_b) ? std::make_pair(actor_a, actor_b) : std::make_pair(actor_b, actor_a);
            curr_active_collisions.insert(sorted_pair);

            // ==================== 碰撞回调（延迟到帧末执行，避免在物理循环中持有锁时调用） ========================
            {
                ktm::fvec3 point;
                point.x = (a.center_world.x + b.center_world.x) * 0.5f;
                point.y = (a.center_world.y + b.center_world.y) * 0.5f;
                point.z = (a.center_world.z + b.center_world.z) * 0.5f;

                std::function<void(std::uintptr_t, bool, const std::array<float, 3>&, const std::array<float, 3>&)> cb_a;
                std::function<void(std::uintptr_t, bool, const std::array<float, 3>&, const std::array<float, 3>&)> cb_b;

                {
                    auto mech_a_acc = mechanics_storage.try_acquire_read(ha);
                    if (mech_a_acc && mech_a_acc->collision_callback) {
                        cb_a = mech_a_acc->collision_callback;
                    }
                }

                {
                    auto mech_b_acc = mechanics_storage.try_acquire_read(hb);
                    if (mech_b_acc && mech_b_acc->collision_callback) {
                        cb_b = mech_b_acc->collision_callback;
                    }
                }

                std::array<float, 3> normal_arr = {normal.x, normal.y, normal.z};
                std::array<float, 3> point_arr = {point.x, point.y, point.z};

                bool was_active = (impl_->prev_active_collisions.find(sorted_pair) != impl_->prev_active_collisions.end());

                if (!was_active && !impl_->shutdown_requested.load(std::memory_order_acquire)) {
                    if (cb_a) {
                        impl_->deferred_collision_callbacks.push_back({std::move(cb_a), actor_b, true, normal_arr, point_arr});
                    }

                    if (cb_b) {
                        std::array<float, 3> reverse_normal_arr = {-normal.x, -normal.y, -normal.z};
                        impl_->deferred_collision_callbacks.push_back({std::move(cb_b), actor_a, true, reverse_normal_arr, point_arr});
                    }
                }
            }
            // =====================================================
        }  // E1：迭代后处理

        // ===== 碰撞结束检测：遍历上帧活跃但本帧消失的碰撞对，延迟触发 end 回调 =====
        for (const auto& old_pair : impl_->prev_active_collisions) {
            if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
                return;
            }
            if (curr_active_collisions.find(old_pair) != curr_active_collisions.end()) {
                continue;  // 仍在碰撞，不触发 end
            }

            std::uintptr_t actor_a = old_pair.first;
            std::uintptr_t actor_b = old_pair.second;

            // 反查 actor_handle → mechanics_handle
            std::uintptr_t mech_ha = first_mechanics_for_actor(actor_a);
            std::uintptr_t mech_hb = first_mechanics_for_actor(actor_b);

            std::array<float, 3> zero_normal = {0.f, 0.f, 0.f};
            std::array<float, 3> zero_point = {0.f, 0.f, 0.f};

            if (mech_ha != 0) {
                std::function<void(std::uintptr_t, bool, const std::array<float, 3>&, const std::array<float, 3>&)> cb;
                {
                    auto m_acc = mechanics_storage.try_acquire_read(mech_ha);
                    if (m_acc && m_acc->collision_callback && !impl_->shutdown_requested.load(std::memory_order_acquire)) {
                        cb = m_acc->collision_callback;
                    }
                }
                if (cb) {
                    impl_->deferred_collision_callbacks.push_back({std::move(cb), actor_b, false, zero_normal, zero_point});
                }
            }

            if (mech_hb != 0) {
                std::function<void(std::uintptr_t, bool, const std::array<float, 3>&, const std::array<float, 3>&)> cb;
                {
                    auto m_acc = mechanics_storage.try_acquire_read(mech_hb);
                    if (m_acc && m_acc->collision_callback && !impl_->shutdown_requested.load(std::memory_order_acquire)) {
                        cb = m_acc->collision_callback;
                    }
                }
                if (cb) {
                    impl_->deferred_collision_callbacks.push_back({std::move(cb), actor_a, false, zero_normal, zero_point});
                }
            }
        }
        // 更新上一帧碰撞对
        impl_->prev_active_collisions.swap(curr_active_collisions);
    } else {
        // 物体数量不足2个时，为残留的碰撞对延迟发送 collision end 回调
        for (const auto& old_pair : impl_->prev_active_collisions) {
            if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
                return;
            }
            std::uintptr_t actor_a = old_pair.first;
            std::uintptr_t actor_b = old_pair.second;

            std::uintptr_t mech_ha = first_mechanics_for_actor(actor_a);
            std::uintptr_t mech_hb = first_mechanics_for_actor(actor_b);

            std::array<float, 3> zero_normal = {0.f, 0.f, 0.f};
            std::array<float, 3> zero_point = {0.f, 0.f, 0.f};

            if (mech_ha != 0) {
                std::function<void(std::uintptr_t, bool, const std::array<float, 3>&, const std::array<float, 3>&)> cb;
                {
                    auto m_acc = mechanics_storage.try_acquire_read(mech_ha);
                    if (m_acc && m_acc->collision_callback && !impl_->shutdown_requested.load(std::memory_order_acquire)) {
                        cb = m_acc->collision_callback;
                    }
                }
                if (cb) {
                    impl_->deferred_collision_callbacks.push_back({std::move(cb), actor_b, false, zero_normal, zero_point});
                }
            }

            if (mech_hb != 0) {
                std::function<void(std::uintptr_t, bool, const std::array<float, 3>&, const std::array<float, 3>&)> cb;
                {
                    auto m_acc = mechanics_storage.try_acquire_read(mech_hb);
                    if (m_acc && m_acc->collision_callback && !impl_->shutdown_requested.load(std::memory_order_acquire)) {
                        cb = m_acc->collision_callback;
                    }
                }
                if (cb) {
                    impl_->deferred_collision_callbacks.push_back({std::move(cb), actor_a, false, zero_normal, zero_point});
                }
            }
        }
        impl_->prev_active_collisions.clear();
    }

    phase.emplace("physics.final_pose");
    // Stage 5b axis projection runs in the final pose kernel. Bodies without
    // geometry cannot receive contact impulses and keep their stage 2 projection.
    const bool resident_velocities = static_cast<bool>(solved_velocities.buffer);
    impl_->gpu_floor_sleep->set_velocities(integrated_velocities, &gpu_batch);
    const auto pose_velocity_source = resident_velocities ? solved_velocities : integrated_velocities;
    const auto pose_velocity_target = impl_->gpu_floor_sleep->velocity_target();

    // --- 阶段 6：半隐式位姿积分（用冲量后的 v,ω）+ 无穷地板 + 休眠累计 + 缓存淘汰 ---
    std::vector<GpuRigidIntegrationPipeline::PoseInput> final_pose_inputs;
    std::vector<std::size_t> final_pose_targets;
    std::vector<bool> final_velocity_locks;
    std::unordered_set<std::uintptr_t> pending_transforms;
    final_pose_inputs.reserve(mechanics_data.size());
    final_pose_targets.reserve(mechanics_data.size());
    final_velocity_locks.reserve(mechanics_data.size());
    auto flush_final_poses = [&]() {
        if (final_pose_inputs.empty()) return true;
        if (impl_->shutdown_requested.load(std::memory_order_acquire)) return false;
        std::vector<GpuRigidIntegrationPipeline::ResidentPoseInput> resident_inputs;
        std::vector<std::uint32_t> destination_indices;
        resident_inputs.reserve(final_pose_inputs.size());
        destination_indices.reserve(final_pose_inputs.size());
        for (std::size_t j = 0; j < final_pose_inputs.size(); ++j) {
            const auto destination = body_state_indices.at(mechanics_data[final_pose_targets[j]].handle);
            const auto source = resident_velocities ? static_cast<std::uint32_t>(final_pose_targets[j]) : destination;
            resident_inputs.push_back({final_pose_inputs[j], source, final_velocity_locks[j]});
            destination_indices.push_back(destination);
        }
        const auto poses = impl_->gpu_rigid_integration->integrate_poses_to_velocities(
            resident_inputs, pose_velocity_source, pose_velocity_target, destination_indices, fixed_dt, &gpu_batch);
        for (std::size_t j = 0; j < final_pose_inputs.size(); ++j) {
            if (impl_->shutdown_requested.load(std::memory_order_acquire)) return false;
            const auto& data = mechanics_data[final_pose_targets[j]];
            auto& body = impl_->body(data.handle);
            // Static/sleeping/invalid-transform bodies still scatter their
            // velocity on device, but must not advance the authoritative pose.
            if (!final_pose_inputs[j].advance) continue;
            // Use the blocking write lock, as before: contention must not skip
            // a physical step. An invalidated transform still skips its body.
            auto tx_w = transform_storage.try_acquire_write(data.transform_handle);
            if (!tx_w) continue;
            auto& expected = step_transforms.at(data.transform_handle);
            if (!same_transform(*tx_w, expected)) {
                // The GPU result belongs to the old pose. Leave the edit intact
                // and observe it at the next step, including rotation/scale.
                body.sleeping = false;
                body.reset_sleep_timer = true;
                continue;
            }
            const auto& pose = poses[j];
            tx_w->position = make_fvec3(pose.position[0], pose.position[1], pose.position[2]);
            body.orientation_quat = ktm::fquat::real_imag(pose.orientation[3],
                make_fvec3(pose.orientation[0], pose.orientation[1], pose.orientation[2]));
            sync_euler_from_orientation_quat(body.orientation_quat, tx_w->euler_rotation);
            expected = *tx_w;
        }
        final_pose_inputs.clear();
        final_pose_targets.clear();
        final_velocity_locks.clear();
        pending_transforms.clear();
        return true;
    };

    for (std::size_t i = 0; i < mechanics_data.size(); ++i) {
        if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
            return;
        }
        const auto& data = mechanics_data[i];  // 与阶段 3 同一套 per-body 缓存
        std::uintptr_t h = data.handle;
        // Stage 5b projection uses sleep state before a lowered floor wakes the
        // body below. Pose integration itself uses the updated state.
        const bool apply_velocity_locks = !impl_->body(h).sleeping;

        // 唤醒因地板降低而悬空的休眠体（物体搁在地板上休眠后，地板调低应自由落体）
        if (impl_->body(h).sleeping && !supported_bodies.contains(h) && data.min_world.y > floor_y + floor_eps + 1e-4f) {
            impl_->body(h).sleeping = false;
            impl_->body(h).reset_sleep_timer = true;
        }

        const bool advance = !impl_->body(h).sleeping && frame_params[h].body_type == BodyType::Dynamic;

        // Distinct mechanics bodies can alias one transform. Flush before
        // reading that transform again so their translations remain ordered.
        if (advance && pending_transforms.contains(data.transform_handle) && !flush_final_poses()) return;
        const auto& body = impl_->body(h);  // Orientation was initialized in stage 3.
        GpuRigidIntegrationPipeline::PoseInput input;
        input.advance = advance;
        if (advance) {
            const auto& transform = step_transforms.at(data.transform_handle);
            input.position = {transform.position.x, transform.position.y, transform.position.z};
        }
        input.orientation = {body.orientation_quat.i, body.orientation_quat.j,
                             body.orientation_quat.k, body.orientation_quat.r};
        input.linear_lock = body.linear_lock;
        input.angular_lock = body.angular_lock;
        if (const auto corr = position_correction.find(h); corr != position_correction.end()) {
            const float length = std::sqrt(ktm::dot(corr->second, corr->second));
            const float scale = length > max_position_correction ? max_position_correction / length : 1.0f;
            input.correction = {corr->second.x * scale, corr->second.y * scale, corr->second.z * scale};
        }
        final_pose_inputs.push_back(input);
        final_pose_targets.push_back(i);
        final_velocity_locks.push_back(apply_velocity_locks);
        if (input.advance) pending_transforms.insert(data.transform_handle);
    }
    if (!flush_final_poses()) return;
    (void)gpu_batch.flush();  // An empty pose set still installs the integrated state.

    phase.emplace("physics.floor");
    // Query exact skinned world-space minima after integration and positional
    // correction, once per body. The result also identifies the contact bone.
    std::unordered_map<std::uintptr_t, GpuCollisionPipeline::VertexResult> floor_samples;
    if (impl_->gpu_collision) {
        std::vector<GpuCollisionPipeline::VertexQuery> queries;
        std::vector<std::uintptr_t> targets;
        for (const auto& data : mechanics_data) {
            if (!data.is_skinned || !ready_collision_keys.contains(data.geom_handle)) continue;
            const auto params = frame_params.find(data.handle);
            if (params == frame_params.end() || params->second.body_type != BodyType::Dynamic) continue;
            const auto& transform = step_transforms.at(data.transform_handle);
            queries.push_back({{data.geom_handle, gpu_matrix(transform.compute_matrix())}, {}});
            targets.push_back(data.handle);
        }
        if (!queries.empty()) {
            const auto samples = impl_->gpu_collision->sample_vertices(queries);
            for (std::size_t i = 0; i < samples.size(); ++i) floor_samples.emplace(targets[i], samples[i]);
        }
    }

    // Evaluate floor candidates without changing the working velocity state.
    // A transform invalidated before writeback must cancel the entire response.
    std::vector<GpuFloorSleepPipeline::FloorInput> floor_inputs;
    std::vector<std::size_t> floor_targets;
    for (std::size_t i = 0; i < mechanics_data.size(); ++i) {
        if (impl_->shutdown_requested.load(std::memory_order_acquire)) return;
        const auto& data = mechanics_data[i];
        const auto h = data.handle;
        const auto fp_it = frame_params.find(h);
        if (fp_it == frame_params.end()) continue;
        const auto btype = fp_it->second.body_type;
        if (btype != BodyType::Dynamic) continue;
        const auto& transform = step_transforms.at(data.transform_handle);
        ktm::fvec3 floor_min, floor_max, floor_center;
        world_aabb_from_local_bounds(transform, data.local_min, data.local_max, floor_min, floor_max, floor_center);
        auto& body = impl_->body(h);
        if (btype == BodyType::Dynamic && body.sleeping) {
            if ((!supported_bodies.contains(h) && floor_min.y > floor_y + floor_eps + 1e-4f) || floor_min.y < floor_y - floor_eps) {
                body.sleeping = false;
                body.reset_sleep_timer = true;
            } else {
                continue;
            }
        }
        GpuFloorSleepPipeline::FloorInput input;
        input.body = body_state_indices.at(h);
        input.center = {floor_center.x, floor_center.y, floor_center.z};
        input.inertia_inv = {data.inertia_inv_body.x, data.inertia_inv_body.y, data.inertia_inv_body.z};
        const auto floor_rotation = quat_from_model_euler(transform.euler_rotation).matrix3x3();
        for (std::size_t col = 0; col < 3; ++col)
            for (std::size_t row = 0; row < 3; ++row)
                input.rotation[col * 3 + row] = floor_rotation[col][row];
        input.mass = fp_it->second.mass;
        input.bottom = floor_min.y;
        input.linear_lock = body.linear_lock;
        input.angular_lock = body.angular_lock;
        input.skinned = data.is_skinned;
        if (data.is_skinned) {
            const auto sample = floor_samples.find(h);
            if (sample == floor_samples.end()) continue;
            input.lowest = sample->second.lowest_world;
            input.bottom = input.lowest[1];
        }
        floor_inputs.push_back(input);
        floor_targets.push_back(i);
    }
    const auto floor_contacts = impl_->gpu_floor_sleep->evaluate_floor(floor_inputs,
        {floor_y, floor_eps, floor_restitution, gravity.y, fixed_dt, low_vel_threshold,
         zero_vel_threshold, friction_coeff, static_friction_coeff});
    std::vector<std::uint8_t> accepted_floor(floor_inputs.size(), 0);
    for (std::size_t i = 0; i < floor_contacts.size(); ++i) {
        if (impl_->shutdown_requested.load(std::memory_order_acquire)) return;
        if (!floor_contacts[i].contact) continue;
        const auto& data = mechanics_data[floor_targets[i]];
        const auto h = data.handle;
        auto tx_w = transform_storage.try_acquire_write(data.transform_handle);
        if (!tx_w) continue;
        auto& expected = step_transforms.at(data.transform_handle);
        if (!same_transform(*tx_w, expected)) continue;
        if (!(impl_->body(h).linear_lock & kLockAxisY)) tx_w->position.y += floor_contacts[i].delta_y;
        expected = *tx_w;
        accepted_floor[i] = 1;
        // 蒙皮额外：IK target 入队（最低顶点所在骨骼）
        if (data.is_skinned) {
            auto sc_it = impl_->skinned_collision_cache.find(data.geom_handle);
            if (sc_it != impl_->skinned_collision_cache.end() && !sc_it->second.triangles.empty()) {
                const auto& sc = sc_it->second;
                const auto sample = floor_samples.find(h);
                if (sample != floor_samples.end() && data.geom_handle != 0) {
                    const auto& lowest = sample->second;
                    const ktm::fvec3 contact_world = make_fvec3(
                        lowest.lowest_world[0], floor_y, lowest.lowest_world[2]);
                    const int best_bone = lowest.lowest_vertex < sc.vertex_bone_ids.size()
                        ? sc.vertex_bone_ids[lowest.lowest_vertex] : -1;
                    if (best_bone >= 0) {
                        auto geometry = geometry_storage.try_acquire_read(data.geom_handle);
                        if (geometry && std::any_of(geometry->ik_chains.begin(), geometry->ik_chains.end(),
                            [](const auto& chain) { return chain.enabled && chain.weight > 0 && chain.contact_driven &&
                                chain.mode != Resource::IkChain::Mode::FootPlant; })) {
                            impl_->deferred_ik_target_updates.push_back({data.geom_handle, data.transform_handle,
                                best_bone, contact_world, make_fvec3(0, 1, 0), geometry->ik_chains_revision});
                        }
                    }
                }
            }
        }
    }

    // Measure residual overlap on the predicted contact plane after the final
    // translation/floor correction. Testing predicted depth alone continually
    // resets quiet stacks because that pose still contains the gravity step.
    // Angular motion is independently covered by the GPU sleep speed test.
    for (const auto& contact : penetration_constraints) {
        const auto& a = mechanics_data[contact.a];
        const auto& b = mechanics_data[contact.b];
        const auto delta_a = step_transforms.at(a.transform_handle).position - collision_transforms[contact.a].position;
        const auto delta_b = step_transforms.at(b.transform_handle).position - collision_transforms[contact.b].position;
        if (contact.depth - ktm::dot(delta_b - delta_a, contact.normal) <= positional_slop * 2.0f) continue;
        for (auto h : {a.handle, b.handle}) {
            if (frame_params.at(h).body_type != BodyType::Dynamic) continue;
            auto& body = impl_->body(h);
            body.sleeping = false; body.reset_sleep_timer = true;
        }
    }

    // Record only snapshots and accepted physics writes. A UI edit arriving
    // during GPU work stays different and will wake the body next step.
    for (const auto& data : mechanics_data) {
        auto& body = impl_->body(data.handle);
        body.last_transform = step_transforms.at(data.transform_handle);
    }

    phase.emplace("physics.sleep");
    impl_->gpu_floor_sleep->commit_floor(accepted_floor, &gpu_batch);
    std::vector<GpuFloorSleepPipeline::ResidentSleepInput> sleep_inputs;
    sleep_inputs.reserve(mechanics_handles.size());
    for (auto h : mechanics_handles) {
        const auto& body = impl_->body(h);
        const auto params = frame_params.find(h);
        sleep_inputs.push_back({body.sleeping,
            params != frame_params.end() && params->second.body_type == BodyType::Dynamic, body.reset_sleep_timer});
    }
    // Only sleeping/was_awake routing bits return. Velocities and timers remain
    // on device, including bodies without geometry, for the next fixed step.
    const auto final_states = impl_->gpu_floor_sleep->finish_sleep_resident(sleep_inputs,
        {fixed_dt, sleep_threshold, sleep_time_needed}, &gpu_batch);
    phase.emplace("physics.events_cleanup");
    for (std::size_t i = 0; i < final_states.size(); ++i) {
        auto& body = impl_->body(mechanics_handles[i]);
        const auto& state = final_states[i];
        body.sleeping = state.sleeping;
        body.reset_sleep_timer = false;
    }
    for (std::size_t i = 0; i < mechanics_handles.size(); ++i) {
        if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
            return;
        }
        // Newly sleeping bodies still run this step's movement callback.
        if (!final_states[i].was_awake) continue;
        const auto h = mechanics_handles[i];

        // ========== 异步执行移动回调 ==========
        {
            auto mech_acc = mechanics_storage.try_acquire_read(h);
            if (mech_acc && mech_acc->on_move_callback) {
                std::function<void()> cb_move = mech_acc->on_move_callback;

                if (cb_move) {
                    // 获取当前位置用于位移检查
                    ktm::fvec3 cur_pos = make_fvec3(0.f, 0.f, 0.f);
                    bool has_pos = false;
                    if (auto geom = geometry_storage.try_acquire_read(mech_acc->geometry_handle)) {
                        if (auto tx_r = transform_storage.try_acquire_read(geom->transform_handle)) {
                            cur_pos = tx_r->position;
                            has_pos = true;
                        }
                    }
                    if (!has_pos) continue;

                    auto& body = impl_->body(h);
                    // 1. 时间检查
                    bool time_elapsed =
                        (impl_->global_simulation_time - body.last_move_callback_time >= kMoveCallbackMinInterval);

                    // 2. 位移检查
                    ktm::fvec3 last_pos = body.last_move_callback_pos;

                    float dx = cur_pos.x - last_pos.x;
                    float dy = cur_pos.y - last_pos.y;
                    float dz = cur_pos.z - last_pos.z;
                    float dist_sq = dx * dx + dy * dy + dz * dz;
                    bool moved_enough = (dist_sq >= kMoveCallbackMinDistance * kMoveCallbackMinDistance);

                    // 3. 同时满足时间间隔和位移阈值才触发
                    if (time_elapsed && moved_enough && !impl_->shutdown_requested.load(std::memory_order_acquire)) {
                        body.last_move_callback_time = impl_->global_simulation_time;
                        body.last_move_callback_pos = cur_pos;

                        // 收集到延迟队列，帧末统一同步执行
                        impl_->deferred_move_callbacks.push_back(std::move(cb_move));
                    }
                }
            }
        }
        // =============================================================
    }

    // Python binding 将调用投递给解释器 pending-call 队列；这里不会获取 GIL 或写盘。
    for (auto& cb : impl_->deferred_move_callbacks) {
        if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
            break;
        }
        try {
            cb();
        } catch (const std::exception& e) {
            CFW_LOG_ERROR("MechanicsSystem: on_move callback exception: {}", e.what());
        } catch (...) {
            CFW_LOG_ERROR("MechanicsSystem: on_move callback unknown exception");
        }
    }
    impl_->deferred_move_callbacks.clear();

    // 碰撞回调 binding 同样只投递 pending call，保持 enter/exit 顺序且不丢事件。
    for (auto& cb : impl_->deferred_collision_callbacks) {
        if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
            break;
        }
        try {
            cb.callback(cb.other_actor, cb.is_start, cb.normal, cb.point);
        } catch (const std::exception& e) {
            CFW_LOG_ERROR("MechanicsSystem: collision callback exception: {}", e.what());
        } catch (...) {
            CFW_LOG_ERROR("MechanicsSystem: collision callback unknown exception");
        }
    }
    impl_->deferred_collision_callbacks.clear();

    // Contact feedback is consumed before CCD in the next animation update.
    // Multiple physics substeps are aggregated into one activation update per chain.

    // 清理无效句柄的缓存
    std::unordered_set<std::uintptr_t> alive_handles(mechanics_handles.begin(), mechanics_handles.end());

    // 收集存活的 geom_handle，用于清理实例拓扑缓存
    std::unordered_set<std::uintptr_t> alive_geom_handles;
    for (const auto& data : mechanics_data) {
        if (data.geom_handle) alive_geom_handles.insert(data.geom_handle);
    }
    // Skin poses also support Foot queries in scenes with simulation disabled.
    // Their lifetime follows live render geometry, not the physics participant list.
    // 只读遍历：用几何存储的 const 视图（cbegin/cend → 槽位共享锁）。此前这里直接
    // 遍历非 const 引用，Storage::begin() 取的是**独占**槽锁，等于每帧把整张几何表
    // 锁成写级别，与 CEF 线程的 IK 写锁、几何系统的读取互相排队。
    for (auto geometry_it = geometry_storage.cbegin(); geometry_it != geometry_storage.cend();
         ++geometry_it) {
        const GeometryDevice& geometry = *geometry_it;
        if (geometry.is_skinned && geometry.model_resource_handle && !geometry.mesh_handles.empty() &&
            geometry.gpu_build_state == GeometryDevice::GpuBuildState::Ready) {
            alive_geom_handles.insert(reinterpret_cast<std::uintptr_t>(&geometry));
        }
    }

    for (auto it = impl_->bodies.begin(); it != impl_->bodies.end();) {
        if (!alive_handles.count(it->first))
            it = impl_->bodies.erase(it);
        else
            ++it;
    }

    // G4：清理 geom_handle 键的 stale 缓存（物体离开场景时释放内存）
    for (auto it = impl_->skinned_collision_cache.begin(); it != impl_->skinned_collision_cache.end();) {
        if (!alive_geom_handles.count(it->first)) it = impl_->skinned_collision_cache.erase(it);
        else ++it;
    }
    std::erase_if(impl_->skinned_collision_model_ids,
                  [&](const auto& item) { return !alive_geom_handles.count(item.first); });

    for (auto it = impl_->skinned_octree_cache.begin(); it != impl_->skinned_octree_cache.end();) {
        if (!alive_geom_handles.count(it->first)) it = impl_->skinned_octree_cache.erase(it);
        else ++it;
    }


    // E2 warm_start_cache stale 清理：移除本帧不再有接触的 pair
    {
        for (auto it = impl_->warm_start_cache.begin(); it != impl_->warm_start_cache.end();) {
            const auto& k = it->first;
            bool found = false;
            for (const auto& pair : collision_pairs) {
                const auto sorted = (pair.first < pair.second)
                    ? std::make_pair(pair.first, pair.second)
                    : std::make_pair(pair.second, pair.first);
                if (sorted == k) { found = true; break; }
            }
            if (!found) it = impl_->warm_start_cache.erase(it);
            else ++it;
        }
    }

}

// ============================================================================
// update_skinned_geometry
// 功能：每真实帧通过 Vulkan/Helicon EDSL 做线性混合蒙皮（LBS），更新 GPU 顶点缓冲。
// 调用时机：MechanicsSystem::update() 的固定步进循环之前（Phase 1 调整）。
//           先蒙皮生成本帧正确 AABB 和蒙皮顶点，物理随后消费同帧数据，消除旧版
//           一帧延迟。蒙皮模型即使未开物理也应自动循环播放。
//
// 数据流：CPU 动画采样/FK/IK → 骨骼矩阵上传 → GPU 蒙皮/AABB 归约 →
//         GPU 拷贝到 vertexBuffer/vertexStorageBuffer。绑定顶点和权重缓存在 GPU。
//         LOD0 直接供 GPU 碰撞消费；仅活跃 Vision 相机请求 CPU 顶点副本。
//         GPU 失败直接向系统错误处理器传播，不发布该批结果或继续物理步进。
//
// LOD1..N 经 GeometrySystem::get_skinning_lod_targets() 借出引用计数句柄和源 LOD
// 索引；LOD0 从 GeometryDevice.mesh_handles 取。只有已驻留级别参与 GPU 更新。
//
// 锁序：Scene 共享读锁保活输入；GPU 调度及等待在 geometry storage 锁外执行；
//       get_skinning_lod_targets 内部锁 lod_cache_mutex，必须在释放 geom 写锁后调用。
//
// 自动循环：始终播放 animations[0]，anim_time 由 advance_anim_time 推进并 fmod 回绕。
// ============================================================================
void MechanicsSystem::update_skinned_geometry(float dt) {
    GpuPerformance::Scope performance{"animation.frame"};
    // 懒缓存 GeometrySystem 指针（first_update 路径可能未经过 update_physics）
    if (!impl_->geometry_sys && impl_->ctx) {
        impl_->geometry_sys = dynamic_cast<GeometrySystem*>(impl_->ctx->get_system("Geometry"));
    }
    if (!impl_->geometry_sys) return;  // LOD1..N 句柄借不到，且无意义

    auto& resource_manager = Resource::ResourceManager::get_instance();
    auto& hub = SharedDataHub::instance();
    auto& geom_storage = hub.geometry_storage();

    // dt 由 update() 测量并传入（已钳制 ≤0.1s），此处不再独立计时。
    // last_skin_update_time 保留但不再用于计算 dt，仅供外部诊断查询。
    impl_->last_skin_update_time = std::chrono::steady_clock::now();

    Corona::Diagnostics::mark_thread_phase("Mechanics", "skinned:collect_handles");
    std::vector<std::uintptr_t> geom_handles;
    bool needs_ground_queries = false;
    for (auto it = geom_storage.cbegin(); it != geom_storage.cend(); ++it) {
        const GeometryDevice& geom_dev = *it;
        geom_handles.push_back(reinterpret_cast<std::uintptr_t>(&geom_dev));
        for (const auto& chain : geom_dev.ik_chains)
            needs_ground_queries |= chain.mode == Resource::IkChain::Mode::FootPlant && chain.enabled && chain.weight > 0;
    }
    Corona::Diagnostics::mark_thread_phase("Mechanics", "skinned:ground_cache");
    // Retain the latest physics sample across animation frames with zero fixed
    // steps. Reproject its world targets using the current actor transform below.
    // The next physics step replaces it (also discarding earlier catch-up samples).
    const auto& pending_ik_updates = impl_->deferred_ik_target_updates;

    // Vision still imports CPU geometry; rasterization and mechanics consume GPU buffers.
    bool vision_readback = false;
    for (const auto& enabled_scene : std::as_const(hub.scene_storage())) {
        if (!enabled_scene.enabled) continue;
        for (auto camera_handle : enabled_scene.camera_handles) {
            auto camera = hub.camera_storage().try_acquire_read(camera_handle);
            if (camera && camera->surface && camera->render_backend == CameraRenderBackend::Vision) {
                vision_readback = true;
                break;
            }
        }
        if (vision_readback) break;
    }
    std::vector<std::uint64_t> active_gpu_keys;
    const auto invalidate_skin = [&](std::uintptr_t key) {
        impl_->skinned_collision_cache.erase(key);
        impl_->skinned_collision_model_ids.erase(key);
        impl_->skinned_octree_cache.erase(key);
        auto geometry = geom_storage.try_acquire_write(key);
        if (!geometry) return;
        const bool had_skin = geometry->is_skinned || geometry->skeleton_cache_model_id != 0;
        geometry->is_skinned = false;
        geometry->skinned_aabb_valid = false;
        geometry->skinned_cpu_vertices.clear();
        geometry->skeleton_cache_model_id = 0;
        geometry->bone_name_to_node_idx.clear();
        geometry->leaf_bone_names.clear();
        if (had_skin) {
            geometry->anim_time = 0.0f;
            for (auto& chain : geometry->ik_chains) chain.runtime = {};
            ++geometry->ik_chains_revision;
        }
    };

    // Keep immutable inputs alive until the scene-wide dispatch completes. One
    // read lock per model also avoids recursively locking a shared skeleton used
    // by several actors on this thread.
    std::unordered_map<std::uint64_t, Resource::ReadHandle<Resource::Scene>> frame_scenes;
    struct SkinFrameJob {
        std::uintptr_t geometry{}, resource{};
        std::uint64_t model{};
        std::size_t mesh_count{};
        std::uintptr_t transform{};
        std::uint64_t ik_revision{};
        float anim_time{};
        bool pose_ready{};
        std::vector<Resource::IkChain> chains;
        std::vector<std::array<float, 16>> locals, finals;
        std::vector<GpuSkinningPipeline::Request> requests;
        std::vector<Horizon::HardwareBuffer> vertex_targets, storage_targets;
        std::unordered_map<std::uint64_t, std::size_t> lod0_meshes;
        std::vector<GpuCollisionPipeline::VertexSource> collision_sources;
        std::vector<std::vector<std::byte>> blobs;
        ktm::fvec3 bounds_min{}, bounds_max{};
        bool bounds_valid{}, collision_ready{};
    };
    std::vector<SkinFrameJob> frame_jobs;
    const auto still_current = [](const GeometryDevice& geometry, const SkinFrameJob& job) {
        if (!(geometry.model_resource_handle == job.resource &&
               geometry.transform_handle == job.transform &&
               geometry.skeleton_cache_model_id == job.model &&
               geometry.gpu_build_state == GeometryDevice::GpuBuildState::Ready &&
               geometry.mesh_handles.size() == job.mesh_count)) return false;
        for (std::size_t i = 0; i < job.mesh_count; ++i) {
            if (!geometry.mesh_handles[i].vertexBuffer.same_resource(job.vertex_targets[i]) ||
                !geometry.mesh_handles[i].vertexStorageBuffer.same_resource(job.storage_targets[i])) return false;
        }
        return true;
    };

    for (auto geom_handle : geom_handles) {
        // ---- 第 1 步：读取 model_resource_handle（brief 读锁）----
        std::uintptr_t model_resource_handle = 0;
        std::uint64_t previous_skin_model = 0;
        bool geometry_ready = false;
        {
            auto geom_read = geom_storage.try_acquire_read(geom_handle);
            if (!geom_read) continue;
            model_resource_handle = geom_read->model_resource_handle;
            previous_skin_model = geom_read->skeleton_cache_model_id;
            geometry_ready = geom_read->gpu_build_state == GeometryDevice::GpuBuildState::Ready &&
                             !geom_read->mesh_handles.empty();
        }
        if (model_resource_handle == 0) {
            invalidate_skin(geom_handle);
            continue;
        }

        // ---- 第 2 步：解析 model_id ----
        std::uint64_t model_id = 0;
        if (auto model_res = hub.model_resource_storage().try_acquire_read(model_resource_handle)) {
            model_id = model_res->model_id;
        }
        if (model_id == 0 || (previous_skin_model != 0 && previous_skin_model != model_id))
            invalidate_skin(geom_handle);
        if (model_id == 0 || !geometry_ready) continue;

        // ---- 第 3 步：取 Scene，判断是否蒙皮（非蒙皮直接跳过）----
        Corona::Diagnostics::PhaseScope loop_phase{"Mechanics", "skinned:scene_lookup"};
        auto scene_entry = frame_scenes.find(model_id);
        if (scene_entry == frame_scenes.end()) {
            auto scene_read = resource_manager.acquire_read<Resource::Scene>(model_id);
            if (!scene_read.valid()) continue;
            scene_entry = frame_scenes.emplace(model_id, std::move(scene_read)).first;
        }
        const Resource::Scene& scene = *scene_entry->second;
        if (!scene.data.skeleton.has_value()) {
            invalidate_skin(geom_handle);
            continue;
        }

        const Resource::SkeletonData& skeleton = *scene.data.skeleton;
        const Resource::AnimationClip empty_clip;
        const Resource::AnimationClip& clip = scene.data.animations.empty() ? empty_clip : scene.data.animations[0];

        // ---- 第 4 步：brief 写锁推进 anim_time + 拷出 buffer 句柄 ----
        // HardwareBuffer 为引用计数句柄，可拷贝；拷出后锁外提交 GPU 蒙皮和拷贝。
        float anim_time = 0.0f;
        std::vector<Horizon::HardwareBuffer> vbufs;
        std::vector<Horizon::HardwareBuffer> vstoragebufs;
        std::size_t mesh_count = 0;
        std::vector<Resource::IkChain> ik_chains;  // 锁外跑 CCD 用（拷出避免持锁）
        std::uintptr_t ik_transform_handle = 0;
        std::uint64_t ik_revision = 0;
        {
            auto geom_write = geom_storage.try_acquire_write(geom_handle);
            if (!geom_write) continue;
            geom_write->is_skinned = true;
            geom_write->anim_time = Resource::advance_anim_time(geom_write->anim_time, dt, clip);
            anim_time = geom_write->anim_time;

            // Build the named-bone cache initially and invalidate it when the model changes.
            if (geom_write->bone_name_to_node_idx.empty() || geom_write->skeleton_cache_model_id != model_id) {
                if (geom_write->skeleton_cache_model_id != model_id) {
                    for (auto& chain : geom_write->ik_chains) chain.runtime = {};
                    if (geom_write->skeleton_cache_model_id != 0) ++geom_write->ik_chains_revision;
                }
                geom_write->bone_name_to_node_idx.clear();
                geom_write->skeleton_cache_model_id = model_id;
                for (std::size_t ni = 0; ni < skeleton.nodes.size(); ++ni) {
                    geom_write->bone_name_to_node_idx[skeleton.nodes[ni].name] = static_cast<int>(ni);
                }
                // 同步构建叶子骨骼列表（children 为空的节点）供编辑器 IK 测试 UI 使用。
                geom_write->leaf_bone_names.clear();
                for (const auto& node : skeleton.nodes) {
                    if (node.children.empty()) {
                        geom_write->leaf_bone_names.push_back(node.name);
                    }
                }
            }

            for (auto& chain : geom_write->ik_chains) {
                if (!chain.end_bone_name.empty()) {
                    const auto node = geom_write->bone_name_to_node_idx.find(chain.end_bone_name);
                    const int resolved = node == geom_write->bone_name_to_node_idx.end() ? -1 : node->second;
                    if (chain.end_node != resolved) chain.runtime = {};
                    chain.end_node = resolved;
                }
                sanitize_foot_ik_endpoint(chain, skeleton);
            }
            ik_transform_handle = geom_write->transform_handle;
            ik_revision = geom_write->ik_chains_revision;
            ik_chains = geom_write->ik_chains;  // 拷贝一份 IK 链定义（锁外跑 CCD）
            mesh_count = geom_write->mesh_handles.size();
            vbufs.reserve(mesh_count);
            vstoragebufs.reserve(mesh_count);
            for (auto& md : geom_write->mesh_handles) {
                vbufs.push_back(md.vertexBuffer);
                vstoragebufs.push_back(md.vertexStorageBuffer);
            }
        }
        if (mesh_count == 0) continue;  // mesh_count==0 等价于 MeshSlot.valid=false：Actor 首次加载未完成

        // ---- 第 4b 步：借出各 mesh 的 LOD1..N GPU 缓冲句柄（蒙皮需写入所有已驻留级别）----
        // 否则拉远切到低 LOD 会读到未蒙皮的绑定姿态顶点 → 冻住不动。
        // 保留 Geometry 缓存的 source_lod_index；CPU 驻留回收可能清空中间级别，
        // 不能再用当前非空 LOD 的压缩序号匹配 GPU 句柄。
        // 注意：必须在释放 geom 写锁之后调用（内部锁 lod_cache_mutex），避免锁嵌套。
        std::vector<std::vector<GeometrySystem::SkinningLodTarget>> lod_targets(mesh_count);
        for (std::size_t mi = 0; mi < mesh_count; ++mi) {
            lod_targets[mi] = impl_->geometry_sys->get_skinning_lod_targets(
                geom_handle, static_cast<uint32_t>(mi));
        }

        // Sample once, then apply automatic drivers before CCD in this same update.
        std::vector<std::array<float, 16>> working_locals;
        {
            GpuPerformance::Scope stage{"animation.sample"};
            GpuPerformance::record_work_items(skeleton.nodes.size());
            Resource::sample_pose_locals(skeleton, clip, anim_time, working_locals);
        }
        SkinFrameJob job;
        job.geometry = geom_handle;
        job.resource = model_resource_handle;
        job.model = model_id;
        job.mesh_count = mesh_count;
        job.transform = ik_transform_handle;
        job.ik_revision = ik_revision;
        job.anim_time = anim_time;
        job.chains = std::move(ik_chains);
        job.locals = std::move(working_locals);
        job.vertex_targets = vbufs;
        job.storage_targets = vstoragebufs;
        const auto append_request = [&](std::size_t mi, std::size_t lod_index,
                                        const auto& vertices, const auto& weights,
                                        const Horizon::HardwareBuffer& vb,
                                        const Horizon::HardwareBuffer& sb, bool readback) {
            if (vertices.empty() || weights.size() != vertices.size()) return;
            // Unloaded LODs have no GPU consumer and must not consume work/cache space.
            if (lod_index != 0 && !vb && !sb) return;
            Impl::GpuMeshIdentity identity{geom_handle, model_id, mi, lod_index};
            auto [entry, inserted] = impl_->gpu_skinning_keys.try_emplace(identity, 0);
            if (inserted) entry->second = impl_->next_gpu_skinning_key++;
            const auto key = entry->second;
            job.requests.push_back({key, model_id, vertices, weights, vb, sb, readback});
            if (lod_index == 0) job.lod0_meshes.emplace(key, mi);
        };
        for (std::size_t mi = 0; mi < mesh_count && mi < scene.data.meshes.size(); ++mi) {
            const auto& mesh = scene.data.meshes[mi];
            if (mesh.bone_weights.empty() || mesh.bone_weights.size() != mesh.vertices.size()) continue;
            append_request(mi, 0, mesh.vertices, mesh.bone_weights, vbufs[mi], vstoragebufs[mi], vision_readback);
            for (const auto& target : lod_targets[mi]) {
                const std::size_t li = target.source_lod_index;
                if (li >= mesh.lod_levels.size()) continue;
                const auto& lod = mesh.lod_levels[li];
                if (lod.vertices.empty() || lod.indices.empty()) continue;
                append_request(mi, li + 1, lod.vertices, lod.bone_weights,
                               target.vertex_buffer, target.storage_buffer, false);
            }
        }
        frame_jobs.push_back(std::move(job));
    }

    // Probe all characters against the one frozen support epoch before any CCD
    // or skinning. Pose vectors have stable addresses once collection is done.
    std::vector<FootPose> foot_poses;
    foot_poses.reserve(frame_jobs.size());
    for (auto& job : frame_jobs) {
        const auto& skeleton = *frame_scenes.at(job.model)->data.skeleton;
        foot_poses.push_back({job.geometry, &skeleton, &job.locals, &job.chains});
    }
    Corona::Diagnostics::mark_thread_phase("Mechanics", "skinned:foot_targets");
    Corona::Diagnostics::mark_thread_phase("Mechanics", "skinned:ccd_pose");
    for (auto& job : frame_jobs) {
        const auto geom_handle = job.geometry;
        const auto ik_transform_handle = job.transform;
        const auto ik_revision = job.ik_revision;
        auto& ik_chains = job.chains;
        auto& working_locals = job.locals;
        const auto& scene = *frame_scenes.at(job.model);
        const auto& skeleton = *scene.data.skeleton;
        const Resource::AnimationClip empty_clip;
        const auto& clip = scene.data.animations.empty() ? empty_clip : scene.data.animations[0];
        ktm::fmat4x4 world_to_model{};
        bool have_transform = false;
        {
            auto tx = hub.model_transform_storage().try_acquire_read(ik_transform_handle);
            if (tx && std::abs(tx->scale.x) > 1e-8f && std::abs(tx->scale.y) > 1e-8f && std::abs(tx->scale.z) > 1e-8f) {
                world_to_model = ktm::inverse(tx->compute_matrix());
                have_transform = true;
            }
        }
        for (auto& chain : ik_chains) {
            if (chain.mode == Resource::IkChain::Mode::FootPlant || !chain.contact_driven) continue;
            std::optional<std::array<float, 3>> target;
            if (have_transform && chain.enabled) {
                for (const auto& update : pending_ik_updates) {
                    if (update.geom_handle != geom_handle || update.configuration_revision != ik_revision) continue;
                    // A contact on any participating joint can drive this chain; leaf
                    // effectors often have no skin weights of their own.
                    bool belongs = false;
                    int node = chain.end_node;
                    for (int depth = 0; depth < std::min(chain.chain_length, static_cast<int>(skeleton.nodes.size())) &&
                         node >= 0 && node < static_cast<int>(skeleton.nodes.size()); ++depth) {
                        if (node == update.node_idx) { belongs = true; break; }
                        node = skeleton.nodes[static_cast<std::size_t>(node)].parent;
                    }
                    if (!belongs) continue;
                    const auto wp = vec3_add(update.contact_world, vec3_mul(update.normal, chain.contact_normal_offset));
                    target = std::array<float, 3>{
                        world_to_model[0][0]*wp.x + world_to_model[1][0]*wp.y + world_to_model[2][0]*wp.z + world_to_model[3][0],
                        world_to_model[0][1]*wp.x + world_to_model[1][1]*wp.y + world_to_model[2][1]*wp.z + world_to_model[3][1],
                        world_to_model[0][2]*wp.x + world_to_model[1][2]*wp.y + world_to_model[2][2]*wp.z + world_to_model[3][2]};
                }
            }
            advance_ik_activation(chain, target, dt);
        }
        { auto resource = hub.model_resource_storage().try_acquire_read(job.resource);
          if (!resource || resource->model_id != job.model) continue; }
        {
            auto geometry = geom_storage.try_acquire_write(geom_handle);
            if (!geometry || !still_current(*geometry, job) ||
                !commit_ik_runtime(geometry->ik_chains, geometry->ik_chains_revision, ik_chains, ik_revision))
                continue; // Model/targets/IK configuration changed while probing; retry next frame.
        }
        for (const auto& chain : ik_chains) {
            const auto solver_chain = ik_solver_parameters(chain);
            if (!solver_chain) continue;
            std::unordered_map<int, std::array<float, 16>> overrides;
            {
                GpuPerformance::Scope stage{"animation.ccd"};
                GpuPerformance::record_work_items(1);
                Resource::solve_ccd(skeleton, *solver_chain, working_locals, overrides);
            }
            for (const auto& [node, local] : overrides) working_locals[static_cast<std::size_t>(node)] = local;
        }
        {
            GpuPerformance::Scope stage{"animation.fk"};
            GpuPerformance::record_work_items(skeleton.nodes.size());
            Resource::compute_pose(skeleton, clip, job.anim_time, job.finals, nullptr, &working_locals);
        }
        job.pose_ready = true;
    }
    // A concurrent IK edit rejects this pose, including its skinning requests.
    // Destroy the pointer views before erasing/moving their owning jobs.
    foot_poses.clear();
    std::erase_if(frame_jobs, [](const auto& job) { return !job.pose_ready; });
    for (const auto& job : frame_jobs)
        for (const auto& request : job.requests) active_gpu_keys.push_back(request.key);

    // Release the per-model Scene read handles before any GPU submission.
    // The collision topology built below is the only later consumer of Scene data
    // and it goes through the self-locking overload, so no Scene read lock is held
    // across skinning/collision waits.  Holding them across a stalled GPU wait
    // blocked GeometrySystem's periodic *exclusive* RAM residency pass forever,
    // which froze the whole geometry pipeline (imports included) until restart.
    frame_scenes.clear();

    // Spans are created only after frame_jobs stops growing. All palettes and
    // source scene locks survive both GPU stages; no geometry storage lock is
    // held while recording, submitting or waiting.
    std::vector<GpuSkinningPipeline::Batch> skin_batches;
    skin_batches.reserve(frame_jobs.size());
    for (const auto& job : frame_jobs) skin_batches.push_back({job.requests, job.finals});
    std::vector<GpuSkinningPipeline::Result> skin_results;
    if (!skin_batches.empty()) {
        Corona::Diagnostics::PhaseScope skin_phase{"Mechanics", "skinned:gpu_skinning"};
        impl_->gpu_skinning.ensure();
        skin_phase.set("skinned:gpu_skinning_submit");
        skin_results = impl_->gpu_skinning->update_batch(skin_batches);
    }
    Corona::Diagnostics::mark_thread_phase("Mechanics", "skinned:publish_and_collision");
    std::size_t result_offset = 0;
    for (auto& job : frame_jobs) {
        Corona::Diagnostics::PhaseScope job_phase{"Mechanics", "skinned:collision_refresh"};
        std::unordered_map<std::size_t, GpuSkinningPipeline::Result> lod0_results;
        job.blobs.resize(job.mesh_count);
        for (std::size_t i = 0; i < job.requests.size(); ++i) {
            auto& result = skin_results.at(result_offset++);
            const auto mesh = job.lod0_meshes.find(result.key);
            if (mesh == job.lod0_meshes.end()) continue;
            job.blobs[mesh->second] = std::move(result.vertices);
            lod0_results.emplace(mesh->second, std::move(result));
        }
        { auto geometry = geom_storage.try_acquire_read(job.geometry);
          if (!geometry || !still_current(*geometry, job)) continue; }
        { auto resource = hub.model_resource_storage().try_acquire_read(job.resource);
          if (!resource || resource->model_id != job.model) continue; }
        const auto geom_handle = job.geometry;
        const auto model_id = job.model;
        job.bounds_min = make_fvec3(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                                    std::numeric_limits<float>::max());
        job.bounds_max = make_fvec3(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                                    std::numeric_limits<float>::lowest());

        // Reuse the immutable segmented topology, including skipped and duplicated
        // mesh instances. Each source applies the same instance matrix as rendering.
        // Scene read lock is taken (and released) inside the call, not across the
        // GPU stages below.
        ensure_collision_mesh(model_id, impl_->collision_mesh_cache,
                              &impl_->static_triangle_index_cache, &impl_->static_triangle_bone_cache);
        auto bind = impl_->collision_mesh_cache.find(model_id);
        if (bind != impl_->collision_mesh_cache.end()) {
            auto& sc = impl_->skinned_collision_cache[geom_handle];
            if (impl_->skinned_collision_model_ids[geom_handle] != model_id || sc.triangles.empty()) {
                sc = bind->second;
                impl_->skinned_collision_model_ids[geom_handle] = model_id;
                impl_->skinned_octree_cache.erase(geom_handle);
            }
            auto& tree = impl_->skinned_octree_cache[geom_handle];
            if (tree.empty()) tree.build(sc.vertices, sc);
            impl_->gpu_collision.ensure();
            impl_->gpu_collision->set_mesh(geom_handle, model_id, sc, tree);
            const auto include_point = [&](const ktm::fvec3& p) {
                job.bounds_min.x = std::min(job.bounds_min.x, p.x);
                job.bounds_min.y = std::min(job.bounds_min.y, p.y);
                job.bounds_min.z = std::min(job.bounds_min.z, p.z);
                job.bounds_max.x = std::max(job.bounds_max.x, p.x);
                job.bounds_max.y = std::max(job.bounds_max.y, p.y);
                job.bounds_max.z = std::max(job.bounds_max.z, p.z);
                job.bounds_valid = true;
            };
            for (const auto& segment : sc.submeshes) {
                const auto output = lod0_results.find(segment.source_mesh_index);
                if (output == lod0_results.end()) {
                    include_point(segment.bind_min);
                    include_point(segment.bind_max);
                    continue;
                }
                job.collision_sources.push_back({output->second.output, segment.vertex_base,
                                                segment.vertex_count, segment.source_transform});
                const auto matrix = make_fmat4x4_colmajor(segment.source_transform);
                // Transform the GPU-reduced source box, never scan skinned CPU positions.
                for (int corner = 0; corner < 8; ++corner) {
                    const auto p = matrix * make_fvec4(
                        (corner & 1) ? output->second.max_xyz[0] : output->second.min_xyz[0],
                        (corner & 2) ? output->second.max_xyz[1] : output->second.min_xyz[1],
                        (corner & 4) ? output->second.max_xyz[2] : output->second.min_xyz[2], 1);
                    include_point(make_fvec3(p.x, p.y, p.z));
                }
            }
            job.collision_ready = true;
        }
    }
    std::vector<GpuCollisionPipeline::VertexUpdate> collision_updates;
    for (const auto& job : frame_jobs)
        if (job.collision_ready) collision_updates.push_back({job.geometry, job.collision_sources, dt});
    if (!collision_updates.empty()) {
        Corona::Diagnostics::PhaseScope collision_phase{"Mechanics", "skinned:gpu_collision"};
        impl_->gpu_collision->update_vertices(collision_updates);
    }

    // Publish only after both batches succeed. Revalidate identity because scene
    // collection and execution can overlap editor/model changes.
    for (auto& job : frame_jobs) {
        { auto resource = hub.model_resource_storage().try_acquire_read(job.resource);
          if (!resource || resource->model_id != job.model) continue; }
        {
            auto geometry = geom_storage.try_acquire_write(job.geometry);
            if (geometry && still_current(*geometry, job)) {
                geometry->skinned_cpu_vertices = vision_readback
                    ? std::move(job.blobs) : std::vector<std::vector<std::byte>>{};
                geometry->skinned_aabb_valid = job.bounds_valid;
                if (job.bounds_valid) {
                    geometry->skinned_aabb_min = job.bounds_min;
                    geometry->skinned_aabb_max = job.bounds_max;
                }
            }
        }
    }

    // Prepare one new resident mesh per animation update, including Phantom and
    // paused scenes. Never wait for background compilation just to do optional
    // preparation. Active physics still waits for every required pipeline.
    Corona::Diagnostics::mark_thread_phase("Mechanics", "skinned:collision_prepare");
    std::unordered_set<std::uint64_t> live_models;
    std::vector<std::uint64_t> collision_keys;
    bool prepared_mesh = false;
    const auto start = impl_->collision_preparation_cursor % std::max<std::size_t>(geom_handles.size(), 1);
    for (std::size_t offset = 0; offset < geom_handles.size(); ++offset) {
        const auto index = (start + offset) % geom_handles.size();
        const auto key = geom_handles[index];
        std::uintptr_t resource_handle{}, transform_handle{};
        {
            auto geometry = geom_storage.try_acquire_read(key);
            if (!geometry || geometry->gpu_build_state != GeometryDevice::GpuBuildState::Ready ||
                geometry->mesh_handles.empty()) continue;
            resource_handle = geometry->model_resource_handle;
            transform_handle = geometry->transform_handle;
        }
        std::uint64_t model_id{};
        if (auto resource = hub.model_resource_storage().try_acquire_read(resource_handle))
            model_id = resource->model_id;
        if (!model_id) continue;
        live_models.insert(model_id);
        // A replaced model must not retain a previous model's GPU geometry.
        if (impl_->gpu_collision && impl_->gpu_collision->contains(key, model_id)) {
            collision_keys.push_back(key);
            continue;
        }
        if (prepared_mesh || !impl_->gpu_collision.ready()) continue;
        ModelTransform transform;
        {
            auto current = hub.model_transform_storage().try_acquire_read(transform_handle);
            if (!current) continue;
            transform = *current;
        }
        // Count a topology build against this frame's budget even if it yields
        // no triangles.  The Scene read lock is acquired inside the call: the
        // per-frame Scene handles were released before the GPU stages above.
        auto mesh = impl_->collision_mesh_cache.find(model_id);
        if (mesh == impl_->collision_mesh_cache.end()) {
            prepared_mesh = true;
            impl_->collision_preparation_cursor = index + 1;
            ensure_collision_mesh(model_id, impl_->collision_mesh_cache,
                                  &impl_->static_triangle_index_cache, &impl_->static_triangle_bone_cache);
            mesh = impl_->collision_mesh_cache.find(model_id);
        }
        // Animated geometry must come from the completed skinning batch above;
        // never substitute bind-pose vertices for missing deformation data.
        if (mesh == impl_->collision_mesh_cache.end() || mesh->second.requires_skinning ||
            mesh->second.triangles.empty()) continue;
        prepared_mesh = true;
        impl_->collision_preparation_cursor = index + 1;
        auto& tree = impl_->triangle_octree_cache[model_id];
        if (tree.empty()) tree.build(mesh->second.vertices, mesh->second);
        impl_->gpu_collision.ensure();
        impl_->gpu_collision->set_mesh(key, model_id, mesh->second, tree);
        const GpuCollisionPipeline::Instance instance{key, gpu_matrix(transform.compute_matrix())};
        impl_->gpu_collision->prepare_instances(std::span(&instance, 1));
        collision_keys.push_back(key);
    }
    if (impl_->gpu_collision) impl_->gpu_collision->retain(collision_keys);
    // Keep immutable topology for all resident models, including Phantom bodies
    // alongside an active simulation. Cleanup must also run while fully paused.
    std::erase_if(impl_->triangle_octree_cache, [&](const auto& item) { return !live_models.contains(item.first); });
    std::erase_if(impl_->collision_mesh_cache, [&](const auto& item) { return !live_models.contains(item.first); });
    std::erase_if(impl_->static_triangle_index_cache, [&](const auto& item) { return !live_models.contains(item.first); });
    std::erase_if(impl_->static_triangle_bone_cache, [&](const auto& item) { return !live_models.contains(item.first); });
    // Cleanup also runs while physics is paused; residency is an animation concern.
    if (impl_->gpu_skinning) {
        impl_->gpu_skinning->retain(active_gpu_keys);
        const std::unordered_set<std::uint64_t> active(active_gpu_keys.begin(), active_gpu_keys.end());
        std::erase_if(impl_->gpu_skinning_keys, [&](const auto& item) { return !active.contains(item.second); });
    }
}

// ============================================================================
// register_foot_plant_chain / unregister_foot_plant_chain
// ============================================================================

namespace {

// 内部辅助：向 GeometryDevice 写入或更新一条 FootPlant 链。调用方必须已持有写锁。
bool apply_foot_plant_chain(Corona::GeometryDevice& gw, int node_idx, std::string_view bone_name,
                            int chain_length, float damping, float weight_decay) {
    using namespace Corona::Resource;
    for (auto& chain : gw.ik_chains) {
        if ((chain.end_bone_name == bone_name || (chain.end_bone_name.empty() && chain.end_node == node_idx)) &&
            chain.mode == IkChain::Mode::FootPlant) {
            if (chain.end_node != node_idx) chain.runtime = {};
            chain.end_node             = node_idx;
            chain.chain_length         = chain_length;
            chain.damping              = damping;
            chain.contact_weight_decay = weight_decay;
            chain.end_bone_name         = bone_name;
            ++gw.ik_chains_revision;
            return true;
        }
    }
    IkChain chain;
    chain.id                    = "native-foot:" + std::string(bone_name);
    chain.end_bone_name          = bone_name;
    chain.end_node              = node_idx;
    chain.chain_length          = chain_length;
    chain.mode                  = IkChain::Mode::FootPlant;
    chain.contact_driven        = false;
    chain.enabled               = true;
    chain.weight                = 1.0f;
    chain.damping               = damping;
    chain.max_iterations        = 10;
    chain.tolerance             = 1e-3f;
    chain.contact_normal_offset = 0.0f;  // 目标贴地面，不偏移
    chain.contact_weight_rise   = 8.0f;  // 约 0.13s（8 帧）升满，落地立即贴地
    chain.contact_weight_decay  = weight_decay;
    gw.ik_chains.push_back(chain);
    ++gw.ik_chains_revision;
    return true;
}

}  // namespace

bool MechanicsSystem::register_foot_plant_chain(
    std::uintptr_t geom_handle,
    std::string_view bone_name,
    int chain_length,
    float damping,
    float weight_decay) {

    if (bone_name.empty() || chain_length < 2 || chain_length > 64 ||
        !std::isfinite(damping) || damping < 0 || damping > 1 ||
        !std::isfinite(weight_decay) || weight_decay < 0 || weight_decay > 100) return false;
    auto& hub     = SharedDataHub::instance();
    auto& geom_st = hub.geometry_storage();

    // Resolve against the current model, without attempting a read-to-write lock upgrade.
    std::uintptr_t mrh = 0;
    {
        auto gr = geom_st.try_acquire_read(geom_handle);
        if (!gr) return false;
        mrh = gr->model_resource_handle;
    }
    if (!mrh) return false;

    std::uint64_t model_id = 0;
    {
        auto mr = hub.model_resource_storage().try_acquire_read(mrh);
        if (!mr) return false;
        model_id = mr->model_id;
    }
    if (!model_id) return false;

    auto scene_read = Resource::ResourceManager::get_instance().acquire_read<Resource::Scene>(model_id);
    if (!scene_read.valid()) return false;
    const Resource::Scene& scene = *scene_read;
    if (!scene.data.skeleton.has_value()) return false;

    const Resource::SkeletonData& skel = *scene.data.skeleton;
    int node_idx = -1;
    for (std::size_t ni = 0; ni < skel.nodes.size(); ++ni) {
        if (skel.nodes[ni].name == bone_name) {
            if (node_idx >= 0) return false; // ambiguous name
            node_idx = static_cast<int>(ni);
        }
    }
    if (!Resource::is_valid_foot_ik_end_node(skel, node_idx)) return false;

    auto gw = geom_st.try_acquire_write(geom_handle);
    if (!gw || gw->model_resource_handle != mrh) return false;
    return apply_foot_plant_chain(*gw, node_idx, bone_name, chain_length, damping, weight_decay);
}

void MechanicsSystem::unregister_foot_plant_chain(
    std::uintptr_t geom_handle,
    std::string_view bone_name) {

    auto& geom_st = SharedDataHub::instance().geometry_storage();
    auto gw = geom_st.try_acquire_write(geom_handle);
    if (!gw) return;

    auto cache_it = gw->bone_name_to_node_idx.find(std::string(bone_name));
    const int node_idx = cache_it == gw->bone_name_to_node_idx.end() ? -1 : cache_it->second;

    auto& chains = gw->ik_chains;
    const auto previous_size = chains.size();
    chains.erase(
        std::remove_if(chains.begin(), chains.end(),
            [node_idx, bone_name](const Resource::IkChain& c) {
                return (c.end_bone_name == bone_name || (c.end_bone_name.empty() && node_idx >= 0 && c.end_node == node_idx)) &&
                       c.mode == Resource::IkChain::Mode::FootPlant;
            }),
        chains.end());
    if (chains.size() != previous_size) ++gw->ik_chains_revision;
}

}  // namespace Corona::Systems




