#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace Corona::Systems::UI {

[[nodiscard]] inline nlohmann::json make_actor_selection_event_payload(
    std::string_view scene,
    std::string_view actor_type,
    std::string_view actor,
    const nlohmann::json& context) {
    std::string source_viewport = "main";
    std::string source_camera_id;

    if (context.is_object()) {
        const auto viewport_it = context.find("sourceViewport");
        if (viewport_it != context.end() && viewport_it->is_string()) {
            const auto requested = viewport_it->get<std::string>();
            if (requested == "main" || requested == "cameraView") {
                source_viewport = requested;
            }
        }

        const auto camera_it = context.find("sourceCameraId");
        if (source_viewport == "cameraView" && camera_it != context.end() && camera_it->is_string()) {
            source_camera_id = camera_it->get<std::string>();
        }
    }

    if (source_viewport == "cameraView" && source_camera_id.empty()) {
        source_viewport = "main";
    }

    nlohmann::json payload = {
        {"actor_type", actor_type},
        {"scene", scene},
        {"actor", actor},
        {"source_viewport", source_viewport},
        {"source_camera_id", source_camera_id},
    };
    if (context.is_object()) {
        const auto node = context.find("node_index");
        if (node != context.end() && node->is_number_integer() &&
            (node->is_number_unsigned() || node->get<std::int64_t>() >= 0) &&
            node->get<std::uint64_t>() < UINT32_MAX) {
            payload["node_index"] = node->get<std::uint32_t>();
        }
    }
    return payload;
}

}  // namespace Corona::Systems::UI
