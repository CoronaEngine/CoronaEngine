#include "vision_light_adapter.h"

#include <cmath>
#include <algorithm>
#include <vector>

#include <corona/shared_data_hub.h>

#include "base/illumination/light.h"
#include "base/import/node_desc.h"
#include "base/mgr/scene.h"

namespace Corona::Systems::Vision {

namespace {

::vision::LightDesc make_light_desc(const char* type) {
    // LightDesc::init reads the JSON type and defaults missing types to "area".
    // The constructor argument does not survive init({}); that accidentally
    // creates emissive quads instead of the intended directional/environment.
    ::vision::LightDesc desc;
    desc.init(::vision::ParameterSet{::vision::DataWrap{
        {"type", type}, {"param", ::vision::DataWrap::object()}}});
    return desc;
}

}  // namespace

VisionLightingMode vision_lighting_mode(const EnvironmentDevice& env) noexcept {
    return std::isfinite(env.sky_intensity) && env.sky_intensity > 0.f
               ? VisionLightingMode::NeutralPreview
               : VisionLightingMode::None;
}

VisionLightingState make_vision_lighting_state(const EnvironmentDevice& env) noexcept {
    VisionLightingState result;
    result.mode = vision_lighting_mode(env);
    const auto positive_finite = [](float value) {
        return std::isfinite(value) ? std::max(value, 0.f) : 0.f;
    };
    result.sun_intensity = positive_finite(env.sun_intensity);
    if (result.sun_intensity == 0.f) return result;
    result.sun_color = {positive_finite(env.sun_color.x), positive_finite(env.sun_color.y),
                        positive_finite(env.sun_color.z)};
    if (result.sun_color == std::array<float, 3>{}) {
        result.sun_intensity = 0.f;
        return result;
    }
    double x = env.sun_position.x, y = env.sun_position.y, z = env.sun_position.z;
    double len = std::sqrt(x * x + y * y + z * z);
    if (!std::isfinite(len) || len < 1e-5) {
        x = y = z = 1.0;
        len = std::sqrt(3.0);
    }
    // Corona -> Vision is F=diag(1,1,-1); then negate the toward-sun vector.
    result.sun_direction = {static_cast<float>(-x / len), static_cast<float>(-y / len),
                            static_cast<float>(z / len)};
    return result;
}

const char* vision_lighting_mode_name(VisionLightingMode mode) noexcept {
    return mode == VisionLightingMode::NeutralPreview ? "neutral-preview" : "none";
}

VisionLightSetupResult setup_vision_lights(::vision::Scene& scene, const EnvironmentDevice& env) {
    VisionLightSetupResult result;
    const auto state = make_vision_lighting_state(env);
    result.mode = state.mode;
    result.environment_radiance = result.mode == VisionLightingMode::NeutralPreview ? 1.f : 0.f;

    // Only the EngineBuilt bridge owns these non-area lights. Removing a
    // geometry-owned area light would leave its ShapeInstance emission pointer
    // invalid. Snapshot the removable handles before mutating the manager.
    auto& light_mgr = scene.light_manager();
    std::vector<::vision::TLight> to_remove;
    for (auto& light : light_mgr.lights()) {
        if (!light->match(::vision::LightType::Area)) {
            to_remove.push_back(light);
        }
    }
    for (auto& light : to_remove) {
        light_mgr.remove_light(light);
    }

    // Infinite lights share the Environment interface intentionally. Both sun
    // and sky stay in the sampling list; the last registered infinite light is
    // the background selected by env_light_. Register the delta sun first so
    // only the spherical sky contributes on ray misses (no double-counted sun).
    if (state.sun_intensity > 0.f) {
        auto sun_desc = make_light_desc("directional");
        sun_desc.set_value("direction", ::vision::DataWrap::array(
            {state.sun_direction[0], state.sun_direction[1], state.sun_direction[2]}));
        sun_desc.set_value("color", ::vision::DataWrap::array(
            {state.sun_color[0], state.sun_color[1], state.sun_color[2]}));
        sun_desc.set_value("scale", state.sun_intensity);
        light_mgr.init({sun_desc});
    }
    // The zero-radiance sky sentinel keeps miss/sampling paths valid when both
    // sun and sky are off. It emits no energy and is not a supplemental light.
    auto sky_desc = make_light_desc("spherical");
    sky_desc.set_value("color", ::vision::DataWrap::array({1.f, 1.f, 1.f}));
    sky_desc.set_value("scale", result.environment_radiance);
    light_mgr.init({sky_desc});

    for (auto& light : light_mgr.lights()) {
        if (light->match(::vision::LightType::Area)) {
            ++result.area_light_count;
        } else if (light->match(::vision::LightType::DeltaDirection)) {
            ++result.directional_light_count;
        } else if (light->match(::vision::LightType::Infinite)) {
            ++result.environment_count;
        } else {
            ++result.punctual_light_count;
        }
    }
    OC_INFO_FORMAT("Corona Vision lighting: mode={}, environment radiance={}, environments={}, directional lights={}, sun irradiance={}, area lights={}, punctual lights={} (atmosphere deferred)",
                   vision_lighting_mode_name(result.mode), result.environment_radiance,
                   result.environment_count, result.directional_light_count, state.sun_intensity,
                   result.area_light_count, result.punctual_light_count);
    return result;
}

}  // namespace Corona::Systems::Vision
