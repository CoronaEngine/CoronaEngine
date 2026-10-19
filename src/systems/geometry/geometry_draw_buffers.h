#pragma once
#include <horizon.h>
namespace Corona::Resource { struct Vertex; }
#include <corona/systems/geometry/mesh_draw_bounds.h>

namespace Corona::Systems::GeometryDetail {
struct DrawBuffers {
    Horizon::HardwareBuffer vertices, indices, shadow_indices;
    Geometry::MeshDrawClusters clusters;
    std::size_t byte_size{};
};

DrawBuffers make_draw_buffers(const std::vector<Resource::Vertex>& vertices,
                              const std::vector<std::uint16_t>& indices);
} // namespace Corona::Systems::GeometryDetail
