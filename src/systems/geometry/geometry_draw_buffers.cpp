#include "geometry_draw_buffers.h"
#include "geometry_upload_batch.h"
#include <corona/resource/types/scene.h>
#include <stdexcept>

namespace Corona::Systems::GeometryDetail {
DrawBuffers make_draw_buffers(const std::vector<Resource::Vertex>& vertices,
                                     const std::vector<std::uint16_t>& indices) {
    UploadBatch uploads;
    auto result = make_draw_buffers(vertices, indices, uploads);
    uploads.finish();
    return result;
}

DrawBuffers make_draw_buffers(const std::vector<Resource::Vertex>& vertices,
                              const std::vector<std::uint16_t>& indices,
                              UploadBatch& uploads) {
    constexpr auto shared = Horizon::BufferUsage_TransferSrc | Horizon::BufferUsage_TransferDst |
                            Horizon::BufferUsage_Storage;
    const auto device_buffer = [&](std::span<const std::byte> bytes, std::uint32_t stride,
                                    Horizon::BufferUsageFlags usage, const char* name) {
        Horizon::HardwareBufferDesc desc;
        desc.element_count = bytes.size() / stride;
        desc.element_size = stride;
        desc.usage = usage | Horizon::BufferUsage_TransferDst;
        desc.cpu_access = Horizon::CpuAccessMode::None;
        desc.debug_name = name;
        Horizon::HardwareBuffer target(desc);
        auto staging = Horizon::HardwareBuffer::from_bytes(bytes, stride, Horizon::BufferUsage_TransferSrc,
                                                           "geometry.upload.staging");
        if (!target || !staging) throw std::runtime_error("Cannot allocate geometry upload buffers");
        uploads.copy_buffer(std::move(staging), target, bytes.size());
        return target;
    };
    DrawBuffers result;
    result.vertices = device_buffer(std::as_bytes(std::span(vertices)),
        sizeof(Resource::Vertex), shared | Horizon::BufferUsage_Vertex, "geometry.vertices.shared");
    // VBuffer reads uint16 indices as uint32 words. A final odd index needs a
    // complete word inside the SSBO descriptor range, even for one triangle.
    auto packed_indices = indices;
    if (packed_indices.size() & 1u) packed_indices.push_back(0);
    result.indices = device_buffer(std::as_bytes(std::span(packed_indices)),
        sizeof(std::uint16_t), shared | Horizon::BufferUsage_Index, "geometry.indices.shared");
    auto shadow = Geometry::make_shadow_draw_data(std::span<const Resource::Vertex>(vertices),
                                                  std::span<const std::uint16_t>(indices));
    // Horizon's transfer command requires a four-byte copy size. Bounds and
    // draw counts still describe the original, unpadded triangle stream.
    if (shadow.indices.size() & 1u) shadow.indices.push_back(0);
    result.shadow_indices = shadow.indices.empty() ? result.indices :
        device_buffer(std::as_bytes(std::span(shadow.indices)), sizeof(std::uint16_t),
                      Horizon::BufferUsage_Index, "geometry.shadow_indices.spatial");
    // from_bytes() creates host-preferred allocations. Only these one-shot
    // staging buffers should use it: raster/VBuffer reads must stay on the GPU.
    // The caller completes the shared batch before publishing any resident mesh.
    result.clusters = std::move(shadow.clusters);
    result.byte_size = result.vertices.get_byte_size() + result.indices.get_byte_size() +
        (result.shadow_indices.same_resource(result.indices) ? 0 : result.shadow_indices.get_byte_size());
    return result;
}
} // namespace Corona::Systems::GeometryDetail
