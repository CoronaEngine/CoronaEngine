// 环境域决策逻辑（WNC-013）：纯 DTO 输入，无 CEF / 无引擎单例依赖。
#include "editor_api/editor_api_services.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace Corona::Systems::UI;
using namespace Corona::Systems::UI::SceneEnvironmentService;

namespace {

int checks = 0;

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

EnvironmentPatch sun_patch(std::optional<bool> enabled = std::nullopt,
                           std::optional<std::array<float, 3>> direction = std::nullopt) {
    EnvironmentPatch patch;
    EnvironmentSunPatch sun;
    sun.enabled = enabled;
    sun.direction = direction;
    patch.sun = sun;
    return patch;
}

void test_validation() {
    EnvironmentPatch empty;
    check(!validate_patch(empty).empty(), "empty patch rejected");

    check(!validate_patch(sun_patch()).empty(), "empty sun rejected");
    check(!validate_patch(sun_patch(std::nullopt, std::array<float, 3>{kNaN, 0.f, 0.f})).empty(),
          "non-finite sun direction rejected");
    check(validate_patch(sun_patch(false)).empty(), "sun toggle alone accepted");

    EnvironmentPatch grid;
    grid.grid_enabled = false;
    check(validate_patch(grid).empty(), "explicit false grid accepted");

    EnvironmentPatch empty_physics;
    empty_physics.physics = EnvironmentPhysicsPatch{};
    check(!validate_patch(empty_physics).empty(), "empty physics rejected");

    EnvironmentPatch bad_gravity;
    EnvironmentPhysicsPatch gravity;
    gravity.gravity = std::array<float, 3>{0.f, kNaN, 0.f};
    bad_gravity.physics = gravity;
    check(!validate_patch(bad_gravity).empty(), "non-finite gravity rejected");

    EnvironmentPatch nan_floor;
    EnvironmentPhysicsPatch floor;
    floor.floor_y = kNaN;
    nan_floor.physics = floor;
    check(!validate_patch(nan_floor).empty(), "non-finite floor_y rejected");

    EnvironmentPatch valid_floor;
    EnvironmentPhysicsPatch floor_ok;
    floor_ok.floor_y = 0.f;
    valid_floor.physics = floor_ok;
    check(validate_patch(valid_floor).empty(), "finite floor_y accepted");
}

void test_sun_and_grid_application() {
    EnvironmentScalars state;
    state.sun_enabled = true;
    state.sun_direction = {0.f, 1.f, 0.f};
    state.grid_enabled = true;

    apply_sun_and_grid(state, sun_patch(false));
    check(state.sun_enabled == false, "sun disabled");
    check((state.sun_direction == std::array<float, 3>{0.f, 1.f, 0.f}), "direction untouched when absent");

    EnvironmentScalars promoted;
    promoted.sun_enabled = true;
    promoted.sun_direction = {0.f, 1.f, 0.f};
    apply_sun_and_grid(promoted, sun_patch(std::nullopt, std::array<float, 3>{0.f, 0.f, 0.f}));
    check((promoted.sun_direction == std::array<float, 3>{1.f, 1.f, 1.f}), "degenerate direction promoted to {1,1,1}");

    EnvironmentScalars disabled;
    disabled.sun_enabled = true;
    disabled.sun_direction = {0.f, 0.f, 0.f};
    apply_sun_and_grid(disabled, sun_patch(false, std::array<float, 3>{0.f, 0.f, 0.f}));
    check((disabled.sun_direction == std::array<float, 3>{0.f, 0.f, 0.f}),
          "degenerate direction kept while the sun is disabled");

    EnvironmentPatch grid;
    grid.grid_enabled = false;
    apply_sun_and_grid(state, grid);
    check(state.grid_enabled == false, "grid toggle applied");
    apply_sun_and_grid(state, EnvironmentPatch{});
    check(state.grid_enabled == false && state.sun_enabled == false, "absent patch keeps state");
}

void test_physics_extraction() {
    EnvironmentPatch empty;
    check(physics_update(empty).empty(), "absent physics yields empty update");

    EnvironmentPatch full;
    EnvironmentPhysicsPatch physics;
    physics.gravity = std::array<float, 3>{0.f, -9.8f, 0.f};
    physics.floor_y = 2.5f;
    full.physics = physics;
    const auto update = physics_update(full);
    check(update.gravity && *update.gravity == (std::array<float, 3>{0.f, -9.8f, 0.f}), "gravity extracted");
    check(update.floor_y && *update.floor_y == 2.5f, "floor_y extracted");
    check(!update.floor_restitution && !update.fixed_dt, "absent fields stay unset");

    EnvironmentPatch restitution;
    EnvironmentPhysicsPatch bounce;
    bounce.floor_restitution = 0.f;
    restitution.physics = bounce;
    const auto extracted = physics_update(restitution);
    check(extracted.floor_restitution && *extracted.floor_restitution == 0.f, "zero value still extracted");
    check(!extracted.empty(), "non-empty update detected");
}

}  // namespace

int main() {
    test_validation();
    test_sun_and_grid_application();
    test_physics_extraction();
    std::cout << "scene_environment_service: " << checks << " checks passed\n";
    return 0;
}
