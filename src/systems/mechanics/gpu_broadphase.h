#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Corona::Systems {

// Scene-scoped Vulkan sweep-and-prune. Only AABBs and compact pair indices
// cross the device boundary; triangle geometry remains in the collision backend.
class GpuBroadphasePipeline {
   public:
    struct Body {
        std::array<float, 3> min{}, max{};
    };
    struct Scene {
        std::span<const std::uint32_t> bodies;
    };
    using Pair = std::array<std::uint32_t, 2>;
    struct Options {
        // Bounds device output/readback memory independently of dense output.
        // Must be in [1, 2^24], fitting Vulkan's minimum storage-buffer range.
        std::uint32_t pair_page_capacity{1u << 20};
    };

    GpuBroadphasePipeline();
    explicit GpuBroadphasePipeline(Options options);
    ~GpuBroadphasePipeline();
    GpuBroadphasePipeline(const GpuBroadphasePipeline&) = delete;
    GpuBroadphasePipeline& operator=(const GpuBroadphasePipeline&) = delete;

    // Inclusive XYZ overlap. Returns sorted,
    // unique original body indices with the smaller index first, including when
    // a body or scene occurs repeatedly. Invalid input is rejected before any
    // submission; empty/single-member scenes require no dispatch. At most 2^21
    // bodies and 2^21 deduplicated memberships in nontrivial scenes are accepted;
    // an output count >= UINT32_MAX is explicitly rejected, never truncated.
    [[nodiscard]] std::vector<Pair> pairs(std::span<const Body> bodies, std::span<const Scene> scenes);
    [[nodiscard]] std::uint64_t submission_count() const noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace Corona::Systems
