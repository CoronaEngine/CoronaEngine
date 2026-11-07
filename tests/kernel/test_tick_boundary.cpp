/**
 * @file test_tick_boundary.cpp
 * @brief SystemsThread tick 边界阶段契约测试
 *
 * 边界阶段不是节点：它没有数据依赖、不受 period 门控、必须在**所有节点之前**
 * 执行一次，用来落地结构变更与全局参数（P4）。
 *
 * 覆盖：
 * - 每个 tick 只跑一次，且在所有节点之前
 * - 抛异常只中止本 tick，不死线程，下一 tick 恢复
 * - 耗时计入 FrameTickReport::total_ms，并可从 last_boundary_ms() 读到
 * - 未设置钩子时行为完全不变
 */

#include <corona/kernel/system/systems_thread.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void check_impl(bool ok, const char* text, const char* file, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "  [FAIL] %s:%d: %s\n", file, line, text);
    }
}

#define CHECK(cond) check_impl((cond), #cond, __FILE__, __LINE__)

class FakeSystem : public Corona::Kernel::ISystem {
   public:
    FakeSystem(std::string name, std::vector<std::string>* journal)
        : name_(std::move(name)), journal_(journal) {}

    std::string_view get_name() const override { return name_; }
    int get_priority() const override { return 0; }
    bool initialize(Corona::Kernel::ISystemContext*) override { return true; }
    void shutdown() override {}
    void update() override {
        ++updates;
        if (journal_ != nullptr) journal_->push_back(name_);
    }

    int get_target_fps() const override { return 60; }
    Corona::Kernel::SystemState get_state() const override {
        return Corona::Kernel::SystemState::running;
    }
    void start() override {}
    void pause() override {}
    void resume() override {}
    void stop() override {}
    float get_actual_fps() const override { return 0.0f; }
    float get_average_frame_time() const override { return 0.0f; }
    float get_max_frame_time() const override { return 0.0f; }
    std::uint64_t get_total_frames() const override {
        return static_cast<std::uint64_t>(updates);
    }
    void reset_stats() override {}

    int updates{0};

   private:
    std::string name_;
    std::vector<std::string>* journal_{nullptr};
};

Corona::Kernel::SystemsThreadNode node_of(FakeSystem& system) {
    Corona::Kernel::SystemsThreadNode node;
    node.system = &system;
    node.period_ticks = 1;
    node.phase = 0;
    return node;
}

void test_hook_runs_before_all_nodes() {
    std::vector<std::string> journal;
    FakeSystem first{"A", &journal};
    FakeSystem second{"B", &journal};

    Corona::Kernel::SystemsThread thread{{.nodes = {node_of(first), node_of(second)},
                                          .tick_rate = 0}};
    CHECK(thread.prepare());
    thread.set_tick_boundary_hook([&journal]() { journal.push_back("boundary"); });

    (void)thread.run_once(0.016);

    CHECK(journal.size() == 3);
    CHECK(journal[0] == "boundary");
    CHECK(first.updates == 1);
    CHECK(second.updates == 1);
}

void test_hook_runs_once_per_tick() {
    std::atomic<int> boundary_calls{0};
    std::vector<std::string> journal;
    FakeSystem system{"A", &journal};

    Corona::Kernel::SystemsThread thread{{.nodes = {node_of(system)}, .tick_rate = 0}};
    CHECK(thread.prepare());
    thread.set_tick_boundary_hook([&boundary_calls]() { ++boundary_calls; });

    for (int i = 0; i < 3; ++i) {
        (void)thread.run_once(0.016);
    }

    CHECK(boundary_calls.load() == 3);
    CHECK(system.updates == 3);
}

void test_hook_exception_aborts_tick_only() {
    std::atomic<bool> armed{true};
    std::vector<std::string> journal;
    FakeSystem system{"A", &journal};

    Corona::Kernel::SystemsThread thread{{.nodes = {node_of(system)}, .tick_rate = 0}};
    CHECK(thread.prepare());
    thread.set_tick_boundary_hook([&armed]() {
        if (armed.load()) {
            throw std::runtime_error{"boundary boom"};
        }
    });

    const auto failed = thread.run_once(0.016);
    CHECK(!failed.completed);
    CHECK(!failed.error.empty());
    CHECK(system.updates == 0);  // 边界失败 → 本 tick 节点不执行

    armed.store(false);
    const auto recovered = thread.run_once(0.016);
    CHECK(recovered.completed);
    CHECK(system.updates == 1);  // 下一 tick 正常
    CHECK(thread.is_running() == false);  // 线程没被拉起来，也没有异常逃逸
}

void test_boundary_time_is_counted() {
    std::vector<std::string> journal;
    FakeSystem system{"A", &journal};

    Corona::Kernel::SystemsThread thread{{.nodes = {node_of(system)}, .tick_rate = 0}};
    CHECK(thread.prepare());
    thread.set_tick_boundary_hook([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    });

    const auto report = thread.run_once(0.016);
    CHECK(report.completed);
    CHECK(thread.last_boundary_ms() >= 4.0);
    CHECK(report.total_ms >= thread.last_boundary_ms());
}

void test_no_hook_keeps_behavior() {
    std::vector<std::string> journal;
    FakeSystem system{"A", &journal};

    Corona::Kernel::SystemsThread thread{{.nodes = {node_of(system)}, .tick_rate = 0}};
    CHECK(thread.prepare());

    const auto report = thread.run_once(0.016);
    CHECK(report.completed);
    CHECK(system.updates == 1);
    CHECK(thread.last_boundary_ms() == 0.0);
}

}  // namespace

int main() {
    std::printf("== SystemsThread tick boundary tests ==\n");
    test_hook_runs_before_all_nodes();
    test_hook_runs_once_per_tick();
    test_hook_exception_aborts_tick_only();
    test_boundary_time_is_counted();
    test_no_hook_keeps_behavior();

    if (g_failures == 0) {
        std::printf("OK: %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("FAILED: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
