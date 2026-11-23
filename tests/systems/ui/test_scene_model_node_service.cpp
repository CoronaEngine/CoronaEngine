// 模型节点领域服务（WNC-012）：纯引擎节点数据 + JSON，无 CEF/V8/JS 路由依赖。
// 该测试只包含服务头（连同纯数据 DTO），不链接 CEF、不自建 scene 单例。
#include "editor_api/editor_api_services.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace Corona;
using namespace Corona::Systems::UI;
using Corona::API::ModelNodeState;

namespace {

int checks = 0;

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

ModelNodeState node(std::uint32_t index, std::int32_t parent, std::string name) {
    ModelNodeState value;
    value.node_index = index;
    value.parent_index = parent;
    value.name = std::move(name);
    value.node_path = "path/" + value.name;
    return value;
}

std::vector<ModelNodeState> model() {
    return {
        node(0, -1, "Root"),
        node(1, 0, "Body"),
        node(2, 1, "Arm_L"),
        node(3, 1, "Arm_R"),
        node(4, -1, "Head"),
    };
}

void test_snapshot_projection() {
    auto value = node(1, 0, "Body");
    value.visible = true;
    value.effective_visible = true;
    value.editable = false;
    value.position = {1.f, 2.f, 3.f};
    const auto hidden = SceneModelNodeService::make_snapshot(value, 2, false);
    check(hidden.node_index == 1 && hidden.parent_index == 0, "identity preserved");
    check(hidden.name == "Body" && hidden.node_path == "path/Body", "names preserved");
    check(hidden.position == value.position, "transform preserved");
    check(hidden.effective_visible == false, "actor visibility gates effective visibility");
    check(hidden.child_count == 2 && hidden.editable == false, "child count and editable preserved");
    const auto shown = SceneModelNodeService::make_snapshot(value, 0, true);
    check(shown.effective_visible, "visible actor keeps effective visibility");
}

void test_page_projection() {
    const auto nodes = model();
    ModelNodePageRequest roots;
    roots.parent_index = -1;
    roots.offset = 0;
    roots.limit = 2;
    const auto first = SceneModelNodeService::project_page(nodes, roots, true);
    check(first.total == 2, "two roots total");
    check(first.nodes.size() == 2, "two roots fit the page");
    check(first.has_more == false && first.next_offset == 2, "single root page is complete");
    check(first.nodes[0].node_index == 0 && first.nodes[1].node_index == 4, "root order preserved");
    check(first.nodes[0].child_count == 1, "root child count = Body");
    check(first.nodes[1].child_count == 0, "headless root has no children");

    ModelNodePageRequest children;
    children.parent_index = 0;
    children.offset = 0;
    children.limit = 1;
    const auto page = SceneModelNodeService::project_page(nodes, children, true);
    check(page.total == 1 && page.nodes.size() == 1, "single child page");
    check(page.has_more == false && page.next_offset == 1, "child page complete");
    check(page.nodes[0].node_index == 1 && page.nodes[0].child_count == 2, "child page order and count");

    ModelNodePageRequest sub;
    sub.parent_index = 1;
    sub.offset = 0;
    sub.limit = 10;
    const auto arms = SceneModelNodeService::project_page(nodes, sub, true);
    check(arms.total == 2 && arms.nodes.size() == 2, "grandchildren listed");
    check(arms.nodes[0].node_index == 2 && arms.nodes[1].node_index == 3, "grandchild order preserved");
    check(arms.nodes[0].child_count == 0, "leaf child count is zero");

    ModelNodePageRequest empty;
    empty.parent_index = -1;
    empty.offset = 0;
    empty.limit = 0;
    check(SceneModelNodeService::project_page(nodes, empty, true).nodes.empty(), "zero limit yields empty page");
}

void test_patch_derivation() {
    auto base = node(2, 1, "Arm_L");
    base.position = {1.f, 1.f, 1.f};
    base.rotation = {0.f, 0.f, 0.f};
    base.scale = {1.f, 1.f, 1.f};
    base.visible = true;
    ModelNodePatch patch;
    patch.position = std::array<float, 3>{4.f, 5.f, 6.f};
    patch.visible = false;
    const auto state = SceneModelNodeService::derive_patched_state(base, patch);
    check(state.position == std::array<float, 3>{4.f, 5.f, 6.f}, "position applied");
    check(state.visible == false, "explicit false visibility applied");
    check(state.rotation == base.rotation && state.scale == base.scale, "unpatched fields stay");
    check(base.position == std::array<float, 3>{1.f, 1.f, 1.f} && base.visible, "base state not mutated");
    check(state.node_index == 2 && state.node_path == "path/Arm_L", "identity preserved by patch");

    ModelNodePatch empty;
    const auto unchanged = SceneModelNodeService::derive_patched_state(base, empty);
    check(unchanged.position == base.position && unchanged.visible == base.visible, "empty patch keeps state");
}

void test_node_keys() {
    const auto keys = SceneModelNodeService::node_keys(model());
    check(keys.is_array() && keys.size() == 5, "one key per node");
    check(keys[0]["node_index"] == 0 && keys[0]["node_path"] == "path/Root", "key shape");
    check(keys[4]["node_index"] == 4 && keys[4]["node_path"] == "path/Head", "key order");
}

}  // namespace

int main() {
    test_snapshot_projection();
    test_page_projection();
    test_patch_derivation();
    test_node_keys();
    std::cout << "scene_model_node_service: " << checks << " checks passed\n";
    return 0;
}
