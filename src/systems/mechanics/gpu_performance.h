#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Corona::Systems::GpuPerformance {

// These are host measurements, never GPU kernel execution times. Stage wall
// times are inclusive (nested stages must not be summed). A wait can include
// work queued by preceding stages. Byte counters count recorded transfer
// payloads, not measured PCIe traffic; dispatches count recorded commands,
// including commands whose shader exits early. Failed attempts remain counted.
struct Sample {
    std::uint64_t calls{}, commit_calls{}, wait_calls{}, dispatches{};
    std::uint64_t upload_bytes{}, readback_bytes{}, device_copy_bytes{}, work_items{};
    double wall_ms{}, commit_ms{}, wait_ms{};
};
using Snapshot = std::map<std::string, Sample, std::less<>>;

namespace Detail {
using Clock = std::chrono::steady_clock;
struct Context {
    Snapshot samples;
    Sample* current{};
    unsigned active_scopes{};
    int override_enabled{-1};
};
inline thread_local Context context;
inline double milliseconds(Clock::time_point start) noexcept {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
}  // namespace Detail

inline bool enabled_setting(std::string_view value) noexcept {
    return !value.empty() && value != "0" && value != "false" && value != "FALSE" && value != "off" && value != "OFF";
}
inline bool enabled() noexcept {
    if (Detail::context.override_enabled >= 0) return Detail::context.override_enabled != 0;
    static const bool environment_enabled = [] {
        const char* value = std::getenv("CORONA_MECHANICS_GPU_PROFILE");
        return value && enabled_setting(value);
    }();
    return environment_enabled;
}

// Overrides the environment for this thread, primarily for tests and tools.
// Does not reset samples, so a nested capture preserves its caller's data.
class Capture {
public:
    explicit Capture(bool value = true) noexcept : previous_(Detail::context.override_enabled) {
        Detail::context.override_enabled = value ? 1 : 0;
    }
    ~Capture() { Detail::context.override_enabled = previous_; }
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
private:
    int previous_;
};

class Scope {
public:
    explicit Scope(std::string_view stage) : previous_(Detail::context.current) {
        if (!enabled()) return;
        sample_ = &Detail::context.samples[std::string(stage)];
        ++sample_->calls;
        ++Detail::context.active_scopes;
        Detail::context.current = sample_;
        start_ = Detail::Clock::now();
    }
    ~Scope() {
        if (!sample_) return;
        sample_->wall_ms += Detail::milliseconds(start_);
        Detail::context.current = previous_;
        --Detail::context.active_scopes;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Sample* sample_{};
    Sample* previous_{};
    Detail::Clock::time_point start_{};
};

enum class Operation { Commit, Wait };
class OperationScope {
public:
    explicit OperationScope(Operation operation) noexcept : operation_(operation) {
        if (!enabled()) return;
        sample_ = Detail::context.current;
        if (!sample_) return;
        if (operation == Operation::Commit) ++sample_->commit_calls;
        else ++sample_->wait_calls;
        start_ = Detail::Clock::now();
    }
    ~OperationScope() {
        if (!sample_) return;
        const double elapsed = Detail::milliseconds(start_);
        if (operation_ == Operation::Commit) sample_->commit_ms += elapsed;
        else sample_->wait_ms += elapsed;
    }
    OperationScope(const OperationScope&) = delete;
    OperationScope& operator=(const OperationScope&) = delete;
private:
    Operation operation_;
    Sample* sample_{};
    Detail::Clock::time_point start_{};
};

inline void record_upload(std::uint64_t bytes) noexcept {
    if (enabled() && Detail::context.current) Detail::context.current->upload_bytes += bytes;
}
inline void record_readback(std::uint64_t bytes) noexcept {
    if (enabled() && Detail::context.current) Detail::context.current->readback_bytes += bytes;
}
inline void record_device_copy(std::uint64_t bytes) noexcept {
    if (enabled() && Detail::context.current) Detail::context.current->device_copy_bytes += bytes;
}
inline void record_dispatch(std::uint64_t count = 1) noexcept {
    if (enabled() && Detail::context.current) Detail::context.current->dispatches += count;
}
inline void record_work_items(std::uint64_t count) noexcept {
    if (enabled() && Detail::context.current) Detail::context.current->work_items += count;
}

// Call outside measured scopes. Returning copies keeps consumers independent
// of backend lifetime and makes per-thread collection explicit.
inline void reset() {
    if (Detail::context.active_scopes) throw std::logic_error("Cannot reset active mechanics GPU performance scopes");
    Detail::context.samples.clear();
}
inline Snapshot snapshot() { return Detail::context.samples; }
inline std::string format(const Snapshot& samples) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(3);
    for (const auto& [stage, value] : samples) {
        text << "[MechanicsGpuPerf] stage=" << stage << " calls=" << value.calls
             << " host_inclusive_ms=" << value.wall_ms
             << " commit_calls=" << value.commit_calls << " host_commit_ms=" << value.commit_ms
             << " wait_calls=" << value.wait_calls << " host_wait_ms=" << value.wait_ms
             << " dispatches=" << value.dispatches << " upload_bytes=" << value.upload_bytes
             << " readback_bytes=" << value.readback_bytes << " device_copy_bytes=" << value.device_copy_bytes
             << " work_items=" << value.work_items << '\n';
    }
    return text.str();
}
}  // namespace Corona::Systems::GpuPerformance
