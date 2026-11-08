#include "actor_properties_service.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <optional>

namespace Corona::Systems::UI::ActorPropertiesService {

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

bool finite3(const std::optional<std::array<float, 3>>& value) {
    return !value || std::all_of(value->begin(), value->end(), [](float x) { return std::isfinite(x); });
}

}  // namespace

ValidationError validate_patch(const ActorPropertiesPatch& patch) {
    if (trim_ascii(patch.actor).empty() ||
        (!patch.name && !patch.visible && !patch.follow_camera && !patch.mechanics &&
         !patch.camera_lock && !patch.animation_sources)) {
        return {"INVALID_ARGUMENT", "Actor property patch is empty"};
    }
    if (patch.mechanics) {
        const auto& values = *patch.mechanics;
        const auto scalar = [](const std::optional<float>& value) { return !value || std::isfinite(*value); };
        if (values.empty() || !scalar(values.mass) || !scalar(values.restitution) || !scalar(values.damping) ||
            (values.body_type && *values.body_type != "dynamic" && *values.body_type != "kinematic" &&
             *values.body_type != "static" && *values.body_type != "phantom")) {
            return {"INVALID_ARGUMENT", "Invalid Actor mechanics patch"};
        }
    }
    if (patch.camera_lock &&
        (!finite3(patch.camera_lock->position_offset) || !finite3(patch.camera_lock->rotation_offset))) {
        return {"INVALID_ARGUMENT", "Camera offsets must be finite"};
    }
    return {};
}

ValidationError validate_rename(const std::string& requested_name,
                                bool read_only,
                                const std::vector<std::string>& other_names) {
    if (read_only) return {"READ_ONLY", "Cannot rename an actor in a read-only scene"};
    const auto name = trim_ascii(requested_name);
    if (name.empty()) return {"INVALID_ARGUMENT", "Actor name cannot be empty"};
    if (std::any_of(other_names.begin(), other_names.end(), [&](const std::string& other) { return other == name; })) {
        return {"DUPLICATE_ACTOR_NAME", "Actor name already exists: " + name};
    }
    return {};
}

std::string normalized_name(const std::string& requested_name) {
    return trim_ascii(requested_name);
}

}  // namespace Corona::Systems::UI::ActorPropertiesService
