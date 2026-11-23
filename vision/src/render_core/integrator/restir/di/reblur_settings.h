#pragma once

#include <algorithm>
#include <nlohmann/json.hpp>

namespace vision {

[[nodiscard]] inline nlohmann::json physical_di_reblur_settings(nlohmann::json settings) {
    // DI shadow edges can lie on the same plane/material; retain their detail
    // without changing the separate stable-plane GI denoisers.
    const float radius = std::min(settings.at("max_radius").get<float>(), 10.f);
    settings["diffuse_prepass"] = 0.f;
    settings["max_radius"] = radius;
    settings["min_radius"] = std::min(settings.at("min_radius").get<float>(), radius);
    return settings;
}

}// namespace vision
