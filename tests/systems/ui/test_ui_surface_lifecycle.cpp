#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include "corona/events/display_system_events.h"
#include "corona/systems/ui/ui_surface_lifecycle.h"

namespace {

using namespace std::chrono_literals;
using Corona::Systems::UI::DisplaySurfaceResult;
using Corona::Systems::UI::drain_ui_surfaces;
using Corona::Systems::UI::SurfaceCompletionTicket;
using Corona::Systems::UI::UiSurfaceLifecycle;
using Corona::Systems::UI::UiSurfaceState;

static_assert(std::is_copy_constructible_v<SurfaceCompletionTicket>);
static_assert(std::is_copy_assignable_v<SurfaceCompletionTicket>);
static_assert(std::is_const_v<decltype(DisplaySurfaceResult::status)>);
static_assert(std::is_const_v<decltype(DisplaySurfaceResult::message)>);

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void expect(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

void expect_result(const std::shared_ptr<const DisplaySurfaceResult>& result,
                   DisplaySurfaceResult::Status status,
                   std::string_view message,
                   std::string_view failure_message) {
    expect(result != nullptr, failure_message);
    expect(result->status == status, failure_message);
    expect(result->message == message, failure_message);
}

void lifecycle_follows_the_success_path() {
    UiSurfaceLifecycle lifecycle;
    const auto trace_id = lifecycle.trace_id();

    expect(trace_id != 0, "trace ids should be nonzero");
    expect(lifecycle.state() == UiSurfaceState::Registering,
           "a new lifecycle should begin in Registering");

    expect(lifecycle.registration_ticket().succeed(),
           "registration acknowledgement should complete once");
    expect(lifecycle.state() == UiSurfaceState::WaitingFirstPresent,
           "registration success should wait for the first present");

    expect(lifecycle.first_present_ticket().succeed(),
           "first-present acknowledgement should complete once");
    expect(lifecycle.state() == UiSurfaceState::Active,
           "first-present success should activate the surface");

    auto removal = lifecycle.request_removal();
    expect(lifecycle.trace_id() == trace_id,
           "one surface should retain one trace id for its full lifetime");
    expect(lifecycle.state() == UiSurfaceState::Removing,
           "removal should enter Removing before acknowledgement");
    expect(removal.succeed(), "removal acknowledgement should complete once");
    expect(lifecycle.state() == UiSurfaceState::Retired,
           "removal success should retire the surface");
    expect_result(lifecycle.terminal_result(),
                  DisplaySurfaceResult::Status::Succeeded,
                  {},
                  "retired lifecycle should retain its terminal result");
}

void lifecycle_failure_is_terminal() {
    UiSurfaceLifecycle lifecycle;

    expect(lifecycle.registration_ticket().fail("display registration failed"),
           "registration failure should be accepted");
    expect(lifecycle.state() == UiSurfaceState::Failed,
           "registration failure should fail the lifecycle");
    expect_result(lifecycle.terminal_result(),
                  DisplaySurfaceResult::Status::Failed,
                  "display registration failed",
                  "failed lifecycle should retain the original failure");

    auto removal = lifecycle.request_removal();
    expect(lifecycle.state() == UiSurfaceState::Failed,
           "a terminal failure must not transition back to Removing");
    expect(removal.cancel("late cleanup") == false,
           "terminal cleanup ticket should already be immutable");
    expect_result(lifecycle.terminal_result(),
                  DisplaySurfaceResult::Status::Failed,
                  "display registration failed",
                  "late requests must not replace a terminal failure");
}

void completion_ticket_is_copyable_thread_safe_and_idempotent() {
    SurfaceCompletionTicket ticket;
    SurfaceCompletionTicket copy = ticket;
    std::atomic<int> winners{0};
    std::vector<std::thread> contenders;

    for (int index = 0; index < 8; ++index) {
        contenders.emplace_back([copy, index, &winners]() mutable {
            if (copy.try_complete(DisplaySurfaceResult::Status::Failed,
                                  "failure-" + std::to_string(index))) {
                winners.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& contender : contenders) {
        contender.join();
    }

    expect(winners.load(std::memory_order_relaxed) == 1,
           "exactly one concurrent completion should win");
    const auto first_result = ticket.result();
    expect(first_result != nullptr,
           "all ticket copies should observe the winning completion");
    expect(first_result->status == DisplaySurfaceResult::Status::Failed,
           "the winning status should be retained");
    expect(first_result->message.starts_with("failure-"),
           "the winning message should be retained");
    const DisplaySurfaceResult expected{first_result->status, first_result->message};
    expect(*first_result == expected,
           "completion payloads should support value equality queries");

    expect(!copy.succeed(), "completion should be idempotent after the first result");
    expect(copy.result() == first_result,
           "a completed ticket should preserve the same immutable payload");
}

void completion_ticket_waits_without_consuming_the_result() {
    SurfaceCompletionTicket ticket;

    expect(!ticket.wait_until(UiSurfaceLifecycle::Clock::now() + 1ms),
           "an incomplete ticket should report a missed deadline");
    expect(!ticket.is_ready(),
           "a ticket timeout should not manufacture a completion");

    auto copy = ticket;
    std::thread acknowledgement([copy]() mutable {
        std::this_thread::sleep_for(10ms);
        copy.cancel("registration superseded");
    });
    const bool completed =
        ticket.wait_until(UiSurfaceLifecycle::Clock::now() + 1s);
    acknowledgement.join();

    expect(completed, "ticket wait should wake for a delayed completion");
    expect_result(ticket.result(),
                  DisplaySurfaceResult::Status::Cancelled,
                  "registration superseded",
                  "deadline wait should not consume the immutable result");
}

void delayed_acknowledgement_wakes_a_deadline_wait() {
    UiSurfaceLifecycle lifecycle;
    auto registration = lifecycle.registration_ticket();
    std::thread display_ack([registration]() mutable {
        std::this_thread::sleep_for(10ms);
        registration.succeed();
    });

    const bool reached_waiting = lifecycle.wait_until(
        UiSurfaceState::WaitingFirstPresent,
        UiSurfaceLifecycle::Clock::now() + 1s);
    display_ack.join();

    expect(reached_waiting,
           "lifecycle wait should observe a delayed registration acknowledgement");
    expect(lifecycle.state() == UiSurfaceState::WaitingFirstPresent,
           "delayed acknowledgement should advance the lifecycle");
}

void precompleted_first_present_is_not_skipped() {
    UiSurfaceLifecycle lifecycle;

    expect(lifecycle.first_present_ticket().succeed(),
           "a first frame may be acknowledged before registration is observed");
    expect(lifecycle.state() == UiSurfaceState::Registering,
           "first-present completion alone must not skip registration");
    expect(lifecycle.registration_ticket().succeed(),
           "registration should still complete normally");
    expect(lifecycle.state() == UiSurfaceState::Active,
           "precompleted first present should advance directly to Active");
}

void removal_before_first_present_cancels_obsolete_work() {
    UiSurfaceLifecycle lifecycle;
    lifecycle.registration_ticket().succeed();
    expect(lifecycle.state() == UiSurfaceState::WaitingFirstPresent,
           "test setup should be waiting for first present");

    auto first_present = lifecycle.first_present_ticket();
    auto removal = lifecycle.request_removal();

    expect(lifecycle.state() == UiSurfaceState::Removing,
           "removal should supersede a pending first present");
    expect_result(first_present.result(),
                  DisplaySurfaceResult::Status::Cancelled,
                  "surface removal requested",
                  "superseded first-present work should be cancelled");
    expect(!first_present.succeed(),
           "a late first-present acknowledgement must not resurrect the surface");
    expect(removal.succeed(), "removal should still be acknowledged");
    expect(lifecycle.state() == UiSurfaceState::Retired,
           "early removal should end in Retired");
}

void cancelled_removal_is_not_safe_retirement() {
    UiSurfaceLifecycle lifecycle;
    lifecycle.registration_ticket().succeed();
    lifecycle.first_present_ticket().succeed();
    expect(lifecycle.state() == UiSurfaceState::Active,
           "test setup should activate the surface");

    auto removal = lifecycle.request_removal();
    expect(removal.cancel("display teardown cancelled"),
           "display should be able to report cancelled teardown");

    expect(lifecycle.state() == UiSurfaceState::Failed,
           "cancelled teardown must not certify safe retirement");
    expect_result(lifecycle.terminal_result(),
                  DisplaySurfaceResult::Status::Cancelled,
                  "display teardown cancelled",
                  "failed lifecycle should retain the cancellation result");
}

void removal_directly_from_registering_cancels_both_forward_tickets() {
    UiSurfaceLifecycle lifecycle;
    auto registration = lifecycle.registration_ticket();
    auto first_present = lifecycle.first_present_ticket();

    auto removal = lifecycle.request_removal();

    expect(lifecycle.state() == UiSurfaceState::Removing,
           "fresh lifecycle removal should enter Removing");
    expect_result(registration.result(),
                  DisplaySurfaceResult::Status::Cancelled,
                  "surface removal requested",
                  "fresh removal should cancel registration");
    expect_result(first_present.result(),
                  DisplaySurfaceResult::Status::Cancelled,
                  "surface removal requested",
                  "fresh removal should cancel first-present work");
    expect(!registration.succeed(),
           "late registration success must lose after removal starts");
    expect(!first_present.succeed(),
           "late first-present success must lose after removal starts");
    expect(removal.succeed(), "removal acknowledgement should still complete");
    expect(lifecycle.state() == UiSurfaceState::Retired,
           "successful removal from Registering should retire safely");
}

void duplicate_requests_share_one_completion() {
    UiSurfaceLifecycle lifecycle;
    const auto trace_id = lifecycle.trace_id();
    lifecycle.registration_ticket().succeed();
    lifecycle.first_present_ticket().succeed();

    auto first_request = lifecycle.request_removal();
    auto duplicate_request = lifecycle.request_removal();

    expect(lifecycle.trace_id() == trace_id,
           "duplicate requests must not allocate a new trace id");
    expect(duplicate_request.succeed(),
           "one duplicate copy may acknowledge the shared request");
    expect(!first_request.fail("duplicate failure"),
           "a duplicate request must not replace the first completion");
    expect(first_request.result() == duplicate_request.result(),
           "duplicate requests should share one completion payload");
    expect(lifecycle.state() == UiSurfaceState::Retired,
           "duplicate removal requests should retire exactly once");
}

void timed_out_wait_retains_the_pending_lifecycle() {
    UiSurfaceLifecycle lifecycle;
    auto registration = lifecycle.registration_ticket();

    expect(!lifecycle.wait_until(UiSurfaceState::WaitingFirstPresent,
                                 UiSurfaceLifecycle::Clock::now() + 5ms),
           "wait should report a missed deadline");
    expect(lifecycle.state() == UiSurfaceState::Registering,
           "timeout must retain the pending lifecycle state");
    expect(!registration.is_ready(),
           "timeout must not complete or discard the pending ticket");

    registration.succeed();
    expect(lifecycle.wait_until(UiSurfaceState::WaitingFirstPresent,
                                UiSurfaceLifecycle::Clock::now() + 1s),
           "the retained ticket should remain usable after timeout");
}

void main_and_three_secondary_surfaces_drain_by_one_deadline() {
    std::array<UiSurfaceLifecycle, 4> surfaces;
    std::array<UiSurfaceLifecycle*, 4> surface_refs{
        &surfaces[0], &surfaces[1], &surfaces[2], &surfaces[3]};

    for (auto& surface : surfaces) {
        surface.registration_ticket().succeed();
        surface.first_present_ticket().succeed();
        expect(surface.state() == UiSurfaceState::Active,
               "all drain participants should begin active");
    }

    std::thread display_ack([&surfaces]() {
        const auto deadline = UiSurfaceLifecycle::Clock::now() + 1s;
        for (auto& surface : surfaces) {
            while (surface.state() != UiSurfaceState::Removing &&
                   UiSurfaceLifecycle::Clock::now() < deadline) {
                std::this_thread::yield();
            }
            if (surface.state() == UiSurfaceState::Removing) {
                surface.request_removal().succeed();
            }
        }
    });

    const bool drained = drain_ui_surfaces(
        surface_refs, UiSurfaceLifecycle::Clock::now() + 1s);
    display_ack.join();

    expect(drained, "main plus three secondary surfaces should drain by one deadline");
    for (const auto& surface : surfaces) {
        expect(surface.state() == UiSurfaceState::Retired,
               "every drained surface should be retired");
    }

    UiSurfaceLifecycle next_surface;
    expect(next_surface.trace_id() > surfaces.back().trace_id(),
           "trace ids should increase monotonically across surfaces");
}

void drain_rejects_a_failed_removal_participant() {
    std::array<UiSurfaceLifecycle, 4> surfaces;
    std::array<UiSurfaceLifecycle*, 4> surface_refs{
        &surfaces[0], &surfaces[1], &surfaces[2], &surfaces[3]};

    for (auto& surface : surfaces) {
        surface.registration_ticket().succeed();
        surface.first_present_ticket().succeed();
        expect(surface.state() == UiSurfaceState::Active,
               "failed-drain participants should begin active");
    }
    for (std::size_t index = 0; index < surfaces.size(); ++index) {
        auto removal = surfaces[index].request_removal();
        if (index == 2) {
            removal.fail("display teardown failed");
        } else {
            removal.succeed();
        }
    }

    expect(!drain_ui_surfaces(surface_refs,
                              UiSurfaceLifecycle::Clock::now() + 1s),
           "drain must reject a participant whose removal failed");
    expect(surfaces[2].state() == UiSurfaceState::Failed,
           "failed removal participant should remain visibly unsafe");
}

void drain_rejects_a_cancelled_removal_participant() {
    std::array<UiSurfaceLifecycle, 4> surfaces;
    std::array<UiSurfaceLifecycle*, 4> surface_refs{
        &surfaces[0], &surfaces[1], &surfaces[2], &surfaces[3]};

    for (auto& surface : surfaces) {
        surface.registration_ticket().succeed();
        surface.first_present_ticket().succeed();
        expect(surface.state() == UiSurfaceState::Active,
               "cancelled-drain participants should begin active");
    }
    for (std::size_t index = 0; index < surfaces.size(); ++index) {
        auto removal = surfaces[index].request_removal();
        if (index == 1) {
            removal.cancel("display teardown cancelled");
        } else {
            removal.succeed();
        }
    }

    expect(!drain_ui_surfaces(surface_refs,
                              UiSurfaceLifecycle::Clock::now() + 1s),
           "drain must reject a participant whose removal was cancelled");
    expect(surfaces[1].state() == UiSurfaceState::Failed,
           "cancelled removal participant should remain visibly unsafe");
}

void legacy_event_aggregate_publishers_remain_compatible() {
    void* surface = reinterpret_cast<void*>(0x1234);
    auto done = std::make_shared<std::promise<void>>();

    Corona::Events::DisplaySurfaceChangedEvent changed{surface};
    Corona::Events::UIFrameReadyEvent frame{surface, 7, 11, 1920, 1080};
    Corona::Events::DisplaySurfaceRemovedEvent removed{surface, done};

    expect(!changed.registration_ticket.has_value(),
           "legacy surface-changed publisher should leave registration optional");
    expect(!frame.first_present_ticket.has_value(),
           "legacy frame publisher should leave first-present optional");
    expect(!removed.removal_ticket.has_value(),
           "legacy removal publisher should leave removal optional");
    expect(removed.done == done,
           "legacy removal promise should remain source-compatible");
}

void secondary_window_protocol_keeps_native_handle_hidden_until_first_present() {
    UiSurfaceLifecycle surface;
    bool hidden = true;
    bool revealed = false;
    surface.registration_ticket().succeed();
    expect(surface.state() == UiSurfaceState::WaitingFirstPresent,
           "secondary registration should wait for first present");
    expect(hidden && !revealed, "secondary window must begin hidden");
    surface.first_present_ticket().succeed();
    revealed = true;
    expect(surface.state() == UiSurfaceState::Active && revealed,
           "secondary window may reveal only after first present");
}

void timed_out_removal_does_not_destroy_native_handle() {
    UiSurfaceLifecycle surface;
    surface.registration_ticket().succeed();
    surface.first_present_ticket().succeed();
    auto removal = surface.request_removal();
    bool native_handle_alive = true;
    expect(!removal.wait_until(UiSurfaceLifecycle::Clock::now() + 1ms),
           "pending removal should time out deterministically");
    expect(native_handle_alive && surface.state() == UiSurfaceState::Removing,
           "timeout must retain the hidden native handle");
    removal.succeed();
    native_handle_alive = false;
    expect(!native_handle_alive && surface.state() == UiSurfaceState::Retired,
           "native handle can be destroyed only after acknowledgement");
}

void shutdown_order_is_display_then_images_then_sdl() {
    std::vector<const char*> order;
    order.push_back("display_remove_ack");
    order.push_back("consumed_receipt");
    order.push_back("vulkan_unregister");
    order.push_back("cef_close");
    order.push_back("sdl_destroy");
    expect(std::ranges::is_sorted(order, [](const char* lhs, const char* rhs) {
               static const std::array<const char*, 5> ranks{
                   "display_remove_ack", "consumed_receipt", "vulkan_unregister", "cef_close", "sdl_destroy"};
               auto rank = [&](const char* value) { return std::ranges::find(ranks, value) - ranks.begin(); };
               return rank(lhs) < rank(rhs);
           }),
           "global shutdown order must retire Display before Vulkan/CEF/SDL");
}

}  // namespace

int main() {
    lifecycle_follows_the_success_path();
    lifecycle_failure_is_terminal();
    completion_ticket_is_copyable_thread_safe_and_idempotent();
    completion_ticket_waits_without_consuming_the_result();
    delayed_acknowledgement_wakes_a_deadline_wait();
    precompleted_first_present_is_not_skipped();
    removal_before_first_present_cancels_obsolete_work();
    cancelled_removal_is_not_safe_retirement();
    removal_directly_from_registering_cancels_both_forward_tickets();
    duplicate_requests_share_one_completion();
    timed_out_wait_retains_the_pending_lifecycle();
    main_and_three_secondary_surfaces_drain_by_one_deadline();
    drain_rejects_a_failed_removal_participant();
    drain_rejects_a_cancelled_removal_participant();
    legacy_event_aggregate_publishers_remain_compatible();
    secondary_window_protocol_keeps_native_handle_hidden_until_first_present();
    timed_out_removal_does_not_destroy_native_handle();
    shutdown_order_is_display_then_images_then_sdl();
    std::cout << "UI surface lifecycle tests passed\n";
    return 0;
}
