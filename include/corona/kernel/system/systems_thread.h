/**
 * @file systems_thread.h
 * @brief 单条线程上按 DAG 顺序驱动多个 ISystem 的执行器
 *
 * 背景：引擎现在是"一个系统一条线程"（SystemBase::start() 里 std::thread）。
 * 这种模型下系统之间的真实数据依赖（SharedDataHub 的槽位锁、事件总线）既看不见、
 * 也无法排序，只能靠 priority 决定谁先被初始化，帧内顺序则完全由各线程的节拍
 * 独占决定 —— 于是出现"同一帧里 Geometry 读到的是 Mechanics 上一帧的刚体"这类
 * 不可复现的时序问题，以及 15.8s 那种一个系统卡住、全部系统跟着排队的事故形态。
 *
 * SystemsThread 把这批系统收进**一条线程**，用 FrameGraph 排成 DAG：
 * - 顺序由依赖决定，不由节拍偶然决定（可 dump、可复现）
 * - 慢链路用 period_ticks 降频（M-β：120Hz tick，60Hz 重链路），不影响快链路
 * - 节点边界是唯一的执行序与打点边界，卡在哪一步一眼可见
 *
 * 与 FrameGraph 的分工：FrameGraph 只管拓扑序、门控、异常与统计；SystemsThread
 * 负责线程生命周期、tick 节拍（绝对 deadline，不累积漂移）、暂停/恢复，
 * 以及把 dt 喂给系统（ISystem::feed_frame_time）。
 */

#pragma once

#include <corona/kernel/system/frame_graph.h>
#include <corona/kernel/system/i_system.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Corona::Kernel {

/**
 * @brief 一个待纳入 DAG 的系统
 */
struct SystemsThreadNode {
    ISystem* system{nullptr};              ///< 目标系统（必须是已 initialize 的）
    std::vector<std::string_view> after;   ///< 必须先执行的其他系统名（可空）
    std::uint32_t period_ticks{1};         ///< 速率门控周期，1 = 每 tick 都跑
    std::uint32_t phase{0};                ///< 速率门控相位，必须 < period_ticks
};

/**
 * @brief SystemsThread 的装配参数
 */
struct SystemsThreadConfig {
    std::vector<SystemsThreadNode> nodes;
    std::uint32_t tick_rate{120};  ///< 本线程的 tick 频率（Hz）；0 表示不自动跑，只允许 run_once()
};

/**
 * @brief 单线程 DAG 执行器
 *
 * 线程模型：
 * - prepare() 必须在 start() 之前、在装配线程上调用一次；之后图不再变动
 * - start() 起一条线程，循环 run_tick()；stop() 之后才能析构
 * - 所有 FrameGraph 的写操作只在装配线程；运行期只有那条线程调用 run_tick()
 *   和 stats()，因此 FrameGraph 自身不加锁
 * - run_once() 供测试与"手动驱动"使用，要求线程未启动（或已暂停）
 *
 * 错误处理：
 * - 节点抛异常不会杀掉线程，也不会跨出 run_tick()：记录 failed_node/last_error
 *   并中止本 tick（等同一次掉帧），下一 tick 继续尝试
 * - 连续整理失败不在这里做熔断，交给上层：这里只把失败次数交出去
 */
class SystemsThread {
public:
    explicit SystemsThread(SystemsThreadConfig config);
    ~SystemsThread();

    SystemsThread(const SystemsThread&) = delete;
    SystemsThread& operator=(const SystemsThread&) = delete;

    /**
     * @brief 建图并编译（幂等：重复调用只会重新编译未变动的图）
     * @param error 失败原因（可为 nullptr）
     */
    bool prepare(std::string* error = nullptr);

    /**
     * @brief 启动执行线程；tick_rate == 0 时什么都不做（返回 false）
     */
    bool start();

    /**
     * @brief 暂停循环（当前 tick 完成后阻塞在条件变量上）
     */
    void pause();

    /**
     * @brief 恢复循环
     */
    void resume();

    /**
     * @brief 停止线程并 join；可重复调用
     */
    void stop();

    /**
     * @brief 线程是否在跑（含暂停态）
     */
    [[nodiscard]] bool is_running() const;

    /**
     * @brief 手动跑一个 tick（测试/降级路径）
     *
     * 会推进帧号并记录统计，与线程循环里的行为一致。
     *
     * @param delta_seconds 本 tick 的时间步长（秒）
     */
    FrameTickReport run_once(double delta_seconds);

    /**
     * @brief 逐节点统计（快照，拷贝）
     *
     * 运行中调用也是安全的：内部加锁拷贝，不阻塞 tick。
     */
    [[nodiscard]] FrameGraphStats stats() const;

    /**
     * @brief 图的可读 dump（节点、顺序、统计、最近错误）
     */
    [[nodiscard]] std::string dump() const;

    [[nodiscard]] std::uint32_t tick_rate() const { return tick_rate_.load(std::memory_order_relaxed); }

    /// 运行期改 tick 频率（下一 tick 生效，不重启线程）
    void set_tick_rate(std::uint32_t hz);

    /// 已完成的 tick 数
    [[nodiscard]] std::uint64_t ticks() const { return ticks_.load(std::memory_order_relaxed); }

    /// 最近一次节点异常文本；无异常时为空
    [[nodiscard]] std::string last_error() const;

    /// 节点异常累计次数（每次 failed tick 计一次）
    [[nodiscard]] std::uint64_t failures() const { return failures_.load(std::memory_order_relaxed); }

    /// 最近一次 tick 的墙钟总耗时（毫秒）
    [[nodiscard]] double last_tick_ms() const { return last_tick_ms_.load(std::memory_order_relaxed); }

    /// 节点平均耗时（毫秒），按系统名查询；不存在返回 -1
    [[nodiscard]] double node_average_ms(std::string_view system_name) const;

    /// 提供给测试/上层：图本身（只读）
    [[nodiscard]] const FrameGraph& graph() const { return graph_; }

private:
    void thread_loop(std::stop_token stop_token);
    FrameTickReport tick_locked(double delta_seconds);
    void record(const FrameTickReport& report);
    /// 同一节点连续失败的上报与升级（不关引擎，但必须喊出来）
    void note_node_failure(const FrameTickReport& report, std::uint64_t tick);
    /// 周期性把逐节点统计打到日志（节奏见 kStatsReportTicks）
    void log_stats(std::uint64_t tick_index) const;
    [[nodiscard]] double period_ms() const;

    SystemsThreadConfig config_;
    FrameGraph graph_;
    /// 与 graph_ 节点一一对应的系统，下标即 FrameNodeId
    std::vector<ISystem*> systems_;
    std::atomic<std::uint32_t> tick_rate_;
    bool prepared_{false};

    std::jthread thread_;
    mutable std::mutex state_mutex_;
    std::condition_variable_any wake_cv_;
    bool paused_{false};

    std::uint64_t next_tick_{0};
    std::atomic<std::uint64_t> ticks_{0};
    std::atomic<std::uint64_t> failures_{0};
    std::atomic<double> last_tick_ms_{0.0};
    std::atomic<std::uint64_t> last_delta_ns_{0};

    mutable std::mutex stats_mutex_;
    FrameGraphStats stats_snapshot_{};
    std::string last_error_;

    // 卡在同一个节点时的现场（只在 tick 线程写）
    FrameNodeId failing_node_{kInvalidFrameNodeId};
    std::uint64_t failing_ticks_{0};
    std::uint64_t failing_total_{0};
    bool failing_escalated_{false};
};

}  // namespace Corona::Kernel
