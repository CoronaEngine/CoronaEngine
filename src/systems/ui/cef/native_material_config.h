#pragma once

#include <corona/systems/optics/material_source.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace NativeMaterialConfig {
using Json = nlohmann::json;

inline Corona::Materials::Overrides parse(const Json& rows) {
    if (!rows.is_array() || rows.size() > 4096)
        throw std::invalid_argument("material.overrides must contain at most 4096 objects");
    Corona::Materials::Overrides result;
    for (const auto& row : rows) {
        if (!row.is_object()) throw std::invalid_argument("material override must be an object");
        Corona::Materials::Override value;
        for (const auto& field : row.items()) {
            const auto& key = field.key();
            const auto& data = field.value();
            const auto number = [&](const Json& input, float low, float high) {
                if (!input.is_number()) throw std::invalid_argument("material " + key + " must be numeric");
                const double v = input.get<double>();
                if (!std::isfinite(v) || v < low || v > high)
                    throw std::invalid_argument("material " + key + " is outside its supported range");
                return static_cast<float>(v);
            };
            const auto color = [&]<std::size_t N>(std::optional<std::array<float, N>>& target, float high) {
                if (!data.is_array() || data.size() != N)
                    throw std::invalid_argument("material " + key + " has an invalid channel count");
                std::array<float, N> channels{};
                for (std::size_t c = 0; c < N; ++c) channels[c] = number(data[c], 0.0f, high);
                target = channels;
            };
            if (key == "material") {
                if (!data.is_string() || data.get_ref<const std::string&>().empty())
                    throw std::invalid_argument("material selector must be a non-empty name");
                value.material = data.get<std::string>();
            } else if (key == "index") {
                const auto v = number(data, 0.0f, 16777215.0f);
                if (std::floor(v) != v) throw std::invalid_argument("material index must be integral");
                value.index = static_cast<std::uint32_t>(v);
            } else if (key == "base_color") color(value.base_color, 1.0f);
            else if (key == "emissive") color(value.emissive, 65504.0f);
            else if (key == "specular_color") color(value.specular_color, 65504.0f);
            else if (key == "metallic") value.metallic = number(data, 0.0f, 1.0f);
            else if (key == "roughness") value.roughness = number(data, 0.0f, 1.0f);
            else if (key == "ior") {
                value.ior = number(data, 0.0f, 10000.0f);
                if (*value.ior > 0.0f && *value.ior < 1.0f) throw std::invalid_argument("material ior must be zero or at least one");
            }
            else if (key == "specular_factor") value.specular_factor = number(data, 0.0f, 1.0f);
            else if (key == "normal_scale") value.normal_scale = number(data, -65504.0f, 65504.0f);
            else if (key == "occlusion_strength") value.occlusion_strength = number(data, 0.0f, 1.0f);
            else if (key == "clearcoat") value.clearcoat = number(data, 0.0f, 1.0f);
            else if (key == "clearcoat_roughness") value.clearcoat_roughness = number(data, 0.0f, 1.0f);
            else if (key == "unlit") {
                if (!data.is_boolean()) throw std::invalid_argument("material unlit must be boolean");
                value.unlit = data.get<bool>();
            } else throw std::invalid_argument("Unknown material override field: " + key);
        }
        if (row.contains("index") && row.contains("material"))
            throw std::invalid_argument("Select a material by index or name, not both");
        result.push_back(std::move(value));
    }
    return result;
}

inline void validate(const Json& config) {
    if (!config.is_object()) throw std::invalid_argument("material must be an object");
    const auto source = config.value("source", std::string{"legacy"});
    if (source != "model" && source != "scene" && source != "legacy")
        throw std::invalid_argument("material.source must be model, scene, or legacy");
    (void)parse(config.value("overrides", Json::array()));
}

// Old editor saves manufactured these exact values, even for models which had
// their own materials. Explicit scene source preserves intentionally authored
// defaults; new saves persist resolved overrides and never manufacture them.
template<class State>
Json resolve(const State& state) {
    validate(state.material);
    Json rows = Json::array();
    const auto source = state.material.value("source", std::string{"legacy"});
    if (source != "model") {
        const bool explicit_scene = source == "scene";
        Json legacy = Json::object();
        const auto differs = [&](float value, float previous_default) {
            return std::isfinite(value) && (explicit_scene || std::abs(value - previous_default) > 1e-6f);
        };
        if (state.diffuse && (differs((*state.diffuse)[0], .8f) || differs((*state.diffuse)[1], .8f) || differs((*state.diffuse)[2], .8f)))
            legacy["base_color"] = {(*state.diffuse)[0], (*state.diffuse)[1], (*state.diffuse)[2], 1.0f};
        if (state.metallic && differs(*state.metallic, 0.0f)) legacy["metallic"] = *state.metallic;
        if (state.roughness && differs(*state.roughness, .5f)) legacy["roughness"] = *state.roughness;
        else if (state.shininess && differs(*state.shininess, 32.0f))
            legacy["roughness"] = 1.0f - std::sqrt(std::clamp(*state.shininess, 0.0f, 1000.0f) / 1000.0f);
        if (state.specular && differs(*state.specular, .5f)) {
            const float root = std::sqrt(.08f * std::clamp(*state.specular, 0.0f, 1.0f));
            legacy["ior"] = (1.0f + root) / (1.0f - root);
            legacy["specular_factor"] = 1.0f;
        }
        if (state.emission && (differs((*state.emission)[0], 0.0f) || differs((*state.emission)[1], 0.0f) || differs((*state.emission)[2], 0.0f)))
            legacy["emissive"] = *state.emission;
        if (!legacy.empty()) rows.push_back(std::move(legacy));
    }
    for (const auto& row : state.material.value("overrides", Json::array())) rows.push_back(row);
    (void)parse(rows);
    return rows;
}
} // namespace NativeMaterialConfig
