#pragma once
// ============================================================================
// 骨骼动画运行时求值：FK（P1）+ IK（CCD）
//
// FK —— 纯函数：给定 SkeletonData + AnimationClip + 时间，算出每根骨骼的最终矩阵
//   final[id] = global_inverse * global(node) * offset
// 供 P2 的 CPU 蒙皮使用：skinned[v] = Σ wᵢ · (final[idᵢ] · bind[v])。
//
// IK —— CCD（循环坐标下降）：给定一条链和目标点，反解出链上关节的新 local 旋转，
//   经 compute_pose 的 local_overrides 注入 FK，叠加在原动画姿态之上。
//   CCD 每步产出的就是关节局部旋转，与本模块的 FK 表示同源，无需额外反解。
//
// 矩阵全程列主序 std::array<float,16>（下标 = col*4 + row），与 scene.h 的
// BoneInfo::offset / BoneNode::local / SkeletonData::global_inverse 约定一致，
// 也与 ktm/glsl 一致。四元数统一 (x,y,z,w)。本模块不依赖 ktm，便于独立单测。
// ============================================================================
#include <array>
#include <unordered_map>
#include <vector>

#include "corona/resource/types/scene.h"

namespace Corona::Resource {

/// 列主序 4x4 矩阵乘法 C = A * B（下标 col*4+row）。
[[nodiscard]] std::array<float, 16> mat4_mul(const std::array<float, 16>& a,
                                             const std::array<float, 16>& b);

/// 由 平移/四元数(x,y,z,w)/缩放 合成局部变换矩阵 M = T * R * S（列主序）。
[[nodiscard]] std::array<float, 16> compose_trs(const std::array<float, 3>& translation,
                                                const std::array<float, 4>& rotation_xyzw,
                                                const std::array<float, 3>& scale);

/// 在给定时间（tick）采样一个动画通道，返回该骨骼的局部变换矩阵（列主序）。
/// 位置/缩放用线性插值，旋转用四元数 Slerp。单关键帧直接返回该值。
[[nodiscard]] std::array<float, 16> sample_channel(const AnimChannel& channel, float time_ticks);

/// 推进动画时间并自动循环：返回 fmod(current + tps*dt, duration)。
/// duration<=0 时返回 0。
[[nodiscard]] float advance_anim_time(float current_ticks, float dt_seconds,
                                      const AnimationClip& clip);

/// 计算给定 clip 在给定时间（tick）下的最终骨骼矩阵。
/// out_finals 会被 resize 到 skeleton.bone_count，每个元素列主序 mat4。
/// 未被动画驱动的节点使用其绑定姿态 local；未出现在 bone_map 的节点不写 final。
///
/// @param local_overrides 可选：node_idx → 覆盖用的 local 矩阵。命中的节点用此覆盖值
///        替代「动画采样 / 绑定姿态」的 local（IK 结果注入点）。nullptr 时行为不变。
/// @param precomputed_locals 可选：已预先采样好的每节点 local（下标对齐 skeleton.nodes）。
///        非 nullptr 时跳过内部 sample_pose_locals，直接用此数组——避免 IK 路径下的重复采样。
void compute_pose(const SkeletonData& skeleton,
                  const AnimationClip& clip,
                  float time_ticks,
                  std::vector<std::array<float, 16>>& out_finals,
                  const std::unordered_map<int, std::array<float, 16>>* local_overrides = nullptr,
                  const std::vector<std::array<float, 16>>* precomputed_locals = nullptr);

/// 采样一个 clip 在给定时间下每个节点的 local 变换（下标对齐 skeleton.nodes）。
/// 有动画通道的节点采样得到，其余用绑定姿态 node.local。out_locals resize 到节点数。
/// 这是 compute_pose 的前半步，单独暴露供 IK 构造 base_locals 用。
void sample_pose_locals(const SkeletonData& skeleton,
                        const AnimationClip& clip,
                        float time_ticks,
                        std::vector<std::array<float, 16>>& out_locals);

// ============================================================================
// IK（CCD）
// ============================================================================

/// 由旋转轴（需已归一化）和角度（弧度）构造四元数 (x,y,z,w)。
[[nodiscard]] std::array<float, 4> quat_from_axis_angle(const std::array<float, 3>& axis, float angle);

/// 四元数乘法 (x,y,z,w)：返回 a ⊗ b（先施加 b 再施加 a 的复合旋转）。
[[nodiscard]] std::array<float, 4> quat_mul(const std::array<float, 4>& a, const std::array<float, 4>& b);

/// 求把单位向量 from 旋到单位向量 to 的最短弧四元数 (x,y,z,w)。
/// 反向（≈180°）时取任意垂直轴；共线同向返回单位四元数。
[[nodiscard]] std::array<float, 4> rotation_between(const std::array<float, 3>& from,
                                                    const std::array<float, 3>& to);

/// 取列主序 4x4 矩阵的平移分量（第 3 列 xyz）。
[[nodiscard]] std::array<float, 3> mat4_translation(const std::array<float, 16>& m);

/// CCD 求解一条 IK 链。
/// @param skeleton     骨架（提供 parent 链、bone local）。
/// @param chain        链定义 + 目标点 + 迭代参数（见 IkChain）。
/// @param base_locals  每节点的「当前」local（动画采样后），下标对齐 skeleton.nodes。
///                     求解不改它，只读作起点。
/// @param out_overrides 输出：被改写关节的 node_idx → 新 local 矩阵，可直接喂给
///                      compute_pose 的 local_overrides。链无效/未启用时清空。
void solve_ccd(const SkeletonData& skeleton,
               const IkChain& chain,
               const std::vector<std::array<float, 16>>& base_locals,
               std::unordered_map<int, std::array<float, 16>>& out_overrides);

}  // namespace Corona::Resource
