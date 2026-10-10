// ============================================================================
// 骨骼动画运行时求值（animation_pose）：FK 数学自洽性和 CCD 回归测试
//
// 不依赖引擎/GPU/磁盘。验证：
//   1. mat4_mul 单位元 / 结合律
//   2. compose_trs：单位四元数 = 纯 T*S；90° 旋转方向正确
//   3. slerp 端点 + 中点
//   4. compute_pose 绑定姿态 → final ≈ 单位阵（核心验证闸）
//   5. compute_pose 无动画通道时回退 local；层级累乘正确
//   6. CCD 旋转混合保长、直链退化恢复、模型空间容差与无效输入
// ============================================================================
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include "corona/resource/types/animation_pose.h"

using namespace Corona::Resource;

namespace {

int g_passed = 0;
int g_failed = 0;

constexpr std::array<float, 16> kIdentity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

void check(bool cond, const char* name) {
    if (cond) {
        ++g_passed;
        std::printf("[PASS] %s\n", name);
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", name);
    }
}

bool near_eq(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

bool mat_near(const std::array<float, 16>& a, const std::array<float, 16>& b, float eps = 1e-4f) {
    for (int i = 0; i < 16; ++i) {
        if (!near_eq(a[i], b[i], eps)) return false;
    }
    return true;
}

// 列主序矩阵 * 点 (x,y,z,1)，返回变换后的 xyz
std::array<float, 3> transform_point(const std::array<float, 16>& m, const std::array<float, 3>& p) {
    return {
        m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
        m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
        m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}

void test_mat4_mul() {
    // 单位元
    std::array<float, 16> a{2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, 5, 6, 7, 1};
    check(mat_near(mat4_mul(a, kIdentity), a), "mat4_mul: A*I == A");
    check(mat_near(mat4_mul(kIdentity, a), a), "mat4_mul: I*A == A");
}

void test_compose_trs() {
    // 单位四元数 → 纯平移+缩放
    auto m = compose_trs({1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, {2.0f, 2.0f, 2.0f});
    auto p = transform_point(m, {1.0f, 0.0f, 0.0f});
    // 缩放 2 后平移 (1,2,3) → (2+1, 0+2, 0+3) = (3,2,3)
    check(near_eq(p[0], 3.0f) && near_eq(p[1], 2.0f) && near_eq(p[2], 3.0f),
          "compose_trs: identity quat = T*S");

    // 绕 Z 轴 90°（四元数 (0,0,sin45,cos45)）：x 轴 → y 轴
    float s = std::sin(3.14159265f / 4.0f);
    float c = std::cos(3.14159265f / 4.0f);
    auto rz = compose_trs({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, s, c}, {1.0f, 1.0f, 1.0f});
    auto rp = transform_point(rz, {1.0f, 0.0f, 0.0f});
    check(near_eq(rp[0], 0.0f) && near_eq(rp[1], 1.0f) && near_eq(rp[2], 0.0f),
          "compose_trs: Z-90deg maps +X to +Y");
}

void test_slerp_endpoints() {
    AnimChannel ch;
    ch.bone_name = "b";
    // 两个旋转关键帧：单位 → 绕 Z 90°
    float s = std::sin(3.14159265f / 4.0f);
    float c = std::cos(3.14159265f / 4.0f);
    ch.rotations.emplace_back(0.0f, std::array<float, 4>{0, 0, 0, 1});
    ch.rotations.emplace_back(10.0f, std::array<float, 4>{0, 0, s, c});
    ch.positions.emplace_back(0.0f, std::array<float, 3>{0, 0, 0});
    ch.scales.emplace_back(0.0f, std::array<float, 3>{1, 1, 1});

    // t=0：单位旋转，+X 不变
    auto m0 = sample_channel(ch, 0.0f);
    auto p0 = transform_point(m0, {1, 0, 0});
    check(near_eq(p0[0], 1.0f) && near_eq(p0[1], 0.0f), "slerp: t=0 -> identity rotation");

    // t=10（末帧）：90°，+X → +Y
    auto m1 = sample_channel(ch, 10.0f);
    auto p1 = transform_point(m1, {1, 0, 0});
    check(near_eq(p1[0], 0.0f) && near_eq(p1[1], 1.0f), "slerp: t=end -> 90deg rotation");

    // t=5（中点）：45°，+X → (cos45, sin45)
    auto mh = sample_channel(ch, 5.0f);
    auto ph = transform_point(mh, {1, 0, 0});
    check(near_eq(ph[0], c) && near_eq(ph[1], s), "slerp: midpoint -> 45deg rotation");
}

void test_advance_time_loops() {
    AnimationClip clip;
    clip.duration = 10.0f;
    clip.ticks_per_second = 1.0f;
    // 从 9 推进 2 秒 → 11 fmod 10 = 1
    float t = advance_anim_time(9.0f, 2.0f, clip);
    check(near_eq(t, 1.0f), "advance_anim_time: wraps around duration");
}

// 核心验证闸：绑定姿态下 final ≈ 单位阵。
// 构造：单骨骼，offset = inverse(local_bind)，global_inverse = I，
// 无动画通道（用 bind local）→ final = I * local_bind * inverse(local_bind) = I。
void test_bind_pose_identity_single() {
    SkeletonData skel;
    skel.bone_count = 1;
    skel.global_inverse = kIdentity;

    // 单骨骼 local_bind = 平移(5,0,0) * 缩放2
    auto local_bind = compose_trs({5.0f, 0.0f, 0.0f}, {0, 0, 0, 1}, {2, 2, 2});

    BoneNode node;
    node.name = "root_bone";
    node.local = local_bind;
    skel.nodes.push_back(node);
    skel.root = 0;

    // offset = inverse(local_bind)。手动求逆（T*S 可解析求逆）：
    // local_bind: 缩放2+平移(5,0,0)。逆 = 缩放0.5，平移 -(5,0,0)*0.5
    std::array<float, 16> inv_bind{0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 0.5f, 0,
                                   -2.5f, 0, 0, 1};
    BoneInfo info;
    info.id = 0;
    info.offset = inv_bind;
    skel.bone_map["root_bone"] = info;

    // 空 clip（无通道）→ 用 bind local
    AnimationClip clip;
    clip.duration = 1.0f;
    clip.ticks_per_second = 1.0f;

    std::vector<std::array<float, 16>> finals;
    compute_pose(skel, clip, 0.0f, finals);

    check(finals.size() == 1, "bind-pose: finals sized to bone_count");
    check(!finals.empty() && mat_near(finals[0], kIdentity, 1e-3f),
          "bind-pose: single bone final ~= identity (CORE GATE)");
}

// 层级：父(平移10) + 子(平移3)，子骨骼 offset=inverse(父*子 bind global)，
// global_inverse=I，无通道 → 子 final ≈ I。验证父变换正确累乘到子。
void test_bind_pose_identity_hierarchy() {
    SkeletonData skel;
    skel.bone_count = 1;
    skel.global_inverse = kIdentity;

    auto parent_local = compose_trs({10.0f, 0.0f, 0.0f}, {0, 0, 0, 1}, {1, 1, 1});
    auto child_local = compose_trs({3.0f, 0.0f, 0.0f}, {0, 0, 0, 1}, {1, 1, 1});

    BoneNode parent;
    parent.name = "parent";
    parent.local = parent_local;
    parent.children = {1};
    BoneNode child;
    child.name = "child";
    child.local = child_local;
    skel.nodes.push_back(parent);
    skel.nodes.push_back(child);
    skel.root = 0;

    // 子骨骼绑定全局 = parent_local * child_local = 平移(13,0,0)
    // offset = inverse = 平移(-13,0,0)
    std::array<float, 16> child_offset{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -13, 0, 0, 1};
    BoneInfo info;
    info.id = 0;
    info.offset = child_offset;
    skel.bone_map["child"] = info;

    AnimationClip clip;
    clip.duration = 1.0f;
    clip.ticks_per_second = 1.0f;

    std::vector<std::array<float, 16>> finals;
    compute_pose(skel, clip, 0.0f, finals);

    check(!finals.empty() && mat_near(finals[0], kIdentity, 1e-3f),
          "bind-pose: hierarchical bone final ~= identity (parent accum)");
}

SkeletonData make_ik_skeleton(int count) {
    SkeletonData skeleton;
    skeleton.root = 0;
    skeleton.bone_count = count;
    skeleton.global_inverse = kIdentity;
    for (int i = 0; i < count; ++i) {
        BoneNode node;
        node.name = "joint_" + std::to_string(i);
        node.parent = i - 1;
        node.local = compose_trs({i == 0 ? 0.0f : 1.0f, 0, 0}, {0, 0, 0, 1}, {1, 1, 1});
        if (i + 1 < count) node.children.push_back(i + 1);
        BoneInfo bone;
        bone.id = i;
        bone.offset = kIdentity;
        skeleton.bone_map[node.name] = bone;
        skeleton.nodes.push_back(node);
    }
    return skeleton;
}

std::vector<std::array<float, 16>> ik_locals(const SkeletonData& skeleton) {
    std::vector<std::array<float, 16>> locals;
    sample_pose_locals(skeleton, AnimationClip{}, 0.0f, locals);
    return locals;
}

IkChain make_ik_chain(int end_node, int length, std::array<float, 3> target) {
    IkChain chain;
    chain.enabled = true;
    chain.end_node = end_node;
    chain.chain_length = length;
    chain.target = target;
    chain.tolerance = 1e-4f;
    chain.max_iterations = 100;
    return chain;
}

std::vector<std::array<float, 16>> solve_ik_pose(const SkeletonData& skeleton, const IkChain& chain) {
    const auto locals = ik_locals(skeleton);
    std::unordered_map<int, std::array<float, 16>> overrides;
    solve_ccd(skeleton, chain, locals, overrides);
    std::vector<std::array<float, 16>> finals;
    compute_pose(skeleton, AnimationClip{}, 0.0f, finals, &overrides, &locals);
    return finals;
}

float point_distance(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    float square = 0.0f;
    for (int i = 0; i < 3; ++i) square += (a[i] - b[i]) * (a[i] - b[i]);
    return std::sqrt(square);
}

// L^T*L is invariant under a left rotation, so this verifies lengths, relative
// axis angles, nonuniform scale, and pre-existing shear without decomposing TRS.
bool same_linear_metric(const std::array<float, 16>& a, const std::array<float, 16>& b) {
    for (int c = 0; c < 3; ++c) {
        for (int d = 0; d < 3; ++d) {
            float dot_a = 0.0f, dot_b = 0.0f;
            for (int r = 0; r < 3; ++r) {
                dot_a += a[c * 4 + r] * a[d * 4 + r];
                dot_b += b[c * 4 + r] * b[d * 4 + r];
            }
            if (!near_eq(dot_a, dot_b, 2e-4f)) return false;
        }
    }
    return true;
}

void test_ik_weight_preserves_bone_length() {
    auto skeleton = make_ik_skeleton(2);
    auto chain = make_ik_chain(1, 2, {0, 1, 0});
    chain.weight = 0.5f;
    auto pose = solve_ik_pose(skeleton, chain);
    const auto end = mat4_translation(pose.back());
    check(near_eq(end[0], std::sqrt(0.5f)) && near_eq(end[1], std::sqrt(0.5f)) && near_eq(end[2], 0),
          "IK: half-weight 90deg correction is a length-preserving 45deg rotation");
    chain.target = {-1, 0, 0};
    pose = solve_ik_pose(skeleton, chain);
    check(near_eq(point_distance(mat4_translation(pose.front()), mat4_translation(pose.back())), 1.0f) &&
              same_linear_metric(pose.front(), kIdentity),
          "IK: half-weight 180deg correction remains nonsingular and preserves length");

    skeleton.nodes[0].local = compose_trs({3, 4, 5}, quat_from_axis_angle({0, 0, 1}, 0.3f), {-2, 3, 0.5f});
    chain.target = {3, 6, 5};
    const auto locals = ik_locals(skeleton);
    std::unordered_map<int, std::array<float, 16>> overrides;
    solve_ccd(skeleton, chain, locals, overrides);
    check(overrides.contains(0) && same_linear_metric(locals[0], overrides.at(0)) &&
              mat4_translation(locals[0]) == mat4_translation(overrides.at(0)),
          "IK: blending preserves input translation and mirrored nonuniform scale");

    skeleton = make_ik_skeleton(3);
    skeleton.nodes[0].local = compose_trs({0, 0, 0}, {0, 0, 0, 1}, {2, 3, 1});
    skeleton.nodes[1].local = compose_trs({1, 0, 0}, {0, 0, 0, 1}, {1, 2, 0.5f});
    chain = make_ik_chain(2, 2, {2, 3, 0});
    solve_ccd(skeleton, chain, ik_locals(skeleton), overrides);
    check(overrides.contains(1) && same_linear_metric(skeleton.nodes[1].local, overrides.at(1)) &&
              mat4_translation(skeleton.nodes[1].local) == mat4_translation(overrides.at(1)),
          "IK: nonuniform parent scale does not inject local scale or shear");
}

void test_ik_straight_chain() {
    auto skeleton = make_ik_skeleton(3);
    auto chain = make_ik_chain(2, 3, {1.5f, 0, 0});
    chain.max_iterations = 10;
    auto pose = solve_ik_pose(skeleton, chain);
    check(point_distance(mat4_translation(pose.back()), chain.target) <= chain.tolerance,
          "IK: straight two-segment chain bends toward a reachable collinear target");
    check(near_eq(point_distance(mat4_translation(pose[0]), mat4_translation(pose[1])), 1.0f) &&
              near_eq(point_distance(mat4_translation(pose[1]), mat4_translation(pose[2])), 1.0f),
          "IK: straight-chain recovery preserves both segment lengths");
    const auto repeated = solve_ik_pose(skeleton, chain);
    check(mat_near(pose[0], repeated[0]) && mat_near(pose[1], repeated[1]),
          "IK: straight-chain bend direction is deterministic");

    chain.target = {4, 0, 0};
    pose = solve_ik_pose(skeleton, chain);
    check(near_eq(mat4_translation(pose.back())[0], 2.0f) && same_linear_metric(pose[0], kIdentity),
          "IK: unreachable outward target leaves a straight chain extended");
    chain.target = {0.75f, 1.25f, 0.5f};
    chain.max_iterations = 100;
    pose = solve_ik_pose(skeleton, chain);
    check(point_distance(mat4_translation(pose.back()), chain.target) <= chain.tolerance,
          "IK: ordinary non-collinear 3D target still converges");
}

void test_ik_model_space_tolerance() {
    auto skeleton = make_ik_skeleton(2);
    skeleton.global_inverse = compose_trs({3, -4, 2}, {0, 0, 0, 1}, {100, 100, 100});
    auto chain = make_ik_chain(1, 2, {103, -3.99f, 2});
    chain.tolerance = 0.005f;
    auto pose = solve_ik_pose(skeleton, chain);
    check(point_distance(mat4_translation(pose.back()), chain.target) <= chain.tolerance,
          "IK: tolerance is checked after global_inverse in output model space");

    skeleton.global_inverse = compose_trs({0, 0, 0}, {0, 0, 0, 1}, {1e-5f, 1e-5f, 1e-5f});
    chain.target = {0, 1e-5f, 0};
    chain.tolerance = 1e-7f;
    pose = solve_ik_pose(skeleton, chain);
    check(point_distance(mat4_translation(pose.back()), chain.target) <= chain.tolerance,
          "IK: small but invertible import normalization is accepted");
}

void test_ik_input_guards() {
    auto skeleton = make_ik_skeleton(3);
    auto chain = make_ik_chain(2, 3, {1, 1, 0});
    std::unordered_map<int, std::array<float, 16>> overrides;
    auto reject = [&](const char* name) {
        overrides[42] = kIdentity;
        solve_ccd(skeleton, chain, ik_locals(skeleton), overrides);
        check(overrides.empty(), name);
    };
    chain.weight = 0;
    reject("IK: zero weight produces no overrides");
    chain.weight = 1;
    chain.damping = 0;
    reject("IK: zero damping produces no overrides");
    chain.damping = 1;
    chain.max_iterations = 0;
    reject("IK: zero iterations produces no overrides");
    chain.max_iterations = 100;
    chain.target[0] = std::numeric_limits<float>::quiet_NaN();
    reject("IK: non-finite target is rejected");
    chain.target = {1, 1, 0};
    chain.weight = std::numeric_limits<float>::infinity();
    reject("IK: non-finite weight is rejected");
    chain.weight = 1;
    skeleton.global_inverse[0] = 0;
    reject("IK: singular model-space conversion is rejected");
    skeleton.global_inverse = kIdentity;
    skeleton.nodes[0].parent = 2;
    chain.chain_length = std::numeric_limits<int>::max();
    reject("IK: cyclic ancestry with huge chain length terminates and is rejected");
    skeleton.nodes[0].parent = 999;
    chain.chain_length = 2;
    reject("IK: invalid ancestor above the chain root is rejected");
    skeleton.nodes[0].parent = -1;
    skeleton.nodes[0].local[0] = 0;
    reject("IK: singular fixed parent transform is rejected");
}

}  // namespace

int main() {
    std::printf("=== Animation Pose / CCD Unit Tests ===\n");
    test_mat4_mul();
    test_compose_trs();
    test_slerp_endpoints();
    test_advance_time_loops();
    test_bind_pose_identity_single();
    test_bind_pose_identity_hierarchy();
    test_ik_weight_preserves_bone_length();
    test_ik_straight_chain();
    test_ik_model_space_tolerance();
    test_ik_input_guards();

    std::printf("\n=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
