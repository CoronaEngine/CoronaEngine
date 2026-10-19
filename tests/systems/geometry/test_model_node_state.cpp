#include <corona/systems/geometry/model_node_state.h>
#include <stdexcept>
int main() {
    Corona::ModelNodes::Node root, child;
    root.state.node_index=0; root.state.position={1,0,0}; root.children={1};
    child.state.node_index=1; child.state.parent_index=0; child.state.position={2,0,0};
    std::vector<Corona::ModelNodes::Node> nodes{root,child}; Corona::ModelNodes::update(nodes,0);
    if(nodes[1].world[12]!=3) throw std::runtime_error("parent transform was not propagated");

    // The scene tree only needs node totals, and it asks for them on the CEF UI
    // thread for models with thousands of nodes. Counting must therefore work on
    // the stored nodes directly instead of forcing a copy of every node state.
    const std::vector<Corona::ModelNodes::Node> no_nodes;
    if (Corona::ModelNodes::counts(no_nodes).total != 0) throw std::runtime_error("empty total");
    if (Corona::ModelNodes::counts(no_nodes).roots != 0) throw std::runtime_error("empty roots");

    const auto counted = Corona::ModelNodes::counts(nodes);
    if (counted.total != 2) throw std::runtime_error("total node count");
    if (counted.roots != 1) throw std::runtime_error("root node count");
}
