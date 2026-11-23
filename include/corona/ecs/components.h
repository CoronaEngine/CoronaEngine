#pragma once

/// 细粒度组件定义（ECS 化 E0/E1）。
///
/// 与旧的 12 个 `*Device` 聚合体不同，这里按"能力"切分：一个 actor 是一个实体，
/// 它的变换、包围盒、可见性、场景归属、Profile 归属分别是一个组件。系统用
/// `view<A, B>()` 组合它们，而不是先查 actor → 再查 profile → 再查 geometry 句柄。
///
/// 迁移期约定：
/// - 这些组件与 `SharedDataHub` 的 device **双写**，device 仍是既有调用点的数据源；
/// - 新代码只读写组件；旧的 `*_storage()` 调用点按阶段逐个退场；
/// - 组件类型一律可平凡拷贝/移动（entt swap-and-pop 会搬移组件）。
///
/// 线程约定：registry 无锁，组件只在 DAG 线程写；跨线程读走快照。

#include <corona/ecs/world.h>

#include <corona/kernel/entity_id.h>

#include <ktm/ktm.h>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace Corona::Ecs {

// ============================================================================
// 身份与层级
// ============================================================================

/// 外部可见的 actor 标识（与旧 `ActorDevice` 一一对应）。
struct ActorTag {
    std::uint64_t actor_id{0};
};

/// profile 实体 → 它所属的 actor 实体。
///
/// 一个 actor 可以挂多套 profile（材质/物理变体）；profile 是**子实体**，
/// 而不是 actor 上的一个句柄数组。
struct ProfileOf {
    Entity actor{kNullEntity};
};

/// 场景实体标记（与 `SceneDevice` 的句柄一一对应）。
struct SceneTag {
    std::uint64_t scene_id{0};
};

/// 实体属于哪个场景，以及在场景内的稳定顺序（渲染/序列化都按它排序）。
struct SceneMember {
    Entity scene{kNullEntity};
    std::uint32_t order{0};
};

/// 父子层级（显式双向，避免每次查询时反查）。
struct Parent {
    Entity value{kNullEntity};
};

struct Children {
    std::vector<Entity> value;
};

// ============================================================================
// 变换与空间
// ============================================================================

/// 可编辑 TRS + 导入带来的线性基（原 `ModelTransform`，现在是 ECS 组件本体）。
///
/// `compute_matrix()` 每 tick 的产物写入 `WorldMatrix`，消费者只读 WorldMatrix
/// 或按需调用；实现放在 shared_data_hub.cpp（避免 ktm 与 vision 头栈的
/// 重载歧义）。
struct Transform {
    // Authored linear transform, composed after editable TRS. Translation lives
    // in position so streaming/picking retain the correct actor origin.
    ktm::fmat4x4 affine_basis{ktm::fmat4x4::from_eye()};
    ktm::fvec3 position;
    ktm::fvec3 euler_rotation;
    ktm::fvec3 scale;

    Transform() {
        position.x = 0.0f;
        position.y = 0.0f;
        position.z = 0.0f;
        euler_rotation.x = 0.0f;
        euler_rotation.y = 0.0f;
        euler_rotation.z = 0.0f;
        scale.x = 1.0f;
        scale.y = 1.0f;
        scale.z = 1.0f;
    }

    [[nodiscard]] ktm::fmat4x4 compute_matrix() const;
};

/// 每 tick 计算出的世界矩阵：只写一次、被多个消费者读。
struct WorldMatrix {
    ktm::fmat4x4 value{ktm::fmat4x4::from_eye()};
};

/// 世界空间包围盒（八叉树/剔除/拾取共用）。
struct Bounds {
    ktm::fvec3 min;
    ktm::fvec3 max;
    bool valid{false};
};

// ============================================================================
// 渲染开关
// ============================================================================

/// 是否跟随相机（UI 层渲染空间）；原 `ActorDevice::follow_camera`。
struct FollowCamera {
    bool enabled{false};
};

/// actor 下的 profile 实体列表（原 `ActorDevice::profile_handles`）。
struct Profiles {
    std::vector<EntityId> handles;
};

/// actor 的模型路径（原 `ActorDevice::model_path`）。
struct ModelPath {
    std::filesystem::path value;
};

/// 几何的导入源：磁盘路径。导入选项仍留在 `GeometryDevice`（含 scene_geometry 等语义）。
struct ModelSource {
    std::string path_utf8;
};

/// 流式/驻留策略：pinned 的 actor 不参与距离剔除与 LRU 淘汰。
///
/// 从 `ActorDevice` 抽出的第一个细粒度组件：策略只影响流式系统，
/// 渲染/物理路径不再读取 actor 聚合体。
struct StreamingPolicy {
    bool pinned{false};
};

/// 是否参与渲染 / 是否接收光照（与旧 `OpticsDevice` 的两个布尔对应）。
struct Visibility {
    bool visible{true};
    bool receives_lighting{true};
};

// ============================================================================
// 资源生命周期
// ============================================================================

/// 异步导入 + GPU 构建的状态机（与 `GeometryDevice` 的状态字段对应）。
///
/// `epoch` 用于识别槽位复用：组件被 destroy 后新实体拿到同一 index 时，
/// 在途构建结果通过 epoch 比对丢弃。
struct ResidencyState {
    enum class Phase : std::uint8_t {
        Unloaded = 0,
        PendingImport = 1,
        PendingBuild = 2,
        Ready = 3,
        Failed = 4,
    };

    Phase phase{Phase::Unloaded};
    std::uint64_t epoch{0};
};

}  // namespace Corona::Ecs
