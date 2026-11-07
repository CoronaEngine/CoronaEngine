#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace Corona::Ui::Cef {

// Owned scene-tree values shared by direct V8 reads and non-Web adapters.
// No AABB, full model nodes, materials or advanced rendering settings.
struct SceneTreeActorRow {
    std::string name;
    std::string actor_guid;
    std::string route;
    std::string actor_type;
    std::string audio_resource_id;
    std::string gpu_build_state;
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

}  // namespace Corona::Ui::Cef
