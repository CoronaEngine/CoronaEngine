#pragma once
#include <array>
#include <cstddef>
#include <vector>

namespace vision::inline reblur {
using PackedNrdInputs = std::array<std::vector<std::byte>, 6>;
using PackedNrdOutputs = std::array<std::vector<std::byte>, 2>;

// Non-jittered, column-major matrices consumed by the repository-local scheduler.
struct NativeCameraParams {
    std::array<float, 16> world_to_view{};
    std::array<float, 16> view_to_clip{};
    std::array<float, 16> world_to_view_prev{};
    std::array<float, 16> view_to_clip_prev{};
    std::array<float, 2> jitter{}, jitter_prev{};
    float frame_delta_ms{1000.f / 60.f};
    float disocclusion_threshold{.01f};
    float disocclusion_threshold_alternate{.1f};
    float denoising_range{100000.f};
};
}
