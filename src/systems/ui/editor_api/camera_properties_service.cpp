#include "editor_api_services.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>

namespace Corona::Systems::UI::CameraPropertiesService {

namespace {

std::string trim_ascii(std::string value) {
    const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    if (begin >= end) {
        return {};
    }
    return std::string(begin, end);
}

}  // namespace

ValidationError validate_view_lifecycle(const CameraPropertiesPatch& patch, bool editor_input) {
    if (editor_input && patch.view_open) {
        return {"INVALID_ARGUMENT", "Camera window lifecycle belongs to window operations"};
    }
    return {};
}

ValidationError validate_identity(const CameraPropertiesPatch& patch, bool has_archive_session) {
    if (patch.scene.empty() || patch.camera_id.empty() || !has_archive_session || patch.empty()) {
        return {"INVALID_ARGUMENT", "Camera patch requires archive, scene, camera identity and properties"};
    }
    return {};
}

ValidationError validate_rename(const std::string& requested_name,
                                bool read_only,
                                const std::vector<std::string>& other_names) {
    if (read_only) return {"READ_ONLY", "Cannot rename a camera in a read-only scene"};
    const auto name = trim_ascii(requested_name);
    if (name.empty()) return {"INVALID_ARGUMENT", "Camera name cannot be empty"};
    if (std::any_of(other_names.begin(), other_names.end(), [&](const std::string& other) { return other == name; })) {
        return {"DUPLICATE_CAMERA_NAME", "Camera name already exists: " + name};
    }
    return {};
}

std::string normalized_name(const std::string& requested_name) {
    return trim_ascii(requested_name);
}

ValidationError validate_numeric(const CameraPropertiesPatch& patch,
                                 const std::array<float, 3>& current_projection) {
    const auto finite = [](const std::optional<float>& value) { return !value || std::isfinite(*value); };
    const auto vector_valid = [](const std::optional<std::array<float, 3>>& value) {
        return !value || std::all_of(value->begin(), value->end(), [](float number) { return std::isfinite(number); });
    };
    const auto positive = [](const std::optional<int>& value) { return !value || *value > 0; };
    const auto sign = patch.projection_x_sign.value_or(current_projection[0]);
    const auto near_plane = patch.near_plane.value_or(current_projection[1]);
    const auto far_plane = patch.far_plane.value_or(current_projection[2]);
    if (!vector_valid(patch.position) || !vector_valid(patch.forward) || !vector_valid(patch.world_up) ||
        !finite(patch.fov) || (patch.fov && (*patch.fov <= 0 || *patch.fov >= 180)) ||
        !finite(patch.move_speed) || (patch.move_speed && *patch.move_speed < 0.01f) ||
        !positive(patch.width) || !positive(patch.height) || !positive(patch.view_width) || !positive(patch.view_height) ||
        !std::isfinite(sign) || std::abs(sign) != 1 || !std::isfinite(near_plane) || !std::isfinite(far_plane) ||
        near_plane <= 0 || far_plane <= near_plane) {
        return {"INVALID_ARGUMENT", "Invalid camera properties"};
    }
    return {};
}

ValidationError validate_editor_input(const CameraPropertiesPatch& patch,
                                      bool editor_input,
                                      bool editor_camera_input_enabled) {
    if (editor_input && patch.has_pose() && !editor_camera_input_enabled) {
        return {"EDITOR_INPUT_LOCKED", "Runtime owns camera input"};
    }
    return {};
}

}  // namespace Corona::Systems::UI::CameraPropertiesService
