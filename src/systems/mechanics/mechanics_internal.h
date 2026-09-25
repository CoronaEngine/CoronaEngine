#pragma once

#include <corona/events/scene_system_events.h>
#include <corona/resource/resource_manager.h>
#include <corona/systems/geometry/geometry_system.h>
#include <corona/systems/mechanics/mechanics_system.h>
#include <corona/resource/types/scene.h>
#include <corona/shared_data_hub.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ktm/ktm.h>

namespace Corona::Systems::MechanicsInternal {

// 按分量构造 fvec3（result：输出向量）
constexpr ktm::fvec3 make_fvec3(float x, float y, float z) {
    ktm::fvec3 result;  // 返回值缓冲
    result.x = x;       // X
    result.y = y;       // Y
    result.z = z;       // Z
    return result;      // 按值传出
}

// 封装构造四元数
constexpr ktm::fvec4 make_fvec4(float x, float y, float z, float w) {
    ktm::fvec4 result2;  // 返回值缓冲
    result2.x = x;       // X
    result2.y = y;       // Y
    result2.z = z;       // Z
    result2.w = w;
    return result2;  // 按值传出
}

inline ktm::fvec3 vec3_add(const ktm::fvec3& a, const ktm::fvec3& b) {
    return make_fvec3(a.x + b.x, a.y + b.y, a.z + b.z);  // 逐分量加
}
inline ktm::fvec3 vec3_sub(const ktm::fvec3& a, const ktm::fvec3& b) {
    return make_fvec3(a.x - b.x, a.y - b.y, a.z - b.z);  // 逐分量减
}
inline ktm::fvec3 vec3_mul(const ktm::fvec3& v, float s) {
    return make_fvec3(v.x * s, v.y * s, v.z * s);  // 标量乘向量
}

// 检测两个 AABB 是否重叠（世界/局部复用）
inline bool aabb_overlap(const ktm::fvec3& a_min, const ktm::fvec3& a_max,
                         const ktm::fvec3& b_min, const ktm::fvec3& b_max) {
    return (a_min.x <= b_max.x && a_max.x >= b_min.x) &&
           (a_min.y <= b_max.y && a_max.y >= b_min.y) &&
           (a_min.z <= b_max.z && a_max.z >= b_min.z);
}

// local：局部点；返回：同一几何点在世界的坐标
inline ktm::fvec3 transform_local_point_to_world(const Corona::ModelTransform& t, const ktm::fvec3& local) {
    ktm::fmat4x4 M = t.compute_matrix();  // 4×4 TRS
    ktm::fvec4 local_h = make_fvec4(local.x, local.y, local.z, 1.0f);
    // w=1 表示点而非方向
    ktm::fvec4 world_h = M * local_h;                    // 齐次乘法
    return make_fvec3(world_h.x, world_h.y, world_h.z);  // 透视下 w 应为 1，取 xyz 即可
}

// lmin,lmax：局部轴对齐盒；out_*：世界 AABB 及中心
inline void world_aabb_from_local_bounds(const Corona::ModelTransform& t,
                                         const ktm::fvec3& lmin, const ktm::fvec3& lmax,
                                         ktm::fvec3& out_min, ktm::fvec3& out_max, ktm::fvec3& out_center) {
    // 局部 AABB 的 8 个角点：min/max 各分量组合 → 世界空间 → 取包络
    const ktm::fvec3 corners[8] = {
        {lmin.x, lmin.y, lmin.z}, {lmax.x, lmin.y, lmin.z},
        {lmin.x, lmax.y, lmin.z}, {lmax.x, lmax.y, lmin.z},
        {lmin.x, lmin.y, lmax.z}, {lmax.x, lmin.y, lmax.z},
        {lmin.x, lmax.y, lmax.z}, {lmax.x, lmax.y, lmax.z},
    };
    ktm::fvec3 wp0 = transform_local_point_to_world(t, corners[0]);
    out_min = wp0;
    out_max = wp0;
    for (int i = 1; i < 8; ++i) {
        const ktm::fvec3 wp = transform_local_point_to_world(t, corners[i]);
        out_min.x = std::min(out_min.x, wp.x);
        out_min.y = std::min(out_min.y, wp.y);
        out_min.z = std::min(out_min.z, wp.z);
        out_max.x = std::max(out_max.x, wp.x);
        out_max.y = std::max(out_max.y, wp.y);
        out_max.z = std::max(out_max.z, wp.z);
    }
    out_center = make_fvec3(
        (out_min.x + out_max.x) * 0.5f,  // 形心 X
        (out_min.y + out_max.y) * 0.5f,
        (out_min.z + out_max.z) * 0.5f);
}

// 返回世界包络盒在 Y 轴上的最小值（贴地）
inline float world_aabb_min_y(const Corona::ModelTransform& t,
                              const ktm::fvec3& lmin, const ktm::fvec3& lmax) {
    ktm::fvec3 out_min{}, out_max{}, out_center{};  // out_max/center 此处不用
    world_aabb_from_local_bounds(t, lmin, lmax, out_min, out_max, out_center);
    return out_min.y;  // 最低点高度
}

// euler：弧度，顺序 XYZ；返回与 TRS 矩阵一致的单位四元数
inline ktm::fquat quat_from_model_euler(const ktm::fvec3& euler) {
    const ktm::fquat qx = ktm::fquat::from_angle_x(euler.x);  // 绕 X
    const ktm::fquat qy = ktm::fquat::from_angle_y(euler.y);  // 绕 Y
    const ktm::fquat qz = ktm::fquat::from_angle_z(euler.z);  // 绕 Z
    return qz * qy * qx;                                      // 组合旋转
}

// R：旋转矩阵；euler：输出的 XYZ 欧拉（弧度）
// KTM 使用列主序存储 R[col][row]，对 Rz·Ry·Rx 有 R[0][2] = -sin(y)
inline void euler_xyz_from_rot_mat(const ktm::fmat3x3& R, ktm::fvec3& euler) {
    const float sy = std::clamp(-R[0][2], -1.0f, 1.0f);  // sin(y)，clamp 防 NaN

    const float pi = 3.1415926535f;  // π

    if (sy > 0.9999f) {                          // 俯仰近 +90°，万向节锁
        euler.x = 0;                             // 俯仰锁定下 x 置 0
        euler.y = pi * 0.5f;                     // y = +π/2
        euler.z = std::atan2(R[1][0], R[1][1]);  // 用 atan2 定 z
    } else if (sy < -0.9999f) {                  // 俯仰近 -90°
        euler.x = 0;
        euler.y = -pi * 0.5f;
        euler.z = std::atan2(R[1][0], R[1][1]);
    } else {
        euler.y = std::asin(sy);  // 一般情形：求中间角
        euler.x = std::atan2(R[1][2], R[2][2]);
        euler.z = std::atan2(R[0][1], R[0][0]);
    }
}
// q：四元数缓存；euler：写回 Transform 的欧拉角
inline void sync_euler_from_orientation_quat(ktm::fquat& q, ktm::fvec3& euler) {
    if (q.r < 0.0f) {  // 实为同一旋转的另一表示
        q.i = -q.i;    // i 分量取反
        q.j = -q.j;    // j
        q.k = -q.k;    // k
        q.r = -q.r;    // r
    }
    euler_xyz_from_rot_mat(q.matrix3x3(), euler);  // 由旋转矩阵反解欧拉
}

// 显式积分四元数导数 dq/dt = 0.5*(0,ω)*q
inline void integrate_orientation_quat(ktm::fquat& q, const ktm::fvec3& omega_world, float dt) {
    const ktm::fquat wq = ktm::fquat::real_imag(0.0f, omega_world);  // 纯虚四元数装角速度
    const ktm::fquat dq = wq * q;                                    // 与 q 相乘得导出
    q.i += 0.5f * dt * dq.i;                                         // 累加 i 分量变化
    q.j += 0.5f * dt * dq.j;
    q.k += 0.5f * dt * dq.k;
    q.r += 0.5f * dt * dq.r;
    q = ktm::normalize(q);  // 保持单位四元数
}

// AABB 在方向 dir 上的极值点（近似接触用）
inline ktm::fvec3 aabb_support_world(const ktm::fvec3& center, const ktm::fvec3& half,
                                     const ktm::fvec3& dir) {
    return make_fvec3(
        center.x + (dir.x >= 0.0f ? half.x : -half.x),  // dir 指向 +X 时取右侧面
        center.y + (dir.y >= 0.0f ? half.y : -half.y),
        center.z + (dir.z >= 0.0f ? half.z : -half.z));
}

// 刚体上一点的世界系线速度 = 质心平动 + 转动项 ω×r
inline ktm::fvec3 velocity_at_point_world(const ktm::fvec3& v_com, const ktm::fvec3& omega_world,
                                          const ktm::fvec3& r_com_to_point) {
    const ktm::fvec3 wxr = ktm::cross(omega_world, r_com_to_point);        // 叉乘
    return make_fvec3(v_com.x + wxr.x, v_com.y + wxr.y, v_com.z + wxr.z);  // 矢量加
}

// 世界系向量左乘 I^{-1}（对角惯量模型 + 当前姿态 R）
inline ktm::fvec3 world_inertia_inv_apply(const ktm::fmat3x3& R_body_to_world,
                                          const ktm::fvec3& inertia_inv_body,
                                          const ktm::fvec3& w_world) {
    const ktm::fmat3x3 RT = ktm::transpose(R_body_to_world);  // 逆旋转（正交阵）
    ktm::fvec3 b = RT * w_world;                              // 世界→体
    b.x *= inertia_inv_body.x;                                // 体坐标乘以 1/Ix
    b.y *= inertia_inv_body.y;
    b.z *= inertia_inv_body.z;
    return R_body_to_world * b;  // 体→世界
}

// 本帧单个 mechanics 物体的碰撞/渲染用几何缓存
struct MechanicsWorldAABB {
    std::uintptr_t handle;               // mechanics 设备句柄键
    std::uintptr_t transform_handle;     // 几何上的 ModelTransform 句柄
    ktm::fvec3 min_world;                // 世界 AABB 最小角
    ktm::fvec3 max_world;                // 世界 AABB 最大角
    ktm::fvec3 center_world;             // AABB 中心（亦作 AABB 窄相参考点）
    ktm::fvec3 half_extents;             // 世界 AABB 半尺寸
    ktm::fvec3 local_min;                // mechanics 局部 min_xyz
    ktm::fvec3 local_max;                // mechanics 局部 max_xyz
    ktm::fvec3 obb_center;               // OBB 中心（世界）
    ktm::fvec3 obb_u, obb_v, obb_w;      // OBB 三个正交轴单位向量（世界）
    float obb_hu{}, obb_hv{}, obb_hw{};  // 对应轴上半轴长
    ktm::fmat3x3 rot_body_to_world{};    // 由预测四元数得到的旋转矩阵
    ktm::fvec3 inertia_inv_body{};       // 体坐标 (1/Ix,1/Iy,1/Iz)
    std::uint64_t model_id = 0;          // 对应的模型资源ID（用于碰撞网格查找）
    bool is_skinned = false;             // 蒙皮物体：跳过三角网格，只用动态 AABB 碰撞（P4）
};

// entry：读写 OBB 字段；t：用于把局部点变到世界
inline void build_mechanics_obb(MechanicsWorldAABB& entry, const Corona::ModelTransform& t) {
    const ktm::fvec3 c_l = make_fvec3(
        (entry.local_min.x + entry.local_max.x) * 0.5f,  // 局部盒中心 X
        (entry.local_min.y + entry.local_max.y) * 0.5f,
        (entry.local_min.z + entry.local_max.z) * 0.5f);
    const ktm::fvec3 e_l = make_fvec3(
        (entry.local_max.x - entry.local_min.x) * 0.5f,  // 沿局部 X 的半棱长
        (entry.local_max.y - entry.local_min.y) * 0.5f,
        (entry.local_max.z - entry.local_min.z) * 0.5f);
    entry.obb_center = transform_local_point_to_world(t, c_l);  // 盒心到世界
    ktm::fvec3 ax = vec3_sub(transform_local_point_to_world(t, vec3_add(c_l, make_fvec3(e_l.x, 0.f, 0.f))),
                             entry.obb_center);  // +局部 X 轴端点相对心的向量（世界）
    ktm::fvec3 ay = vec3_sub(transform_local_point_to_world(t, vec3_add(c_l, make_fvec3(0.f, e_l.y, 0.f))),
                             entry.obb_center);  // +Y
    ktm::fvec3 az = vec3_sub(transform_local_point_to_world(t, vec3_add(c_l, make_fvec3(0.f, 0.f, e_l.z))),
                             entry.obb_center);  // +Z
    const float lu = ktm::length(ax);            // 轴方向未归一长度
    const float lv = ktm::length(ay);
    const float lz = ktm::length(az);
    constexpr float obb_eps = 1e-8f;  // 退化盒厚度下限
    if (lu > obb_eps) {
        const float inv = 1.0f / lu;                                   // 归一化因子
        entry.obb_u = make_fvec3(ax.x * inv, ax.y * inv, ax.z * inv);  // 单位轴 u
        entry.obb_hu = lu;                                             // 半轴长取几何长度
    } else {
        entry.obb_u = make_fvec3(1.0f, 0.0f, 0.0f);  // 默认 X
        entry.obb_hu = obb_eps;                      // 极小半长避免除零
    }
    if (lv > obb_eps) {
        const float inv = 1.0f / lv;
        entry.obb_v = make_fvec3(ay.x * inv, ay.y * inv, ay.z * inv);
        entry.obb_hv = lv;
    } else {
        entry.obb_v = make_fvec3(0.0f, 1.0f, 0.0f);
        entry.obb_hv = obb_eps;
    }
    if (lz > obb_eps) {
        const float inv = 1.0f / lz;
        entry.obb_w = make_fvec3(az.x * inv, az.y * inv, az.z * inv);
        entry.obb_hw = lz;
    } else {
        entry.obb_w = make_fvec3(0.0f, 0.0f, 1.0f);
        entry.obb_hw = obb_eps;
    }
}

// OBB 投影到单位方向 L_unit 上的半长（分离轴定理里「半径」项）
inline float obb_radius_on_axis(float hu, float hv, float hw,
                                const ktm::fvec3& u, const ktm::fvec3& v, const ktm::fvec3& w,
                                const ktm::fvec3& L_unit) {
    return hu * std::abs(ktm::dot(u, L_unit))     // u 轴贡献
           + hv * std::abs(ktm::dot(v, L_unit))   // v 轴
           + hw * std::abs(ktm::dot(w, L_unit));  // w 轴
}

// 在方向 dir 上取 OBB 支撑点：c 为中心；u,v,w 轴与 hu,hv,hw 半长；沿 dir 取最远顶点
inline ktm::fvec3 obb_support_point(const ktm::fvec3& c,
                                    const ktm::fvec3& u, const ktm::fvec3& v, const ktm::fvec3& w,
                                    float hu, float hv, float hw,
                                    const ktm::fvec3& dir) {
    ktm::fvec3 p = c;                                       // 从中心出发
    const float su = (ktm::dot(u, dir) >= 0.f) ? hu : -hu;  // u 轴上取与 dir 同向或反向端点
    const float sv = (ktm::dot(v, dir) >= 0.f) ? hv : -hv;
    const float sw = (ktm::dot(w, dir) >= 0.f) ? hw : -hw;
    p = vec3_add(p, vec3_mul(u, su));  // 沿 u 平移 su*u
    p = vec3_add(p, vec3_mul(v, sv));
    p = vec3_add(p, vec3_mul(w, sw));
    return p;  // 角点之一
}

/*
 * OBB–OBB 窄相：15 轴 SAT（A 的三面法向、B 的三面法向、9 个边叉积）。
 * 返回最小穿透深度对应轴作为分离方向；法线取向为 A → B（与盒心连线一致）。
 */
inline bool sat_obb_obb(const ktm::fvec3& ca, const ktm::fvec3& ua, const ktm::fvec3& va, const ktm::fvec3& wa,
                        float hau, float hav, float haw,
                        const ktm::fvec3& cb, const ktm::fvec3& ub, const ktm::fvec3& vb, const ktm::fvec3& wb,
                        float hbu, float hbv, float hbw,
                        ktm::fvec3& out_normal, float& out_penetration) {
    constexpr float ax_eps = 1e-8f;                     // 轴长过短视为退化，跳过该轴
    const ktm::fvec3 d_cb = vec3_sub(cb, ca);           // B 中心相对 A 中心的位移
    float best_ov = std::numeric_limits<float>::max();  // 当前找到的最小正重叠（最深穿透轴）
    bool have_axis = false;                             // 是否至少成功检测过一条有效轴
    ktm::fvec3 best_L = make_fvec3(1.0f, 0.0f, 0.0f);   // 最优分离轴方向（单位向量）

    // 在候选轴 Lraw 上做 1D SAT：两 OBB 在轴上的投影区间若不相交则整体分离
    auto test_axis = [&](const ktm::fvec3& Lraw) -> bool {
        const float len = ktm::length(Lraw);  // 未归一轴长
        if (len < ax_eps) {                   // 叉积近似零向量
            return true;                      // 不计入分离轴，视为通过
        }
        const float inv = 1.0f / len;                                               // 归一化因子
        const ktm::fvec3 L = make_fvec3(Lraw.x * inv, Lraw.y * inv, Lraw.z * inv);  // 单位轴
        const float rA = obb_radius_on_axis(hau, hav, haw, ua, va, wa, L);          // A 投影半宽
        const float rB = obb_radius_on_axis(hbu, hbv, hbw, ub, vb, wb, L);          // B 投影半宽
        const float cA = ktm::dot(ca, L);                                           // A 中心在轴上坐标
        const float cB = ktm::dot(cb, L);                                           // B 中心在轴上坐标
        const float minA = cA - rA;                                                 // A 投影区间左端
        const float maxA = cA + rA;                                                 // A 投影区间右端
        const float minB = cB - rB;                                                 // B 左端
        const float maxB = cB + rB;                                                 // B 右端
        const float overlap = std::min(maxA, maxB) - std::max(minA, minB);          // 两区间重叠长度
        if (overlap <= 0.f) {                                                       // 存在分离轴
            return false;                                                           // 无事相交
        }
        if (overlap < best_ov) {  // 记录重叠更小的轴（更接近分离）
            best_ov = overlap;
            best_L = L;
            have_axis = true;
        }
        return true;  // 本轴未排除相交
    };

    if (!test_axis(ua)) return false;  // A 的三个面法向
    if (!test_axis(va)) return false;
    if (!test_axis(wa)) return false;
    if (!test_axis(ub)) return false;  // B 的三个面法向
    if (!test_axis(vb)) return false;
    if (!test_axis(wb)) return false;

    const ktm::fvec3 crosses[9] = {// 边与边的叉积方向，共 9 条
                                   ktm::cross(ua, ub), ktm::cross(ua, vb), ktm::cross(ua, wb),
                                   ktm::cross(va, ub), ktm::cross(va, vb), ktm::cross(va, wb),
                                   ktm::cross(wa, ub), ktm::cross(wa, vb), ktm::cross(wa, wb)};
    for (const ktm::fvec3& cax : crosses) {  // 逐条测试叉积轴
        if (!test_axis(cax)) {
            return false;
        }
    }

    if (!have_axis) {  // 理论上不应发生：无有效轴却通过全部测试
        return false;
    }
    if (ktm::dot(best_L, d_cb) < 0.f) {  // 令 best_L 与 A→B 同向，作为 A→B 法线
        best_L = make_fvec3(-best_L.x, -best_L.y, -best_L.z);
    }
    out_normal = best_L;        // 输出接触法线（单位向量）
    out_penetration = best_ov;  // 输出沿该轴穿透深度估计
    return true;
}

// 碰撞对哈希（用于 unordered_set 去重）
struct PairHash {
    std::size_t operator()(const std::pair<std::uintptr_t, std::uintptr_t>& p) const noexcept {
        // Fibonacci hashing 混合
        std::size_t h1 = std::hash<std::uintptr_t>{}(p.first);
        std::size_t h2 = std::hash<std::uintptr_t>{}(p.second);
        h1 ^= h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2);
        return h1;
    }
};

// 移动回调最小间隔时间（秒）
constexpr float kMoveCallbackMinInterval = 0.1f;
// 移动回调最小位移阈值（单位：米/坐标单位）
constexpr float kMoveCallbackMinDistance = 0.1f;

// 轴锁定位掩码常量
constexpr uint8_t kLockAxisX = 0b001;
constexpr uint8_t kLockAxisY = 0b010;
constexpr uint8_t kLockAxisZ = 0b100;

// ========== 延迟回调队列（同步执行，避免跨线程竞争） ==========
struct DeferredCollisionCallback {
	std::function<void(std::uintptr_t, bool, const std::array<float, 3>&, const std::array<float, 3>&)> callback;
	std::uintptr_t other_actor;
	bool is_start;
	std::array<float, 3> normal;
	std::array<float, 3> point;
};

/// Phase 3 — 碰撞→IK target 延迟更新（帧末锁外执行，避免物理循环持锁写 GeometryDevice）。
/// 三角窄相末轮命中蒙皮物体时入队；帧末统一写入匹配的 contact_driven IK 链的 target/weight。
struct DeferredIkTargetUpdate {
    std::uintptr_t geom_handle;       // GeometryDevice（含 ik_chains）
    std::uintptr_t transform_handle;  // 用于 world→model 坐标变换
    int node_idx;                     // 接触骨骼的 SkeletonData::nodes 下标
    ktm::fvec3 contact_world;         // 世界空间接触点（从 tri_result.contact_point 拷贝）
};

// 注意：八叉树实现已迁移到 include/corona/spatial/octree.h，由 GeometrySystem 持有并维护。
// MechanicsSystem 仅作为消费者使用（宽相候选对生成仍可复用该通用实现）。

struct BodyRuntimeState {
    ktm::fvec3 velocity = make_fvec3(0.0f, 0.0f, 0.0f);          // 线速度 m/s
    ktm::fvec3 angular_velocity = make_fvec3(0.0f, 0.0f, 0.0f);  // 角速度 rad/s 世界系
    ktm::fquat orientation_quat{};
    bool orientation_initialized = false;
    bool sleeping = false;
    float sleep_timer = 0.0f;
    uint8_t linear_lock = 0;
    uint8_t angular_lock = 0;
    float last_move_callback_time = -kMoveCallbackMinInterval;
    ktm::fvec3 last_move_callback_pos = make_fvec3(1e9f, 1e9f, 1e9f);
};

struct BodyFrameParams {
    float mass = 1.0f;
    float damping = 0.99f;
    float restitution = 0.8f;
    bool collision_enabled = true;
    CollisionShape collision_shape = CollisionShape::Box;
    std::uintptr_t actor = 0;
};

// ============================================================================
// 碰撞网格（基于最低级 LOD 的三角形碰撞检测）
// ============================================================================

/// 碰撞网格：存储局部空间顶点和三角形索引
struct CollisionMesh {
    std::vector<ktm::fvec3> vertices;                     // 局部/世界空间顶点（蒙皮网格每帧刷新）
    std::vector<std::array<std::uint16_t, 3>> triangles;  // 三角形索引三元组（静态，不随蒙皮变化）
    float min_local_y = 0.0f;                             // 最低点Y（精确地板碰撞）

    // 每个三角形的主导骨骼 node_idx（SkeletonData::nodes 下标）。
    // 导入期一次性计算：取三个顶点 BoneWeights 中权重最大的 bone_id，
    // 再经 bone_id → node_idx 反查表映射。非蒙皮网格此数组为空。
    // Phase 3（碰撞→IK）靠此字段确定接触三角形归属哪条 IK 链。
    std::vector<int> triangle_bone_ids;  // 下标对齐 triangles，-1=无归属骨骼
};

/// 三角形碰撞检测结果
struct TriangleContactResult {
    bool has_contact = false;
    ktm::fvec3 normal;         // 碰撞法线（从 A 指向 B）
    float penetration = 0.0f;  // 穿透深度
    ktm::fvec3 contact_point;  // 接触点

    // Phase 3：记录穿透最深的三角形在各自 CollisionMesh::triangles 中的下标（-1=未记录）。
    // 供碰撞→IK 反馈通路查 triangle_bone_ids，确定接触归属哪条 IK 链。
    int best_tri_a = -1;  // 对应 mesh_a 的三角形下标
    int best_tri_b = -1;  // 对应 mesh_b 的三角形下标
};

// ============================================================================
// 碰撞网格加载
// ============================================================================

/// 从 Resource 层加载最低级 LOD 碰撞网格（静态非蒙皮物体专用）。
/// 返回 true 表示成功加载（或已在缓存中）。
/// 同时把三角索引和骨骼映射写入 static_triangle_index_cache / static_triangle_bone_cache
/// （供蒙皮物体的每帧实例化路径复用，避免重复解析 Scene）。
inline bool ensure_collision_mesh(
    std::uint64_t model_id,
    std::unordered_map<std::uint64_t, CollisionMesh>& collision_mesh_cache,
    std::unordered_map<std::uint64_t, std::vector<std::array<std::uint16_t, 3>>>* static_tri_cache = nullptr,
    std::unordered_map<std::uint64_t, std::vector<int>>* static_bone_cache = nullptr) {
    if (model_id == 0) return false;
    if (collision_mesh_cache.count(model_id)) return true;

    auto scene = Corona::Resource::ResourceManager::get_instance()
                     .acquire_read<Corona::Resource::Scene>(model_id);
    if (!scene) return false;

    // 构建 bone_id → node_idx 反查表（仅蒙皮场景需要）
    // bone_id 是 BoneInfo::id（骨骼在 final 矩阵数组中的下标），
    // node_idx 是 SkeletonData::nodes 的扁平下标。
    std::unordered_map<int, int> bone_id_to_node_idx;
    if (scene->data.skeleton.has_value()) {
        const auto& skel = *scene->data.skeleton;
        for (std::size_t ni = 0; ni < skel.nodes.size(); ++ni) {
            const auto& node = skel.nodes[ni];
            auto it = skel.bone_map.find(node.name);
            if (it != skel.bone_map.end() && it->second.id >= 0) {
                bone_id_to_node_idx[it->second.id] = static_cast<int>(ni);
            }
        }
    }

    CollisionMesh mesh;
    std::vector<std::array<std::uint16_t, 3>> static_tris;
    std::vector<int> static_bone_ids;
    std::uint16_t vertex_offset = 0;

    for (std::uint32_t mi = 0; mi < static_cast<std::uint32_t>(scene->data.meshes.size()); ++mi) {
        const std::vector<Corona::Resource::Vertex>* src_verts = nullptr;
        const std::vector<std::uint16_t>* src_indices = nullptr;
        const std::vector<Corona::Resource::BoneWeights>* src_bw = nullptr;

        std::uint32_t lod_count = scene->get_mesh_lod_count(mi);
        if (lod_count > 0) {
            // 取最后一级 LOD（最简化）
            const auto& lod = scene->get_mesh_lod(mi, lod_count - 1);
            src_verts = &lod.vertices;
            src_indices = &lod.indices;
            src_bw = lod.bone_weights.empty() ? nullptr : &lod.bone_weights;
        } else {
            // 无 LOD，回退原始网格
            src_verts = &scene->get_mesh_vertices(mi);
            src_indices = &scene->get_mesh_indices(mi);
            const auto& bw = scene->data.meshes[mi].bone_weights;
            src_bw = bw.empty() ? nullptr : &bw;
        }

        if (!src_verts || src_verts->empty() || !src_indices || src_indices->empty()) continue;

        // 三角形数过多时跳过此 mesh（降级为 AABB）
        constexpr std::size_t kMaxTrianglesPerMesh = 500;
        if (src_indices->size() / 3 > kMaxTrianglesPerMesh && lod_count == 0) continue;

        // 复制顶点（绑定姿态；蒙皮物体运行期会覆盖为蒙皮后坐标）
        for (const auto& v : *src_verts) {
            ktm::fvec3 pos;
            pos.x = v.position[0];
            pos.y = v.position[1];
            pos.z = v.position[2];
            mesh.vertices.push_back(pos);
        }

        // 复制三角形索引（加偏移）+ 计算每三角主导骨骼 node_idx
        for (std::size_t i = 0; i + 2 < src_indices->size(); i += 3) {
            const auto tri = std::array<std::uint16_t, 3>{
                static_cast<std::uint16_t>((*src_indices)[i]     + vertex_offset),
                static_cast<std::uint16_t>((*src_indices)[i + 1] + vertex_offset),
                static_cast<std::uint16_t>((*src_indices)[i + 2] + vertex_offset),
            };
            mesh.triangles.push_back(tri);
            static_tris.push_back(tri);

            // 主导骨骼：三顶点中权重最大的 bone_id → node_idx
            int dominant_node = -1;
            if (src_bw && !bone_id_to_node_idx.empty()) {
                float best_w = 0.0f;
                for (int vi = 0; vi < 3; ++vi) {
                    std::uint16_t vidx = (*src_indices)[i + static_cast<std::size_t>(vi)];
                    if (vidx >= src_bw->size()) continue;
                    const auto& bw = (*src_bw)[vidx];
                    for (int bi = 0; bi < Corona::Resource::MAX_BONE_INFLUENCE; ++bi) {
                        if (bw.ids[bi] >= 0 && bw.weights[bi] > best_w) {
                            auto nit = bone_id_to_node_idx.find(bw.ids[bi]);
                            if (nit != bone_id_to_node_idx.end()) {
                                best_w = bw.weights[bi];
                                dominant_node = nit->second;
                            }
                        }
                    }
                }
            }
            mesh.triangle_bone_ids.push_back(dominant_node);
            static_bone_ids.push_back(dominant_node);
        }

        vertex_offset = static_cast<std::uint16_t>(mesh.vertices.size());
    }

    if (mesh.vertices.empty() || mesh.triangles.empty()) return false;

    // 预计算最低点Y
    mesh.min_local_y = mesh.vertices[0].y;
    for (const auto& v : mesh.vertices) {
        mesh.min_local_y = std::min(mesh.min_local_y, v.y);
    }

    // 写入静态索引/骨骼缓存（供蒙皮物体每帧复用索引和映射，不重解析 Scene）
    if (static_tri_cache)  (*static_tri_cache)[model_id]  = std::move(static_tris);
    if (static_bone_cache) (*static_bone_cache)[model_id] = std::move(static_bone_ids);

    collision_mesh_cache[model_id] = std::move(mesh);
    return true;
}

/// 获取 mechanics handle 对应的 model_id
inline std::uint64_t get_model_id_for_mechanics(std::uintptr_t mech_handle) {
    auto& mechanics_storage = Corona::SharedDataHub::instance().mechanics_storage();
    auto& geometry_storage = Corona::SharedDataHub::instance().geometry_storage();
    auto& model_resource_storage = Corona::SharedDataHub::instance().model_resource_storage();

    auto m_acc = mechanics_storage.try_acquire_read(mech_handle);
    if (!m_acc) return 0;
    auto geom_acc = geometry_storage.try_acquire_read(m_acc->geometry_handle);
    if (!geom_acc) return 0;
    auto res_acc = model_resource_storage.try_acquire_read(geom_acc->model_resource_handle);
    if (!res_acc) return 0;
    return res_acc->model_id;
}

// ============================================================================
// 三角形工具函数
// ============================================================================

inline ktm::fvec3 cross(const ktm::fvec3& a, const ktm::fvec3& b) {
    return make_fvec3(
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x);
}

inline float dot(const ktm::fvec3& a, const ktm::fvec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline ktm::fvec3 sub(const ktm::fvec3& a, const ktm::fvec3& b) {
    return make_fvec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

inline float vec_length(const ktm::fvec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

inline ktm::fvec3 normalize_safe(const ktm::fvec3& v) {
    float len = vec_length(v);
    if (len < 1e-8f) return make_fvec3(0.0f, 1.0f, 0.0f);
    return make_fvec3(v.x / len, v.y / len, v.z / len);
}

/// 将局部空间碰撞网格顶点变换到世界空间
inline void transform_vertices_to_world(
    const std::vector<ktm::fvec3>& local_verts,
    const Corona::ModelTransform& tx,
    std::vector<ktm::fvec3>& world_verts) {
    world_verts.resize(local_verts.size());

    // 如果有旋转，使用完整矩阵变换
    bool has_rotation = (std::abs(tx.euler_rotation.x) > 1e-6f ||
                         std::abs(tx.euler_rotation.y) > 1e-6f ||
                         std::abs(tx.euler_rotation.z) > 1e-6f);

    if (has_rotation) {
        ktm::fmat4x4 mat = tx.compute_matrix();
        for (std::size_t i = 0; i < local_verts.size(); ++i) {
            const auto& v = local_verts[i];
            // mat * (v, 1)
            world_verts[i] = make_fvec3(
                mat[0][0] * v.x + mat[1][0] * v.y + mat[2][0] * v.z + mat[3][0],
                mat[0][1] * v.x + mat[1][1] * v.y + mat[2][1] * v.z + mat[3][1],
                mat[0][2] * v.x + mat[1][2] * v.y + mat[2][2] * v.z + mat[3][2]);
        }
    } else {
        // 无旋转：简单缩放+平移
        for (std::size_t i = 0; i < local_verts.size(); ++i) {
            const auto& v = local_verts[i];
            world_verts[i] = make_fvec3(
                v.x * tx.scale.x + tx.position.x,
                v.y * tx.scale.y + tx.position.y,
                v.z * tx.scale.z + tx.position.z);
        }
    }
}

// ============================================================================
// Möller 三角形-三角形相交测试
// 基于分离轴定理 (SAT) 的实现
// ============================================================================

/// 将三角形的三个顶点投影到轴上，返回 [min, max]
inline void project_triangle(const ktm::fvec3& axis,
                             const ktm::fvec3& v0, const ktm::fvec3& v1, const ktm::fvec3& v2,
                             float& out_min, float& out_max) {
    float d0 = dot(axis, v0);
    float d1 = dot(axis, v1);
    float d2 = dot(axis, v2);
    out_min = std::min({d0, d1, d2});
    out_max = std::max({d0, d1, d2});
}

/// 测试两个三角形在给定轴上的投影是否分离
/// 若不分离，返回重叠量
inline bool test_axis(const ktm::fvec3& axis,
                      const ktm::fvec3 a[3], const ktm::fvec3 b[3],
                      float& overlap) {
    float axis_len_sq = dot(axis, axis);
    if (axis_len_sq < 1e-12f) {
        // 退化轴（平行边），不作为分离轴
        overlap = std::numeric_limits<float>::max();
        return false;  // 不分离
    }

    float a_min, a_max, b_min, b_max;
    project_triangle(axis, a[0], a[1], a[2], a_min, a_max);
    project_triangle(axis, b[0], b[1], b[2], b_min, b_max);

    constexpr float sep_eps = 1e-5f;  // 浮点容差，避免擦边接触被误判为分离
    if (a_max < b_min - sep_eps || b_max < a_min - sep_eps) {
        return true;  // 分离
    }

    // 计算重叠深度
    float inv_len = 1.0f / std::sqrt(axis_len_sq);
    overlap = (std::min(a_max, b_max) - std::max(a_min, b_min)) * inv_len;
    return false;  // 不分离
}

/// SAT 三角形-三角形相交测试
/// 返回是否相交，并输出穿透法线和深度
inline bool triangle_triangle_sat(const ktm::fvec3 tri_a[3], const ktm::fvec3 tri_b[3],
                                  ktm::fvec3& out_normal, float& out_depth) {
    // 计算三角形边向量
    ktm::fvec3 edge_a[3] = {
        sub(tri_a[1], tri_a[0]),
        sub(tri_a[2], tri_a[1]),
        sub(tri_a[0], tri_a[2])};
    ktm::fvec3 edge_b[3] = {
        sub(tri_b[1], tri_b[0]),
        sub(tri_b[2], tri_b[1]),
        sub(tri_b[0], tri_b[2])};

    // 面法线
    ktm::fvec3 normal_a = cross(edge_a[0], edge_a[1]);
    ktm::fvec3 normal_b = cross(edge_b[0], edge_b[1]);

    float min_overlap = std::numeric_limits<float>::max();
    ktm::fvec3 min_axis = make_fvec3(0.0f, 1.0f, 0.0f);

    // 测试轴：面法线A
    float overlap;
    if (test_axis(normal_a, tri_a, tri_b, overlap)) return false;
    if (overlap < min_overlap) {
        min_overlap = overlap;
        min_axis = normal_a;
    }

    // 测试轴：面法线B
    if (test_axis(normal_b, tri_a, tri_b, overlap)) return false;
    if (overlap < min_overlap) {
        min_overlap = overlap;
        min_axis = normal_b;
    }

    // 测试轴：9 个边叉积
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            ktm::fvec3 axis = cross(edge_a[i], edge_b[j]);
            if (test_axis(axis, tri_a, tri_b, overlap)) return false;
            if (overlap < min_overlap) {
                min_overlap = overlap;
                min_axis = axis;
            }
        }
    }

    // 所有轴均未分离 → 相交
    out_normal = normalize_safe(min_axis);
    out_depth = min_overlap;
    return true;
}

/// 执行两个碰撞网格之间的三角形精确碰撞检测
/// world_verts_a/b: 已变换到世界空间的顶点
/// mesh_a/b: 碰撞网格（提供三角形索引）
/// result: 输出接触信息
inline void triangle_narrowphase(
    const std::vector<ktm::fvec3>& world_verts_a,
    const CollisionMesh& mesh_a,
    const std::vector<ktm::fvec3>& world_verts_b,
    const CollisionMesh& mesh_b,
    const ktm::fvec3& center_a,
    const ktm::fvec3& center_b,
    TriangleContactResult& result) {
    result.has_contact = false;
    float best_depth = 0.0f;
    ktm::fvec3 best_normal = make_fvec3(0.0f, 1.0f, 0.0f);
    ktm::fvec3 best_point = make_fvec3(0.0f, 0.0f, 0.0f);
    int contact_count = 0;
    ktm::fvec3 contact_sum = make_fvec3(0.0f, 0.0f, 0.0f);
    int best_tri_a_idx = -1;  // 穿透最深那对三角形在 mesh_a 中的下标
    int best_tri_b_idx = -1;  // 同上，mesh_b

    int cur_tri_a = -1;
    for (const auto& tri_a_idx : mesh_a.triangles) {
        ++cur_tri_a;
        // 三角形 A 的世界空间顶点
        const ktm::fvec3& a0 = world_verts_a[tri_a_idx[0]];
        const ktm::fvec3& a1 = world_verts_a[tri_a_idx[1]];
        const ktm::fvec3& a2 = world_verts_a[tri_a_idx[2]];

        // 三角形 A 的 mini-AABB
        ktm::fvec3 a_min = make_fvec3(
            std::min({a0.x, a1.x, a2.x}), std::min({a0.y, a1.y, a2.y}), std::min({a0.z, a1.z, a2.z}));
        ktm::fvec3 a_max = make_fvec3(
            std::max({a0.x, a1.x, a2.x}), std::max({a0.y, a1.y, a2.y}), std::max({a0.z, a1.z, a2.z}));

        int cur_tri_b = -1;
        for (const auto& tri_b_idx : mesh_b.triangles) {
            ++cur_tri_b;
            // 三角形 B 的世界空间顶点
            const ktm::fvec3& b0 = world_verts_b[tri_b_idx[0]];
            const ktm::fvec3& b1 = world_verts_b[tri_b_idx[1]];
            const ktm::fvec3& b2 = world_verts_b[tri_b_idx[2]];

            // Mini-AABB 预筛选
            ktm::fvec3 b_min = make_fvec3(
                std::min({b0.x, b1.x, b2.x}), std::min({b0.y, b1.y, b2.y}), std::min({b0.z, b1.z, b2.z}));
            ktm::fvec3 b_max = make_fvec3(
                std::max({b0.x, b1.x, b2.x}), std::max({b0.y, b1.y, b2.y}), std::max({b0.z, b1.z, b2.z}));

            if (!aabb_overlap(a_min, a_max, b_min, b_max)) continue;

            // SAT 精确测试
            ktm::fvec3 tri_a_verts[3] = {a0, a1, a2};
            ktm::fvec3 tri_b_verts[3] = {b0, b1, b2};

            ktm::fvec3 normal;
            float depth;
            if (!triangle_triangle_sat(tri_a_verts, tri_b_verts, normal, depth)) continue;

            // 累积接触点（两三角形中心的平均）
            ktm::fvec3 tri_center = make_fvec3(
                (a0.x + a1.x + a2.x + b0.x + b1.x + b2.x) / 6.0f,
                (a0.y + a1.y + a2.y + b0.y + b1.y + b2.y) / 6.0f,
                (a0.z + a1.z + a2.z + b0.z + b1.z + b2.z) / 6.0f);
            contact_sum.x += tri_center.x;
            contact_sum.y += tri_center.y;
            contact_sum.z += tri_center.z;
            ++contact_count;

            // 取穿透最深的法线和深度，同时记录对应三角形下标（Phase 3 IK 反馈用）
            if (depth > best_depth) {
                best_depth = depth;
                best_normal = normal;
                best_point = tri_center;
                best_tri_a_idx = cur_tri_a;
                best_tri_b_idx = cur_tri_b;
            }
        }
    }

    if (contact_count == 0) return;

    result.has_contact = true;
    result.penetration = best_depth;
    result.best_tri_a = best_tri_a_idx;
    result.best_tri_b = best_tri_b_idx;
    result.contact_point = make_fvec3(
        contact_sum.x / static_cast<float>(contact_count),
        contact_sum.y / static_cast<float>(contact_count),
        contact_sum.z / static_cast<float>(contact_count));

    // 确保法线方向从 A 指向 B
    ktm::fvec3 a_to_b = sub(center_b, center_a);
    if (dot(best_normal, a_to_b) < 0.0f) {
        best_normal = make_fvec3(-best_normal.x, -best_normal.y, -best_normal.z);
    }
    result.normal = best_normal;
}


}  // namespace Corona::Systems::MechanicsInternal


namespace Corona::Systems {

struct MechanicsSystem::Impl {
    Kernel::ISystemContext* ctx = nullptr;
    GeometrySystem* geometry_sys = nullptr;

    // ActorResidencyChangedEvent 驱动的驻留集合
    Kernel::EventId residency_sub_id_ = 0;
    mutable std::shared_mutex residency_mtx_;
    std::unordered_set<std::uintptr_t> resident_actors_;
    float time_accumulator = 0.0f;
    std::chrono::steady_clock::time_point last_update_time{};
    bool first_update = true;

    /// 骨骼动画上一次蒙皮的时间戳（诊断用；dt 已由 update() 测量并传入，不再用于计算）。
    std::optional<std::chrono::steady_clock::time_point> last_skin_update_time;

    std::atomic<bool> shutdown_requested{false};
    float global_simulation_time = 0.0f;

    std::unordered_map<std::uintptr_t, MechanicsInternal::BodyRuntimeState> bodies;
    std::unordered_map<std::uint64_t, MechanicsInternal::CollisionMesh> collision_mesh_cache;

    // 蒙皮物体的每帧碰撞网格（键：geom_handle，每帧顶点刷新，索引/bone_ids 不变）
    std::unordered_map<std::uintptr_t, MechanicsInternal::CollisionMesh> skinned_collision_cache;

    // 按 model_id 缓存的静态三角索引和骨骼映射（同一模型所有实例共享，只在首次导入时算一次）
    std::unordered_map<std::uint64_t, std::vector<std::array<std::uint16_t, 3>>> static_triangle_index_cache;
    std::unordered_map<std::uint64_t, std::vector<int>>                          static_triangle_bone_cache;

    // Phase 3：碰撞→IK target 延迟更新队列（帧末在 geometry_storage 锁外执行）
    std::vector<MechanicsInternal::DeferredIkTargetUpdate> deferred_ik_target_updates;

    std::unordered_set<std::pair<std::uintptr_t, std::uintptr_t>, MechanicsInternal::PairHash> prev_active_collisions;
    std::vector<std::function<void()>> deferred_move_callbacks;
    std::vector<MechanicsInternal::DeferredCollisionCallback> deferred_collision_callbacks;
    MechanicsInternal::BodyRuntimeState& body(std::uintptr_t handle) {
        return bodies.try_emplace(handle).first->second;
    }

    void clear_runtime_state() {
        deferred_move_callbacks.clear();
        deferred_collision_callbacks.clear();
        deferred_ik_target_updates.clear();
        prev_active_collisions.clear();
        bodies.clear();
        collision_mesh_cache.clear();
        skinned_collision_cache.clear();
        global_simulation_time = 0.0f;
        time_accumulator = 0.0f;
        first_update = true;
        geometry_sys = nullptr;
    }
};

}  // namespace Corona::Systems
