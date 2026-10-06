#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#include "corona/systems/display/display_callback_gate.h"
#include "corona/systems/display/display_system.h"
#include "corona/systems/display/surface_frame_coordinator.h"
#include "corona/systems/display/surface_lifecycle_acks.h"
#include "corona/systems/ui/ui_surface_lifecycle.h"

namespace {

using namespace std::chrono_literals;
using Corona::Systems::DisplaySystem;
using Corona::Systems::Detail::ForwardCompletionFence;
using Corona::Systems::Detail::OwnerCallbackGate;
using Corona::Systems::Detail::PresentOutcome;
using Corona::Systems::Detail::SurfaceFrameCoordinator;
using Corona::Systems::Detail::SurfaceLifecycleAcks;
using Corona::Systems::UI::DisplaySurfaceResult;
using Corona::Systems::UI::SurfaceCompletionTicket;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void expect(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

void expect_result(const SurfaceCompletionTicket& ticket,
                   DisplaySurfaceResult::Status status,
                   std::string_view message,
                   std::string_view failure_message) {
    const auto result = ticket.result();
    expect(result != nullptr, failure_message);
    expect(result->status == status, failure_message);
    expect(result->message == message, failure_message);
}

void acknowledgements_follow_registration_and_real_present_boundaries() {
    SurfaceLifecycleAcks acknowledgements;
    SurfaceCompletionTicket registration;
    SurfaceCompletionTicket first_present;
    acknowledgements.add_registration(registration);
    const auto first_present_boundary =
        acknowledgements.add_first_present(first_present);

    expect(!registration.is_ready(),
           "registration must remain pending before displayer construction");
    expect(!first_present.is_ready(),
           "first present must remain pending before compose/present");

    acknowledgements.registration_succeeded();
    expect_result(registration,
                  DisplaySurfaceResult::Status::Succeeded,
                  {},
                  "successful construction should acknowledge registration");
    expect(!first_present.is_ready(),
           "registration must not acknowledge the first present");

    acknowledgements.present_completed(PresentOutcome::Skipped,
                                       first_present_boundary);
    expect(!first_present.is_ready(),
           "a skipped compose/present must leave first present pending");

    acknowledgements.present_completed(PresentOutcome::Presented,
                                       first_present_boundary);
    expect_result(first_present,
                  DisplaySurfaceResult::Status::Succeeded,
                  {},
                  "a real compose/present should acknowledge first present");
}

void failed_present_terminates_first_present_instead_of_skipping_it() {
    SurfaceLifecycleAcks acknowledgements;
    SurfaceCompletionTicket first_present;
    const auto boundary = acknowledgements.add_first_present(first_present);

    acknowledgements.present_completed(PresentOutcome::Failed,
                                       boundary,
                                       "composite output creation failed");

    expect_result(first_present,
                  DisplaySurfaceResult::Status::Failed,
                  "composite output creation failed",
                  "a resource failure must terminate first present");
}

void first_present_only_completes_tickets_in_the_presented_snapshot() {
    SurfaceLifecycleAcks acknowledgements;
    SurfaceCompletionTicket captured_ticket;
    SurfaceCompletionTicket late_ticket;

    const auto captured_boundary =
        acknowledgements.add_first_present(captured_ticket);
    const auto late_boundary = acknowledgements.add_first_present(late_ticket);

    acknowledgements.present_completed(PresentOutcome::Presented,
                                       captured_boundary);
    expect(captured_ticket.is_ready(),
           "the ticket captured with a presented UI frame should complete");
    expect(!late_ticket.is_ready(),
           "a ticket added after the frame snapshot must remain pending");

    acknowledgements.present_completed(PresentOutcome::Presented,
                                       late_boundary);
    expect(late_ticket.is_ready(),
           "the late ticket should complete with a later UI snapshot");
}

void registration_and_present_failures_complete_forward_tickets() {
    SurfaceLifecycleAcks construction_failure;
    SurfaceCompletionTicket registration;
    SurfaceCompletionTicket first_present;
    construction_failure.add_registration(registration);
    (void)construction_failure.add_first_present(first_present);
    construction_failure.registration_failed(
        "HardwareDisplayer construction failed: test exception");

    expect_result(registration,
                  DisplaySurfaceResult::Status::Failed,
                  "HardwareDisplayer construction failed: test exception",
                  "construction failure should fail registration");
    expect_result(first_present,
                  DisplaySurfaceResult::Status::Failed,
                  "HardwareDisplayer construction failed: test exception",
                  "construction failure must not strand first present");

    SurfaceLifecycleAcks present_failure;
    SurfaceCompletionTicket failed_present;
    present_failure.registration_succeeded();
    (void)present_failure.add_first_present(failed_present);
    present_failure.present_failed("compose/present failed: test exception");
    expect_result(failed_present,
                  DisplaySurfaceResult::Status::Failed,
                  "compose/present failed: test exception",
                  "present exception should fail first present");
}

void device_loss_outcome_completes_tickets_attached_after_the_transition() {
    SurfaceLifecycleAcks acknowledgements;
    acknowledgements.fail_forward(
        "Display first present failed: Vulkan device lost");

    SurfaceCompletionTicket late_registration;
    SurfaceCompletionTicket late_first_present;
    acknowledgements.add_registration(late_registration);
    (void)acknowledgements.add_first_present(late_first_present);

    expect_result(late_registration,
                  DisplaySurfaceResult::Status::Failed,
                  "Display first present failed: Vulkan device lost",
                  "a registration attached after device loss must fail");
    expect_result(late_first_present,
                  DisplaySurfaceResult::Status::Failed,
                  "Display first present failed: Vulkan device lost",
                  "a first present attached after device loss must fail");
}

void removal_cancels_forward_work_and_waits_for_the_safe_point() {
    SurfaceLifecycleAcks acknowledgements;
    SurfaceCompletionTicket registration;
    SurfaceCompletionTicket first_present;
    SurfaceCompletionTicket removal;
    auto done = std::make_shared<std::promise<void>>();
    auto done_future = done->get_future();

    acknowledgements.add_registration(registration);
    acknowledgements.registration_succeeded();
    (void)acknowledgements.add_first_present(first_present);
    acknowledgements.removal_requested(removal, done);

    expect_result(first_present,
                  DisplaySurfaceResult::Status::Cancelled,
                  "surface removal requested",
                  "early removal should cancel first present");
    expect(!removal.is_ready(),
           "removal must remain pending before resource destruction");
    expect(done_future.wait_for(0ms) == std::future_status::timeout,
           "legacy done must remain pending before resource destruction");

    acknowledgements.removal_succeeded();
    expect_result(removal,
                  DisplaySurfaceResult::Status::Succeeded,
                  {},
                  "safe teardown should acknowledge removal");
    expect(done_future.wait_for(0ms) == std::future_status::ready,
           "legacy done should complete at the removal safe point");
}

void duplicate_and_late_removals_complete_idempotently() {
    SurfaceLifecycleAcks acknowledgements;
    SurfaceCompletionTicket first_removal;
    SurfaceCompletionTicket duplicate_removal;
    auto first_done = std::make_shared<std::promise<void>>();
    auto duplicate_done = std::make_shared<std::promise<void>>();
    auto first_future = first_done->get_future();
    auto duplicate_future = duplicate_done->get_future();

    acknowledgements.removal_requested(first_removal, first_done);
    acknowledgements.removal_requested(duplicate_removal, duplicate_done);
    acknowledgements.removal_succeeded();
    acknowledgements.removal_succeeded();

    expect_result(first_removal,
                  DisplaySurfaceResult::Status::Succeeded,
                  {},
                  "first removal should succeed once");
    expect_result(duplicate_removal,
                  DisplaySurfaceResult::Status::Succeeded,
                  {},
                  "duplicate removal should share the safe completion");
    expect(first_future.wait_for(0ms) == std::future_status::ready &&
               duplicate_future.wait_for(0ms) == std::future_status::ready,
           "all distinct legacy promises should complete once");

    SurfaceCompletionTicket late_removal;
    auto late_done = std::make_shared<std::promise<void>>();
    auto late_future = late_done->get_future();
    acknowledgements.removal_requested(late_removal, late_done);
    expect_result(late_removal,
                  DisplaySurfaceResult::Status::Succeeded,
                  {},
                  "already-closed surface removal should complete immediately");
    expect(late_future.wait_for(0ms) == std::future_status::ready,
           "already-closed legacy promise should complete immediately");
}

void shutdown_cancels_impossible_forward_work_without_certifying_removal_early() {
    SurfaceLifecycleAcks acknowledgements;
    SurfaceCompletionTicket registration;
    SurfaceCompletionTicket first_present;
    SurfaceCompletionTicket removal;
    auto done = std::make_shared<std::promise<void>>();
    auto done_future = done->get_future();

    acknowledgements.add_registration(registration);
    (void)acknowledgements.add_first_present(first_present);
    acknowledgements.removal_requested(removal, done);
    acknowledgements.cancel_forward("display shutdown before surface became ready");

    expect(registration.is_ready() && first_present.is_ready(),
           "shutdown should leave no forward ticket pending");
    expect(!removal.is_ready() &&
               done_future.wait_for(0ms) == std::future_status::timeout,
           "shutdown must not certify removal before resources are destroyed");

    acknowledgements.removal_succeeded();
    expect(removal.is_ready() &&
               done_future.wait_for(0ms) == std::future_status::ready,
           "shutdown should acknowledge removal after resource destruction");
}

void closed_forward_fence_defers_admitted_removal_completions() {
    auto fence = std::make_shared<ForwardCompletionFence>();
    SurfaceLifecycleAcks acknowledgements{fence};
    SurfaceCompletionTicket registration;
    SurfaceCompletionTicket first_present;
    SurfaceCompletionTicket removal;
    auto done = std::make_shared<std::promise<void>>();
    auto done_future = done->get_future();

    acknowledgements.add_registration(registration);
    (void)acknowledgements.add_first_present(first_present);
    fence->close();

    const bool ran_after_close = fence->run([&]() {
        acknowledgements.removal_requested(removal, done);
    });
    expect(!ran_after_close,
           "the shutdown fence must reject an admitted completion after close");
    acknowledgements.removal_requested(removal, done);

    expect(!registration.is_ready() && !first_present.is_ready() &&
               !removal.is_ready(),
           "closed-fence lifecycle tickets must wait for resource destruction");
    expect(done_future.wait_for(0ms) == std::future_status::timeout,
           "closed-fence legacy removal must wait for resource destruction");

    acknowledgements.cancel_forward(
        "Display shutdown before surface lifecycle completed");
    acknowledgements.removal_succeeded();
    expect_result(registration,
                  DisplaySurfaceResult::Status::Cancelled,
                  "Display shutdown before surface lifecycle completed",
                  "registration should cancel only after resource destruction");
    expect_result(first_present,
                  DisplaySurfaceResult::Status::Cancelled,
                  "Display shutdown before surface lifecycle completed",
                  "first present should cancel only after resource destruction");
    expect_result(removal,
                  DisplaySurfaceResult::Status::Succeeded,
                  {},
                  "removal should succeed only after resource destruction");
    expect(done_future.wait_for(0ms) == std::future_status::ready,
           "legacy removal should complete only after resource destruction");
}

void copied_callback_rejects_owner_access_and_defers_acknowledgements() {
    struct Owner {
        std::atomic<int> accesses{0};

        void touch() {
            accesses.fetch_add(1, std::memory_order_relaxed);
        }
    } owner;

    auto gate = std::make_shared<OwnerCallbackGate<Owner>>(owner);
    SurfaceCompletionTicket registration;
    SurfaceCompletionTicket removal;
    auto done = std::make_shared<std::promise<void>>();
    auto done_future = done->get_future();
    std::mutex mutex;
    std::condition_variable state_changed;
    bool callback_copied = false;
    bool invoke_callback = false;

    std::thread copied_callback([gate,
                                 registration,
                                 removal,
                                 done,
                                 &mutex,
                                 &state_changed,
                                 &callback_copied,
                                 &invoke_callback]() mutable {
        {
            std::lock_guard lock(mutex);
            callback_copied = true;
        }
        state_changed.notify_all();
        {
            std::unique_lock lock(mutex);
            state_changed.wait(lock, [&]() { return invoke_callback; });
        }

        auto access = gate->try_acquire();
        if (!access) {
            gate->defer_registration(registration);
            gate->defer_removal(removal, done);
            return;
        }
        access.owner().touch();
    });

    {
        std::unique_lock lock(mutex);
        state_changed.wait(lock, [&]() { return callback_copied; });
    }
    gate->close();
    gate->wait_for_quiescence();
    {
        std::lock_guard lock(mutex);
        invoke_callback = true;
    }
    state_changed.notify_all();
    copied_callback.join();

    expect(owner.accesses.load(std::memory_order_relaxed) == 0,
           "a copied callback starting after close must not touch its owner");
    expect(!registration.is_ready() && !removal.is_ready(),
           "closed callback acknowledgements must wait for resource destruction");
    expect(done_future.wait_for(0ms) == std::future_status::timeout,
           "closed legacy removal must wait for resource destruction");

    gate->complete_deferred_after_resources_destroyed(
        DisplaySurfaceResult::Status::Cancelled,
        "Display shutdown before callback ran");
    expect_result(registration,
                  DisplaySurfaceResult::Status::Cancelled,
                  "Display shutdown before callback ran",
                  "closed registration should cancel after destruction");
    expect_result(removal,
                  DisplaySurfaceResult::Status::Succeeded,
                  {},
                  "closed removal should succeed after destruction");
    expect(done_future.wait_for(0ms) == std::future_status::ready,
           "closed legacy removal should complete after destruction");
}

void callback_deferred_after_resource_drain_completes_immediately() {
    struct Owner {
    } owner;
    auto gate = std::make_shared<OwnerCallbackGate<Owner>>(owner);
    gate->close();
    gate->wait_for_quiescence();
    gate->complete_deferred_after_resources_destroyed(
        DisplaySurfaceResult::Status::Cancelled,
        "Display resources already destroyed");

    SurfaceCompletionTicket registration;
    SurfaceCompletionTicket first_present;
    SurfaceCompletionTicket removal;
    auto done = std::make_shared<std::promise<void>>();
    auto done_future = done->get_future();

    expect(!gate->try_acquire(),
           "a callback starting after resource drain must not access its owner");
    gate->defer_registration(registration);
    gate->defer_first_present(first_present);
    gate->defer_removal(removal, done);

    expect_result(registration,
                  DisplaySurfaceResult::Status::Cancelled,
                  "Display resources already destroyed",
                  "late registration must complete from the terminal gate state");
    expect_result(first_present,
                  DisplaySurfaceResult::Status::Cancelled,
                  "Display resources already destroyed",
                  "late first present must complete from the terminal gate state");
    expect_result(removal,
                  DisplaySurfaceResult::Status::Succeeded,
                  {},
                  "late removal must complete from the terminal gate state");
    expect(done_future.wait_for(0ms) == std::future_status::ready,
           "late legacy removal must complete from the terminal gate state");
}

void initialization_failure_is_preserved_for_late_callbacks() {
    struct Owner {
    } owner;
    auto gate = std::make_shared<OwnerCallbackGate<Owner>>(owner);
    gate->close();
    gate->wait_for_quiescence();
    gate->complete_deferred_after_resources_destroyed(
        DisplaySurfaceResult::Status::Failed,
        "Display initialization failed");

    SurfaceCompletionTicket registration;
    SurfaceCompletionTicket first_present;
    gate->defer_registration(registration);
    gate->defer_first_present(first_present);

    expect_result(registration,
                  DisplaySurfaceResult::Status::Failed,
                  "Display initialization failed",
                  "late registration must preserve initialization failure");
    expect_result(first_present,
                  DisplaySurfaceResult::Status::Failed,
                  "Display initialization failed",
                  "late first present must preserve initialization failure");
}

void callback_close_waits_for_an_active_owner_lease() {
    struct Owner {
    } owner;
    auto gate = std::make_shared<OwnerCallbackGate<Owner>>(owner);
    auto access = gate->try_acquire();
    expect(static_cast<bool>(access), "an open callback gate should grant access");

    gate->close();
    std::mutex mutex;
    std::condition_variable state_changed;
    bool waiter_started = false;
    bool quiescent = false;
    std::thread shutdown_waiter([&]() {
        {
            std::lock_guard lock(mutex);
            waiter_started = true;
        }
        state_changed.notify_all();
        gate->wait_for_quiescence();
        {
            std::lock_guard lock(mutex);
            quiescent = true;
        }
        state_changed.notify_all();
    });

    {
        std::unique_lock lock(mutex);
        state_changed.wait(lock, [&]() { return waiter_started; });
    }
    bool observed_quiescent = false;
    {
        std::lock_guard lock(mutex);
        observed_quiescent = quiescent;
    }
    expect(!observed_quiescent,
           "callback shutdown must wait while an owner lease is active");
    access = {};
    {
        std::unique_lock lock(mutex);
        state_changed.wait(lock, [&]() { return quiescent; });
    }
    shutdown_waiter.join();
}

void acknowledgement_races_are_terminal_and_idempotent() {
    SurfaceLifecycleAcks registration_acknowledgements;
    SurfaceCompletionTicket registration;
    std::barrier registration_start(3);
    std::thread attach_registration([&]() {
        registration_start.arrive_and_wait();
        registration_acknowledgements.add_registration(registration);
    });
    std::thread fail_registration([&]() {
        registration_start.arrive_and_wait();
        registration_acknowledgements.fail_forward("device lost");
    });
    registration_start.arrive_and_wait();
    attach_registration.join();
    fail_registration.join();
    expect_result(registration,
                  DisplaySurfaceResult::Status::Failed,
                  "device lost",
                  "registration attachment racing failure must terminate");

    SurfaceLifecycleAcks removal_acknowledgements;
    SurfaceCompletionTicket removal;
    auto done = std::make_shared<std::promise<void>>();
    auto done_future = done->get_future();
    std::barrier removal_start(3);
    std::thread request_removal([&]() {
        removal_start.arrive_and_wait();
        removal_acknowledgements.removal_requested(removal, done);
    });
    std::thread complete_removal([&]() {
        removal_start.arrive_and_wait();
        removal_acknowledgements.removal_succeeded();
    });
    removal_start.arrive_and_wait();
    request_removal.join();
    complete_removal.join();
    expect_result(removal,
                  DisplaySurfaceResult::Status::Succeeded,
                  {},
                  "removal request racing completion must succeed");
    expect(done_future.wait_for(0ms) == std::future_status::ready,
           "legacy removal racing completion must become ready");
}

void removal_invalidates_a_snapshot_captured_before_retirement() {
    struct FrameImages {};
    constexpr std::uint64_t surface_id = 0x1234;
    SurfaceFrameCoordinator coordinator;
    expect(coordinator.activate(surface_id),
           "a new surface should activate its frame coordinator");

    std::mutex mutex;
    std::condition_variable state_changed;
    bool snapshot_captured = false;
    bool resume_display = false;
    std::atomic<int> retired_image_accesses{0};

    std::thread display_frame([&]() {
        const auto snapshot = coordinator.capture(surface_id);
        auto frame_access = coordinator.begin_frame(
            snapshot,
            [&]() {
                {
                    std::lock_guard lock(mutex);
                    snapshot_captured = true;
                }
                state_changed.notify_all();
                std::unique_lock lock(mutex);
                state_changed.wait(lock, [&]() { return resume_display; });
            },
            [&]() {
                retired_image_accesses.fetch_add(1,
                                                 std::memory_order_relaxed);
                return FrameImages{};
            });
        expect(!frame_access,
               "a retired generation must reject the paused frame lease");
    });

    {
        std::unique_lock lock(mutex);
        state_changed.wait(lock, [&]() { return snapshot_captured; });
    }

    auto retirement = coordinator.retire(surface_id);

    {
        std::lock_guard lock(mutex);
        resume_display = true;
    }
    state_changed.notify_all();
    display_frame.join();

    expect(retired_image_accesses.load(std::memory_order_relaxed) == 0,
           "a retired generation must invalidate an already-captured snapshot");
}

void removal_acknowledgement_waits_for_an_acquired_frame_lease() {
    struct FrameImages {};
    constexpr std::uint64_t surface_id = 0x5678;
    SurfaceFrameCoordinator coordinator;
    expect(coordinator.activate(surface_id),
           "a new surface should activate its frame coordinator");

    auto frame_access = coordinator.begin_frame(
        coordinator.capture(surface_id), []() { return FrameImages{}; });
    expect(frame_access.has_value(),
           "an active snapshot should acquire frame access");

    using FrameAccess = typename decltype(frame_access)::value_type;
    static_assert(std::is_move_constructible_v<FrameAccess>);
    static_assert(!std::is_move_assignable_v<FrameAccess>);
    static_assert(!std::is_copy_constructible_v<FrameAccess>);
    static_assert(!std::is_copy_assignable_v<FrameAccess>);

    std::optional<FrameAccess> moved_frame_access{
        std::move(*frame_access)};
    expect(!static_cast<bool>(*frame_access),
           "moving frame access should empty the source");
    frame_access.reset();
    expect(static_cast<bool>(*moved_frame_access),
           "moving frame access should retain ownership");

    auto retirement = coordinator.retire(surface_id);
    SurfaceCompletionTicket removal_ack;
    std::barrier wait_entered(2);
    std::vector<std::string> teardown_order;

    std::thread display_teardown([&]() mutable {
        coordinator.teardown(
            surface_id,
            retirement,
            [&]() { wait_entered.arrive_and_wait(); },
            [&]() { teardown_order.push_back("displayer"); },
            [&]() { teardown_order.push_back("composite"); },
            [&]() {
                teardown_order.push_back("acknowledgement");
                removal_ack.succeed();
            });
    });

    wait_entered.arrive_and_wait();
    expect(!removal_ack.is_ready(),
           "removal acknowledgement must remain pending while a frame lease is active");
    expect(teardown_order.empty(),
           "resource erasure must not start while a frame lease is active");

    moved_frame_access.reset();
    display_teardown.join();

    const auto result = removal_ack.result();
    expect(result != nullptr &&
               result->status == DisplaySurfaceResult::Status::Succeeded,
           "removal should acknowledge after the final frame lease is released");
    expect(teardown_order ==
               std::vector<std::string>{"displayer",
                                        "composite",
                                        "acknowledgement"},
           "coordinator teardown must erase both resources before acknowledgement");
}

void acquisition_failure_destroys_images_before_releasing_the_lease() {
    struct TrackedHandle {
        std::atomic<int>* sequence = nullptr;
        std::atomic<int>* destroyed_order = nullptr;

        TrackedHandle(std::atomic<int>& next,
                      std::atomic<int>& destroyed) noexcept
            : sequence(&next), destroyed_order(&destroyed) {}
        TrackedHandle(const TrackedHandle&) = delete;
        TrackedHandle& operator=(const TrackedHandle&) = delete;
        TrackedHandle(TrackedHandle&& other) noexcept
            : sequence(std::exchange(other.sequence, nullptr)),
              destroyed_order(
                  std::exchange(other.destroyed_order, nullptr)) {}
        TrackedHandle& operator=(TrackedHandle&&) = delete;
        ~TrackedHandle() {
            if (sequence && destroyed_order) {
                destroyed_order->store(
                    sequence->fetch_add(1, std::memory_order_seq_cst) + 1,
                    std::memory_order_seq_cst);
            }
        }
    };
    struct FrameImages {
        TrackedHandle first;
    };

    constexpr std::uint64_t surface_id = 0x9abc;
    SurfaceFrameCoordinator coordinator;
    expect(coordinator.activate(surface_id),
           "the exception-order surface should activate");
    const auto snapshot = coordinator.capture(surface_id);

    std::barrier first_image_acquired(2);
    std::barrier allow_second_acquisition(2);
    std::barrier wait_entered(2);
    std::atomic<int> sequence{0};
    std::atomic<int> image_destroyed_order{0};
    std::atomic<int> displayer_erased_order{0};
    std::atomic<int> composite_erased_order{0};
    std::atomic<int> acknowledgement_order{0};
    std::atomic<bool> acquisition_failed{false};
    SurfaceCompletionTicket removal_ack;

    const auto record = [&sequence](std::atomic<int>& destination) {
        destination.store(
            sequence.fetch_add(1, std::memory_order_seq_cst) + 1,
            std::memory_order_seq_cst);
    };

    std::thread display_frame([&]() {
        try {
            (void)coordinator.begin_frame(
                snapshot,
                [&]() -> FrameImages {
                    FrameImages images{
                        TrackedHandle{sequence, image_destroyed_order}};
                    first_image_acquired.arrive_and_wait();
                    allow_second_acquisition.arrive_and_wait();
                    throw std::runtime_error("second image acquisition failed");
                });
        } catch (const std::runtime_error&) {
            acquisition_failed.store(true, std::memory_order_release);
        }
    });

    first_image_acquired.arrive_and_wait();
    auto retirement = coordinator.retire(surface_id);
    std::thread display_teardown([&]() {
        coordinator.teardown(
            surface_id,
            retirement,
            [&]() { wait_entered.arrive_and_wait(); },
            [&]() { record(displayer_erased_order); },
            [&]() { record(composite_erased_order); },
            [&]() {
                record(acknowledgement_order);
                removal_ack.succeed();
            });
    });

    wait_entered.arrive_and_wait();
    expect(image_destroyed_order.load(std::memory_order_seq_cst) == 0,
           "the first acquired image must remain owned while acquisition is paused");
    expect(!removal_ack.is_ready(),
           "teardown must wait for the acquisition lease");

    allow_second_acquisition.arrive_and_wait();
    display_frame.join();
    display_teardown.join();

    expect(acquisition_failed.load(std::memory_order_acquire),
           "the injected second acquisition must throw");
    expect(image_destroyed_order.load(std::memory_order_seq_cst) == 1,
           "an acquired image must be destroyed before the frame lease releases");
    expect(displayer_erased_order.load(std::memory_order_seq_cst) == 2 &&
               composite_erased_order.load(std::memory_order_seq_cst) == 3 &&
               acknowledgement_order.load(std::memory_order_seq_cst) == 4,
           "exception teardown must destroy images, displayer, and composite "
           "before acknowledgement");
}

void production_display_empty_update_is_linked() {
    DisplaySystem display;
    display.update();
    display.shutdown();
}

}  // namespace

int main() {
    acknowledgements_follow_registration_and_real_present_boundaries();
    failed_present_terminates_first_present_instead_of_skipping_it();
    first_present_only_completes_tickets_in_the_presented_snapshot();
    registration_and_present_failures_complete_forward_tickets();
    device_loss_outcome_completes_tickets_attached_after_the_transition();
    removal_cancels_forward_work_and_waits_for_the_safe_point();
    duplicate_and_late_removals_complete_idempotently();
    shutdown_cancels_impossible_forward_work_without_certifying_removal_early();
    closed_forward_fence_defers_admitted_removal_completions();
    copied_callback_rejects_owner_access_and_defers_acknowledgements();
    callback_deferred_after_resource_drain_completes_immediately();
    initialization_failure_is_preserved_for_late_callbacks();
    callback_close_waits_for_an_active_owner_lease();
    acknowledgement_races_are_terminal_and_idempotent();
    removal_invalidates_a_snapshot_captured_before_retirement();
    removal_acknowledgement_waits_for_an_acquired_frame_lease();
    acquisition_failure_destroys_images_before_releasing_the_lease();
    production_display_empty_update_is_linked();
    std::cout << "UI surface removal race tests passed\n";
    return 0;
}
