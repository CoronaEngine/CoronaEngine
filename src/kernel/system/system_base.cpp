#include "corona/kernel/system/system_base.h"

#include <chrono>
#include <algorithm>
#include <exception>
#include <thread>

#include "corona/events/engine_events.h"
#include "horizon/core/logging.h"
#include "corona/kernel/event/i_event_stream.h"

namespace Corona::Kernel {

SystemBase::SystemBase()
    : state_(SystemState::idle),
      should_run_(false),
      is_paused_(false),
      context_(nullptr),
      target_fps_(60),
      frame_number_(0),
      last_delta_time_(0.0f),
      total_frames_(0),
      total_frame_time_(0.0),
      max_frame_time_(0.0f),
      stats_window_size_(60) {}

SystemBase::~SystemBase() {
    if (thread_.joinable()) {
        stop();
    }
}

// ========================================
// ISystem 接口实现
// ========================================

SystemState SystemBase::get_state() const {
    return state_.load(std::memory_order_acquire);
}

int SystemBase::get_target_fps() const {
    return target_fps_;
}

void SystemBase::set_target_fps(int fps) {
    {
        std::lock_guard lock(pause_mutex_);
        target_fps_.store(fps, std::memory_order_release);
    }
    pause_cv_.notify_all();
}

void SystemBase::start() {
    std::lock_guard<std::mutex> lock(control_mutex_);

    // 只能从 idle 或 stopped 状态启动
    if (state_ != SystemState::idle && state_ != SystemState::stopped) {
        return;
    }

    should_run_.store(true, std::memory_order_release);
    is_paused_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> exit_lock(thread_exit_mutex_);
        thread_finished_ = false;
    }
    state_.store(SystemState::running, std::memory_order_release);

    // 创建工作线程
    thread_ = std::thread(&SystemBase::thread_loop, this);
}

void SystemBase::pause() {
    std::lock_guard<std::mutex> lock(control_mutex_);

    if (state_ != SystemState::running) {
        return;
    }

    {
        std::lock_guard lock(pause_mutex_);
        is_paused_.store(true, std::memory_order_release);
        state_.store(SystemState::paused, std::memory_order_release);
    }
    pause_cv_.notify_all();
}

void SystemBase::resume() {
    std::lock_guard<std::mutex> lock(control_mutex_);

    if (state_ != SystemState::paused) {
        return;
    }

    {
        std::lock_guard<std::mutex> pause_lock(pause_mutex_);
        is_paused_.store(false, std::memory_order_release);
        state_.store(SystemState::running, std::memory_order_release);
    }
    pause_cv_.notify_one();  // 唤醒暂停的线程
}

void SystemBase::stop() {
    std::lock_guard<std::mutex> lock(control_mutex_);

    if (state_ == SystemState::stopped || state_ == SystemState::idle) {
        return;
    }

    state_.store(SystemState::stopping, std::memory_order_release);
    should_run_.store(false, std::memory_order_release);

    // 唤醒可能在暂停或等待的线程
    {
        std::lock_guard<std::mutex> pause_lock(pause_mutex_);
        is_paused_.store(false, std::memory_order_release);
    }
    pause_cv_.notify_one();

    // 等待线程结束
    if (thread_.joinable()) {
        thread_.join();
    }

    state_.store(SystemState::stopped, std::memory_order_release);
}

bool SystemBase::stop_for(std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> lock(control_mutex_);

    if (state_ == SystemState::stopped || state_ == SystemState::idle) {
        return true;
    }

    state_.store(SystemState::stopping, std::memory_order_release);
    should_run_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> pause_lock(pause_mutex_);
        is_paused_.store(false, std::memory_order_release);
    }
    pause_cv_.notify_one();

    if (!thread_.joinable()) {
        state_.store(SystemState::stopped, std::memory_order_release);
        return true;
    }

    std::unique_lock<std::mutex> exit_lock(thread_exit_mutex_);
    if (!thread_exit_cv_.wait_for(exit_lock, timeout, [this] { return thread_finished_; })) {
        return false;
    }
    exit_lock.unlock();
    thread_.join();
    state_.store(SystemState::stopped, std::memory_order_release);
    return true;
}

// ========================================
// 性能统计接口实现
// ========================================

float SystemBase::get_actual_fps() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    if (frame_samples_.empty() || stats_elapsed_time_ <= 0.0) return 0.0f;
    return static_cast<float>(frame_samples_.size() / stats_elapsed_time_);
}

float SystemBase::get_average_frame_time() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    if (frame_samples_.empty()) return 0.0f;
    return static_cast<float>((total_frame_time_ / frame_samples_.size()) * 1000.0);
}

float SystemBase::get_max_frame_time() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return max_frame_time_;
}

std::uint64_t SystemBase::get_total_frames() const {
    return total_frames_.load(std::memory_order_relaxed);
}

void SystemBase::reset_stats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    total_frames_.store(0, std::memory_order_relaxed);
    total_frame_time_ = 0.0;
    stats_elapsed_time_ = 0.0;
    frame_samples_.clear();
    max_frame_time_ = 0.0f;
}

// ========================================
// Protected 方法
// ========================================

ISystemContext* SystemBase::context() {
    return context_;
}

const ISystemContext* SystemBase::context() const {
    return context_;
}

uint64_t SystemBase::frame_number() const {
    return frame_number_;
}

float SystemBase::delta_time() const {
    return last_delta_time_;
}

void SystemBase::request_update() {
    {
        std::lock_guard lock(pause_mutex_);
        update_requested_ = true;
    }
    pause_cv_.notify_one();
}

void SystemBase::thread_loop() {
    this->on_thread_started();

    using Clock = std::chrono::steady_clock;
    auto last_time = Clock::now();
    auto last_completion = last_time;
    auto next_deadline = last_time;
    int previous_target = target_fps_.load(std::memory_order_acquire);

    while (should_run_.load(std::memory_order_acquire)) {
        auto frame_start = Clock::now();

        // 检查暂停状态
        if (is_paused_.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(pause_mutex_);
            pause_cv_.wait(lock, [this] {
                return !is_paused_.load(std::memory_order_acquire) ||
                       !should_run_.load(std::memory_order_acquire);
            });
            // 恢复后重置时间，避免大的 delta_time 跳变
            last_time = last_completion = next_deadline = Clock::now();
            continue;
        }

        // 计算 delta_time
        {
            std::lock_guard lock(pause_mutex_);
            // Consume before update(), so a publication concurrent with that
            // update remains pending and cannot be lost at the wait boundary.
            update_requested_ = false;
        }
        auto current_time = Clock::now();
        last_delta_time_ = std::chrono::duration<float>(current_time - last_time).count();
        last_time = current_time;

        // 调用子类的更新逻辑
        try {
            update();
        } catch (const std::exception& error) {
            CFW_LOG_CRITICAL(
                "System '{}' update threw an exception; requesting engine shutdown: {}",
                get_name(),
                error.what());
            if (auto* stream = context_ ? context_->event_stream() : nullptr) {
                stream->get_stream<Events::EngineShutdownEvent>()->publish(Events::EngineShutdownEvent{});
            }
            should_run_.store(false, std::memory_order_release);
            state_.store(SystemState::stopping, std::memory_order_release);
            break;
        } catch (...) {
            CFW_LOG_CRITICAL(
                "System '{}' update threw an unknown exception; requesting engine shutdown",
                get_name());
            if (auto* stream = context_ ? context_->event_stream() : nullptr) {
                stream->get_stream<Events::EngineShutdownEvent>()->publish(Events::EngineShutdownEvent{});
            }
            should_run_.store(false, std::memory_order_release);
            state_.store(SystemState::stopping, std::memory_order_release);
            break;
        }

        ++frame_number_;

        // 收集性能统计
        auto frame_end = Clock::now();
        float frame_time = std::chrono::duration<float>(frame_end - frame_start).count();
        float frame_time_ms = frame_time * 1000.0f;

        // Absolute deadlines avoid accumulating sleep overshoot. Stop, pause,
        // and a changed frame limit interrupt the wait without spinning.
        const int target = target_fps_.load(std::memory_order_acquire);
        if (target != previous_target) next_deadline = frame_start;
        previous_target = target;
        if (target > 0) {
            const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / target));
            next_deadline += period;
            if (next_deadline < frame_end) next_deadline = frame_end;
            std::unique_lock lock(pause_mutex_);
            pause_cv_.wait_until(lock, next_deadline, [&] {
                return !should_run_.load(std::memory_order_acquire) ||
                       is_paused_.load(std::memory_order_acquire) ||
                       update_requested_ ||
                       target_fps_.load(std::memory_order_acquire) != target;
            });
            // Rebase the periodic fallback after event-driven work rather than
            // accumulating future deadlines on every early wakeup.
            if (update_requested_) next_deadline = Clock::now();
        } else {
            next_deadline = frame_end;
        }
        const auto completed = Clock::now();
        const double elapsed = std::chrono::duration<double>(completed - last_completion).count();
        last_completion = completed;

        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            total_frames_.fetch_add(1, std::memory_order_relaxed);
            frame_samples_.emplace_back(frame_time, elapsed);
            total_frame_time_ += frame_time;
            stats_elapsed_time_ += elapsed;
            if (frame_samples_.size() > static_cast<std::size_t>(stats_window_size_)) {
                total_frame_time_ -= frame_samples_.front().first;
                stats_elapsed_time_ -= frame_samples_.front().second;
                frame_samples_.pop_front();
            }
            max_frame_time_ = frame_time_ms;
            for (const auto& sample : frame_samples_)
                max_frame_time_ = std::max(max_frame_time_, sample.first * 1000.0f);
        }
    }

    this->on_thread_stopped();
    {
        std::lock_guard<std::mutex> lock(thread_exit_mutex_);
        thread_finished_ = true;
    }
    thread_exit_cv_.notify_all();
}

void SystemBase::on_thread_started() {
    // 默认实现为空，派生类可覆盖
}

void SystemBase::on_thread_stopped() {
    // 默认实现为空，派生类可覆盖
}

// ========================================
// Private 方法
// ========================================

void SystemBase::set_context(ISystemContext* context) {
    context_ = context;
}

}  // namespace Corona::Kernel
