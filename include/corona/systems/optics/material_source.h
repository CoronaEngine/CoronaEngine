#pragma once

#include <corona/resource/types/scene.h>
#include <optional>

namespace Corona::Materials {

// Scene-authored overrides. Empty fields inherit the model. No renderer API
// setter is needed; scene loading publishes one immutable list per actor.
struct Override {
    std::string material{"*"};
    std::optional<std::uint32_t> index;
    std::optional<std::array<float, 4>> base_color;
    std::optional<std::array<float, 3>> emissive, specular_color;
    std::optional<float> metallic, roughness, ior, specular_factor;
    std::optional<float> normal_scale, occlusion_strength, clearcoat, clearcoat_roughness;
    std::optional<bool> unlit;

    [[nodiscard]] bool matches(std::uint32_t material_index, const std::string& name) const {
        return index ? *index == material_index : material == "*" || material == name;
    }
    void apply(Resource::MaterialParameters& value) const {
        if (base_color) value.base_color = *base_color;
        if (emissive) value.emissive = *emissive;
        if (specular_color) value.specular_color = *specular_color;
        if (metallic) value.metallic = *metallic;
        if (roughness) value.roughness = *roughness;
        if (ior) value.ior = *ior;
        if (specular_factor) value.specular_factor = *specular_factor;
        if (normal_scale) value.normal_scale = *normal_scale;
        if (occlusion_strength) value.occlusion_strength = *occlusion_strength;
        if (clearcoat) value.clearcoat = *clearcoat;
        if (clearcoat_roughness) value.clearcoat_roughness = *clearcoat_roughness;
        if (unlit) value.unlit = *unlit;
    }
};
using Overrides = std::vector<Override>;

} // namespace Corona::Materials
