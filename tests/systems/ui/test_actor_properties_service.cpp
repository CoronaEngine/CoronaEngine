// updateActor 校验逻辑（WNC-013）：纯 DTO，无 CEF/引擎依赖。
// 这些规则保证"混合无效 patch 绝不部分应用"（计划 §7）。
#include "editor_api/actor_properties_service.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace Corona::Systems::UI;
using namespace Corona::Systems::UI::ActorPropertiesService;

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

ActorPropertiesPatch base_patch() {
    ActorPropertiesPatch patch;
    patch.scene = "scene.ini";
    patch.actor = "Hero";
    return patch;
}

void test_patch_validation() {
    ActorPropertiesPatch empty;
    auto error = validate_patch(empty);
    check(error && error.code == "INVALID_ARGUMENT" && error.message == "Actor property patch is empty",
          "missing actor identity is an empty patch");

    auto identity_only = base_patch();
    error = validate_patch(identity_only);
    check(error && error.message == "Actor property patch is empty", "no fields is an empty patch");

    auto visible = base_patch();
    visible.visible = false;
    check(!validate_patch(visible), "explicit false visibility accepted");

    auto mechanics = base_patch();
    mechanics.mechanics = ActorMechanicsPatch{};
    error = validate_patch(mechanics);
    check(error && error.message == "Invalid Actor mechanics patch", "empty mechanics rejected");

    auto bad_mass = base_patch();
    ActorMechanicsPatch mass;
    mass.mass = kNaN;
    bad_mass.mechanics = mass;
    check(static_cast<bool>(validate_patch(bad_mass)), "non-finite mass rejected");

    auto bad_body = base_patch();
    ActorMechanicsPatch body;
    body.body_type = "balloon";
    bad_body.mechanics = body;
    check(static_cast<bool>(validate_patch(bad_body)), "unknown body type rejected");

    for (const char* type : {"dynamic", "kinematic", "static", "phantom"}) {
        auto good = base_patch();
        ActorMechanicsPatch value;
        value.body_type = type;
        good.mechanics = value;
        check(!validate_patch(good), "supported body type accepted");
    }

    auto bad_lock = base_patch();
    ActorCameraLockPatch lock;
    lock.enabled = true;
    lock.position_offset = std::array<float, 3>{0.f, kNaN, 0.f};
    bad_lock.camera_lock = lock;
    error = validate_patch(bad_lock);
    check(error && error.message == "Camera offsets must be finite", "non-finite camera offset rejected");

    auto good_lock = base_patch();
    good_lock.camera_lock = ActorCameraLockPatch{};
    good_lock.animation_sources = std::vector<std::string>{"idle"};
    check(!validate_patch(good_lock), "lock and animation sources accepted together");
}

void test_rename_validation() {
    const std::vector<std::string> others{"Other", "Hero"};

    auto error = validate_rename("Hero", true, others);
    check(error && error.code == "READ_ONLY", "read-only rejects rename first");
    error = validate_rename("", true, others);
    check(error && error.code == "READ_ONLY", "read-only precedes the empty-name check");

    error = validate_rename("   ", false, others);
    check(error && error.code == "INVALID_ARGUMENT" && error.message == "Actor name cannot be empty",
          "blank rename rejected");

    error = validate_rename("  Hero  ", false, others);
    check(error && error.code == "DUPLICATE_ACTOR_NAME" &&
          error.message == "Actor name already exists: Hero", "trimmed duplicate reported with trimmed name");

    check(!validate_rename("  Fresh  ", false, others), "unique trimmed name accepted");
    check(normalized_name("  Fresh  ") == "Fresh", "normalized name is trimmed");
    check(normalized_name(" \t ") == "", "whitespace-only name normalizes to empty");
}

}  // namespace

int main() {
    test_patch_validation();
    test_rename_validation();
    std::cout << "actor_properties_service: " << checks << " checks passed\n";
    return 0;
}
