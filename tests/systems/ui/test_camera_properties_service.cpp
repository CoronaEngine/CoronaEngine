// Camera 属性更新校验（WNC-013）：纯 DTO，无 CEF/引擎依赖。
// 注意：apply_native_camera_properties 被 tests/systems/ui/test_camera_view_update.py
// 直接抽取源码编译，修改本服务时必须同步该 harness 的 SERVICE 注入。
#include "editor_api/camera_properties_service.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace Corona::Systems::UI;
using namespace Corona::Systems::UI::CameraPropertiesService;

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
const std::array<float, 3> kProjection{1.f, 0.1f, 100.f};

CameraPropertiesPatch base_patch() {
    CameraPropertiesPatch patch;
    patch.scene = "scene.ini";
    patch.camera_id = "camera-main";
    return patch;
}

void test_view_lifecycle() {
    auto patch = base_patch();
    patch.view_open = true;
    auto error = validate_view_lifecycle(patch, true);
    check(error && error.code == "INVALID_ARGUMENT" &&
          error.message == "Camera window lifecycle belongs to window operations",
          "editor input rejects view lifecycle");
    check(!validate_view_lifecycle(patch, false), "non-editor input may drive the window lifecycle");
}

void test_identity() {
    auto empty = base_patch();
    auto error = validate_identity(empty, true);
    check(error && error.message == "Camera patch requires archive, scene, camera identity and properties",
          "empty patch rejected");

    auto no_camera = base_patch();
    no_camera.camera_id.clear();
    no_camera.name = "Renamed";
    check(static_cast<bool>(validate_identity(no_camera, true)), "missing camera identity rejected");

    auto no_archive = base_patch();
    no_archive.name = "Renamed";
    check(static_cast<bool>(validate_identity(no_archive, false)), "missing archive session rejected");

    auto ok = base_patch();
    ok.name = "Renamed";
    check(!validate_identity(ok, true), "identity with a field accepted");
}

void test_rename() {
    const std::vector<std::string> others{"Main", "Top"};
    check(validate_rename("Main", true, others).code == "READ_ONLY", "read-only rejects rename first");
    check(validate_rename("  ", false, others).message == "Camera name cannot be empty", "blank rename rejected");
    auto duplicate = validate_rename("  Top ", false, others);
    check(duplicate.code == "DUPLICATE_CAMERA_NAME" &&
          duplicate.message == "Camera name already exists: Top", "trimmed duplicate reported");
    check(!validate_rename("Third", false, others), "unique name accepted");
    check(normalized_name("  Third ") == "Third", "name normalized");
}

void test_numeric() {
    auto ok = base_patch();
    ok.fov = 60.f;
    check(!validate_numeric(ok, kProjection), "valid fov accepted");

    auto bad_fov = base_patch();
    bad_fov.fov = 180.f;
    check(validate_numeric(bad_fov, kProjection).message == "Invalid camera properties", "fov >= 180 rejected");
    bad_fov.fov = 0.f;
    check(static_cast<bool>(validate_numeric(bad_fov, kProjection)), "fov <= 0 rejected");

    auto slow = base_patch();
    slow.move_speed = 0.001f;
    check(static_cast<bool>(validate_numeric(slow, kProjection)), "too-small move speed rejected");

    auto zero_width = base_patch();
    zero_width.width = 0;
    check(static_cast<bool>(validate_numeric(zero_width, kProjection)), "non-positive width rejected");

    auto bad_vector = base_patch();
    bad_vector.position = std::array<float, 3>{0.f, kNaN, 0.f};
    check(static_cast<bool>(validate_numeric(bad_vector, kProjection)), "non-finite pose rejected");

    auto bad_sign = base_patch();
    bad_sign.projection_x_sign = 0.5f;
    check(static_cast<bool>(validate_numeric(bad_sign, kProjection)), "projection sign must be +/-1");

    // Unset fields fall back to the current projection, so a bogus projection
    // fails even when the patch does not touch projection fields.
    check(static_cast<bool>(validate_numeric(base_patch(), {1.f, 0.1f, 0.05f})), "inverted near/far rejected");

    auto bad_near = base_patch();
    bad_near.near_plane = 0.f;
    check(static_cast<bool>(validate_numeric(bad_near, kProjection)), "non-positive near plane rejected");
}

void test_editor_input() {
    auto pose = base_patch();
    pose.position = std::array<float, 3>{0.f, 1.f, 0.f};
    check(validate_editor_input(pose, true, false).code == "EDITOR_INPUT_LOCKED", "runtime owns camera input");
    check(!validate_editor_input(pose, true, true), "editor input allowed while enabled");
    check(!validate_editor_input(pose, false, false), "non-editor callers are not gated");
    auto name_only = base_patch();
    name_only.name = "Renamed";
    check(!validate_editor_input(name_only, true, false), "rename is not pose input");
}

}  // namespace

int main() {
    test_view_lifecycle();
    test_identity();
    test_rename();
    test_numeric();
    test_editor_input();
    std::cout << "camera_properties_service: " << checks << " checks passed\n";
    return 0;
}
