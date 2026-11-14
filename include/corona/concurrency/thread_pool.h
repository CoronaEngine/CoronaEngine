#pragma once

/**
 * @file thread_pool.h
 * @brief 引擎唯一的"辅助任务"线程池（header-only）
 *
 * 背景：引擎常驻线程收敛为 main / SystemsThread(DAG) / Script / Network 四条，
 * 其余原本各自建线程的工作（资源导入、几何/LOD 构建、LOD 落盘、拾取回读、
 * 网格运动完成、GPU 预热）统一提交到这里执行。
 *
 * 调度策略（近似 CPU 分时共享，但受限于"一个任务就是一次不可抢占的函数调用"）：
 * - **三类任务**：Realtime / Compute / Background，按类排队。
 * - **专用道**：前 `realtime_workers` 个 worker 只跑 Realtime，空闲时帮忙跑
 *   Compute，**永远不取 Background**。Background 是秒级的资源导入与磁盘写入，
 *   这样它们不会挡住拾取回读这类亚帧级任务。
 * - **加权公平**：其余 worker 在 Compute / Background 之间比较
 *   `已派发数 / 权重`（交叉相乘，无浮点），近似 WFQ。
 * - **老化**：某类最老任务等待超过 `aging_dispatches` 次派发时强制优先，
 *   避免 Background 长期拿不到 worker。
 *
 * 明确不做（v1）：抢占、取消、work-stealing、插队。
 * 因为不可抢占，单个超长任务仍会占住它所在 worker —— 这正是保留专用道的原因。
 *
 * 线程安全：所有公开接口可从任意线程调用。
 * 约束：**不要在池任务内部 `wait()` 它自己所属的 TaskGroup**（会自锁）。
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace Corona::Concurrency {

/// 任务类别；数值用于计数数组下标，顺序不可随意调整。
enum class TaskClass : std::uint8_t {
    Realtime = 0,    ///< 亚帧级、延迟敏感：GPU 回读、碰撞完成、事件收尾
    Compute = 1,     ///< 数十毫秒级、可排队：几何构建、LOD 构建、shader 预热
    Background = 2,  ///< 百毫秒~秒级、可长期推迟：资源导入、LOD 落盘
};

inline constexpr std::size_t kTaskClassCount = 3;

[[nodiscard]] inline std::string_view task_class_name(TaskClass cls) noexcept {
    switch (cls) {
        case TaskClass::Realtime: return "realtime";
        case TaskClass::Compute: return "compute";
        case TaskClass::Background: return "background";
    }
    return "unknown";
}

/// 池配置：默认 2 条 worker，其中 1 条 Realtime 专用道（运行前可改）。
struct PoolConfig {
    std::uint32_t workers{2};            ///< worker 总数（>=1）
    std::uint32_t realtime_workers{1};   ///< 专用道数量；>= workers 时退化为无专用道
    std::uint32_t compute_weight{3};     ///< Compute 相对权重
    std::uint32_t background_weight{1};  ///< Background 相对权重
    std::uint32_t aging_dispatches{32};  ///< 老化阈值（派发次数）
};

/// 运行期快照，供日志与卡顿定位。
struct PoolStats {
    std::uint32_t workers{0};
    std::uint32_t realtime_workers{0};
    std::uint64_t submitted[kTaskClassCount]{};
    std::uint64_t executed[kTaskClassCount]{};
    std::uint64_t dropped[kTaskClassCount]{};
    std::size_t pending[kTaskClassCount]{};
    std::uint64_t max_wait_micros[kTaskClassCount]{};
};

namespace detail {

/// 类型擦除的可移动任务包装。
///
/// 不能用 std::function：几何/碰撞任务的 lambda 捕获了 std::move 进来的
/// GPU 句柄容器，属于不可拷贝类型；std::move_only_function 是 C++23，
/// 这里用最小实现替代。
class TaskFn {
public:
    TaskFn() = default;

    template <typename F,
              typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, TaskFn>>>
    explicit TaskFn(F&& fn)
        : impl_(std::make_unique<Model<std::decay_t<F>>>(std::forward<F>(fn))) {}

    TaskFn(TaskFn&& other) noexcept = default;
    TaskFn& operator=(TaskFn&& other) noexcept = default;
    TaskFn(const TaskFn&) = delete;
    TaskFn& operator=(const TaskFn&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept { return impl_ != nullptr; }

    void operator()() {
        if (impl_) impl_->call();
    }

private:
    struct Concept {
        virtual ~Concept() = default;
        virtual void call() = 0;
    };

    template <typename F>
    struct Model final : Concept {
        explicit Model(F&& fn) : fn_(std::move(fn)) {}
        void call() override { fn_(); }
        F fn_;
    };

    std::unique_ptr<Concept> impl_;
};

/// 读取环境变量（MSVC 下走 _dupenv_s，避免 getenv 的弃用告警）。
inline std::string env_string(const char* name) {
#if defined(_MSC_VER)
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, name) == 0 && buffer != nullptr) {
        std::string value{buffer};
        std::free(buffer);
        return value;
    }
    return {};
#else
    const char* raw = std::getenv(name);
    return raw != nullptr ? std::string{raw} : std::string{};
#endif
}

inline std::uint32_t env_unsigned(const char* name, std::uint32_t fallback,
                                  std::uint32_t minimum, std::uint32_t maximum) {
    const std::string raw = env_string(name);
    if (raw.empty()) return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(raw.c_str(), &end, 10);
    if (end == raw.c_str() || parsed < minimum) return fallback;
    if (parsed > maximum) return maximum;
    return static_cast<std::uint32_t>(parsed);
}

}  // namespace detail

/// 一批任务的完成点，语义对齐 tbb::task_group：
/// - submit() 不阻塞，任务结束时计数减一；
/// - wait() 阻塞到本组已提交任务全部结束；
/// - 析构等价于 wait()，适合作为 Impl 成员，保证先于被引用的数据析构。
class TaskGroup {
public:
    TaskGroup() = default;
    ~TaskGroup() { wait(); }

    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;
    TaskGroup(TaskGroup&&) = delete;
    TaskGroup& operator=(TaskGroup&&) = delete;

    /// 提交任务；池已关闭时不会执行，立即归还计数并返回 false。
    template <typename F>
    bool submit(TaskClass cls, F&& fn);

    /// 阻塞到本组任务全部结束。禁止在池任务内部等待自身所在组。
    void wait();

    [[nodiscard]] std::size_t pending() const;

private:
    void acquire();
    void release();

    mutable std::mutex mutex_;
    std::condition_variable drained_;
    std::size_t pending_{0};
};

/// 全局线程池单例。首次 submit 惰性启动；Engine::shutdown() 显式 stop()。
class ThreadPool {
public:
    static ThreadPool& instance();

    /// 从环境变量读取配置：
    ///   CORONA_UTILITY_THREADS            worker 总数，默认 2，范围 1..32
    ///   CORONA_UTILITY_REALTIME_THREADS   专用道数量，默认 1，范围 0..32
    ///   CORONA_UTILITY_AGING             老化阈值，默认 32，范围 1..100000
    [[nodiscard]] static PoolConfig config_from_environment();

    /// 启动前替换配置；已启动时忽略并返回 false。
    bool configure(PoolConfig config);

    /// 显式启动（submit 亦会惰性启动）。可重复调用。
    void start();

    /// 停止：置停止标志 → 排空已入队任务 → join。可重复调用。
    /// 停止后新的 submit 被丢弃并计入 dropped。
    void stop();

    [[nodiscard]] bool started() const noexcept { return started_.load(std::memory_order_acquire); }

    /// 提交任务并返回 future；异常被捕获并转发到 future。
    template <typename F>
    auto submit(TaskClass cls, F&& fn) -> std::future<std::invoke_result_t<F>>;

    /// 提交不关心返回值的任务；返回是否成功入队。
    template <typename F>
    bool submit_void(TaskClass cls, F&& fn);

    [[nodiscard]] PoolStats stats() const;

    ThreadPool();
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

private:
    struct Task {
        detail::TaskFn fn;
        TaskClass cls{TaskClass::Compute};
        std::uint64_t seq{0};
        std::chrono::steady_clock::time_point enqueued{};
    };

    void ensure_started();
    bool enqueue(TaskClass cls, detail::TaskFn fn);
    void worker_main(std::uint32_t index);
    [[nodiscard]] int select_class_locked(bool realtime_lane);
    [[nodiscard]] bool queues_empty_locked() const;
    void stop_workers();

    PoolConfig config_{};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> queues_[kTaskClassCount];
    std::vector<std::thread> workers_;
    std::uint64_t next_seq_{0};
    std::uint64_t dispatched_[kTaskClassCount]{};
    std::atomic<bool> started_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> terminated_{false};
    std::uint64_t submitted_[kTaskClassCount]{};
    std::uint64_t executed_[kTaskClassCount]{};
    std::uint64_t dropped_[kTaskClassCount]{};
    std::uint64_t max_wait_micros_[kTaskClassCount]{};
};

// ---------------------------------------------------------------------------
// TaskGroup
// ---------------------------------------------------------------------------

inline void TaskGroup::acquire() {
    std::lock_guard lock(mutex_);
    ++pending_;
}

inline void TaskGroup::release() {
    std::lock_guard lock(mutex_);
    if (pending_ > 0) --pending_;
    if (pending_ == 0) drained_.notify_all();
}

inline void TaskGroup::wait() {
    std::unique_lock lock(mutex_);
    drained_.wait(lock, [this] { return pending_ == 0; });
}

inline std::size_t TaskGroup::pending() const {
    std::lock_guard lock(mutex_);
    return pending_;
}

template <typename F>
bool TaskGroup::submit(TaskClass cls, F&& fn) {
    acquire();
    auto* group = this;
    const bool enqueued = ThreadPool::instance().submit_void(
        cls, [group, callable = std::forward<F>(fn)]() mutable {
            struct ReleaseOnExit {
                TaskGroup* group;
                ~ReleaseOnExit() { group->release(); }
            } release_on_exit{group};
            callable();
        });
    if (!enqueued) {
        // 池已停止：调用方可能正卡在 wait()，必须立即归还计数。
        release();
    }
    return enqueued;
}

// ---------------------------------------------------------------------------
// ThreadPool
// ---------------------------------------------------------------------------

inline PoolConfig ThreadPool::config_from_environment() {
    PoolConfig config{};
    config.workers = detail::env_unsigned("CORONA_UTILITY_THREADS", config.workers, 1, 32);
    config.realtime_workers = detail::env_unsigned(
        "CORONA_UTILITY_REALTIME_THREADS", config.realtime_workers, 0, 32);
    config.aging_dispatches = detail::env_unsigned(
        "CORONA_UTILITY_AGING", config.aging_dispatches, 1, 100000);
    if (config.compute_weight == 0) config.compute_weight = 1;
    if (config.background_weight == 0) config.background_weight = 1;
    return config;
}

inline ThreadPool& ThreadPool::instance() {
    static ThreadPool pool;
    return pool;
}

inline ThreadPool::ThreadPool() : config_(config_from_environment()) {}

inline ThreadPool::~ThreadPool() {
    stop();
}

inline bool ThreadPool::configure(PoolConfig config) {
    std::lock_guard lock(mutex_);
    if (started_.load(std::memory_order_acquire)) return false;
    if (config.workers == 0) config.workers = 1;
    if (config.compute_weight == 0) config.compute_weight = 1;
    if (config.background_weight == 0) config.background_weight = 1;
    if (config.aging_dispatches == 0) config.aging_dispatches = 1;
    config_ = config;
    return true;
}

inline void ThreadPool::start() {
    ensure_started();
}

inline void ThreadPool::ensure_started() {
    std::lock_guard lock(mutex_);
    if (started_.load(std::memory_order_acquire) || terminated_.load(std::memory_order_acquire)) {
        return;
    }
    // workers == 1 时没有"其余 worker"，专用道会独占全部线程；退化为无专用道，
    // 单 worker 按 Realtime > Compute/Background 顺序服务。
    if (config_.realtime_workers >= config_.workers) {
        config_.realtime_workers = config_.workers > 1 ? config_.workers - 1 : 0;
    }
    workers_.reserve(config_.workers);
    started_.store(true, std::memory_order_release);
    // 在锁内起线程：worker 会立刻抢同一把锁，阻塞到本函数返回，不会有竞态。
    for (std::uint32_t index = 0; index < config_.workers; ++index) {
        workers_.emplace_back([this, index] { worker_main(index); });
    }
}

inline void ThreadPool::stop() {
    stop_workers();
}

inline void ThreadPool::stop_workers() {
    {
        std::lock_guard lock(mutex_);
        if (terminated_.load(std::memory_order_acquire)) return;
        stopping_.store(true, std::memory_order_release);
        terminated_.store(true, std::memory_order_release);
    }
    cv_.notify_all();

    std::vector<std::thread> workers;
    {
        std::lock_guard lock(mutex_);
        workers.swap(workers_);
    }
    for (auto& worker : workers) {
        // 池线程自己调用 stop() 不能 join 自己（如任务里触发关停）。
        if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
            worker.join();
        } else if (worker.joinable()) {
            worker.detach();
        }
    }
}

inline bool ThreadPool::enqueue(TaskClass cls, detail::TaskFn fn) {
    ensure_started();

    const auto index = static_cast<std::size_t>(cls);
    {
        std::lock_guard lock(mutex_);
        if (terminated_.load(std::memory_order_acquire) ||
            stopping_.load(std::memory_order_acquire)) {
            ++dropped_[index];
            return false;
        }
        Task task;
        task.fn = std::move(fn);
        task.cls = cls;
        task.seq = ++next_seq_;
        task.enqueued = std::chrono::steady_clock::now();
        queues_[index].push_back(std::move(task));
        ++submitted_[index];
    }
    cv_.notify_all();
    return true;
}

inline bool ThreadPool::queues_empty_locked() const {
    for (const auto& queue : queues_) {
        if (!queue.empty()) return false;
    }
    return true;
}

inline int ThreadPool::select_class_locked(bool realtime_lane) {
    const auto realtime_index = static_cast<std::size_t>(TaskClass::Realtime);
    const auto compute_index = static_cast<std::size_t>(TaskClass::Compute);
    const auto background_index = static_cast<std::size_t>(TaskClass::Background);

    // Realtime 永远第一优先：谁先醒谁接手，避免排在长任务之后。
    if (!queues_[realtime_index].empty()) return static_cast<int>(TaskClass::Realtime);

    // 专用道只帮忙跑 Compute（数十毫秒级），绝不取 Background（秒级会毁掉延迟保证）。
    if (realtime_lane) {
        return queues_[compute_index].empty() ? -1 : static_cast<int>(TaskClass::Compute);
    }

    const bool has_compute = !queues_[compute_index].empty();
    const bool has_background = !queues_[background_index].empty();
    if (!has_compute && !has_background) return -1;
    if (has_compute && !has_background) return static_cast<int>(TaskClass::Compute);
    if (has_background && !has_compute) return static_cast<int>(TaskClass::Background);

    // 老化：等待太久的一类强制优先，防止 Background 被 WFQ 长期压住。
    if (queues_[background_index].front().seq + config_.aging_dispatches <= next_seq_) {
        return static_cast<int>(TaskClass::Background);
    }
    if (queues_[compute_index].front().seq + config_.aging_dispatches <= next_seq_) {
        return static_cast<int>(TaskClass::Compute);
    }

    // 加权公平（近似 WFQ）：比较 dispatched/weight，交叉相乘避免浮点。
    const auto compute_score = dispatched_[compute_index] * config_.background_weight;
    const auto background_score = dispatched_[background_index] * config_.compute_weight;
    return compute_score <= background_score ? static_cast<int>(TaskClass::Compute)
                                             : static_cast<int>(TaskClass::Background);
}

inline void ThreadPool::worker_main(std::uint32_t index) {
    const bool realtime_lane = index < config_.realtime_workers;

    for (;;) {
        Task task;
        {
            std::unique_lock lock(mutex_);
            for (;;) {
                if (stopping_.load(std::memory_order_acquire) && queues_empty_locked()) {
                    return;
                }
                const int selected = select_class_locked(realtime_lane);
                if (selected >= 0) {
                    auto& queue = queues_[static_cast<std::size_t>(selected)];
                    task = std::move(queue.front());
                    queue.pop_front();
                    ++dispatched_[static_cast<std::size_t>(selected)];
                    break;
                }
                cv_.wait(lock);
            }
        }

        const auto waited = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - task.enqueued)
                                .count();
        const auto wait_micros =
            static_cast<std::uint64_t>(waited < 0 ? 0 : waited);
        const auto class_index = static_cast<std::size_t>(task.cls);

        try {
            task.fn();
        } catch (...) {
            // submit() 已把异常转给 promise；这里兜底只为不让一条任务杀死 worker。
        }

        {
            std::lock_guard lock(mutex_);
            ++executed_[class_index];
            if (wait_micros > max_wait_micros_[class_index]) {
                max_wait_micros_[class_index] = wait_micros;
            }
        }
    }
}

inline PoolStats ThreadPool::stats() const {
    PoolStats snapshot;
    std::lock_guard lock(mutex_);
    snapshot.workers = static_cast<std::uint32_t>(workers_.size());
    snapshot.realtime_workers = config_.realtime_workers;
    for (std::size_t i = 0; i < kTaskClassCount; ++i) {
        snapshot.submitted[i] = submitted_[i];
        snapshot.executed[i] = executed_[i];
        snapshot.dropped[i] = dropped_[i];
        snapshot.pending[i] = queues_[i].size();
        snapshot.max_wait_micros[i] = max_wait_micros_[i];
    }
    return snapshot;
}

template <typename F>
auto ThreadPool::submit(TaskClass cls, F&& fn) -> std::future<std::invoke_result_t<F>> {
    using Result = std::invoke_result_t<F>;
    auto promise = std::make_shared<std::promise<Result>>();
    auto future = promise->get_future();
    enqueue(cls, detail::TaskFn([promise, callable = std::forward<F>(fn)]() mutable {
                try {
                    if constexpr (std::is_void_v<Result>) {
                        callable();
                        promise->set_value();
                    } else {
                        promise->set_value(callable());
                    }
                } catch (...) {
                    try {
                        promise->set_exception(std::current_exception());
                    } catch (...) {
                    }
                }
            }));
    return future;
}

template <typename F>
bool ThreadPool::submit_void(TaskClass cls, F&& fn) {
    return enqueue(cls, detail::TaskFn(std::forward<F>(fn)));
}

}  // namespace Corona::Concurrency
