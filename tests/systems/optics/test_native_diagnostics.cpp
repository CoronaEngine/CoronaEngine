#include "native_diagnostics.h"
#include "common/diagnostic_env.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void expect(bool condition, std::string_view message) {
    if (!condition) fail(message);
}

void expect_near(double actual, double expected, std::string_view message) {
    if (std::abs(actual - expected) > 1.0e-6) fail(message);
}

void diagnostic_flags_default_off_and_accept_truthy_values() {
    using Corona::Systems::Diagnostics::parse_env_flag;
    expect(!parse_env_flag(nullptr), "missing flag must be disabled");
    expect(!parse_env_flag(""), "empty flag must be disabled");
    expect(!parse_env_flag("0"), "zero flag must be disabled");
    expect(!parse_env_flag("OFF"), "off flag must be case insensitive");
    expect(parse_env_flag("1"), "one flag must be enabled");
    expect(parse_env_flag("yes"), "yes flag must be enabled");
}

void cascade_mask_defaults_to_all_and_clamps_to_four_bits() {
    using Corona::Systems::OpticsDetail::parse_shadow_cascade_mask;
    expect(parse_shadow_cascade_mask(std::nullopt) == 0x0fu,
           "missing cascade mask must enable all four cascades");
    expect(parse_shadow_cascade_mask(0x05u) == 0x05u,
           "explicit cascade bits must be preserved");
    expect(parse_shadow_cascade_mask(0xffu) == 0x0fu,
           "cascade mask must ignore unsupported high bits");
    expect(parse_shadow_cascade_mask(0u) == 0u,
           "zero mask must support disabling every cascade diagnostically");
}

void native_perf_window_aggregates_cpu_and_workload_metrics() {
    using namespace Corona::Systems::OpticsDetail;
    NativePerfWindow window;
    NativePerfSample first;
    first.total_ms = 10.0;
    first.throttle_wait_ms = 2.0;
    first.collect_ms = 3.0;
    first.submit_ms = 4.0;
    first.shadow_record_ms = 1.0;
    first.commit_ms = 0.5;
    first.instance_count = 4;
    first.visibility_draws = 3;
    first.visibility_indices = 30;
    first.cascade_draws = {2, 2, 1, 1};
    first.cascade_indices = {20, 20, 10, 10};
    window.add(first);

    NativePerfSample second = first;
    second.total_ms = 20.0;
    second.throttle_wait_ms = 6.0;
    second.instance_count = 8;
    second.visibility_draws = 5;
    second.visibility_indices = 50;
    second.cascade_draws = {4, 3, 2, 1};
    second.cascade_indices = {40, 30, 20, 10};
    window.add(second);

    const auto stats = window.snapshot();
    expect(stats.samples == 2, "window must count samples");
    expect_near(stats.avg_total_ms, 15.0, "window must average total time");
    expect_near(stats.max_total_ms, 20.0, "window must retain max total time");
    expect_near(stats.p95_total_ms, 20.0,
                "two-sample p95 must use the nearest-rank upper sample");
    expect_near(stats.avg_throttle_wait_ms, 4.0,
                "window must average throttle wait time");
    expect(stats.max_instances == 8, "window must retain maximum instances");
    expect_near(stats.avg_visibility_draws, 4.0,
                "window must average visibility draws");
    expect_near(stats.avg_visibility_indices, 40.0,
                "window must average visibility indices");
    expect_near(stats.avg_cascade_draws[0], 3.0,
                "window must average per-cascade draws");
    expect_near(stats.avg_cascade_indices[2], 15.0,
                "window must average per-cascade indices");

    window.reset();
    expect(window.snapshot().samples == 0, "reset must clear the window");

    NativePerfWindow percentile_window;
    for (int value = 1; value <= 20; ++value) {
        NativePerfSample sample;
        sample.total_ms = static_cast<double>(value);
        percentile_window.add(sample);
    }
    expect_near(percentile_window.snapshot().p95_total_ms, 19.0,
                "p95 must use nearest-rank selection over frame samples");
}

}  // namespace

int main() {
    diagnostic_flags_default_off_and_accept_truthy_values();
    cascade_mask_defaults_to_all_and_clamps_to_four_bits();
    native_perf_window_aggregates_cpu_and_workload_metrics();
    std::cout << "Native diagnostics tests passed\n";
    return 0;
}
