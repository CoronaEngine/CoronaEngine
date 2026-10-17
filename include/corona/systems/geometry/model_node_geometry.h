#pragma once
#include <corona/shared_data_hub.h>
#include <algorithm>

namespace Corona::ModelNodes {
inline ktm::fmat4x4 ktm_matrix(const Matrix& source) { ktm::fmat4x4 m=ktm::fmat4x4::from_eye(); for(int c=0;c<4;++c)for(int r=0;r<4;++r)m[c][r]=source[c*4+r]; return m; }
inline bool update_instances(GeometryDevice& g) {
    if(!g.model_node_instances_mapped) { for(auto& n:g.model_nodes)n.instance_indices.clear(); for(std::uint32_t i=0;i<g.mesh_instances.size();++i) if(g.mesh_instances[i].node_index<g.model_nodes.size()) g.model_nodes[g.mesh_instances[i].node_index].instance_indices.push_back(i); g.model_node_instances_mapped=true; }
    std::optional<std::array<float,6>> all;
    for(std::uint32_t i=0;i<g.model_nodes.size();++i) {
        auto& n=g.model_nodes[i]; if(!finite(n.world)) return false;
        for(auto instance:n.instance_indices) { auto& item=g.mesh_instances[instance]; item.visible=n.state.effective_visible; item.transform=ktm_matrix(n.world); }
    }
    g.model_node_bounds=all; ++g.model_node_revision; return true;
}
inline bool apply_overrides(GeometryDevice& g) {
    for(const auto& o:g.model_node_overrides) { if(o.node_index>=g.model_nodes.size() || g.model_nodes[o.node_index].state.node_path!=o.node_path) return false; auto state=g.model_nodes[o.node_index].state; if(o.position)state.position=*o.position; if(o.rotation)state.rotation=*o.rotation; if(o.scale)state.scale=*o.scale; if(o.visible)state.visible=*o.visible; if(!set(g.model_nodes,o.node_index,state))return false; }
    return update_instances(g);
}
inline std::optional<std::array<float,6>> bounds(const GeometryDevice&, std::uint32_t) { return std::nullopt; }
}
