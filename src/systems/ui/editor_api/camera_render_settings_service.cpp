#include "camera_render_settings_service.h"

#include <algorithm>
#include <array>

namespace Corona::Systems::UI::CameraRenderSettingsService {

ValidationError validate_patch(const CameraRenderSettingsPatch& patch,
                               SceneMethod method,
                               bool has_archive_session) {
    if (patch.scene.empty() || !has_archive_session ||
        (method != SceneMethod::ReadCameraRenderSettings && patch.empty()) ||
        (method == SceneMethod::ReadCameraRenderSettings && !patch.family)) {
        return {"INVALID_ARGUMENT", "Camera render settings require archive, scene, camera and a non-empty patch"};
    }
    if (patch.family && *patch.family != "pt" && *patch.family != "restir" && *patch.family != "reblur" && *patch.family != "relax") {
        return {"INVALID_ARGUMENT", "Unsupported Vision settings family"};
    }
    if (patch.render_backend && *patch.render_backend != "native" && *patch.render_backend != "vision") {
        return {"INVALID_ARGUMENT", "Unsupported render backend"};
    }
    static const std::array output_modes{"final_color", "base_color", "normal", "position", "object_id",
        "visibility_buffer", "ssao", "ssao_raw", "shadow_mask_raw", "shadow_mask"};
    if (patch.output_mode && std::find(output_modes.begin(), output_modes.end(), *patch.output_mode) == output_modes.end()) {
        return {"INVALID_ARGUMENT", "Unsupported camera output mode"};
    }
    static const std::array vision_modes{"path_tracing", "restir", "progressive_path_tracing", "svgf", "ssat"};
    if (patch.vision_render_mode && std::find(vision_modes.begin(), vision_modes.end(), *patch.vision_render_mode) == vision_modes.end()) {
        return {"INVALID_ARGUMENT", "Unsupported Vision render mode"};
    }
    if (patch.vision_denoiser && *patch.vision_denoiser != "scene" && *patch.vision_denoiser != "svgf" &&
        *patch.vision_denoiser != "reblur" && *patch.vision_denoiser != "relax") {
        return {"INVALID_ARGUMENT", "Unsupported Vision denoiser"};
    }
    return {};
}

}  // namespace Corona::Systems::UI::CameraRenderSettingsService
