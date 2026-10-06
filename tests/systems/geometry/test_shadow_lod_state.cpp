#include "shadow_lod_state.h"

#include <array>
#include <iostream>

using namespace Corona::Systems::GeometryDetail;

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    bool ok = true;
    const std::array<float, 8> errors{0.0f, 0.05f, 0.2f, 0.8f};
    ok &= expect(choose_shadow_target(errors, 4, 1.0f, 0.2f, 0) == 2,
                 "single shadow target should select the coarsest fitting level");
    ok &= expect(choose_shadow_target(errors, 4, 1.0f, 0.01f, 0) == 0,
                 "single shadow target should retain LOD0 for a tight budget");

    const ShadowLodState state{2, 1, true, 100};
    const auto live = decide_shadow_lod(state, 220, 0, 3, 4);
    ok &= expect(!live.expired && live.target == 2 && live.keep_level(1),
                 "shadow demand must remain live through its TTL");
    const auto expired = decide_shadow_lod(state, 221, 0, 3, 4);
    ok &= expect(expired.expired && expired.target == -1,
                 "shadow demand must expire on frame 121");
    ok &= expect(expired.keep_level(0) && expired.keep_level(3),
                 "main residency guards must survive shadow expiry");

    const std::array<ShadowLodQueryInput, 4> queries{{
        {true, 0.8f},
        {true, 0.2f},
        {false, 0.01f},
        {true, 0.01f},
    }};
    std::array<int, 4> targets{};
    const auto batch = choose_shadow_targets(errors, 4, 1.0f, queries, targets, 1);
    ok &= expect(targets == std::array<int, 4>{3, 2, -1, 0},
                 "batch query must preserve each enabled cascade target");
    ok &= expect(batch.aggregated_demand == 0,
                 "batch query must aggregate cascades to the highest precision target");

    const std::array<ShadowLodQueryInput, 2> disabled_or_invalid{{
        {false, 0.2f},
        {true, 0.0f},
    }};
    std::array<int, 2> fallback_targets{};
    const auto no_demand = choose_shadow_targets(
        errors, 4, 1.0f, disabled_or_invalid, fallback_targets, 2);
    ok &= expect(fallback_targets == std::array<int, 2>{-1, 2},
                 "disabled cascades must be omitted and invalid queries must use main fallback");
    ok &= expect(no_demand.aggregated_demand == -1,
                 "disabled or invalid cascades must not register shadow demand");

    return ok ? 0 : 1;
}
