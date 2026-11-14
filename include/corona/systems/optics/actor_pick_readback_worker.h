#pragma once

#include <corona/concurrency/thread_pool.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_set>

namespace Corona::Systems::OpticsDetail {

/**
 * @brief GPU 拾取回读工作队列（无线程版）
 *
 * 历史上这里带一条专用线程 + condition_variable。现在任务提交到引擎统一线程池的
 * Realtime 道：GPU 回读是亚帧级、对延迟敏感，且必须避开资源导入之类的秒级长任务。
 *
 * 本类只保留两件与线程无关的职责：
 * - 容量背压：同时最多 capacity 个"已预留/在飞"的请求；
 * - Reservation 的 RAII 取消语义：请求被丢弃时立刻释放槽位。
 *
 * 对外接口与线程版完全一致，调用方（OpticsSystem）无需改动。
 */
class ActorPickReadbackWorker {
public:
    class Reservation {
    public:
        Reservation() = default;
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        Reservation(Reservation&& other) noexcept;
        Reservation& operator=(Reservation&& other) noexcept;
        ~Reservation();

    private:
        friend class ActorPickReadbackWorker;
        Reservation(ActorPickReadbackWorker* owner, std::uint64_t id) noexcept;
        void reset() noexcept;

        ActorPickReadbackWorker* owner_{nullptr};
        std::uint64_t id_{0};
    };

    using Work = std::function<void()>;

    explicit ActorPickReadbackWorker(std::size_t capacity = 64);
    ActorPickReadbackWorker(const ActorPickReadbackWorker&) = delete;
    ActorPickReadbackWorker& operator=(const ActorPickReadbackWorker&) = delete;
    ~ActorPickReadbackWorker();

    /// 允许接收新请求（幂等）。
    void start();

    /// 预留一个槽位；满了或已关闭时返回 nullopt。
    [[nodiscard]] std::optional<Reservation> reserve();

    /// 把工作提交到线程池；reservation 被消费。提交失败返回 false。
    [[nodiscard]] bool submit(Reservation&& reservation, Work work);

    /// 关闭：拒绝新请求，等待已在飞的工作结束。
    void shutdown();

    [[nodiscard]] std::size_t pending_count() const noexcept;

private:
    void cancel(std::uint64_t id) noexcept;

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::unordered_set<std::uint64_t> reservations_;
    std::uint64_t next_reservation_id_{1};
    std::size_t occupied_{0};
    bool accepting_{false};

    /// 在飞任务；shutdown() 等待其归零。声明在最后 → 先于其它成员析构。
    Corona::Concurrency::TaskGroup in_flight_;
};

}  // namespace Corona::Systems::OpticsDetail
