#include "scene_model_node_service.h"

#include "cef/model_node_page.h"

#include <unordered_map>

namespace Corona::Systems::UI::SceneModelNodeService {

ModelNodeSnapshot make_snapshot(const Corona::API::ModelNodeState& node,
                                std::size_t child_count,
                                bool actor_visible) {
    ModelNodeSnapshot result;
    result.node_index = node.node_index;
    result.parent_index = node.parent_index;
    result.name = node.name;
    result.node_path = node.node_path;
    result.position = node.position;
    result.rotation = node.rotation;
    result.scale = node.scale;
    result.visible = node.visible;
    result.effective_visible = node.effective_visible && actor_visible;
    result.editable = node.editable;
    result.child_count = child_count;
    return result;
}

ModelNodePageSnapshot project_page(const std::vector<Corona::API::ModelNodeState>& nodes,
                                   const ModelNodePageRequest& request,
                                   bool actor_visible) {
    ModelNodePageSnapshot result;
    const auto page = Corona::Ui::Cef::model_node_page(nodes, request.parent_index, request.offset, request.limit);
    std::unordered_map<std::uint32_t, std::size_t> child_counts;
    for (const auto& node : nodes) ++child_counts[node.parent_index];
    result.parent_index = request.parent_index;
    result.total = page.total;
    result.next_offset = page.next_offset;
    result.has_more = page.has_more;
    result.nodes.reserve(page.items.size());
    for (const auto* node : page.items) {
        result.nodes.push_back(make_snapshot(*node, child_counts[node->node_index], actor_visible));
    }
    return result;
}

Corona::API::ModelNodeState derive_patched_state(const Corona::API::ModelNodeState& base,
                                                 const ModelNodePatch& patch) {
    auto state = base;
    if (patch.position) state.position = *patch.position;
    if (patch.rotation) state.rotation = *patch.rotation;
    if (patch.scale) state.scale = *patch.scale;
    if (patch.visible) state.visible = *patch.visible;
    return state;
}

nlohmann::json node_keys(const std::vector<Corona::API::ModelNodeState>& nodes) {
    nlohmann::json keys = nlohmann::json::array();
    for (const auto& node : nodes) {
        keys.push_back({{"node_index", node.node_index}, {"node_path", node.node_path}});
    }
    return keys;
}

}  // namespace Corona::Systems::UI::SceneModelNodeService
