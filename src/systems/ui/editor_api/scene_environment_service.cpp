#include "scene_environment_service.h"

#include <algorithm>
#include <cmath>

namespace Corona::Systems::UI::SceneEnvironmentService {

namespace {

bool finite_vector(const std::array<float, 3>& value) {
    return std::all_of(value.begin(), value.end(), [](float x) { return std::isfinite(x); });
}

bool finite_scalar(float value) {
    return std::isfinite(value);
}

}  // namespace

std::string validate_patch(const EnvironmentPatch& patch) {
    const auto invalid = [](const std::optional<std::array<float, 3>>& value) {
        return value && !finite_vector(*value);
    };
    if ((!patch.sun && !patch.grid_enabled && !patch.physics) ||
        (patch.sun && (patch.sun->empty() || invalid(patch.sun->direction))) ||
        (patch.physics && (patch.physics->empty() || invalid(patch.physics->gravity) ||
                           (patch.physics->floor_y && !finite_scalar(*patch.physics->floor_y)) ||
                           (patch.physics->floor_restitution && !finite_scalar(*patch.physics->floor_restitution)) ||
                           (patch.physics->fixed_dt && !finite_scalar(*patch.physics->fixed_dt))))) {
        return "Invalid environment patch";
    }
    return {};
}

void apply_sun_and_grid(EnvironmentScalars& scalars, const EnvironmentPatch& patch) {
    if (patch.sun) {
        if (patch.sun->enabled) scalars.sun_enabled = *patch.sun->enabled;
        if (patch.sun->direction) scalars.sun_direction = *patch.sun->direction;
        const auto& direction = scalars.sun_direction;
        const double length_sq = static_cast<double>(direction[0]) * direction[0] +
            static_cast<double>(direction[1]) * direction[1] +
            static_cast<double>(direction[2]) * direction[2];
        if (scalars.sun_enabled && length_sq < 1.0e-8) scalars.sun_direction = {1, 1, 1};
    }
    if (patch.grid_enabled) scalars.grid_enabled = *patch.grid_enabled;
}

PhysicsUpdate physics_update(const EnvironmentPatch& patch) {
    PhysicsUpdate update;
    if (!patch.physics) return update;
    const auto& physics = *patch.physics;
    if (physics.gravity) update.gravity = physics.gravity;
    if (physics.floor_y) update.floor_y = physics.floor_y;
    if (physics.floor_restitution) update.floor_restitution = physics.floor_restitution;
    if (physics.fixed_dt) update.fixed_dt = physics.fixed_dt;
    return update;
}

}  // namespace Corona::Systems::UI::SceneEnvironmentService
