#include "shadow_culling.h"

#include <array>
#include <cmath>
#include <iostream>
#include <vector>

using namespace Corona::Systems::OpticsDetail;

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
    const auto view = make_shadow_cascade_view(
        ktm::fmat4x4::from_eye(), 20.0f, 10.0f, 1024.0f);
    ok &= expect(std::abs(view.world_units_per_texel - (20.0f / 1024.0f)) < 1.0e-6f,
                 "world-units-per-texel must derive from the largest cascade extent");

    Corona::Spatial::AABB inside;
    inside.min = {-0.5f, -0.5f, 0.1f};
    inside.max = {0.5f, 0.5f, 0.9f};
    ok &= expect(shadow_caster_visible(view.frustum, inside),
                 "inside caster must remain visible");

    Corona::Spatial::AABB outside;
    outside.min = {10.0f, 10.0f, 0.1f};
    outside.max = {11.0f, 11.0f, 0.9f};
    ok &= expect(!shadow_caster_visible(view.frustum, outside),
                 "outside caster must be culled");

    Corona::Spatial::AABB invalid;
    invalid.min = {NAN, 0.0f, 0.0f};
    invalid.max = {1.0f, 1.0f, 1.0f};
    ok &= expect(shadow_caster_visible(view.frustum, invalid),
                 "invalid caster bounds must use conservative visibility");

    const std::array<ShadowCascadeView, 4> cascades{view, view, view, view};
    const ShadowCasterBoundsSnapshot inside_snapshot{inside, true};
    ok &= expect(shadow_cascade_visibility_mask(inside_snapshot, cascades, 0b0101u) == 0b0101u,
                 "snapshot visibility must preserve the enabled cascade mask");
    const ShadowCasterBoundsSnapshot outside_snapshot{outside, true};
    ok &= expect(shadow_cascade_visibility_mask(outside_snapshot, cascades, 0b1111u) == 0u,
                 "snapshot visibility must cull an outside caster from every cascade");
    const ShadowCasterBoundsSnapshot invalid_snapshot{invalid, false};
    ok &= expect(shadow_cascade_visibility_mask(invalid_snapshot, cascades, 0b1010u) == 0b1010u,
                 "invalid snapshot bounds must remain visible in enabled cascades");

    std::vector<std::uint32_t> ordered_cascades;
    for_each_enabled_shadow_cascade(0b1101u, 4, [&](std::uint32_t cascade) {
        ordered_cascades.push_back(cascade);
    });
    ok &= expect(ordered_cascades == std::vector<std::uint32_t>{0, 2, 3},
                 "parallel cascade results must be appended in deterministic 0-to-3 order");

    ShadowSceneBounds scene_bounds;
    include_shadow_scene_bounds(scene_bounds, inside_snapshot);
    Corona::Spatial::AABB second;
    second.min = {-2.0f, -3.0f, -4.0f};
    second.max = {4.0f, 5.0f, 6.0f};
    include_shadow_scene_bounds(scene_bounds, {second, true});
    ok &= expect(scene_bounds.valid &&
                     scene_bounds.min_world.x == -2.0f &&
                     scene_bounds.min_world.y == -3.0f &&
                     scene_bounds.min_world.z == -4.0f &&
                     scene_bounds.max_world.x == 4.0f &&
                     scene_bounds.max_world.y == 5.0f &&
                     scene_bounds.max_world.z == 6.0f,
                 "scene bounds must be aggregated from immutable caster snapshots");

    std::array<float, 8> errors{0.0f, 0.05f, 0.15f, 0.4f, 0.8f, 1.0f, 1.0f, 1.0f};
    ok &= expect(select_shadow_lod_level(errors, 4, 1.0f, 0.2f, 0) == 2,
                 "far shadow cascade should select a coarser fitting LOD");
    ok &= expect(select_shadow_lod_level(errors, 4, 1.0f, 0.01f, 0) == 0,
                 "near shadow cascade should retain LOD0");
    ok &= expect(select_shadow_lod_level(errors, 0, 1.0f, 1.0f, 3) == 3,
                 "missing LOD metadata must retain the fallback");
    return ok ? 0 : 1;
}
