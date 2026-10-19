#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace Corona::Geometry {

struct MeshDrawBounds {
    std::uint32_t first_index{}, index_count{};
    std::array<float, 3> minimum{}, maximum{};
    bool valid{true};
};
using MeshDrawClusters = std::shared_ptr<const std::vector<MeshDrawBounds>>;

// Preserve the exact index stream. These CPU-only bounds merely allow whole
// contiguous triangle ranges to be rejected before a depth draw.
template <typename Vertex, typename Index>
[[nodiscard]] MeshDrawClusters make_draw_clusters(std::span<const Vertex> vertices,
                                                  std::span<const Index> indices) {
    constexpr std::size_t indices_per_cluster = 128 * 3;
    if (indices.empty() || indices.size() % 3 != 0 || indices.size() > UINT32_MAX) return {};
    auto result = std::make_shared<std::vector<MeshDrawBounds>>();
    result->reserve((indices.size() + indices_per_cluster - 1) / indices_per_cluster);
    for (std::size_t first = 0; first < indices.size(); first += indices_per_cluster) {
        MeshDrawBounds cluster;
        cluster.first_index = static_cast<std::uint32_t>(first);
        cluster.index_count = static_cast<std::uint32_t>(std::min(indices_per_cluster, indices.size() - first));
        cluster.minimum.fill(std::numeric_limits<float>::max());
        cluster.maximum.fill(std::numeric_limits<float>::lowest());
        for (std::size_t i = first; i < first + cluster.index_count; ++i) {
            const auto index = static_cast<std::size_t>(indices[i]);
            if (index >= vertices.size()) { cluster.valid = false; continue; }
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const float value = vertices[index].position[axis];
                cluster.valid = cluster.valid && std::isfinite(value);
                cluster.minimum[axis] = std::min(cluster.minimum[axis], value);
                cluster.maximum[axis] = std::max(cluster.maximum[axis], value);
            }
        }
        result->push_back(cluster);
    }
    return result;
}

struct ShadowDrawData {
    std::vector<std::uint16_t> indices;
    MeshDrawClusters clusters;
};

// Only the depth pass uses this order. Every triangle retains its three vertex
// indices and winding; visibility/material primitive IDs keep their old order.
template <typename Vertex>
[[nodiscard]] ShadowDrawData make_shadow_draw_data(std::span<const Vertex> vertices,
                                                   std::span<const std::uint16_t> indices) {
    if (indices.empty() || indices.size() % 3 != 0 || indices.size() > UINT32_MAX) return {};
    std::array<double, 3> minimum, maximum;
    minimum.fill(std::numeric_limits<double>::max());
    maximum.fill(std::numeric_limits<double>::lowest());
    for (auto index : indices) {
        if (index >= vertices.size()) return {};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const double p = vertices[index].position[axis];
            if (!std::isfinite(p)) return {};
            minimum[axis] = std::min(minimum[axis], p);
            maximum[axis] = std::max(maximum[axis], p);
        }
    }
    const auto spread = [](std::uint32_t value) {
        value = (value | (value << 16)) & 0x030000ffu;
        value = (value | (value << 8)) & 0x0300f00fu;
        value = (value | (value << 4)) & 0x030c30c3u;
        return (value | (value << 2)) & 0x09249249u;
    };
    struct Triangle { std::uint32_t morton, first; };
    std::vector<Triangle> triangles;
    triangles.reserve(indices.size() / 3);
    for (std::size_t first = 0; first < indices.size(); first += 3) {
        std::uint32_t code = 0;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const double center = (double(vertices[indices[first]].position[axis]) +
                vertices[indices[first + 1]].position[axis] + vertices[indices[first + 2]].position[axis]) / 3;
            const double extent = maximum[axis] - minimum[axis];
            const auto coordinate = static_cast<std::uint32_t>(std::clamp(
                extent > 0 ? (center - minimum[axis]) / extent * 1023 : 0.0, 0.0, 1023.0));
            code |= spread(coordinate) << axis;
        }
        triangles.push_back({code, static_cast<std::uint32_t>(first)});
    }
    std::sort(triangles.begin(), triangles.end(), [](const auto& a, const auto& b) {
        return a.morton != b.morton ? a.morton < b.morton : a.first < b.first;
    });
    ShadowDrawData result;
    result.indices.reserve(indices.size());
    for (const auto& triangle : triangles)
        result.indices.insert(result.indices.end(), indices.begin() + triangle.first, indices.begin() + triangle.first + 3);
    result.clusters = make_draw_clusters(vertices, std::span<const std::uint16_t>(result.indices));
    return result;
}

} // namespace Corona::Geometry
