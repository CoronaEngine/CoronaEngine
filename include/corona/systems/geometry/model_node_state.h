#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Corona::API {
struct ModelNodeState {
    std::uint32_t node_index{};
    std::int32_t parent_index{-1};
    std::string name;
    std::string node_path;
    std::array<float, 3> position{};
    std::array<float, 3> rotation{}; // degrees
    std::array<float, 3> scale{1.f, 1.f, 1.f};
    bool visible{true};
    bool effective_visible{true};
    bool editable{true};
};

struct ModelNodeOverride {
    std::uint32_t node_index{};
    std::string node_path;
    std::optional<std::array<float, 3>> position;
    std::optional<std::array<float, 3>> rotation;
    std::optional<std::array<float, 3>> scale;
    std::optional<bool> visible;
};
}

namespace Corona::ModelNodes {
using Matrix = std::array<float, 16>;
inline Matrix identity() { return {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; }
inline bool finite(const Matrix& m) { for (float v : m) if (!std::isfinite(v)) return false; return true; }
inline Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix r{};
    for (int c=0;c<4;++c) for (int row=0;row<4;++row)
        for (int k=0;k<4;++k) r[c*4+row] += a[k*4+row]*b[c*4+k];
    return r;
}
inline Matrix trs(const std::array<float,3>& p, const std::array<float,3>& d, const std::array<float,3>& s) {
    constexpr float pi=3.14159265358979323846f;
    const float x=d[0]*pi/180.f, y=d[1]*pi/180.f, z=d[2]*pi/180.f;
    const float cx=std::cos(x), sx=std::sin(x), cy=std::cos(y), sy=std::sin(y), cz=std::cos(z), sz=std::sin(z);
    Matrix r={cy*cz, cy*sz, -sy,0, sx*sy*cz-cx*sz, sx*sy*sz+cx*cz, sx*cy,0, cx*sy*cz+sx*sz, cx*sy*sz-sx*cz, cx*cy,0, p[0],p[1],p[2],1};
    for(int row=0;row<3;++row) { r[row]*=s[0]; r[4+row]*=s[1]; r[8+row]*=s[2]; }
    return r;
}
inline bool finite(const API::ModelNodeState& s) {
    for (auto a : {s.position,s.rotation,s.scale}) for(float v:a) if(!std::isfinite(v)) return false;
    return true;
}
struct Node {
    API::ModelNodeState base;
    API::ModelNodeState state;
    Matrix local_base{identity()};
    Matrix local_current{identity()};
    Matrix world{identity()};
    std::vector<std::uint32_t> children;
    std::vector<std::uint32_t> instance_indices;
    bool dirty{true};
};

/// Node totals of one model.  The scene tree asks for these on the CEF UI thread
/// for scenes with thousands of nodes, so they are counted in place and never
/// require a copy of the node state.  Roots are nodes without a parent.
struct Counts {
    std::size_t total{0};
    std::size_t roots{0};
};

[[nodiscard]] inline Counts counts(const std::vector<Node>& nodes) {
    Counts result;
    result.total = nodes.size();
    for (const auto& node : nodes) {
        if (node.state.parent_index < 0) ++result.roots;
    }
    return result;
}

inline void update(std::vector<Node>& nodes, std::uint32_t index) {
    if (index >= nodes.size()) return;
    auto& n=nodes[index];
    n.local_current=trs(n.state.position,n.state.rotation,n.state.scale);
    n.world = n.state.parent_index >= 0 && static_cast<std::size_t>(n.state.parent_index)<nodes.size()
        ? multiply(nodes[static_cast<std::size_t>(n.state.parent_index)].world,n.local_current) : n.local_current;
    n.state.effective_visible=n.state.visible && (n.state.parent_index<0 || nodes[static_cast<std::size_t>(n.state.parent_index)].state.effective_visible);
    for(auto child:n.children) update(nodes,child);
    n.dirty=false;
}
inline bool set(std::vector<Node>& nodes, std::uint32_t index, const API::ModelNodeState& value) {
    if(index>=nodes.size() || !nodes[index].state.editable || !finite(value)) return false;
    auto next=value; next.node_index=nodes[index].state.node_index; next.parent_index=nodes[index].state.parent_index;
    next.name=nodes[index].state.name; next.node_path=nodes[index].state.node_path; next.editable=nodes[index].state.editable;
    nodes[index].state=std::move(next); update(nodes,index); return finite(nodes[index].world);
}
}
