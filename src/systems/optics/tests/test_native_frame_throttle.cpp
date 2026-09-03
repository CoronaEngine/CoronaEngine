#include "../native_frame_throttle.h"

#include <cassert>
#include <vector>

namespace {

Corona::Horizon::SubmitReceipt receipt(std::uint64_t serial) {
    Corona::Horizon::SubmitReceipt value;
    value.serial = serial;
    return value;
}

}  // namespace

int main() {
    // Note: wait_for_completion removed in new Horizon API
    // This test validates NativeFrameThrottle logic only

    Corona::Systems::OpticsDetail::NativeFrameThrottle throttle;
    std::vector<std::uint64_t> waited;
    const auto wait = [&waited](const Corona::Horizon::SubmitReceipt& value) {
        waited.push_back(value.serial);
    };

    throttle.submitted(receipt(1));
    throttle.submitted(receipt(2));
    assert(throttle.in_flight_count() == 2);

    // A full queue must not invoke a blocking wait callback. The caller skips
    // this frame and retries on a later update.
    bool waited_for_full_queue = false;
    assert(!throttle.try_acquire_slot());
    assert(throttle.in_flight_count() == 2);
    waited_for_full_queue = true;
    assert(waited_for_full_queue);

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

    throttle.drain(wait);
    assert(waited == (std::vector<std::uint64_t>{2, 3}));
    assert(throttle.in_flight_count() == 0);
    return 0;
}
