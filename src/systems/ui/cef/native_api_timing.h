#pragma once

#include <cstdint>
#include <string>

namespace Corona::Systems::UI {

/// Timing of one native editor API request.
///
/// Every request runs on the CEF UI thread, which is also the thread that paints
/// the offscreen editor UI.  A slow handler therefore freezes the whole editor
/// while the engine render loop keeps running, so a long request is the signal
/// that identifies what blocked the UI.
struct NativeApiTiming {
    std::string module;
    std::string function;
    std::int64_t elapsed_ms{0};
    bool handled{true};
    bool success{true};
};

/// Requests at or above this budget are reported.  The threshold is deliberately
/// far below the multi-second stalls it must catch, and low enough that a
/// handler gradually drifting into the jank range is noticed before it freezes.
inline constexpr std::int64_t kSlowNativeRequestThresholdMs = 50;

[[nodiscard]] constexpr bool is_slow_native_request(
    std::int64_t elapsed_ms,
    std::int64_t threshold_ms = kSlowNativeRequestThresholdMs) noexcept {
    return elapsed_ms >= threshold_ms;
}

/// Stable label for a request: "scene_tools.list_scene_tree".
[[nodiscard]] inline std::string describe_native_request(const NativeApiTiming& timing) {
    if (timing.module.empty()) {
        return timing.function;
    }
    if (timing.function.empty()) {
        return timing.module;
    }
    return timing.module + "." + timing.function;
}

/// A no-op when no request is being dispatched, so this is also the state of an
/// idle editor.  Only an *active* request that outlives the budget proves that the
/// CEF UI thread is stuck inside a handler.
struct NativeApiInFlight {
    std::string module;
    std::string function;
    std::int64_t started_ms{0};
    std::uint64_t sequence{0};
    std::uint32_t dispatcher_thread_id{0};
    bool active{false};
};

/// Requests still running after this are reported while they are stuck.  A
/// handler that never returns is the one failure the completion-time report can
/// never print, and it is exactly what a permanently frozen editor looks like.
inline constexpr std::int64_t kStuckNativeRequestThresholdMs = 5000;

/// @param reported_sequence sequence of the last request already reported, so one
///        stuck request is announced once instead of on every poll.
[[nodiscard]] constexpr bool is_stuck_native_request(
    const NativeApiInFlight& request,
    std::int64_t now_ms,
    std::uint64_t reported_sequence,
    std::int64_t stuck_after_ms = kStuckNativeRequestThresholdMs) noexcept {
    return request.active && request.sequence != reported_sequence &&
           now_ms - request.started_ms >= stuck_after_ms;
}

}  // namespace Corona::Systems::UI
