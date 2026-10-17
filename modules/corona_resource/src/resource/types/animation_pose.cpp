#include "corona/resource/types/animation_pose.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace Corona::Resource {

namespace {

constexpr std::array<float, 16> kIdentity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

/// 在排序关键帧序列中找到包围 time 的左侧下标 i（keys[i].time <= time < keys[i+1].time）。
/// time 早于首帧返回 0；晚于末帧返回 size-2（与末帧插值因子=1）。
template <typename KeyVec>
std::size_t find_key_index(const KeyVec& keys, float time) {
    // keys 非空且 size>=2 由调用方保证
    for (std::size_t i = 0; i + 1 < keys.size(); ++i) {
        if (time < keys[i + 1].first) {
            return i;
        }
    }
    return keys.size() - 2;
}

/// 归一化插值因子 [0,1]：(time - t0) / (t1 - t0)，分母为 0 时返回 0。
float blend_factor(float time, float t0, float t1) {
    float denom = t1 - t0;
    if (denom <= 0.0f) return 0.0f;
    float f = (time - t0) / denom;
    return std::clamp(f, 0.0f, 1.0f);
}

std::array<float, 3> lerp_vec3(const std::array<float, 3>& a, const std::array<float, 3>& b, float t) {
    return {a[0] + (b[0] - a[0]) * t,
            a[1] + (b[1] - a[1]) * t,
            a[2] + (b[2] - a[2]) * t};
}

/// 四元数 (x,y,z,w) 球面插值，自动选短弧。接近共线时退化为归一化线性插值。
std::array<float, 4> slerp_quat(std::array<float, 4> a, std::array<float, 4> b, float t) {
    float cos_theta = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    // 选短弧
    if (cos_theta < 0.0f) {
        b = {-b[0], -b[1], -b[2], -b[3]};
        cos_theta = -cos_theta;
    }

    std::array<float, 4> result;
    if (cos_theta > 0.9995f) {
        // 近共线：线性插值后归一化，避免 sin(theta)→0 数值问题
        result = {a[0] + (b[0] - a[0]) * t,
                  a[1] + (b[1] - a[1]) * t,
                  a[2] + (b[2] - a[2]) * t,
                  a[3] + (b[3] - a[3]) * t};
    } else {
        float theta = std::acos(cos_theta);
        float sin_theta = std::sin(theta);
        float wa = std::sin((1.0f - t) * theta) / sin_theta;
        float wb = std::sin(t * theta) / sin_theta;
        result = {a[0] * wa + b[0] * wb,
                  a[1] * wa + b[1] * wb,
                  a[2] * wa + b[2] * wb,
                  a[3] * wa + b[3] * wb};
    }

    float len = std::sqrt(result[0] * result[0] + result[1] * result[1] +
                          result[2] * result[2] + result[3] * result[3]);
    if (len <= 0.0f) return {0.0f, 0.0f, 0.0f, 1.0f};
    float inv = 1.0f / len;
    return {result[0] * inv, result[1] * inv, result[2] * inv, result[3] * inv};
}

// ---- IK 用向量/矩阵小工具（列主序 mat4，下标 col*4+row）----

std::array<float, 3> v3_sub(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
float v3_dot(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
std::array<float, 3> v3_cross(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return {a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
}
float v3_len(const std::array<float, 3>& v) {
    return std::sqrt(v3_dot(v, v));
}
/// 归一化；零向量返回 {0,0,0}（调用方需自行判退化）。
std::array<float, 3> v3_normalize(const std::array<float, 3>& v) {
    float l = v3_len(v);
    if (l <= 1e-8f) return {0.0f, 0.0f, 0.0f};
    float inv = 1.0f / l;
    return {v[0] * inv, v[1] * inv, v[2] * inv};
}

template <std::size_t N>
bool all_finite(const std::array<float, N>& values) {
    return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); });
}

/// 一般 4x4 逆矩阵（列主序）。拒绝奇异/非有限输入，不能用单位阵掩盖无效空间变换。
bool mat4_inverse(const std::array<float, 16>& m, std::array<float, 16>& result) {
    if (!all_finite(m)) return false;
    std::array<float, 16> inv{};
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
             m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
             m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
              m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
              m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!std::isfinite(det) || det == 0.0f) return false;
    float inv_det = 1.0f / det;
    for (int i = 0; i < 16; ++i) inv[i] *= inv_det;
    if (!all_finite(inv)) return false;
    result = inv;
    return true;
}

std::array<float, 3> transform_point(const std::array<float, 16>& m, const std::array<float, 3>& p) {
    return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
            m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
            m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}

}  // namespace

std::array<float, 16> mat4_mul(const std::array<float, 16>& a, const std::array<float, 16>& b) {
    // 列主序：C[col*4+row] = Σ_k A[k*4+row] * B[col*4+k]
    std::array<float, 16> c{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            c[col * 4 + row] = sum;
        }
    }
    return c;
}

std::array<float, 16> compose_trs(const std::array<float, 3>& t,
                                  const std::array<float, 4>& q,
                                  const std::array<float, 3>& s) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;

    // 旋转矩阵元素 R[row][col]
    const float r00 = 1.0f - 2.0f * (yy + zz);
    const float r01 = 2.0f * (xy - wz);
    const float r02 = 2.0f * (xz + wy);
    const float r10 = 2.0f * (xy + wz);
    const float r11 = 1.0f - 2.0f * (xx + zz);
    const float r12 = 2.0f * (yz - wx);
    const float r20 = 2.0f * (xz - wy);
    const float r21 = 2.0f * (yz + wx);
    const float r22 = 1.0f - 2.0f * (xx + yy);

    // M = T * R * S，列主序：列 c (c<3) = R 第 c 列 * scale[c]，列 3 = 平移
    std::array<float, 16> m{};
    m[0] = r00 * s[0];  m[1] = r10 * s[0];  m[2] = r20 * s[0];  m[3] = 0.0f;   // 列 0
    m[4] = r01 * s[1];  m[5] = r11 * s[1];  m[6] = r21 * s[1];  m[7] = 0.0f;   // 列 1
    m[8] = r02 * s[2];  m[9] = r12 * s[2];  m[10] = r22 * s[2]; m[11] = 0.0f;  // 列 2
    m[12] = t[0];       m[13] = t[1];       m[14] = t[2];       m[15] = 1.0f;  // 列 3
    return m;
}

std::array<float, 16> sample_channel(const AnimChannel& channel, float time_ticks) {
    // 位置
    std::array<float, 3> pos{0.0f, 0.0f, 0.0f};
    if (channel.positions.size() == 1) {
        pos = channel.positions[0].second;
    } else if (channel.positions.size() >= 2) {
        std::size_t i = find_key_index(channel.positions, time_ticks);
        float f = blend_factor(time_ticks, channel.positions[i].first, channel.positions[i + 1].first);
        pos = lerp_vec3(channel.positions[i].second, channel.positions[i + 1].second, f);
    }

    // 旋转（四元数 Slerp）
    std::array<float, 4> rot{0.0f, 0.0f, 0.0f, 1.0f};
    if (channel.rotations.size() == 1) {
        rot = channel.rotations[0].second;
    } else if (channel.rotations.size() >= 2) {
        std::size_t i = find_key_index(channel.rotations, time_ticks);
        float f = blend_factor(time_ticks, channel.rotations[i].first, channel.rotations[i + 1].first);
        rot = slerp_quat(channel.rotations[i].second, channel.rotations[i + 1].second, f);
    }

    // 缩放
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
    if (channel.scales.size() == 1) {
        scale = channel.scales[0].second;
    } else if (channel.scales.size() >= 2) {
        std::size_t i = find_key_index(channel.scales, time_ticks);
        float f = blend_factor(time_ticks, channel.scales[i].first, channel.scales[i + 1].first);
        scale = lerp_vec3(channel.scales[i].second, channel.scales[i + 1].second, f);
    }

    return compose_trs(pos, rot, scale);
}

float advance_anim_time(float current_ticks, float dt_seconds, const AnimationClip& clip) {
    if (clip.duration <= 0.0f) return 0.0f;
    float t = current_ticks + clip.ticks_per_second * dt_seconds;
    t = std::fmod(t, clip.duration);
    if (t < 0.0f) t += clip.duration;  // 负 dt 容错
    return t;
}

void sample_pose_locals(const SkeletonData& skeleton,
                        const AnimationClip& clip,
                        float time_ticks,
                        std::vector<std::array<float, 16>>& out_locals) {
    out_locals.assign(skeleton.nodes.size(), kIdentity);
    if (skeleton.nodes.empty()) return;

    // 构建 骨骼名 → 通道 映射（仅本 clip）
    std::unordered_map<std::string, const AnimChannel*> channel_map;
    channel_map.reserve(clip.channels.size());
    for (const auto& ch : clip.channels) {
        channel_map[ch.bone_name] = &ch;
    }

    for (std::size_t i = 0; i < skeleton.nodes.size(); ++i) {
        const BoneNode& node = skeleton.nodes[i];
        auto ch_it = channel_map.find(node.name);
        // 有动画通道则采样，否则回退绑定姿态 local
        out_locals[i] = (ch_it != channel_map.end())
                            ? sample_channel(*ch_it->second, time_ticks)
                            : node.local;
    }
}

void compute_pose(const SkeletonData& skeleton,
                  const AnimationClip& clip,
                  float time_ticks,
                  std::vector<std::array<float, 16>>& out_finals,
                  const std::unordered_map<int, std::array<float, 16>>* local_overrides,
                  const std::vector<std::array<float, 16>>* precomputed_locals) {
    out_finals.assign(static_cast<std::size_t>(std::max(0, skeleton.bone_count)), kIdentity);

    if (skeleton.nodes.empty()) return;

    // locals 优先用调用方预计算的（IK 路径下避免重复采样），否则自行采样。
    std::vector<std::array<float, 16>> sampled;
    const std::vector<std::array<float, 16>>* locals_ptr = precomputed_locals;
    if (!locals_ptr || locals_ptr->size() != skeleton.nodes.size()) {
        sample_pose_locals(skeleton, clip, time_ticks, sampled);
        locals_ptr = &sampled;
    }

    // 递归层级累乘。用显式栈避免深骨架递归过深。
    struct StackItem {
        int node_idx;
        std::array<float, 16> parent_global;
    };
    std::vector<StackItem> stack;
    stack.push_back({skeleton.root, kIdentity});

    while (!stack.empty()) {
        StackItem item = stack.back();
        stack.pop_back();

        if (item.node_idx < 0 || item.node_idx >= static_cast<int>(skeleton.nodes.size())) {
            continue;
        }
        const BoneNode& node = skeleton.nodes[static_cast<std::size_t>(item.node_idx)];

        // 局部变换：override 命中优先（IK 注入点），否则用采样/绑定姿态
        std::array<float, 16> local = (*locals_ptr)[static_cast<std::size_t>(item.node_idx)];
        if (local_overrides) {
            auto ov_it = local_overrides->find(item.node_idx);
            if (ov_it != local_overrides->end()) {
                local = ov_it->second;
            }
        }

        std::array<float, 16> global = mat4_mul(item.parent_global, local);

        // 若该节点是受蒙皮影响的骨骼，写最终矩阵
        auto bone_it = skeleton.bone_map.find(node.name);
        if (bone_it != skeleton.bone_map.end()) {
            std::int32_t id = bone_it->second.id;
            if (id >= 0 && id < static_cast<std::int32_t>(out_finals.size())) {
                // final = global_inverse * global * offset
                std::array<float, 16> tmp = mat4_mul(skeleton.global_inverse, global);
                out_finals[static_cast<std::size_t>(id)] = mat4_mul(tmp, bone_it->second.offset);
            }
        }

        for (int child : node.children) {
            stack.push_back({child, global});
        }
    }
}

// ============================================================================
// IK（CCD）实现
// ============================================================================

std::array<float, 4> quat_from_axis_angle(const std::array<float, 3>& axis, float angle) {
    float half = angle * 0.5f;
    float s = std::sin(half);
    return {axis[0] * s, axis[1] * s, axis[2] * s, std::cos(half)};
}

std::array<float, 4> quat_mul(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    // (x,y,z,w) Hamilton 积 a ⊗ b
    const float ax = a[0], ay = a[1], az = a[2], aw = a[3];
    const float bx = b[0], by = b[1], bz = b[2], bw = b[3];
    return {
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz};
}

std::array<float, 4> rotation_between(const std::array<float, 3>& from,
                                      const std::array<float, 3>& to) {
    std::array<float, 3> f = v3_normalize(from);
    std::array<float, 3> t = v3_normalize(to);
    // 任一退化为零向量：无从定义旋转，返回单位四元数
    if (v3_len(f) < 0.5f || v3_len(t) < 0.5f) return {0.0f, 0.0f, 0.0f, 1.0f};

    float d = std::clamp(v3_dot(f, t), -1.0f, 1.0f);
    const auto cross = v3_cross(f, t);
    const float cross_length = v3_len(cross);
    if (cross_length < 1e-7f && d >= 0.0f) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    if (cross_length < 1e-7f) {
        // 近反向（≈180°）：绕任意垂直轴转 π。先试与 X 轴叉乘，退化再用 Y 轴。
        std::array<float, 3> axis = v3_cross({1.0f, 0.0f, 0.0f}, f);
        if (v3_len(axis) < 1e-4f) axis = v3_cross({0.0f, 1.0f, 0.0f}, f);
        axis = v3_normalize(axis);
        return quat_from_axis_angle(axis, 3.14159265358979f);
    }
    std::array<float, 3> axis = v3_normalize(cross);
    float angle = std::atan2(cross_length, d);
    return quat_from_axis_angle(axis, angle);
}

std::array<float, 3> mat4_translation(const std::array<float, 16>& m) {
    return {m[12], m[13], m[14]};
}

namespace {
/// 从四元数 (x,y,z,w) 构造纯旋转 mat4（列主序）。复用 compose_trs（零平移、单位缩放）。
std::array<float, 16> quat_to_mat4(const std::array<float, 4>& q) {
    return compose_trs({0.0f, 0.0f, 0.0f}, q, {1.0f, 1.0f, 1.0f});
}

// Only rotate the linear part. Keeping the input translation and its complete
// scale/shear factor avoids changing bone offsets, including mirrored rigs.
std::array<float, 16> rotate_local(const std::array<float, 16>& base,
                                  const std::array<float, 4>& rotation) {
    auto result = mat4_mul(quat_to_mat4(rotation), base);
    for (int i = 12; i < 16; ++i) result[i] = base[i];
    return result;
}
}  // namespace

void solve_ccd(const SkeletonData& skeleton,
               const IkChain& chain,
               const std::vector<std::array<float, 16>>& base_locals,
               std::unordered_map<int, std::array<float, 16>>& out_overrides) {
    out_overrides.clear();

    const int node_count = static_cast<int>(skeleton.nodes.size());
    if (!chain.enabled || node_count == 0) return;
    if (chain.end_node < 0 || chain.end_node >= node_count) return;
    if (base_locals.size() != skeleton.nodes.size()) return;
    if (chain.chain_length < 2 || chain.max_iterations <= 0) return;
    if (!all_finite(chain.target) || !std::isfinite(chain.weight) ||
        !std::isfinite(chain.damping) || !std::isfinite(chain.tolerance)) return;
    const float weight = std::clamp(chain.weight, 0.0f, 1.0f);
    const float damping = std::clamp(chain.damping, 0.0f, 1.0f);
    if (weight == 0.0f || damping == 0.0f) return;
    const float tolerance = std::max(chain.tolerance, 0.0f);
    const int max_iterations = std::min(chain.max_iterations, 256);

    // target 以模型空间给出（与 compute_pose 输出的蒙皮网格同空间）。但 CCD 内部所有 global
    // 都在「节点层级 global 空间」（parent·local 累积，未乘 global_inverse）。二者相差一个
    // global_inverse，故先用 inverse(global_inverse)=root_global 把 target 变换回节点空间，
    // 使末端 E 与 target 在同一空间比较。global_inverse=单位阵时此步为恒等，无副作用。
    std::array<float, 16> root_global;
    if (!mat4_inverse(skeleton.global_inverse, root_global)) return;
    const auto target_node = transform_point(root_global, chain.target);
    if (!all_finite(target_node)) return;

    // Validate the complete ancestry, including ancestors above the IK root.
    // A cycle must not grow a vector up to an untrusted chain_length.
    std::vector<int> ancestry;
    std::vector<bool> visited(skeleton.nodes.size(), false);
    int cur = chain.end_node;
    while (cur >= 0) {
        if (cur >= node_count || visited[static_cast<std::size_t>(cur)] ||
            !all_finite(base_locals[static_cast<std::size_t>(cur)])) return;
        visited[static_cast<std::size_t>(cur)] = true;
        ancestry.push_back(cur);
        cur = skeleton.nodes[static_cast<std::size_t>(cur)].parent;
    }
    const int L = std::min(chain.chain_length, static_cast<int>(ancestry.size()));
    if (L < 2) return;  // 链太短（末端已是根附近），无可转关节
    std::vector<int> joints(ancestry.begin(), ancestry.begin() + L);
    std::reverse(joints.begin(), joints.end());

    // 链根之上（IK 不改）的固定父 global
    auto root_parent_global = kIdentity;
    for (int k = static_cast<int>(ancestry.size()) - 1; k >= L; --k) {
        root_parent_global = mat4_mul(root_parent_global, base_locals[static_cast<std::size_t>(ancestry[k])]);
    }

    // 链上每关节的可写 local（起点 = base_locals），以及当前 global
    std::vector<std::array<float, 16>> local_work(static_cast<std::size_t>(L));
    std::vector<std::array<float, 16>> gchain(static_cast<std::size_t>(L));
    for (int k = 0; k < L; ++k) {
        local_work[static_cast<std::size_t>(k)] = base_locals[static_cast<std::size_t>(joints[static_cast<std::size_t>(k)])];
    }
    auto rebuild_from = [&](int start) {
        for (int k = start; k < L; ++k) {
            const std::array<float, 16>& pg =
                (k == 0) ? root_parent_global : gchain[static_cast<std::size_t>(k - 1)];
            gchain[static_cast<std::size_t>(k)] = mat4_mul(pg, local_work[static_cast<std::size_t>(k)]);
        }
    };
    rebuild_from(0);

    const std::array<float, 4> q_identity{0.0f, 0.0f, 0.0f, 1.0f};
    std::vector<std::array<float, 4>> rotations(static_cast<std::size_t>(L), q_identity);
    auto apply_rotation = [&](int j, const std::array<float, 4>& dq) {
        auto& rotation = rotations[static_cast<std::size_t>(j)];
        // slerp normalizes the product as well as choosing a consistent unit quaternion.
        rotation = slerp_quat(q_identity, quat_mul(dq, rotation), 1.0f);
        local_work[static_cast<std::size_t>(j)] =
            rotate_local(base_locals[static_cast<std::size_t>(joints[j])], rotation);
        rebuild_from(j);
    };
    auto model_error = [&]() {
        const auto end_model = transform_point(skeleton.global_inverse, mat4_translation(gchain.back()));
        return v3_len(v3_sub(end_model, chain.target));
    };
    float best_error = model_error();
    if (!std::isfinite(best_error) || best_error <= tolerance) return;
    auto best_rotations = rotations;

    // A straight chain and an inward collinear target give every CCD joint an
    // identity rotation. Seed one bend in a deterministic plane; subsequent CCD
    // still decides the final pose. Do not bend an already satisfied/outward chain.
    if (L >= 3) {
        const auto root = mat4_translation(gchain.front());
        const auto root_to_end = v3_sub(mat4_translation(gchain.back()), root);
        const auto root_to_target = v3_sub(target_node, root);
        const float end_distance = v3_len(root_to_end);
        const float target_distance = v3_len(root_to_target);
        const auto direction = v3_normalize(root_to_end);
        bool straight = end_distance > 1e-8f && target_distance < end_distance &&
                        v3_dot(direction, v3_normalize(root_to_target)) > 0.999999f;
        for (int k = 1; k < L && straight; ++k) {
            const auto segment = v3_sub(mat4_translation(gchain[k]), mat4_translation(gchain[k - 1]));
            straight = v3_len(segment) <= 1e-8f || v3_dot(v3_normalize(segment), direction) > 0.999999f;
        }
        if (straight) {
            for (int j = L - 2; j >= 1; --j) {
                const auto joint = mat4_translation(gchain[j]);
                const float a = v3_len(v3_sub(joint, root));
                const float b = v3_len(v3_sub(mat4_translation(gchain.back()), joint));
                if (a <= 1e-8f || b <= 1e-8f) continue;
                std::array<float, 16> inverse_parent;
                if (!mat4_inverse(gchain[j - 1], inverse_parent)) return;
                const auto local_direction = v3_normalize(v3_sub(
                    transform_point(inverse_parent, mat4_translation(gchain.back())),
                    mat4_translation(local_work[j])));
                auto axis = v3_cross({0.0f, 0.0f, 1.0f}, local_direction);
                if (v3_len(axis) < 1e-4f) axis = v3_cross({0.0f, 1.0f, 0.0f}, local_direction);
                const float angle = std::acos(std::clamp(
                    (target_distance * target_distance - a * a - b * b) / (2.0f * a * b), -1.0f, 1.0f));
                apply_rotation(j, quat_from_axis_angle(v3_normalize(axis), angle * damping));
                // Align the newly bent chain before another distal CCD step can
                // undo the seed. For a rigid two-segment chain this also reaches
                // the target directly (subject to damping).
                if (!mat4_inverse(root_parent_global, inverse_parent)) return;
                const auto root_local = mat4_translation(local_work.front());
                const auto align = rotation_between(
                    v3_sub(transform_point(inverse_parent, mat4_translation(gchain.back())), root_local),
                    v3_sub(transform_point(inverse_parent, target_node), root_local));
                apply_rotation(0, slerp_quat(q_identity, align, damping));
                const float error = model_error();
                if (std::isfinite(error) && error < best_error) {
                    best_error = error;
                    best_rotations = rotations;
                }
                break;
            }
        }
    }

    // CCD 迭代：每轮从最靠近末端的关节（L-2）扫到链根（0）。
    // 末端关节 joints[L-1] 绕自身原点转不改变末端位置，故不旋转它。
    for (int iter = 0; iter < max_iterations; ++iter) {
        if (best_error <= tolerance) break;
        std::array<float, 3> E = mat4_translation(gchain[static_cast<std::size_t>(L - 1)]);

        for (int j = L - 2; j >= 0; --j) {
            // Work in the joint's parent space and only update a local rotation.
            // Conjugating a world rotation through nonuniform parent scale would
            // instead inject shear/scale into the joint's local matrix.
            const auto& parent_global = (j == 0) ? root_parent_global : gchain[j - 1];
            std::array<float, 16> inverse_parent;
            if (!mat4_inverse(parent_global, inverse_parent)) return;
            const auto J = mat4_translation(local_work[j]);
            const auto to_eff = v3_normalize(v3_sub(transform_point(inverse_parent, E), J));
            const auto to_tgt = v3_normalize(v3_sub(transform_point(inverse_parent, target_node), J));
            if (v3_len(to_eff) < 0.5f || v3_len(to_tgt) < 0.5f) continue;  // 关节与末端/目标重合

            std::array<float, 4> dq = rotation_between(to_eff, to_tgt);
            if (damping < 1.0f) dq = slerp_quat(q_identity, dq, damping);  // 压步长防抖

            apply_rotation(j, dq);
            E = mat4_translation(gchain[static_cast<std::size_t>(L - 1)]);
        }
        const float error = model_error();
        if (!std::isfinite(error)) return;
        if (error < best_error) {
            best_error = error;
            best_rotations = rotations;
        }
        if (error <= tolerance) break;
    }

    // Blend rotations, never matrices: intermediate weights must not shrink a
    // rigid bone or make a half-weight 180-degree correction singular.
    for (int j = 0; j <= L - 2; ++j) {
        int node_idx = joints[static_cast<std::size_t>(j)];
        out_overrides[node_idx] = rotate_local(base_locals[static_cast<std::size_t>(node_idx)],
                                               slerp_quat(q_identity, best_rotations[j], weight));
    }
}

}  // namespace Corona::Resource
