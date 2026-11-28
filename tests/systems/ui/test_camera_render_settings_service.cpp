// Camera 渲染设置校验（WNC-013）：纯 DTO 白名单与优先级，无 CEF/引擎依赖。
#include "editor_api/editor_api_services.h"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace Corona::Systems::UI;
using CameraRenderSettingsService::validate_patch;

namespace {

int checks = 0;

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_identity() {
    CameraRenderSettingsPatch empty{};
    auto error = validate_patch(empty, SceneMethod::SetCameraRenderSettings, true);
    check(error && error.code == "INVALID_ARGUMENT" &&
          error.message == "Camera render settings require archive, scene, camera and a non-empty patch",
          "empty patch rejected");

    CameraRenderSettingsPatch no_scene{};
    no_scene.vision_render_mode = "restir";
    check(static_cast<bool>(validate_patch(no_scene, SceneMethod::SetCameraRenderSettings, true)),
          "missing scene rejected");

    CameraRenderSettingsPatch no_archive{};
    no_archive.scene = "scene.ini";
    no_archive.vision_render_mode = "restir";
    check(static_cast<bool>(validate_patch(no_archive, SceneMethod::SetCameraRenderSettings, false)),
          "missing archive rejected");

    CameraRenderSettingsPatch read{};
    read.scene = "scene.ini";
    check(validate_patch(read, SceneMethod::ReadCameraRenderSettings, true).message ==
          "Camera render settings require archive, scene, camera and a non-empty patch",
          "read without family rejected");
    read.family = "pt";
    check(!validate_patch(read, SceneMethod::ReadCameraRenderSettings, true), "read with family accepted");
}

void test_whitelists() {
    CameraRenderSettingsPatch patch{};
    patch.scene = "scene.ini";
    for (const char* family : {"pt", "restir", "reblur", "relax"}) {
        auto value = patch;
        value.family = family;
        value.settings = {{"x", 1}};
        check(!validate_patch(value, SceneMethod::SetCameraRenderSettings, true), "known family accepted");
    }
    auto bad_family = patch;
    bad_family.family = "foo";
    bad_family.settings = {{"x", 1}};
    check(validate_patch(bad_family, SceneMethod::SetCameraRenderSettings, true).message ==
          "Unsupported Vision settings family", "unknown family rejected");

    for (const char* mode : {"path_tracing", "restir", "progressive_path_tracing", "svgf", "ssat"}) {
        auto value = patch;
        value.vision_render_mode = mode;
        check(!validate_patch(value, SceneMethod::SetCameraRenderSettings, true), "known vision mode accepted");
    }
    auto bad_vision = patch;
    bad_vision.vision_render_mode = "pt";
    check(validate_patch(bad_vision, SceneMethod::SetCameraRenderSettings, true).message ==
          "Unsupported Vision render mode", "unknown vision mode rejected");

    for (const char* denoiser : {"scene", "svgf", "reblur", "relax"}) {
        auto value = patch;
        value.vision_denoiser = denoiser;
        check(!validate_patch(value, SceneMethod::SetCameraRenderSettings, true), "known denoiser accepted");
    }
    auto bad_denoiser = patch;
    bad_denoiser.vision_denoiser = "oidn";
    check(validate_patch(bad_denoiser, SceneMethod::SetCameraRenderSettings, true).message ==
          "Unsupported Vision denoiser", "unknown denoiser rejected");

    // Family validation keeps priority over the later whitelists.
    auto both_bad = patch;
    both_bad.family = "foo";
    both_bad.settings = {{"x", 1}};
    both_bad.vision_render_mode = "invalid";
    check(validate_patch(both_bad, SceneMethod::SetCameraRenderSettings, true).message ==
          "Unsupported Vision settings family", "family error precedes render mode error");
}

}  // namespace

int main() {
    test_identity();
    test_whitelists();
    std::cout << "camera_render_settings_service: " << checks << " checks passed\n";
    return 0;
}
