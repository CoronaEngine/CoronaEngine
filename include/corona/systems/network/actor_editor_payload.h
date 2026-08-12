#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace Corona::Network {

inline bool is_process_local_actor_editor_field(std::string_view field_name) {
    static constexpr std::array<std::string_view, 16> kProcessLocalFields = {
        "handle",
        "entity_id",
        "local_aabb",
        "world_aabb",
        "aabb",
        "bounds_ready",
        "render_status_observed",
        "render_ready",
        "render_failed",
        "gpu_build_state",
        "mesh_count",
        "renderable_mesh_count",
        "invalid_mesh_count",
        "size",
        "load_status",
        "load_error",
    };
    for (const auto candidate : kProcessLocalFields) {
        if (candidate == field_name) return true;
    }
    return false;
}

inline nlohmann::json logical_actor_editor_payload(const nlohmann::json& actor) {
    if (!actor.is_object()) return nlohmann::json::object();
    auto logical = actor;
    for (auto it = logical.begin(); it != logical.end();) {
        if (is_process_local_actor_editor_field(it.key())) {
            it = logical.erase(it);
        } else {
            ++it;
        }
    }
    return logical;
}

inline std::optional<std::string> logical_actor_editor_json(
    const std::string& actor_json) {
    if (actor_json.empty()) return std::string{"{}"};
    try {
        const auto parsed = nlohmann::json::parse(actor_json);
        if (!parsed.is_object()) return std::nullopt;
        return logical_actor_editor_payload(parsed).dump();
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

}  // namespace Corona::Network
