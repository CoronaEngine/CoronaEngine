#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace Corona::Ui::Cef {

template <typename Node>
struct ModelNodePage {
    std::vector<const Node*> items;
    std::size_t total{0};
    std::size_t next_offset{0};
    bool has_more{false};
};

template <typename Node>
ModelNodePage<Node> model_node_page(const std::vector<Node>& nodes,
                                    std::int64_t parent_index,
                                    std::size_t offset,
                                    std::size_t limit) {
    ModelNodePage<Node> page;
    if (parent_index < -1 || parent_index > std::numeric_limits<std::uint32_t>::max() || limit == 0) return page;
    const auto parent = parent_index < 0 ? std::numeric_limits<std::uint32_t>::max()
                                         : static_cast<std::uint32_t>(parent_index);
    for (const auto& node : nodes) {
        if (node.parent_index != parent) continue;
        if (page.total >= offset && page.items.size() < limit) page.items.push_back(&node);
        ++page.total;
    }
    page.next_offset = offset + page.items.size();
    page.has_more = page.next_offset < page.total;
    return page;
}

}  // namespace Corona::Ui::Cef
