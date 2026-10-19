#pragma once

/// Phase watchdog for engine system threads.
///
/// A system runs its own `update()` loop, and every blocking wait inside it
/// (storage slot locks, resource entry locks, GPU queue acquires, TBB arenas)
/// silently freezes that system for as long as the wait lasts.  From the logs
/// that is indistinguishable from "the system is idle": it simply stops
/// producing output, and the per-system `[UI/Frame]` averages are cumulative, so
/// they stop moving too.  Reporting *which stage* a system entered and how long
/// it has been there is the missing half of that picture.
///
/// Usage:
/// @code
///   Diagnostics::PhaseScope phase{"Geometry", "update"};
///   ...
///   phase.set("imports");      // mark the stage currently running
/// @endcode
/// A separate thread (any thread that keeps ticking, e.g. the engine main loop
/// through UiSystem) calls report_stuck_thread_phases() to print what is stuck.
///
/// Header-only on purpose: the setters are called from many system translation
/// units, so there is no library to link and no build-system change.
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <horizon/core/logging.h>

namespace Corona::Diagnostics {

using PhaseClock = std::chrono::steady_clock;

/// OS thread id, matching the thread id column of the engine log.
[[nodiscard]] inline std::uint32_t phase_watchdog_thread_id() noexcept {
#ifdef _WIN32
    return static_cast<std::uint32_t>(::GetCurrentThreadId());
#else
    return static_cast<std::uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

struct ThreadPhaseRecord {
    std::string          owner;
    std::string          phase;
    PhaseClock::time_point started{};
    PhaseClock::time_point last_reported{};
    bool                 reported{false};
};

inline std::mutex& phase_watchdog_mutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::unordered_map<std::uint32_t, ThreadPhaseRecord>& phase_watchdog_table() {
    static std::unordered_map<std::uint32_t, ThreadPhaseRecord> table;
    return table;
}

/// Enter a phase: the stage is now visible to report_stuck_thread_phases().
inline void enter_thread_phase(std::string_view owner, std::string_view phase) {
    std::lock_guard<std::mutex> lock(phase_watchdog_mutex());
    auto& record = phase_watchdog_table()[phase_watchdog_thread_id()];
    record.owner = std::string{owner};
    record.phase = std::string{phase};
    record.started = PhaseClock::now();
    record.reported = false;
}

/// Leave the current phase: an idle system reports nothing.
inline void leave_thread_phase() {
    std::lock_guard<std::mutex> lock(phase_watchdog_mutex());
    phase_watchdog_table().erase(phase_watchdog_thread_id());
}

/// RAII phase marker. The whole `update()` is usually one scope, with set() used
/// to name the stage inside it.
///
/// Scopes nest: the inner scope restores the outer phase when it ends, so a
/// helper (e.g. ResourceManager::import_async) can borrow the current thread's
/// phase without erasing the outer record on the way out.
class PhaseScope {
   public:
    PhaseScope(std::string_view owner, std::string_view phase) {
        std::lock_guard<std::mutex> lock(phase_watchdog_mutex());
        auto& table = phase_watchdog_table();
        const auto thread_id = phase_watchdog_thread_id();
        if (const auto it = table.find(thread_id); it != table.end()) {
            previous_ = it->second;
        }
        ThreadPhaseRecord& record = table[thread_id];
        record.owner = std::string{owner};
        record.phase = std::string{phase};
        record.started = PhaseClock::now();
        record.reported = false;
    }

    ~PhaseScope() {
        std::lock_guard<std::mutex> lock(phase_watchdog_mutex());
        const auto thread_id = phase_watchdog_thread_id();
        if (previous_) {
            phase_watchdog_table()[thread_id] = *previous_;
        } else {
            phase_watchdog_table().erase(thread_id);
        }
    }

    PhaseScope(const PhaseScope&) = delete;
    PhaseScope& operator=(const PhaseScope&) = delete;

    void set(std::string_view phase) {
        std::lock_guard<std::mutex> lock(phase_watchdog_mutex());
        auto it = phase_watchdog_table().find(phase_watchdog_thread_id());
        if (it == phase_watchdog_table().end()) return;
        it->second.phase = std::string{phase};
        it->second.started = PhaseClock::now();
        it->second.reported = false;
    }

   private:
    std::optional<ThreadPhaseRecord> previous_{};
};

/// Requests stuck longer than this are reported while they are still stuck.
inline constexpr std::int64_t kStuckPhaseThresholdMs = 3000;
/// One stuck phase is announced at most this often.
inline constexpr std::int64_t kStuckPhaseReportIntervalMs = 5000;

/// Overwrite the current thread's phase without restoring it on scope exit.
///
/// Use inside a long function that has no PhaseScope of its own (helper called
/// from an instrumented update): the outermost scope still clears the record when
/// it ends, so a marker may be left behind safely.
inline void mark_thread_phase(std::string_view owner, std::string_view phase) {
    std::lock_guard<std::mutex> lock(phase_watchdog_mutex());
    ThreadPhaseRecord& record = phase_watchdog_table()[phase_watchdog_thread_id()];
    record.owner = std::string{owner};
    record.phase = std::string{phase};
    record.started = PhaseClock::now();
    record.reported = false;
}

/// Print every phase that has been running longer than the threshold.
///
/// @return number of phases reported
inline std::size_t report_stuck_thread_phases(
    std::int64_t threshold_ms = kStuckPhaseThresholdMs,
    std::int64_t report_interval_ms = kStuckPhaseReportIntervalMs) {
    const auto now = PhaseClock::now();
    struct Pending {
        std::uint32_t thread_id;
        std::string   owner;
        std::string   phase;
        std::int64_t  elapsed_ms;
    };
    std::vector<Pending> pending;
    {
        std::lock_guard<std::mutex> lock(phase_watchdog_mutex());
        for (auto& [thread_id, record] : phase_watchdog_table()) {
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - record.started).count();
            if (elapsed_ms < threshold_ms) continue;
            if (record.reported &&
                std::chrono::duration_cast<std::chrono::milliseconds>(now - record.last_reported).count() <
                    report_interval_ms) {
                continue;
            }
            record.reported = true;
            record.last_reported = now;
            pending.push_back({thread_id, record.owner, record.phase, elapsed_ms});
        }
    }
    for (const auto& item : pending) {
        CFW_LOG_WARNING("[PhaseWatchdog] thread={} system='{}' stuck in phase '{}' for {}ms",
                        item.thread_id, item.owner, item.phase, item.elapsed_ms);
    }
    return pending.size();
}

}  // namespace Corona::Diagnostics
