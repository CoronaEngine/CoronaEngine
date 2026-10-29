//
// Created by GitHub Copilot on 2026/4/5.
//

#include "base/shader_graph/shader_graph.h"
#include "base/scattering/material.h"
#include "base/illumination/light.h"
#include "base/import/node_desc.h"

#include <iostream>

using namespace vision;
using namespace ocarina;

namespace {

int g_pass = 0;
int g_fail = 0;

bool expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << std::endl;
        ++g_fail;
        return false;
    }
    ++g_pass;
    return true;
}

MaterialDesc make_principled_desc() {
    MaterialDesc desc;
    desc.init(ParameterSet(DataWrap::parse(R"json(
{
    "type": "principled_bsdf",
    "name": "LifecyclePrincipled",
    "param": {
        "color": { "node": "albedo", "channels": "xyz" },
        "roughness": { "node": "roughness_node", "channels": "x" }
    },
    "node_tab": {
        "albedo": {
            "type": "number",
            "param": { "value": [0.7, 0.6, 0.5] }
        },
        "roughness_node": {
            "type": "number",
            "param": { "value": 0.0 }
        }
    }
}
)json")));
    return desc;
}

LightDesc make_directional_light_desc() {
    LightDesc desc;
    desc.init(ParameterSet(DataWrap::parse(R"json(
{
    "type": "directional",
    "name": "LifecycleDirectional",
    "param": {
        "direction": [0.0, -1.0, 0.0],
        "scale": 1.0,
        "color": { "node": "light_color", "channels": "xyz" },
        "strength": { "node": "light_strength", "channels": "x" }
    },
    "node_tab": {
        "light_color": {
            "type": "number",
            "param": { "value": [1.0, 0.8, 0.6] }
        },
        "light_strength": {
            "type": "number",
            "param": { "value": 2.0 }
        }
    }
}
)json")));
    return desc;
}

bool test_root_shader_graph_resolves_blueprint() {
    auto graph = make_shared<ShaderGraph>();
    map<string, ShaderNodeDesc> node_map;
    ShaderNodeDesc value_desc{AttrTag::Number, "number"};
    value_desc.set_parameter(ParameterSet(DataWrap::object({{"value", DataWrap::array({0.1f, 0.2f, 0.3f})}})));
    node_map.emplace("value", value_desc);

    graph->initialize_root_graph();
    graph->set_graph_blueprint(node_map);

    bool ok = true;
    ok &= expect(graph->has_graph(), "root ShaderGraph should report has_graph");
    ok &= expect(graph->has_blueprint(), "root ShaderGraph should keep blueprint after build");
    ok &= expect(!graph->graph_resolved(), "root ShaderGraph should start unresolved after setting blueprint");

    graph->ensure_graph_ready();

    ok &= expect(graph->graph_resolved(), "root ShaderGraph should resolve after ensure_graph_ready");
    ok &= expect(graph->get_node("value") != nullptr, "root ShaderGraph should instantiate blueprint nodes");
    ok &= expect(graph->get_node("value")->graph().shared_graph().get() == graph.get(),
                 "instantiated ShaderNode should point back to root graph");
    return ok;
}

bool test_material_root_and_attached_lifecycle() {
    MaterialDesc desc = make_principled_desc();

    auto root = Material::create_root(desc);
    auto child = Node::create_shared<Material>(desc);

    bool ok = true;
    MaterialDesc default_desc;
    default_desc.init(ParameterSet(DataWrap::object({{"param", DataWrap::object()}})));
    ok &= expect(default_desc.sub_type == "principled_bsdf", "omitted material type should default to Principled");
    ok &= expect(root->is_root(), "root material should be marked as root");
    ok &= expect(root->has_graph(), "root material should have a graph");
    ok &= expect(root->graph_resolved(), "root material should be resolved after create_root");
    ok &= expect(root->get_node("albedo") != nullptr, "root material should resolve node_tab entries");

    ok &= expect(!child->is_root(), "attached child material should not be root before attach");
    ok &= expect(!child->has_graph(), "child material should start detached");
    ok &= expect(!child->graph_resolved(), "child material should start unresolved");

    child->initialize_attached(root->shared_graph(), desc);

    ok &= expect(!child->is_root(), "attached child material should remain non-root");
    ok &= expect(child->has_graph(), "attached child material should have graph after attach");
    ok &= expect(child->graph_resolved(), "attached child material should resolve after initialize_attached");
    ok &= expect(child->get_node("albedo") != nullptr, "attached child material should resolve node_tab entries");
    ok &= expect(child->shared_graph().get() == root->shared_graph().get(),
                 "attached child material should share parent graph handle");
    return ok;
}

bool test_light_root_and_attached_lifecycle() {
    LightDesc desc = make_directional_light_desc();

    auto root = Node::create_shared<Light>(desc);
    root->initialize_root(desc);
    auto child = Node::create_shared<Light>(desc);

    bool ok = true;
    ok &= expect(root->is_root(), "root light should be marked as root");
    ok &= expect(root->has_graph(), "root light should have a graph");
    ok &= expect(root->graph_resolved(), "root light should resolve after initialize_root");
    ok &= expect(root->get_node("light_color") != nullptr, "root light should resolve node_tab entries");

    ok &= expect(!child->has_graph(), "child light should start detached");
    ok &= expect(!child->graph_resolved(), "child light should start unresolved");

    child->initialize_attached(root->shared_graph(), desc);

    ok &= expect(child->has_graph(), "attached child light should have graph after initialize_attached");
    ok &= expect(child->graph_resolved(), "attached child light should resolve after initialize_attached");
    ok &= expect(child->get_node("light_color") != nullptr, "attached child light should resolve node_tab entries");
    ok &= expect(child->shared_graph().get() == root->shared_graph().get(),
                 "attached child light should share parent graph handle");
    return ok;
}

}// namespace

int main() {
    bool ok = true;
    ok &= test_root_shader_graph_resolves_blueprint();
    ok &= test_material_root_and_attached_lifecycle();
    ok &= test_light_root_and_attached_lifecycle();
    if (!ok) {
        std::cerr << "[test-shader-graph-lifecycle] failed, pass=" << g_pass << " fail=" << g_fail << std::endl;
        return 1;
    }
    std::cout << "ShaderGraph lifecycle test passed. pass=" << g_pass << " fail=" << g_fail << std::endl;
    return 0;
}
