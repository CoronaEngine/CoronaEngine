#pragma once
#include <corona/resource/types/scene.h>
#include <corona/systems/geometry/model_node_state.h>

namespace Corona::ModelNodes {
inline std::vector<Node> from_scene(const Resource::SceneData& scene) {
    std::vector<Node> out(scene.nodes.size());
    for (std::uint32_t i=0;i<scene.nodes.size();++i) {
        const auto& src=scene.nodes[i]; auto& dst=out[i];
        dst.base.node_index=dst.state.node_index=i; dst.base.parent_index=dst.state.parent_index=src.parent_index;
        dst.base.name=dst.state.name=src.name.empty()?"Node "+std::to_string(i):src.name;
        dst.base.node_path=dst.state.node_path=(src.parent_index>=0 && static_cast<std::size_t>(src.parent_index)<out.size()?out[static_cast<std::size_t>(src.parent_index)].state.node_path+"/":"")+dst.state.name;
        dst.base.position=dst.state.position=src.transform.position; dst.base.rotation=dst.state.rotation=src.transform.rotation; dst.base.scale=dst.state.scale=src.transform.scale;
        dst.base.visible=dst.state.visible=dst.base.effective_visible=dst.state.effective_visible=true;
        dst.base.editable=dst.state.editable=!src.has_local_matrix || finite(trs(src.transform.position,src.transform.rotation,src.transform.scale));
        dst.local_base=trs(dst.base.position,dst.base.rotation,dst.base.scale);
        if(src.parent_index>=0 && static_cast<std::size_t>(src.parent_index)<out.size()) out[static_cast<std::size_t>(src.parent_index)].children.push_back(i);
    }
    for(std::uint32_t i=0;i<out.size();++i) if(out[i].state.parent_index<0) update(out,i);
    return out;
}
}
