#pragma once
#include "math/basic_types.h"
#include "dsl/dsl.h"
#include <cstddef>
#include <cstdlib>

namespace vision::path_cost {
// Diagnostic API-level work, not a hardware traversal/instruction counter.
// No record is instantiated in an ordinary (disabled) shader AST.
struct alignas(16) PathWorkCounters {
    ocarina::uint4 trace{}; // closest requests, shadow requests, surface reconstruction calls, material builds/native loads
    ocarina::uint4 bsdf{};  // scatter samples, explicit direction F calls, explicit direction PDF calls, NEE candidate proposals
    ocarina::uint4 path{};  // full NEE samples entering visibility, accepted vertices, max accepted surface depth, delta scatters
    ocarina::uint4 rr{};    // RR decisions, scheduled RR deaths, rejected interior hits, reserved
};
static_assert(sizeof(PathWorkCounters) == 64u);
static_assert(alignof(PathWorkCounters) == 16u);
static_assert(offsetof(PathWorkCounters, trace) == 0u && offsetof(PathWorkCounters, bsdf) == 16u &&
              offsetof(PathWorkCounters, path) == 32u && offsetof(PathWorkCounters, rr) == 48u);
[[nodiscard]] inline bool enabled() noexcept {
    const char *value = std::getenv("VISION_PATH_WORK_OUTPUT");
    return value && *value;
}
}// namespace vision::path_cost
OC_STRUCT(vision::path_cost, PathWorkCounters, trace, bsdf, path, rr) {};
