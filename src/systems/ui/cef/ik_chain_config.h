#pragma once

#include <corona/resource/types/scene.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace Corona::Systems::UI::IkChainConfig {

inline const char* mode_name(Resource::IkChain::Mode mode) {
    switch (mode) {
        case Resource::IkChain::Mode::FootPlant: return "foot_plant";
        case Resource::IkChain::Mode::LookAt: return "look_at";
        case Resource::IkChain::Mode::WeaponAim: return "weapon_aim";
        default: return "contact";
    }
}

inline std::string stable_id(const Resource::IkChain& chain) {
    return chain.id.empty() ? "native-" + std::string(mode_name(chain.mode)) + "-" +
                                 std::to_string(chain.end_node)
                           : chain.id;
}

inline double number(const nlohmann::json& value, const char* field,
                     double fallback, double minimum, double maximum) {
    if (!value.contains(field)) return fallback;
    const auto& member = value.at(field);
    if (!member.is_number()) {
        throw std::invalid_argument(std::string(field) + " must be a number");
    }
    const auto result = member.get<double>();
    if (!std::isfinite(result) || result < minimum || result > maximum) {
        throw std::invalid_argument(std::string(field) + " is outside the supported range");
    }
    return result;
}

inline int integer(const nlohmann::json& value, const char* field,
                   int fallback, int minimum, int maximum) {
    const auto result = number(value, field, fallback, minimum, maximum);
    if (std::floor(result) != result) {
        throw std::invalid_argument(std::string(field) + " must be an integer");
    }
    return static_cast<int>(result);
}

// Parsing finishes before storage is touched. The optional skeleton allows saved
// named configuration to be restored before asynchronous model import completes.
inline std::vector<Resource::IkChain> parse(
    const nlohmann::json& values, const Resource::SkeletonData* skeleton = nullptr) {
    if (!values.is_array() || values.size() > 64) {
        throw std::invalid_argument("IK chains must be an array with at most 64 entries");
    }
    std::vector<Resource::IkChain> result;
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> bone_modes;
    for (std::size_t index = 0; index < values.size(); ++index) {
        try {
            const auto& value = values[index];
            if (!value.is_object()) throw std::invalid_argument("chain must be an object");
            Resource::IkChain chain;
            chain.end_bone_name = value.value("bone_name", std::string{});
            if (chain.end_bone_name.empty() || chain.end_bone_name.size() > 1024) {
                throw std::invalid_argument("bone_name must be a non-empty bone name");
            }
            const auto mode = value.value("mode", std::string{"foot_plant"});
            if (mode == "foot_plant") chain.mode = Resource::IkChain::Mode::FootPlant;
            else if (mode == "contact") chain.mode = Resource::IkChain::Mode::Contact;
            else if (mode == "look_at") chain.mode = Resource::IkChain::Mode::LookAt;
            else if (mode == "weapon_aim") chain.mode = Resource::IkChain::Mode::WeaponAim;
            else throw std::invalid_argument("unsupported IK mode: " + mode);
            if (!bone_modes.insert(chain.end_bone_name + "\n" + mode).second) {
                throw std::invalid_argument("duplicate bone and mode");
            }
            chain.id = value.value("id", std::string{});
            if (chain.id.empty()) chain.id = mode + ":" + chain.end_bone_name;
            if (chain.id.size() > 2048 || !ids.insert(chain.id).second) {
                throw std::invalid_argument("invalid or duplicate chain id");
            }
            if (skeleton) {
                for (std::size_t node = 0; node < skeleton->nodes.size(); ++node) {
                    if (skeleton->nodes[node].name != chain.end_bone_name) continue;
                    if (chain.end_node >= 0) {
                        throw std::invalid_argument("bone name is ambiguous: " + chain.end_bone_name);
                    }
                    chain.end_node = static_cast<int>(node);
                }
                if (chain.end_node < 0) {
                    throw std::invalid_argument("bone not found: " + chain.end_bone_name);
                }
                if (skeleton->nodes[chain.end_node].parent < 0) {
                    throw std::invalid_argument("IK end bone requires a parent joint");
                }
            }
            chain.chain_length = integer(value, "chain_length", 3, 2, 64);
            chain.max_iterations = integer(value, "max_iterations", 10, 1, 128);
            chain.weight = static_cast<float>(number(value, "weight", 1, 0, 1));
            chain.damping = static_cast<float>(number(value, "damping", 0.88, 0, 1));
            chain.tolerance = static_cast<float>(number(value, "tolerance", 1e-3, 1e-6, 1));
            chain.enabled = value.value("enabled", true);
            chain.contact_driven = chain.mode == Resource::IkChain::Mode::Contact;
            const bool foot = chain.mode == Resource::IkChain::Mode::FootPlant;
            chain.contact_weight_rise = static_cast<float>(number(value, "contact_weight_rise", foot ? 8 : 2, 0, 100));
            chain.contact_weight_decay = static_cast<float>(number(value, "contact_weight_decay", foot ? 0.8 : 2, 0, 100));
            chain.contact_normal_offset = static_cast<float>(number(value, "contact_normal_offset", foot ? 0 : 0.03, -100, 100));
            chain.probe_max_drop = static_cast<float>(number(value, "probe_max_drop", 0.5, 0, 100));
            chain.foot_height = static_cast<float>(number(value, "foot_height", 0, 0, 10));
            chain.plant_threshold = static_cast<float>(number(value, "plant_threshold", 0.08, 0, 100));
            if (value.contains("target")) {
                const auto& target = value.at("target");
                if (!target.is_array() || target.size() != 3) {
                    throw std::invalid_argument("target must contain exactly three finite numbers");
                }
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    if (!target[axis].is_number()) throw std::invalid_argument("invalid target component");
                    const auto component = target[axis].get<double>();
                    if (!std::isfinite(component) || std::abs(component) > std::numeric_limits<float>::max()) {
                        throw std::invalid_argument("target contains a non-finite or overflowing number");
                    }
                    chain.target[axis] = static_cast<float>(component);
                }
            }
            result.push_back(std::move(chain));
        } catch (const std::exception& error) {
            throw std::invalid_argument("IK chain " + std::to_string(index + 1) + ": " + error.what());
        }
    }
    return result;
}

inline nlohmann::json serialize(const std::vector<Resource::IkChain>& chains,
                                const Resource::SkeletonData* skeleton = nullptr,
                                bool include_runtime = false) {
    auto result = nlohmann::json::array();
    for (const auto& chain : chains) {
        auto bone = chain.end_bone_name;
        if (bone.empty() && skeleton && chain.end_node >= 0 && chain.end_node < static_cast<int>(skeleton->nodes.size())) {
            bone = skeleton->nodes[chain.end_node].name;
        }
        nlohmann::json value = {
            {"id", stable_id(chain)}, {"bone_name", bone}, {"mode", mode_name(chain.mode)},
            {"chain_length", chain.chain_length}, {"weight", chain.weight}, {"target", chain.target},
            {"max_iterations", chain.max_iterations}, {"tolerance", chain.tolerance},
            {"damping", chain.damping}, {"enabled", chain.enabled},
            {"contact_weight_rise", chain.contact_weight_rise}, {"contact_weight_decay", chain.contact_weight_decay},
            {"contact_normal_offset", chain.contact_normal_offset},
            {"probe_max_drop", chain.probe_max_drop}, {"foot_height", chain.foot_height},
            {"plant_threshold", chain.plant_threshold},
        };
        if (include_runtime) {
            value["runtime"] = {{"has_target", chain.runtime.has_target}, {"weight", chain.runtime.weight},
                                 {"target", chain.runtime.target}, {"grounded", chain.runtime.grounded},
                                 {"releasing", chain.runtime.releasing}, {"normal", chain.runtime.normal}};
        }
        result.push_back(std::move(value));
    }
    return result;
}

inline std::string dump_ini(const nlohmann::json& configuration) {
    auto text = configuration.dump();
    // ConfigParser interpolates percent signs even inside JSON strings. A JSON
    // Unicode escape preserves arbitrary bone names in both native/Python INI readers.
    for (std::size_t position = 0; (position = text.find('%', position)) != std::string::npos;) {
        text.replace(position, 1, "\\u0025");
        position += 6;
    }
    return text;
}

inline void preserve_runtime(std::vector<Resource::IkChain>& replacement,
                              const std::vector<Resource::IkChain>& current) {
    for (auto& chain : replacement) {
        if (!chain.enabled) continue;
        const auto previous = std::find_if(current.begin(), current.end(), [&](const auto& old) {
            return stable_id(old) == chain.id && old.end_node == chain.end_node &&
                   old.mode == chain.mode && old.target == chain.target && old.enabled &&
                   old.chain_length == chain.chain_length && old.foot_height == chain.foot_height &&
                   old.probe_max_drop == chain.probe_max_drop && old.plant_threshold == chain.plant_threshold;
        });
        if (previous != current.end()) chain.runtime = previous->runtime;
    }
}

// A collaborative actor snapshot may repeat the same configuration with every
// transform update. Such a replay must not reset support or invalidate UI revisions.
inline bool apply_snapshot(std::vector<Resource::IkChain>& current,
                           std::uint64_t& revision,
                           std::vector<Resource::IkChain> replacement,
                           const Resource::SkeletonData* skeleton = nullptr) {
    if (serialize(current, skeleton) == serialize(replacement, skeleton)) return false;
    preserve_runtime(replacement, current);
    current = std::move(replacement);
    ++revision;
    return true;
}

}  // namespace Corona::Systems::UI::IkChainConfig
