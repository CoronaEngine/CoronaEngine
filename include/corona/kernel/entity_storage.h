#pragma once

/// 基于 EnTT 的存储实现，替代 `horizon::core::Storage`。
///
/// 设计目标：**API 兼容 + 数据在 entt 里**。
/// 这样 Engine 里 250+ 处 `*_storage().acquire_*()` 调用点一行都不用改，
/// 而底层已经从"分块静态数组 + 每槽锁"变成 entt 的组件池。
///
/// ---- 与旧 Storage 的关键差异 ----
/// 1. 实体 ID：`entt::entity` 的 integral + 1。
///    +1 是刻意的：全仓大量 `if (handle == 0) return;` 用 0 表示无效，
///    而 entt 的第一个实体就是 0，必须避开。
/// 2. 锁：旧实现是"每槽一把"，这里改成**条带锁**（按实体索引哈希到
///    固定数量的槽）。竞态更多但内存恒定、不需要为每个实体分配 mutex。
///    这是**过渡期兼容层**：等 47 处跨线程读改成快照后（见
///    docs/s4-cross-thread-reads-plan.md），这些锁可以直接删掉。
/// 3. 遍历：旧实现遍历缓冲区，这里遍历 `entt::view<T>`；顺序不再保证，
///    调用方本来就只做过滤式遍历。

#include <corona/ecs/world.h>
#include <corona/kernel/entity_id.h>

#include <array>
#include <memory>
#include <cstddef>
#include <mutex>
#include <shared_mutex>

namespace Corona::Kernel::Utils {

/// 条带锁数量。必须是 2 的幂，容量与并发度之间取平衡。
inline constexpr std::size_t kEntityLockStripes = 4096;

inline std::array<std::shared_timed_mutex, kEntityLockStripes>& entity_lock_stripes() {
    static std::array<std::shared_timed_mutex, kEntityLockStripes> stripes;
    return stripes;
}

inline std::shared_timed_mutex& entity_lock_for(EntityId id) {
    // 实体索引低位分布不均，用乘法散列再取模。
    const auto index = static_cast<std::size_t>(id) & (kEntityLockStripes - 1);
    return entity_lock_stripes()[index];
}

/// 实体 ID <-> entt 实体。0 保留给"无效"。
inline EntityId to_entity_id(entt::entity entity) {
    return static_cast<EntityId>(entt::to_integral(entity)) + 1;
}

inline entt::entity to_entity(EntityId id) {
    if (id == kInvalidEntity) {
        return entt::null;
    }
    return static_cast<entt::entity>(static_cast<std::uint64_t>(id) - 1);
}

/// 真正属于 entt 的句柄无效值。
inline constexpr EntityId kInvalidEntityId = kInvalidEntity;

template <typename T, std::size_t BufferCapacity = 128, std::size_t InitialBuffers = 2>
class EntityStorage {
   public:
    using ObjectId = EntityId;
    using value_type = T;

    /// entt 组件用一个可拷贝的间接层： 本身可能是 move-only
    /// （例如 GeometryDevice 内含 GpuMemToken，拷贝被删除）。
    /// 用 shared_ptr 包装后组件可拷贝，且语义上是同一个设备实例。
    struct Holder {
        std::shared_ptr<T> value;
    };

    class ReadHandle {
       public:
        ReadHandle() = default;
        ReadHandle(std::shared_lock<std::shared_timed_mutex> lock, const T* value, EntityId id)
            : lock_(std::move(lock)), value_(value), id_(id) {}
        [[nodiscard]] EntityId id() const noexcept { return id_; }

        [[nodiscard]] const T* operator->() const noexcept { return value_; }
        [[nodiscard]] const T& operator*() const noexcept { return *value_; }
        [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }
        [[nodiscard]] bool valid() const noexcept { return value_ != nullptr; }
        [[nodiscard]] const T* get() const noexcept { return value_; }

       private:
        std::shared_lock<std::shared_timed_mutex> lock_;
        const T* value_{nullptr};
        EntityId id_{kInvalidEntity};
    };

    class WriteHandle {
       public:
        WriteHandle() = default;
        WriteHandle(std::unique_lock<std::shared_timed_mutex> lock, T* value, EntityId id)
            : lock_(std::move(lock)), value_(value), id_(id) {}
        [[nodiscard]] EntityId id() const noexcept { return id_; }

        [[nodiscard]] T* operator->() const noexcept { return value_; }
        [[nodiscard]] T& operator*() const noexcept { return *value_; }
        [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }
        [[nodiscard]] bool valid() const noexcept { return value_ != nullptr; }
        [[nodiscard]] T* get() const noexcept { return value_; }

       private:
        std::unique_lock<std::shared_timed_mutex> lock_;
        T* value_{nullptr};
        EntityId id_{kInvalidEntity};
    };

    /// 创建实体并挂上组件。
    [[nodiscard]] ObjectId allocate() {
        auto& registry = Corona::Ecs::World::instance().registry();
        const entt::entity entity = registry.create();
        auto& holder = registry.emplace<Holder>(entity);
        holder.value = std::make_shared<T>();
        return to_entity_id(entity);
    }

    void deallocate(ObjectId id) {
        auto& registry = Corona::Ecs::World::instance().registry();
        const entt::entity entity = to_entity(id);
        if (entity != entt::null && registry.valid(entity)) {
            registry.destroy(entity);
        }
    }

    [[nodiscard]] ReadHandle acquire_read(ObjectId id) {
        auto& lock = entity_lock_for(id);
        std::shared_lock<std::shared_timed_mutex> guard(lock);
        auto* value = try_value(id);
        return ReadHandle(std::move(guard), value, id);
    }

    [[nodiscard]] ReadHandle try_acquire_read(ObjectId id) { return acquire_read(id); }

    /// 非阻塞读：拿不到锁就返回空句柄。调用方必须按"可能失败"处理。
    [[nodiscard]] ReadHandle try_acquire_read_nowait(ObjectId id) {
        auto& lock = entity_lock_for(id);
        std::shared_lock<std::shared_timed_mutex> guard(lock, std::try_to_lock);
        if (!guard.owns_lock()) {
            return ReadHandle{};
        }
        return ReadHandle(std::move(guard), try_value(id), id);
    }

    [[nodiscard]] WriteHandle acquire_write(ObjectId id) {
        auto& lock = entity_lock_for(id);
        std::unique_lock<std::shared_timed_mutex> guard(lock);
        auto* value = try_value(id);
        return WriteHandle(std::move(guard), value, id);
    }

    [[nodiscard]] WriteHandle try_acquire_write(ObjectId id) { return acquire_write(id); }

    [[nodiscard]] bool contains(ObjectId id) const {
        const entt::entity entity = to_entity(id);
        if (entity == entt::null) {
            return false;
        }
        const auto& registry = Corona::Ecs::World::instance().registry();
        if (!registry.valid(entity)) {
            return false;
        }
        const auto* holder = registry.try_get<Holder>(entity);
        return holder != nullptr && holder->value != nullptr;
    }

    /// 实体 -> 稠密整数索引。旧 Storage 用 缓冲序号*容量+槽位 计算，
    /// 这里 entt 的实体本身已经是 index+version 的稠密值，直接取 integral。
    /// 网络同步用它做哈希 key，只要同一实体稳定即可。
    [[nodiscard]] std::int64_t seq_id(ObjectId id) const {
        return static_cast<std::int64_t>(static_cast<std::uint64_t>(id) - 1);
    }
    [[nodiscard]] std::int64_t seq_id(const ReadHandle& handle) const { return seq_id(handle.id()); }
    [[nodiscard]] std::int64_t seq_id(const WriteHandle& handle) const { return seq_id(handle.id()); }

    [[nodiscard]] std::size_t count() const {
        const auto& registry = Corona::Ecs::World::instance().registry();
        return registry.view<Holder>().size();
    }

    [[nodiscard]] std::size_t size() const { return count(); }
    [[nodiscard]] bool empty() const { return count() == 0; }

    /// 遍历（过滤式）。顺序不保证——旧实现遍历缓冲区，这里遍历 entt 组件池，
    /// 但调用方全部是筛选式遍历，不依赖顺序。
    using View = decltype(std::declval<entt::registry&>().view<Holder>());
    using ViewIterator = decltype(std::declval<View>().begin());

    class Iterator {
       public:
        Iterator() = default;
        Iterator(entt::registry* registry, ViewIterator it)
            : registry_(registry), it_(std::move(it)) {}

        [[nodiscard]] T& operator*() const { return *registry_->get<Holder>(*it_).value; }
        [[nodiscard]] T* operator->() const { return registry_->get<Holder>(*it_).value.get(); }

        Iterator& operator++() {
            ++it_;
            return *this;
        }

        [[nodiscard]] bool operator!=(const Iterator& other) const { return it_ != other.it_; }
        [[nodiscard]] bool operator==(const Iterator& other) const { return it_ == other.it_; }

       private:
        entt::registry* registry_{nullptr};
        ViewIterator it_{};
    };

    [[nodiscard]] Iterator begin() {
        auto& registry = Corona::Ecs::World::instance().registry();
        return Iterator(&registry, registry.view<Holder>().begin());
    }

    [[nodiscard]] Iterator end() {
        auto& registry = Corona::Ecs::World::instance().registry();
        return Iterator(&registry, registry.view<Holder>().end());
    }

    [[nodiscard]] Iterator begin() const { return const_cast<EntityStorage*>(this)->begin(); }
    [[nodiscard]] Iterator end() const { return const_cast<EntityStorage*>(this)->end(); }
    [[nodiscard]] Iterator cbegin() const { return begin(); }
    [[nodiscard]] Iterator cend() const { return end(); }

    /// 需要 entt 原生 view（多组件过滤、each）时直接用这个。
    [[nodiscard]] View view() { return Corona::Ecs::World::instance().registry().view<Holder>(); }

   private:
    [[nodiscard]] T* try_value(ObjectId id) {
        const entt::entity entity = to_entity(id);
        if (entity == entt::null) {
            return nullptr;
        }
        auto& registry = Corona::Ecs::World::instance().registry();
        if (!registry.valid(entity)) {
            return nullptr;
        }
        auto* holder = registry.try_get<Holder>(entity);
        return holder != nullptr ? holder->value.get() : nullptr;
    }

    [[nodiscard]] const T* try_value(ObjectId id) const {
        return const_cast<EntityStorage*>(this)->try_value(id);
    }
};

}  // namespace Corona::Kernel::Utils
