#include "native_frame_throttle.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <vector>

namespace {

Corona::Horizon::SubmitReceipt receipt(std::uint64_t serial) {
    Corona::Horizon::SubmitReceipt value;
    value.serial = serial;
    return value;
}

}  // namespace

// NativeFrameThrottle decides whether the Optics thread may record another native
// view.  Its capacity is released by Display's OpticsFrameConsumedEvent, which is
// only published for surfaces that were actually presented with an optics layer;
// every other exit (surface removed, no displayer, present skipped/failed, device
// lost) drops the acknowledgement.  Two dropped acknowledgements therefore used to
// stop native rendering for every camera, permanently and silently.  This pins the
// contract that capacity is always recoverable and that the stall is observable.
int main() {
    using Corona::Systems::OpticsDetail::NativeFrameThrottle;

    NativeFrameThrottle throttle;
    std::vector<std::uint64_t> waited;
    const auto wait = [&waited](const Corona::Horizon::SubmitReceipt& value) {
        waited.push_back(value.serial);
    };

    throttle.submitted(receipt(1));
    throttle.submitted(receipt(2));
    assert(throttle.in_flight_count() == 2);

    // A full queue must not invoke a blocking wait callback by itself: the caller
    // decides when to pay for capacity.
    assert(!throttle.try_acquire_slot());
    assert(throttle.in_flight_count() == 2);

    // Display acknowledgement retires the matching submitted frame without
    // waiting on the Optics thread, making capacity available again.
    throttle.acknowledge_consumed(1);
    assert(throttle.try_acquire_slot());
    assert(throttle.in_flight_count() == 1);

    throttle.submitted(receipt(3));
    assert(!throttle.try_acquire_slot());
    assert(waited.empty());
    assert(throttle.in_flight_count() == 2);

    // A stale acknowledgement cannot retire a newer frame.
    throttle.acknowledge_consumed(1);
    assert(throttle.in_flight_count() == 2);

    // Dropped acknowledgements: the caller retires the oldest submission by
    // waiting for its own GPU completion, which restores capacity instead of
    // leaving the throttle full forever.
    throttle.note_saturated_frame();
    assert(throttle.retire_oldest(wait));
    assert(waited == (std::vector<std::uint64_t>{2}));
    assert(throttle.in_flight_count() == 1);
    assert(throttle.try_acquire_slot());
    assert(throttle.released_without_ack() == 1);
    assert(throttle.saturated_frames() == 1);

    // The oldest entry is the one retired, and its age is observable.
    throttle.submitted(receipt(4));
    assert(throttle.oldest_serial() == 3);
    assert(throttle.oldest_age_ms(NativeFrameThrottle::Clock::now()) < 1000);
    assert(throttle.retire_oldest(wait));
    assert(waited == (std::vector<std::uint64_t>{2, 3}));
    assert(throttle.oldest_serial() == 4);

    // Retiring an empty throttle reports that nothing happened.
    throttle.drain(wait);
    assert(throttle.released_without_ack() == 3);
    assert(!throttle.retire_oldest(wait));
    assert(throttle.oldest_age_ms(NativeFrameThrottle::Clock::now()) == 0);
    assert(throttle.oldest_serial() == 0);

    // Submitting into a full throttle must never throw: an exception here escapes
    // update() and SystemBase treats that as fatal (it requests engine shutdown).
    throttle.submitted(receipt(10));
    throttle.submitted(receipt(11));
    assert(!throttle.try_acquire_slot());
    throttle.submitted(receipt(12));
    assert(throttle.in_flight_count() == 2);
    assert(throttle.forgotten_for_capacity() == 1);
    assert(throttle.oldest_serial() == 11);

    // A zero serial means "nothing was submitted" and must not consume capacity.
    throttle.submitted(receipt(0));
    assert(throttle.in_flight_count() == 2);

    return 0;
}
