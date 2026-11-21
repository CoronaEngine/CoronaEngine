#pragma once

/// 实体组件的直接访问器 —— 取代 `EntityStorage<T>` 与 `acquire_*` 那一套。
///
/// 迁移约定：
/// - `EntityId` 就是 entt 实体句柄（`integral + 1`，0 保留给无效），不再有
///   "槽位地址"语义；
/// - 组件直接挂在实体上，没有 `Holder` 间接层，也没有条带锁；
/// - 遍历用 `view<T>()`，顺序不保证（调用方本来就只做过滤式遍历）。
///
/// 线程约定：写只能在 DAG 线程；跨线程读走快照通道。

#include <corona/ecs/world.h>

#include <corona/kernel/entity_id.h>

#include <utility>

namespace Corona::Ecs {

/// entt 实体 → 对外句柄（0 保留给无效）。
[[nodiscard]] inline EntityId to_handle(Entity entity) noexcept {
    return static_cast<EntityId>(entt::to_integral(entity)) + 1;
}

/// 对外句柄 → entt 实体。
[[nodiscard]] inline Entity to_entity(EntityId handle) noexcept {
    if (handle == kInvalidEntity) {
        return kNullEntity;
    }
    return static_cast<Entity>(static_cast<std::uint64_t>(handle) - 1);
}

/// 句柄是否对应一个存活实体。
[[nodiscard]] inline bool alive(EntityId handle) noexcept {
    return World::instance().alive(to_entity(handle));
}

/// 取组件；实体无效或没有该组件时返回 nullptr。
template <typename T>
[[nodiscard]] T* try_get(EntityId handle) {
    const Entity entity = to_entity(handle);
    return entity == kNullEntity ? nullptr : World::instance().try_get<T>(entity);
}

template <typename T>
[[nodiscard]] const T* try_get_const(EntityId handle) {
    const Entity entity = to_entity(handle);
    return entity == kNullEntity ? nullptr : World::instance().try_get<T>(entity);
}

/// 取组件；调用方保证存在（否则抛 entt 的断言）。
template <typename T>
[[nodiscard]] T& get(EntityId handle) {
    return World::instance().get<T>(to_entity(handle));
}

/// 创建实体并挂上组件，返回句柄。
template <typename T>
[[nodiscard]] EntityId emplace() {
    const Entity entity = World::instance().create<T>();
    return to_handle(entity);
}

/// 销毁实体（连同它的全部组件）。
inline void destroy(EntityId handle) {
    World::instance().destroy(to_entity(handle));
}

/// 网络同步用的稳定序号：实体索引（与旧 `EntityStorage::seq_id` 完全一致）。
[[nodiscard]] inline std::int64_t seq_of(EntityId handle) noexcept {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(handle) - 1);
}

/// 指定组件的视图（遍历时用 `entity` 取回句柄：`to_handle(entity)`）。
template <typename... T>
[[nodiscard]] auto view() {
    return World::instance().view<T...>();
}

}  // namespace Corona::Ecs
