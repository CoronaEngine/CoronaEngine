/**
 * @file test_snapshot_channel.cpp
 * @brief SnapshotChannel 与 ThreadAffinity 契约测试
 *
 * 覆盖：
 * - 发布前读为空；发布后读到值与 revision
 * - revision 单调递增
 * - 旧快照在发布新快照后仍然有效（读者持有的副本不被回收）
 * - 并发读：不撕裂、revision 单调、payload 内部自洽
 * - ThreadAffinity：本线程=拥有者、异线程被识别、未绑定时不误报
 */

#include <corona/kernel/command/snapshot_channel.h>
#include <corona/kernel/command/thread_affinity.h>

#include <atomic>
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

/// 快照载荷：a + b 恒等于 1000，用来检测"半更新"（撕裂读）
struct CameraRow {
    std::uintptr_t handle{0};
    int a{0};
    int b{1000};
};

struct CameraSnapshot {
    std::uint64_t tick{0};
    std::vector<CameraRow> rows;
};

void test_read_before_publish() {
    Corona::Kernel::SnapshotChannel<CameraSnapshot> channel;
    CHECK(!channel.published());
    CHECK(channel.read() == nullptr);
    CHECK(channel.revision() == 0);
}

void test_publish_and_revision() {
    Corona::Kernel::SnapshotChannel<CameraSnapshot> channel;

    CameraSnapshot first;
    first.tick = 1;
    first.rows.push_back({7, 100, 900});
    CHECK(channel.publish(first) == 1);
    CHECK(channel.revision() == 1);

    auto read_back = channel.read();
    CHECK(read_back != nullptr);
    CHECK(read_back->tick == 1);
    CHECK(read_back->rows.size() == 1);
    CHECK(read_back->rows[0].handle == 7);

    CameraSnapshot second;
    second.tick = 2;
    CHECK(channel.publish(second) == 2);

    std::uint64_t revision = 0;
    auto with_revision = channel.read(&revision);
    CHECK(revision == 2);
    CHECK(with_revision->tick == 2);
    CHECK(with_revision->rows.empty());

    channel.clear();
    CHECK(!channel.published());
    CHECK(channel.revision() == 0);
}

void test_old_snapshot_survives() {
    Corona::Kernel::SnapshotChannel<CameraSnapshot> channel;

    CameraSnapshot first;
    first.tick = 1;
    first.rows.push_back({1, 500, 500});
    channel.publish(first);

    // 读者先拿到一份
    auto held = channel.read();
    CHECK(held != nullptr);

    // 发布者随后连续覆盖
    for (int i = 2; i <= 5; ++i) {
        CameraSnapshot next;
        next.tick = static_cast<std::uint64_t>(i);
        channel.publish(next);
    }

    // 手里这份必须还是 tick=1 且内容完整（生命周期与 DAG 解耦）
    CHECK(held->tick == 1);
    CHECK(held->rows.size() == 1);
    CHECK(held->rows[0].a == 500);
    CHECK(channel.revision() == 5);
}

void test_concurrent_readers_see_consistent_payload() {
    Corona::Kernel::SnapshotChannel<CameraSnapshot> channel;
    std::atomic<bool> stop{false};
    std::atomic<int> tornado_reads{0};
    std::atomic<int> failures{0};

    // 发布者：每轮 a 递增，b 恒为 1000-a，保证 a+b==1000
    std::thread publisher([&channel, &stop]() {
        for (int i = 0; i < 2000 && !stop.load(); ++i) {
            CameraSnapshot snapshot;
            snapshot.tick = static_cast<std::uint64_t>(i);
            snapshot.rows.push_back({1, i, 1000 - i});
            channel.publish(snapshot);
        }
        stop.store(true);
    });

    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&channel, &stop, &tornado_reads, &failures]() {
            std::uint64_t last_revision = 0;
            while (!stop.load()) {
                std::uint64_t revision = 0;
                auto snapshot = channel.read(&revision);
                if (snapshot == nullptr) {
                    continue;
                }
                if (revision < last_revision) {
                    ++failures;  // revision 不允许回退
                }
                last_revision = revision;
                if (!snapshot->rows.empty()) {
                    const auto& row = snapshot->rows[0];
                    if (row.a + row.b != 1000) {
                        ++failures;  // 撕裂读
                    }
                }
                ++tornado_reads;
            }
        });
    }

    publisher.join();
    stop.store(true);
    for (auto& reader : readers) {
        reader.join();
    }

    CHECK(failures.load() == 0);
    CHECK(tornado_reads.load() > 0);
    CHECK(channel.revision() == 2000);
}

void test_thread_affinity() {
    Corona::Kernel::ThreadAffinity affinity;
    CHECK(!affinity.bound());
    CHECK(affinity.assert_owner_thread("unbound"));  // 未绑定时不误报

    affinity.bind_to_current_thread();
    CHECK(affinity.bound());
    CHECK(affinity.is_owner_thread());
    CHECK(affinity.assert_owner_thread("owner"));

    bool foreign_is_owner = true;
    bool foreign_assert = true;
    std::thread foreign([&affinity, &foreign_is_owner, &foreign_assert]() {
        foreign_is_owner = affinity.is_owner_thread();
        foreign_assert = affinity.assert_owner_thread("foreign");
    });
    foreign.join();

    CHECK(!foreign_is_owner);
    CHECK(!foreign_assert);
    CHECK(affinity.violations() == 1);
    CHECK(affinity.assert_owner_thread("owner again"));

    affinity.reset();
    CHECK(!affinity.bound());
    CHECK(affinity.violations() == 0);
}

}  // namespace

int main() {
    std::printf("== SnapshotChannel / ThreadAffinity tests ==\n");
    test_read_before_publish();
    test_publish_and_revision();
    test_old_snapshot_survives();
    test_concurrent_readers_see_consistent_payload();
    test_thread_affinity();

    if (g_failures == 0) {
        std::printf("OK: %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("FAILED: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
