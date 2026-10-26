/**
 * @file systems_thread.cpp
 * @brief 单线程 DAG 执行器实现
 */

#include <corona/kernel/system/systems_thread.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace Corona::Kernel {
namespace {

using Clock = std::chrono::steady_clock;

/// 单次 tick 允许的最大 dt：暂停/断点/长阻塞之后不要让物理与动画一次性跳太远
constexpr double kMaxDeltaSeconds = 0.25;

/// 追赶上限：落后超过这么多 tick 就重新对齐 deadline，不做逐帧补跑
constexpr std::uint64_t kMaxCatchUpTicks = 4;

double to_ms(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

}  // namespace

SystemsThread::SystemsThread(SystemsThreadConfig config)
    : config_(std::move(config)),
      tick_rate_(config_.tick_rate) {
    systems_.reserve(config_.nodes.size());
}

SystemsThread::~SystemsThread() {
    stop();
}

bool SystemsThread::prepare(std::string* error) {
    graph_ = FrameGraph{};
    systems_.clear();
    prepared_ = false;

    // 第一遍：登记节点，同时建立 "系统名 → 节点 id" 映射（依赖按名字给）
    for (const auto& spec : config_.nodes) {
        if (spec.system == nullptr) {
            if (error) *error = "systems thread: null system in node list";
            return false;
        }
        const std::string_view name = spec.system->get_name();
        FrameNodeDesc desc;
        desc.name = std::string{name};
        desc.period_ticks = std::max<std::uint32_t>(1, spec.period_ticks);
        desc.phase = desc.period_ticks > 0 ? spec.phase % desc.period_ticks : 0;
        ISystem* system = spec.system;
        desc.body = [system](const FrameTick& tick) {
            system->feed_frame_time(static_cast<float>(tick.delta_seconds), tick.tick);
            system->update();
            return FrameNodeStatus::ran;
        };
        try {
            systems_.push_back(system);
            graph_.add_node(std::move(desc));
        } catch (const std::invalid_argument& ex) {
            if (error) *error = std::string{"systems thread: "} + ex.what();
            return false;
        }
    }

    // 第二遍：把 "after 系统名" 解析成节点 id。这里允许构造出环，
    // 由 compile() 拒绝 —— 正是 set_dependencies() 存在的理由。
    for (std::size_t i = 0; i < config_.nodes.size(); ++i) {
        const auto& spec = config_.nodes[i];
        if (spec.after.empty()) continue;

        std::vector<FrameNodeId> deps;
        deps.reserve(spec.after.size());
        for (const std::string_view dep_name : spec.after) {
            bool found = false;
            for (std::size_t j = 0; j < config_.nodes.size(); ++j) {
                if (config_.nodes[j].system->get_name() == dep_name) {
                    deps.push_back(static_cast<FrameNodeId>(j));
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (error) {
                    *error = "systems thread: node '" + std::string{spec.system->get_name()} +
                             "' depends on unknown system '" + std::string{dep_name} + "'";
                }
                return false;
            }
        }
        graph_.set_dependencies(static_cast<FrameNodeId>(i), std::move(deps));
    }

    if (!graph_.compile(error)) {
        return false;
    }

    prepared_ = true;
    return true;
}

bool SystemsThread::start() {
    if (is_running()) return true;
    if (!prepared_) {
        if (!prepare(nullptr)) return false;
    }
    if (tick_rate_.load(std::memory_order_relaxed) == 0) {
        // 不自动驱动：保留手动 run_once() 模式（例如编辑器里逐帧调试）
        return false;
    }

    {
        std::lock_guard lock{state_mutex_};
        paused_ = false;
        next_tick_ = 0;
    }
    thread_ = std::jthread{[this](std::stop_token token) { thread_loop(token); }};
    return true;
}

void SystemsThread::pause() {
    {
        std::lock_guard lock{state_mutex_};
        paused_ = true;
    }
    wake_cv_.notify_all();
}

void SystemsThread::resume() {
    {
        std::lock_guard lock{state_mutex_};
        paused_ = false;
        // 恢复后不要把暂停期间的时间一次性补回来
        next_tick_ = 0;
    }
    wake_cv_.notify_all();
}

void SystemsThread::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        {
            std::lock_guard lock{state_mutex_};
            paused_ = false;
        }
        wake_cv_.notify_all();
        thread_.join();
    }
}

bool SystemsThread::is_running() const {
    return thread_.joinable();
}

void SystemsThread::set_tick_rate(std::uint32_t hz) {
    tick_rate_.store(hz, std::memory_order_relaxed);
    wake_cv_.notify_all();
}

double SystemsThread::period_ms() const {
    const auto hz = tick_rate_.load(std::memory_order_relaxed);
    return hz == 0 ? 0.0 : 1000.0 / static_cast<double>(hz);
}

FrameTickReport SystemsThread::run_once(double delta_seconds) {
    FrameTickReport report = tick_locked(delta_seconds);
    record(report);
    return report;
}

FrameTickReport SystemsThread::tick_locked(double delta_seconds) {
    const double dt = std::clamp(delta_seconds, 0.0, kMaxDeltaSeconds);
    const std::uint64_t tick = ticks_.load(std::memory_order_relaxed);
    FrameTickReport report;
    if (prepared_) {
        report = graph_.run_tick(FrameTick{tick, dt});
    }
    return report;
}

void SystemsThread::record(const FrameTickReport& report) {
    ticks_.fetch_add(1, std::memory_order_relaxed);
    last_tick_ms_.store(report.total_ms, std::memory_order_relaxed);
    if (!report.completed) {
        failures_.fetch_add(1, std::memory_order_relaxed);
    }

    // 统计快照与最后一帧错误：运行期读取方（编辑器/诊断）随时可能来取
    FrameGraphStats snapshot = graph_.snapshot_stats();
    std::lock_guard lock{stats_mutex_};
    stats_snapshot_ = std::move(snapshot);
    if (!report.error.empty()) {
        last_error_ = report.error;
    }
}

void SystemsThread::thread_loop(std::stop_token stop_token) {
    using namespace std::chrono_literals;

    auto last = Clock::now();
    while (!stop_token.stop_requested()) {
        {
            std::unique_lock lock{state_mutex_};
            if (paused_) {
                wake_cv_.wait(lock, [&] {
                    return !paused_ || stop_token.stop_requested() ||
                           tick_rate_.load(std::memory_order_relaxed) == 0;
                });
                last = Clock::now();
                if (stop_token.stop_requested()) break;
                if (tick_rate_.load(std::memory_order_relaxed) == 0) {
                    // 运行期被降级为手动模式：不空转，等一下再说
                    wake_cv_.wait_for(lock, 50ms);
                    last = Clock::now();
                    continue;
                }
            }
        }

        const auto period_ns = std::chrono::nanoseconds{
            static_cast<std::int64_t>(period_ms() * 1e6)};
        if (period_ns.count() <= 0) {
            std::this_thread::sleep_for(1ms);
            continue;
        }

        const auto now = Clock::now();
        const double dt = std::min(std::chrono::duration<double>(now - last).count(), kMaxDeltaSeconds);
        last = now;

        const auto begin = Clock::now();
        const FrameTickReport report = tick_locked(dt);
        record(report);
        const auto cost = Clock::now() - begin;

        // 绝对 deadline 节拍：睡眠到下一个边界，避免"计算耗时 + 睡眠"的累积漂移
        const auto deadline = Clock::now() + (period_ns - cost);
        std::unique_lock lock{state_mutex_};
        wake_cv_.wait_until(lock, deadline, [&] {
            return stop_token.stop_requested() || paused_ ||
                   tick_rate_.load(std::memory_order_relaxed) == 0;
        });
        if (stop_token.stop_requested()) break;

        // 落后太多（例如一次长阻塞）就重新对齐，不做逐帧补跑
        const auto behind = Clock::now() - deadline;
        if (behind > period_ns * static_cast<std::int64_t>(kMaxCatchUpTicks)) {
            last = Clock::now();
        }
    }
}

FrameGraphStats SystemsThread::stats() const {
    std::lock_guard lock{stats_mutex_};
    return stats_snapshot_;
}

std::string SystemsThread::dump() const {
    std::string text = graph_.dump();
    text += "\n# systems_thread\n";
    text += "  tick_rate=" + std::to_string(tick_rate_.load(std::memory_order_relaxed)) + "Hz";
    text += "  ticks=" + std::to_string(ticks_.load(std::memory_order_relaxed));
    text += "  failures=" + std::to_string(failures_.load(std::memory_order_relaxed));
    text += "  last_tick_ms=" + std::to_string(last_tick_ms_.load(std::memory_order_relaxed));
    text += "\n";
    const std::string error = last_error();
    if (!error.empty()) {
        text += "  last_error=" + error + "\n";
    }
    return text;
}

std::string SystemsThread::last_error() const {
    std::lock_guard lock{stats_mutex_};
    return last_error_;
}

double SystemsThread::node_average_ms(std::string_view system_name) const {
    const FrameGraphStats snapshot = stats();
    for (const auto& node : snapshot.nodes) {
        if (node.name == system_name) {
            return node.stats.average_ms;
        }
    }
    return -1.0;
}

}  // namespace Corona::Kernel
