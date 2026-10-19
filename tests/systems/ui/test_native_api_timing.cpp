#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include "cef/native_api_timing.h"

namespace {

using Corona::Systems::UI::describe_native_request;
using Corona::Systems::UI::is_slow_native_request;
using Corona::Systems::UI::is_stuck_native_request;
using Corona::Systems::UI::kSlowNativeRequestThresholdMs;
using Corona::Systems::UI::kStuckNativeRequestThresholdMs;
using Corona::Systems::UI::NativeApiInFlight;
using Corona::Systems::UI::NativeApiTiming;

// Prints instead of throwing so a failure names itself in ctest output.
void require(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAILED: %s\n", message);
    std::fflush(stderr);
    std::exit(1);
}

}  // namespace

int main() {
    // One native API call runs on the CEF UI thread, which also paints the
    // editor: anything at or above the threshold is what a frozen UI looks like,
    // so the boundary itself must be exact.
    require(kSlowNativeRequestThresholdMs == 50, "threshold");
    require(!is_slow_native_request(0), "fast request");
    require(!is_slow_native_request(kSlowNativeRequestThresholdMs - 1), "just under threshold");
    require(is_slow_native_request(kSlowNativeRequestThresholdMs), "threshold is inclusive");
    require(is_slow_native_request(8123), "seconds-long request");
    require(is_slow_native_request(5, 5), "custom threshold");
    require(!is_slow_native_request(5, 6), "custom threshold above");

    NativeApiTiming timing;
    timing.module = "scene_tools";
    timing.function = "list_scene_tree";
    require(describe_native_request(timing) == "scene_tools.list_scene_tree", "module.function");

    NativeApiTiming module_only;
    module_only.module = "scene_tools";
    require(describe_native_request(module_only) == "scene_tools", "module only");

    NativeApiTiming function_only;
    function_only.function = "on_init";
    require(describe_native_request(function_only) == "on_init", "function only");

    // A request that never returns is invisible to the completion-time report, so
    // the stuck check must fire while the editor is frozen.
    NativeApiInFlight in_flight;
    require(!is_stuck_native_request(in_flight, 60000, 0), "idle editor must never look stuck");

    in_flight.module = "scene_tools";
    in_flight.function = "get_scene_snapshot";
    in_flight.started_ms = 1000;
    in_flight.sequence = 7;
    in_flight.active = true;
    require(!is_stuck_native_request(in_flight, 1000 + kStuckNativeRequestThresholdMs - 1, 0),
            "just under stuck threshold");
    require(is_stuck_native_request(in_flight, 1000 + kStuckNativeRequestThresholdMs, 0),
            "threshold is inclusive");
    require(is_stuck_native_request(in_flight, 999999, 0), "permanent hang");
    require(!is_stuck_native_request(in_flight, 999999, 7),
            "a stuck request is reported once, not on every poll");
    require(is_stuck_native_request(in_flight, 999999, 6),
            "another request's report must not silence this one");

    in_flight.active = false;
    require(!is_stuck_native_request(in_flight, 999999, 0), "completed request is not stuck");
    return 0;
}
