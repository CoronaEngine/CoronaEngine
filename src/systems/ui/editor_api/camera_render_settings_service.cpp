#include "editor_api_services.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

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
    static const std::array vision_modes{"path_tracing", "restir", "progressive_path_tracing", "svgf", "ssat"};
    if (patch.vision_render_mode && std::find(vision_modes.begin(), vision_modes.end(), *patch.vision_render_mode) == vision_modes.end()) {
        return {"INVALID_ARGUMENT", "Unsupported Vision render mode"};
    }
    if (patch.vision_denoiser && *patch.vision_denoiser != "scene" && *patch.vision_denoiser != "svgf" &&
        *patch.vision_denoiser != "reblur" && *patch.vision_denoiser != "relax") {
        return {"INVALID_ARGUMENT", "Unsupported Vision denoiser"};
    }
    if (patch.family && patch.settings.is_object() && !patch.settings.empty()) {
        if (const auto invalid = validate_media_flags(patch.settings)) return invalid;
    }
    return {};
}

ValidationError validate_media_flags(const nlohmann::json& settings) {
    if (!settings.is_object()) return {};
    for (const auto* key : {"participating_media", "medium_diagnostics"}) {
        if (settings.contains(key) && !settings.at(key).is_boolean()) {
            return {"INVALID_ARGUMENT", std::string{"PT "} + key + " must be a boolean"};
        }
    }
    return {};
}

ValidationError validate_archive_pt_settings(const nlohmann::json& settings) {
    if (!settings.is_object()) return {"INVALID_ARGUMENT", "Expected PT settings object"};
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        if (it.key() == "participating_media" || it.key() == "medium_diagnostics") {
            if (!it->is_boolean()) return {"INVALID_ARGUMENT", "PT " + it.key() + " must be a boolean"};
            continue;
        }
        const double maximum = it.key() == "max_depth" ? 128 : it.key() == "min_depth" ? 20 : 1;
        if ((it.key() != "max_depth" && it.key() != "min_depth" && it.key() != "rr_threshold") || !it->is_number()) {
            return {"INVALID_ARGUMENT", "Invalid PT parameter: " + it.key()};
        }
        const double value = it->get<double>();
        if (!std::isfinite(value) || value < 0 || value > maximum ||
            (it.key() != "rr_threshold" && value != std::floor(value))) {
            return {"INVALID_ARGUMENT", "PT parameter out of range: " + it.key()};
        }
    }
    return {};
}

}  // namespace Corona::Systems::UI::CameraRenderSettingsService
