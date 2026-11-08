#pragma once

#include "cef/cef_scene_operations.h"

#include <array>
#include <optional>
#include <string>

namespace Corona::Systems::UI::SceneEnvironmentService {

// Editor-scene environment scalars that are not owned by the engine component.
struct EnvironmentScalars {
    bool sun_enabled = true;
    std::array<float, 3> sun_direction{1.f, 1.f, 1.f};
    bool grid_enabled = false;
};

// Physics values a SetEnvironment patch asks the caller to push into the
// engine environment component; unset members mean "keep the current value".
struct PhysicsUpdate {
    std::optional<std::array<float, 3>> gravity;
    std::optional<float> floor_y, floor_restitution, fixed_dt;
    bool empty() const { return !gravity && !floor_y && !floor_restitution && !fixed_dt; }
};

// Validates a SetEnvironment patch. Returns an empty string when valid,
// otherwise the message reported with INVALID_ARGUMENT.
std::string validate_patch(const EnvironmentPatch& patch);

// Applies sun/grid to the editor-owned scalars. A degenerate sun direction is
// replaced with {1,1,1} only while the sun is enabled, mirroring the native
// scene behaviour. Engine-side physics stays with the caller.
void apply_sun_and_grid(EnvironmentScalars& scalars, const EnvironmentPatch& patch);

// Extracts the physics fields present in the patch.
PhysicsUpdate physics_update(const EnvironmentPatch& patch);

}  // namespace Corona::Systems::UI::SceneEnvironmentService
