#pragma once

/// ECS **结构命令**通道：registry 的跨线程创建边界（P1 / spec §5.1）。
///
/// `entt::registry` 没有任何同步（见 `ecs/world.h` 的线程约定）：它必须只被一个
/// 线程触碰。但 `create_geometry()` 的调用方是编辑器 UI / 脚本线程，而 DAG 线程
/// 同时在遍历同一 registry —— 这是"两个写者 + 一个读者"的未同步状态。
///
/// 因此结构创建不再由调用方直接写 registry：
///
///   create_* 不创建实体：组装"初始组件值"入队 → 同步等待 DAG 在 tick 边界
///   创建 → 拿回 `EntityId`。句柄到手时实体已经存在，没有"先无效后生效"的中间态。
///
/// 为什么是 **Fifo**：创建顺序有语义（actor 先于它的 profile、场景先于成员），
/// 不能合并；与 `scene_commands.h` / `mechanics_structural_commands.h` 同一选择。
/// 为什么应用在 **tick 边界**：所有节点遍历之前落地，本 tick 内实体集合不变化。
///
/// ---- 四条不变量（缺一条就是回归）----
/// 1. **结算**：每个入队请求都拿到结果（句柄或失败）。DAG 未运行 / 正在停止 /
///    等待超时，调用方都**有界地**拿到失败，绝不无限等待。本仓库 Python 桥的
///    `submit_and_wait` 没有截止时间，一次漏结算就永久挂住一个线程 —— 这里不允许重演。
/// 2. **重入**：调用方本身就是 DAG 线程（系统内部创建）时**直接应用**，
///    入队后等自己会自死锁。
/// 3. **超时**：默认**两帧**，帧周期由装配方用 `set_tick_period()` 告知（DAG 模式取
///    配置的 tick，降级模式取 Geometry 的更新周期），下限 17ms；
///    `CORONA_ECS_STRUCTURAL_TIMEOUT_MS` 可覆盖。超时按失败返回并写诊断，不静默吞。
/// 4. **停机**：DAG 停止时把队列里未消费的请求全部以失败结算
///    （与 Python 桥 `transition_and_cancel` 同一原则）。

#include <corona/ecs/access.h>
#include <corona/ecs/components.h>
#include <corona/kernel/command/command_buffer.h>
#include <corona/kernel/command/thread_affinity.h>
#include <corona/shared_data_hub.h>

#include <horizon/core/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Corona::Ecs {

/// 自动超时的下限：两帧 @120Hz ≈ 17ms（旧默认值，保留为 floor）。
inline constexpr std::int64_t kMinStructuralTimeoutMs = 17;

/// 显式/自动超时的上限：再长也不该把一个 UI / 脚本线程挂住。
inline constexpr std::int64_t kMaxStructuralTimeoutMs = 5000;

/// 装配方告知真实帧周期之前使用的保守假设（120Hz）。
inline constexpr std::int64_t kDefaultFramePeriodUs = 8333;

/// 环境覆盖（`CORONA_ECS_STRUCTURAL_TIMEOUT_MS`，毫秒）；0 表示未覆盖。
/// 覆盖值优先于"两帧"的自动推导，语义与旧版一致。
[[nodiscard]] inline std::int64_t structural_timeout_override_ms() noexcept {
    if (const char* env = std::getenv("CORONA_ECS_STRUCTURAL_TIMEOUT_MS")) {
        const long parsed = std::strtol(env, nullptr, 10);
        if (parsed > 0 && parsed <= kMaxStructuralTimeoutMs) {
            return parsed;
        }
    }
    return 0;
}

/// 自动超时 = **两帧**（帧周期向上取整到毫秒），下限 `kMinStructuralTimeoutMs`。
///
/// 帧周期由装配方用 `set_tick_period()` 告知：DAG 模式取配置的 tick
/// （`CORONA_SYSTEMS_DAG_TICK`，30..480Hz），降级模式取 Geometry 的更新周期。
/// 写死 17ms 会在 30Hz（33.3ms/帧）下让近一半创建请求假超时。
[[nodiscard]] inline std::int64_t two_frame_timeout_ms(std::int64_t frame_period_us) noexcept {
    const std::int64_t two_frames_ms = (frame_period_us * 2 + 999) / 1000;
    return std::clamp(two_frames_ms, kMinStructuralTimeoutMs, kMaxStructuralTimeoutMs);
}

/// 一条结构请求的**结算槽**：等待方（任意线程）与拥有者（DAG 线程）共享。
///
/// 关键点：拥有者在**持有槽锁**的状态下创建实体。等待方超时也需要先拿到槽锁，
/// 因此不会出现"等待方已放弃、拥有者却刚创建出一个无人签收的实体"的孤儿。
class StructuralRequest {
   public:
    struct Result {
        EntityId handle{0};  ///< 0 表示失败
        std::string error;   ///< 失败原因（成功时为空）
        [[nodiscard]] bool ok() const noexcept { return handle != 0; }
    };

    explicit StructuralRequest(std::uint64_t id) noexcept : id_(id) {}

    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }

    /// 等待方（任意线程）：最多等 `timeout`。超时把请求标记为已放弃，
    /// 拥有者之后取到它也会跳过 —— 不会创建无人签收的实体。
    [[nodiscard]] Result wait(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this] { return settled_; })) {
            settled_ = true;
            abandoned_ = true;
            result_.handle = 0;
            result_.error = "timeout waiting for the systems DAG to apply the request";
            return result_;
        }
        return result_;
    }

    /// 拥有者线程：持锁创建并结算。请求已被放弃 / 已结算时返回 false（跳过创建）。
    template <typename Create>
    bool apply(Create&& create) {
        std::unique_lock lock(mutex_);
        if (settled_) {
            // 已被等待方放弃（超时）或被停机结算：跳过，绝不创建无人签收的实体。
            return false;
        }
        try {
            result_.handle = create();
            if (result_.handle == 0) {
                result_.error = "entity creation returned an invalid handle";
            }
        } catch (const std::exception& error) {
            result_.handle = 0;
            result_.error = std::string{"exception while creating the entity: "} + error.what();
        } catch (...) {
            result_.handle = 0;
            result_.error = "unknown exception while creating the entity";
        }
        settled_ = true;
        lock.unlock();
        cv_.notify_all();
        return true;
    }

    /// 停机结算（任意线程）。返回本次是否真的结算了一个在途请求。
    bool settle_failure(std::string reason) {
        {
            std::lock_guard lock(mutex_);
            if (settled_) {
                return false;
            }
            settled_ = true;
            result_.handle = 0;
            result_.error = std::move(reason);
        }
        cv_.notify_all();
        return true;
    }

    [[nodiscard]] bool abandoned() const {
        std::lock_guard lock(mutex_);
        return abandoned_;
    }

   private:
    std::uint64_t id_{0};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool settled_{false};
    bool abandoned_{false};
    Result result_{};
};

/// 创建几何实体时携带的初始组件值。命令里装的是**值**，不是裸指针：
/// 调用方线程写、DAG 线程读，中间的交接由队列的互斥量负责。
struct CreateGeometryPayload {
    GeometryDevice geometry;
    ModelSource model_source;
};

/// 结构命令种类。1b 会在这里加入 scene / actor / profile / destroy。
enum class StructuralCommandKind : std::uint8_t {
    CreateGeometry,
};

struct StructuralCommand {
    StructuralCommandKind kind{StructuralCommandKind::CreateGeometry};
    std::shared_ptr<StructuralRequest> request;  ///< 结算槽（唯一，key 即序号）
    CreateGeometryPayload geometry;
};

struct StructuralCommandKey {
    static std::uintptr_t get(const StructuralCommand& command) noexcept {
        return command.request ? static_cast<std::uintptr_t>(command.request->id()) : 0;
    }
};

/// 拥有者线程：创建实体并把初始组件值一次性写进去（不产生"裸实体"）。
/// 前向声明：通道的 `apply_pending()` 在类内用到它，定义放在通道之后。
[[nodiscard]] inline EntityId create_geometry_entity(CreateGeometryPayload payload);

/// 通道本身：队列 + 拥有者线程标识 + 生命周期状态 + 诊断计数。
class StructuralCommandChannel {
   public:
    using Buffer = Kernel::CommandBuffer<StructuralCommand, StructuralCommandKey,
                                         Kernel::CoalescePolicy::Fifo>;

    /// 诊断计数（进程内累计；`reset_stats()` 供测试用）
    struct Stats {
        std::uint64_t requested{0};       ///< 入队的外部请求数
        std::uint64_t inline_applied{0};  ///< 重入规则下直接应用的请求数
        std::uint64_t applied{0};         ///< 由 DAG 应用并成功结算的请求数
        std::uint64_t failed{0};          ///< 等待方拿到失败的请求数（每条只记一次）
        std::uint64_t rejected{0};        ///< 未入队就拒绝的请求数（停机/队列满）
        std::uint64_t timeouts{0};        ///< 等待超时的请求数（failed 的子集）
        std::uint64_t abandoned{0};       ///< 等待方放弃后拥有者跳过的命令数
        std::uint64_t settled_on_stop{0}; ///< 停机时批量结算的请求数（不计入 failed）
    };

    static StructuralCommandChannel& instance() {
        static StructuralCommandChannel channel;
        return channel;
    }

    StructuralCommandChannel(const StructuralCommandChannel&) = delete;
    StructuralCommandChannel& operator=(const StructuralCommandChannel&) = delete;

    [[nodiscard]] Buffer& commands() noexcept { return commands_; }

    /// registry 的拥有者线程（DAG 线程）。重入判定与越界断言都用它。
    [[nodiscard]] Kernel::ThreadAffinity& affinity() noexcept { return affinity_; }

    [[nodiscard]] std::uint64_t next_request_id() noexcept {
        return request_ids_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    /// 当前等待上限：显式覆盖优先，否则 = 两帧（真实帧周期，下限 17ms）。
    [[nodiscard]] std::chrono::milliseconds timeout() const noexcept {
        const auto override_ms = timeout_override_ms_.load(std::memory_order_relaxed);
        if (override_ms > 0) {
            return std::chrono::milliseconds{override_ms};
        }
        return std::chrono::milliseconds{
            two_frame_timeout_ms(frame_period_us_.load(std::memory_order_relaxed))};
    }

    /// 装配方告知真实帧周期（DAG tick / 降级模式下 Geometry 的更新周期），
    /// 自动超时据此推导；显式 `set_timeout()` 与 env 覆盖不受影响。
    void set_tick_period(std::chrono::microseconds period) noexcept {
        const auto count = std::clamp<std::int64_t>(period.count(), 1, 1'000'000);
        frame_period_us_.store(count, std::memory_order_relaxed);
    }

    /// 显式覆盖（测试与需要固定预算的集成方）；env 覆盖在构造时已生效。
    void set_timeout(std::chrono::milliseconds value) noexcept {
        const auto count = std::clamp<std::int64_t>(value.count(), 1, kMaxStructuralTimeoutMs);
        timeout_override_ms_.store(count, std::memory_order_relaxed);
    }

    /// 清除显式覆盖，回到"两帧"自动推导（测试与运行时重配用）。
    void clear_timeout_override() noexcept {
        timeout_override_ms_.store(0, std::memory_order_relaxed);
    }

    /// 是否由 env / `set_timeout()` 显式指定（诊断用）。
    [[nodiscard]] bool timeout_is_override() const noexcept {
        return timeout_override_ms_.load(std::memory_order_relaxed) > 0;
    }

    // ------------------------------------------------------------ 生命周期

    /// DAG 线程即将开始 tick：重新接受请求。
    void on_dag_started() noexcept { stopped_.store(false, std::memory_order_release); }

    /// DAG 线程即将停止：先关入口，再把未消费的请求全部以失败结算。
    /// 与 Python 桥 `transition_and_cancel` 同一原则：停机不能让任何请求悬空。
    std::size_t on_dag_stopping(std::string_view reason) {
        stopped_.store(true, std::memory_order_release);
        const std::size_t settled = fail_pending(reason);
        CFW_LOG_INFO("[Ecs/StructuralCommand] DAG stopping: settled {} pending structural request(s) as failure",
                     settled);
        return settled;
    }

    [[nodiscard]] bool stopped() const noexcept { return stopped_.load(std::memory_order_acquire); }

    /// 装配方是否安装了 DAG tick 边界钩子。安装后 `GeometrySystem::update()` 的
    /// 降级兜底不再 drain —— 保证"实体集合在一个 tick 内不变"。
    [[nodiscard]] bool tick_boundary_installed() const noexcept {
        return boundary_installed_.load(std::memory_order_acquire);
    }

    void notify_tick_boundary_installed() noexcept {
        boundary_installed_.store(true, std::memory_order_release);
    }

    // ------------------------------------------------------------ 应用

    /// 应用入站结构命令（tick 边界阶段 / DAG 降级兜底）。返回是否真的应用了命令。
    bool apply_pending() {
        auto commands = commands_.drain();
        if (commands.empty()) {
            return false;
        }
        for (auto& command : commands) {
            auto request = command.request;
            if (!request) {
                continue;
            }
            try {
                bool applied = false;
                switch (command.kind) {
                    case StructuralCommandKind::CreateGeometry: {
                        CreateGeometryPayload payload = std::move(command.geometry);
                        applied = request->apply([payload = std::move(payload)]() mutable {
                            return create_geometry_entity(std::move(payload));
                        });
                        break;
                    }
                }
                if (!applied && request->abandoned()) {
                    // 等待方已超时放弃：命令被跳过，不创建无人签收的实体。
                    // 成功与失败都由等待方在 wait() 返回时各记一次，这里不重复计数。
                    record_abandoned();
                }
            } catch (const std::exception& error) {
                // settle_failure 唤醒等待方，由它记一次 failed。
                request->settle_failure(std::string{"structural command failed: "} + error.what());
                CFW_LOG_ERROR("[Ecs/StructuralCommand] apply failed: {}", error.what());
            } catch (...) {
                request->settle_failure("structural command failed: unknown exception");
                CFW_LOG_ERROR("[Ecs/StructuralCommand] apply failed: unknown exception");
            }
        }
        return true;
    }

    /// 把所有未消费的请求以失败结算。返回结算的请求数。
    std::size_t fail_pending(std::string_view reason) {
        auto commands = commands_.drain();
        std::size_t settled = 0;
        for (auto& command : commands) {
            if (!command.request) {
                continue;
            }
            if (command.request->settle_failure(std::string(reason))) {
                ++settled;
            }
        }
        if (settled != 0) {
            // 只记"停机时结算了多少条"；失败本身由各等待方各记一次。
            std::lock_guard lock(stats_mutex_);
            stats_.settled_on_stop += settled;
        }
        return settled;
    }

    // ------------------------------------------------------------ 诊断

    [[nodiscard]] Stats stats() const {
        std::lock_guard lock(stats_mutex_);
        return stats_;
    }

    void reset_stats() {
        std::lock_guard lock(stats_mutex_);
        stats_ = Stats{};
    }

    void record_requested() { note([](Stats& s) { ++s.requested; }); }
    void record_inline_applied() { note([](Stats& s) { ++s.inline_applied; }); }
    void record_rejected() { note([](Stats& s) { ++s.rejected; }); }
    void record_applied() { note([](Stats& s) { ++s.applied; }); }
    void record_failed() { note([](Stats& s) { ++s.failed; }); }
    void record_timeout() { note([](Stats& s) { ++s.timeouts; ++s.failed; }); }
    void record_abandoned() { note([](Stats& s) { ++s.abandoned; }); }

   private:
    StructuralCommandChannel() = default;

    template <typename Mutate>
    void note(Mutate&& mutate) {
        std::lock_guard lock(stats_mutex_);
        mutate(stats_);
    }

    Buffer commands_;
    Kernel::ThreadAffinity affinity_;
    std::atomic<bool> stopped_{false};              ///< 停机后拒绝新请求
    std::atomic<bool> boundary_installed_{false};   ///< DAG tick 边界钩子已安装
    std::atomic<std::uint64_t> request_ids_{0};
    std::atomic<std::int64_t> frame_period_us_{kDefaultFramePeriodUs};
    std::atomic<std::int64_t> timeout_override_ms_{structural_timeout_override_ms()};
    mutable std::mutex stats_mutex_;
    Stats stats_{};
};

[[nodiscard]] inline EntityId create_geometry_entity(CreateGeometryPayload payload) {
    auto& world = World::instance();
    const Entity entity = world.create<GeometryDevice, ModelSource>();
    world.get<GeometryDevice>(entity) = std::move(payload.geometry);
    world.get<ModelSource>(entity) = std::move(payload.model_source);
    return to_handle(entity);
}

/// 应用入站结构命令（tick 边界，先于 scene 命令）。绑定/刷新 registry 的拥有者线程。
inline bool apply_pending_structural_commands() {
    auto& channel = StructuralCommandChannel::instance();
    channel.affinity().bind_to_current_thread();
    return channel.apply_pending();
}

/// DAG 降级路径（`CORONA_SYSTEMS_DAG=0`）的兜底 drain：只在装配方**没有**安装
/// tick 边界钩子时生效。DAG 模式下返回 false 且不消费队列，因此实体集合在
/// 一个 tick 内不会因为 GeometrySystem::update() 而改变。
inline bool apply_pending_structural_commands_legacy_fallback() {
    if (StructuralCommandChannel::instance().tick_boundary_installed()) {
        return false;
    }
    return apply_pending_structural_commands();
}

/// DAG 线程启动（由 `SystemsThread` 的 start 钩子调用，接线在 engine.cpp）。
inline void on_dag_thread_started() {
    StructuralCommandChannel::instance().on_dag_started();
}

/// DAG 线程停止（由 `SystemsThread` 的 stop 钩子调用，接线在 engine.cpp）。
inline void on_dag_thread_stopping() {
    StructuralCommandChannel::instance().on_dag_stopping("systems DAG thread is stopping");
}

/// 请求创建几何实体（任意调用线程）。
///
/// - 调用方就是 DAG 线程 → 直接应用（重入规则）；
/// - 否则入队并同步等待，最多 `timeout()`；超时/停机/队列满都返回 0 并写诊断。
///
/// @return 有效实体句柄；失败返回 0（实体不存在，调用方按失败处理）。
[[nodiscard]] inline EntityId request_create_geometry(CreateGeometryPayload payload) {
    auto& channel = StructuralCommandChannel::instance();

    // 停机后即使调用方是 DAG 线程也不允许再写 registry：引擎正在关停，
    // 新建实体会落在一个马上要被拆掉的世界上。所以停机判定在重入判定之前。
    if (channel.stopped()) {
        channel.record_rejected();
        CFW_LOG_ERROR(
            "[Ecs/StructuralCommand] create_geometry rejected: the systems DAG is not accepting "
            "structural commands (stopped)");
        return 0;
    }

    // 重入：系统在 DAG 线程内部创建实体。入队后等自己 = 自死锁。
    if (channel.affinity().is_owner_thread()) {
        channel.record_inline_applied();
        return create_geometry_entity(std::move(payload));
    }

    auto request = std::make_shared<StructuralRequest>(channel.next_request_id());
    StructuralCommand command{};
    command.kind = StructuralCommandKind::CreateGeometry;
    command.request = request;
    command.geometry = std::move(payload);
    if (!channel.commands().push(std::move(command))) {
        channel.record_rejected();
        CFW_LOG_ERROR(
            "[Ecs/StructuralCommand] create_geometry rejected: structural command queue is full "
            "({} pending); the DAG is not draining it",
            channel.commands().size());
        return 0;
    }
    channel.record_requested();

    const auto timeout = channel.timeout();
    const StructuralRequest::Result result = request->wait(timeout);
    if (!result.ok()) {
        // 失败只在这里记一次（超时是它的子集），停机/异常路径不再重复计数。
        const bool abandoned = request->abandoned();
        if (abandoned) {
            channel.record_timeout();
            CFW_LOG_ERROR(
                "[Ecs/StructuralCommand] create_geometry timed out after {}ms: {}; owner thread {}; "
                "the late command is skipped, no entity is created",
                timeout.count(), result.error,
                channel.affinity().bound()
                    ? "is bound but did not drain the request"
                    : "has never drained this channel (DAG not running?)");
        } else {
            channel.record_failed();
            CFW_LOG_ERROR("[Ecs/StructuralCommand] create_geometry failed: {}", result.error);
        }
        return 0;
    }
    channel.record_applied();
    return result.handle;
}

}  // namespace Corona::Ecs
