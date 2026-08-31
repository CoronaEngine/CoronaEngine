#include <corona/events/engine_events.h>
#include <corona/events/mechanics_system_events.h>
#include <corona/kernel/core/i_logger.h>
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
#include <span>           // std::span（蒙皮 write_bytes）
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

#ifndef CORONA_MECHANICS_USE_OBB_SAT
#define CORONA_MECHANICS_USE_OBB_SAT 1
#endif

#ifndef CORONA_MECHANICS_USE_TRIANGLE_NARROWPHASE
#define CORONA_MECHANICS_USE_TRIANGLE_NARROWPHASE 1
#endif

#include "mechanics_internal.h"

namespace Corona::Systems {

using namespace MechanicsInternal;

namespace {

// ----------------------------------------------------------------------------
// CPU 蒙皮（P2，自 GeometrySystem 迁入）
// ----------------------------------------------------------------------------
// 对单个 mesh 的绑定姿态顶点做线性混合蒙皮（LBS），输出标准 Resource::Vertex。
//   skinned.pos = Σ wᵢ · (final[idᵢ] · bind.pos)
//   skinned.nrm = normalize(Σ wᵢ · (mat3(final[idᵢ]) · bind.nrm))
// final[] 为 compute_pose 的输出（列主序 mat4，下标 col*4+row）。
// bind 顶点已在导入期保留绑定姿态空间（未烘世界变换、未单位化），
// 故蒙皮结果在模型空间，运行时由 model transform (o2w) 放到世界空间。
// uv 原样保留。无骨骼影响（ids 全 -1）的顶点回退为原始绑定位置。
inline Corona::Resource::Vertex skin_one_vertex(
    const Corona::Resource::Vertex& bind,
    const Corona::Resource::BoneWeights& bw,
    const std::vector<std::array<float, 16>>& finals) {
    // 累加权重>0 的骨骼贡献
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    float nx = 0.0f, ny = 0.0f, nz = 0.0f;
    float total_w = 0.0f;

    for (int i = 0; i < Corona::Resource::MAX_BONE_INFLUENCE; ++i) {
        const std::int32_t id = bw.ids[i];
        const float w = bw.weights[i];
        if (id < 0 || w <= 0.0f) continue;
        if (id >= static_cast<std::int32_t>(finals.size())) continue;

        const std::array<float, 16>& m = finals[static_cast<std::size_t>(id)];
        const float bx = bind.position[0], by = bind.position[1], bz = bind.position[2];
        // 位置：齐次点变换（含平移，第 3 列）—— 列主序 m[col*4+row]
        px += w * (m[0] * bx + m[4] * by + m[8] * bz + m[12]);
        py += w * (m[1] * bx + m[5] * by + m[9] * bz + m[13]);
        pz += w * (m[2] * bx + m[6] * by + m[10] * bz + m[14]);
        // 法线：仅 3x3 线性部分（不含平移）
        const float bnx = bind.normal[0], bny = bind.normal[1], bnz = bind.normal[2];
        nx += w * (m[0] * bnx + m[4] * bny + m[8] * bnz);
        ny += w * (m[1] * bnx + m[5] * bny + m[9] * bnz);
        nz += w * (m[2] * bnx + m[6] * bny + m[10] * bnz);
        total_w += w;
    }

    Corona::Resource::Vertex out = bind;  // 复制 uv；无影响时保留绑定位置/法线
    if (total_w > 0.0f) {
        out.position = {px, py, pz};
        // 归一化法线（避免缩放骨骼导致非单位法线 → shader 错误）
        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-8f) {
            float inv = 1.0f / len;
            out.normal = {nx * inv, ny * inv, nz * inv};
        }
    }
    return out;
}

}  // namespace

void MechanicsSystem::update_physics(float fixed_dt) {
    // 如果正在关闭，不再处理新的物理更新
    if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
        return;
    }

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
    const float sleep_threshold_sq = sleep_threshold * sleep_threshold;  // 休眠速度阈值平方
    const float sleep_time_needed = 0.4f;                                // 静止多久后休眠
    const float min_inertia = 0.0001f;                                   // 最小转动惯量，防止除零
    const float rot_damping_factor = 0.97f;                              // 基础旋转阻尼系数
    const float max_linear_speed = 80.0f;                                // 线速度上限 m/s，防碰飞
    const float max_angular_speed = 30.0f;                               // 角速度上限 rad/s
    const float max_impulse_per_contact = 500.0f;                        // 单次接触最大冲量 N·s
    const float max_position_correction = 0.5f;                          // 单帧最大位置修正距离 m

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
    std::vector<std::uintptr_t> scene_handles;  // 参与遍历的 scene 指针键，用于写 scene AABB
    scene_handles.reserve(4);

    // --- 阶段 1：遍历场景 → 读环境(gravity/floor/dt) → 展开 Actor/Profile → 收集 mechanics_handle ---
    for (const auto& scene : scene_storage) {
        if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
            return;
        }
        if (!scene.enabled)
            continue;

        scene_handles.push_back(reinterpret_cast<std::uintptr_t>(&scene));

        if (!scene.simulation_enabled)
            continue;

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
            // TODO: 后续实现 offline physics proxy —— Unloaded + physics_enabled
            //       的 actor 用简化 AABB 碰撞体继续参与物理
            {
                std::shared_lock lock(impl_->residency_mtx_);
                if (!impl_->resident_actors_.count(actor_handle)) continue;
            }
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
                                if (!m_acc->physics_enabled) continue;  // 物理已禁用，跳过本 mechanics
                                params.mass = m_acc->mass;
                                params.damping = m_acc->damping;
                                params.restitution = m_acc->restitution;
                                params.collision_shape = m_acc->collision_shape;
                                params.collision_enabled = m_acc->collision_shape != CollisionShape::None;
                                new_linear = m_acc->linear_lock_mask;
                                new_angular = m_acc->angular_lock_mask;
                                body.linear_lock = new_linear;    // 缓存线性轴锁
                                body.angular_lock = new_angular;  // 缓存角度轴锁
                            } else {
                                params = BodyFrameParams{};
                                params.actor = actor_handle;
                                body.linear_lock = 0;   // 读失败时默认不锁任何轴
                                body.angular_lock = 0;
                            }

                            mechanics_handles.push_back(h);

                            // 轴锁解除时唤醒休眠体：若锁定位从 1→0（解锁），休眠体需恢复物理响应
                            if (((old_linear & ~new_linear) != 0 || (old_angular & ~new_angular) != 0) && body.sleeping) {
                                body.sleeping = false;
                                body.sleep_timer = 0.0f;
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

    // --- 阶段 2：半隐式前推速度（仅非休眠体）：先阻尼旧速度，再叠加重力加速度 ---
    for (std::uintptr_t h : mechanics_handles) {  // 对存活列表逐个施力
        if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
            return;
        }
        if (impl_->body(h).sleeping) continue;    // 休眠体本阶段不改速度

        float damping = frame_params[h].damping;   // 线性阻尼乘子（以 60Hz 为基准的每步保留系数）
        auto& av = impl_->body(h).angular_velocity;  // 可修改的角速度引用

        // 1. 先对上一帧遗留的速度施加阻尼（指数衰减，与 dt 无关）
        const float effective_damping = std::pow(damping, fixed_dt * 60.0f);
        impl_->body(h).velocity.x *= effective_damping;
        impl_->body(h).velocity.y *= effective_damping;
        impl_->body(h).velocity.z *= effective_damping;

        // 2. 再叠加本帧重力加速度（不被阻尼衰减）
        impl_->body(h).velocity.x += gravity.x * fixed_dt;
        impl_->body(h).velocity.y += gravity.y * fixed_dt;
        impl_->body(h).velocity.z += gravity.z * fixed_dt;

        // 3. 角速度阻尼（指数衰减）
        const float effective_rot_damping = std::pow(
            std::max(damping * rot_damping_factor, 0.9f), fixed_dt * 60.0f);
        av.x *= effective_rot_damping;
        av.y *= effective_rot_damping;
        av.z *= effective_rot_damping;

        // 4. 速度钳制：防止重力累积或异常冲量导致物体飞走
        auto clamp_speed = [&](ktm::fvec3& v, float max_spd) {
            float spd_sq = v.x * v.x + v.y * v.y + v.z * v.z;
            if (spd_sq > max_spd * max_spd) {
                float inv = max_spd / std::sqrt(spd_sq);
                v.x *= inv;
                v.y *= inv;
                v.z *= inv;
            }
        };
        clamp_speed(impl_->body(h).velocity, max_linear_speed);
        clamp_speed(impl_->body(h).angular_velocity, max_angular_speed);
    }

    // --- 阶段 2b：轴锁定强制执行 — 将已锁轴的速度/角速度分量清零 ---
    for (std::uintptr_t h : mechanics_handles) {
        if (impl_->body(h).sleeping) continue;
        uint8_t lin_lock = impl_->body(h).linear_lock;
        if (lin_lock & kLockAxisX) impl_->body(h).velocity.x = 0.0f;
        if (lin_lock & kLockAxisY) impl_->body(h).velocity.y = 0.0f;
        if (lin_lock & kLockAxisZ) impl_->body(h).velocity.z = 0.0f;

        uint8_t ang_lock = impl_->body(h).angular_lock;
        if (ang_lock & kLockAxisX) impl_->body(h).angular_velocity.x = 0.0f;
        if (ang_lock & kLockAxisY) impl_->body(h).angular_velocity.y = 0.0f;
        if (ang_lock & kLockAxisZ) impl_->body(h).angular_velocity.z = 0.0f;
    }

    // --- 阶段 3：为每个 mechanics 读几何/变换 → 首遇则建四元数朝向 → 预测位姿 → 世界 AABB + 长方体对角惯量（世界系冲量用）---
    std::vector<MechanicsWorldAABB> mechanics_data;
    mechanics_data.reserve(mechanics_handles.size());
    std::unordered_map<std::uintptr_t, std::size_t> handle_to_index;

    for (std::uintptr_t h : mechanics_handles) {         // 为每个力学体准备碰撞与惯量数据
        if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
            return;
        }
        auto m_acc = mechanics_storage.try_acquire_read(h);  // mechanics 组件读锁
        if (!m_acc) continue;                            // 无数据则跳过
        const auto& m = *m_acc;                          // 其 min/max、geometry_handle

        auto geom_acc = geometry_storage.try_acquire_read(m.geometry_handle);
        if (!geom_acc) continue;

        auto tx_acc = transform_storage.try_acquire_read(geom_acc->transform_handle);
        if (!tx_acc) continue;
        const auto& t = *tx_acc;  // 只读当前变换（复制后做预测）

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
        }
        // 无记录则从当前欧拉初始化四元数
        auto& body = impl_->body(h);
        if (!body.orientation_initialized) {
            body.orientation_quat = quat_from_model_euler(t.euler_rotation);
            body.orientation_initialized = true;
        }
        ktm::fquat q_pred = body.orientation_quat;  // 复制：预测用，不提前改全局缓存
        Corona::ModelTransform t_collision = t;            // 复制当前变换
        if (!impl_->body(h).sleeping) {
            // 为碰撞预测外推位姿时也遵守轴锁定
            ktm::fvec3 vc = impl_->body(h).velocity;
            ktm::fvec3 ang_pred = impl_->body(h).angular_velocity;
            uint8_t lin_lock = impl_->body(h).linear_lock;
            uint8_t ang_lock = impl_->body(h).angular_lock;
            if (lin_lock & kLockAxisX) vc.x = 0.0f;
            if (lin_lock & kLockAxisY) vc.y = 0.0f;
            if (lin_lock & kLockAxisZ) vc.z = 0.0f;
            if (ang_lock & kLockAxisX) ang_pred.x = 0.0f;
            if (ang_lock & kLockAxisY) ang_pred.y = 0.0f;
            if (ang_lock & kLockAxisZ) ang_pred.z = 0.0f;

            t_collision.position.x += vc.x * fixed_dt;       // 外推平移用于碰撞检测
            t_collision.position.y += vc.y * fixed_dt;
            t_collision.position.z += vc.z * fixed_dt;
            integrate_orientation_quat(q_pred, ang_pred, fixed_dt);  // 外推旋转
        }
        sync_euler_from_orientation_quat(q_pred, t_collision.euler_rotation);  // 矩阵一致化 euler
        world_aabb_from_local_bounds(t_collision, entry.local_min, entry.local_max,
                                     entry.min_world, entry.max_world, entry.center_world);
        entry.half_extents = make_fvec3(
            (entry.max_world.x - entry.min_world.x) * 0.5f,  // 世界 AABB 半宽
            (entry.max_world.y - entry.min_world.y) * 0.5f,
            (entry.max_world.z - entry.min_world.z) * 0.5f);
#if CORONA_MECHANICS_USE_OBB_SAT
        build_mechanics_obb(entry, t_collision);  // 由同一预测位姿构造 OBB
#endif

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
        entry.rot_body_to_world = q_pred.matrix3x3();                          // 体→世界旋转（预测姿态）
        entry.inertia_inv_body = make_fvec3(1.0f / Ix, 1.0f / Iy, 1.0f / Iz);  // 体系逆惯量对角

        handle_to_index[h] = mechanics_data.size();  // 句柄→本轮 mechanics_data 下标
        mechanics_data.push_back(entry);
    }

    // 预加载所有物理物体的碰撞网格（用于三角形碰撞检测和精确地板碰撞）
    for (const auto& entry : mechanics_data) {
        if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
            return;
        }
        // 蒙皮物体跳过三角网格：其网格随动画每帧变形，静态绑定姿态三角网格已失真，
        // 且窄相一旦双方都有网格就走三角 SAT，会绕开我们要的动态 AABB 碰撞（P4）。
        if (entry.is_skinned) continue;
        const auto params_it = frame_params.find(entry.handle);
        if (params_it != frame_params.end() &&
            params_it->second.collision_shape == CollisionShape::Mesh &&
            entry.model_id != 0) {
            ensure_collision_mesh(entry.model_id, impl_->collision_mesh_cache);
        }
    }

    // 临时校正表：记录 Phase 5 末轮的位置校正量，在 Phase 6 积分后统一应用
    std::unordered_map<std::uintptr_t, ktm::fvec3> position_correction;

    // 阶段 5：从 GeometrySystem 获取宽相候选对 → 窄相（AABB 或 OBB+SAT）→ 顺序冲量 + 摩擦 + 末轮位置校正 ---
    //GeometrySystem 八叉树 payload 是 actor_handle，query_pairs() 返回 (actor_a, actor_b)
    //一个 actor 可能挂多个含 mechanics 的 profile，故用 vector 存储所有 mechanics_handle
    //转换时展开笛卡尔积；遍历 actor_a 的每个 mechanics vs actor_b 的每个 mechanics

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

    // 通过 ISystemContext 获取 GeometrySystem 指针，调用其八叉树的 query_pairs()
    // 宽相阶段由 GeometrySystem 维护的八叉树统一服务，MechanicsSystem 不再自建本地 octree。
    // GeometrySystem(85) 优先级高于 MechanicsSystem(75)，八叉树在同帧物理前已重建。
    // 指针在 initialize() 中缓存，避免每帧通过 get_system() 加锁查询。
    if (impl_->geometry_sys) {
        for (auto sh : scene_handles) {
            if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
                return;
            }
            auto actor_pairs = impl_->geometry_sys->query_pairs(sh);
            for (const auto& [ah, bh] : actor_pairs) {
                if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
                    return;
                }
                auto it_a = actor_to_mech.find(ah);
                auto it_b = actor_to_mech.find(bh);
                if (it_a == actor_to_mech.end() || it_b == actor_to_mech.end()) continue;

                for (auto mh_a : it_a->second) {
                    for (auto mh_b : it_b->second) {
                        collision_pairs.emplace_back(mh_a, mh_b);
                    }
                }
            }
        }
    }

    if (mechanics_data.size() >= 2) {
        // 碰撞对跟踪（用于回调通知 collision start/end）
        std::unordered_set<std::pair<std::uintptr_t, std::uintptr_t>, PairHash> curr_active_collisions;

        // 惰性缓存：仅对候选对涉及的物体计算世界空间碰撞网格
        std::unordered_map<std::uintptr_t, std::vector<ktm::fvec3>> world_verts_cache;

        // 5.4 对候选对做法向/切向冲量（半隐式 GS：多轮依次解每对约束近似同时满足）
        constexpr float eps = 1e-8f;                   // 分母稳定项，非物理
        constexpr float min_overlap = 0.001f;          // 小于此视为数值噪声/SAT 抖振，跳过
        constexpr float k_positional_slop = 0.004f;    // Baumgarte 式校正：小穿透只靠冲量，不修位姿
        constexpr float k_positional_percent = 0.35f;  // 仅末轮按穿透拆分平移，且只推一部分，防过冲
        constexpr int k_impulse_iterations = 8;        // 轮数↑ 堆叠更稳；提升到8以改善坡面接触收敛
        constexpr float k_early_exit_vel_eps = 0.002f; // 本轮最大速度修正低于此值则提前退出
        for (int impulse_iter = 0; impulse_iter < k_impulse_iterations; ++impulse_iter) {
            float max_delta_v_sq_this_iter = 0.0f;  // 本轮最大速度修正平方，用于收敛检测
            if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
                return;
            }
            for (const auto& pair : collision_pairs) {  // 内层：单对接触解一次（顺序依赖）
                if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
                    return;
                }
                std::uintptr_t ha = pair.first;
                std::uintptr_t hb = pair.second;
                // 两个都休眠则跳过
                if (impl_->body(ha).sleeping && impl_->body(hb).sleeping)
                    continue;

                // 查找物体A/B的AABB数据
                auto it_a = handle_to_index.find(ha);
                auto it_b = handle_to_index.find(hb);
                if (it_a == handle_to_index.end() || it_b == handle_to_index.end()) {
                    continue;
                }

                const MechanicsWorldAABB& a = mechanics_data[it_a->second];
                const MechanicsWorldAABB& b = mechanics_data[it_b->second];

                // 碰撞检测开关判断：任一物体关闭碰撞则跳过此对
                // 使用 find() 而非 operator[] 避免默认构造 false 导致碰撞被静默跳过
                {
                    bool col_a = true, col_b = true;
                    auto it_col_a = frame_params.find(ha);
                    if (it_col_a != frame_params.end()) col_a = it_col_a->second.collision_enabled;
                    auto it_col_b = frame_params.find(hb);
                    if (it_col_b != frame_params.end()) col_b = it_col_b->second.collision_enabled;
                    if (!col_a || !col_b) continue;
                }

                // ===== Phase 1: AABB 碰撞检测（Broadphase 确认）=====
                if (!aabb_overlap(a.min_world, a.max_world, b.min_world, b.max_world)) {
                    continue;
                }

                ktm::fvec3 normal{};
                float penetration = 0.f;
#if CORONA_MECHANICS_USE_OBB_SAT
                if (!sat_obb_obb(a.obb_center, a.obb_u, a.obb_v, a.obb_w, a.obb_hu, a.obb_hv, a.obb_hw,
                                 b.obb_center, b.obb_u, b.obb_v, b.obb_w, b.obb_hu, b.obb_hv, b.obb_hw,
                                 normal, penetration)) {
                    continue;
                }
                if (penetration < min_overlap) {
                    continue;
                }
#else
                // AABB–AABB：MTD 必沿世界轴；旋转体用世界 AABB 包络时斜面接触法线仍错，真斜碰请开 OBB+SAT。
                // 稳定：在「并列最浅穿透」的轴里优先 |Δcenter| 最大者，避免主轴每帧切换；符号用死区避免 0 附近翻转。
                const float diff_x = b.center_world.x - a.center_world.x;
                const float diff_y = b.center_world.y - a.center_world.y;
                const float diff_z = b.center_world.z - a.center_world.z;
                const float overlap_x =
                    (a.max_world.x - a.min_world.x) * 0.5f + (b.max_world.x - b.min_world.x) * 0.5f - std::abs(diff_x);
                const float overlap_y =
                    (a.max_world.y - a.min_world.y) * 0.5f + (b.max_world.y - b.min_world.y) * 0.5f - std::abs(diff_y);
                const float overlap_z =
                    (a.max_world.z - a.min_world.z) * 0.5f + (b.max_world.z - b.min_world.z) * 0.5f - std::abs(diff_z);
                if (overlap_x < min_overlap || overlap_y < min_overlap || overlap_z < min_overlap) {
                    continue;
                }
                const float mtd_min = std::min({overlap_x, overlap_y, overlap_z});
                constexpr float k_mtd_tie_abs = 0.0025f;
                constexpr float k_mtd_tie_rel = 0.04f;
                const float mtd_band = std::max(k_mtd_tie_abs, k_mtd_tie_rel * std::max(mtd_min, min_overlap));
                const float adx = std::abs(diff_x);
                const float ady = std::abs(diff_y);
                const float adz = std::abs(diff_z);
                int axis = 0;
                float best_dabs = -1.f;
                int best_stack_pri = 999;
                for (int i = 0; i < 3; ++i) {
                    const float ov = (i == 0) ? overlap_x : (i == 1) ? overlap_y
                                                                     : overlap_z;
                    const float ab = (i == 0) ? adx : (i == 1) ? ady
                                                               : adz;
                    if (ov > mtd_min + mtd_band) {
                        continue;
                    }
                    const int stack_pri = (i == 1) ? 0 : (i == 0) ? 1
                                                                  : 2;
                    if (ab > best_dabs + 1e-6f) {
                        axis = i;
                        best_dabs = ab;
                        best_stack_pri = stack_pri;
                    } else if (std::abs(ab - best_dabs) <= 1e-6f && stack_pri < best_stack_pri) {
                        axis = i;
                        best_stack_pri = stack_pri;
                    }
                }
                if (best_dabs < 0.f) {
                    if (overlap_y <= overlap_x && overlap_y <= overlap_z) {
                        axis = 1;
                    } else if (overlap_x <= overlap_z) {
                        axis = 0;
                    } else {
                        axis = 2;
                    }
                }
                constexpr float k_mtd_sign_eps = 1e-4f;
                auto mtd_axis_sign = [](float d) -> float {
                    if (d > k_mtd_sign_eps) {
                        return 1.f;
                    }
                    if (d < -k_mtd_sign_eps) {
                        return -1.f;
                    }
                    return 1.f;
                };
                if (axis == 0) {
                    penetration = overlap_x;
                    normal = make_fvec3(mtd_axis_sign(diff_x), 0.f, 0.f);
                } else if (axis == 1) {
                    penetration = overlap_y;
                    normal = make_fvec3(0.f, mtd_axis_sign(diff_y), 0.f);
                } else {
                    penetration = overlap_z;
                    normal = make_fvec3(0.f, 0.f, mtd_axis_sign(diff_z));
                }
#endif

                // ===== 三角形窄相精化（可选）=====
                // 当双方都有碰撞网格且三角形数在限制内时，用三角形级 SAT 替换 AABB/OBB 的法线和穿透
#if CORONA_MECHANICS_USE_TRIANGLE_NARROWPHASE
                const bool both_mesh =
                    frame_params.at(ha).collision_shape == CollisionShape::Mesh &&
                    frame_params.at(hb).collision_shape == CollisionShape::Mesh;
                if (both_mesh && a.model_id != 0 && b.model_id != 0) {
                    auto it_mesh_a = impl_->collision_mesh_cache.find(a.model_id);
                    auto it_mesh_b = impl_->collision_mesh_cache.find(b.model_id);
                    if (it_mesh_a != impl_->collision_mesh_cache.end() &&
                        it_mesh_b != impl_->collision_mesh_cache.end()) {
                        // 惰性计算世界空间顶点（每物体每帧最多算一次）
                        if (world_verts_cache.find(ha) == world_verts_cache.end()) {
                            auto tx_a = transform_storage.try_acquire_read(a.transform_handle);
                            if (tx_a) {
                                transform_vertices_to_world(
                                    it_mesh_a->second.vertices, *tx_a, world_verts_cache[ha]);
                            }
                        }
                        if (world_verts_cache.find(hb) == world_verts_cache.end()) {
                            auto tx_b = transform_storage.try_acquire_read(b.transform_handle);
                            if (tx_b) {
                                transform_vertices_to_world(
                                    it_mesh_b->second.vertices, *tx_b, world_verts_cache[hb]);
                            }
                        }

                        auto wit_a = world_verts_cache.find(ha);
                        auto wit_b = world_verts_cache.find(hb);
                        if (wit_a != world_verts_cache.end() && wit_b != world_verts_cache.end() && !wit_a->second.empty() && !wit_b->second.empty()) {
                            auto& wv_a = wit_a->second;
                            auto& wv_b = wit_b->second;
                            TriangleContactResult tri_result;
                            triangle_narrowphase(wv_a, it_mesh_a->second,
                                                 wv_b, it_mesh_b->second,
                                                 a.center_world, b.center_world, tri_result);
                            if (tri_result.has_contact) {
                                normal = tri_result.normal;
                                // 保留 AABB/OBB 级别的穿透深度；三角形 SAT 深度只反映
                                // 单个三角形对的局部重叠，远小于物体级实际穿透，会导致严重穿模。
                                // 三角形窄相的价值在于提供更精确的碰撞法线方向。
                            } else {
                                continue;  // 三角形级无接触，跳过此对
                            }
                        }
                    }
                }
#endif

                const float mass_a = frame_params[ha].mass;
                const float mass_b = frame_params[hb].mass;
                const bool sleep_a = impl_->body(ha).sleeping;  // 休眠体当「动不了」：不接收冲量速度增量
                const bool sleep_b = impl_->body(hb).sleeping;
                const float inv_ma = sleep_a ? 0.f : 1.0f / mass_a;  // Δv = (j/m)·n 中的 1/m
                const float inv_mb = sleep_b ? 0.f : 1.0f / mass_b;
                const float rest_a = frame_params[ha].restitution;  // 双方恢复系数各取组件；此处简单平均
                const float rest_b = frame_params[hb].restitution;
                const float rest = (rest_a + rest_b) * 0.5f;
                // 前几轮 e=0：先把接触簇里的相对法向「扎进」速度吃掉；末轮再加 e，减轻来回弹
                const float rest_use = (impulse_iter == k_impulse_iterations - 1) ? rest : 0.f;

#if CORONA_MECHANICS_USE_OBB_SAT
                const ktm::fvec3 p_a = obb_support_point(a.obb_center, a.obb_u, a.obb_v, a.obb_w,
                                                         a.obb_hu, a.obb_hv, a.obb_hw, normal);
                const ktm::fvec3 p_b = obb_support_point(b.obb_center, b.obb_u, b.obb_v, b.obb_w,
                                                         b.obb_hu, b.obb_hv, b.obb_hw,
                                                         make_fvec3(-normal.x, -normal.y, -normal.z));
                const ktm::fvec3 p_contact = vec3_mul(vec3_add(p_a, p_b), 0.5f);  // 近似接触点：两支撑点中点
                const ktm::fvec3 r_a = vec3_sub(p_contact, a.obb_center);         // 质心/盒心到触点的臂
                const ktm::fvec3 r_b = vec3_sub(p_contact, b.obb_center);
#else
                const ktm::fvec3 p_a = aabb_support_world(a.center_world, a.half_extents, normal);
                const ktm::fvec3 p_b = aabb_support_world(b.center_world, b.half_extents,
                                                          make_fvec3(-normal.x, -normal.y, -normal.z));
                const ktm::fvec3 p_contact = vec3_mul(vec3_add(p_a, p_b), 0.5f);  // AABB 模式下同样用中点
                const ktm::fvec3 r_a = vec3_sub(p_contact, a.center_world);       // 此处 center_world≈AABB 心
                const ktm::fvec3 r_b = vec3_sub(p_contact, b.center_world);
#endif

                ktm::fvec3& va = impl_->body(ha).velocity;
                ktm::fvec3& vb = impl_->body(hb).velocity;
                ktm::fvec3& wa = impl_->body(ha).angular_velocity;
                ktm::fvec3& wb = impl_->body(hb).angular_velocity;

                ktm::fvec3 v_pa = velocity_at_point_world(va, wa, r_a);
                ktm::fvec3 v_pb = velocity_at_point_world(vb, wb, r_b);
                ktm::fvec3 v_rel = make_fvec3(v_pa.x - v_pb.x, v_pa.y - v_pb.y, v_pa.z - v_pb.z);
                // n 从 A 指向 B：v_rel = v_pa - v_pb，v_n > 0 表示沿 n 相互接近（需法向冲量）
                const float v_n = ktm::dot(v_rel, normal);
                if (v_n < -1e-4f) {
                    continue;
                }

                const ktm::fvec3 raxn = ktm::cross(r_a, normal);  // r×n，进入 ω 的有效惯量投影公式
                const ktm::fvec3 rbxn = ktm::cross(r_b, normal);
                // 标量 ang_n：世界系下 (I_w^{-1} (r×n))·(r×n)，即柔度矩阵 K 中法对角项
                const float ang_n_a = sleep_a ? 0.f
                                              : ktm::dot(raxn, world_inertia_inv_apply(a.rot_body_to_world, a.inertia_inv_body, raxn));
                const float ang_n_b = sleep_b ? 0.f
                                              : ktm::dot(rbxn, world_inertia_inv_apply(b.rot_body_to_world, b.inertia_inv_body, rbxn));
                const float denom_n = inv_ma + inv_mb + ang_n_a + ang_n_b + eps;  // 1 / (有效质量)
                if (denom_n <= 1e-12f) {
                    continue;  // 近奇异（例如双臂共线且惯量项异常）
                }
                const float j_raw = -(1.0f + rest_use) * v_n / denom_n;  // 法向冲量标量（未钳制）
                const float j = std::max(-max_impulse_per_contact,
                                         std::min(max_impulse_per_contact, j_raw));  // 钳制防止单帧爆炸

                va.x += normal.x * j * inv_ma;
                va.y += normal.y * j * inv_ma;
                va.z += normal.z * j * inv_ma;
                vb.x -= normal.x * j * inv_mb;
                vb.y -= normal.y * j * inv_mb;
                vb.z -= normal.z * j * inv_mb;

                const ktm::fvec3 Jn = make_fvec3(normal.x * j, normal.y * j, normal.z * j);  // 法向冲量向量
                if (!sleep_a) {
                    const ktm::fvec3 dw =
                        world_inertia_inv_apply(a.rot_body_to_world, a.inertia_inv_body, ktm::cross(r_a, Jn));  // Δω = I^{-1}(r×J)
                    wa.x += dw.x;
                    wa.y += dw.y;
                    wa.z += dw.z;
                }
                if (!sleep_b) {
                    const ktm::fvec3 dw = world_inertia_inv_apply(
                        b.rot_body_to_world, b.inertia_inv_body, ktm::cross(r_b, make_fvec3(-Jn.x, -Jn.y, -Jn.z)));  // B 受力为 -J
                    wb.x += dw.x;
                    wb.y += dw.y;
                    wb.z += dw.z;
                }

                // 法向冲量后钳制速度，防止累积过大
                {
                    auto clamp_speed_local = [&](ktm::fvec3& v, float max_spd) {
                        float spd_sq = v.x * v.x + v.y * v.y + v.z * v.z;
                        if (spd_sq > max_spd * max_spd) {
                            float inv = max_spd / std::sqrt(spd_sq);
                            v.x *= inv;
                            v.y *= inv;
                            v.z *= inv;
                        }
                    };
                    if (!sleep_a) {
                        clamp_speed_local(va, max_linear_speed);
                        clamp_speed_local(wa, max_angular_speed);
                    }
                    if (!sleep_b) {
                        clamp_speed_local(vb, max_linear_speed);
                        clamp_speed_local(wb, max_angular_speed);
                    }
                }

                // 追踪本轮最大速度修正，用于提前终止判断
                {
                    float dv_a = std::abs(j) * inv_ma;
                    float dv_b = std::abs(j) * inv_mb;
                    float dv_max = std::max(dv_a, dv_b);
                    max_delta_v_sq_this_iter = std::max(max_delta_v_sq_this_iter, dv_max * dv_max);
                }

                v_pa = velocity_at_point_world(va, wa, r_a);
                v_pb = velocity_at_point_world(vb, wb, r_b);
                v_rel = make_fvec3(v_pa.x - v_pb.x, v_pa.y - v_pb.y, v_pa.z - v_pb.z);
                const float v_n_rel = ktm::dot(v_rel, normal);  // 法向冲量后的接近速度（可 <0，表示分离中）
                ktm::fvec3 v_t = make_fvec3(                    // v_rel 去掉法向分量 = 切向滑移速度
                    v_rel.x - normal.x * v_n_rel,
                    v_rel.y - normal.y * v_n_rel,
                    v_rel.z - normal.z * v_n_rel);
                const float vt_len = ktm::length(v_t);
                // 追踪摩擦冲量导致的速度修正，纳入收敛判断
                // （jt_abs 在摩擦块内赋值，此处默认为0；仅在摩擦块执行后被设为非零）
                float jt_abs = 0.0f;
                if (vt_len > eps) {
                    const ktm::fvec3 tdir = make_fvec3(v_t.x / vt_len, v_t.y / vt_len, v_t.z / vt_len);  // 滑移方向单位向量
                    const float v_slip = ktm::dot(v_rel, tdir);                                          // 沿 tdir 的标量滑移速度
                    const ktm::fvec3 raxt = ktm::cross(r_a, tdir);
                    const ktm::fvec3 rbxt = ktm::cross(r_b, tdir);
                    const float ang_t_a = sleep_a ? 0.f
                                                  : ktm::dot(raxt, world_inertia_inv_apply(a.rot_body_to_world, a.inertia_inv_body, raxt));
                    const float ang_t_b = sleep_b ? 0.f
                                                  : ktm::dot(rbxt, world_inertia_inv_apply(b.rot_body_to_world, b.inertia_inv_body, rbxt));
                    const float denom_t = inv_ma + inv_mb + ang_t_a + ang_t_b + eps;
                    if (denom_t > 1e-12f) {
                        const float jt_free = -v_slip / denom_t;                        // 无摩擦上限时的切向冲量（完全粘滞）
                        // 静/动摩擦区分：极低滑移速度时使用更高的静摩擦系数，防止坡面微滑循环
                        constexpr float k_static_slip_threshold = 0.02f;               // 低于此速度视为"欲动未动"
                        const float eff_friction = (vt_len < k_static_slip_threshold)
                                                   ? static_friction_coeff
                                                   : friction_coeff;
                        const float jt_cap = eff_friction * std::fabs(j);              // 库仑锥 |jt| ≤ μ|j|
                        const float jt = std::max(-jt_cap, std::min(jt_cap, jt_free));  // 钳位到摩擦锥内
                        jt_abs = std::abs(jt);  // 记录用于收敛判断

                        va.x += tdir.x * jt * inv_ma;
                        va.y += tdir.y * jt * inv_ma;
                        va.z += tdir.z * jt * inv_ma;
                        vb.x -= tdir.x * jt * inv_mb;
                        vb.y -= tdir.y * jt * inv_mb;
                        vb.z -= tdir.z * jt * inv_mb;

                        const ktm::fvec3 Jt = make_fvec3(tdir.x * jt, tdir.y * jt, tdir.z * jt);
                        if (!sleep_a) {
                            const ktm::fvec3 dw =
                                world_inertia_inv_apply(a.rot_body_to_world, a.inertia_inv_body, ktm::cross(r_a, Jt));
                            wa.x += dw.x;
                            wa.y += dw.y;
                            wa.z += dw.z;
                        }
                        if (!sleep_b) {
                            const ktm::fvec3 dw = world_inertia_inv_apply(
                                b.rot_body_to_world, b.inertia_inv_body,
                                ktm::cross(r_b, make_fvec3(-Jt.x, -Jt.y, -Jt.z)));
                            wb.x += dw.x;
                            wb.y += dw.y;
                            wb.z += dw.z;
                        }
                    }
                }

                // 摩擦冲量后再次钳制速度
                {
                    auto clamp_speed_local2 = [&](ktm::fvec3& v, float max_spd) {
                        float spd_sq = v.x * v.x + v.y * v.y + v.z * v.z;
                        if (spd_sq > max_spd * max_spd) {
                            float inv = max_spd / std::sqrt(spd_sq);
                            v.x *= inv;
                            v.y *= inv;
                            v.z *= inv;
                        }
                    };
                    if (!sleep_a) {
                        clamp_speed_local2(va, max_linear_speed);
                        clamp_speed_local2(wa, max_angular_speed);
                    }
                    if (!sleep_b) {
                        clamp_speed_local2(vb, max_linear_speed);
                        clamp_speed_local2(wb, max_angular_speed);
                    }
                }

                // 追踪摩擦冲量导致的速度修正，纳入收敛判断
                if (jt_abs > 0.0f) {
                    float dv_a_t = jt_abs * inv_ma;
                    float dv_b_t = jt_abs * inv_mb;
                    float dv_max_t = std::max(dv_a_t, dv_b_t);
                    max_delta_v_sq_this_iter = std::max(max_delta_v_sq_this_iter, dv_max_t * dv_max_t);
                }

                if (impulse_iter == k_impulse_iterations - 1) {
                    // 末轮：按穿透深度记录软位置校正（延迟到 Phase 6 积分后统一应用，避免抖动）
                    const float pen = std::max(0.f, penetration - k_positional_slop);
                    // 钳制穿透深度以防止大穿透时的位置校正过大（如物体生成在碰撞体内部）
                    const float pen_clamped = std::min(pen, max_position_correction / k_positional_percent);
                    if (pen_clamped > 0.f) {
                        const float inv_sum = inv_ma + inv_mb;  // 按逆质量比例分摊平移
                        if (inv_sum > eps) {
                            const float corr_scale = k_positional_percent * pen_clamped / inv_sum;
                            const auto record_corr = [&](std::uintptr_t handle, float inv_eff, float sign) {
                                if (inv_eff <= eps) return;
                                auto& corr = position_correction[handle];  // 默认初始化为 {0,0,0}
                                corr.x += sign * normal.x * corr_scale * inv_eff;
                                corr.y += sign * normal.y * corr_scale * inv_eff;
                                corr.z += sign * normal.z * corr_scale * inv_eff;
                            };
                            record_corr(ha, inv_ma, -1.f);
                            record_corr(hb, inv_mb, +1.f);
                        }
                    }

                    // 只有当法向冲量导致的速度变化超过休眠阈值时才唤醒
                    {
                        const float wake_impulse_threshold = sleep_threshold * 2.0f;
                        const float delta_v_a = std::abs(j) * inv_ma;
                        const float delta_v_b = std::abs(j) * inv_mb;

                        if (delta_v_a > wake_impulse_threshold) {
                            impl_->body(ha).sleeping = false;
                            impl_->body(ha).sleep_timer = 0.0f;
                        }
                        if (delta_v_b > wake_impulse_threshold) {
                            impl_->body(hb).sleeping = false;
                            impl_->body(hb).sleep_timer = 0.0f;
                        }
                    }

                    // 记录活跃碰撞对
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
                }  // 末轮：impulse_iter == k_impulse_iterations - 1
            }      // 内层：collision_pairs

            // 收敛早退：本轮所有碰撞对的速度修正都极小，说明已稳定，无需继续迭代
            if (max_delta_v_sq_this_iter < k_early_exit_vel_eps * k_early_exit_vel_eps) {
                break;
            }
        }  // 外层：impulse_iter

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

    // --- 阶段 5b：轴锁定强制执行 — 碰撞冲量求解后，再次清零锁定轴的速度分量 ---
    for (std::uintptr_t h : mechanics_handles) {
        if (impl_->body(h).sleeping) continue;
        uint8_t lin_lock = impl_->body(h).linear_lock;
        if (lin_lock & kLockAxisX) impl_->body(h).velocity.x = 0.0f;
        if (lin_lock & kLockAxisY) impl_->body(h).velocity.y = 0.0f;
        if (lin_lock & kLockAxisZ) impl_->body(h).velocity.z = 0.0f;

        uint8_t ang_lock = impl_->body(h).angular_lock;
        if (ang_lock & kLockAxisX) impl_->body(h).angular_velocity.x = 0.0f;
        if (ang_lock & kLockAxisY) impl_->body(h).angular_velocity.y = 0.0f;
        if (ang_lock & kLockAxisZ) impl_->body(h).angular_velocity.z = 0.0f;
    }

    // --- 阶段 6：半隐式位姿积分（用冲量后的 v,ω）+ 无穷地板 + 休眠累计 + 缓存淘汰 ---
    for (std::size_t i = 0; i < mechanics_data.size(); ++i) {
        if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
            return;
        }
        const auto& data = mechanics_data[i];  // 与阶段 3 同一套 per-body 缓存
        std::uintptr_t h = data.handle;

        // 唤醒因地板降低而悬空的休眠体（物体搁在地板上休眠后，地板调低应自由落体）
        if (impl_->body(h).sleeping && data.min_world.y > floor_y + floor_eps) {
            impl_->body(h).sleeping = false;
            impl_->body(h).sleep_timer = 0.0f;
        }

        if (impl_->body(h).sleeping)
            continue;  // 休眠体不再推进变换

        // 阻塞写锁：位置积分每帧都要写回，_nowait 拿不到锁会跳过本帧导致物体卡顿/抖动。
        // 用阻塞版等锁（不漏帧），槽位失效时返回无效句柄而非抛异常。
        auto tx_w = transform_storage.try_acquire_write(data.transform_handle);
        if (!tx_w) continue;

        // 为轴锁定准备一份清零后的速度副本（用于积分，不修改全局缓存）
        ktm::fvec3 vel_for_pos = impl_->body(h).velocity;
        uint8_t lin_lock = impl_->body(h).linear_lock;
        if (lin_lock & kLockAxisX) vel_for_pos.x = 0.0f;
        if (lin_lock & kLockAxisY) vel_for_pos.y = 0.0f;
        if (lin_lock & kLockAxisZ) vel_for_pos.z = 0.0f;

        // 速度 × dt 平移（显式欧拉；与阶段 3 预测一致）
        tx_w->position.x += vel_for_pos.x * fixed_dt;
        tx_w->position.y += vel_for_pos.y * fixed_dt;
        tx_w->position.z += vel_for_pos.z * fixed_dt;

        // 应用 Phase 5 累积的位置校正（积分后统一应用，避免校正与积分不一致导致抖动）
        auto corr_it = position_correction.find(h);
        if (corr_it != position_correction.end()) {
            ktm::fvec3 corr = corr_it->second;
            if (lin_lock & kLockAxisX) corr.x = 0.0f;
            if (lin_lock & kLockAxisY) corr.y = 0.0f;
            if (lin_lock & kLockAxisZ) corr.z = 0.0f;
            tx_w->position.x += corr.x;
            tx_w->position.y += corr.y;
            tx_w->position.z += corr.z;
        }

        {  // 朝向：以四元数为真值源，欧拉仅用于与渲染/资产管线对齐
            auto& body = impl_->body(h);
            if (!body.orientation_initialized) {
                body.orientation_quat = quat_from_model_euler(tx_w->euler_rotation);
                body.orientation_initialized = true;
            }
            // 为轴锁定准备一份清零后的角速度副本（用于积分，不修改全局缓存）
            ktm::fvec3 ang_for_rot = body.angular_velocity;
            uint8_t ang_lock = body.angular_lock;
            if (ang_lock & kLockAxisX) ang_for_rot.x = 0.0f;
            if (ang_lock & kLockAxisY) ang_for_rot.y = 0.0f;
            if (ang_lock & kLockAxisZ) ang_for_rot.z = 0.0f;
            integrate_orientation_quat(body.orientation_quat, ang_for_rot, fixed_dt);  // q ← q ⊗ Δq(ω)
            sync_euler_from_orientation_quat(body.orientation_quat, tx_w->euler_rotation);            // 写回 XYZ 欧拉（约定与引擎一致）
        }

        // 复用 Phase 3 已计算的世界 AABB min.y，避免重新计算完整 world AABB
        // 积分后 position 偏移量与 Phase 3 的预测一致，因此可直接复用
        float object_bottom_y = data.min_world.y;
        // 若有位置校正，补偿底面高度（仅在 Y 轴未锁时考虑校正）
        if (corr_it != position_correction.end() && !(lin_lock & kLockAxisY)) {
            object_bottom_y += corr_it->second.y;
        }

        // 碰撞检测开关判断：若物体关闭碰撞，跳过地板碰撞处理
        bool collision_enabled = true;
        auto frame_it = frame_params.find(h);
        if (frame_it != frame_params.end()) {
            collision_enabled = frame_it->second.collision_enabled;
        }

        // 水平 floor_y：穿插时整体上抬，并做法向/切向「处方」（非完整接触流形）
        // 轴锁定：地板碰撞的各轴修正仅在对应轴未锁时执行
        if (collision_enabled && object_bottom_y < floor_y + floor_eps) {
            // 消穿修正仅在 Y 轴未锁时执行
            if (!(lin_lock & kLockAxisY)) {
                tx_w->position.y += (floor_y + floor_eps) - object_bottom_y;
            }

            // 法向速度反弹仅在 Y 轴未锁时处理
            if (!(lin_lock & kLockAxisY)) {
                float y_vel = impl_->body(h).velocity.y;  // 向上为正
                if (y_vel < -low_vel_threshold) {
                    impl_->body(h).velocity.y = -y_vel * floor_restitution;  // 下行且够快则反弹
                    impl_->body(h).sleep_timer = 0.0f;                       // 显著弹跳才打断休眠计时
                } else {
                    if (std::abs(impl_->body(h).velocity.y) < zero_vel_threshold) {
                        impl_->body(h).velocity.y = 0.0f;  // 粘地：贴住时竖直速度清零
                    } else {
                        impl_->body(h).velocity.y *= 0.15f;  // 弱弹簧感衰减残余弹跳
                    }
                }
            }

            // 地板摩擦力仅在对应轴未锁时应用
            if (!(lin_lock & kLockAxisX)) impl_->body(h).velocity.x *= 0.8f;
            if (!(lin_lock & kLockAxisZ)) impl_->body(h).velocity.z *= 0.8f;

            // 滚阻仅在对应轴未锁时应用
            uint8_t ang_lock = impl_->body(h).angular_lock;
            if (!(ang_lock & kLockAxisX)) impl_->body(h).angular_velocity.x *= 0.7f;
            if (!(ang_lock & kLockAxisY)) impl_->body(h).angular_velocity.y *= 0.7f;
            if (!(ang_lock & kLockAxisZ)) impl_->body(h).angular_velocity.z *= 0.7f;
            // 静接触不打断休眠计时，让休眠检测正常累积
        }
    }

    for (std::uintptr_t h : mechanics_handles) {
        if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
            return;
        }
        if (impl_->body(h).sleeping) continue;

        const auto& v = impl_->body(h).velocity;
        const auto& av = impl_->body(h).angular_velocity;

        float v_sq = v.x * v.x + v.y * v.y + v.z * v.z;
        float av_sq = av.x * av.x + av.y * av.y + av.z * av.z;

        if (v_sq < sleep_threshold_sq && av_sq < sleep_threshold_sq) {
            impl_->body(h).sleep_timer += fixed_dt;  // 低速窗口累加
            if (impl_->body(h).sleep_timer >= sleep_time_needed) {
                impl_->body(h).sleeping = true;
                impl_->body(h).velocity = make_fvec3(0.0f, 0.0f, 0.0f);
                impl_->body(h).angular_velocity = make_fvec3(0.0f, 0.0f, 0.0f);  // 冻结动力学状态
            }
        } else {
            impl_->body(h).sleep_timer = 0.0f;  // 一有运动就打断休眠倒计时
        }

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

    // 清理无效句柄的缓存
    std::unordered_set<std::uintptr_t> alive_handles(mechanics_handles.begin(), mechanics_handles.end());

    for (auto it = impl_->bodies.begin(); it != impl_->bodies.end();) {
        if (!alive_handles.count(it->first)) {
            it = impl_->bodies.erase(it);  // 本帧未出现的 mechanics 句柄：删掉 stale 条目防 map 膨胀
        } else {
            ++it;
        }
    }

}

// ============================================================================
// update_skinned_geometry（P2，自 GeometrySystem 迁入）
// 功能：每真实帧对蒙皮 actor 做 CPU 线性混合蒙皮（LBS），把结果重传到 GPU 顶点缓冲。
// 调用时机：MechanicsSystem::update() 的固定步进循环之后（每真实帧一次，独立于
//           物理 fixed_dt 的步进次数，也不受 simulation_enabled 影响——蒙皮模型
//           即使未开物理也应自动循环播放）。
//
// 数据流：Scene(绑定顶点+骨骼权重+骨架+动画) → compute_pose 算 final[] →
//         skin_one_vertex 蒙皮 → write_bytes 重传 vertexBuffer/vertexStorageBuffer。
//         蒙皮结果同时存入 GeometryDevice.skinned_cpu_vertices（结果槽，所有 buffer/CPU
//         数据仍归 GeometrySystem 持有，便于流式加载 LRU 管理），供 Vision / 物理消费。
//
// 与原 GeometrySystem 版本的唯一差异：LOD1..N 的 GPU 缓冲句柄不再直接读 Geometry 的
// 私有 lod_cache，而是通过公有接口 GeometrySystem::get_skinning_targets() 借出（引用
// 计数句柄，锁外 write_bytes）。LOD0 仍从 GeometryDevice.mesh_handles 取（无 LOD 的
// 蒙皮模型唯一可写路径）。
//
// 锁序：对 Scene 仅加共享读锁（播放期无人写 Scene）；重 CPU 蒙皮在所有 storage 锁外执行；
//       get_skinning_targets 内部锁 lod_cache_mutex，必须在释放 geom 写锁之后调用，
//       避免新锁嵌套。
//
// 自动循环：始终播放 animations[0]，anim_time 由 advance_anim_time 推进并 fmod 回绕。
// ============================================================================
void MechanicsSystem::update_skinned_geometry() {
    // 懒缓存 GeometrySystem 指针（first_update 路径可能未经过 update_physics）
    if (!impl_->geometry_sys && impl_->ctx) {
        impl_->geometry_sys = dynamic_cast<GeometrySystem*>(impl_->ctx->get_system("Geometry"));
    }
    if (!impl_->geometry_sys) return;  // LOD1..N 句柄借不到，且无意义

    auto& resource_manager = Resource::ResourceManager::get_instance();
    auto& hub = SharedDataHub::instance();
    auto& geom_storage = hub.geometry_storage();

    // ---- 计算 dt（首帧为 0）----
    const auto now = std::chrono::steady_clock::now();
    float dt = 0.0f;
    if (impl_->last_skin_update_time.has_value()) {
        dt = std::chrono::duration<float>(now - *impl_->last_skin_update_time).count();
    }
    impl_->last_skin_update_time = now;
    if (dt > 0.1f) dt = 0.1f;  // 容错：断点/卡顿导致的大 dt 夹住，避免动画跳跃

    // 先收集所有 geometry handle，避免在迭代 storage 期间持锁做重计算
    std::vector<std::uintptr_t> geom_handles;
    for (auto it = geom_storage.cbegin(); it != geom_storage.cend(); ++it) {
        const GeometryDevice& geom_dev = *it;
        geom_handles.push_back(reinterpret_cast<std::uintptr_t>(&geom_dev));
    }

    for (auto geom_handle : geom_handles) {
        // ---- 第 1 步：读取 model_resource_handle（brief 读锁）----
        std::uintptr_t model_resource_handle = 0;
        {
            auto geom_read = geom_storage.try_acquire_read(geom_handle);
            if (!geom_read) continue;
            model_resource_handle = geom_read->model_resource_handle;
        }
        if (model_resource_handle == 0) continue;

        // ---- 第 2 步：解析 model_id ----
        std::uint64_t model_id = 0;
        if (auto model_res = hub.model_resource_storage().try_acquire_read(model_resource_handle)) {
            model_id = model_res->model_id;
        }
        if (model_id == 0) continue;

        // ---- 第 3 步：取 Scene，判断是否蒙皮（非蒙皮直接跳过）----
        auto scene_read = resource_manager.acquire_read<Resource::Scene>(model_id);
        if (!scene_read.valid()) continue;
        const Resource::Scene& scene = *scene_read;
        if (!scene.data.skeleton.has_value() || scene.data.animations.empty()) continue;

        const Resource::SkeletonData& skeleton = *scene.data.skeleton;
        const Resource::AnimationClip& clip = scene.data.animations[0];  // 自动循环第 0 个

        // ---- 第 4 步：brief 写锁推进 anim_time + 拷出 buffer 句柄 ----
        // HardwareBuffer 为引用计数句柄，可拷贝；拷出后锁外做蒙皮+write_bytes。
        float anim_time = 0.0f;
        std::vector<Horizon::HardwareBuffer> vbufs;
        std::vector<Horizon::HardwareBuffer> vstoragebufs;
        std::size_t mesh_count = 0;
        std::vector<Resource::IkChain> ik_chains;  // 锁外跑 CCD 用（拷出避免持锁）
        {
            auto geom_write = geom_storage.try_acquire_write(geom_handle);
            if (!geom_write) continue;
            geom_write->is_skinned = true;
            geom_write->anim_time = Resource::advance_anim_time(geom_write->anim_time, dt, clip);
            anim_time = geom_write->anim_time;
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
        // get_skinning_targets 返回 levels[0..N]，[0] 即 LOD0（已在 vbufs/vstoragebufs 中），
        // 此处只取 [1..]。LOD1..N 中未驻留的级别其句柄为空，下方写入时由空句柄判断跳过。
        // 注意：必须在释放 geom 写锁之后调用（内部锁 lod_cache_mutex），避免锁嵌套。
        std::vector<std::vector<std::pair<Horizon::HardwareBuffer, Horizon::HardwareBuffer>>>
            lod_targets(mesh_count);
        for (std::size_t mi = 0; mi < mesh_count; ++mi) {
            auto all_levels = impl_->geometry_sys->get_skinning_targets(
                geom_handle, static_cast<uint32_t>(mi));
            // [0] 是 LOD0（= mesh_dev 缓冲），跳过；[1..] 为简化级
            for (std::size_t l = 1; l < all_levels.size(); ++l) {
                lod_targets[mi].push_back(all_levels[l]);
            }
        }

        // ---- 第 5 步：锁外计算骨骼最终矩阵（每 geom 一次）----
        // 若该实例有启用的 IK 链：采样动画 local → 逐链 CCD 求解并写回 working_locals →
        // 用 working_locals 算 finals，使 IK 姿态叠加在动画之上。无 IK 链时走原路径。
        std::vector<std::array<float, 16>> finals;
        bool has_active_ik = false;
        for (const auto& ch : ik_chains) {
            if (ch.enabled) {
                has_active_ik = true;
                break;
            }
        }
        if (has_active_ik) {
            // working_locals：每条链解完后把结果写回，供下一条链感知前链修改（Bug-2 修复）。
            // 解完所有链后直接作为 precomputed_locals 传给 compute_pose，避免重复采样（Perf-1 修复）。
            std::vector<std::array<float, 16>> working_locals;
            Resource::sample_pose_locals(skeleton, clip, anim_time, working_locals);

            for (const auto& ch : ik_chains) {
                if (!ch.enabled) continue;
                std::unordered_map<int, std::array<float, 16>> chain_ov;
                Resource::solve_ccd(skeleton, ch, working_locals, chain_ov);
                // 把本链改写的关节写回 working_locals，下一条链从更新后的状态出发。
                // 注：weight < 1 时此处写入的是 IK 与原动画的混合值，后链的 weight 混合
                //     以此为基准而非原动画——多链叠加语义，首版可接受。
                for (const auto& kv : chain_ov) {
                    working_locals[static_cast<std::size_t>(kv.first)] = kv.second;
                }
            }
            // working_locals 已含所有链的 IK 结果，直接传入跳过 compute_pose 内部的重复采样。
            Resource::compute_pose(skeleton, clip, anim_time, finals, nullptr, &working_locals);
        } else {
            Resource::compute_pose(skeleton, clip, anim_time, finals);
        }

        // ---- 第 6 步：锁外 CPU 蒙皮每个 mesh + 重传 GPU ----
        // skinned_cpu_vertices：每 mesh 一份原始字节（布局即 Resource::Vertex 数组），
        // Vision / 物理复用同一数据源。
        // 同时累积所有蒙皮顶点的动态 local AABB（物理用，跟随当前姿态）。
        std::vector<std::vector<std::byte>> skinned_blobs(mesh_count);
        float aabb_min_x = std::numeric_limits<float>::max();
        float aabb_min_y = std::numeric_limits<float>::max();
        float aabb_min_z = std::numeric_limits<float>::max();
        float aabb_max_x = std::numeric_limits<float>::lowest();
        float aabb_max_y = std::numeric_limits<float>::lowest();
        float aabb_max_z = std::numeric_limits<float>::lowest();
        bool aabb_any = false;
        for (std::size_t mesh_idx = 0;
             mesh_idx < mesh_count && mesh_idx < scene.data.meshes.size(); ++mesh_idx) {
            const Resource::MeshData& mesh = scene.data.meshes[mesh_idx];
            // 该 mesh 非蒙皮（混合场景）或数据不一致 → 跳过（保留其原顶点缓冲不动）
            if (mesh.bone_weights.empty() ||
                mesh.bone_weights.size() != mesh.vertices.size()) {
                continue;
            }

            std::vector<Resource::Vertex> skinned(mesh.vertices.size());
            for (std::size_t v = 0; v < mesh.vertices.size(); ++v) {
                skinned[v] = skin_one_vertex(mesh.vertices[v], mesh.bone_weights[v], finals);
                const auto& p = skinned[v].position;
                aabb_min_x = std::min(aabb_min_x, p[0]);
                aabb_min_y = std::min(aabb_min_y, p[1]);
                aabb_min_z = std::min(aabb_min_z, p[2]);
                aabb_max_x = std::max(aabb_max_x, p[0]);
                aabb_max_y = std::max(aabb_max_y, p[1]);
                aabb_max_z = std::max(aabb_max_z, p[2]);
                aabb_any = true;
            }

            // 转字节并重传（vertexBuffer 供光栅，vertexStorageBuffer 供 material_resolve compute）
            auto src = std::as_bytes(std::span<const Resource::Vertex>(skinned.data(), skinned.size()));
            auto& blob = skinned_blobs[mesh_idx];
            blob.assign(src.begin(), src.end());

            if (mesh_idx < vbufs.size() && vbufs[mesh_idx]) {
                (void)vbufs[mesh_idx].write_bytes(std::span<const std::byte>(blob.data(), blob.size()));
            }
            if (mesh_idx < vstoragebufs.size() && vstoragebufs[mesh_idx]) {
                (void)vstoragebufs[mesh_idx].write_bytes(std::span<const std::byte>(blob.data(), blob.size()));
            }

            // ---- 同法蒙皮所有已驻留 LOD 级别 ----
            // 每个 LODLevel 携带与其顶点等长的 bone_weights（导入时随顶点同步 remap），
            // 故按 LOD0 同样的方式 skin_one_vertex 后写入对应 LOD GPU 缓冲。
            // 顺序与 upload_lod_from_scene_data / get_skinning_targets 一致：跳过空 LOD、
            // 按非空顺序与 levels[1..] 对齐；未驻留级别其句柄为空，write 由空句柄跳过。
            const auto& targets = lod_targets[mesh_idx];
            if (!targets.empty()) {
                std::size_t ti = 0;
                for (std::size_t li = 0;
                     li < mesh.lod_levels.size() && ti < targets.size(); ++li) {
                    const Resource::LODLevel& lod = mesh.lod_levels[li];
                    if (lod.vertices.empty() || lod.indices.empty()) continue;  // 与 upload 跳过逻辑一致

                    // 该 LOD 有等长骨骼权重才能蒙皮；否则保持其绑定姿态（仍推进 ti 保持对齐）
                    if (!lod.bone_weights.empty() &&
                        lod.bone_weights.size() == lod.vertices.size()) {
                        std::vector<Resource::Vertex> sk(lod.vertices.size());
                        for (std::size_t v = 0; v < lod.vertices.size(); ++v) {
                            sk[v] = skin_one_vertex(lod.vertices[v], lod.bone_weights[v], finals);
                        }
                        auto lsrc = std::as_bytes(
                            std::span<const Resource::Vertex>(sk.data(), sk.size()));
                        std::vector<std::byte> lblob(lsrc.begin(), lsrc.end());
                        if (targets[ti].first) {
                            (void)targets[ti].first.write_bytes(
                                std::span<const std::byte>(lblob.data(), lblob.size()));
                        }
                        if (targets[ti].second) {
                            (void)targets[ti].second.write_bytes(
                                std::span<const std::byte>(lblob.data(), lblob.size()));
                        }
                    }
                    ++ti;
                }
            }
        }

        // ---- 第 7 步：brief 写锁存回蒙皮结果 + 动态 AABB（供 Vision / 物理消费）----
        {
            auto geom_write = geom_storage.try_acquire_write(geom_handle);
            if (geom_write) {
                geom_write->skinned_cpu_vertices = std::move(skinned_blobs);
                if (aabb_any) {
                    geom_write->skinned_aabb_min = ktm::fvec3{aabb_min_x, aabb_min_y, aabb_min_z};
                    geom_write->skinned_aabb_max = ktm::fvec3{aabb_max_x, aabb_max_y, aabb_max_z};
                    geom_write->skinned_aabb_valid = true;
                }
            }
        }
    }
}

}  // namespace Corona::Systems




