#pragma once

/// EnTT 世界（S2 骨架）。
///
/// 这是 `Storage` 迁移的**新增层**：现有 `Storage` 路径完全不受影响，
/// 只有主动使用 `World` 的代码才会进入 EnTT。
///
/// 刻意不在这里 include `shared_data_hub.h`：组件类型清单放在
/// `corona/ecs/components.h`，避免每个用到世界的 TU 都被拽进整个引擎头栈。
///
/// ---- 线程约定（迁移期间必须守住）----
/// EnTT 没有任何同步原语。`registry` 的写入必须发生在**单一线程**上
/// （通常是该组件的 owner system），跨线程读必须走快照。
/// 这也是为什么迁移顺序是「先把跨线程读改快照（S4），再切组件（S3）」。

#include <entt/entt.hpp>

#include <cstddef>
#include <utility>

namespace Corona::Ecs {

/// 实体句柄。`ENTT_ID_TYPE=uint64_t` 已在 corona_compile_config.cmake 中设定，
/// 因此与当前的 `uintptr_t` 句柄同宽，替换时不会改变任何结构体布局。
using Entity = entt::entity;

/// 无效实体。
inline constexpr Entity kNullEntity = entt::null;

class World {
   public:
    static World& instance() {
        static World world;
        return world;
    }

    [[nodiscard]] entt::registry& registry() noexcept { return registry_; }
    [[nodiscard]] const entt::registry& registry() const noexcept { return registry_; }

    /// 创建实体（并立即附加组件，避免出现"裸实体"）。
    template <typename... Components>
    Entity create() {
        const Entity entity = registry_.create();
        ++entity_count_;
        (registry_.emplace<Components>(entity), ...);
        return entity;
    }

    void destroy(Entity entity) {
        if (registry_.valid(entity)) {
            registry_.destroy(entity);
            if (entity_count_ > 0) {
                --entity_count_;
            }
        }
    }

    [[nodiscard]] bool alive(Entity entity) const { return registry_.valid(entity); }

    /// EnTT 的 registry 没有全局实体计数（`storage<entt::entity>()` 不对外提供），
    /// 而所有创建/销毁都经过本类，所以自己维护一个。
    [[nodiscard]] std::size_t size() const noexcept { return entity_count_; }

    template <typename T, typename... Args>
    T& emplace(Entity entity, Args&&... args) {
        return registry_.emplace<T>(entity, std::forward<Args>(args)...);
    }

    template <typename T>
    [[nodiscard]] T* try_get(Entity entity) {
        return registry_.try_get<T>(entity);
    }

    template <typename T>
    [[nodiscard]] const T* try_get(Entity entity) const {
        return registry_.try_get<T>(entity);
    }

    template <typename T>
    [[nodiscard]] T& get(Entity entity) {
        return registry_.get<T>(entity);
    }

    template <typename... T>
    [[nodiscard]] auto view() {
        return registry_.view<T...>();
    }

   private:
    World() = default;
    entt::registry registry_;
    std::size_t entity_count_{0};
};

}  // namespace Corona::Ecs
