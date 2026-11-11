#pragma once

// Tolerant JSON readers for the request channels.
//
// The Web and the archive write the same field in several shapes depending on which era
// produced the file: a scalar, a [x,y,z] array, or an object with aliases ("pos" for
// "position"). Every reader here accepts the shapes it knows and falls back otherwise -
// these functions never throw, because a malformed optional field must not fail a request
// that is otherwise valid.

#include <array>
#include <initializer_list>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace Corona::Systems::UI {

bool parse_bool(std::string value, bool fallback = false);
std::string normalize_body_type(std::string value);

/// Reads a 3-vector from an array or an object, falling back when absent or malformed.
std::array<float, 3> snapshot_float3(const nlohmann::json& value,
                                     std::array<float, 3> fallback);
std::optional<std::array<float, 3>> json_float3_value(const nlohmann::json& value);

/// First key that exists and holds a string.
std::string json_string_value(const nlohmann::json& object,
                              std::initializer_list<const char*> keys);

float json_float_at(const nlohmann::json& value, size_t index, float fallback = 0.0f);
bool json_bool_at(const nlohmann::json& value, size_t index, bool fallback = false);

/// Looks the same value up under several aliases; empty when none matches or is malformed.
std::optional<float> actor_data_float(const nlohmann::json& actor_data,
                                      std::initializer_list<const char*> keys);
std::optional<bool> actor_data_bool(const nlohmann::json& actor_data,
                                    std::initializer_list<const char*> keys);

}  // namespace Corona::Systems::UI
