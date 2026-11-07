/**
 * @file test_command_buffer.cpp
 * @brief CommandBuffer（入站命令队列）契约测试
 *
 * 覆盖：
 * - Fifo：顺序、容量拒绝、出队清空、统计
 * - LatestWins：按 key 合并、按 sequence 出队、满容量挤掉最旧 key
 * - peek：待提交视图（读己之写）与 drain 后失效
 * - erase：槽位销毁时丢弃待处理命令
 * - 多生产者：并发 push 不丢、不重、顺序可复现
 */

#include <corona/kernel/command/command_buffer.h>

#include <cstdio>
#include <cstdint>
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

struct Move {
    std::uintptr_t handle{0};
    int value{0};
};

struct MoveKey {
    static std::uintptr_t get(const Move& move) noexcept { return move.handle; }
};

using FifoMoves = Corona::Kernel::CommandBuffer<Move, MoveKey,
                                                Corona::Kernel::CoalescePolicy::Fifo>;
using LatestMoves = Corona::Kernel::CommandBuffer<Move, MoveKey,
                                                  Corona::Kernel::CoalescePolicy::LatestWins>;

void test_fifo_preserves_order() {
    FifoMoves buffer;
    for (int i = 0; i < 8; ++i) {
        CHECK(buffer.push(Move{static_cast<std::uintptr_t>(i % 3), i}));
    }
    CHECK(buffer.size() == 8);

    const auto drained = buffer.drain();
    CHECK(drained.size() == 8);
    for (int i = 0; i < 8; ++i) {
        CHECK(drained[static_cast<std::size_t>(i)].value == i);
    }
    CHECK(buffer.empty());
}

void test_fifo_rejects_when_full() {
    FifoMoves buffer{4};
    CHECK(buffer.push(Move{1, 0}));
    CHECK(buffer.push(Move{1, 1}));
    CHECK(buffer.push(Move{1, 2}));
    CHECK(buffer.push(Move{1, 3}));
    CHECK(!buffer.push(Move{1, 4}));  // 队列满 → 拒绝
    CHECK(buffer.size() == 4);

    const auto stats = buffer.stats();
    CHECK(stats.pushed == 4);
    CHECK(stats.dropped == 1);

    const auto drained = buffer.drain();
    CHECK(drained.size() == 4);
    CHECK(drained.back().value == 3);  // 被拒绝的那条没有混进来
    CHECK(buffer.stats().drained == 4);
}

void test_latest_wins_coalesces_per_key() {
    LatestMoves buffer;
    buffer.push(Move{7, 1});
    buffer.push(Move{9, 1});
    buffer.push(Move{7, 2});
    buffer.push(Move{7, 3});

    CHECK(buffer.size() == 2);  // 两个 key
    const auto stats = buffer.stats();
    CHECK(stats.pushed == 4);
    CHECK(stats.coalesced == 2);  // 7 被覆盖两次

    const auto drained = buffer.drain();
    CHECK(drained.size() == 2);
    // 按 sequence 排序：7 的最新一条（3）在 9（1）之后
    CHECK(drained[0].handle == 9);
    CHECK(drained[0].value == 1);
    CHECK(drained[1].handle == 7);
    CHECK(drained[1].value == 3);
}

void test_latest_wins_orders_by_sequence() {
    LatestMoves buffer;
    buffer.push(Move{1, 100});
    buffer.push(Move{2, 200});
    buffer.push(Move{1, 101});  // 覆盖 key=1，sequence 更新到末尾

    const auto drained = buffer.drain();
    CHECK(drained.size() == 2);
    CHECK(drained[0].handle == 2);
    CHECK(drained[1].handle == 1);
    CHECK(drained[1].value == 101);
}

void test_latest_wins_evicts_oldest_key_at_capacity() {
    LatestMoves buffer{2};
    buffer.push(Move{1, 1});
    buffer.push(Move{2, 2});
    buffer.push(Move{3, 3});  // 满 → 挤掉最旧的 key=1

    const auto drained = buffer.drain();
    CHECK(drained.size() == 2);
    CHECK(drained[0].handle == 2);
    CHECK(drained[1].handle == 3);
    CHECK(buffer.stats().dropped == 1);
}

void test_peek_is_pending_view() {
    LatestMoves buffer;
    CHECK(!buffer.peek(5).has_value());

    buffer.push(Move{5, 41});
    const auto pending = buffer.peek(5);
    CHECK(pending.has_value());
    CHECK(pending->value == 41);

    buffer.push(Move{5, 42});  // 读己之写：pending 立刻反映最新提交
    CHECK(buffer.peek(5)->value == 42);

    (void)buffer.drain();
    CHECK(!buffer.peek(5).has_value());

    FifoMoves fifo;
    fifo.push(Move{3, 7});
    fifo.push(Move{4, 8});
    CHECK(fifo.peek(4).has_value());
    CHECK(fifo.peek(4)->value == 8);
    CHECK(!fifo.peek(9).has_value());
}

void test_erase_drops_pending_for_key() {
    LatestMoves latest;
    latest.push(Move{1, 1});
    latest.push(Move{2, 2});
    CHECK(latest.erase(1));
    CHECK(latest.size() == 1);
    CHECK(!latest.peek(1).has_value());

    FifoMoves fifo;
    fifo.push(Move{1, 1});
    fifo.push(Move{2, 2});
    fifo.push(Move{1, 3});
    CHECK(fifo.erase(1));
    CHECK(fifo.size() == 1);
    CHECK(fifo.drain()[0].handle == 2);
}

void test_multithreaded_producers_fifo() {
    constexpr int kThreads = 4;
    constexpr int kPerThread = 500;
    FifoMoves buffer{kThreads * kPerThread};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&buffer, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                buffer.push(Move{static_cast<std::uintptr_t>(t), i});
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    const auto drained = buffer.drain();
    CHECK(drained.size() == static_cast<std::size_t>(kThreads * kPerThread));
    CHECK(buffer.stats().dropped == 0);

    // 每个线程内部的相对顺序必须保持（跨线程的绝对顺序不做承诺）
    std::vector<int> next(kThreads, 0);
    bool order_ok = true;
    for (const auto& move : drained) {
        const auto thread = static_cast<std::size_t>(move.handle);
        if (move.value != next[thread]) {
            order_ok = false;
            break;
        }
        ++next[thread];
    }
    CHECK(order_ok);
}

void test_multithreaded_producers_latest_wins() {
    constexpr int kThreads = 4;
    constexpr int kPerThread = 400;
    LatestMoves buffer;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&buffer, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                buffer.push(Move{static_cast<std::uintptr_t>(t), i});
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    CHECK(buffer.size() == static_cast<std::size_t>(kThreads));  // 每个线程一个 key
    const auto drained = buffer.drain();
    CHECK(drained.size() == static_cast<std::size_t>(kThreads));

    // 每个 key 的最终值必须是该线程最后提交的那条
    std::vector<bool> seen(kThreads, false);
    for (const auto& move : drained) {
        const auto thread = static_cast<std::size_t>(move.handle);
        CHECK(move.value == kPerThread - 1);
        seen[thread] = true;
    }
    CHECK(seen[0] && seen[1] && seen[2] && seen[3]);

    const auto stats = buffer.stats();
    CHECK(stats.pushed == static_cast<std::uint64_t>(kThreads * kPerThread));
    CHECK(stats.coalesced == static_cast<std::uint64_t>(kThreads * (kPerThread - 1)));
    CHECK(stats.dropped == 0);
}

}  // namespace

int main() {
    std::printf("== CommandBuffer tests ==\n");
    test_fifo_preserves_order();
    test_fifo_rejects_when_full();
    test_latest_wins_coalesces_per_key();
    test_latest_wins_orders_by_sequence();
    test_latest_wins_evicts_oldest_key_at_capacity();
    test_peek_is_pending_view();
    test_erase_drops_pending_for_key();
    test_multithreaded_producers_fifo();
    test_multithreaded_producers_latest_wins();

    if (g_failures == 0) {
        std::printf("OK: %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("FAILED: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
