//
// Created by Zero on 2025/4/9.
//

#include "shader_graph.h"

namespace vision {

GraphBlueprint::GraphBlueprint(map<string, ShaderNodeDesc> node_map) noexcept
    : node_map_(std::move(node_map)) {}

void GraphBlueprint::clear() noexcept {
    node_map_.clear();
}

bool GraphBlueprint::empty() const noexcept {
    return node_map_.empty();
}

GraphBlueprint GraphBuilder::build(const map<string, ShaderNodeDesc> &tab) noexcept {
    return GraphBlueprint{tab};
}

void GraphResolver::resolve(ShaderGraph &graph, const GraphBlueprint &blueprint) noexcept {
    if (blueprint.empty()) {
        return;
    }
    OC_ASSERT(graph.has_graph());
    std::vector<std::pair<SP<ShaderNode>, ShaderNodeDesc>> pending_nodes;
    pending_nodes.reserve(blueprint.node_map().size());
    for (const auto &[key, desc] : blueprint.node_map()) {
        auto shader_node = Node::create_shared<ShaderNode>(desc);
        graph.add_node(key, shader_node);
        pending_nodes.emplace_back(shader_node, desc);
    }
    for (auto &[shader_node, desc] : pending_nodes) {
        shader_node->initialize_slots(desc);
    }
}

void ShaderGraph::add_node(SP<ShaderNode> node) noexcept {
    string name = node->name();
    add_node(name, std::move(node));
}

void ShaderGraph::add_node(const std::string &name, SP<vision::ShaderNode> node) {
    if (node_map_.contains(name)) {
        return;
    }
    node->set_graph(shared_from_this());
    node_map_.insert(make_pair(name, node));
}

void ShaderGraph::clear() noexcept {
    for (auto &it : node_map_) {
        it.second->set_graph(nullptr);
    }
    node_map_.clear();
    graph_resolved_ = false;
}

void ShaderGraph::set_graph_blueprint(const map<string, ShaderNodeDesc> &tab) noexcept {
    clear();
    blueprint_ = GraphBuilder::build(tab);
}

void ShaderGraph::build_graph_blueprint(const map<string, ShaderNodeDesc> &tab) noexcept {
    set_graph_blueprint(tab);
}

void ShaderGraph::ensure_graph_ready() noexcept {
    if (graph_resolved_) {
        return;
    }
    if (!blueprint_.empty()) {
        GraphResolver::resolve(*this, blueprint_);
    }
    graph_resolved_ = true;
}

void ShaderGraph::resolve_graph() noexcept {
    ensure_graph_ready();
}

void ShaderGraph::initialize_root_graph() noexcept {
    set_is_root(true);
    ensure_graph_ready();
}

void ShaderGraph::initialize_attached_graph(const SP<ShaderGraph> &graph) noexcept {
    attach_graph(graph);
    ensure_graph_ready();
}

ShaderGraph *ShaderGraph::try_graph() noexcept {
    if (is_root_) {
        return this;
    }
    auto graph = graph_.lock();
    return graph.get();
}

const ShaderGraph *ShaderGraph::try_graph() const noexcept {
    if (is_root_) {
        return this;
    }
    auto graph = graph_.lock();
    return graph.get();
}

ShaderGraph &ShaderGraph::graph() noexcept {
    ShaderGraph *ret = try_graph();
    OC_ASSERT(ret != nullptr);
    return *ret;
}

const ShaderGraph &ShaderGraph::graph() const noexcept {
    const ShaderGraph *ret = try_graph();
    OC_ASSERT(ret != nullptr);
    return *ret;
}

SP<const ShaderGraph> ShaderGraph::try_shared_graph() const noexcept {
    if (is_root_) {
        return shared_from_this();
    }
    return graph_.lock();
}

SP<const ShaderGraph> ShaderGraph::shared_graph() const noexcept {
    auto ret = try_shared_graph();
    OC_ASSERT(ret != nullptr);
    return ret;
}

SP<ShaderGraph> ShaderGraph::try_shared_graph() noexcept {
    if (is_root_) {
        return shared_from_this();
    }
    return graph_.lock();
}

SP<ShaderGraph> ShaderGraph::shared_graph() noexcept {
    auto ret = try_shared_graph();
    OC_ASSERT(ret != nullptr);
    return ret;
}

bool ShaderGraph::has_graph() const noexcept {
    return is_root_ || !graph_.expired();
}

void ShaderGraph::attach_graph(SP<ShaderGraph> graph) noexcept {
    OC_ASSERT(!is_root_);
    graph_ = std::move(graph);
    graph_resolved_ = false;
}

void ShaderGraph::set_graph(SP<ShaderGraph> graph) noexcept {
    attach_graph(std::move(graph));
}

void ShaderGraph::attach_graph_and_resolve(const SP<ShaderGraph> &graph) noexcept {
    initialize_attached_graph(graph);
}

void ShaderGraph::set_graph_and_resolve(const SP<ShaderGraph> &graph) noexcept {
    attach_graph_and_resolve(graph);
}

bool ShaderGraph::has_blueprint() const noexcept {
    return !blueprint_.empty();
}

bool ShaderGraph::graph_resolved() const noexcept {
    return graph_resolved_;
}

ShaderNodeSlot ShaderGraph::construct_slot(const vision::ParameterSet &ps, vision::AttrTag tag) const noexcept {
    DataWrap data = ps.data();
    if (data.is_null() || (data.is_object() && data.empty())) {
        return ShaderNodeSlot{};
    }
    SP<ShaderNode> shader_node = get_node(data["node"]);
    ShaderNodeSlot slot = ShaderNodeSlot(shader_node, data["channels"], tag,
                                         ps.value("output_key").as_string());
    return slot;
}

template<typename T>
[[nodiscard]] ShaderNodeSlot ShaderGraph::construct_slot(const AttrDesc &desc, const string &attr_name,
                                                         T val, AttrTag tag) noexcept {
    ParameterSet ps = desc.value(attr_name);
    DataWrap data = ps.data();
    ShaderNodeSlot slot;
    if (data.contains("node") && data["node"].is_string()) {
        slot = construct_slot(ps, tag);
    } else {
        SlotDesc slot_desc = desc.slot(attr_name, val, tag);
        slot = ShaderNodeSlot::create_slot(slot_desc);
        slot->set_graph(shared_graph());
        // Inline compound nodes need the same initialization as node_tab
        // entries, after their graph is available to resolve child slots.
        slot->initialize_slots(slot_desc.node);
    }
    return slot;
}

ShaderNodeSlot ShaderGraph::construct_slot(const AttrDesc &desc, const string &attr_name,
                                           vision::AttrTag tag) const noexcept {
    ParameterSet ps = desc.value(attr_name);
    return construct_slot(ps, tag);
}

#define VS_INSTANCE_CONSTRUCT_SLOT(type)                                                                     \
    template ShaderNodeSlot ShaderGraph::construct_slot<type>(const AttrDesc &desc, const string &attr_name, \
                                                              type val, AttrTag tag) noexcept;

VS_INSTANCE_CONSTRUCT_SLOT(float)
VS_INSTANCE_CONSTRUCT_SLOT(float2)
VS_INSTANCE_CONSTRUCT_SLOT(float3)
VS_INSTANCE_CONSTRUCT_SLOT(float4)

#undef VS_INSTANCE_CONSTRUCT_SLOT

}// namespace vision
