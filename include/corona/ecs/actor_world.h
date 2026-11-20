#pragma once

/// actor / profile / scene 的实体模型（ECS 化 E1）。
///
/// 旧模型：actor 是 `actor_storage` 里的一条记录，profile 是另一张表里的一条记录，
/// 两者靠 `ActorDevice::profile_handles` 和 `ProfileDevice::*_handle` 手工缝合。
/// 新模型：**actor 就是一个实体**，profile 是挂在它下面的子实体，场景归属是
/// `SceneMember` 组件。
///
/// ---- 线程契约（必须遵守）----
/// EnTT 的 registry 没有任何同步。本类只允许在 **DAG 线程**调用：
/// 迁移期由 tick 边界（`apply_pending_scene_commands`）和 DAG 系统驱动，
/// CEF / 脚本 / 网络线程只能通过命令通道间接影响这里。
/// `bind_owner_thread()` / `ensure_owner_thread()` 在调试构建里会验证这一点。
///
/// ---- 与 device 的关系 ----
/// 迁移期“双写”：device 仍是既有调用点的数据源，本层维护对应的实体与关系组件。
/// 每个阶段把一类 device 的读写搬过来，最后删掉 device。

#include <corona/ecs/components.h>

#include <corona/kernel/entity_id.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Corona::Ecs {

/// 句柄（旧的 device 槽位 id）↔ 实体 的双向映射。
class EntityIndex {
   public:
    /// 绑定一个**已创建**的实体；已存在时返回旧实体。
    Entity bind(EntityId handle, Entity entity) {
        if (handle == kInvalidEntity) {
            return kNullEntity;
        }
        std::lock_guard lock(mutex_);
        auto [it, inserted] = by_handle_.try_emplace(handle, entity);
        if (inserted) {
            by_entity_.try_emplace(entity, handle);
        }
        return it->second;
    }

    [[nodiscard]] Entity find(EntityId handle) const {
        std::lock_guard lock(mutex_);
        const auto it = by_handle_.find(handle);
        return it == by_handle_.end() ? kNullEntity : it->second;
    }

    [[nodiscard]] EntityId handle_of(Entity entity) const {
        std::lock_guard lock(mutex_);
        const auto it = by_entity_.find(entity);
        return it == by_entity_.end() ? kInvalidEntity : it->second;
    }

    void unbind(EntityId handle) {
        std::lock_guard lock(mutex_);
        const auto it = by_handle_.find(handle);
        if (it == by_handle_.end()) {
            return;
        }
        by_entity_.erase(it->second);
        by_handle_.erase(it);
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return by_handle_.size();
    }

   private:
    mutable std::mutex mutex_;
    std::unordered_map<EntityId, Entity> by_handle_;
    std::unordered_map<Entity, EntityId> by_entity_;
};

/// actor / profile / scene 的实体生命周期与关系维护。
class ActorWorld {
   public:
    static ActorWorld& instance() {
        static ActorWorld world;
        return world;
    }

    /// 登记 registry 的 owner 线程（DAG 线程首次使用时调用，幂等）。
    void bind_owner_thread() {
        std::lock_guard lock(thread_mutex_);
        owner_ = std::this_thread::get_id();
        owner_is_set_ = true;
    }

    /// 当前线程是否为 owner；未登记时返回 true（单线程测试场景）。
    [[nodiscard]] bool is_owner_thread() const {
        std::lock_guard lock(thread_mutex_);
        return owner_is_set_ ? owner_ == std::this_thread::get_id() : true;
    }

    // ---- 创建（幂等：同一句柄重复调用返回同一实体）----

    /// 确保 actor 实体存在，并补齐身份/变换/可见性/包围盒/驻留组件。
    Entity ensure_actor(EntityId handle) {
        if (handle == kInvalidEntity) {
            return kNullEntity;
        }
        if (const auto existing = index_.find(handle); existing != kNullEntity) {
            return existing;
        }
        auto& world = World::instance();
        const Entity entity = world.create<ActorTag, Transform, Visibility, Bounds, ResidencyState>();
        world.get<ActorTag>(entity).actor_id = static_cast<std::uint64_t>(handle);
        index_.bind(handle, entity);
        return entity;
    }

    /// 确保 scene 实体存在。
    Entity ensure_scene(EntityId handle) {
        if (handle == kInvalidEntity) {
            return kNullEntity;
        }
        if (const auto existing = index_.find(handle); existing != kNullEntity) {
            return existing;
        }
        auto& world = World::instance();
        const Entity entity = world.create<SceneTag, Children>();
        world.get<SceneTag>(entity).scene_id = static_cast<std::uint64_t>(handle);
        index_.bind(handle, entity);
        return entity;
    }

    /// 为 actor 创建 profile 子实体（同一父实体下允许多个）。
    Entity create_profile(Entity actor, EntityId profile_handle) {
        if (actor == kNullEntity || profile_handle == kInvalidEntity) {
            return kNullEntity;
        }
        auto& world = World::instance();
        const Entity profile = world.create<ProfileOf, Parent>();
        world.get<ProfileOf>(profile).actor = actor;
        world.get<Parent>(profile).value = actor;
        auto* children = world.try_get<Children>(actor);
        if (children == nullptr) {
            children = &world.emplace<Children>(actor);
        }
        children->value.push_back(profile);
        index_.bind(profile_handle, profile);
        return profile;
    }

    // ---- 关系（场景归属）----

    /// 把 actor 加入场景。order 用于稳定遍历顺序（渲染/序列化）。
    void attach_to_scene(Entity actor, Entity scene, std::uint32_t order) {
        if (actor == kNullEntity || scene == kNullEntity) {
            return;
        }
        auto& world = World::instance();
        if (auto* member = world.try_get<SceneMember>(actor); member != nullptr) {
            if (member->scene == scene) {
                member->order = order;
                return;
            }
            detach_from_scene(actor);
        }
        world.emplace<SceneMember>(actor, SceneMember{scene, order});

        auto* children = world.try_get<Children>(scene);
        if (children == nullptr) {
            children = &world.emplace<Children>(scene);
        }
        if (std::find(children->value.begin(), children->value.end(), actor) ==
            children->value.end()) {
            children->value.push_back(actor);
        }
    }

    /// 从当前场景移除（若存在）。
    void detach_from_scene(Entity actor) {
        if (actor == kNullEntity) {
            return;
        }
        auto& world = World::instance();
        const auto* member = world.try_get<SceneMember>(actor);
        if (member == nullptr) {
            return;
        }
        const Entity scene = member->scene;
        world.remove<SceneMember>(actor);
        if (auto* children = world.try_get<Children>(scene); children != nullptr) {
            std::erase(children->value, actor);
        }
    }

    // ---- 销毁 ----

    /// 销毁 actor 及其全部 profile 子实体，并解除句柄绑定。
    void destroy_actor(Entity actor) {
        if (actor == kNullEntity || !World::instance().alive(actor)) {
            return;
        }
        detach_from_scene(actor);

        auto& world = World::instance();
        if (auto* children = world.try_get<Children>(actor); children != nullptr) {
            const std::vector<Entity> owned = children->value;  // 复制：销毁过程中会改原容器
            for (const Entity child : owned) {
                index_.unbind(index_.handle_of(child));
                world.destroy(child);
            }
        }
        index_.unbind(index_.handle_of(actor));
        world.destroy(actor);
    }

    // ---- 查询 ----

    [[nodiscard]] Entity find(EntityId handle) const { return index_.find(handle); }

    [[nodiscard]] EntityId handle_of(Entity entity) const { return index_.handle_of(entity); }

    /// 当前场景内的 actor（按 order 升序），供系统按"场景视图"遍历。
    [[nodiscard]] std::vector<Entity> actors_in_scene(Entity scene) const {
        std::vector<Entity> result;
        const auto& world = World::instance();
        if (const auto* children = world.try_get<Children>(scene); children != nullptr) {
            result.reserve(children->value.size());
            for (const Entity child : children->value) {
                if (world.try_get<SceneMember>(child) != nullptr) {
                    result.push_back(child);
                }
            }
            std::stable_sort(result.begin(), result.end(), [&world](Entity lhs, Entity rhs) {
                return world.get<SceneMember>(lhs).order < world.get<SceneMember>(rhs).order;
            });
        }
        return result;
    }

    [[nodiscard]] std::size_t bound_entities() const { return index_.size(); }

   private:
    ActorWorld() = default;

    EntityIndex index_;
    mutable std::mutex thread_mutex_;
    std::thread::id owner_{};
    bool owner_is_set_{false};
};

}  // namespace Corona::Ecs
