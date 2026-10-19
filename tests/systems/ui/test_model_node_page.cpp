#include <cstdint>
#include <vector>

#include "cef/model_node_page.h"

namespace {
struct Node {
    std::uint32_t node_index;
    std::uint32_t parent_index;
};
}

int main() {
    constexpr std::uint32_t root = UINT32_MAX;
    const std::vector<Node> nodes{{0, root}, {1, root}, {2, root}, {3, 1}, {4, 1}, {5, 2}};

    const auto root_page = Corona::Ui::Cef::model_node_page(nodes, -1, 1, 1);
    if (root_page.total != 3) return 1;
    if (root_page.items.size() != 1) return 2;
    if (root_page.items.front()->node_index != 1) return 3;
    if (root_page.next_offset != 2) return 4;
    if (!root_page.has_more) return 5;

    const auto children = Corona::Ui::Cef::model_node_page(nodes, 1, 0, 200);
    if (children.total != 2) return 6;
    if (children.items.size() != 2) return 7;
    if (children.items[0]->node_index != 3) return 8;
    if (children.items[1]->node_index != 4) return 9;
    if (children.has_more) return 10;

    const auto no_page = Corona::Ui::Cef::model_node_page(nodes, 1, 99, 200);
    if (!no_page.items.empty()) return 11;
    if (no_page.total != 2) return 12;
    if (no_page.has_more) return 13;
}

