#include "vision/vision_interop_lifetime.h"
#include <horizon.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using Corona::Systems::Vision::drain_vision_interop_submissions;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void expect(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

using Receipt = Corona::Horizon::SubmitReceipt;

void interop_resources_are_released_only_after_submissions_finish() {
    std::unordered_map<std::uintptr_t, Receipt> receipts{
        {10u, Receipt{.serial = 101}},
        {20u, Receipt{}},
        {30u, Receipt{.serial = 303}},
    };
    std::vector<std::string> events;

    drain_vision_interop_submissions(
        receipts,
        [&](std::uintptr_t camera, const Receipt& receipt) {
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
    interop_resources_are_released_only_after_submissions_finish();
    std::cout << "Vision interop lifetime tests passed\n";
    return 0;
}
