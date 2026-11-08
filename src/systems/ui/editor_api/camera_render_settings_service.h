#pragma once

#include "cef/cef_scene_operations.h"

#include <string>

namespace Corona::Systems::UI::CameraRenderSettingsService {

struct ValidationError {
    std::string code;
    std::string message;
    explicit operator bool() const { return !code.empty(); }
};

// Validates identity, patch shape and the Vision/settings whitelists for both
// ReadCameraRenderSettings and SetCameraRenderSettings. Messages are part of the
// frozen contract.
ValidationError validate_patch(const CameraRenderSettingsPatch& patch,
                               SceneMethod method,
                               bool has_archive_session);

}  // namespace Corona::Systems::UI::CameraRenderSettingsService
