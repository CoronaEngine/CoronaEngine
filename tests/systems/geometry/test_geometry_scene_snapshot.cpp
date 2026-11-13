// The scene-state snapshot outside readers consume: node metadata, build state and
// counts. GPU handles never enter it; this test builds one from a hand-made device.
#include <corona/shared_data_hub.h>

#include <cassert>
#include <iostream>

int main() {
    Corona::GeometryDevice device;
    device.gpu_build_state = Corona::GeometryDevice::GpuBuildState::Ready;
    device.model_node_revision = 4;

    Corona::ModelNodes::Node root;
    root.state.node_index = 0;
    root.state.parent_index = -1;
    root.state.name = "Root";
    root.state.node_path = "Root";
    root.state.visible = false;
    root.state.effective_visible = false;

    Corona::ModelNodes::Node child;
    child.state.node_index = 1;
    child.state.parent_index = 0;
    child.state.name = "Child";
    child.state.node_path = "Root/Child";
    child.state.visible = true;

    device.model_nodes.push_back(root);
    device.model_nodes.push_back(child);
    device.mesh_handles.emplace_back();  // present but without GPU handles
    device.transform_handle = 77;
    device.ik_chains_revision = 9;
    Corona::Resource::IkChain chain;
    chain.id = "leg";
    device.ik_chains.push_back(chain);

    const auto row = Corona::make_geometry_scene_row(device);
    assert(row.model_node_total == 2);
    assert(row.model_node_roots == 1);
    assert(row.mesh_count == 1);
    assert(row.renderable_mesh_count == 0);
    assert(row.invalid_mesh_count == 1);
    assert(row.model_node_revision == 4);
    assert(row.gpu_build_state == Corona::GeometryDevice::GpuBuildState::Ready);
    assert(row.model_resource_handle == device.model_resource_handle);
    assert(row.transform_handle == 77);
    assert(row.ik_chains_revision == 9);
    assert(row.ik_chains && row.ik_chains->size() == 1);
    assert(row.ik_chains->front().id == "leg");
    assert(row.nodes && row.nodes->size() == 2);
    assert(row.nodes->at(0).visible == false);
    assert(row.nodes->at(1).node_path == "Root/Child");

    // A later edit is reflected by the next publication; the old row stays immutable.
    device.model_nodes[0].state.visible = true;
    const auto next = Corona::make_geometry_scene_row(device);
    assert(next.nodes->at(0).visible == true);
    assert(row.nodes->at(0).visible == false);

    std::cout << "GeometrySceneSnapshotTests passed\n";
    return 0;
}
