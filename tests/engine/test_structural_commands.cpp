/**
 * @file test_structural_commands.cpp
 * @brief 结构命令通道的结算 / 重入 / 超时 / 停机契约（spec §5.1）
 *
 * 这些行为只能用一个"假拥有者线程"来验证：真引擎里 DAG 与 UI 的时序不可复现。
 * 覆盖（缺一条就是回归）：
 * 1. 超时是失败且**不留下实体**：调用方返回 0，registry 实体数不变；迟到的
 *    drain 必须跳过已被放弃的请求（不能创建无人签收的实体）；
 * 2. 停机结算：仍在等待的调用方立刻拿到失败，而不是等满预算；
 * 3. 停机后新请求立即拒绝（不排队、不阻塞）——包括**拥有者线程自己**的创建请求
 *    （停机判定先于重入判定，见 `test_the_owner_thread_cannot_create_once_shutdown_begins`）；
 * 4. 重入：拥有者线程自己调用 → 直接应用，不入队（否则自死锁）；
 * 5. 正常路径：假 DAG 线程 drain → 应用 → 句柄有效、初始组件值正确；
 * 6. 默认超时 = 真实 tick 的两帧（下限 17ms），显式覆盖优先；
 * 7. 装配前（还没有拥有者）调用线程直接应用：引擎装配期与单线程夹具的既定行为；
 * 8. 所有等待都有界：每个等待都断言 elapsed < 一个上界；
 * 9. 批量创建成本：N=200 个 actor 通过公开 API 创建/销毁的实测墙钟时间。
 */

#include <corona/ecs/structural_commands.h>
#include <corona/engine/engine_runtime_api.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
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

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Corona::Ecs::CreateGeometryPayload;
using Corona::Ecs::StructuralCommandChannel;

long long elapsed_ms(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

CreateGeometryPayload make_payload(Corona::EntityId transform_handle) {
    CreateGeometryPayload payload{};
    payload.geometry.transform_handle = transform_handle;
    payload.geometry.model_resource_handle = 0x5EED;
    payload.model_source.path_utf8 = "structural-command-test.obj";
    return payload;
}

std::size_t entity_count() {
    return Corona::Ecs::World::instance().size();
}

/// 默认超时 = 两帧（帧周期向上的毫秒数），下限 17ms。
void test_the_default_timeout_is_two_frames_of_the_real_tick() {
    using Corona::Ecs::two_frame_timeout_ms;
    CHECK(two_frame_timeout_ms(1'000'000 / 30) == 67);   // 30Hz: 33.33ms/帧
    CHECK(two_frame_timeout_ms(1'000'000 / 60) == 34);   // 60Hz: 16.67ms/帧
    CHECK(two_frame_timeout_ms(1'000'000 / 120) == 17);  // 120Hz: 下限 17ms
    CHECK(two_frame_timeout_ms(1'000'000 / 480) == 17);  // 480Hz: 下限 17ms

    auto& channel = StructuralCommandChannel::instance();
    channel.clear_timeout_override();
    channel.set_tick_period(std::chrono::microseconds{1'000'000 / 30});
    CHECK(!channel.timeout_is_override());
    CHECK(channel.timeout() == 67ms);
    channel.set_tick_period(std::chrono::microseconds{1'000'000 / 120});
    CHECK(channel.timeout() == 17ms);

    // 显式覆盖优先于自动推导
    channel.set_timeout(123ms);
    CHECK(channel.timeout_is_override());
    CHECK(channel.timeout() == 123ms);
    channel.clear_timeout_override();
    CHECK(channel.timeout() == 17ms);
}

/// 装配前：还没有任何拥有者存在时，调用线程直接应用（引擎装配期 / 单线程夹具）。
///
/// 这条必须**先于**任何 `on_dag_started()` 运行：装配方一旦宣布拥有者存在，
/// 例外就结束。它也解释了为什么引擎初始化阶段（系统线程还没起）创建 surface
/// 图像不会失败。
void test_before_assembly_the_calling_thread_applies_directly() {
    auto& channel = StructuralCommandChannel::instance();
    CHECK(!channel.owner_expected());
    channel.clear_timeout_override();
    channel.set_tick_period(8333us);
    channel.set_timeout(60ms);

    const auto before = entity_count();
    const auto stats_before = channel.stats();

    const auto started = Clock::now();
    const Corona::EntityId handle = Corona::Ecs::request_create_geometry(make_payload(0x0BEF));
    const long long waited = elapsed_ms(started);

    CHECK(handle != 0);
    CHECK(waited < 60);                   // 没有等 tick
    CHECK(channel.commands().size() == 0);  // 没有入队
    CHECK(entity_count() == before + 1);
    CHECK(channel.stats().pre_assembly == stats_before.pre_assembly + 1);
    CHECK(channel.stats().inline_applied == stats_before.inline_applied);
}

/// 正常路径：假 DAG 线程 drain → 应用 → 调用方拿到有效句柄与初始组件值。
void test_a_running_owner_applies_the_request() {
    auto& channel = StructuralCommandChannel::instance();
    channel.clear_timeout_override();
    channel.set_tick_period(8333us);
    channel.set_timeout(500ms);
    channel.on_dag_started();

    const auto before = entity_count();
    std::atomic<bool> stop{false};
    std::atomic<int> applied{0};
    std::thread owner([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            if (Corona::Ecs::apply_pending_structural_commands()) {
                applied.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::sleep_for(200us);
        }
    });

    const auto started = Clock::now();
    const Corona::EntityId handle = Corona::Ecs::request_create_geometry(make_payload(0xA11CE));
    const long long waited = elapsed_ms(started);
    stop.store(true, std::memory_order_relaxed);
    owner.join();

    CHECK(handle != 0);
    CHECK(applied.load() >= 1);
    CHECK(waited < 500);  // 有界
    CHECK(entity_count() == before + 1);
    CHECK(channel.commands().size() == 0);

    const auto* geometry = Corona::Ecs::try_get<Corona::GeometryDevice>(handle);
    CHECK(geometry != nullptr);
    if (geometry != nullptr) {
        CHECK(geometry->transform_handle == 0xA11CE);
        CHECK(geometry->model_resource_handle == 0x5EED);
    }
    const auto* source = Corona::Ecs::try_get<Corona::Ecs::ModelSource>(handle);
    CHECK(source != nullptr);
    if (source != nullptr) {
        CHECK(source->path_utf8 == "structural-command-test.obj");
    }
}

/// 超时：失败、有界、不创建实体；迟到的 drain 跳过已放弃的请求。
void test_a_timeout_fails_and_leaves_no_entity_behind() {
    auto& channel = StructuralCommandChannel::instance();
    channel.set_tick_period(8333us);
    channel.set_timeout(60ms);
    channel.on_dag_started();

    const auto before = entity_count();
    const auto stats_before = channel.stats();

    const auto started = Clock::now();
    const Corona::EntityId handle = Corona::Ecs::request_create_geometry(make_payload(0x7E57));
    const long long waited = elapsed_ms(started);

    CHECK(handle == 0);
    CHECK(waited >= 60);    // 真的等满了预算
    CHECK(waited < 1000);   // 但有界
    CHECK(entity_count() == before);  // 没有孤儿实体
    CHECK(channel.commands().size() == 1);  // 命令还在队列里，无人签收
    CHECK(channel.stats().timeouts == stats_before.timeouts + 1);

    // 迟到的 drain：拥有者拿到它也只能跳过
    Corona::Ecs::apply_pending_structural_commands();
    CHECK(channel.commands().size() == 0);
    CHECK(entity_count() == before);
    CHECK(channel.stats().abandoned == stats_before.abandoned + 1);
}

/// 停机：等待中的调用方被结算为失败；之后的新请求立即拒绝。
void test_stop_settles_waiting_callers_and_rejects_new_requests() {
    auto& channel = StructuralCommandChannel::instance();
    channel.clear_timeout_override();
    channel.set_timeout(400ms);
    channel.on_dag_started();

    const auto before = entity_count();
    std::atomic<Corona::EntityId> caller_result{0};
    std::atomic<long long> caller_waited{-1};

    std::thread caller([&]() {
        const auto started = Clock::now();
        caller_result.store(Corona::Ecs::request_create_geometry(make_payload(0xDEAD)));
        caller_waited.store(elapsed_ms(started));
    });

    // 等到请求真的入队（调用方正阻塞在 wait() 上）
    const auto enqueue_deadline = Clock::now() + 2s;
    while (channel.commands().size() == 0 && Clock::now() < enqueue_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(channel.commands().size() == 1);
    const auto stats_before_stop = channel.stats();

    const auto settle_started = Clock::now();
    const std::size_t settled = channel.on_dag_stopping("structural command test shutdown");
    const long long settle_ms = elapsed_ms(settle_started);
    caller.join();

    CHECK(settled == 1);
    CHECK(caller_result.load() == 0);
    CHECK(caller_waited.load() >= 0);
    CHECK(caller_waited.load() < 400);  // 不是等满预算才失败
    CHECK(settle_ms < 400);
    CHECK(entity_count() == before);    // 停机没有创建任何实体
    CHECK(channel.stats().settled_on_stop == stats_before_stop.settled_on_stop + 1);
    // 失败只由等待方记一次（不再与停机结算重复计数）
    CHECK(channel.stats().failed == stats_before_stop.failed + 1);

    // 停机后的新请求：立即拒绝，不排队、不阻塞
    const auto rejected_before = channel.stats().rejected;
    const auto immediate_started = Clock::now();
    const Corona::EntityId rejected = Corona::Ecs::request_create_geometry(make_payload(0xBEEF));
    const long long immediate_ms = elapsed_ms(immediate_started);
    CHECK(rejected == 0);
    CHECK(immediate_ms < 50);
    CHECK(channel.stats().rejected == rejected_before + 1);
    CHECK(channel.commands().size() == 0);
    CHECK(entity_count() == before);
}

/// 重入：拥有者线程自己调用 → 直接应用，不入队、不自等。
void test_the_owner_thread_applies_inline() {
    auto& channel = StructuralCommandChannel::instance();
    channel.set_tick_period(8333us);
    channel.clear_timeout_override();
    channel.set_timeout(50ms);
    channel.on_dag_started();

    // tick 边界的 drain 把当前线程登记为 registry 的拥有者
    Corona::Ecs::apply_pending_structural_commands();
    CHECK(channel.affinity().is_owner_thread());

    const auto before = entity_count();
    const auto stats_before = channel.stats();

    const auto started = Clock::now();
    const Corona::EntityId handle = Corona::Ecs::request_create_geometry(make_payload(0x1A11));
    const long long waited = elapsed_ms(started);

    CHECK(handle != 0);
    CHECK(waited < 50);                    // 没有等一次 tick
    CHECK(channel.commands().size() == 0); // 没有入队
    CHECK(channel.stats().inline_applied == stats_before.inline_applied + 1);
    CHECK(entity_count() == before + 1);
    const auto* geometry = Corona::Ecs::try_get<Corona::GeometryDevice>(handle);
    CHECK(geometry != nullptr);
    if (geometry != nullptr) {
        CHECK(geometry->transform_handle == 0x1A11);
    }
}

/// 停机判定先于重入判定：**拥有者线程自己**在停机开始后的创建请求也必须被拒绝。
///
/// `request_*` 先检查 `stopped_` 再看 `affinity`，正是为了让"引擎正在关停、DAG
/// 线程自己还在跑最后一个 tick"这种情况也拿不到新实体：新建的实体会落在一个马上
/// 被拆掉的世界上。这条谁都不许改成"拥有者可以例外"。
void test_the_owner_thread_cannot_create_once_shutdown_begins() {
    auto& channel = StructuralCommandChannel::instance();
    channel.set_tick_period(8333us);
    channel.clear_timeout_override();
    channel.set_timeout(50ms);
    channel.on_dag_started();

    // tick 边界的 drain 把当前线程登记为 registry 的拥有者
    Corona::Ecs::apply_pending_structural_commands();
    CHECK(channel.affinity().is_owner_thread());
    CHECK(!channel.stopped());

    channel.on_dag_stopping("structural command test shutdown begins");
    CHECK(channel.stopped());

    const auto before = entity_count();
    const auto stats_before = channel.stats();

    const auto started = Clock::now();
    const Corona::EntityId handle = Corona::Ecs::request_create_geometry(make_payload(0x5109));
    const long long waited = elapsed_ms(started);

    CHECK(handle == 0);                    // 被拒绝，而不是被应用
    CHECK(waited < 50);                    // 立即拒绝，不排队、不等待
    CHECK(entity_count() == before);       // 没有创建任何实体
    CHECK(channel.commands().size() == 0); // 没有入队
    CHECK(channel.stats().rejected == stats_before.rejected + 1);
    // 也没有悄悄走"重入直接应用"的路径
    CHECK(channel.stats().inline_applied == stats_before.inline_applied);
}

/// 批量创建成本：N=200 个 actor 通过**公开 API** 创建/销毁的实测墙钟。
///
/// 同步应答契约意味着每个请求要等一次 tick 边界：这条用例量出"每个结构命令
/// 的边界成本"，并把它与"直接调用创建原语"的基线和上限断言放在一起。它不追求
/// 好看的数字，只保证成本线性、有界，并把真实数值打出来供 Task 1b 报告引用。
void test_bulk_creation_cost_through_the_public_api() {
    constexpr int kCount = 200;

    auto& channel = StructuralCommandChannel::instance();
    channel.clear_timeout_override();
    channel.set_tick_period(8333us);   // 120Hz：一帧 8.33ms
    channel.set_timeout(500ms);
    channel.on_dag_started();
    // 上面的用例把本线程登记成了拥有者；这里要让假 DAG 线程成为拥有者，
    // 否则 200 个请求会全部走重入直接应用，量到的就不是边界成本。
    channel.affinity().reset();
    CHECK(!channel.affinity().is_owner_thread());

    // 基线：同一个创建原语直接调用（拥有者线程 / 装配前的成本）
    std::vector<Corona::EntityId> baseline_handles;
    baseline_handles.reserve(kCount);
    const auto baseline_started = Clock::now();
    for (int i = 0; i < kCount; ++i) {
        baseline_handles.push_back(
            Corona::Ecs::create_transform_entity(Corona::Ecs::CreateTransformPayload{}));
    }
    const long long baseline_ms = elapsed_ms(baseline_started);
    for (const auto handle : baseline_handles) {
        Corona::Ecs::destroy(handle);
    }

    std::atomic<bool> stop{false};
    std::thread owner([&]() {
        // 120Hz 边界：用截止时间 + yield 自旋，而不是 sleep_for —— Windows 默认计时器
        // 分辨率是 15.6ms，sleep_for(8333us) 实际会睡约 15.6ms，量出来的就不是 120Hz。
        auto next = Clock::now();
        while (!stop.load(std::memory_order_relaxed)) {
            Corona::Ecs::apply_pending_structural_commands();
            next += std::chrono::microseconds{8333};
            while (!stop.load(std::memory_order_relaxed) && Clock::now() < next) {
                std::this_thread::yield();
            }
        }
    });

    std::vector<std::unique_ptr<Corona::API::Actor>> actors;
    actors.reserve(kCount);
    const auto create_started = Clock::now();
    for (int i = 0; i < kCount; ++i) {
        actors.push_back(std::make_unique<Corona::API::Actor>());
    }
    const long long create_ms = elapsed_ms(create_started);

    int valid = 0;
    for (const auto& actor : actors) {
        if (actor->get_handle() != 0) {
            ++valid;
        }
    }

    const auto destroy_started = Clock::now();
    actors.clear();
    const long long destroy_ms = elapsed_ms(destroy_started);

    stop.store(true, std::memory_order_relaxed);
    owner.join();

    const double per_create_ms = static_cast<double>(create_ms) / kCount;
    const double per_destroy_ms = static_cast<double>(destroy_ms) / kCount;
    std::printf("  [bulk] %d actors through the public API: create %lldms (%.2fms/actor), "
                "destroy %lldms (%.2fms/actor); direct primitive baseline %lldms (%.3fms/actor)\n",
                kCount, create_ms, per_create_ms, destroy_ms, per_destroy_ms, baseline_ms,
                static_cast<double>(baseline_ms) / kCount);

    CHECK(valid == kCount);                 // 每个请求都结算成功
    CHECK(create_ms >= 0 && destroy_ms >= 0);
    // 有界：一条命令最多等一个 tick（8.33ms）+ 余量；200 个请求的线性成本不可能超过
    // 200×50ms。超过就说明边界没有在 drain（或每次都在等满超时预算）。
    CHECK(per_create_ms < 50.0);
    CHECK(per_destroy_ms < 50.0);
    CHECK(create_ms < kCount * 50);
    // 边界成本必须明显高于直接调用原语，否则量到的不是同步应答的成本。
    CHECK(baseline_ms < create_ms);
}

}  // namespace

int main() {
    std::printf("== Structural command (settlement / re-entrancy / timeout / shutdown) ==\n");
    test_the_default_timeout_is_two_frames_of_the_real_tick();
    test_before_assembly_the_calling_thread_applies_directly();
    test_a_running_owner_applies_the_request();
    test_a_timeout_fails_and_leaves_no_entity_behind();
    test_stop_settles_waiting_callers_and_rejects_new_requests();
    test_the_owner_thread_applies_inline();
    test_the_owner_thread_cannot_create_once_shutdown_begins();
    test_bulk_creation_cost_through_the_public_api();

    if (g_failures == 0) {
        std::printf("OK: %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("FAILED: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
