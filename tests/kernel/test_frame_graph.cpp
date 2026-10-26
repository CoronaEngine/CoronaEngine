// FrameGraph 的行为契约测试。
//
// 覆盖点：
//  1. 稳定拓扑序（平局按插入序）
//  2. 悬空依赖 / 自依赖 / 环的报错
//  3. 未编译时 run_tick 的安全返回
//  4. 速率门控：只跳过自己，不跳过下游（M-β 语义）
//  5. enabled：关闭子树，不影响无关分支
//  6. 节点抛异常 => 报告 failed 且中止本 tick，但图仍可继续使用
//  7. 边界钩子按执行序调用
//  8. 统计累计与清零、dump 内容

#include "corona/kernel/system/frame_graph.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* expression, int line, const std::string& detail = {}) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAIL(line " << line << "): " << expression;
    if (!detail.empty()) std::cerr << " [" << detail << ']';
    std::cerr << '\n';
}

#define CHECK(expr) check((expr), #expr, __LINE__)
#define CHECK_MSG(expr, detail) check((expr), #expr, __LINE__, (detail))

using Corona::Kernel::FrameGraph;
using Corona::Kernel::FrameNodeDesc;
using Corona::Kernel::FrameNodeId;
using Corona::Kernel::FrameNodeStatus;
using Corona::Kernel::FrameTick;

FrameNodeDesc node(std::string name,
                   std::vector<FrameNodeId> deps = {},
                   std::function<FrameNodeStatus(const FrameTick&)> body = {},
                   std::uint32_t period = 1,
                   std::uint32_t phase = 0) {
    FrameNodeDesc desc;
    desc.name = std::move(name);
    desc.dependencies = std::move(deps);
    desc.period_ticks = period;
    desc.phase = phase;
    desc.body = std::move(body);
    return desc;
}

std::vector<std::string> order_names(const FrameGraph& graph) {
    std::vector<std::string> names;
    for (const auto id : graph.execution_order()) {
        names.emplace_back(graph.node_name(id));
    }
    return names;
}

void test_stable_topological_order() {
    FrameGraph graph;
    // Add the dependent first on purpose: compile() must not depend on insertion order
    // for correctness, but ties must break by insertion index for determinism.
    const auto step = graph.add_node(node("Mechanics.step"));
    const auto skin = graph.add_node(node("Mechanics.skin"));
    const auto upload = graph.add_node(node("Geometry.upload", {skin, step}));
    const auto optics = graph.add_node(node("Optics.render", {upload}));
    const auto audio = graph.add_node(node("Acoustics.update"));
    const auto present = graph.add_node(node("Display.present", {optics}));

    std::string error;
    CHECK_MSG(graph.compile(&error), error);
    CHECK(graph.compiled());
    CHECK(graph.node_count() == 6);

    const auto names = order_names(graph);
    CHECK(names.size() == 6);
    const auto position = [&names](const std::string& name) {
        return std::find(names.begin(), names.end(), name) - names.begin();
    };
    CHECK(position("Mechanics.step") < position("Geometry.upload"));
    CHECK(position("Mechanics.skin") < position("Geometry.upload"));
    CHECK(position("Geometry.upload") < position("Optics.render"));
    CHECK(position("Optics.render") < position("Display.present"));

    // Both roots are ready at tick 0, so insertion order decides: step (0) before skin (1).
    // Acoustics (4) has no dependency either but a higher insertion index, so it comes last
    // among the roots.
    CHECK(position("Mechanics.step") < position("Mechanics.skin"));
    CHECK(position("Mechanics.skin") < position("Acoustics.update"));

    // Compiling twice is idempotent.
    CHECK(graph.compile(nullptr));
    CHECK(order_names(graph) == names);

    // find_node / node_name round trip.
    CHECK(graph.find_node("Optics.render") == optics);
    CHECK(graph.find_node("missing") == Corona::Kernel::kInvalidFrameNodeId);
    CHECK(graph.node_name(skin) == "Mechanics.skin");
    CHECK(graph.node_name(Corona::Kernel::kInvalidFrameNodeId).empty());
}

void test_invalid_graphs() {
    {
        // Dangling dependency id.
        FrameGraph graph;
        graph.add_node(node("A", {Corona::Kernel::kInvalidFrameNodeId}));
        std::string error;
        CHECK(!graph.compile(&error));
        CHECK_MSG(error.find("unknown node id") != std::string::npos, error);
        CHECK(!graph.compiled());
    }
    {
        // Self dependency, now expressible through set_dependencies().
        FrameGraph graph;
        const auto only = graph.add_node(node("Only"));
        CHECK(graph.set_dependencies(only, {only}));
        std::string error;
        CHECK(!graph.compile(&error));
        CHECK_MSG(error.find("depends on itself") != std::string::npos, error);
    }
    {
        // Duplicate / empty names and bad rate parameters are programmer errors.
        FrameGraph graph;
        graph.add_node(node("Dup"));
        bool threw = false;
        try {
            graph.add_node(node("Dup"));
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);

        threw = false;
        try {
            graph.add_node(node(""));
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);

        threw = false;
        try {
            graph.add_node(node("BadPhase", {}, {}, 4, 4));
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);

        CHECK(!graph.set_dependencies(Corona::Kernel::kInvalidFrameNodeId, {}));
    }
}

void test_cycle_detection() {
    FrameGraph graph;
    const auto a = graph.add_node(node("A"));
    const auto b = graph.add_node(node("B", {a}));
    const auto c = graph.add_node(node("C", {b}));

    // Close the loop: A must run after C.
    CHECK(graph.set_dependencies(a, {c}));

    std::string error;
    CHECK(!graph.compile(&error));
    CHECK(!graph.compiled());
    CHECK_MSG(error.find("cycle") != std::string::npos, error);
    CHECK_MSG(error.find("A") != std::string::npos, error);

    // run_tick() must refuse to execute a graph without a valid order.
    const auto report = graph.run_tick(FrameTick{0, 0.016});
    CHECK(!report.completed);

    // Breaking the cycle lets the same graph compile and run again.
    CHECK(graph.set_dependencies(a, {}));
    CHECK_MSG(graph.compile(&error), error);
    CHECK(graph.compiled());
    CHECK(order_names(graph) == std::vector<std::string>({"A", "B", "C"}));
}

void test_not_compiled() {
    FrameGraph graph;
    auto ran = false;
    graph.add_node(node("A", {}, [&ran](const FrameTick&) {
        ran = true;
        return FrameNodeStatus::ran;
    }));
    const auto report = graph.run_tick(FrameTick{0, 0.016});
    CHECK(!report.completed);
    CHECK_MSG(report.error.find("not compiled") != std::string::npos, report.error);
    CHECK(!ran);
}

void test_run_and_rate_gating() {
    FrameGraph graph;
    int fast = 0;
    int slow = 0;
    int after_slow = 0;

    const auto fast_node = graph.add_node(node("fast", {}, [&fast](const FrameTick&) {
        ++fast;
        return FrameNodeStatus::ran;
    }));
    const auto slow_node = graph.add_node(node("slow", {}, [&slow](const FrameTick&) {
        ++slow;
        return FrameNodeStatus::ran;
    }, 4, 0));  // every 4th tick
    const auto after = graph.add_node(node("after_slow", {slow_node}, [&after_slow](const FrameTick&) {
        ++after_slow;
        return FrameNodeStatus::ran;
    }));
    (void)fast_node;
    (void)after;

    std::string error;
    CHECK_MSG(graph.compile(&error), error);

    std::uint64_t gated_total = 0;
    std::uint64_t executed_total = 0;
    for (std::uint64_t tick = 0; tick < 8; ++tick) {
        const auto report = graph.run_tick(FrameTick{tick, 1.0 / 120.0});
        CHECK_MSG(report.completed, report.error);
        gated_total += report.gated;
        executed_total += report.executed;
    }

    CHECK(slow == 2);              // ticks 0 and 4
    CHECK(fast == 8);
    CHECK(after_slow == 8);        // gated upstream does not gate downstream (M-beta)
    CHECK(gated_total == 6);       // 8 - 2
    CHECK(executed_total == 8 + 2 + 8);
    CHECK(graph.stats(slow_node).gated_out == 6);
    CHECK(graph.stats(slow_node).invocations == 2);
    CHECK(graph.stats(after).invocations == 8);

    // Phase shifts the cadence without changing the rate.
    CHECK(graph.set_rate(slow_node, 4, 2));
    slow = 0;
    for (std::uint64_t tick = 0; tick < 8; ++tick) {
        graph.run_tick(FrameTick{tick, 1.0 / 120.0});
    }
    CHECK(slow == 2);  // ticks 2 and 6
    CHECK(!graph.set_rate(slow_node, 4, 4));  // phase must be < period
    CHECK(!graph.set_rate(Corona::Kernel::kInvalidFrameNodeId, 2, 0));
}

void test_disable_subtree() {
    FrameGraph graph;
    int root = 0;
    int child = 0;
    int grandchild = 0;
    int unrelated = 0;

    const auto root_id = graph.add_node(node("root", {}, [&root](const FrameTick&) {
        ++root;
        return FrameNodeStatus::ran;
    }));
    const auto child_id = graph.add_node(node("child", {root_id}, [&child](const FrameTick&) {
        ++child;
        return FrameNodeStatus::ran;
    }));
    const auto grandchild_id = graph.add_node(node("grandchild", {child_id}, [&grandchild](const FrameTick&) {
        ++grandchild;
        return FrameNodeStatus::ran;
    }));
    graph.add_node(node("unrelated", {}, [&unrelated](const FrameTick&) {
        ++unrelated;
        return FrameNodeStatus::ran;
    }));

    CHECK(graph.compile(nullptr));
    graph.run_tick(FrameTick{0, 0.016});
    CHECK(root == 1 && child == 1 && grandchild == 1 && unrelated == 1);

    CHECK(graph.set_enabled(child_id, false));
    const auto report = graph.run_tick(FrameTick{1, 0.016});
    CHECK(report.completed);
    CHECK(root == 2);
    CHECK(child == 1);       // disabled
    CHECK(grandchild == 1);  // disabled through its ancestor
    CHECK(unrelated == 2);
    CHECK(report.disabled == 2);
    CHECK(graph.stats(grandchild_id).disabled_out == 1);

    graph.set_enabled(child_id, true);
    graph.run_tick(FrameTick{2, 0.016});
    CHECK(child == 2 && grandchild == 2);
}

void test_exception_aborts_tick_only() {
    FrameGraph graph;
    int before = 0;
    int after = 0;

    graph.add_node(node("before", {}, [&before](const FrameTick&) {
        ++before;
        return FrameNodeStatus::ran;
    }));
    const auto boom = graph.add_node(node("boom", {}, [](const FrameTick&) -> FrameNodeStatus {
        throw std::runtime_error("boom");
    }));
    graph.add_node(node("after", {}, [&after](const FrameTick&) {
        ++after;
        return FrameNodeStatus::ran;
    }));

    CHECK(graph.compile(nullptr));

    const auto first = graph.run_tick(FrameTick{0, 0.016});
    CHECK(!first.completed);
    CHECK(first.failed_node == boom);
    CHECK_MSG(first.error == "boom", first.error);
    CHECK(before == 1);
    CHECK(after == 0);  // aborted before reaching it

    const auto second = graph.run_tick(FrameTick{1, 0.016});
    CHECK(!second.completed);
    CHECK(before == 2);  // the graph keeps working, it is not poisoned
}

void test_boundary_hook() {
    FrameGraph graph;
    std::vector<std::string> visited;

    const auto a = graph.add_node(node("A"));
    const auto b = graph.add_node(node("B"));
    (void)a;
    (void)b;
    CHECK(graph.compile(nullptr));

    graph.set_boundary_hook([&graph, &visited](FrameNodeId id, const FrameTick&) {
        visited.emplace_back(graph.node_name(id));
    });

    graph.run_tick(FrameTick{0, 0.016});
    CHECK(visited.size() == 2);
    CHECK(visited.front() == "A");
    CHECK(visited.back() == "B");

    // The hook is not called for gated-out nodes.
    graph.set_rate(b, 2, 1);
    visited.clear();
    graph.run_tick(FrameTick{1, 0.016});  // tick 1 => B matches phase 1, so both run
    CHECK(visited.size() == 2);
    visited.clear();
    graph.run_tick(FrameTick{2, 0.016});  // tick 2 => B is gated
    CHECK(visited.size() == 1);
    CHECK(visited.front() == "A");
}

void test_idle_and_stats() {
    FrameGraph graph;
    const auto idle_node = graph.add_node(node("idle", {}, [](const FrameTick&) {
        return FrameNodeStatus::idle;
    }));
    const auto busy = graph.add_node(node("busy", {}, [](const FrameTick&) {
        volatile int spin = 0;
        for (int i = 0; i < 2000; ++i) spin = spin + i;
        (void)spin;
        return FrameNodeStatus::ran;
    }));

    CHECK(graph.compile(nullptr));
    for (int i = 0; i < 3; ++i) graph.run_tick(FrameTick{static_cast<std::uint64_t>(i), 0.016});

    const auto idle_stats = graph.stats(idle_node);
    CHECK(idle_stats.invocations == 3);
    CHECK(idle_stats.idle_invocations == 3);
    const auto busy_stats = graph.stats(busy);
    CHECK(busy_stats.invocations == 3);
    CHECK(busy_stats.idle_invocations == 0);
    CHECK(busy_stats.max_ms >= 0.0);

    const auto dump = graph.dump();
    CHECK_MSG(dump.find("idle") != std::string::npos, dump);
    CHECK_MSG(dump.find("busy") != std::string::npos, dump);
    CHECK_MSG(dump.find("order:") != std::string::npos, dump);

    graph.reset_stats();
    CHECK(graph.stats(idle_node).invocations == 0);
    CHECK(graph.stats(busy).max_ms == 0.0);
}

}  // namespace

int main() {
    test_stable_topological_order();
    test_invalid_graphs();
    test_cycle_detection();
    test_not_compiled();
    test_run_and_rate_gating();
    test_disable_subtree();
    test_exception_aborts_tick_only();
    test_boundary_hook();
    test_idle_and_stats();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "FrameGraphTests: all checks passed\n";
    return 0;
}
