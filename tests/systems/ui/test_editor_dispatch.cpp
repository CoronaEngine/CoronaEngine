// 编辑器状态派发的契约（`src/systems/ui/editor_api/editor_dispatch.h`）。
//
// 被钉住的规则有三条，它们都是"曾经靠运气"的并发假设：
//   1. handler 只在 owner（CEF UI）线程执行；别的线程必须 marshal 过去，而且等待是
//      有界的 —— 不能把 Python 线程永久挂在 UI 线程上。
//   2. owner 线程阻塞等 Python 时必须打标，让 Python 消费线程就地执行（否则互等）。
//   3. 就地执行与 owner 恢复之间不能有窗口：owner 退出等待前必须等在跑的就地执行结束，
//      并且外来线程在拿到锁之后要复查标记。这里用一个慢速就地执行来falsify第 3 条 ——
//      去掉 drain 之后本测试会看到 foreign_done == false。
//
// 超时路径被钉住两次：一次是"还没开始执行就撤销"（迟到的任务必须变成空操作，否则调用方
// 报告没执行、重试，状态就被改两次），一次是"已经开始执行就必须等到真实结果"（否则同样
// 会被重试成重复应用）。
#include "editor_api/editor_dispatch.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// 不用 assert：本仓的测试按 RelWithDebInfo 构建，`/DNDEBUG` 会让 assert 变成空语句，
// 于是"通过的测试"什么都没检查。CHECK 在任何配置下都会失败退出（非零退出码 → ctest 红）。
#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            std::fprintf(stderr,                                                      \
                         "CHECK failed: %s (%s:%d)\n",                                \
                         #condition,                                                  \
                         __FILE__,                                                    \
                         __LINE__);                                                   \
            std::abort();                                                             \
        }                                                                             \
    } while (false)

namespace {

using namespace Corona::Systems::UI::Detail;
using namespace std::chrono_literals;

void test_route_decisions() {
    // 在 owner 线程上：就地执行（该线程天然串行，不需要锁）。
    CHECK(decide_editor_dispatch_route(true, false, true) == EditorDispatchRoute::Inline);
    CHECK(decide_editor_dispatch_route(true, true, true) == EditorDispatchRoute::Inline);

    // 别的线程 + owner 正在等 Python：就地执行，避免互等环路（即便还没见过 owner 线程，
    // 交接协议的语义也成立）。
    CHECK(decide_editor_dispatch_route(false, true, true) == EditorDispatchRoute::InlineBusy);
    CHECK(decide_editor_dispatch_route(false, true, false) == EditorDispatchRoute::InlineBusy);

    // 别的线程、owner 健康：marshal。
    CHECK(decide_editor_dispatch_route(false, false, true) == EditorDispatchRoute::Marshal);

    // 还没见过 owner 线程（CEF 未起 / headless）：没有可投递的线程，就地执行。
    CHECK(decide_editor_dispatch_route(false, false, false) == EditorDispatchRoute::InlineNoOwner);
}

void test_inline_runs_on_caller() {
    clear_editor_dispatch_hooks();
    bool ran = false;
    const auto report = run_editor_api_dispatch(
        [&ran] { ran = true; }, EditorDispatchRoute::Inline, 100ms);
    CHECK(ran);
    CHECK(report.run == EditorOwnerRun::Ran);
    // Inline 的前提就是"已经在 owner 线程上"，不算"就地绕过 owner"。
    CHECK(!report.ran_inline_on_caller);
}

void test_marshal_posts_to_owner() {
    const auto caller_id = std::this_thread::get_id();
    std::thread::id ran_on;
    EditorDispatchHooks hooks;
    hooks.is_owner_thread = [caller_id] { return std::this_thread::get_id() == caller_id; };
    hooks.post_to_owner = [&ran_on](std::function<void()> task) {
        ran_on = std::this_thread::get_id();  // 模拟 CEF：任务由 owner 线程的消息循环执行
        task();
        return true;
    };
    set_editor_dispatch_hooks(hooks);

    bool ran = false;
    const auto report = run_editor_api_dispatch(
        [&ran] { ran = true; }, EditorDispatchRoute::Marshal, 100ms);
    CHECK(ran);
    CHECK(report.run == EditorOwnerRun::Ran);
    CHECK(!report.ran_inline_on_caller);
    clear_editor_dispatch_hooks();
}

void test_marshal_timeout_cancels_the_late_task() {
    // post 成功但 owner 线程"卡住"：任务被压在队列里，稍后才执行。
    std::deque<std::function<void()>> queued;
    EditorDispatchHooks hooks;
    hooks.is_owner_thread = [] { return false; };
    hooks.post_to_owner = [&queued](std::function<void()> task) {
        queued.push_back(std::move(task));
        return true;
    };
    set_editor_dispatch_hooks(hooks);

    int writes = 0;
    const auto started = std::chrono::steady_clock::now();
    const auto report = run_editor_api_dispatch([&writes] { ++writes; },
                                                EditorDispatchRoute::Marshal,
                                                20ms);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK(report.run == EditorOwnerRun::Timeout);
    CHECK(writes == 0);                 // 调用方拿到的是"没执行"
    CHECK(elapsed >= 20ms);             // 真的等到了时限，而不是立刻返回
    CHECK(elapsed < 5s);                // 但等待是有界的

    // 超时之后那个任务才轮到执行：它必须什么都不做。否则调用方已经报告"没执行"、转头
    // 重试，状态就会被改两次（协作创建 actor 会变成两个）。这里同时验证它不会写到已经
    // 退栈的调用方栈帧上。
    CHECK(queued.size() == 1);
    queued.front()();
    CHECK(writes == 0);
    clear_editor_dispatch_hooks();
}

void test_marshal_timeout_after_start_returns_real_result() {
    // owner 线程已经开始执行，只是慢：此时绝不能报告"没执行"（那会让调用方重试并重复
    // 应用），必须等它跑完把真实结果带回来。
    std::thread worker;
    EditorDispatchHooks hooks;
    hooks.is_owner_thread = [] { return false; };
    hooks.post_to_owner = [&worker](std::function<void()> task) {
        worker = std::thread(std::move(task));
        return true;
    };
    set_editor_dispatch_hooks(hooks);

    EditorDispatchReport report;
    const auto result = call_editor_dispatch(
        [] {
            std::this_thread::sleep_for(120ms);
            return 42;
        },
        EditorDispatchRoute::Marshal,
        20ms,
        &report);
    if (worker.joinable()) worker.join();

    CHECK(result.has_value());
    CHECK(*result == 42);
    CHECK(report.run == EditorOwnerRun::Ran);
    clear_editor_dispatch_hooks();
}

void test_call_returns_nullopt_only_when_body_never_runs() {
    // 撤销语义的正面表述：返回空 ⇒ body 绝不会执行。
    std::deque<std::function<void()>> queued;
    EditorDispatchHooks hooks;
    hooks.is_owner_thread = [] { return false; };
    hooks.post_to_owner = [&queued](std::function<void()> task) {
        queued.push_back(std::move(task));
        return true;
    };
    set_editor_dispatch_hooks(hooks);

    bool ran = false;
    const auto result = call_editor_dispatch(
        [&ran] {
            ran = true;
            return 7;
        },
        EditorDispatchRoute::Marshal,
        10ms);
    CHECK(!result.has_value());
    CHECK(!ran);
    CHECK(queued.size() == 1);
    queued.front()();  // 迟到的任务：什么都不做
    CHECK(!ran);
    clear_editor_dispatch_hooks();
}

void test_post_failure_is_reported() {
    EditorDispatchHooks no_post;
    no_post.is_owner_thread = [] { return false; };
    set_editor_dispatch_hooks(no_post);
    const auto no_hook = run_editor_api_dispatch([] {}, EditorDispatchRoute::Marshal, 20ms);
    CHECK(no_hook.run == EditorOwnerRun::PostFailed);

    EditorDispatchHooks refusing;
    refusing.is_owner_thread = [] { return false; };
    refusing.post_to_owner = [](std::function<void()>) { return false; };
    set_editor_dispatch_hooks(refusing);
    const auto refused = run_editor_api_dispatch([] {}, EditorDispatchRoute::Marshal, 20ms);
    CHECK(refused.run == EditorOwnerRun::PostFailed);
    clear_editor_dispatch_hooks();
}

void test_body_exception_reaches_caller() {
    EditorDispatchHooks hooks;
    hooks.is_owner_thread = [] { return false; };
    hooks.post_to_owner = [](std::function<void()> task) {
        task();
        return true;
    };
    set_editor_dispatch_hooks(hooks);

    bool thrown = false;
    try {
        const auto report = run_editor_api_dispatch(
            [] { throw std::runtime_error("handler blew up"); },
            EditorDispatchRoute::Marshal,
            100ms);
        (void)report;
    } catch (const std::runtime_error& error) {
        thrown = std::string(error.what()) == "handler blew up";
    }
    CHECK(thrown);
    clear_editor_dispatch_hooks();
}

void test_inline_no_owner_serializes_and_runs_on_caller() {
    clear_editor_dispatch_hooks();
    reset_editor_owner_thread_seen_for_test();
    bool ran = false;
    const auto report = run_editor_api_dispatch(
        [&ran] { ran = true; }, EditorDispatchRoute::InlineNoOwner, 100ms);
    CHECK(ran);
    CHECK(report.ran_inline_on_caller);
}

void test_inline_no_owner_falls_back_when_owner_appears() {
    // 见过 owner 线程之后再拿到"陈旧的" InlineNoOwner 路由：必须改投 owner 线程，
    // 绝不能就在调用线程上跑（那会和 owner 并发写状态）。
    EditorDispatchHooks owner_hooks;
    owner_hooks.is_owner_thread = [] { return true; };
    set_editor_dispatch_hooks(owner_hooks);
    reset_editor_owner_thread_seen_for_test();
    CHECK(on_editor_owner_thread());
    CHECK(editor_owner_thread_seen());

    auto posted = std::make_shared<std::atomic<bool>>(false);
    EditorDispatchHooks normal_hooks;
    normal_hooks.is_owner_thread = [] { return false; };
    normal_hooks.post_to_owner = [posted](std::function<void()> task) {
        posted->store(true, std::memory_order_release);
        task();
        return true;
    };
    set_editor_dispatch_hooks(normal_hooks);

    EditorDispatchReport report;
    const auto result = call_editor_dispatch(
        [] { return 5; }, EditorDispatchRoute::InlineNoOwner, 100ms, &report);
    CHECK(result.has_value());
    CHECK(*result == 5);
    CHECK(posted->load());                 // 走了 owner 线程
    CHECK(!report.ran_inline_on_caller);
    clear_editor_dispatch_hooks();
    reset_editor_owner_thread_seen_for_test();
}

void test_first_owner_dispatch_waits_for_in_flight_inline_run() {
    // "见过 owner"首次置位要和正在就地执行的外来线程握手：置位发生在 editor_state_mutex
    // 之内，所以 owner 必须等那个就地执行跑完才能回到 handler 里。删掉握手之后这里会看到
    // foreign_done == false。
    const auto owner_id = std::this_thread::get_id();
    EditorDispatchHooks hooks;
    hooks.is_owner_thread = [owner_id] { return std::this_thread::get_id() == owner_id; };
    set_editor_dispatch_hooks(hooks);
    reset_editor_owner_thread_seen_for_test();

    std::atomic<bool> foreign_started{false};
    std::atomic<bool> allow_finish{false};
    std::atomic<bool> foreign_done{false};

    std::thread foreign([&] {
        const auto report = run_editor_api_dispatch(
            [&] {
                foreign_started.store(true, std::memory_order_release);
                while (!allow_finish.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(1ms);
                }
                foreign_done.store(true, std::memory_order_release);
            },
            EditorDispatchRoute::InlineNoOwner,
            2s);
        CHECK(report.ran_inline_on_caller);
    });
    std::thread releaser([&] {
        while (!foreign_started.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
        std::this_thread::sleep_for(150ms);
        allow_finish.store(true, std::memory_order_release);
    });

    while (!foreign_started.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(on_editor_owner_thread());  // 要等到那个就地执行结束才会返回
    const bool owner_waited = foreign_done.load(std::memory_order_acquire);
    CHECK(owner_waited);
    CHECK(editor_owner_thread_seen());

    releaser.join();
    foreign.join();
    clear_editor_dispatch_hooks();
}

void test_inline_busy_runs_inline_while_owner_waits_for_python() {
    set_editor_waiting_for_python(true);
    bool ran = false;
    const auto report = run_editor_api_dispatch(
        [&ran] { ran = true; }, EditorDispatchRoute::InlineBusy, 100ms);
    set_editor_waiting_for_python(false);
    CHECK(ran);
    CHECK(report.ran_inline_on_caller);
    CHECK(report.run == EditorOwnerRun::Ran);
}

void test_inline_busy_rechecks_marker_after_locking() {
    // 路由选定之后 owner 就恢复了：此时必须改走 marshal，绝不能再就地执行，
    // 否则会和 owner 线程上的 handler 并发写同一份状态。
    bool posted = false;
    EditorDispatchHooks hooks;
    hooks.is_owner_thread = [] { return false; };
    hooks.post_to_owner = [&posted](std::function<void()> task) {
        posted = true;
        task();
        return true;
    };
    set_editor_dispatch_hooks(hooks);

    set_editor_waiting_for_python(true);
    const auto route = decide_editor_dispatch_route(false, editor_thread_waiting_for_python(), true);
    CHECK(route == EditorDispatchRoute::InlineBusy);
    set_editor_waiting_for_python(false);  // owner 恢复（模拟交接窗口）

    bool ran_inline = false;
    const auto report = run_editor_api_dispatch(
        [&ran_inline] { ran_inline = true; }, route, 100ms);
    CHECK(ran_inline);
    CHECK(posted);                      // 走了 owner 线程
    CHECK(!report.ran_inline_on_caller);
    clear_editor_dispatch_hooks();
}

void test_owner_waits_for_in_flight_inline_handler() {
    const auto owner_id = std::this_thread::get_id();
    EditorDispatchHooks hooks;
    hooks.is_owner_thread = [owner_id] { return std::this_thread::get_id() == owner_id; };
    set_editor_dispatch_hooks(hooks);

    std::atomic<bool> foreign_started{false};
    std::atomic<bool> allow_foreign_to_finish{false};
    std::atomic<bool> foreign_done{false};

    std::thread foreign;
    std::thread releaser;
    {
        // owner 线程进入"等 Python"状态（先打标，外来线程才允许就地执行）。
        EditorWaitingForPythonScope waiting_for_python;
        CHECK(waiting_for_python.engaged());

        foreign = std::thread([&] {
            const auto report = run_editor_api_dispatch(
                [&] {
                    foreign_started.store(true, std::memory_order_release);
                    while (!allow_foreign_to_finish.load(std::memory_order_acquire)) {
                        std::this_thread::sleep_for(1ms);
                    }
                    foreign_done.store(true, std::memory_order_release);
                },
                EditorDispatchRoute::InlineBusy,
                2s);
            CHECK(report.ran_inline_on_caller);
            CHECK(report.run == EditorOwnerRun::Ran);
        });
        // 让就地执行"跑得慢"：它自己不会结束，得等这个计时线程放行。
        releaser = std::thread([&] {
            while (!foreign_started.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(1ms);
            }
            std::this_thread::sleep_for(150ms);
            allow_foreign_to_finish.store(true, std::memory_order_release);
        });

        while (!foreign_started.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
        // 就地执行已经在 handler 里面了。现在退出作用域 = owner 恢复：恢复必须等它结束
        // （否则 owner 会和它并发写状态）。drain 被删掉的话这里立刻返回，
        // foreign_done 仍是 false。
    }
    const bool drained_before_owner_resumed = foreign_done.load(std::memory_order_acquire);

    releaser.join();
    foreign.join();

    CHECK(drained_before_owner_resumed);
    CHECK(!editor_thread_waiting_for_python());

    // owner 恢复之后，外来线程只能 marshal：不能再有并发就地执行。
    bool posted = false;
    EditorDispatchHooks posting;
    posting.is_owner_thread = [owner_id] { return std::this_thread::get_id() == owner_id; };
    posting.post_to_owner = [&posted](std::function<void()> task) {
        posted = true;
        task();
        return true;
    };
    set_editor_dispatch_hooks(posting);
    bool ran_inline = false;
    const auto report = run_editor_api_dispatch(
        [&ran_inline] { ran_inline = true; },
        EditorDispatchRoute::InlineBusy,  // 路由陈旧：标记已经清掉了
        100ms);
    CHECK(ran_inline);
    CHECK(posted);
    CHECK(!report.ran_inline_on_caller);
    clear_editor_dispatch_hooks();
}

void test_scope_is_ignored_off_the_owner_thread() {
    const auto owner_id = std::this_thread::get_id();
    EditorDispatchHooks hooks;
    hooks.is_owner_thread = [owner_id] { return std::this_thread::get_id() == owner_id; };
    set_editor_dispatch_hooks(hooks);

    // 非 owner 线程等 Python 不构成环路（owner 还在正常跑 handler），所以不能发"就地
    // 执行"的许可，否则外来线程会和 owner 并发。
    std::thread foreign([] {
        EditorWaitingForPythonScope waiting_for_python;
        CHECK(!waiting_for_python.engaged());
        CHECK(!editor_thread_waiting_for_python());
    });
    foreign.join();
    clear_editor_dispatch_hooks();
}

}  // namespace

int main() {
    test_route_decisions();
    test_inline_runs_on_caller();
    test_marshal_posts_to_owner();
    test_marshal_timeout_cancels_the_late_task();
    test_marshal_timeout_after_start_returns_real_result();
    test_call_returns_nullopt_only_when_body_never_runs();
    test_post_failure_is_reported();
    test_body_exception_reaches_caller();
    test_inline_no_owner_serializes_and_runs_on_caller();
    test_inline_no_owner_falls_back_when_owner_appears();
    test_first_owner_dispatch_waits_for_in_flight_inline_run();
    test_inline_busy_runs_inline_while_owner_waits_for_python();
    test_inline_busy_rechecks_marker_after_locking();
    test_owner_waits_for_in_flight_inline_handler();
    test_scope_is_ignored_off_the_owner_thread();
    std::puts("editor dispatch tests passed");
    return 0;
}
