#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace Corona::Ui::Cef {

/// One actor row of the native editor scene tree.
///
/// The scene tree is refreshed from the CEF UI thread, which is also the thread
/// that paints the editor. Re-deriving the full actor JSON for every refresh
/// (AABBs, materials, mechanics, render status) is what made a scene with a
/// large model stall the whole UI, so a row is built from values the caller has
/// already read and carries only what the scene dock, the viewport pick index
/// and the model node panel consume.
struct SceneTreeActorRow {
    std::string name;
    std::string actor_guid;
    std::string route;
    std::string actor_type;
    std::string audio_resource_id;
    std::string gpu_build_state;
    /// Dock-facing status: "loaded", "gpu_uploading", "gpu_failed" or the raw
    /// actor load status name.
    std::string load_status;
    std::string load_error_code;
    std::string load_error_message;
    std::string load_error_resource_path;
    std::string load_error_path;
    std::uintptr_t handle{0};
    bool visible{true};
    bool has_load_error{false};
    std::size_t model_node_count{0};
    std::size_t model_node_root_count{0};
};

[[nodiscard]] inline nlohmann::json scene_tree_actor_to_json(const SceneTreeActorRow& row) {
    nlohmann::json item = nlohmann::json::object();
    item["name"] = row.name;
    item["actor_guid"] = row.actor_guid;
    item["path"] = row.route;
    item["type"] = row.actor_type;
    item["visible"] = row.visible;
    item["handle"] = row.handle;
    item["audio_resource_id"] = row.audio_resource_id;
    item["vision_proxy"] = false;
    item["load_status"] = row.load_status;
    item["gpu_build_state"] = row.gpu_build_state;
    item["model_node_count"] = row.model_node_count;
    item["model_node_root_count"] = row.model_node_root_count;
    item["load_error"] = row.has_load_error
                             ? nlohmann::json({{"code", row.load_error_code},
                                               {"message", row.load_error_message},
                                               {"resource_path", row.load_error_resource_path},
                                               {"path", row.load_error_path}})
                             : nlohmann::json(nullptr);
    return item;
}

}  // namespace Corona::Ui::Cef
