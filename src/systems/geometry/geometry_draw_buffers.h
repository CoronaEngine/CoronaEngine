#pragma once
#include <horizon.h>
#include <vector>

namespace Corona::Resource { struct Vertex; }

namespace Corona::Systems::GeometryDetail {
class UploadBatch;
struct DrawBuffers {
    Horizon::HardwareBuffer vertices, indices;
    std::size_t byte_size{};
};

DrawBuffers make_draw_buffers(const std::vector<Resource::Vertex>& vertices,
                              const std::vector<std::uint16_t>& indices);
DrawBuffers make_draw_buffers(const std::vector<Resource::Vertex>& vertices,
                              const std::vector<std::uint16_t>& indices,
                              UploadBatch& uploads);
} // namespace Corona::Systems::GeometryDetail
