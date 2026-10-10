#include <corona/events/engine_events.h>
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
            // TODO: 后续实现 offline physics proxy —— Unloaded + body_type=Dynamic
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
        if (frame_params[h].body_type != BodyType::Dynamic) continue;  // Static/Kinematic 不受重力
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

        entry.rot_body_to_world = q_pred.matrix3x3();  // 体→世界旋转（预测姿态）
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

    // 预加载所有物理物体的碰撞网格（三角形碰撞检测）
    // 有 model_id 就尝试建碰撞网格；AABB 宽相已完成粗筛，三角窄相是精化阶段。
    for (const auto& entry : mechanics_data) {
        if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
            return;
        }
        if (entry.model_id == 0) continue;

        if (entry.is_skinned) {
            // 蒙皮物体：确保 static_triangle_index_cache 已解析（顶点每帧由 update_skinned_geometry 刷新）
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

    // 阶段 2：为每个有碰撞网格的物体建/refit TriangleOctree
    // 静态物体（非蒙皮）：按 model_id 缓存，建一次不 refit。
    // 蒙皮物体：键为 geom_handle，每子步用当前世界顶点 refit（拓扑固定，只更新拟合盒）。
    // 世界顶点在下面的 ensure_world_verts 路径中惰性建立；此处只确保静态树存在。
    for (const auto& entry : mechanics_data) {
        if (entry.model_id == 0 || entry.is_skinned) continue;
        auto cit = impl_->collision_mesh_cache.find(entry.model_id);
        if (cit == impl_->collision_mesh_cache.end() || cit->second.triangles.empty()) continue;
        auto oit = impl_->triangle_octree_cache.find(entry.model_id);
        if (oit == impl_->triangle_octree_cache.end()) {
            // 首次：用绑定姿态顶点建树
            MechanicsInternal::TriangleOctree tree;
            tree.build(cit->second.vertices, cit->second);
            impl_->triangle_octree_cache[entry.model_id] = std::move(tree);

            // FootPlant probe 需要世界空间顶点（静态体不移动，只建一次）
            auto tx_r = transform_storage.try_acquire_read(entry.transform_handle);
            if (tx_r) {
                std::vector<ktm::fvec3> wv;
                wv.reserve(cit->second.vertices.size());
                transform_vertices_to_world(cit->second.vertices, *tx_r, wv);
                impl_->static_world_verts_cache[entry.model_id] = std::move(wv);
            }
        }
    }

    // 阶段 5：从 GeometrySystem 获取宽相候选对 → 窄相（AABB 或 OBB+SAT）→ 顺序冲量 + 摩擦 + 迭代后位置校正 ---
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
        constexpr float k_positional_slop = 0.004f;    // Baumgarte 式校正：小穿透只靠冲量，不修位姿
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
            float last_j = 0.f;       // 最后一轮施加的法向冲量；该轮处于分离（未施加）则为 0
            // E2：累积冲量（跨迭代轮次，本子步内累加，warm start 用）
            float accumulated_j  = 0.f;  // 法向累积冲量（≥ 0，不允许负值即拉力）
            float accumulated_jt = 0.f;  // 切向累积冲量绝对值（不超过 μ * accumulated_j）
            // 反弹：迭代前记录的初始法向接近速度（v_n_initial > 0 表示接近）
            float v_n_initial = 0.f;
        };
        std::vector<PairContactRecord> pair_records(collision_pairs.size());

        // E2 Bug-3 修复：从跨子步缓存预加载上一子步的 accumulated_j（warm start 数据）
        for (std::size_t pair_idx = 0; pair_idx < collision_pairs.size(); ++pair_idx) {
            const auto& pair = collision_pairs[pair_idx];
            const auto sorted = (pair.first < pair.second)
                ? std::make_pair(pair.first, pair.second)
                : std::make_pair(pair.second, pair.first);
            auto ws_it = impl_->warm_start_cache.find(sorted);
            if (ws_it != impl_->warm_start_cache.end()) {
                pair_records[pair_idx].accumulated_j  = ws_it->second.first;
                pair_records[pair_idx].accumulated_jt = ws_it->second.second;
            }
        }

        // S2：三角形窄相预计算 pass（提出冲量迭代循环，每子步只执行一次）。
        // 世界顶点、octree refit、triangle_narrowphase2 的结果缓存在 PairContactRecord 中，
        // 冲量迭代内直接读取，不再重复执行相同的几何计算。
        {
            // IK 入队去重集合（同帧各对共用，防止多对接触重复覆盖同一骨骼）
            std::unordered_set<int> ik_queued_a_global, ik_queued_b_global;
            for (std::size_t pair_idx = 0; pair_idx < collision_pairs.size(); ++pair_idx) {
                if (impl_->shutdown_requested.load(std::memory_order_acquire)) return;
                const auto& pair = collision_pairs[pair_idx];
                PairContactRecord& rec = pair_records[pair_idx];
                rec.in_contact = false;

                std::uintptr_t ha = pair.first;
                std::uintptr_t hb = pair.second;

                auto it_a = handle_to_index.find(ha);
                auto it_b = handle_to_index.find(hb);
                if (it_a == handle_to_index.end() || it_b == handle_to_index.end()) continue;

                const MechanicsWorldAABB& a = mechanics_data[it_a->second];
                const MechanicsWorldAABB& b = mechanics_data[it_b->second];

                if (!aabb_overlap(a.min_world, a.max_world, b.min_world, b.max_world)) continue;

                auto get_mesh = [&](const MechanicsWorldAABB& body) -> const CollisionMesh* {
                    if (body.is_skinned) {
                        auto it = impl_->skinned_collision_cache.find(body.geom_handle);
                        if (it != impl_->skinned_collision_cache.end() && !it->second.triangles.empty())
                            return &it->second;
                    } else if (body.model_id != 0) {
                        auto it = impl_->collision_mesh_cache.find(body.model_id);
                        if (it != impl_->collision_mesh_cache.end() && !it->second.triangles.empty())
                            return &it->second;
                    }
                    return nullptr;
                };

                const CollisionMesh* mesh_a = get_mesh(a);
                const CollisionMesh* mesh_b = get_mesh(b);

                if (!mesh_a && !mesh_b) continue;
                if (!mesh_a || !mesh_b) {
                    static bool s_logged_missing = false;
                    if (!s_logged_missing) {
                        CFW_LOG_ERROR("MechanicsSystem: collision pair skipped — one side has no triangle mesh "
                                      "(model not yet loaded or ensure_collision_mesh failed). First occurrence only.");
                        s_logged_missing = true;
                    }
                    continue;
                }

                auto ensure_world_verts = [&](std::uintptr_t h, std::uintptr_t transform_h, const CollisionMesh* mesh) {
                    if (!mesh || world_verts_cache.count(h)) return;
                    auto tx = transform_storage.try_acquire_read(transform_h);
                    if (tx) transform_vertices_to_world(mesh->vertices, *tx, world_verts_cache[h]);
                };
                ensure_world_verts(ha, a.transform_handle, mesh_a);
                ensure_world_verts(hb, b.transform_handle, mesh_b);

                auto get_verts = [&](std::uintptr_t h) -> const std::vector<ktm::fvec3>* {
                    auto it = world_verts_cache.find(h);
                    return (it != world_verts_cache.end() && !it->second.empty()) ? &it->second : nullptr;
                };
                const auto* verts_a = get_verts(ha);
                const auto* verts_b = get_verts(hb);
                if (!verts_a || !verts_b) continue;

                // octree 路径（refit 只在此预计算 pass 中执行一次）
                const TriangleOctree* oa = nullptr;
                const TriangleOctree* ob = nullptr;
                if (!a.is_skinned && a.model_id != 0) {
                    auto oit = impl_->triangle_octree_cache.find(a.model_id);
                    if (oit != impl_->triangle_octree_cache.end() && !oit->second.empty()) oa = &oit->second;
                } else if (a.is_skinned && a.geom_handle != 0) {
                    auto oit = impl_->skinned_octree_cache.find(a.geom_handle);
                    if (oit != impl_->skinned_octree_cache.end() && !oit->second.empty()) oa = &oit->second;
                }
                if (!b.is_skinned && b.model_id != 0) {
                    auto oit = impl_->triangle_octree_cache.find(b.model_id);
                    if (oit != impl_->triangle_octree_cache.end() && !oit->second.empty()) ob = &oit->second;
                } else if (b.is_skinned && b.geom_handle != 0) {
                    auto oit = impl_->skinned_octree_cache.find(b.geom_handle);
                    if (oit != impl_->skinned_octree_cache.end() && !oit->second.empty()) ob = &oit->second;
                }

                TriangleContactResult tri;
                if (oa && ob) {
                    auto get_oct_ref = [&](const MechanicsWorldAABB& body) -> TriangleOctree& {
                        if (!body.is_skinned) return impl_->triangle_octree_cache.at(body.model_id);
                        return impl_->skinned_octree_cache.at(body.geom_handle);
                    };
                    auto get_prev_verts = [&](const MechanicsWorldAABB& body, const CollisionMesh* mesh)
                        -> const std::vector<ktm::fvec3>* {
                        if (body.is_skinned) {
                            auto it = impl_->prev_skinned_verts_cache.find(body.geom_handle);
                            return (it != impl_->prev_skinned_verts_cache.end() &&
                                    it->second.size() == (mesh ? mesh->vertices.size() : 0))
                                   ? &it->second : nullptr;
                        } else {
                            auto it = impl_->prev_transform_cache.find(body.transform_handle);
                            if (it == impl_->prev_transform_cache.end() || !mesh) return nullptr;
                            auto& prev_buf = impl_->prev_world_verts_cache[body.handle];
                            transform_vertices_to_world_matrix(mesh->vertices, it->second, prev_buf);
                            return &prev_buf;
                        }
                    };
                    auto& oa_mut = get_oct_ref(a);
                    auto& ob_mut = get_oct_ref(b);
                    oa_mut.refit(*verts_a, *mesh_a, get_prev_verts(a, mesh_a));
                    ob_mut.refit(*verts_b, *mesh_b, get_prev_verts(b, mesh_b));
                    std::vector<std::pair<std::uint32_t, std::uint32_t>> candidate_pairs;
                    TriangleOctree::query_pairs(oa_mut, *verts_a, ob_mut, *verts_b, candidate_pairs);
                    triangle_narrowphase2(*verts_a, *mesh_a, *verts_b, *mesh_b,
                                         a.center_world, b.center_world, tri, 0.02f, &candidate_pairs);
                } else {
                    triangle_narrowphase2(*verts_a, *mesh_a, *verts_b, *mesh_b,
                                         a.center_world, b.center_world, tri);
                }

                if (!tri.has_contact) continue;

                // 缓存窄相结果供冲量迭代消费
                rec.in_contact   = true;
                rec.index_a      = it_a->second;
                rec.index_b      = it_b->second;
                rec.normal       = tri.normal;
                rec.penetration  = tri.penetration > 0.0f ? tri.penetration : 0.0f;
                rec.contact_pt   = tri.contact_point;

                // Phase 3 IK 入队（窄相确认后立即处理，每子步一次）
                // contact_normal_offset 从对应链的配置中读取（FootPlant=0.0, Contact=0.03）。
                auto enqueue_ik = [&](bool skinned, std::uintptr_t mh, int tri_idx,
                                      const MechanicsWorldAABB& body, const ktm::fvec3& contact_normal,
                                      std::unordered_set<int>& queued_nodes) {
                    if (!skinned || tri_idx < 0) return;
                    auto sit = impl_->skinned_collision_cache.find(body.geom_handle);
                    if (sit == impl_->skinned_collision_cache.end()) return;
                    const auto& sc = sit->second;
                    if (tri_idx >= static_cast<int>(sc.triangle_bone_ids.size())) return;
                    int node = sc.triangle_bone_ids[static_cast<std::size_t>(tri_idx)];
                    if (node < 0) return;
                    if (!queued_nodes.insert(node).second) return;
                    std::uintptr_t gh = 0;
                    { auto m = mechanics_storage.try_acquire_read(mh); if (m) gh = m->geometry_handle; }
                    if (!gh) return;
                    // 读该骨骼对应链的 contact_normal_offset；无匹配链时用保守默认值 0.03f
                    // FootPlant 链由地面 probe 驱动，不走碰撞入队，跳过。
                    float normal_offset = 0.03f;
                    bool is_foot_plant_node = false;
                    {
                        auto gr = geometry_storage.try_acquire_read(gh);
                        if (gr) {
                            for (const auto& ch : gr->ik_chains) {
                                if (ch.contact_driven && ch.end_node == node) {
                                    if (ch.mode == Resource::IkChain::Mode::FootPlant) {
                                        is_foot_plant_node = true;
                                    } else {
                                        normal_offset = ch.contact_normal_offset;
                                    }
                                    break;
                                }
                            }
                        }
                    }
                    if (is_foot_plant_node) return;  // FootPlant 由 probe 驱动，不入碰撞队
                    const ktm::fvec3 offset_contact = make_fvec3(
                        tri.contact_point.x + contact_normal.x * normal_offset,
                        tri.contact_point.y + contact_normal.y * normal_offset,
                        tri.contact_point.z + contact_normal.z * normal_offset);
                    impl_->deferred_ik_target_updates.push_back({gh, body.transform_handle, node, offset_contact});
                };
                enqueue_ik(a.is_skinned, ha, tri.best_tri_a, a, tri.normal, ik_queued_a_global);
                enqueue_ik(b.is_skinned, hb, tri.best_tri_b, b,
                           make_fvec3(-tri.normal.x, -tri.normal.y, -tri.normal.z), ik_queued_b_global);
            }
        }

        for (int impulse_iter = 0; impulse_iter < k_impulse_iterations; ++impulse_iter) {
            float max_delta_v_sq_this_iter = 0.0f;
            if (impl_->shutdown_requested.load(std::memory_order_acquire)) return;
            for (std::size_t pair_idx = 0; pair_idx < collision_pairs.size(); ++pair_idx) {
                if (impl_->shutdown_requested.load(std::memory_order_acquire)) return;
                const auto& pair = collision_pairs[pair_idx];
                PairContactRecord& rec = pair_records[pair_idx];
                rec.last_j = 0.f;
                std::uintptr_t ha = pair.first;
                std::uintptr_t hb = pair.second;

                // 跳过未接触对（窄相预计算 pass 已确认）
                if (!rec.in_contact) continue;
                // 双方均休眠则跳过
                if (impl_->body(ha).sleeping && impl_->body(hb).sleeping) continue;

                auto it_a = handle_to_index.find(ha);
                auto it_b = handle_to_index.find(hb);
                if (it_a == handle_to_index.end() || it_b == handle_to_index.end()) continue;

                const MechanicsWorldAABB& a = mechanics_data[it_a->second];
                const MechanicsWorldAABB& b = mechanics_data[it_b->second];

                // AABB 快速排除（迭代间物体可能分离）
                if (!aabb_overlap(a.min_world, a.max_world, b.min_world, b.max_world)) continue;

                // 读取预计算的接触数据
                const ktm::fvec3 normal       = rec.normal;
                const float      penetration  = rec.penetration;
                const ktm::fvec3 contact_point = rec.contact_pt;

                // inv_ma/inv_mb 每轮重新计算（休眠状态可能在迭代间变化）
                const float mass_a = frame_params[ha].mass;
                const float mass_b = frame_params[hb].mass;
                const bool sleep_a = impl_->body(ha).sleeping;
                const bool sleep_b = impl_->body(hb).sleeping;
                const bool fixed_a = frame_params[ha].body_type != BodyType::Dynamic;
                const bool fixed_b = frame_params[hb].body_type != BodyType::Dynamic;
                const float inv_ma = (sleep_a || fixed_a) ? 0.f : 1.0f / mass_a;
                const float inv_mb = (sleep_b || fixed_b) ? 0.f : 1.0f / mass_b;
                // 写回 rec 供 E1 后处理读取
                rec.inv_ma = inv_ma;
                rec.inv_mb = inv_mb;

                const float rest_a = frame_params[ha].restitution;
                const float rest_b = frame_params[hb].restitution;
                const float rest = (rest_a + rest_b) * 0.5f;

                const ktm::fvec3 p_contact = contact_point;
                const ktm::fvec3 r_a = vec3_sub(p_contact, a.center_world);
                const ktm::fvec3 r_b = vec3_sub(p_contact, b.center_world);

                ktm::fvec3& va = impl_->body(ha).velocity;
                ktm::fvec3& vb = impl_->body(hb).velocity;
                ktm::fvec3& wa = impl_->body(ha).angular_velocity;
                ktm::fvec3& wb = impl_->body(hb).angular_velocity;

                // E2：warm start — 第 0 轮把上一子步的累积冲量重新施加
                if (impulse_iter == 0 && rec.accumulated_j > 0.f) {
                    const ktm::fvec3 Jws = make_fvec3(normal.x * rec.accumulated_j,
                                                      normal.y * rec.accumulated_j,
                                                      normal.z * rec.accumulated_j);
                    va.x += Jws.x * inv_ma; va.y += Jws.y * inv_ma; va.z += Jws.z * inv_ma;
                    vb.x -= Jws.x * inv_mb; vb.y -= Jws.y * inv_mb; vb.z -= Jws.z * inv_mb;
                    if (!sleep_a) {
                        const ktm::fvec3 dw = world_inertia_inv_apply(a.rot_body_to_world, a.inertia_inv_body, ktm::cross(r_a, Jws));
                        wa.x += dw.x; wa.y += dw.y; wa.z += dw.z;
                    }
                    if (!sleep_b) {
                        const ktm::fvec3 dw = world_inertia_inv_apply(b.rot_body_to_world, b.inertia_inv_body,
                            ktm::cross(r_b, make_fvec3(-Jws.x, -Jws.y, -Jws.z)));
                        wb.x += dw.x; wb.y += dw.y; wb.z += dw.z;
                    }
                }

                ktm::fvec3 v_pa = velocity_at_point_world(va, wa, r_a);
                ktm::fvec3 v_pb = velocity_at_point_world(vb, wb, r_b);

                // 反弹修正：第 0 轮在加入表面速度之前记录纯刚体法向接近速度
                // warm start 已施加（速度已修正），但不含表面速度，用于确定反弹目标
                if (impulse_iter == 0) {
                    const float v_n_pre_surface = ktm::dot(
                        make_fvec3(v_pa.x - v_pb.x, v_pa.y - v_pb.y, v_pa.z - v_pb.z), normal);
                    rec.v_n_initial = v_n_pre_surface;
                }

                // E3：动画表面速度 — 蒙皮物体的接触点速度还要加上表面本身的运动速度。
                // 从 prev_skinned_verts_cache 和当前帧顶点插值出接触点处的表面速度（Δpos / fixed_dt）。
                auto add_surface_vel = [&](const MechanicsWorldAABB& body,
                                           ktm::fvec3& v_contact,
                                           const ktm::fvec3& cp_world) {
                    if (!body.is_skinned || fixed_dt < 1e-10f) return;
                    auto prev_it = impl_->prev_skinned_verts_cache.find(body.geom_handle);
                    auto curr_it = impl_->skinned_collision_cache.find(body.geom_handle);
                    if (prev_it == impl_->prev_skinned_verts_cache.end() ||
                        curr_it == impl_->skinned_collision_cache.end()) return;
                    const auto& prev_v = prev_it->second;
                    const auto& curr_m = curr_it->second;
                    if (prev_v.empty() || curr_m.vertices.empty() ||
                        prev_v.size() != curr_m.vertices.size()) return;

                    // 将世界空间接触点逆变换到模型空间，再与模型空间顶点比距离
                    ktm::fvec3 cp_model = cp_world;  // 默认兜底（无变换时）
                    {
                        auto tx = transform_storage.try_acquire_read(body.transform_handle);
                        if (tx) {
                            // 逆变换：先减平移，再除以缩放，再逆旋转
                            // 用完整逆矩阵避免非均匀缩放引起误差
                            ktm::fmat4x4 M    = tx->compute_matrix();
                            ktm::fmat4x4 Minv = ktm::inverse(M);
                            ktm::fvec4 cp_h   = make_fvec4(cp_world.x, cp_world.y, cp_world.z, 1.0f);
                            ktm::fvec4 res    = Minv * cp_h;
                            if (std::abs(res.w) > 1e-8f)
                                cp_model = make_fvec3(res.x / res.w, res.y / res.w, res.z / res.w);
                        }
                    }

                    // 找距接触点（模型空间）最近的顶点，把该顶点当前帧与上一帧都变换到
                    // 世界空间后求差，得到正确的世界空间表面速度。
                    // prev/curr 均为模型空间；直接用模型空间差值当世界空间速度会在骨架有
                    // 朝向或旋转时产生方向错误，必须经由世界变换矩阵 M 投影后再做差。
                    float best_d2 = std::numeric_limits<float>::max();
                    std::size_t best_vi = 0;
                    for (std::size_t vi = 0; vi < curr_m.vertices.size(); ++vi) {
                        const ktm::fvec3& cv = curr_m.vertices[vi];
                        const ktm::fvec3 d = sub(cv, cp_model);  // 模型空间距离判断最近顶点
                        float d2 = dot(d, d);
                        if (d2 < best_d2) {
                            best_d2 = d2;
                            best_vi = vi;
                        }
                    }
                    {
                        // 需要 M 在此处——从外层 lambda 捕获的 tx 已被局部作用域释放，重新读取
                        // M 已在上方 cp_model 的求逆路径中算出，但作用域不同；此处单独获取避免
                        // 悬挂引用（成本：一次额外锁 + compute_matrix，与上方逻辑解耦）。
                        auto tx2 = transform_storage.try_acquire_read(body.transform_handle);
                        if (tx2) {
                            const ktm::fmat4x4 M2 = tx2->compute_matrix();
                            const ktm::fvec3& cv  = curr_m.vertices[best_vi];
                            const ktm::fvec3& pv  = prev_v[best_vi];
                            // 模型空间 → 世界空间（列主序：M[col][row]）
                            const ktm::fvec3 world_cv = make_fvec3(
                                M2[0][0]*cv.x + M2[1][0]*cv.y + M2[2][0]*cv.z + M2[3][0],
                                M2[0][1]*cv.x + M2[1][1]*cv.y + M2[2][1]*cv.z + M2[3][1],
                                M2[0][2]*cv.x + M2[1][2]*cv.y + M2[2][2]*cv.z + M2[3][2]);
                            const ktm::fvec3 world_pv = make_fvec3(
                                M2[0][0]*pv.x + M2[1][0]*pv.y + M2[2][0]*pv.z + M2[3][0],
                                M2[0][1]*pv.x + M2[1][1]*pv.y + M2[2][1]*pv.z + M2[3][1],
                                M2[0][2]*pv.x + M2[1][2]*pv.y + M2[2][2]*pv.z + M2[3][2]);
                            const ktm::fvec3 surf_vel = vec3_mul(sub(world_cv, world_pv), 1.0f / fixed_dt);
                            v_contact.x += surf_vel.x;
                            v_contact.y += surf_vel.y;
                            v_contact.z += surf_vel.z;
                        }
                    }
                };
                add_surface_vel(a, v_pa, p_contact);
                add_surface_vel(b, v_pb, p_contact);

                ktm::fvec3 v_rel = make_fvec3(v_pa.x - v_pb.x, v_pa.y - v_pb.y, v_pa.z - v_pb.z);
                // n 从 A 指向 B：v_rel = v_pa - v_pb，v_n > 0 表示沿 n 相互接近（需法向冲量）
                const float v_n = ktm::dot(v_rel, normal);
                // v_n_initial 已在上方 warm start 之后、E3 表面速度之前记录（纯刚体值），此处不覆盖

                // 预判接触（消除悬浮）：
                //   穿透 > 0（已穿入）→ 原始法向冲量逻辑；
                //   穿透 ≤ 0（近接未穿透）→ 仅限制接近速度 ≤ dist/step（不主动推离），不做位置校正。
                //   两种情况下，已分离（v_n < 0）的对都跳过。
                if (penetration <= 0.0f) {
                    // 近接未穿透：只阻止进一步接近，不做推离
                    // 允许速度为 0（静止接触），只阻止 v_n > 0（继续接近）
                    if (v_n <= 0.0f) continue;
                    // 将接近速度限制到 0（不再用 -e*v0 反弹，只是停住）
                    // 使用标准冲量公式，但目标速度 = 0
                } else {
                    // 已穿透：常规分离检测
                    if (v_n < -1e-4f) continue;
                }

                const ktm::fvec3 raxn = ktm::cross(r_a, normal);
                const ktm::fvec3 rbxn = ktm::cross(r_b, normal);
                const float ang_n_a = (sleep_a || fixed_a) ? 0.f
                                              : ktm::dot(raxn, world_inertia_inv_apply(a.rot_body_to_world, a.inertia_inv_body, raxn));
                const float ang_n_b = (sleep_b || fixed_b) ? 0.f
                                              : ktm::dot(rbxn, world_inertia_inv_apply(b.rot_body_to_world, b.inertia_inv_body, rbxn));
                const float denom_n = inv_ma + inv_mb + ang_n_a + ang_n_b + eps;
                if (denom_n <= 1e-12f) continue;

                // 反弹目标：用迭代前记录的 v_n_initial，避免逐轮累加弹性
                // 只在最后一轮加反弹目标，前几轮只消除接近速度（e=0）
                const float v_target = (impulse_iter == k_impulse_iterations - 1)
                                           ? (-rest * std::max(0.0f, rec.v_n_initial))
                                           : 0.0f;
                const float j_raw = -(v_n - v_target) / denom_n;

                // E2：累积冲量 clamp（法向累积 ≥ 0，不允许拉力）
                const float j_prev = rec.accumulated_j;
                const float j_new = std::max(0.0f, j_prev + j_raw);
                const float j = std::max(-max_impulse_per_contact,
                                         std::min(max_impulse_per_contact, j_new - j_prev));
                rec.accumulated_j = j_new;
                rec.last_j = j;

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
                    const ktm::fvec3 tdir = make_fvec3(v_t.x / vt_len, v_t.y / vt_len, v_t.z / vt_len);
                    const float v_slip = ktm::dot(v_rel, tdir);
                    const ktm::fvec3 raxt = ktm::cross(r_a, tdir);
                    const ktm::fvec3 rbxt = ktm::cross(r_b, tdir);
                    const float ang_t_a = sleep_a ? 0.f
                                                  : ktm::dot(raxt, world_inertia_inv_apply(a.rot_body_to_world, a.inertia_inv_body, raxt));
                    const float ang_t_b = sleep_b ? 0.f
                                                  : ktm::dot(rbxt, world_inertia_inv_apply(b.rot_body_to_world, b.inertia_inv_body, rbxt));
                    const float denom_t = inv_ma + inv_mb + ang_t_a + ang_t_b + eps;
                    if (denom_t > 1e-12f) {
                        const float jt_free = -v_slip / denom_t;
                        constexpr float k_static_slip_threshold = 0.02f;
                        const float eff_friction = (vt_len < k_static_slip_threshold)
                                                   ? static_friction_coeff
                                                   : friction_coeff;
                        // E2：切向累积 clamp（|accumulated_jt| ≤ μ * accumulated_j）
                        const float jt_cap = eff_friction * rec.accumulated_j;
                        const float jt_prev = rec.accumulated_jt;  // 上轮累计（带符号，正/负表示方向）
                        const float jt_new_raw = jt_prev + jt_free;
                        const float jt_new = std::max(-jt_cap, std::min(jt_cap, jt_new_raw));
                        const float jt = jt_new - jt_prev;
                        rec.accumulated_jt = jt_new;
                        jt_abs = std::abs(jt);

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
            }      // 内层：collision_pairs

            // 收敛早退：本轮所有碰撞对的速度修正都极小，说明已稳定，无需继续迭代
            if (max_delta_v_sq_this_iter < k_early_exit_vel_eps * k_early_exit_vel_eps) {
                break;
            }
        }  // 外层：impulse_iter

        // E2 Bug-3 修复：把本子步最终的 accumulated_j 写回跨子步缓存，供下一子步 warm start 使用
        for (std::size_t pair_idx = 0; pair_idx < collision_pairs.size(); ++pair_idx) {
            const auto& pair = collision_pairs[pair_idx];
            const PairContactRecord& rec = pair_records[pair_idx];
            const auto sorted = (pair.first < pair.second)
                ? std::make_pair(pair.first, pair.second)
                : std::make_pair(pair.second, pair.first);
            if (rec.in_contact && rec.accumulated_j > 0.f) {
                impl_->warm_start_cache[sorted] = {rec.accumulated_j, rec.accumulated_jt};
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
            const float pen = std::max(0.f, rec.penetration - k_positional_slop);
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

            // 只有当法向冲量导致的速度变化超过休眠阈值时才唤醒。
            // 取最后一轮施加的冲量（与原末轮语义一致）而非累计冲量：静止堆叠每子步
            // 都要抵消重力，累计值恒超阈值，会让堆叠永远无法入睡。
            // 早退时最后一轮 |j|·inv_m < k_early_exit_vel_eps，远低于阈值，不会误唤醒。
            {
                const float wake_impulse_threshold = sleep_threshold * 2.0f;
                const float delta_v_a = std::abs(rec.last_j) * inv_ma;
                const float delta_v_b = std::abs(rec.last_j) * inv_mb;

                if (delta_v_a > wake_impulse_threshold) {
                    impl_->body(ha).sleeping = false;
                    impl_->body(ha).sleep_timer = 0.0f;
                }
                if (delta_v_b > wake_impulse_threshold) {
                    impl_->body(hb).sleeping = false;
                    impl_->body(hb).sleep_timer = 0.0f;
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

        // Static/Kinematic 不被积分器移动（由外部脚本/动画驱动）
        if (frame_params[h].body_type != BodyType::Dynamic)
            continue;

        // 阻塞写锁：位置积分每帧都要写回，_nowait 拿不到锁会跳过本帧导致物体卡顿/抖动。
        // 用阻塞版等锁（不漏帧），槽位失效时返回无效句柄而非抛异常。
        auto tx_w = transform_storage.try_acquire_write(data.transform_handle);
        if (!tx_w) continue;

        // F1：积分前保存本帧变换矩阵（供下一子步 CCD 重建上帧世界顶点）
        impl_->prev_transform_cache[data.transform_handle] = tx_w->compute_matrix();

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

    }

    // --- 阶段 6b：统一地板碰撞 ---
    // 规则：
    //   - Phantom / Static：完全跳过（Static 仅参与物体间碰撞）。
    //   - Dynamic / Kinematic（含带骨骼/不带骨骼）：产生位移 + 速度响应。
    //   - 带骨骼物体额外入队 IK target 更新。
    // 底面取法：
    //   - 带骨骼：遍历 skinned_collision_cache 世界空间顶点取最低 Y（精确姿态）。
    //   - 不带骨骼：复用 Phase 3 预测 AABB min_world.y（与积分量一致）。
    // Kinematic 无速度积分，但写位移（把骨骼/Kinematic 体推出地板）。
    for (std::size_t i = 0; i < mechanics_data.size(); ++i) {
        if (impl_->shutdown_requested.load(std::memory_order_acquire)) return;
        const auto& data = mechanics_data[i];
        std::uintptr_t h = data.handle;

        const auto fp_it = frame_params.find(h);
        if (fp_it == frame_params.end()) continue;
        const BodyType btype = fp_it->second.body_type;

        // Phantom / Static 完全跳过地板
        if (btype == BodyType::Phantom || btype == BodyType::Static) continue;

        // Dynamic 休眠体：先检查是否因地板降低而需唤醒，再跳过
        if (btype == BodyType::Dynamic && impl_->body(h).sleeping) {
            if (data.min_world.y > floor_y + floor_eps) {
                impl_->body(h).sleeping = false;
                impl_->body(h).sleep_timer = 0.0f;
            } else {
                continue;
            }
        }

        // 确定底面高度
        float object_bottom_y = data.min_world.y;  // 默认：非蒙皮用 AABB

        if (data.is_skinned) {
            // 蒙皮：遍历碰撞网格世界顶点取最低 Y（已为世界空间）
            auto sc_it = impl_->skinned_collision_cache.find(data.geom_handle);
            if (sc_it != impl_->skinned_collision_cache.end() && !sc_it->second.vertices.empty()) {
                // skinned_collision_cache 中存储的是模型空间顶点，需经变换矩阵投影到世界 Y
                auto tx_r = transform_storage.try_acquire_read(data.transform_handle);
                if (!tx_r) continue;
                const ktm::fmat4x4 wm = tx_r->compute_matrix();
                float lowest = std::numeric_limits<float>::max();
                for (const auto& lv : sc_it->second.vertices) {
                    const float wy = wm[0][1]*lv.x + wm[1][1]*lv.y + wm[2][1]*lv.z + wm[3][1];
                    if (wy < lowest) lowest = wy;
                }
                object_bottom_y = lowest;
            }
        } else {
            // Dynamic 非蒙皮：补偿 Phase 5 位置校正量
            auto corr_it2 = position_correction.find(h);
            if (corr_it2 != position_correction.end() && !(impl_->body(h).linear_lock & kLockAxisY))
                object_bottom_y += corr_it2->second.y;
        }

        if (object_bottom_y >= floor_y + floor_eps) continue;  // 不接触地板

        // 取写锁写回位移（Kinematic 也写）
        auto tx_w = transform_storage.try_acquire_write(data.transform_handle);
        if (!tx_w) continue;

        const uint8_t lin_lock = impl_->body(h).linear_lock;

        // 消穿：把物体推到 floor_y + floor_eps
        if (!(lin_lock & kLockAxisY))
            tx_w->position.y += (floor_y + floor_eps) - object_bottom_y;

        // 速度响应：完整冲量力学（含角速度耦合和接触点速度，蒙皮/非蒙皮通用）
        {
            const float floor_mass = frame_params[h].mass;
            const float inv_mass_floor = (floor_mass > 0.001f) ? 1.0f / floor_mass : 0.0f;
            auto& b = impl_->body(h);

            // 接触点 XZ 偏移：蒙皮物体用最低顶点实际世界 XZ（已持有 tx_w 写锁，直接用）；
            // 非蒙皮用 AABB，底面中心与质心 XZ 重合，偏移始终为 0
            float r_cx = 0.0f, r_cz = 0.0f;
            if (data.is_skinned) {
                auto sc_it2 = impl_->skinned_collision_cache.find(data.geom_handle);
                if (sc_it2 != impl_->skinned_collision_cache.end() && !sc_it2->second.vertices.empty()) {
                    const ktm::fmat4x4 wm2 = tx_w->compute_matrix();
                    float lowest_y2 = std::numeric_limits<float>::max();
                    for (const auto& lv2 : sc_it2->second.vertices) {
                        const float wy2 = wm2[0][1]*lv2.x + wm2[1][1]*lv2.y + wm2[2][1]*lv2.z + wm2[3][1];
                        if (wy2 < lowest_y2) {
                            lowest_y2 = wy2;
                            r_cx = wm2[0][0]*lv2.x + wm2[1][0]*lv2.y + wm2[2][0]*lv2.z + wm2[3][0] - data.center_world.x;
                            r_cz = wm2[0][2]*lv2.x + wm2[1][2]*lv2.y + wm2[2][2]*lv2.z + wm2[3][2] - data.center_world.z;
                        }
                    }
                }
            }
            // 接触点 r：质心到底部接触点的向量（接触点在 floor_y + floor_eps 处）
            const ktm::fvec3 r_c = make_fvec3(
                r_cx,
                (floor_y + floor_eps) - data.center_world.y,
                r_cz);

            // 接触点实际速度 = 线速度 + ω × r（含旋转贡献，这是 XZ 不动 bug 的根因）
            const ktm::fvec3 v_cp = velocity_at_point_world(b.velocity, b.angular_velocity, r_c);

            // 提升作用域：弹跳时实际法向冲量远大于 m·g·dt，切向 Coulomb 上限应基于实测值
            float jn_applied = 0.0f;

            // --- 法向冲量（Y 轴），含转动惯量 ---
            if (!(lin_lock & kLockAxisY)) {
                const ktm::fvec3 floor_n = make_fvec3(0.0f, 1.0f, 0.0f);
                const ktm::fvec3 rxn = ktm::cross(r_c, floor_n);
                const float ang_n = ktm::dot(
                    rxn, world_inertia_inv_apply(data.rot_body_to_world, data.inertia_inv_body, rxn));
                const float denom_n = inv_mass_floor + ang_n + 1e-8f;
                const float v_n = v_cp.y;

                if (v_n < -low_vel_threshold) {
                    const float jn = -(1.0f + floor_restitution) * v_n / denom_n;
                    jn_applied = jn;
                    b.velocity.y += jn * inv_mass_floor;
                    const ktm::fvec3 dw_n = world_inertia_inv_apply(
                        data.rot_body_to_world, data.inertia_inv_body,
                        ktm::cross(r_c, make_fvec3(0.0f, jn, 0.0f)));
                    if (!(b.angular_lock & kLockAxisX)) b.angular_velocity.x += dw_n.x;
                    if (!(b.angular_lock & kLockAxisY)) b.angular_velocity.y += dw_n.y;
                    if (!(b.angular_lock & kLockAxisZ)) b.angular_velocity.z += dw_n.z;
                    b.sleep_timer = 0.0f;
                } else {
                    b.velocity.y = (std::abs(v_n) < zero_vel_threshold) ? 0.0f : v_n * 0.15f;
                }
            }

            // --- 切向摩擦冲量（XZ），基于接触点切向速度（含 ω × r 贡献） ---
            const float vt_x = v_cp.x;
            const float vt_z = v_cp.z;
            const float vt_len = std::sqrt(vt_x * vt_x + vt_z * vt_z);
            if (vt_len > 1e-6f) {
                const ktm::fvec3 tdir = make_fvec3(vt_x / vt_len, 0.0f, vt_z / vt_len);
                const ktm::fvec3 rxt = ktm::cross(r_c, tdir);
                const float ang_t = ktm::dot(
                    rxt, world_inertia_inv_apply(data.rot_body_to_world, data.inertia_inv_body, rxt));
                const float denom_t = inv_mass_floor + ang_t + 1e-8f;

                const float jt_free = -vt_len / denom_t;
                const float eff_friction = (vt_len < 0.02f) ? static_friction_coeff : friction_coeff;
                // Coulomb 上限：弹跳时用实测法向冲量（高于 m·g·dt）；稳态接触 jn_applied=0 时 fallback
                const float jt_cap = eff_friction * std::max(jn_applied,
                                         floor_mass * std::abs(gravity.y) * fixed_dt);
                const float jt = std::max(-jt_cap, std::min(jt_cap, jt_free));

                if (!(lin_lock & kLockAxisX)) b.velocity.x += tdir.x * jt * inv_mass_floor;
                if (!(lin_lock & kLockAxisZ)) b.velocity.z += tdir.z * jt * inv_mass_floor;

                // 摩擦冲量同时产生角动量变化（Δω = I⁻¹ · (r × J_t)）
                const ktm::fvec3 dw_t = world_inertia_inv_apply(
                    data.rot_body_to_world, data.inertia_inv_body,
                    ktm::cross(r_c, make_fvec3(tdir.x * jt, 0.0f, tdir.z * jt)));
                if (!(b.angular_lock & kLockAxisX)) b.angular_velocity.x += dw_t.x;
                if (!(b.angular_lock & kLockAxisY)) b.angular_velocity.y += dw_t.y;
                if (!(b.angular_lock & kLockAxisZ)) b.angular_velocity.z += dw_t.z;
            }
        }

        // 蒙皮额外：IK target 入队（最低顶点所在骨骼）
        if (data.is_skinned) {
            auto sc_it = impl_->skinned_collision_cache.find(data.geom_handle);
            if (sc_it != impl_->skinned_collision_cache.end() && !sc_it->second.triangles.empty()) {
                const auto& sc = sc_it->second;
                const ktm::fmat4x4 wm = tx_w->compute_matrix();

                // 找最低世界 Y 顶点
                float lowest_vy = std::numeric_limits<float>::max();
                int   lowest_vi = -1;
                for (std::size_t vi = 0; vi < sc.vertices.size(); ++vi) {
                    const auto& lv = sc.vertices[vi];
                    const float wy = wm[0][1]*lv.x + wm[1][1]*lv.y + wm[2][1]*lv.z + wm[3][1];
                    if (wy < lowest_vy) { lowest_vy = wy; lowest_vi = static_cast<int>(vi); }
                }
                if (lowest_vi >= 0 && data.geom_handle != 0) {
                    const auto& lv = sc.vertices[static_cast<std::size_t>(lowest_vi)];
                    const ktm::fvec3 contact_world = make_fvec3(
                        wm[0][0]*lv.x + wm[1][0]*lv.y + wm[2][0]*lv.z + wm[3][0],
                        floor_y,
                        wm[0][2]*lv.x + wm[1][2]*lv.y + wm[2][2]*lv.z + wm[3][2]);

                    // 找最近三角形的主导骨骼
                    int best_bone = -1;
                    if (!sc.triangle_bone_ids.empty()) {
                        float best_d2 = std::numeric_limits<float>::max();
                        for (std::size_t ti = 0; ti < sc.triangles.size(); ++ti) {
                            for (int c = 0; c < 3; ++c) {
                                const std::uint32_t vi2 = resolve_vertex(sc, static_cast<std::uint32_t>(ti), c);
                                if (vi2 >= sc.vertices.size()) continue;
                                const auto& v = sc.vertices[vi2];
                                const float dx = v.x - lv.x, dy = v.y - lv.y, dz = v.z - lv.z;
                                const float d2 = dx*dx + dy*dy + dz*dz;
                                if (d2 < best_d2 && ti < sc.triangle_bone_ids.size()) {
                                    best_d2 = d2;
                                    best_bone = sc.triangle_bone_ids[ti];
                                }
                            }
                        }
                    }
                    if (best_bone >= 0)
                        impl_->deferred_ik_target_updates.push_back(
                            {data.geom_handle, data.transform_handle, best_bone, contact_world});
                }
            }
        }
    }

    for (std::uintptr_t h : mechanics_handles) {
        if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
            return;
        }
        if (impl_->body(h).sleeping) continue;

        // E4：Dynamic 体允许休眠；Static/Kinematic 不参与速度积分，不需要休眠机制。
        const bool is_dynamic = (frame_params.count(h) && frame_params.at(h).body_type == BodyType::Dynamic);
        const bool can_sleep = is_dynamic;

        if (can_sleep) {
            const auto& v = impl_->body(h).velocity;
            const auto& av = impl_->body(h).angular_velocity;
            const float v_sq = v.x*v.x + v.y*v.y + v.z*v.z;
            const float av_sq = av.x*av.x + av.y*av.y + av.z*av.z;

            if (v_sq < sleep_threshold_sq && av_sq < sleep_threshold_sq) {
                impl_->body(h).sleep_timer += fixed_dt;
                if (impl_->body(h).sleep_timer >= sleep_time_needed) {
                    impl_->body(h).sleeping = true;
                    impl_->body(h).velocity = make_fvec3(0.0f, 0.0f, 0.0f);
                    impl_->body(h).angular_velocity = make_fvec3(0.0f, 0.0f, 0.0f);
                }
            } else {
                impl_->body(h).sleep_timer = 0.0f;
            }
        } else {
            impl_->body(h).sleep_timer = 0.0f;  // 重置计时，永不进入休眠
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

    // S5：IK target 更新已移到 update_skinned_geometry 末尾执行，
    // 使本帧物理产生的接触能在同帧蒙皮时被 CCD 消费，消除一帧延迟。
    // deferred_ik_target_updates 由此处入队，由 update_skinned_geometry 在
    // 下一次蒙皮开始前（同帧或下帧首）统一 flush。

    // 清理无效句柄的缓存
    std::unordered_set<std::uintptr_t> alive_handles(mechanics_handles.begin(), mechanics_handles.end());

    // 收集存活的 geom_handle 和 transform_handle，用于清理 geom/transform 键的缓存
    std::unordered_set<std::uintptr_t> alive_geom_handles;
    std::unordered_set<std::uintptr_t> alive_transform_handles;
    for (const auto& data : mechanics_data) {
        if (data.geom_handle) alive_geom_handles.insert(data.geom_handle);
        if (data.transform_handle) alive_transform_handles.insert(data.transform_handle);
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
    for (auto it = impl_->prev_skinned_verts_cache.begin(); it != impl_->prev_skinned_verts_cache.end();) {
        if (!alive_geom_handles.count(it->first)) it = impl_->prev_skinned_verts_cache.erase(it);
        else ++it;
    }
    for (auto it = impl_->skinned_octree_cache.begin(); it != impl_->skinned_octree_cache.end();) {
        if (!alive_geom_handles.count(it->first)) it = impl_->skinned_octree_cache.erase(it);
        else ++it;
    }
    for (auto it = impl_->prev_world_verts_cache.begin(); it != impl_->prev_world_verts_cache.end();) {
        if (!alive_handles.count(it->first)) it = impl_->prev_world_verts_cache.erase(it);
        else ++it;
    }
    for (auto it = impl_->prev_transform_cache.begin(); it != impl_->prev_transform_cache.end();) {
        if (!alive_transform_handles.count(it->first)) it = impl_->prev_transform_cache.erase(it);
        else ++it;
    }

    // Bug-8：triangle_octree_cache 按 model_id 缓存，没有随物体消失的清理机制
    // 收集本帧出现的所有 model_id，清理不再使用的静态 octree（防 model 卸载后内存泄漏）
    {
        std::unordered_set<std::uint64_t> alive_model_ids;
        for (const auto& data : mechanics_data) {
            if (data.model_id != 0) alive_model_ids.insert(data.model_id);
        }
        for (auto it = impl_->triangle_octree_cache.begin(); it != impl_->triangle_octree_cache.end();) {
            if (!alive_model_ids.count(it->first)) it = impl_->triangle_octree_cache.erase(it);
            else ++it;
        }
        // static_world_verts_cache 与 triangle_octree_cache 同步清理
        for (auto it = impl_->static_world_verts_cache.begin(); it != impl_->static_world_verts_cache.end();) {
            if (!alive_model_ids.count(it->first)) it = impl_->static_world_verts_cache.erase(it);
            else ++it;
        }
        // collision_mesh_cache 和 static_triangle_index_cache 同样按 model_id 缓存，一并清理
        for (auto it = impl_->collision_mesh_cache.begin(); it != impl_->collision_mesh_cache.end();) {
            if (!alive_model_ids.count(it->first)) it = impl_->collision_mesh_cache.erase(it);
            else ++it;
        }
        for (auto it = impl_->static_triangle_index_cache.begin(); it != impl_->static_triangle_index_cache.end();) {
            if (!alive_model_ids.count(it->first)) it = impl_->static_triangle_index_cache.erase(it);
            else ++it;
        }
        for (auto it = impl_->static_triangle_bone_cache.begin(); it != impl_->static_triangle_bone_cache.end();) {
            if (!alive_model_ids.count(it->first)) it = impl_->static_triangle_bone_cache.erase(it);
            else ++it;
        }
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
// update_skinned_geometry（P2，自 GeometrySystem 迁入）
// 功能：每真实帧对蒙皮 actor 做 CPU 线性混合蒙皮（LBS），把结果重传到 GPU 顶点缓冲。
// 调用时机：MechanicsSystem::update() 的固定步进循环之前（Phase 1 调整）。
//           先蒙皮生成本帧正确 AABB 和蒙皮顶点，物理随后消费同帧数据，消除旧版
//           一帧延迟。蒙皮模型即使未开物理也应自动循环播放。
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
void MechanicsSystem::update_skinned_geometry(float dt) {
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

    // Phase 3：IK target 衰减（contact_driven 链每帧把 weight 减小，碰撞结束后平滑归零）
    // 在收集 geom_handles 之前处理，避免在遍历 storage 时持锁写 ik_chains。
    {
        auto& geom_st = hub.geometry_storage();
        std::vector<std::uintptr_t> all_geom_handles;
        for (auto it = geom_st.cbegin(); it != geom_st.cend(); ++it) {
            all_geom_handles.push_back(reinterpret_cast<std::uintptr_t>(&*it));
        }
        for (auto gh : all_geom_handles) {
            auto gw = geom_st.try_acquire_write(gh);
            if (!gw) continue;
            for (auto& chain : gw->ik_chains) {
                if (!chain.contact_driven || !chain.enabled) continue;
                chain.weight -= chain.contact_weight_decay * dt;
                if (chain.weight <= 0.0f) {
                    chain.weight = 0.0f;
                    chain.enabled = false;
                }
            }
        }
    }

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
        std::uintptr_t fp_transform_handle = 0;   // FootPlant probe 用：model→world 矩阵来源
        {
            auto geom_write = geom_storage.try_acquire_write(geom_handle);
            if (!geom_write) continue;
            geom_write->is_skinned = true;
            geom_write->anim_time = Resource::advance_anim_time(geom_write->anim_time, dt, clip);
            anim_time = geom_write->anim_time;

            // ---- 首帧：建立骨骼名→node_idx 缓存（仅一次，后续 register_*_chain 快速查询）----
            if (geom_write->bone_name_to_node_idx.empty()) {
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

            fp_transform_handle = geom_write->transform_handle;  // FootPlant probe 用
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
        // FootPlant 链（Mode::FootPlant）在 CCD 求解之前先做地面 probe，直接写入 chain.target
        // 和 chain.weight，然后再走普通的 has_active_ik 路径——同帧 CCD 立即消费 probe 结果。
        // 其他 contact_driven 链（Contact 模式）靠 deferred_ik_target_updates flush 驱动，不在此处处理。
        //
        // FootPlant probe 流程：
        //   1. 从 working_locals FK 计算末端骨骼世界位置 P（pre-IK 姿态）
        //   2. 对所有 Static 物体的 triangle_octree_cache 做 query_ground_height
        //   3. 找到地面高度 → 世界坐标 → 模型空间，写 chain.target / chain.weight
        //   4. 未找到地面（脚在空中超 max_drop）→ 降 weight，降到 0 则 disabled
        //
        // 读取 model→world 矩阵（FootPlant probe 需要把骨骼位置转世界，再把地面高度转模型空间）
        bool has_foot_plant_chains = false;
        for (const auto& ch : ik_chains) {
            if (ch.mode == Resource::IkChain::Mode::FootPlant) { has_foot_plant_chains = true; break; }
        }

        // working_locals 无论是否有活跃 IK 都提前采样一次，供 FootPlant probe 读取 pre-IK 骨骼位置。
        // 若没有 FootPlant 链则按原路径懒采样，避免多余开销。
        std::vector<std::array<float, 16>> working_locals_for_probe;
        if (has_foot_plant_chains && fp_transform_handle != 0) {
            Resource::sample_pose_locals(skeleton, clip, anim_time, working_locals_for_probe);

            // 读 model→world 矩阵（brief 读锁）
            ktm::fmat4x4 model_to_world{};
            ktm::fmat4x4 world_to_model{};
            bool have_transform = false;
            {
                auto tx_r = hub.model_transform_storage().try_acquire_read(fp_transform_handle);
                if (tx_r) {
                    model_to_world = tx_r->compute_matrix();
                    world_to_model = ktm::inverse(model_to_world);
                    have_transform = true;
                }
            }

            if (have_transform) {
                constexpr float k_fp_max_drop = 0.5f;  // 向下探测最大距离（米）

                for (auto& ch : ik_chains) {
                    if (ch.mode != Resource::IkChain::Mode::FootPlant) continue;
                    if (ch.end_node < 0 || ch.end_node >= static_cast<int>(skeleton.nodes.size())) continue;
                    if (working_locals_for_probe.size() != skeleton.nodes.size()) continue;

                    // 从 root 到 end_node 累乘 local 得 global（模型空间）
                    // 复用 compute_global_of 的逻辑（沿 parent 链从根往下累乘）
                    std::vector<int> path_to_root;
                    {
                        int cur = ch.end_node;
                        const int max_depth = static_cast<int>(skeleton.nodes.size()) + 1;
                        int guard = 0;
                        while (cur >= 0 && cur < static_cast<int>(skeleton.nodes.size()) && guard++ < max_depth) {
                            path_to_root.push_back(cur);
                            cur = skeleton.nodes[static_cast<std::size_t>(cur)].parent;
                        }
                    }
                    // 从根往下累乘
                    std::array<float, 16> g{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
                    for (auto it = path_to_root.rbegin(); it != path_to_root.rend(); ++it) {
                        const auto& lm = working_locals_for_probe[static_cast<std::size_t>(*it)];
                        // 列主序 mat4 乘法：g = g * lm
                        std::array<float, 16> tmp{};
                        for (int col = 0; col < 4; ++col)
                            for (int row = 0; row < 4; ++row)
                                for (int k = 0; k < 4; ++k)
                                    tmp[col*4+row] += g[k*4+row] * lm[col*4+k];
                        g = tmp;
                    }
                    // g[12..14] = 末端骨骼模型空间位置
                    const float bone_mx = g[12], bone_my = g[13], bone_mz = g[14];

                    // 模型空间 → 世界空间（bone_world = model_to_world * (bone_m, 1)）
                    const float bwx = model_to_world[0][0]*bone_mx + model_to_world[1][0]*bone_my
                                    + model_to_world[2][0]*bone_mz + model_to_world[3][0];
                    const float bwy = model_to_world[0][1]*bone_mx + model_to_world[1][1]*bone_my
                                    + model_to_world[2][1]*bone_mz + model_to_world[3][1];
                    const float bwz = model_to_world[0][2]*bone_mx + model_to_world[1][2]*bone_my
                                    + model_to_world[2][2]*bone_mz + model_to_world[3][2];

                    const ktm::fvec3 probe = make_fvec3(bwx, bwy, bwz);

                    // 遍历所有 Static 物体的 octree 做地面 probe
                    float best_ground_y = probe.y - k_fp_max_drop - 1.0f;
                    bool ground_found = false;
                    for (const auto& [mid, oct] : impl_->triangle_octree_cache) {
                        auto wvit = impl_->static_world_verts_cache.find(mid);
                        if (wvit == impl_->static_world_verts_cache.end()) continue;
                        auto cmit = impl_->collision_mesh_cache.find(mid);
                        if (cmit == impl_->collision_mesh_cache.end()) continue;
                        float gy = 0.0f;
                        if (oct.query_ground_height(wvit->second, cmit->second, probe, k_fp_max_drop, gy)) {
                            if (!ground_found || gy > best_ground_y) {
                                best_ground_y = gy;
                                ground_found = true;
                            }
                        }
                    }

                    if (ground_found) {
                        // 地面世界坐标 → 模型空间
                        const float gx = probe.x, gz = probe.z;
                        const float tmx = world_to_model[0][0]*gx + world_to_model[1][0]*best_ground_y
                                        + world_to_model[2][0]*gz + world_to_model[3][0];
                        const float tmy = world_to_model[0][1]*gx + world_to_model[1][1]*best_ground_y
                                        + world_to_model[2][1]*gz + world_to_model[3][1];
                        const float tmz = world_to_model[0][2]*gx + world_to_model[1][2]*best_ground_y
                                        + world_to_model[2][2]*gz + world_to_model[3][2];
                        ch.target  = {tmx, tmy, tmz};
                        ch.enabled = true;
                        const float rise_rate = (ch.contact_weight_rise > 0.0f)
                            ? ch.contact_weight_rise : ch.contact_weight_decay;
                        ch.weight = std::min(ch.weight + rise_rate * dt, 1.0f);
                    } else {
                        // 脚在空中：降 weight，降到 0 则关闭
                        ch.weight -= ch.contact_weight_decay * dt;
                        if (ch.weight <= 0.0f) { ch.weight = 0.0f; ch.enabled = false; }
                    }
                }
            }
        }

        // FootPlant 链的 target/weight 已就绪，写回 GeometryDevice（brief 写锁）
        // 同时把更新后的 ik_chains 拷贝一份供下方 CCD 使用。
        if (has_foot_plant_chains) {
            auto gw = geom_storage.try_acquire_write(geom_handle);
            if (gw) {
                for (std::size_t ci = 0; ci < ik_chains.size() && ci < gw->ik_chains.size(); ++ci) {
                    if (ik_chains[ci].mode == Resource::IkChain::Mode::FootPlant) {
                        gw->ik_chains[ci].target  = ik_chains[ci].target;
                        gw->ik_chains[ci].weight  = ik_chains[ci].weight;
                        gw->ik_chains[ci].enabled = ik_chains[ci].enabled;
                    }
                }
                ik_chains = gw->ik_chains;  // 刷新拷贝（同步非 FootPlant 链可能在别处变化）
            }
        }

        std::vector<std::array<float, 16>> finals;
        bool has_active_ik = false;
        for (const auto& ch : ik_chains) {
            if (ch.enabled) { has_active_ik = true; break; }
        }
        if (has_active_ik) {
            // working_locals：每条链解完后把结果写回，供下一条链感知前链修改（Bug-2 修复）。
            // 解完所有链后直接作为 precomputed_locals 传给 compute_pose，避免重复采样（Perf-1 修复）。
            // FootPlant 链已经在上方通过 working_locals_for_probe 采样过，若存在 FootPlant 链直接复用；
            // 否则才重新采样（避免二次采样）。
            std::vector<std::array<float, 16>> working_locals;
            if (has_foot_plant_chains && !working_locals_for_probe.empty()) {
                working_locals = std::move(working_locals_for_probe);
            } else {
                Resource::sample_pose_locals(skeleton, clip, anim_time, working_locals);
            }

            for (const auto& ch : ik_chains) {
                if (!ch.enabled) continue;
                std::unordered_map<int, std::array<float, 16>> chain_ov;
                Resource::solve_ccd(skeleton, ch, working_locals, chain_ov);
                // 把本链改写的关节写回 working_locals，下一条链从更新后的状态出发。
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
        // 同时更新 skinned_collision_cache：把本帧蒙皮顶点（模型空间）装入物理系统的
        // 每帧实例化碰撞网格。索引和 triangle_bone_ids 从 static_triangle_index_cache /
        // static_triangle_bone_cache 复用（首帧由 ensure_collision_mesh 顺带解析好），
        // 只刷新顶点坐标，避免每帧重解 Scene。
        {
            auto geom_write = geom_storage.try_acquire_write(geom_handle);
            if (geom_write) {
                geom_write->skinned_cpu_vertices = std::move(skinned_blobs);
                if (aabb_any) {
                    geom_write->skinned_aabb_min = ktm::fvec3{aabb_min_x, aabb_min_y, aabb_min_z};
                    geom_write->skinned_aabb_max = ktm::fvec3{aabb_max_x, aabb_max_y, aabb_max_z};
                    geom_write->skinned_aabb_valid = true;
                }

                // ---- 更新蒙皮碰撞网格（Phase 2）----
                // 从 static_triangle_index_cache 取索引（首帧需先确保已解析）
                bool have_static = impl_->static_triangle_index_cache.count(model_id) > 0;
                if (!have_static && model_id != 0) {
                    // 首帧：触发 ensure_collision_mesh 解析索引和骨骼映射，存入 static 缓存
                    // 锁序注意（A3）：此处位于 geom 写锁内，且本循环体开头取得的 Scene 读锁
                    // scene_read 仍然持有。必须复用它（传 const Scene& 的重载），不能调用自行
                    // acquire_read 的版本：那会对同一 ResourceEntry 的 std::shared_mutex 再加一次
                    // shared 锁——递归 shared 锁是 UB；MSVC 的 SRWLOCK 写者优先，若 Geometry 线程
                    // 恰在 acquire_write<Scene>（LOD 驻留回收）上等待，第二次 shared 会排到写者
                    // 之后，而写者又在等本线程已持有的第一次 shared，双方互等挂死。
                    MechanicsInternal::ensure_collision_mesh(
                        model_id,
                        scene,
                        impl_->collision_mesh_cache,
                        &impl_->static_triangle_index_cache,
                        &impl_->static_triangle_bone_cache);
                    have_static = impl_->static_triangle_index_cache.count(model_id) > 0;
                }

                if (have_static && !geom_write->skinned_cpu_vertices.empty()) {
                    const auto& static_tris  = impl_->static_triangle_index_cache.at(model_id);
                    const auto& static_bones = impl_->static_triangle_bone_cache.count(model_id)
                                                ? impl_->static_triangle_bone_cache.at(model_id)
                                                : std::vector<int>{};

                    MechanicsInternal::CollisionMesh& sc = impl_->skinned_collision_cache[geom_handle];
                    sc.triangles         = static_tris;
                    sc.triangle_bone_ids = static_bones;

                    // 从静态绑定姿态网格复制子网格元数据和顶点基底
                    // 非蒙皮子网格保留绑定姿态坐标；蒙皮子网格下方用 blob 覆盖
                    auto static_it = impl_->collision_mesh_cache.find(model_id);
                    if (static_it != impl_->collision_mesh_cache.end() &&
                        !static_it->second.vertices.empty()) {
                        sc.vertices  = static_it->second.vertices;  // 复制绑定姿态顶点
                        sc.submeshes = static_it->second.submeshes;
                        sc.total_vertex_count = static_it->second.total_vertex_count;
                    } else {
                        sc.vertices.clear();
                    }

                    // 用蒙皮 blob 覆盖各蒙皮子网格的顶点
                    float min_y = sc.vertices.empty()
                                      ? 0.0f
                                      : std::numeric_limits<float>::max();
                    if (!sc.vertices.empty()) {
                        for (const auto& v : sc.vertices) min_y = std::min(min_y, v.y);
                    }

                    for (std::size_t mi = 0; mi < geom_write->skinned_cpu_vertices.size(); ++mi) {
                        const auto& blob = geom_write->skinned_cpu_vertices[mi];
                        if (blob.empty()) continue;  // 非蒙皮子网格：保留绑定姿态（已复制）

                        // 找该子网格在扁平顶点数组中的基址
                        std::uint32_t v_base = 0;
                        if (static_it != impl_->collision_mesh_cache.end() &&
                            mi < static_it->second.submeshes.size()) {
                            v_base = static_it->second.submeshes[mi].vertex_base;
                        }

                        constexpr std::size_t kVertexStride = sizeof(Corona::Resource::Vertex);
                        const std::size_t vert_count = blob.size() / kVertexStride;
                        const auto* vptr = reinterpret_cast<const Corona::Resource::Vertex*>(blob.data());
                        for (std::size_t vi = 0; vi < vert_count; ++vi) {
                            const std::uint32_t flat_idx = v_base + static_cast<std::uint32_t>(vi);
                            if (flat_idx >= sc.vertices.size()) break;  // 防越界
                            sc.vertices[flat_idx].x = vptr[vi].position[0];
                            sc.vertices[flat_idx].y = vptr[vi].position[1];
                            sc.vertices[flat_idx].z = vptr[vi].position[2];
                            min_y = std::min(min_y, sc.vertices[flat_idx].y);
                        }
                    }
                    sc.min_local_y = (sc.vertices.empty() ? 0.0f : min_y);

                    // B3：用 sc.vertices（含全部段，包括非蒙皮段绑定姿态坐标）
                    // 重新计算完整 skinned_aabb，覆盖上方只含蒙皮段的值
                    if (!sc.vertices.empty()) {
                        float full_min_x = std::numeric_limits<float>::max();
                        float full_min_y = std::numeric_limits<float>::max();
                        float full_min_z = std::numeric_limits<float>::max();
                        float full_max_x = std::numeric_limits<float>::lowest();
                        float full_max_y = std::numeric_limits<float>::lowest();
                        float full_max_z = std::numeric_limits<float>::lowest();
                        for (const auto& v : sc.vertices) {
                            full_min_x = std::min(full_min_x, v.x);
                            full_min_y = std::min(full_min_y, v.y);
                            full_min_z = std::min(full_min_z, v.z);
                            full_max_x = std::max(full_max_x, v.x);
                            full_max_y = std::max(full_max_y, v.y);
                            full_max_z = std::max(full_max_z, v.z);
                        }
                        geom_write->skinned_aabb_min = ktm::fvec3{full_min_x, full_min_y, full_min_z};
                        geom_write->skinned_aabb_max = ktm::fvec3{full_max_x, full_max_y, full_max_z};
                        geom_write->skinned_aabb_valid = true;
                    }

                    // E3：更新上一帧顶点缓存（保存本帧蒙皮世界顶点，下一子步用于表面速度插值）
                    impl_->prev_skinned_verts_cache[geom_handle] = sc.vertices;

                    // 阶段 4 L4：建/维护蒙皮 octree（拓扑固定，只在首帧 build，后续每帧 refit）
                    // build 用模型空间顶点建拓扑（首帧接受坐标系不精确，refit 第一帧立即修正）
                    auto& soct = impl_->skinned_octree_cache;
                    auto soct_it = soct.find(geom_handle);
                    if (soct_it == soct.end()) {
                        MechanicsInternal::TriangleOctree tree;
                        tree.build(sc.vertices, sc);
                        soct[geom_handle] = std::move(tree);
                    }
                }
            }
        }
    }

    // S5：在蒙皮结束后立即 flush 上一物理步产生的 IK target 更新，
    // 使同帧物理接触能在本帧 CCD 求解中被消费，消除一帧延迟。
    // 注意调用顺序：update_skinned_geometry 在 update_physics 之前执行，
    // 所以此处 flush 的是上一个物理步的 deferred 队列（上帧接触），
    // 比原先在 update_physics 末尾 flush（要等下下帧蒙皮）早一整帧。
    if (!impl_->deferred_ik_target_updates.empty()) {
        auto& geom_st2    = SharedDataHub::instance().geometry_storage();
        auto& transform_st = SharedDataHub::instance().model_transform_storage();

        for (const auto& upd : impl_->deferred_ik_target_updates) {
            if (impl_->shutdown_requested.load(std::memory_order_acquire)) break;

            ktm::fmat4x4 inv_m{};
            bool have_inv = false;
            {
                auto tx = transform_st.try_acquire_read(upd.transform_handle);
                if (tx) {
                    inv_m    = ktm::inverse(tx->compute_matrix());
                    have_inv = true;
                }
            }
            if (!have_inv) continue;

            const auto& wp = upd.contact_world;
            const float mx = inv_m[0][0]*wp.x + inv_m[1][0]*wp.y + inv_m[2][0]*wp.z + inv_m[3][0];
            const float my = inv_m[0][1]*wp.x + inv_m[1][1]*wp.y + inv_m[2][1]*wp.z + inv_m[3][1];
            const float mz = inv_m[0][2]*wp.x + inv_m[1][2]*wp.y + inv_m[2][2]*wp.z + inv_m[3][2];

            auto geom_w = geom_st2.try_acquire_write(upd.geom_handle);
            if (!geom_w) continue;
            for (auto& chain : geom_w->ik_chains) {
                if (!chain.contact_driven) continue;
                if (chain.end_node != upd.node_idx) continue;
                chain.target = {mx, my, mz};
                // 上升速率：contact_weight_rise>0 时用之，否则与衰减速率一致（向后兼容）
                const float rise_rate = (chain.contact_weight_rise > 0.0f)
                    ? chain.contact_weight_rise
                    : chain.contact_weight_decay;
                chain.weight = std::min(chain.weight + rise_rate * dt, 1.0f);
                chain.enabled = true;
                break;
            }
        }
        impl_->deferred_ik_target_updates.clear();
    }
}

// ============================================================================
// register_foot_plant_chain / unregister_foot_plant_chain
// ============================================================================

namespace {

// 内部辅助：向 GeometryDevice 写入或更新一条 FootPlant 链。调用方必须已持有写锁。
bool apply_foot_plant_chain(Corona::GeometryDevice& gw, int node_idx,
                            int chain_length, float damping, float weight_decay) {
    using namespace Corona::Resource;
    for (auto& chain : gw.ik_chains) {
        if (chain.end_node == node_idx &&
            chain.mode == IkChain::Mode::FootPlant) {
            chain.chain_length         = chain_length;
            chain.damping              = damping;
            chain.contact_weight_decay = weight_decay;
            return true;
        }
    }
    IkChain chain;
    chain.end_node              = node_idx;
    chain.chain_length          = chain_length;
    chain.mode                  = IkChain::Mode::FootPlant;
    chain.contact_driven        = true;
    chain.enabled               = false;
    chain.weight                = 0.0f;
    chain.damping               = damping;
    chain.max_iterations        = 10;
    chain.tolerance             = 1e-3f;
    chain.contact_normal_offset = 0.0f;  // 目标贴地面，不偏移
    chain.contact_weight_rise   = 8.0f;  // 约 0.13s（8 帧）升满，落地立即贴地
    chain.contact_weight_decay  = weight_decay;
    gw.ik_chains.push_back(chain);
    return true;
}

}  // namespace

bool MechanicsSystem::register_foot_plant_chain(
    std::uintptr_t geom_handle,
    std::string_view bone_name,
    int chain_length,
    float damping,
    float weight_decay) {

    auto& hub     = SharedDataHub::instance();
    auto& geom_st = hub.geometry_storage();

    // 快速路径：bone_name_to_node_idx 缓存已由首帧 update_skinned_geometry 建立
    {
        auto gr = geom_st.try_acquire_read(geom_handle);
        if (!gr) return false;
        auto cache_it = gr->bone_name_to_node_idx.find(std::string(bone_name));
        if (cache_it != gr->bone_name_to_node_idx.end()) {
            const int node_idx = cache_it->second;
            auto gw = geom_st.try_acquire_write(geom_handle);
            if (!gw) return false;
            return apply_foot_plant_chain(*gw, node_idx, chain_length, damping, weight_decay);
        }
    }

    // 慢速路径：首帧蒙皮尚未跑过，直接查 Scene（持 ResourceManager 读锁）
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
            node_idx = static_cast<int>(ni);
            break;
        }
    }
    if (node_idx < 0) return false;

    auto gw = geom_st.try_acquire_write(geom_handle);
    if (!gw) return false;
    // 顺便回填缓存，避免下次再走慢速路径
    gw->bone_name_to_node_idx[std::string(bone_name)] = node_idx;
    return apply_foot_plant_chain(*gw, node_idx, chain_length, damping, weight_decay);
}

void MechanicsSystem::unregister_foot_plant_chain(
    std::uintptr_t geom_handle,
    std::string_view bone_name) {

    auto& geom_st = SharedDataHub::instance().geometry_storage();
    auto gw = geom_st.try_acquire_write(geom_handle);
    if (!gw) return;

    auto cache_it = gw->bone_name_to_node_idx.find(std::string(bone_name));
    if (cache_it == gw->bone_name_to_node_idx.end()) return;
    const int node_idx = cache_it->second;

    auto& chains = gw->ik_chains;
    chains.erase(
        std::remove_if(chains.begin(), chains.end(),
            [node_idx](const Resource::IkChain& c) {
                return c.end_node == node_idx &&
                       c.mode == Resource::IkChain::Mode::FootPlant;
            }),
        chains.end());
}

}  // namespace Corona::Systems




