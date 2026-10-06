#include "corona/kernel/system/system_base.h"

#include <chrono>
#include <atomic>
#include <iostream>
#include <thread>

namespace {

class SleepingSystem final : public Corona::Kernel::SystemBase {
public:
    std::string_view get_name() const override { return "Sleeping"; }
    int get_priority() const override { return 0; }
    bool initialize(Corona::Kernel::ISystemContext*) override { return true; }
    void update() override {
        entered_.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    void shutdown() override {}
    std::atomic<bool> entered_{false};
};

}  // namespace

int main() {
    SleepingSystem system;
    system.start();

    while (!system.entered_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    if (system.stop_for(std::chrono::milliseconds(1))) {
        std::cerr << "expected the first stop attempt to time out\n";
        return 1;
    }

    // The timed stop must leave the worker joinable so the owner can perform
    // a later safe join after the blocking operation returns.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    if (!system.stop_for(std::chrono::milliseconds(100))) {
        std::cerr << "expected the second stop attempt to join the worker\n";
        return 1;
    }
    return 0;
}
