#include "vision_light_adapter.h"

#include <cmath>
#include <algorithm>
#include <vector>

#include <corona/shared_data_hub.h>

#include "base/illumination/directional_light.h"
#include "base/import/node_desc.h"
#include "base/mgr/pipeline.h"
#include "vision_environment_adapter.h"

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
    if (env.authored_lighting) return VisionLightingMode::Authored;
    return std::isfinite(env.sky_intensity) && env.sky_intensity > 0.f
               ? VisionLightingMode::Atmosphere
               : VisionLightingMode::None;
}

VisionLightingState make_vision_lighting_state(const EnvironmentDevice& env) noexcept {
    VisionLightingState result;
    result.authored_revision=env.authored_lighting_revision;
    result.mode = vision_lighting_mode(env);
    const auto positive_finite = [](float value) {
        return std::isfinite(value) ? std::max(value, 0.f) : 0.f;
    };
    result.sun_intensity = positive_finite(env.sun_intensity);
    if (result.mode == VisionLightingMode::Atmosphere) result.sky_intensity = env.sky_intensity;
    if (result.sun_intensity > 0.f) {
        result.sun_color = {positive_finite(env.sun_color.x), positive_finite(env.sun_color.y),
                            positive_finite(env.sun_color.z)};
        if (result.sun_color == std::array<float, 3>{}) result.sun_intensity = 0.f;
    }
    if (result.sun_intensity == 0.f && result.mode != VisionLightingMode::Atmosphere) return result;
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
    if (mode==VisionLightingMode::Authored) return "authored";
    return mode == VisionLightingMode::Atmosphere ? "atmosphere" : "none";
}

bool try_update_vision_sun_direction(::vision::Pipeline& pipeline, VisionLightingState previous,
                                     const VisionLightingState& current) {
    previous.sun_direction = current.sun_direction;
    auto& manager = pipeline.scene().light_manager();
    auto& lights = manager.lights();
    // Topology dispatch decodes the light parameters at runtime. The uniform
    // proposal depends only on light count, so its per-view probabilities stay valid.
    if (previous != current || current.mode == VisionLightingMode::Authored ||
        lights.mode() != ocarina::ETopology || pipeline.renderer().light_sampler()->impl_type() != "uniform") return false;

    ::vision::IDirectionalLight* sun = nullptr;
    if (current.sun_intensity > 0.f) {
        for (auto& light : lights) {
            if (light->match(::vision::LightType::DeltaDirection)) {
                sun = dynamic_cast<::vision::IDirectionalLight*>(light.get());
                break;
            }
        }
        if (!sun) return false;
    }
    auto* sky = manager.env_light();
    if (current.mode == VisionLightingMode::Atmosphere && (!sky || sky->impl_type() != "spherical")) return false;

    const auto upload = [&](const ::vision::Light& light) {
        auto& data = lights.get_datas(&light);
        light.encode(data);
        data.upload_immediately();
    };
    const auto& d = current.sun_direction;
    const auto outgoing = ocarina::make_float3(d[0], d[1], d[2]);
    if (sun) {
        sun->set_direction(outgoing);
        upload(*sun);
    }
    if (current.mode == VisionLightingMode::Atmosphere) {
        (void)pipeline.image_pool().atmospheric_sky_slot(pipeline, -outgoing);
        sky->prepare();
        upload(*sky); // includes the new integral used to normalize the environment PDF
    }
    return true;
}

VisionLightSetupResult setup_vision_lights(::vision::Pipeline& pipeline, const EnvironmentDevice& env) {
    VisionLightSetupResult result;
    const auto state = make_vision_lighting_state(env);
    result.mode = state.mode;
    result.environment_scale = state.sky_intensity;

    // Only the EngineBuilt bridge owns these non-area lights. Removing a
    // geometry-owned area light would leave its ShapeInstance emission pointer
    // invalid. Snapshot the removable handles before mutating the manager.
    auto& light_mgr = pipeline.scene().light_manager();
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
    if (env.authored_lighting) {
        for (const auto& light:env.authored_lighting->at("directionals")) {
            auto desc=make_light_desc("directional");
            const auto d=light.at("direction").get<std::array<float,3>>();
            desc.set_value("direction",::vision::DataWrap::array({d[0],d[1],-d[2]}));
            desc.set_value("color",vision_json(appearance_slot(light.at("color"))));
            desc.set_value("scale",1.f);
            light_mgr.init({desc});
        }
    } else if (state.sun_intensity > 0.f) {
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
    sky_desc.set_value("scale", result.environment_scale);
    if (state.mode == VisionLightingMode::Atmosphere) {
        const auto& d = state.sun_direction;
        sky_desc.set_value("color", pipeline.image_pool().atmospheric_sky_slot(
            pipeline, ocarina::make_float3(-d[0], -d[1], -d[2])));
    } else if (env.authored_lighting) {
        const auto& environments=env.authored_lighting->at("environments");
        if (!environments.empty()) {
            sky_desc.set_value("color",vision_json(authored_environment_slot(environments)));
            sky_desc.set_value("scale",1.f);
            result.environment_scale=1.f;
        }
    }
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
    OC_INFO_FORMAT("Corona Vision lighting: mode={}, environment scale={}, environments={}, directional lights={}, sun irradiance={}, area lights={}, punctual lights={}",
                   vision_lighting_mode_name(result.mode), result.environment_scale,
                   result.environment_count, result.directional_light_count, state.sun_intensity,
                   result.area_light_count, result.punctual_light_count);
    return result;
}

}  // namespace Corona::Systems::Vision
