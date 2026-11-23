/**
 * @file test_structural_commands.cpp
 * @brief 结构命令通道的结算 / 重入 / 超时 / 停机契约（spec §5.1）
 *
 * 这些行为只能用一个"假拥有者线程"来验证：真引擎里 DAG 与 UI 的时序不可复现。
 * 覆盖（缺一条就是回归）：
 * 1. 超时是失败且**不留下实体**：调用方返回 0，registry 实体数不变；迟到的
 *    drain 必须跳过已被放弃的请求（不能创建无人签收的实体）；
 * 2. 停机结算：仍在等待的调用方立刻拿到失败，而不是等满预算；
 * 3. 停机后新请求立即拒绝（不排队、不阻塞）；
 * 4. 重入：拥有者线程自己调用 → 直接应用，不入队（否则自死锁）；
 * 5. 正常路径：假 DAG 线程 drain → 应用 → 句柄有效、初始组件值正确；
 * 6. 默认超时 = 真实 tick 的两帧（下限 17ms），显式覆盖优先；
 * 7. 所有等待都有界：每个等待都断言 elapsed < 一个上界。
 */

#include <corona/ecs/structural_commands.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>

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

}  // namespace

int main() {
    std::printf("== Structural command (settlement / re-entrancy / timeout / shutdown) ==\n");
    test_the_default_timeout_is_two_frames_of_the_real_tick();
    test_a_running_owner_applies_the_request();
    test_a_timeout_fails_and_leaves_no_entity_behind();
    test_stop_settles_waiting_callers_and_rejects_new_requests();
    test_the_owner_thread_applies_inline();

    if (g_failures == 0) {
        std::printf("OK: %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("FAILED: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
