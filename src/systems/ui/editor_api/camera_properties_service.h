#pragma once

#include "cef/cef_scene_operations.h"

#include <array>
#include <string>
#include <vector>

namespace Corona::Systems::UI::CameraPropertiesService {

struct ValidationError {
    std::string code;
    std::string message;
    explicit operator bool() const { return !code.empty(); }
};

// Window lifecycle does not belong to a camera property patch.
ValidationError validate_view_lifecycle(const CameraPropertiesPatch& patch, bool editor_input);

// A non-empty patch must identify an archive, scene, camera and carry fields.
ValidationError validate_identity(const CameraPropertiesPatch& patch, bool has_archive_session);

// Rename rules: read-only scenes reject first, then empty trimmed names, then
// names already used by another camera.
ValidationError validate_rename(const std::string& requested_name,
                                bool read_only,
                                const std::vector<std::string>& other_names);

// Trimmed rename value the caller applies after validation.
std::string normalized_name(const std::string& requested_name);

// Numeric ranges for pose, projection and window geometry. Unset fields fall
// back to the current projection, mirroring the native camera.
ValidationError validate_numeric(const CameraPropertiesPatch& patch,
                                 const std::array<float, 3>& current_projection);

// Runtime owns camera input while a game/script is running.
ValidationError validate_editor_input(const CameraPropertiesPatch& patch,
                                      bool editor_input,
                                      bool editor_camera_input_enabled);

}  // namespace Corona::Systems::UI::CameraPropertiesService
