#pragma once

#include "horizon.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>

namespace Corona::Systems::OpticsDetail {

/**
 * Keeps the native renderer from recording an unbounded queue of stale views.
 *
 * Capacity is normally released by Display's per-surface
 * `OpticsFrameConsumedEvent`, but that event is only published when a surface was
 * actually presented with an optics layer.  Every other exit skips it (surface
 * removed, no displayer, present skipped or failed, device lost), so two dropped
 * acknowledgements used to park this throttle forever: `update()` returned
 * before recording anything, silently, for *every* camera at once, while the UI
 * kept running.  The throttle therefore also offers retirement by waiting for the
 * submission itself (`retire_oldest`), and it counts saturation so the caller can
 * report it instead of dropping frames invisibly.
 */
class NativeFrameThrottle {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t kMaxInFlight = 2;

    [[nodiscard]] bool try_acquire_slot() const noexcept {
        return in_flight_.size() < kMaxInFlight;
    }

    /// Display reported that every submission up to `serial` has been consumed.
    void acknowledge_consumed(std::uint64_t serial) noexcept {
        while (!in_flight_.empty() && in_flight_.front().receipt.serial <= serial) {
            in_flight_.pop_front();
        }
    }

    /**
     * Retire the oldest in-flight submission after `wait_for_completion` returns.
     *
     * Used when no acknowledgement arrived, so the caller can wait for the
     * submission's own GPU completion and keep rendering instead of stalling.
     *
     * @param wait_for_completion invoked with the oldest `SubmitReceipt`
     * @return false when nothing was in flight
     */
    template <typename WaitForCompletion>
    bool retire_oldest(WaitForCompletion&& wait_for_completion) {
        if (in_flight_.empty()) return false;
        std::forward<WaitForCompletion>(wait_for_completion)(in_flight_.front().receipt);
        in_flight_.pop_front();
        ++released_without_ack_;
        return true;
    }

    template <typename WaitForCompletion>
    void retire_one(WaitForCompletion&& wait_for_completion) {
        (void)retire_oldest(std::forward<WaitForCompletion>(wait_for_completion));
    }

    /**
     * Track a submitted frame.
     *
     * Deliberately does not throw: this used to raise `std::logic_error` when
     * capacity had not been made available, and an exception escaping `update()`
     * is treated as fatal by `SystemBase` (it requests engine shutdown).  The
     * oldest entry is forgotten instead, and counted for diagnostics.
     */
    bool submitted(Corona::Horizon::SubmitReceipt receipt) {
        if (receipt.serial == 0) return true;
        while (in_flight_.size() >= kMaxInFlight) {
            in_flight_.pop_front();
            ++forgotten_for_capacity_;
        }
        in_flight_.push_back(Entry{std::move(receipt), Clock::now()});
        return true;
    }

    template <typename WaitForCompletion>
    void drain(WaitForCompletion&& wait_for_completion) {
        while (!in_flight_.empty()) {
            retire_oldest(std::forward<WaitForCompletion>(wait_for_completion));
        }
    }

    [[nodiscard]] std::size_t in_flight_count() const noexcept {
        return in_flight_.size();
    }

    [[nodiscard]] std::uint64_t oldest_serial() const noexcept {
        return in_flight_.empty() ? 0 : in_flight_.front().receipt.serial;
    }

    /// Age of the oldest in-flight submission (0 when the throttle is idle).
    [[nodiscard]] std::uint64_t oldest_age_ms(Clock::time_point now) const noexcept {
        if (in_flight_.empty()) return 0;
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now - in_flight_.front().submitted_at)
                             .count();
        return age > 0 ? static_cast<std::uint64_t>(age) : 0;
    }

    /// Frames that found the throttle full and had to make capacity themselves.
    void note_saturated_frame() noexcept { ++saturated_frames_; }
    [[nodiscard]] std::uint64_t saturated_frames() const noexcept { return saturated_frames_; }
    /// Submissions retired by completion because no acknowledgement arrived.
    [[nodiscard]] std::uint64_t released_without_ack() const noexcept { return released_without_ack_; }
    /// Submissions dropped at submit time because capacity was never released.
    [[nodiscard]] std::uint64_t forgotten_for_capacity() const noexcept { return forgotten_for_capacity_; }

private:
    struct Entry {
        Corona::Horizon::SubmitReceipt receipt;
        Clock::time_point submitted_at{};
    };

    std::deque<Entry> in_flight_;
    std::uint64_t saturated_frames_{0};
    std::uint64_t released_without_ack_{0};
    std::uint64_t forgotten_for_capacity_{0};
};

}  // namespace Corona::Systems::OpticsDetail
