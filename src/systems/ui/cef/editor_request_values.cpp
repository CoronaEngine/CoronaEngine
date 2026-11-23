#include "editor_request_values.h"

#include "editor_text_utils.h"

#include <corona/diagnostics/thread_phase.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>

namespace Corona::Systems::UI {

bool parse_bool(std::string value, bool fallback) {
    value = to_lower_ascii(trim_ascii(std::move(value)));
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        return false;
    }
    return fallback;
}

std::string normalize_body_type(std::string value) {
    if (value == "dynamic" || value == "kinematic" || value == "static" || value == "phantom") return value;
    // 向后兼容旧碰撞形状字段映射
    if (value == "none") return "phantom";
    if (value == "box" || value == "mesh") return "dynamic";
    CFW_LOG_WARNING("Invalid body type '{}'; using dynamic", value);
    return "dynamic";
}

std::array<float, 3> snapshot_float3(const nlohmann::json& value,
                                     std::array<float, 3> fallback) {
    if (!value.is_array() || value.size() != 3) return fallback;
    std::array<float, 3> result = fallback;
    for (std::size_t index = 0; index < 3; ++index) {
        if (!value[index].is_number()) return fallback;
        const auto number = value[index].get<double>();
        if (!std::isfinite(number)) return fallback;
        result[index] = static_cast<float>(number);
    }
    return result;
}

std::string json_string_value(const nlohmann::json& object,
                              std::initializer_list<const char*> keys) {
    if (!object.is_object()) {
        return {};
    }
    for (const char* key : keys) {
        const auto it = object.find(key);
        if (it != object.end() && it->is_string()) {
            const auto value = trim_ascii(it->get<std::string>());
            if (!value.empty()) {
                return value;
            }
        }
    }
    return {};
}

float json_float_at(const nlohmann::json& value, size_t index, float fallback) {
    if (!value.is_array() || index >= value.size() || !value[index].is_number()) {
        return fallback;
    }
    return value[index].get<float>();
}

std::optional<std::array<float, 3>> json_float3_value(const nlohmann::json& value) {
    if (!value.is_array() || value.size() < 3) {
        return std::nullopt;
    }
    return std::array<float, 3>{
        json_float_at(value, 0, 0.0f),
        json_float_at(value, 1, 0.0f),
        json_float_at(value, 2, 0.0f),
    };
}

std::optional<float> actor_data_float(const nlohmann::json& actor_data,
                                      std::initializer_list<const char*> keys) {
    if (!actor_data.is_object()) {
        return std::nullopt;
    }
    for (const char* key : keys) {
        const auto it = actor_data.find(key);
        if (it != actor_data.end()) {
            if (it->is_number()) {
                return it->get<float>();
            }
            if (it->is_string()) {
                try {
                    return std::stof(it->get<std::string>());
                } catch (...) {
                    return std::nullopt;
                }
            }
        }
    }
    return std::nullopt;
}

bool json_bool_at(const nlohmann::json& value, size_t index, bool fallback) {
    if (!value.is_array() || index >= value.size()) {
        return fallback;
    }
    if (value[index].is_boolean()) {
        return value[index].get<bool>();
    }
    if (value[index].is_number_integer()) {
        return value[index].get<int>() != 0;
    }
    if (value[index].is_string()) {
        return parse_bool(value[index].get<std::string>(), fallback);
    }
    return fallback;
}

std::optional<bool> actor_data_bool(const nlohmann::json& actor_data,
                                    std::initializer_list<const char*> keys) {
    if (!actor_data.is_object()) {
        return std::nullopt;
    }
    for (const char* key : keys) {
        const auto it = actor_data.find(key);
        if (it != actor_data.end()) {
            if (it->is_boolean()) {
                return it->get<bool>();
            }
            if (it->is_number_integer()) {
                return it->get<int>() != 0;
            }
            if (it->is_string()) {
                return parse_bool(it->get<std::string>());
            }
        }
    }
    const auto mechanics_it = actor_data.find("mechanics");
    if (mechanics_it != actor_data.end() && mechanics_it->is_object()) {
        return actor_data_bool(*mechanics_it, keys);
    }
    return std::nullopt;
}

}
  // namespace Corona::Systems::UI
