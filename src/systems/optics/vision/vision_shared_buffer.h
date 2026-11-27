#pragma once

#include <horizon.h>

namespace ocarina { class Device; }

namespace Corona::Systems::Vision {

// Both devices reference the dedicated allocation owned by the Vision buffer.
Horizon::HardwareBuffer import_vision_buffer(ocarina::Device& device, std::uint64_t buffer,
                                             Horizon::HardwareBufferDesc desc);

}  // namespace Corona::Systems::Vision
