#pragma once

#include <cstdint>
#include <horizon.h>
#include "gpu_submission_batch.h"

namespace Corona::Systems {

// Device state: six tightly packed floats per body (vx,vy,vz,wx,wy,wz).
// The producer owns/reuses the buffer. Consume before its next solve, on the
// same CPU thread; retaining this view does not create a state snapshot.
// A pending readiness token requires recording the consumer in the same batch.
struct GpuRigidVelocityView {
    Horizon::HardwareBuffer buffer;
    std::uint32_t count{};
    std::shared_ptr<GpuSubmissionState> readiness;
};

// Explicit writable destination for projected velocities. Keep it alive until
// the consumer completes; it must not alias that consumer's velocity source.
struct GpuRigidVelocityTarget {
    Horizon::HardwareBuffer buffer;
    std::uint32_t count{};
    std::shared_ptr<GpuSubmissionState> readiness;
};

}  // namespace Corona::Systems
