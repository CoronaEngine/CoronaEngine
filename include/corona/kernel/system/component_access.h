#pragma once

/// 组件访问声明：DAG 的依赖边不再只靠手写 `after`，而是由"读/写哪些组件"推导。
///
/// 设计动机（见 docs/ecs-entt-migration-analysis.md）：
/// 现在的 `FrameNodeDesc::dependencies` 是人工维护的拓扑，改一个系统就要重算边，
/// 而且无法回答"这两个系统会不会写同一份数据"。ECS 化之后，系统的输入输出就是
/// 组件集合，于是可以把调度契约变成数据声明：
///
/// ```cpp
/// FrameNodeDesc desc;
/// desc.name = "Mechanics";
/// desc.access.write<Transform, RigidBody>().read<MeshSet>();
/// ```
///
/// 规则（`compile()` 按节点声明序推导）：
/// - 声明序在前的节点写、在后的节点读或写同一组件 → 连边（RAW / WAW）。
/// - 声明序在前的节点只读、在后的节点写同一组件 → **不连边**：前者已经执行完，
///   写后读不会撕裂（WAR 在单 tick 串行模型下无危害）。
/// - `structural`（创建/销毁实体）与任何有访问的节点互斥，等价于旧的 tick 边界。
///
/// 推导出的边**只增不减**：手写 `dependencies` 仍然有效，两者合并后再做拓扑排序，
/// 因此迁移期可以逐系统切换，不必一次改完。
///
/// 该头只依赖 STL，不引入 entt —— 内核层不应该知道 ECS 容器的存在。

#include <algorithm>
#include <cstddef>
#include <typeindex>
#include <utility>
#include <vector>

namespace Corona::Kernel {

/**
 * @brief 一个系统声明自己读/写哪些组件类型
 *
 * 只存 `std::type_index`，用于建图；不持有数据，也不参与运行期查找。
 */
struct ComponentAccess {
    std::vector<std::type_index> reads;
    std::vector<std::type_index> writes;

    /// 创建/销毁实体：与任何有访问声明的节点互斥。
    bool structural{false};

    template <typename... Components>
    ComponentAccess& read() {
        (reads.emplace_back(typeid(Components)), ...);
        return *this;
    }

    template <typename... Components>
    ComponentAccess& write() {
        (writes.emplace_back(typeid(Components)), ...);
        return *this;
    }

    ComponentAccess& mark_structural(bool value = true) {
        structural = value;
        return *this;
    }

    [[nodiscard]] bool empty() const noexcept {
        return reads.empty() && writes.empty() && !structural;
    }

    /// 是否声明了对该组件的访问（读或写）
    [[nodiscard]] bool touches(const std::type_index& component) const {
        return std::ranges::find(reads, component) != reads.end() ||
               std::ranges::find(writes, component) != writes.end();
    }
};

/**
 * @brief 两个系统之间是否需要一条"先 a 后 b"的边
 *
 * 调用方负责保证 a 在声明序上早于 b；本函数只回答数据冲突。
 */
[[nodiscard]] inline bool access_conflicts(const ComponentAccess& a,
                                           const ComponentAccess& b) {
    if (a.empty() || b.empty()) {
        return false;
    }

    // 结构变更影响一切：与任何有访问的节点互斥（等价于 tick 边界）。
    if (a.structural || b.structural) {
        return true;
    }

    for (const auto& written : a.writes) {
        if (b.touches(written)) {
            return true;
        }
    }
    return false;
}

}  // namespace Corona::Kernel
