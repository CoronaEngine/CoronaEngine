/**
 * @file test_systems_thread.cpp
 * @brief SystemsThread（单线程 DAG 执行器）契约测试
 *
 * 覆盖：
 * - 依赖装配：按名字解析、未知依赖、依赖环
 * - 执行序：run_once() 按拓扑序调用 update()
 * - 帧时间注入：feed_frame_time() 收到正确的 dt 与帧号
 * - 速率门控：period_ticks=2 的节点隔 tick 执行，且不影响下游
 * - 异常隔离：节点抛异常只中止本 tick，线程/执行器继续可用
 * - 线程生命周期：start/pause/resume/stop 与 tick 计数
 */

#include <corona/kernel/system/systems_thread.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
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
#define CHECK_MSG(cond, msg) check_impl((cond), (msg), __FILE__, __LINE__)

using Clock = std::chrono::steady_clock;

/**
 * @brief 记录调用序、dt 与帧号的假系统
 */
class FakeSystem : public Corona::Kernel::ISystem {
public:
    FakeSystem(std::string name, std::vector<std::string>* journal, bool throw_on_update = false)
        : name_(std::move(name)), journal_(journal), throw_on_update_(throw_on_update) {}

    std::string_view get_name() const override { return name_; }
    int get_priority() const override { return 0; }
    bool initialize(Corona::Kernel::ISystemContext*) override { return true; }
    void shutdown() override {}

    void update() override {
        if (throw_on_update_) {
            throw std::runtime_error{"fake update failure"};
        }
        ++updates;
        if (journal_ != nullptr) journal_->push_back(name_);
    }

    void feed_frame_time(float dt, std::uint64_t frame_number) override {
        ++feeds;
        last_dt = dt;
        last_frame = frame_number;
    }

    int get_target_fps() const override { return 60; }
    Corona::Kernel::SystemState get_state() const override { return Corona::Kernel::SystemState::running; }
    void start() override {}
    void pause() override {}
    void resume() override {}
    void stop() override {}
    float get_actual_fps() const override { return 0.0f; }
    float get_average_frame_time() const override { return 0.0f; }
    float get_max_frame_time() const override { return 0.0f; }
    std::uint64_t get_total_frames() const override { return static_cast<std::uint64_t>(updates); }
    void reset_stats() override {}

    int updates{0};
    int feeds{0};
    float last_dt{0.0f};
    std::uint64_t last_frame{0};

private:
    std::string name_;
    std::vector<std::string>* journal_{nullptr};
    bool throw_on_update_{false};
};

Corona::Kernel::SystemsThreadNode node_of(FakeSystem& system,
                                          std::vector<std::string_view> after = {},
                                          std::uint32_t period = 1,
                                          std::uint32_t phase = 0) {
    Corona::Kernel::SystemsThreadNode node;
    node.system = &system;
    node.after = std::move(after);
    node.period_ticks = period;
    node.phase = phase;
    return node;
}

void test_prepare_rejects_unknown_dependency() {
    std::vector<std::string> journal;
    FakeSystem a{"A", &journal};
    Corona::Kernel::SystemsThread thread{{.nodes = {node_of(a, {"Nope"})}}};

    std::string error;
    CHECK(!thread.prepare(&error));
    CHECK_MSG(error.find("unknown system") != std::string::npos, error.c_str());
}

void test_prepare_rejects_cycle() {
    std::vector<std::string> journal;
    FakeSystem a{"A", &journal};
    FakeSystem b{"B", &journal};
    // A 依赖 B、B 依赖 A：两条边都是后补的（add_node 时不带依赖），
    // 因此"节点只能依赖已存在节点"的结构性限制不会替我们挡住这个环。
    Corona::Kernel::SystemsThread thread{
        {.nodes = {node_of(a, {"B"}), node_of(b, {"A"})}}};

    std::string error;
    CHECK(!thread.prepare(&error));
    CHECK_MSG(error.find("cycle") != std::string::npos, error.c_str());
}

void test_run_once_follows_dependency_order() {
    std::vector<std::string> journal;
    FakeSystem display{"Display", &journal};
    FakeSystem optics{"Optics", &journal};
    FakeSystem geometry{"Geometry", &journal};
    FakeSystem mechanics{"Mechanics", &journal};

    // 故意倒序登记：执行序必须由依赖决定，而不是登记顺序
    Corona::Kernel::SystemsThread thread{{.nodes = {
        node_of(display, {"Optics"}),
        node_of(optics, {"Geometry"}),
        node_of(geometry, {"Mechanics"}),
        node_of(mechanics, {}),
    }}};

    CHECK_MSG(thread.prepare(), "prepare");
    const auto report = thread.run_once(0.016);
    CHECK(report.completed);
    CHECK(report.executed == 4);

    const std::vector<std::string> expected{"Mechanics", "Geometry", "Optics", "Display"};
    CHECK(journal == expected);
    CHECK(thread.ticks() == 1);
}

void test_feed_frame_time_is_called_before_update() {
    std::vector<std::string> journal;
    FakeSystem only{"Only", &journal};
    Corona::Kernel::SystemsThread thread{{.nodes = {node_of(only)}}};
    CHECK(thread.prepare());

    thread.run_once(0.008);
    CHECK(only.feeds == 1);
    CHECK(only.updates == 1);
    CHECK(only.last_frame == 0);
    CHECK_MSG(only.last_dt > 0.0079f && only.last_dt < 0.0081f, "dt should be passed through");

    thread.run_once(0.008);
    CHECK(only.last_frame == 1);
}

void test_rate_gating_skips_only_itself() {
    std::vector<std::string> journal;
    FakeSystem fast{"Fast", &journal};
    FakeSystem slow{"Slow", &journal};
    FakeSystem tail{"Tail", &journal};

    // Slow 每 2 tick 跑一次；Tail 依赖 Slow，但 Slow 被门控跳过时 Tail 仍要跑
    // （M-beta 语义：慢链路按自己的节拍跑，快链路不因上游本 tick 没跑而停下）
    Corona::Kernel::SystemsThread thread{{.nodes = {
        node_of(fast, {}),
        node_of(slow, {}, 2, 0),
        node_of(tail, {"Slow"}),
    }}};
    CHECK(thread.prepare());

    for (int i = 0; i < 6; ++i) {
        thread.run_once(0.016);
    }

    CHECK(fast.updates == 6);
    CHECK(slow.updates == 3);
    CHECK(tail.updates == 6);

    const auto stats = thread.stats();
    CHECK(stats.nodes.size() == 3);
    bool found = false;
    for (const auto& node : stats.nodes) {
        if (node.name == "Slow") {
            found = true;
            CHECK(node.stats.invocations == 3);
            CHECK(node.stats.gated_out == 3);
        }
    }
    CHECK(found);
}

void test_exception_isolated_to_tick() {
    std::vector<std::string> journal;
    FakeSystem ok{"Ok", &journal};
    FakeSystem bad{"Bad", &journal, true};
    FakeSystem after{"After", nullptr};

    Corona::Kernel::SystemsThread thread{{.nodes = {
        node_of(ok),
        node_of(bad, {"Ok"}),
        node_of(after, {"Bad"}),
    }}};
    CHECK(thread.prepare());

    const auto report = thread.run_once(0.016);
    CHECK(!report.completed);
    CHECK(!report.error.empty());
    CHECK(thread.failures() == 1);
    CHECK(ok.updates == 1);
    // 异常发生在 Bad：依赖它的 After 本 tick 不再执行
    CHECK(after.updates == 0);
    CHECK_MSG(thread.last_error().find("fake update failure") != std::string::npos,
              thread.last_error().c_str());

    // 执行器仍然可用：再跑一 tick，依然只是这一个分支失败
    const auto again = thread.run_once(0.016);
    CHECK(!again.completed);
    CHECK(thread.failures() == 2);
    CHECK(ok.updates == 2);
}

void test_thread_lifecycle_and_pause() {
    std::vector<std::string> journal;
    FakeSystem a{"A", &journal};
    FakeSystem b{"B", &journal};
    Corona::Kernel::SystemsThread thread{{.nodes = {node_of(a), node_of(b, {"A"})}, .tick_rate = 200}};
    CHECK(thread.prepare());

    CHECK(thread.start());
    CHECK(thread.is_running());

    // 等到至少 3 个 tick，避免固定 sleep 带来的抖动
    const auto deadline = Clock::now() + std::chrono::seconds{2};
    while (thread.ticks() < 3 && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    CHECK_MSG(thread.ticks() >= 3, "thread should tick at 200Hz");
    CHECK(a.updates == b.updates);  // 每 tick 两者都跑

    thread.pause();
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    const auto paused_ticks = thread.ticks();
    std::this_thread::sleep_for(std::chrono::milliseconds{60});
    CHECK_MSG(thread.ticks() == paused_ticks, "paused thread must not tick");

    thread.resume();
    const auto resume_deadline = Clock::now() + std::chrono::seconds{2};
    while (thread.ticks() <= paused_ticks && Clock::now() < resume_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    CHECK_MSG(thread.ticks() > paused_ticks, "resume should restart ticking");

    thread.stop();
    CHECK(!thread.is_running());

    const std::string dump = thread.dump();
    CHECK_MSG(dump.find("A") != std::string::npos, dump.c_str());
    CHECK_MSG(dump.find("tick_rate=200Hz") != std::string::npos, dump.c_str());
}

void test_start_requires_prepare_and_rate() {
    std::vector<std::string> journal;
    FakeSystem a{"A", &journal};

    {
        // tick_rate = 0：不自动驱动，start() 返回 false，但仍可 run_once()
        Corona::Kernel::SystemsThread manual{{.nodes = {node_of(a)}, .tick_rate = 0}};
        CHECK(!manual.start());
        CHECK(!manual.is_running());
        manual.run_once(0.016);
        CHECK(a.updates == 1);
    }
}

}  // namespace

int main() {
    std::printf("== SystemsThread tests ==\n");
    test_prepare_rejects_unknown_dependency();
    test_prepare_rejects_cycle();
    test_run_once_follows_dependency_order();
    test_feed_frame_time_is_called_before_update();
    test_rate_gating_skips_only_itself();
    test_exception_isolated_to_tick();
    test_thread_lifecycle_and_pause();
    test_start_requires_prepare_and_rate();

    if (g_failures == 0) {
        std::printf("OK: %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("FAILED: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
