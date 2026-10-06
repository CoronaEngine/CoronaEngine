#pragma once
// mechanics_narrowphase2.h
// Phase 2：精确三角形接触检测（替换基于 SAT 的旧实现）+ TriangleOctree 加速结构。
// 集成：在 mechanics_internal.h 末尾（namespace 结束前）添加：
//   #include "mechanics_narrowphase2.h"
// 本文件假定 CollisionMesh、resolve_vertex、TriangleContactResult 已由
// mechanics_internal.h 定义。
//
// 四条约束（来自 docs/development/mechanics-collision-fix-plan.md §1）：
//  1. 只做三角-三角，不引入代理形状。
//  2. 法线来自面法线（点-面）或最近点连线（边-边），不做质心翻转。
//  3. 加速结构只用八叉树拓扑 + 拟合 AABB，不引入 BVH。
//  4. 加速结构只输出候选对，最终接触一律由精确三角形基元决定。

#include "mechanics_internal.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace Corona::Systems::MechanicsInternal {

// ============================================================
// 1. 辅助几何工具
// ============================================================

/// 线段 P+t*d 与线段 Q+s*e 的最近点参数（t,s ∈ [0,1]）。
/// Ericson "Real-Time Collision Detection" p.149 ClosestPtSegmentSegment.
static inline void segment_segment_closest(
    const ktm::fvec3& p, const ktm::fvec3& d,
    const ktm::fvec3& q, const ktm::fvec3& e,
    float& t, float& s) {
    constexpr float kEps = 1e-10f;
    const ktm::fvec3 r    = sub(p, q);
    const float      a    = dot(d, d);   // |d|^2
    const float      f    = dot(e, r);
    const float      ee   = dot(e, e);   // |e|^2

    if (a < kEps && ee < kEps) { t = 0.0f; s = 0.0f; return; }

    if (a < kEps) {
        t = 0.0f;
        s = std::max(0.0f, std::min(1.0f, f / ee));
    } else {
        const float c = dot(d, r);
        if (ee < kEps) {
            s = 0.0f;
            t = std::max(0.0f, std::min(1.0f, -c / a));
        } else {
            const float b     = dot(d, e);
            const float denom = a * ee - b * b;  // always >= 0
            t = (denom > kEps) ? std::max(0.0f, std::min(1.0f, (b * f - c * ee) / denom)) : 0.0f;
            s = (b * t + f) / ee;
            if (s < 0.0f) {
                s = 0.0f;
                t = std::max(0.0f, std::min(1.0f, -c / a));
            } else if (s > 1.0f) {
                s = 1.0f;
                t = std::max(0.0f, std::min(1.0f, (b - c) / a));
            }
        }
    }
}

/// 点 pt 到三角形 (a,b,c) 的有符号距离（沿已归一化法线 n）。
/// out_inside：pt 投影到三角形平面后是否在三角形内部。
/// 正值 = pt 在法线朝向的一侧；负值 = 背面。
static inline float point_triangle_signed_dist(
    const ktm::fvec3& pt,
    const ktm::fvec3& a, const ktm::fvec3& b, const ktm::fvec3& c,
    const ktm::fvec3& n,
    bool& out_inside) {
    const float sd      = dot(n, sub(pt, a));   // 有符号距离
    const ktm::fvec3 proj = sub(pt, vec3_mul(n, sd));  // 投影到平面上的点

    // 重心坐标法判断投影点是否在三角形内
    const ktm::fvec3 v0 = sub(c, a);
    const ktm::fvec3 v1 = sub(b, a);
    const ktm::fvec3 v2 = sub(proj, a);
    const float dot00 = dot(v0, v0);
    const float dot01 = dot(v0, v1);
    const float dot02 = dot(v0, v2);
    const float dot11 = dot(v1, v1);
    const float dot12 = dot(v1, v2);
    const float inv   = (dot00 * dot11 - dot01 * dot01);
    if (std::abs(inv) < 1e-12f) { out_inside = false; return sd; }
    const float inv_denom = 1.0f / inv;
    const float u = (dot11 * dot02 - dot01 * dot12) * inv_denom;
    const float v = (dot00 * dot12 - dot01 * dot02) * inv_denom;
    out_inside = (u >= -1e-5f) && (v >= -1e-5f) && (u + v <= 1.0f + 1e-5f);
    return sd;
}

// ============================================================
// 2. Möller 三角-三角相交（含共面分支）
// ============================================================

/// 共面三角形 2D 分离轴测试（把两三角形投影到 A 的平面）。
/// 6 条轴（各三角形的 3 条边法线）。返回 true 表示重叠（不分离）。
static inline bool coplanar_triangles_overlap(
    const ktm::fvec3& a0, const ktm::fvec3& a1, const ktm::fvec3& a2,
    const ktm::fvec3& b0, const ktm::fvec3& b1, const ktm::fvec3& b2,
    const ktm::fvec3& n) {
    // 投影到 n 的正交补空间（选两个正交基向量）
    // 简单取 n 的某个垂直向量
    ktm::fvec3 t_axis = make_fvec3(1.0f, 0.0f, 0.0f);
    if (std::abs(dot(n, t_axis)) > 0.9f) t_axis = make_fvec3(0.0f, 1.0f, 0.0f);
    const ktm::fvec3 u = normalize_safe(cross(n, t_axis));
    const ktm::fvec3 v = cross(n, u);

    // 2D 投影
    auto proj2 = [&](const ktm::fvec3& p) -> std::array<float, 2> {
        return {dot(u, p), dot(v, p)};
    };
    std::array<float,2> A[3] = {proj2(a0), proj2(a1), proj2(a2)};
    std::array<float,2> B[3] = {proj2(b0), proj2(b1), proj2(b2)};

    // 对每条分离轴测试：边的法向
    auto test_axis_2d = [&](float ax, float ay) -> bool {
        // 投影所有点，检查区间是否重叠
        float a_min =  std::numeric_limits<float>::max();
        float a_max = -std::numeric_limits<float>::max();
        float b_min =  std::numeric_limits<float>::max();
        float b_max = -std::numeric_limits<float>::max();
        for (int i = 0; i < 3; ++i) {
            float pa = A[i][0]*ax + A[i][1]*ay;
            a_min = std::min(a_min, pa); a_max = std::max(a_max, pa);
            float pb = B[i][0]*ax + B[i][1]*ay;
            b_min = std::min(b_min, pb); b_max = std::max(b_max, pb);
        }
        return !(a_max < b_min - 1e-5f || b_max < a_min - 1e-5f);
    };

    // A 的三条边
    for (int i = 0; i < 3; ++i) {
        int j = (i + 1) % 3;
        float dx = A[j][0] - A[i][0];
        float dy = A[j][1] - A[i][1];
        if (!test_axis_2d(-dy, dx)) return false;
    }
    // B 的三条边
    for (int i = 0; i < 3; ++i) {
        int j = (i + 1) % 3;
        float dx = B[j][0] - B[i][0];
        float dy = B[j][1] - B[i][1];
        if (!test_axis_2d(-dy, dx)) return false;
    }
    return true;
}

/// Möller 1997 三角-三角相交测试（含共面分支）。
/// 返回 true 表示相交；out_normal 为接触法线（从 B 面朝向 A），out_depth 为穿透深度（> 0）。
static inline bool moller_triangle_triangle_intersect(
    const ktm::fvec3& a0, const ktm::fvec3& a1, const ktm::fvec3& a2,
    const ktm::fvec3& b0, const ktm::fvec3& b1, const ktm::fvec3& b2,
    ktm::fvec3& out_normal, float& out_depth,
    float contact_threshold) {
    constexpr float kEps = 1e-7f;

    // Step 1：A 的平面
    const ktm::fvec3 nA_raw = cross(sub(a1, a0), sub(a2, a0));
    const float nA_len = vec_length(nA_raw);
    if (nA_len < kEps) return false;  // 退化三角形
    const ktm::fvec3 nA = vec3_mul(nA_raw, 1.0f / nA_len);
    const float dA = dot(nA, a0);

    // Step 2：B 三顶点到 A 面的有符号距离
    const float db0 = dot(nA, b0) - dA;
    const float db1 = dot(nA, b1) - dA;
    const float db2 = dot(nA, b2) - dA;

    // 全同号且都不接近 0 → B 与 A 平面完全分离
    if (db0 * db1 > 0.0f && db0 * db2 > 0.0f &&
        std::abs(db0) > kEps && std::abs(db1) > kEps && std::abs(db2) > kEps)
        return false;

    // Step 3：B 的平面
    const ktm::fvec3 nB_raw = cross(sub(b1, b0), sub(b2, b0));
    const float nB_len = vec_length(nB_raw);
    if (nB_len < kEps) return false;
    const ktm::fvec3 nB = vec3_mul(nB_raw, 1.0f / nB_len);
    const float dB = dot(nB, b0);

    // Step 4：A 三顶点到 B 面的有符号距离
    const float da0 = dot(nB, a0) - dB;
    const float da1 = dot(nB, a1) - dB;
    const float da2 = dot(nB, a2) - dB;

    if (da0 * da1 > 0.0f && da0 * da2 > 0.0f &&
        std::abs(da0) > kEps && std::abs(da1) > kEps && std::abs(da2) > kEps)
        return false;

    // Step 5：共面检测
    const ktm::fvec3 D = cross(nA, nB);
    const float D_len = vec_length(D);
    if (D_len < 1e-6f) {
        // 共面：用 2D SAT
        if (!coplanar_triangles_overlap(a0, a1, a2, b0, b1, b2, nA)) return false;
        out_normal = nA;
        out_depth  = contact_threshold;
        return true;
    }

    // Step 6：交线方向（已归一化）
    const ktm::fvec3 D_unit = vec3_mul(D, 1.0f / D_len);

    // Step 7：把 A 三顶点投影到 D 轴，找交线段区间
    // 只取 B 面符号不同的两个端点处插值的参数
    auto proj_interval = [&](const float d0, const float d1, const float d2,
                              const ktm::fvec3& v0, const ktm::fvec3& v1, const ktm::fvec3& v2,
                              float& tmin, float& tmax) {
        // 三顶点在 D_unit 上的投影
        float p0 = dot(D_unit, v0);
        float p1 = dot(D_unit, v1);
        float p2 = dot(D_unit, v2);
        // 找异号的那一侧端点（"孤点"与"对边"）
        // 找孤点：与另外两点异号
        int lone = -1;
        if ((d0 > 0.0f) != (d1 > 0.0f) && (d0 > 0.0f) != (d2 > 0.0f)) lone = 0;
        else if ((d1 > 0.0f) != (d0 > 0.0f) && (d1 > 0.0f) != (d2 > 0.0f)) lone = 1;
        else lone = 2;

        float dl = (lone == 0) ? d0 : (lone == 1) ? d1 : d2;
        float pl = (lone == 0) ? p0 : (lone == 1) ? p1 : p2;
        float da = (lone == 0) ? d1 : (lone == 1) ? d0 : d0;  // 孤点对边的第一个顶点距离
        float db = (lone == 0) ? d2 : (lone == 1) ? d2 : d1;
        float pa = (lone == 0) ? p1 : (lone == 1) ? p0 : p0;
        float pb = (lone == 0) ? p2 : (lone == 1) ? p2 : p1;

        // t 为与孤点的线段交点参数（Möller 公式）
        // Bug-2 修复：dl - da 和 dl - db 可能为零（三角形刚好共面时），加保护避免除以零
        constexpr float kDivEps = 1e-10f;
        float denom_a = dl - da;
        float denom_b = dl - db;
        float ta = (std::abs(denom_a) > kDivEps) ? (pl + (pa - pl) * (dl / denom_a)) : pl;
        float tb = (std::abs(denom_b) > kDivEps) ? (pl + (pb - pl) * (dl / denom_b)) : pl;
        tmin = std::min(ta, tb);
        tmax = std::max(ta, tb);
    };

    float tA0, tA1, tB0, tB1;
    proj_interval(da0, da1, da2, a0, a1, a2, tA0, tA1);
    proj_interval(db0, db1, db2, b0, b1, b2, tB0, tB1);

    // Step 8：区间重叠检测
    const float overlap_start = std::max(tA0, tB0);
    const float overlap_end   = std::min(tA1, tB1);
    if (overlap_end <= overlap_start - kEps) return false;

    // Step 9-10：深度和法线
    out_depth  = (overlap_end - overlap_start);  // 世界空间长度（D_unit 已归一化）
    // 法线：从 B 面法线反向（从 B 朝 A），保证冲量方向是把 B 推离 A
    out_normal = make_fvec3(-nB.x, -nB.y, -nB.z);
    return true;
}

// ============================================================
// 3. triangle_narrowphase2（替换旧版 triangle_narrowphase）
// ============================================================

/// 精确三角形接触检测（阶段 2 版本）。
/// 修复 D1–D6：
///   D1/D2/D3：SAT 投影退化 → 改用 Möller 相交 + 点-面 + 边-边。
///   D4：深度恒 0 → best_depth 初始值为 -threshold，任何有效接触都更新。
///   D5：best_normal 恒为默认 Y → 取实际面法线/连线方向。
///   D6：质心翻转法线 → 删除，法线由基元决定。
/// candidate_pairs：来自 TriangleOctree::query_pairs 的候选对集合（可为空表示全量遍历）。
///   非空时只检测候选对，跳过其余对，实现 octree 真正加速。
///   空集合退化为 O(T_a × T_b) 暴力遍历，语义与旧版完全相同。
inline void triangle_narrowphase2(
    const std::vector<ktm::fvec3>& world_verts_a,
    const CollisionMesh& mesh_a,
    const std::vector<ktm::fvec3>& world_verts_b,
    const CollisionMesh& mesh_b,
    const ktm::fvec3& /*center_a*/,
    const ktm::fvec3& /*center_b*/,
    TriangleContactResult& result,
    float contact_threshold = 0.02f,
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>* candidate_pairs = nullptr) {
    result.has_contact = false;
    float best_depth = -contact_threshold;
    ktm::fvec3 best_normal = make_fvec3(0.0f, 1.0f, 0.0f);
    ktm::fvec3 contact_sum = make_fvec3(0.0f, 0.0f, 0.0f);
    int contact_count   = 0;
    int best_tri_a_idx  = -1;
    int best_tri_b_idx  = -1;

    const std::size_t tri_count_a = mesh_a.triangles.size();
    const std::size_t tri_count_b = mesh_b.triangles.size();

    // 单对三角形测试（供全量遍历和候选对加速路径共用）
    auto test_pair = [&](std::uint32_t cur_ta, std::uint32_t cur_tb) {
        const std::uint32_t ai0 = resolve_vertex(mesh_a, cur_ta, 0);
        const std::uint32_t ai1 = resolve_vertex(mesh_a, cur_ta, 1);
        const std::uint32_t ai2 = resolve_vertex(mesh_a, cur_ta, 2);
        if (ai0 >= world_verts_a.size() || ai1 >= world_verts_a.size() || ai2 >= world_verts_a.size()) return;
        const ktm::fvec3& a0 = world_verts_a[ai0];
        const ktm::fvec3& a1 = world_verts_a[ai1];
        const ktm::fvec3& a2 = world_verts_a[ai2];
        const ktm::fvec3 nA_raw = cross(sub(a1, a0), sub(a2, a0));
        const ktm::fvec3 nA     = normalize_safe(nA_raw);
        const ktm::fvec3 a_min = make_fvec3(
            std::min({a0.x,a1.x,a2.x}), std::min({a0.y,a1.y,a2.y}), std::min({a0.z,a1.z,a2.z}));
        const ktm::fvec3 a_max = make_fvec3(
            std::max({a0.x,a1.x,a2.x}), std::max({a0.y,a1.y,a2.y}), std::max({a0.z,a1.z,a2.z}));

        const std::uint32_t bi0 = resolve_vertex(mesh_b, cur_tb, 0);
        const std::uint32_t bi1 = resolve_vertex(mesh_b, cur_tb, 1);
        const std::uint32_t bi2 = resolve_vertex(mesh_b, cur_tb, 2);
        if (bi0 >= world_verts_b.size() || bi1 >= world_verts_b.size() || bi2 >= world_verts_b.size()) return;
        const ktm::fvec3& b0 = world_verts_b[bi0];
        const ktm::fvec3& b1 = world_verts_b[bi1];
        const ktm::fvec3& b2 = world_verts_b[bi2];
        const ktm::fvec3 b_min = make_fvec3(
            std::min({b0.x,b1.x,b2.x}), std::min({b0.y,b1.y,b2.y}), std::min({b0.z,b1.z,b2.z}));
        const ktm::fvec3 b_max = make_fvec3(
            std::max({b0.x,b1.x,b2.x}), std::max({b0.y,b1.y,b2.y}), std::max({b0.z,b1.z,b2.z}));

        const float th = contact_threshold;
        if (a_min.x-th > b_max.x || b_min.x-th > a_max.x ||
            a_min.y-th > b_max.y || b_min.y-th > a_max.y ||
            a_min.z-th > b_max.z || b_min.z-th > a_max.z) return;

        const ktm::fvec3 nB_raw = cross(sub(b1, b0), sub(b2, b0));
        const ktm::fvec3 nB     = normalize_safe(nB_raw);

        // 相交测试（穿透）
        ktm::fvec3 inter_n; float inter_d;
        if (moller_triangle_triangle_intersect(a0, a1, a2, b0, b1, b2, inter_n, inter_d, contact_threshold)) {
            const ktm::fvec3 cp = make_fvec3(
                (a0.x+a1.x+a2.x+b0.x+b1.x+b2.x)/6.0f,
                (a0.y+a1.y+a2.y+b0.y+b1.y+b2.y)/6.0f,
                (a0.z+a1.z+a2.z+b0.z+b1.z+b2.z)/6.0f);
            contact_sum.x += cp.x; contact_sum.y += cp.y; contact_sum.z += cp.z;
            ++contact_count;
            if (inter_d > best_depth) {
                best_depth = inter_d; best_normal = inter_n;
                best_tri_a_idx = static_cast<int>(cur_ta);
                best_tri_b_idx = static_cast<int>(cur_tb);
            }
            return;
        }

        // 近接测试（非穿透）：B 顶点对 A 面
        const ktm::fvec3 bverts[3] = {b0, b1, b2};
        for (int vi = 0; vi < 3; ++vi) {
            bool inside;
            const float sd = point_triangle_signed_dist(bverts[vi], a0, a1, a2, nA, inside);
            if (!inside || sd < 0.0f || sd > contact_threshold) continue;
            const float depth = contact_threshold - sd;
            const ktm::fvec3 cp = sub(bverts[vi], vec3_mul(nA, sd));
            contact_sum.x += cp.x; contact_sum.y += cp.y; contact_sum.z += cp.z;
            ++contact_count;
            if (depth > best_depth) {
                best_depth = depth; best_normal = nA;
                best_tri_a_idx = static_cast<int>(cur_ta);
                best_tri_b_idx = static_cast<int>(cur_tb);
            }
        }

        // 近接测试：A 顶点对 B 面
        const ktm::fvec3 averts[3] = {a0, a1, a2};
        for (int vi = 0; vi < 3; ++vi) {
            bool inside;
            const float sd = point_triangle_signed_dist(averts[vi], b0, b1, b2, nB, inside);
            if (!inside || sd < 0.0f || sd > contact_threshold) continue;
            const float depth = contact_threshold - sd;
            const ktm::fvec3 cp = sub(averts[vi], vec3_mul(nB, sd));
            contact_sum.x += cp.x; contact_sum.y += cp.y; contact_sum.z += cp.z;
            ++contact_count;
            if (depth > best_depth) {
                best_depth = depth; best_normal = make_fvec3(-nB.x, -nB.y, -nB.z);
                best_tri_a_idx = static_cast<int>(cur_ta);
                best_tri_b_idx = static_cast<int>(cur_tb);
            }
        }

        // 边-边最近点（9 组）
        const ktm::fvec3 ea_p[3] = {a0, a1, a2};
        const ktm::fvec3 ea_d[3] = {sub(a1,a0), sub(a2,a1), sub(a0,a2)};
        const ktm::fvec3 eb_p[3] = {b0, b1, b2};
        const ktm::fvec3 eb_d[3] = {sub(b1,b0), sub(b2,b1), sub(b0,b2)};
        for (int ei = 0; ei < 3; ++ei) for (int ej = 0; ej < 3; ++ej) {
            float t, s;
            segment_segment_closest(ea_p[ei], ea_d[ei], eb_p[ej], eb_d[ej], t, s);
            const ktm::fvec3 pA = vec3_add(ea_p[ei], vec3_mul(ea_d[ei], t));
            const ktm::fvec3 pB = vec3_add(eb_p[ej], vec3_mul(eb_d[ej], s));
            const ktm::fvec3 diff = sub(pB, pA);
            const float dist2 = dot(diff, diff);
            const float thr2  = contact_threshold * contact_threshold;
            if (dist2 >= thr2 || dist2 < 1e-12f) continue;
            const float dist  = std::sqrt(dist2);
            const float depth = contact_threshold - dist;
            const ktm::fvec3 n_edge = vec3_mul(diff, 1.0f / dist);
            const ktm::fvec3 cp = vec3_mul(vec3_add(pA, pB), 0.5f);
            contact_sum.x += cp.x; contact_sum.y += cp.y; contact_sum.z += cp.z;
            ++contact_count;
            if (depth > best_depth) {
                best_depth = depth; best_normal = n_edge;
                best_tri_a_idx = static_cast<int>(cur_ta);
                best_tri_b_idx = static_cast<int>(cur_tb);
            }
        }
    };

    // candidate_pairs 非空时只遍历候选对（octree 加速路径）；否则全量遍历
    if (candidate_pairs && !candidate_pairs->empty()) {
        for (const auto& [cur_ta, cur_tb] : *candidate_pairs) {
            if (cur_ta < tri_count_a && cur_tb < tri_count_b)
                test_pair(cur_ta, cur_tb);
        }
    } else {
        for (std::uint32_t cur_ta = 0; cur_ta < static_cast<std::uint32_t>(tri_count_a); ++cur_ta)
            for (std::uint32_t cur_tb = 0; cur_tb < static_cast<std::uint32_t>(tri_count_b); ++cur_tb)
                test_pair(cur_ta, cur_tb);
    }

    if (contact_count == 0) return;

    result.has_contact    = true;
    result.penetration    = best_depth > 0.0f ? best_depth : 0.0f;
    result.normal         = best_normal;
    result.contact_point  = make_fvec3(
        contact_sum.x / static_cast<float>(contact_count),
        contact_sum.y / static_cast<float>(contact_count),
        contact_sum.z / static_cast<float>(contact_count));
    result.best_tri_a     = best_tri_a_idx;
    result.best_tri_b     = best_tri_b_idx;
    // 注意：不做质心翻转（修 D6），法线由基元决定。
}

// ============================================================
// 4. TriangleOctree（per-model，绑定姿态建拓扑，逐子步 refit）
// ============================================================

/// 三角形八叉树：拓扑在绑定姿态下建一次（cached by model_id），
/// 逐子步 refit 更新拟合盒，剪枝只用拟合盒（不用划分盒）。
/// 这是约束 2"仅用八叉树 + AABB"在精细粒度上的具体实现。
struct TriangleOctree {
    struct Node {
        ktm::fvec3   fitted_min;    // 子树内全部三角形当前 AABB 的并集（含 delta 扩展）
        ktm::fvec3   fitted_max;
        std::uint32_t tri_begin  = 0; // 叶节点：在 leaf_tris 中的起始下标
        std::uint32_t tri_count  = 0; // 叶节点：三角形数；内部节点：0
        std::uint32_t child_begin = 0; // 内部节点：第一个子节点下标
        std::uint8_t  child_count = 0; // 0 = 叶节点
    };

    std::vector<Node>          nodes;      // 树节点（DFS 前序）；nodes[0] 为根
    std::vector<std::uint32_t> leaf_tris;  // 叶节点三角形下标（在 CollisionMesh.triangles 中）
    float delta = 0.005f;                  // 拟合盒扩展量

    bool empty() const { return nodes.empty(); }

    // 在绑定姿态下建树（bind_verts 为绑定姿态世界顶点，mesh 为碰撞网格）
    void build(const std::vector<ktm::fvec3>& bind_verts,
               const CollisionMesh& mesh,
               int max_tris_per_leaf = 8) {
        nodes.clear();
        leaf_tris.clear();
        if (mesh.triangles.empty()) return;

        std::vector<std::uint32_t> all_tris(mesh.triangles.size());
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(mesh.triangles.size()); ++i)
            all_tris[i] = i;

        build_recursive(bind_verts, mesh, all_tris, max_tris_per_leaf, 0);
    }

    // 逐子步自底向上 refit（用当前 world_verts 更新所有节点 fitted_min/max）
    // 若提供 prev_verts（非空且大小一致），拟合盒取两帧顶点的包络（B2 扫掠 AABB），
    // 使高速运动时节点盒保守覆盖整个运动轨迹，避免快速穿透漏检。
    void refit(const std::vector<ktm::fvec3>& world_verts,
               const CollisionMesh& mesh,
               const std::vector<ktm::fvec3>* prev_verts = nullptr) {
        if (nodes.empty()) return;
        bool sweep = (prev_verts != nullptr && prev_verts->size() == world_verts.size());
        refit_dfs_postorder_sweep(world_verts, mesh, 0, sweep ? prev_verts : nullptr);
    }

    // 两树下降求候选三角形对（结果追加到 out_pairs）
    static void query_pairs(
        const TriangleOctree& oa, const std::vector<ktm::fvec3>& /*verts_a*/,
        const TriangleOctree& ob, const std::vector<ktm::fvec3>& /*verts_b*/,
        std::vector<std::pair<std::uint32_t, std::uint32_t>>& out_pairs) {
        if (oa.empty() || ob.empty()) return;
        descend(oa, 0, ob, 0, out_pairs);
    }

private:
    // 递归建树（DFS，返回当前节点在 nodes[] 中的下标）
    std::uint32_t build_recursive(
        const std::vector<ktm::fvec3>& verts,
        const CollisionMesh& mesh,
        std::vector<std::uint32_t>& tris,
        int max_per_leaf, int depth) {
        const std::uint32_t node_idx = static_cast<std::uint32_t>(nodes.size());
        nodes.emplace_back();

        // 计算本节点的拟合盒
        auto& nd = nodes[node_idx];
        nd.fitted_min = make_fvec3( std::numeric_limits<float>::max(),  std::numeric_limits<float>::max(),  std::numeric_limits<float>::max());
        nd.fitted_max = make_fvec3(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
        for (std::uint32_t ti : tris) {
            for (int c = 0; c < 3; ++c) {
                const std::uint32_t vi = resolve_vertex(mesh, ti, c);
                if (vi >= verts.size()) continue;
                const ktm::fvec3& p = verts[vi];
                nd.fitted_min.x = std::min(nd.fitted_min.x, p.x);
                nd.fitted_min.y = std::min(nd.fitted_min.y, p.y);
                nd.fitted_min.z = std::min(nd.fitted_min.z, p.z);
                nd.fitted_max.x = std::max(nd.fitted_max.x, p.x);
                nd.fitted_max.y = std::max(nd.fitted_max.y, p.y);
                nd.fitted_max.z = std::max(nd.fitted_max.z, p.z);
            }
        }
        nd.fitted_min.x -= delta; nd.fitted_min.y -= delta; nd.fitted_min.z -= delta;
        nd.fitted_max.x += delta; nd.fitted_max.y += delta; nd.fitted_max.z += delta;

        // 叶节点
        if (static_cast<int>(tris.size()) <= max_per_leaf || depth >= 20) {
            nd.tri_begin = static_cast<std::uint32_t>(leaf_tris.size());
            nd.tri_count = static_cast<std::uint32_t>(tris.size());
            nd.child_count = 0;
            for (std::uint32_t ti : tris) leaf_tris.push_back(ti);
            return node_idx;
        }

        // 内部节点：计算质心均值，按 8 象限分组
        ktm::fvec3 center = make_fvec3(
            (nd.fitted_min.x + nd.fitted_max.x) * 0.5f,
            (nd.fitted_min.y + nd.fitted_max.y) * 0.5f,
            (nd.fitted_min.z + nd.fitted_max.z) * 0.5f);

        std::vector<std::uint32_t> buckets[8];
        for (std::uint32_t ti : tris) {
            // 三角形质心
            float cx = 0.0f, cy = 0.0f, cz = 0.0f;
            for (int c = 0; c < 3; ++c) {
                const std::uint32_t vi = resolve_vertex(mesh, ti, c);
                if (vi < verts.size()) {
                    cx += verts[vi].x; cy += verts[vi].y; cz += verts[vi].z;
                }
            }
            cx /= 3.0f; cy /= 3.0f; cz /= 3.0f;
            int oct = ((cx >= center.x) ? 1 : 0)
                    | ((cy >= center.y) ? 2 : 0)
                    | ((cz >= center.z) ? 4 : 0);
            buckets[oct].push_back(ti);
        }

        // 合并空桶（三角形全在一侧时避免无限递归）
        // 若某桶大小 == tris.size()，降级为叶节点
        bool all_in_one = false;
        for (int i = 0; i < 8; ++i) {
            if (buckets[i].size() == tris.size()) { all_in_one = true; break; }
        }
        if (all_in_one) {
            nd.tri_begin = static_cast<std::uint32_t>(leaf_tris.size());
            nd.tri_count = static_cast<std::uint32_t>(tris.size());
            nd.child_count = 0;
            for (std::uint32_t ti : tris) leaf_tris.push_back(ti);
            return node_idx;
        }

        // 记录子节点起始下标（在添加子节点之前记录，但子节点是动态追加的）
        // 子节点会 push_back 进 nodes[]，因此需在递归后设置 child_begin
        std::vector<std::uint32_t> child_indices;
        for (int i = 0; i < 8; ++i) {
            if (buckets[i].empty()) continue;
            child_indices.push_back(build_recursive(verts, mesh, buckets[i], max_per_leaf, depth + 1));
        }

        // 递归结束后 nodes[] 已扩充，重新取 nd 引用（原引用可能因 realloc 失效）
        Node& nd2 = nodes[node_idx];
        nd2.child_begin = child_indices.empty() ? 0 : child_indices[0];
        nd2.child_count = static_cast<std::uint8_t>(child_indices.size());
        nd2.tri_count   = 0;
        return node_idx;
    }

    // 自底向上 refit（后序）
    void refit_recursive(
        const std::vector<ktm::fvec3>& verts,
        const CollisionMesh& mesh,
        std::uint32_t node_idx) {
        Node& nd = nodes[node_idx];

        if (nd.child_count == 0) {
            // 叶节点：重算拟合盒
            nd.fitted_min = make_fvec3( std::numeric_limits<float>::max(),  std::numeric_limits<float>::max(),  std::numeric_limits<float>::max());
            nd.fitted_max = make_fvec3(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
            for (std::uint32_t k = nd.tri_begin; k < nd.tri_begin + nd.tri_count; ++k) {
                const std::uint32_t ti = leaf_tris[k];
                for (int c = 0; c < 3; ++c) {
                    const std::uint32_t vi = resolve_vertex(mesh, ti, c);
                    if (vi >= verts.size()) continue;
                    const ktm::fvec3& p = verts[vi];
                    nd.fitted_min.x = std::min(nd.fitted_min.x, p.x);
                    nd.fitted_min.y = std::min(nd.fitted_min.y, p.y);
                    nd.fitted_min.z = std::min(nd.fitted_min.z, p.z);
                    nd.fitted_max.x = std::max(nd.fitted_max.x, p.x);
                    nd.fitted_max.y = std::max(nd.fitted_max.y, p.y);
                    nd.fitted_max.z = std::max(nd.fitted_max.z, p.z);
                }
            }
        } else {
            // 内部节点：先递归所有子节点，再取并集
            nd.fitted_min = make_fvec3( std::numeric_limits<float>::max(),  std::numeric_limits<float>::max(),  std::numeric_limits<float>::max());
            nd.fitted_max = make_fvec3(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
            // 子节点索引是连续存储在 nodes[] 中的（build_recursive 按 DFS 顺序追加）
            // 遍历：child_begin 是第一个子节点的 nodes 下标，child_count 个子节点连续
            // 注意：子节点并非严格连续（DFS 追加时子树完整存入），所以需要另存子节点 idx
            // 此实现中 child_begin 存的是直接子节点的第一个 index，不连续——改用辅助遍历。
            // 简化：重新对 child_begin..child_begin+child_count-1 做后序遍历
            // （实际上 build_recursive 记录的是各子节点的 node_idx，不是连续的）
            // 为保持实现简单，这里对整棵子树做全量扫描（O(N)，N=子树节点数）：
            // 递归处理前序中 node_idx 之后的 child_count 棵子树
            for (std::uint8_t ci = 0; ci < nd.child_count; ++ci) {
                // child_begin + ci 只对"连续子节点"有效；此实现中子节点确实是
                // 在 child_begin 之后连续 push_back 的（每个子树整体推入），
                // 但下标不是 child_begin+0, child_begin+1…
                // 因此在 build 阶段改为记录子节点数组。
                // 当前实现：child_begin 存的是第 0 个直接子节点；其余子节点的
                // 根节点在 leaf_tris/nodes 中没有直接索引。
                // 临时可行方案：对整棵树做 DFS 后序，不依赖 child_begin+ci 索引。
                // （阶段 3 前可以做专项清理；当前先保证功能正确）
                (void)ci;
            }
            // 简化版 refit：直接从叶节点向上聚合，通过全树后序实现
            // 这里利用 DFS 顺序：对整棵树做单次后序遍历
            refit_dfs_postorder(verts, mesh, node_idx);
            return;
        }
        nd.fitted_min.x -= delta; nd.fitted_min.y -= delta; nd.fitted_min.z -= delta;
        nd.fitted_max.x += delta; nd.fitted_max.y += delta; nd.fitted_max.z += delta;
    }

    // 后序 DFS refit（解决 child_begin+ci 不连续的问题）
    // 返回值：{fitted_min, fitted_max}
    std::pair<ktm::fvec3, ktm::fvec3> refit_dfs_postorder(
        const std::vector<ktm::fvec3>& verts,
        const CollisionMesh& mesh,
        std::uint32_t node_idx) {
        Node& nd = nodes[node_idx];

        ktm::fvec3 fmin = make_fvec3( std::numeric_limits<float>::max(),  std::numeric_limits<float>::max(),  std::numeric_limits<float>::max());
        ktm::fvec3 fmax = make_fvec3(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());

        if (nd.child_count == 0) {
            // 叶节点
            for (std::uint32_t k = nd.tri_begin; k < nd.tri_begin + nd.tri_count; ++k) {
                for (int c = 0; c < 3; ++c) {
                    const std::uint32_t vi = resolve_vertex(mesh, leaf_tris[k], c);
                    if (vi >= verts.size()) continue;
                    const ktm::fvec3& p = verts[vi];
                    fmin.x = std::min(fmin.x, p.x); fmin.y = std::min(fmin.y, p.y); fmin.z = std::min(fmin.z, p.z);
                    fmax.x = std::max(fmax.x, p.x); fmax.y = std::max(fmax.y, p.y); fmax.z = std::max(fmax.z, p.z);
                }
            }
        } else {
            std::uint32_t cur = nd.child_begin;
            for (std::uint8_t ci = 0; ci < nd.child_count; ++ci) {
                auto [cmin, cmax] = refit_dfs_postorder(verts, mesh, cur);
                fmin.x = std::min(fmin.x, cmin.x); fmin.y = std::min(fmin.y, cmin.y); fmin.z = std::min(fmin.z, cmin.z);
                fmax.x = std::max(fmax.x, cmax.x); fmax.y = std::max(fmax.y, cmax.y); fmax.z = std::max(fmax.z, cmax.z);
                cur += subtree_size(cur);
            }
        }

        fmin.x -= delta; fmin.y -= delta; fmin.z -= delta;
        fmax.x += delta; fmax.y += delta; fmax.z += delta;
        nd.fitted_min = fmin;
        nd.fitted_max = fmax;
        return {fmin, fmax};
    }

    // B2：双帧扫掠 refit — 拟合盒取当前帧和上一帧顶点的包络，覆盖运动轨迹
    // prev_verts 为 nullptr 时退化为单帧 refit
    std::pair<ktm::fvec3, ktm::fvec3> refit_dfs_postorder_sweep(
        const std::vector<ktm::fvec3>& verts,
        const CollisionMesh& mesh,
        std::uint32_t node_idx,
        const std::vector<ktm::fvec3>* prev_verts) {
        Node& nd = nodes[node_idx];

        ktm::fvec3 fmin = make_fvec3( std::numeric_limits<float>::max(),  std::numeric_limits<float>::max(),  std::numeric_limits<float>::max());
        ktm::fvec3 fmax = make_fvec3(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());

        auto expand = [&](const ktm::fvec3& p) {
            fmin.x = std::min(fmin.x, p.x); fmin.y = std::min(fmin.y, p.y); fmin.z = std::min(fmin.z, p.z);
            fmax.x = std::max(fmax.x, p.x); fmax.y = std::max(fmax.y, p.y); fmax.z = std::max(fmax.z, p.z);
        };

        if (nd.child_count == 0) {
            for (std::uint32_t k = nd.tri_begin; k < nd.tri_begin + nd.tri_count; ++k) {
                for (int c = 0; c < 3; ++c) {
                    const std::uint32_t vi = resolve_vertex(mesh, leaf_tris[k], c);
                    if (vi < verts.size()) expand(verts[vi]);
                    if (prev_verts && vi < prev_verts->size()) expand((*prev_verts)[vi]);
                }
            }
        } else {
            std::uint32_t cur = nd.child_begin;
            for (std::uint8_t ci = 0; ci < nd.child_count; ++ci) {
                auto [cmin, cmax] = refit_dfs_postorder_sweep(verts, mesh, cur, prev_verts);
                fmin.x = std::min(fmin.x, cmin.x); fmin.y = std::min(fmin.y, cmin.y); fmin.z = std::min(fmin.z, cmin.z);
                fmax.x = std::max(fmax.x, cmax.x); fmax.y = std::max(fmax.y, cmax.y); fmax.z = std::max(fmax.z, cmax.z);
                cur += subtree_size(cur);
            }
        }

        fmin.x -= delta; fmin.y -= delta; fmin.z -= delta;
        fmax.x += delta; fmax.y += delta; fmax.z += delta;
        nd.fitted_min = fmin;
        nd.fitted_max = fmax;
        return {fmin, fmax};
    }

    // 子树大小（节点数，含自身）
    std::uint32_t subtree_size(std::uint32_t node_idx) const {
        const Node& nd = nodes[node_idx];
        if (nd.child_count == 0) return 1;
        std::uint32_t sz = 1;
        std::uint32_t cur = nd.child_begin;
        for (std::uint8_t ci = 0; ci < nd.child_count; ++ci) {
            std::uint32_t csz = subtree_size(cur);
            sz  += csz;
            cur += csz;
        }
        return sz;
    }

    // 两树下降（静态，用 aabb_overlap 剪枝）
    static void descend(
        const TriangleOctree& oa, std::uint32_t ia,
        const TriangleOctree& ob, std::uint32_t ib,
        std::vector<std::pair<std::uint32_t, std::uint32_t>>& out) {
        const Node& na = oa.nodes[ia];
        const Node& nb = ob.nodes[ib];

        if (!aabb_overlap(na.fitted_min, na.fitted_max, nb.fitted_min, nb.fitted_max)) return;

        if (na.child_count == 0 && nb.child_count == 0) {
            // 双叶节点：输出所有候选对
            for (std::uint32_t i = na.tri_begin; i < na.tri_begin + na.tri_count; ++i)
                for (std::uint32_t j = nb.tri_begin; j < nb.tri_begin + nb.tri_count; ++j)
                    out.emplace_back(oa.leaf_tris[i], ob.leaf_tris[j]);
            return;
        }

        // 展开子节点较多（或唯一非叶）的一侧
        if (na.child_count == 0) {
            // 展开 b
            std::uint32_t cur = nb.child_begin;
            for (std::uint8_t ci = 0; ci < nb.child_count; ++ci) {
                descend(oa, ia, ob, cur, out);
                cur += ob.subtree_size(cur);
            }
        } else if (nb.child_count == 0) {
            // 展开 a
            std::uint32_t cur = na.child_begin;
            for (std::uint8_t ci = 0; ci < na.child_count; ++ci) {
                descend(oa, cur, ob, ib, out);
                cur += oa.subtree_size(cur);
            }
        } else {
            // 展开子节点更多的一侧
            if (na.child_count >= nb.child_count) {
                std::uint32_t cur = na.child_begin;
                for (std::uint8_t ci = 0; ci < na.child_count; ++ci) {
                    descend(oa, cur, ob, ib, out);
                    cur += oa.subtree_size(cur);
                }
            } else {
                std::uint32_t cur = nb.child_begin;
                for (std::uint8_t ci = 0; ci < nb.child_count; ++ci) {
                    descend(oa, ia, ob, cur, out);
                    cur += ob.subtree_size(cur);
                }
            }
        }
    }
};

}  // namespace Corona::Systems::MechanicsInternal
