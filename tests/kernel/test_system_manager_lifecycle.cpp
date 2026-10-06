#include "corona/kernel/system/i_system_manager.h"
#include "corona/kernel/system/system_base.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

namespace {

class PassiveSystem final : public Corona::Kernel::SystemBase {
public:
    explicit PassiveSystem(std::string_view name) : name_(name) {}

    std::string_view get_name() const override { return name_; }
    int get_priority() const override { return 0; }
    bool initialize(Corona::Kernel::ISystemContext*) override { return true; }
    void update() override {}
    void shutdown() override {}

private:
    std::string_view name_;
};

class ReentrantStopSystem final : public Corona::Kernel::SystemBase {
public:
    explicit ReentrantStopSystem(Corona::Kernel::ISystemManager* manager)
        : manager_(manager) {}

    std::string_view get_name() const override { return "Reentrant"; }
    int get_priority() const override { return 1; }
    bool initialize(Corona::Kernel::ISystemContext*) override { return true; }
    void update() override {}
    void shutdown() override {}
    void stop() override {
        found_peer_.store(manager_->get_system("Peer") != nullptr,
                          std::memory_order_release);
    }

    std::atomic<bool> found_peer_{false};

private:
    Corona::Kernel::ISystemManager* manager_;
};

}  // namespace

int main() {
    auto manager = Corona::Kernel::create_system_manager();
    auto peer = std::make_shared<PassiveSystem>("Peer");
    auto reentrant = std::make_shared<ReentrantStopSystem>(manager.get());
    manager->register_system(peer);
    manager->register_system(reentrant);

    std::atomic<bool> completed{false};
    std::thread watchdog([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!completed.load(std::memory_order_acquire)) {
            std::cerr << "stop_all held the manager lock while invoking a system callback\n";
            std::_Exit(2);
        }
    });

    manager->stop_all();
    completed.store(true, std::memory_order_release);
    watchdog.join();

    if (!reentrant->found_peer_.load(std::memory_order_acquire)) {
        std::cerr << "reentrant system lookup did not find the registered peer\n";
        return 1;
    }
    return 0;
}
