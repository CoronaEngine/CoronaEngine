#pragma once

#include "cef/cef_scene_operations.h"

#include <corona/systems/geometry/model_node_state.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <nlohmann/json.hpp>

namespace Corona::Systems::UI::SceneModelNodeService {

// Pure projection of one engine model node into the Web-facing snapshot.
// CEF-free by construction: the service only sees engine node state, never a
// browser, V8 value or JS route.
ModelNodeSnapshot make_snapshot(const Corona::API::ModelNodeState& node,
                                std::size_t child_count,
                                bool actor_visible);

// Paginated projection for ReadModelNodes. Fills nodes/total/next_offset/
// has_more and each snapshot; the caller owns archive/scene/actor identity and
// the GPU build status string (they depend on scene/actor state, not nodes).
ModelNodePageSnapshot project_page(const std::vector<Corona::API::ModelNodeState>& nodes,
                                   const ModelNodePageRequest& request,
                                   bool actor_visible);

// Applies the optional patch fields onto a copy of the base node state.
// Mirror of the engine commit input; the caller still performs the controlled
// write through the actor geometry.
Corona::API::ModelNodeState derive_patched_state(const Corona::API::ModelNodeState& base,
                                                 const ModelNodePatch& patch);

// node_index/node_path identity pairs consumed by ModelNodeConfig::partition
// when override groups are re-keyed after a model node edit.
nlohmann::json node_keys(const std::vector<Corona::API::ModelNodeState>& nodes);

}  // namespace Corona::Systems::UI::SceneModelNodeService
