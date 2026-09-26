#pragma once

#include <array>
#include <cmath>
#include <nlohmann/json.hpp>

namespace Corona::Systems::UI {

// Inputs and result use Corona coordinates, except the target stored in Vision JSON.
inline std::array<float, 3> vision_camera_direction(
    const nlohmann::json& transform_params,
    const std::array<float, 3>& position,
    const std::array<float, 3>& fallback) {
    const auto target = transform_params.find("target_pos");
    if (target == transform_params.end() || !target->is_array() || target->size() != 3) {
        return fallback;
    }
    for (const auto& component : *target) {
        if (!component.is_number()) return fallback;
    }
    std::array<float, 3> direction{
        (*target)[0].get<float>() - position[0],
        (*target)[1].get<float>() - position[1],
        -(*target)[2].get<float>() - position[2],
    };
    const auto length = std::hypot(direction[0], direction[1], direction[2]);
    if (!std::isfinite(length) || length <= 0.0f) return fallback;
    for (auto& component : direction) component /= length;
    return direction;
}

}  // namespace Corona::Systems::UI
