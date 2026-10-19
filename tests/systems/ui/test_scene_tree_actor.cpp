#include <cstdint>
#include <stdexcept>
#include <string>

#include "cef/scene_tree_actor.h"

namespace {

using Corona::Ui::Cef::SceneTreeActorRow;
using Corona::Ui::Cef::scene_tree_actor_to_json;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main() {
    SceneTreeActorRow row;
    row.name = "01_gate_temple";
    row.actor_guid = "native-9bd739334447c23a-0-1";
    row.route = "Assets/Models/fac4c1256eda/01_gate_temple.glb";
    row.actor_type = "model";
    row.gpu_build_state = "PendingBuild";
    row.load_status = "gpu_uploading";
    row.handle = 1868024825968ull;
    row.visible = true;
    row.model_node_count = 3778;
    row.model_node_root_count = 1;

    const auto item = scene_tree_actor_to_json(row);

    // Every field the scene dock, the viewport pick index and the model node
    // panel read must survive the slimming.
    require(item.at("name") == row.name, "name");
    require(item.at("actor_guid") == row.actor_guid, "actor_guid");
    require(item.at("path") == row.route, "path");
    require(item.at("type") == row.actor_type, "type");
    require(item.at("visible") == true, "visible");
    require(item.at("handle") == row.handle, "handle");
    require(item.at("audio_resource_id") == "", "audio_resource_id");
    require(item.at("vision_proxy") == false, "vision_proxy");
    require(item.at("load_status") == "gpu_uploading", "load_status");
    require(item.at("gpu_build_state") == "PendingBuild", "gpu_build_state");
    require(item.at("model_node_count") == 3778, "model_node_count");
    require(item.at("model_node_root_count") == 1, "model_node_root_count");
    require(item.at("load_error").is_null(), "load_error");

    // A tree row must not carry the engine-side payload that made the previous
    // snapshot re-derive AABBs, materials and mechanics for every actor on the
    // CEF UI thread.
    for (const char* heavy : {"local_aabb", "world_aabb", "aabb", "bounds_ready",
                              "mechanics", "optics", "geometry", "render_ready",
                              "render_status_observed", "ik_chains"}) {
        require(!item.contains(heavy), heavy);
    }

    SceneTreeActorRow failing;
    failing.name = "broken";
    failing.load_status = "failed";
    failing.has_load_error = true;
    failing.load_error_code = "ASSET_MISSING";
    failing.load_error_message = "asset not found";
    failing.load_error_resource_path = "Assets/missing.glb";
    failing.load_error_path = "/abs/missing.glb";
    const auto failed_item = scene_tree_actor_to_json(failing);
    require(failed_item.at("load_error").at("message") == "asset not found", "load_error.message");
    require(failed_item.at("load_error").at("code") == "ASSET_MISSING", "load_error.code");
    require(failed_item.at("load_error").at("path") == "/abs/missing.glb", "load_error.path");
    return 0;
}
