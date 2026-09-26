#include "vision/vision_interop_lifetime.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using Corona::Systems::Vision::drain_vision_interop_submissions;
using Corona::Systems::Vision::vision_zero_copy_disabled_from_value;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void expect(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

struct FakeReceipt {
    int serial{};
    bool is_empty{};

    [[nodiscard]] bool empty() const noexcept { return is_empty; }
};

void zero_copy_disable_flag_accepts_boolean_values() {
    expect(!vision_zero_copy_disabled_from_value(nullptr),
           "unset zero-copy flag should keep zero-copy enabled");
    expect(!vision_zero_copy_disabled_from_value(""),
           "empty zero-copy flag should keep zero-copy enabled");
    expect(!vision_zero_copy_disabled_from_value("0"),
           "zero should keep zero-copy enabled");
    expect(!vision_zero_copy_disabled_from_value("false"),
           "false should keep zero-copy enabled");
    expect(!vision_zero_copy_disabled_from_value("OFF"),
           "OFF should keep zero-copy enabled");
    expect(vision_zero_copy_disabled_from_value("1"),
           "one should disable zero-copy");
    expect(vision_zero_copy_disabled_from_value("true"),
           "true should disable zero-copy");
}

void interop_resources_are_released_only_after_submissions_finish() {
    std::unordered_map<std::uintptr_t, FakeReceipt> receipts{
        {10u, FakeReceipt{.serial = 101, .is_empty = false}},
        {20u, FakeReceipt{.serial = 202, .is_empty = true}},
        {30u, FakeReceipt{.serial = 303, .is_empty = false}},
    };
    std::vector<std::string> events;

    drain_vision_interop_submissions(
        receipts,
        [&](std::uintptr_t camera, const FakeReceipt& receipt) {
            events.push_back("wait:" + std::to_string(camera) + ":" +
                             std::to_string(receipt.serial));
        },
        [&] { events.push_back("release"); });

    expect(receipts.empty(), "draining interop submissions should clear tracked receipts");
    expect(events.size() == 3u,
           "two non-empty receipts should be waited before one release");
    expect(events.back() == "release",
           "interop resources must be released after all receipt waits");
    expect(events[0].starts_with("wait:") && events[1].starts_with("wait:"),
           "all events before release should be receipt waits");
}

}  // namespace

int main() {
    zero_copy_disable_flag_accepts_boolean_values();
    interop_resources_are_released_only_after_submissions_finish();
    std::cout << "Vision interop lifetime tests passed\n";
    return 0;
}
