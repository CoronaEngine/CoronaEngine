#pragma once

#include "scene_tree_actor_row.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace Corona::Ui::Cef {

[[nodiscard]] inline nlohmann::json scene_tree_actor_to_json(const SceneTreeActorRow& row) {
    nlohmann::json item = nlohmann::json::object();
    item["name"] = row.name;
    item["actor_guid"] = row.actor_guid;
    item["path"] = row.route;
    item["type"] = row.actor_type;
    item["visible"] = row.visible;
    item["handle"] = row.handle;
    item["audio_resource_id"] = row.audio_resource_id;
    item["load_status"] = row.load_status;
    item["gpu_build_state"] = row.gpu_build_state;
    item["model_node_count"] = row.model_node_count;
    item["model_node_root_count"] = row.model_node_root_count;
    item["actor_version"] = row.actor_version;
    item["load_error"] = row.has_load_error
                             ? nlohmann::json({{"code", row.load_error_code},
                                               {"message", row.load_error_message},
                                               {"resource_path", row.load_error_resource_path},
                                               {"path", row.load_error_path}})
                             : nlohmann::json(nullptr);
    return item;
}

}  // namespace Corona::Ui::Cef
