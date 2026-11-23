/**
 * @file test_structural_commands.cpp
 * @brief 结构命令通道的结算 / 重入 / 存活等待 / 停机契约（spec §5.1）
 *
 * 这些行为只能用一个"假拥有者线程"来验证：真引擎里 DAG 与 UI 的时序不可复现。
 * 覆盖（缺一条就是回归）：
 * 1. 无进展失败且**不留下实体**：边界真的卡死（预算内没有心跳）时调用方返回 0，
 *    registry 实体数不变；迟到的 drain 必须跳过已被放弃的请求（不能创建无人签收的实体）；
 * 2. **慢 tick 不失败**：拥有者停顿 > 1s（远大于两帧，远小于无进展预算）之后
 *    drain，调用方必须成功 —— 这是 Task 1c 的核心（项目加载期 Optics 会占住
 *    DAG 3–4s，旧的 17ms 墙钟预算会丢掉 actor 实体）；
 * 3. **心跳重置预算**：只要边界还在完成，等待就可以远超墙钟预算而不失败；
 * 4. 停机结算：仍在等待的调用方立刻拿到失败，而不是等满预算；
 * 5. 停机后新请求立即拒绝（不排队、不阻塞）——包括**拥有者线程自己**的创建请求；
 * 6. 重入：拥有者线程自己调用 → 直接应用，不入队（否则自死锁）；
 * 7. 正常路径：假 DAG 线程 drain → 应用 → 句柄有效、初始组件值正确；
 * 8. 两帧只是**诊断阈值**（30Hz→67ms 等），无进展预算才是失败判据（默认 10s）；
 * 9. 装配前（还没有拥有者）调用线程直接应用：引擎装配期与单线程夹具的既定行为；
 * 10. 所有等待都有界：每个等待都断言 elapsed < 一个上界；
 * 11. **空队列的边界也发心跳**：等待方要的是"边界在推进"，不是"队列被消费过"；
 * 12. **DAG_UNRESPONSIVE 闩锁**：一次无进展超时之后请求立即失败（不再等预算），
 *     一次边界完成自动解锁；
 * 13. **销毁不等待**：闩锁/卡死期间 200 条销毁仍是入队即返回（不按实体数 × 预算放大），
 *     边界恢复后统一落地，不留孤儿；
 * 14. **profile 创建没结算不算成功**：`Actor::add_profile` 返回 nullptr 并回滚登记
 *     （否则装载会产出"没有引擎侧 profile"的半连接 actor）；
 * 15. 批量创建成本：N=200 个 actor 通过公开 API 创建/销毁的实测墙钟时间。
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

/// 两帧只是**诊断阈值**，失败判据是"无进展预算"（默认 10s）。
///
/// 旧公式断言（30Hz→67ms 等）保留，但语义变了：它描述"多久算慢"（写 WARNING），
/// 不再描述"多久失败"。帧周期改阈值、不改预算；覆盖只改预算、不改阈值。
void test_two_frames_is_a_diagnostic_threshold_not_a_wait_limit() {
    using Corona::Ecs::kDefaultNoProgressBudgetMs;
    using Corona::Ecs::two_frame_diagnostic_ms;
    CHECK(two_frame_diagnostic_ms(1'000'000 / 30) == 67);   // 30Hz: 33.33ms/帧
    CHECK(two_frame_diagnostic_ms(1'000'000 / 60) == 34);   // 60Hz: 16.67ms/帧
    CHECK(two_frame_diagnostic_ms(1'000'000 / 120) == 17);  // 120Hz: 下限 17ms
    CHECK(two_frame_diagnostic_ms(1'000'000 / 480) == 17);  // 480Hz: 下限 17ms

    auto& channel = StructuralCommandChannel::instance();
    channel.clear_no_progress_budget_override();
    CHECK(!channel.no_progress_budget_is_override());
    CHECK(kDefaultNoProgressBudgetMs == 10'000);
    CHECK(channel.no_progress_budget() == std::chrono::milliseconds{kDefaultNoProgressBudgetMs});

    // 帧周期只改诊断阈值，预算不动。
    channel.set_tick_period(std::chrono::microseconds{1'000'000 / 30});
    CHECK(channel.slow_wait_diagnostic_threshold() == 67ms);
    CHECK(channel.no_progress_budget() == std::chrono::milliseconds{kDefaultNoProgressBudgetMs});
    channel.set_tick_period(std::chrono::microseconds{1'000'000 / 120});
    CHECK(channel.slow_wait_diagnostic_threshold() == 17ms);

    // 显式覆盖只改预算，阈值不动。
    channel.set_no_progress_budget(123ms);
    CHECK(channel.no_progress_budget_is_override());
    CHECK(channel.no_progress_budget() == 123ms);
    CHECK(channel.slow_wait_diagnostic_threshold() == 17ms);
    channel.clear_no_progress_budget_override();
    CHECK(channel.no_progress_budget() == std::chrono::milliseconds{kDefaultNoProgressBudgetMs});
    CHECK(!channel.no_progress_budget_is_override());
}

/// 装配前：还没有任何拥有者存在时，调用线程直接应用（引擎装配期 / 单线程夹具）。
///
/// 这条必须**先于**任何 `on_dag_started()` 运行：装配方一旦宣布拥有者存在，
/// 例外就结束。它也解释了为什么引擎初始化阶段（系统线程还没起）创建 surface
/// 图像不会失败。
void test_before_assembly_the_calling_thread_applies_directly() {
    auto& channel = StructuralCommandChannel::instance();
    CHECK(!channel.owner_expected());
    channel.clear_no_progress_budget_override();
    channel.set_tick_period(8333us);
    channel.set_no_progress_budget(60ms);

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
    channel.clear_no_progress_budget_override();
    channel.set_tick_period(8333us);
    channel.set_no_progress_budget(500ms);
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

/// 心跳的契约：**队列为空**的边界也要发心跳。
///
/// 等待方要的是"边界还在推进"，而不是"队列被消费过"。如果心跳只在有命令时才发，
/// 一次加载里的空闲 tick 会被当成卡死。
void test_an_empty_tick_boundary_still_publishes_the_heartbeat() {
    auto& channel = StructuralCommandChannel::instance();
    channel.on_dag_started();
    channel.affinity().reset();

    // 先清空队列（上一条用例可能留下命令）。
    Corona::Ecs::apply_pending_structural_commands();
    CHECK(channel.commands().size() == 0);

    const auto before = channel.heartbeat();
    const bool applied = Corona::Ecs::apply_pending_structural_commands();
    CHECK(!applied);                          // 没有命令可应用
    CHECK(channel.heartbeat() == before + 1);  // 但边界仍然发布了一次心跳
    CHECK(channel.commands().size() == 0);
    channel.affinity().reset();
}

/// 无进展（边界真的卡死）：失败、有界、不创建实体；迟到的 drain 跳过已放弃的请求。
///
/// 注意这是"卡死"而不是"慢"：全程没有任何 tick 边界完成（心跳不动），
/// 所以无进展预算（这里 60ms）耗尽后失败 —— 旧契约里这一幕由墙钟超时承担。
void test_a_stalled_boundary_fails_after_the_no_progress_budget() {
    auto& channel = StructuralCommandChannel::instance();
    channel.set_tick_period(8333us);
    channel.set_no_progress_budget(60ms);
    channel.on_dag_started();
    // 上面的用例把假 DAG 线程登记成了拥有者；这里要确保本线程不是拥有者，
    // 否则请求会走重入直接应用，量到的不是等待。
    channel.affinity().reset();
    CHECK(!channel.affinity().is_owner_thread());

    const auto before = entity_count();
    const auto stats_before = channel.stats();
    const auto heartbeat_before = channel.heartbeat();

    const auto started = Clock::now();
    const Corona::EntityId handle = Corona::Ecs::request_create_geometry(make_payload(0x7E57));
    const long long waited = elapsed_ms(started);

    CHECK(handle == 0);
    CHECK(waited >= 60);    // 真的等满了无进展预算
    CHECK(waited < 1000);   // 但有界
    CHECK(entity_count() == before);  // 没有孤儿实体
    CHECK(channel.commands().size() == 1);  // 命令还在队列里，无人签收
    CHECK(channel.stats().timeouts == stats_before.timeouts + 1);
    CHECK(channel.heartbeat() == heartbeat_before);  // 全程没有边界完成 = 卡死

    // 迟到的 drain：拥有者拿到它也只能跳过（并发布一次心跳）
    Corona::Ecs::apply_pending_structural_commands();
    CHECK(channel.commands().size() == 0);
    CHECK(entity_count() == before);
    CHECK(channel.stats().abandoned == stats_before.abandoned + 1);
    CHECK(channel.heartbeat() == heartbeat_before + 1);
}

/// 停机：等待中的调用方被结算为失败；之后的新请求立即拒绝。
void test_stop_settles_waiting_callers_and_rejects_new_requests() {
    auto& channel = StructuralCommandChannel::instance();
    channel.clear_no_progress_budget_override();
    channel.set_no_progress_budget(400ms);
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

/// **Task 1c 的核心**：拥有者停顿 > 1s（远大于两帧 17ms）之后 drain，调用方必须成功。
///
/// 场景与项目加载完全同构：DAG 线程在 Optics `render_dispatch` 里待了 3–4s
/// （GPU 管线编译 + 128MiB shadow 分配），这期间没有任何 tick 边界完成——心跳确实
/// 不前进，但 DAG 活着。旧契约（两帧墙钟）在这里超时并丢掉 actor 实体；新契约
/// （无进展预算，默认 10s）只是写一条诊断 WARNING，然后继续等，边界恢复即成功。
void test_an_owner_that_stalls_for_a_second_then_drains_lets_the_caller_succeed() {
    auto& channel = StructuralCommandChannel::instance();
    channel.clear_no_progress_budget_override();
    channel.set_no_progress_budget(5000ms);  // 远大于 1.2s 停顿，也远小于"卡死"
    channel.set_tick_period(8333us);         // 诊断阈值 17ms：这条等待必然越过它
    channel.on_dag_started();
    channel.affinity().reset();
    CHECK(!channel.affinity().is_owner_thread());

    const auto before = entity_count();
    const auto stats_before = channel.stats();
    const auto heartbeat_before = channel.heartbeat();

    std::atomic<Corona::EntityId> caller_result{0};
    std::atomic<long long> caller_waited{-1};
    std::thread caller([&]() {
        const auto started = Clock::now();
        caller_result.store(Corona::Ecs::request_create_geometry(make_payload(0x5107)));
        caller_waited.store(elapsed_ms(started));
    });

    // 等请求真的入队，然后让"拥有者"停住 1.2s：不 drain、不发心跳。
    const auto enqueue_deadline = Clock::now() + 2s;
    while (channel.commands().size() == 0 && Clock::now() < enqueue_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(channel.commands().size() == 1);
    std::this_thread::sleep_for(1200ms);
    CHECK(channel.heartbeat() == heartbeat_before);  // 停顿期间没有边界完成
    CHECK(caller_result.load() == 0);                // 但调用方还在等，没有失败

    // 边界恢复：一次普通 drain 就把它结算掉。
    Corona::Ecs::apply_pending_structural_commands();
    caller.join();

    CHECK(caller_result.load() != 0);            // 慢 tick 之后**成功**
    CHECK(caller_waited.load() >= 1200);         // 真的等过了停顿
    CHECK(caller_waited.load() < 5000);          // 但没有等满无进展预算
    CHECK(channel.stats().timeouts == stats_before.timeouts);  // 没有一条失败
    CHECK(channel.stats().failed == stats_before.failed);
    CHECK(channel.stats().applied == stats_before.applied + 1);
    CHECK(channel.stats().slow_waits == stats_before.slow_waits + 1);  // 诊断阈值报警一次
    CHECK(channel.commands().size() == 0);
    CHECK(entity_count() == before + 1);
}

/// 心跳重置无进展预算：只要边界还在完成，等待可以远超墙钟预算而不失败。
///
/// 这条用"只发心跳、不 drain"的拥有者：边界一直在推进（心跳在动），但这一条请求
/// 还没轮到。墙钟已经远超预算，调用方仍然必须继续等 —— 判据是"有没有进展"，
/// 不是"等了多久"。
///
/// 同步点是**入队**：拥有者先等请求真的进队列，再开始心跳。否则 CI 负载下拥有者
/// 可能把 8 次心跳 + drain 全部跑完而调用方还没提交，请求就永远没人应用（假超时）。
void test_completed_boundaries_reset_the_no_progress_budget() {
    auto& channel = StructuralCommandChannel::instance();
    channel.clear_no_progress_budget_override();
    channel.set_no_progress_budget(150ms);   // 墙钟预算
    channel.set_tick_period(8333us);
    channel.on_dag_started();
    channel.affinity().reset();

    const auto before = entity_count();
    const auto stats_before = channel.stats();
    const auto heartbeat_before = channel.heartbeat();

    std::atomic<bool> draining{false};
    std::thread owner([&]() {
        // 等请求入队（而不是等一个固定时长）。
        const auto enqueue_deadline = Clock::now() + 2s;
        while (channel.commands().size() == 0 && Clock::now() < enqueue_deadline) {
            std::this_thread::yield();
        }
        // ~320ms 的心跳（40ms 一次）> 150ms 预算，但每次心跳都重置计时器。
        for (int i = 0; i < 8; ++i) {
            channel.publish_tick_boundary();
            std::this_thread::sleep_for(40ms);
        }
        draining.store(true, std::memory_order_release);
        Corona::Ecs::apply_pending_structural_commands();
    });

    const auto started = Clock::now();
    const Corona::EntityId handle = Corona::Ecs::request_create_geometry(make_payload(0xB0A7));
    const long long waited = elapsed_ms(started);
    owner.join();

    CHECK(draining.load());
    CHECK(handle != 0);                     // 边界一直在推进 → 绝不失败
    CHECK(waited >= 280);                   // 远超 150ms 的墙钟预算
    CHECK(waited < 5000);                   // 仍然有界
    CHECK(waited > 150);                    // 不是"预算都没过"的巧合
    CHECK(channel.heartbeat() >= heartbeat_before + 8);
    CHECK(channel.stats().timeouts == stats_before.timeouts);
    CHECK(channel.commands().size() == 0);
    CHECK(entity_count() == before + 1);
}

/// DAG_UNRESPONSIVE 闩锁：一次无进展超时之后，后续请求**立即**失败（不再等一个
/// 预算），一次 tick 边界完成就自动解锁。
///
/// 没有闩锁时，"卡死一次 × N 个实体"会把调用方冻住 N × 预算；装载失败的回滚路径
/// 一次要销毁 ~8 个实体/actor，这样一次卡死就能冻住几分钟到几小时。
void test_an_unresponsive_dag_rejects_immediately_until_a_boundary_completes() {
    auto& channel = StructuralCommandChannel::instance();
    channel.clear_no_progress_budget_override();
    channel.set_no_progress_budget(60ms);
    channel.set_tick_period(8333us);
    channel.on_dag_started();
    channel.affinity().reset();
    CHECK(!channel.unresponsive());

    const auto stats_before = channel.stats();

    // 第一次：没有任何边界完成 → 等满 60ms 预算后失败，并打开闩锁。
    const auto first_started = Clock::now();
    CHECK(Corona::Ecs::request_create_geometry(make_payload(0x0BAD)) == 0);
    const long long first_ms = elapsed_ms(first_started);
    CHECK(first_ms >= 60);
    CHECK(first_ms < 1000);
    CHECK(channel.unresponsive());
    CHECK(channel.stats().timeouts == stats_before.timeouts + 1);
    CHECK(channel.stats().unresponsive_rejections == stats_before.unresponsive_rejections);

    // 第二次：立即失败 —— 不排队、也不再等一个预算。
    const auto second_started = Clock::now();
    CHECK(Corona::Ecs::request_create_geometry(make_payload(0x0BAD)) == 0);
    const long long second_ms = elapsed_ms(second_started);
    CHECK(second_ms < 20);
    CHECK(channel.stats().unresponsive_rejections == stats_before.unresponsive_rejections + 1);
    CHECK(channel.stats().timeouts == stats_before.timeouts + 1);  // 不是又一次超时

    // 一次边界完成 → 闩锁清除、排队的废弃命令被跳过。
    Corona::Ecs::apply_pending_structural_commands();
    CHECK(!channel.unresponsive());
    CHECK(channel.commands().size() == 0);

    // 解锁后请求重新被接受（假拥有者线程来应用）。
    channel.affinity().reset();
    std::atomic<bool> stop{false};
    std::thread owner([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            Corona::Ecs::apply_pending_structural_commands();
            std::this_thread::sleep_for(200us);
        }
    });
    const Corona::EntityId handle = Corona::Ecs::request_create_geometry(make_payload(0xA17C));
    stop.store(true, std::memory_order_relaxed);
    owner.join();
    CHECK(handle != 0);
    CHECK(channel.stats().applied == stats_before.applied + 1);
}

/// 销毁不等待：DAG 被闩锁/卡死时，回滚销毁必须立即返回，不能按实体数 × 预算放大。
///
/// 旧行为：每条 `request_destroy_entity` 都等一个无进展预算（这里 60ms），
/// 100 个实体就是 6s、一个 actor（~8 个实体）的失败回滚就会卡住调用方。
/// 新行为：销毁入队即返回（幂等，不需要同步答案），DAG 恢复后下一次边界统一应用。
void test_teardown_destroys_do_not_wait_on_an_unresponsive_dag() {
    auto& channel = StructuralCommandChannel::instance();
    channel.clear_no_progress_budget_override();
    channel.set_no_progress_budget(60ms);
    channel.set_tick_period(8333us);
    channel.on_dag_started();
    channel.affinity().reset();

    constexpr int kCount = 200;
    std::vector<Corona::EntityId> handles;
    handles.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        handles.push_back(
            Corona::Ecs::create_transform_entity(Corona::Ecs::CreateTransformPayload{}));
    }
    for (const auto handle : handles) {
        CHECK(handle != 0);
    }
    const auto entities_before = entity_count();

    // 一次无进展超时把闩锁打开（先证明它之前是关的）。
    CHECK(!channel.unresponsive());
    CHECK(Corona::Ecs::request_create_geometry(make_payload(0xC0DE)) == 0);
    CHECK(channel.unresponsive());
    const auto stats_before = channel.stats();

    const auto destroy_started = Clock::now();
    int queued = 0;
    for (const auto handle : handles) {
        if (Corona::Ecs::request_destroy_entity(handle, "destroy_transform")) {
            ++queued;
        }
    }
    const long long destroy_ms = elapsed_ms(destroy_started);

    CHECK(queued == kCount);
    // 旧语义会是 200 × 60ms = 12s；新语义是入队即返回。
    CHECK(destroy_ms < 500);
    CHECK(channel.stats().deferred_destroys == stats_before.deferred_destroys + kCount);
    CHECK(channel.stats().timeouts == stats_before.timeouts);  // 销毁不消耗预算
    CHECK(channel.commands().size() >= static_cast<std::size_t>(kCount));

    // 边界恢复 → 闩锁清除，排队的销毁真的落地（没有孤儿）。
    Corona::Ecs::apply_pending_structural_commands();
    CHECK(!channel.unresponsive());
    CHECK(channel.commands().size() == 0);
    CHECK(entity_count() == entities_before - static_cast<std::size_t>(kCount));
}

/// Profile 的结构创建没结算时，`Actor::add_profile` 必须返回 nullptr 并回滚内存登记：
/// 交出一个没有引擎侧 ProfileStorage 的 Profile 就是"半连接"的 actor（装载会报成功、
/// 引擎里却没有 profile）。
void test_a_profile_whose_create_did_not_settle_is_not_registered() {
    auto& channel = StructuralCommandChannel::instance();
    channel.clear_no_progress_budget_override();
    channel.set_no_progress_budget(500ms);
    channel.set_tick_period(8333us);
    channel.on_dag_started();
    channel.affinity().reset();

    std::atomic<bool> stop{false};
    std::thread owner([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            Corona::Ecs::apply_pending_structural_commands();
            std::this_thread::sleep_for(200us);
        }
    });

    auto geometry = std::make_unique<Corona::API::Geometry>(std::string{});
    auto optics = std::make_unique<Corona::API::Optics>(*geometry);
    auto mechanics = std::make_unique<Corona::API::Mechanics>(*geometry);
    auto acoustics = std::make_unique<Corona::API::Acoustics>(*geometry);
    auto actor = std::make_unique<Corona::API::Actor>();
    CHECK(geometry->is_valid());
    CHECK(optics->is_valid());
    CHECK(mechanics->is_valid());
    CHECK(acoustics->is_valid());
    CHECK(actor->is_valid());

    // 故障注入：DAG 停止后所有结构创建都被拒绝，profile 的创建一定不结算。
    channel.on_dag_stopping("structural command test: the profile create must fail");

    Corona::API::Actor::Profile profile{};
    profile.geometry = geometry.get();
    profile.optics = optics.get();
    profile.mechanics = mechanics.get();
    profile.acoustics = acoustics.get();
    const auto* added = actor->add_profile(profile);

    CHECK(added == nullptr);                 // 不交出半连接的 Profile
    CHECK(actor->profile_count() == 0);      // 内存登记也回滚了
    CHECK(actor->get_active_profile() == nullptr);

    stop.store(true, std::memory_order_relaxed);
    owner.join();
}

/// 重入：拥有者线程自己调用 → 直接应用，不入队、不自等。
void test_the_owner_thread_applies_inline() {
    auto& channel = StructuralCommandChannel::instance();
    channel.set_tick_period(8333us);
    channel.clear_no_progress_budget_override();
    channel.set_no_progress_budget(50ms);
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
    channel.clear_no_progress_budget_override();
    channel.set_no_progress_budget(50ms);
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
    channel.clear_no_progress_budget_override();
    channel.set_tick_period(8333us);   // 120Hz：一帧 8.33ms
    channel.set_no_progress_budget(500ms);
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
    // 有界（宽松，不依赖 CI 负载）：每条同步创建最多等一个 tick（8.33ms）+ 调度抖动，
    // 但绝不该等满无进展预算（这里 500ms）。销毁是 fire-and-forget，成本接近 0。
    // 用预算而不是一个漂亮的小数字做上界，否则慢 CI 上会假红。
    CHECK(per_create_ms < 500.0);
    CHECK(per_destroy_ms < 500.0);
    CHECK(create_ms < kCount * 500);
    // 边界成本必须明显高于直接调用原语，否则量到的不是同步应答的成本。
    CHECK(baseline_ms < create_ms);
}

}  // namespace

int main() {
    std::printf("== Structural command (settlement / re-entrancy / liveness / shutdown) ==\n");
    test_two_frames_is_a_diagnostic_threshold_not_a_wait_limit();
    test_before_assembly_the_calling_thread_applies_directly();
    test_a_running_owner_applies_the_request();
    test_an_empty_tick_boundary_still_publishes_the_heartbeat();
    test_a_stalled_boundary_fails_after_the_no_progress_budget();
    test_stop_settles_waiting_callers_and_rejects_new_requests();
    test_an_owner_that_stalls_for_a_second_then_drains_lets_the_caller_succeed();
    test_completed_boundaries_reset_the_no_progress_budget();
    test_an_unresponsive_dag_rejects_immediately_until_a_boundary_completes();
    test_teardown_destroys_do_not_wait_on_an_unresponsive_dag();
    test_a_profile_whose_create_did_not_settle_is_not_registered();
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
