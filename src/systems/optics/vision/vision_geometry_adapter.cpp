#include "vision_geometry_adapter.h"

#ifdef CORONA_ENABLE_VISION

#include <corona/kernel/core/i_logger.h>
#include <corona/resource/resource_manager.h>
#include <corona/resource/types/scene.h>
#include <corona/shared_data_hub.h>

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

#include "base/mgr/geometry.h"
#include "base/mgr/scene.h"
#include "base/shape.h"
#include "base/using.h"
#include "math/basic_types.h"
#include "vision_material_adapter.h"

namespace Corona::Systems::Vision {

namespace {

struct CpuMeshData {
    std::vector<Corona::Resource::Vertex> vertices;
    std::vector<uint32_t> indices;
};

[[nodiscard]] auto load_cpu_mesh_from_resource(const GeometryDevice& geometry,
                                               std::size_t mesh_index,
                                               CpuMeshData& out_mesh) -> bool {
    if (geometry.model_resource_handle == 0) {
        return false;
    }

    auto resource = SharedDataHub::instance().model_resource_storage().try_acquire_read(geometry.model_resource_handle);
    if (!resource || resource->model_id == 0) {
        return false;
    }

    auto scene = Resource::ResourceManager::get_instance().acquire_read<Resource::Scene>(resource->model_id);
    if (!scene || mesh_index >= scene->data.meshes.size()) {
        return false;
    }

    const auto& vertices = scene->get_mesh_vertices(static_cast<uint32_t>(mesh_index));
    const auto& indices = scene->get_mesh_indices(static_cast<uint32_t>(mesh_index));
    if (vertices.empty() || indices.empty()) {
        return false;
    }

    out_mesh.vertices.assign(vertices.begin(), vertices.end());
    out_mesh.indices.assign(indices.begin(), indices.end());
    return true;
}

[[nodiscard]] auto load_cpu_mesh_from_buffers(const MeshDevice& mesh_dev,
                                              CpuMeshData& out_mesh) -> bool {
    Corona::Horizon::HardwareBuffer const* vertex_buffer = &mesh_dev.vertexBuffer;
    if (!(*vertex_buffer) || vertex_buffer->get_element_count() == 0) {
        vertex_buffer = &mesh_dev.vertexStorageBuffer;
    }
    if (!(*vertex_buffer) || vertex_buffer->get_element_count() == 0) {
        return false;
    }

    const uint64_t vertex_bytes = vertex_buffer->get_element_count() * vertex_buffer->get_element_size();
    constexpr uint64_t vertex_stride = sizeof(Corona::Resource::Vertex);
    if (vertex_bytes == 0 || vertex_bytes % vertex_stride != 0) {
        return false;
    }

    out_mesh.vertices.resize(static_cast<std::size_t>(vertex_bytes / vertex_stride));
    if (!vertex_buffer->read_bytes(std::as_writable_bytes(std::span(out_mesh.vertices)))) {
        out_mesh.vertices.clear();
        return false;
    }

    Corona::Horizon::HardwareBuffer const* index_buffer = &mesh_dev.indexBuffer;
    if (!(*index_buffer) || index_buffer->get_element_count() == 0) {
        index_buffer = &mesh_dev.indexStorageBuffer;
    }
    if (!(*index_buffer) || index_buffer->get_element_count() == 0) {
        out_mesh.vertices.clear();
        return false;
    }

    const uint64_t index_bytes = index_buffer->get_element_count() * index_buffer->get_element_size();
    const uint64_t element_size = index_buffer->get_element_size();
    if (index_bytes == 0 || (element_size != 2 && element_size != 4)) {
        out_mesh.vertices.clear();
        return false;
    }

    std::vector<uint8_t> index_data(index_bytes);
    if (!index_buffer->read_bytes(std::as_writable_bytes(std::span(index_data)))) {
        out_mesh.vertices.clear();
        return false;
    }

    out_mesh.indices.clear();
    out_mesh.indices.reserve(static_cast<std::size_t>(index_bytes / element_size));
    if (element_size == 2) {
        const auto* indices16 = reinterpret_cast<const uint16_t*>(index_data.data());
        const auto count = index_bytes / element_size;
        for (uint64_t i = 0; i < count; ++i) {
            out_mesh.indices.push_back(indices16[i]);
        }
    } else {
        const auto* indices32 = reinterpret_cast<const uint32_t*>(index_data.data());
        const auto count = index_bytes / element_size;
        for (uint64_t i = 0; i < count; ++i) {
            out_mesh.indices.push_back(indices32[i]);
        }
    }

    return !out_mesh.vertices.empty() && !out_mesh.indices.empty();
}

}  // namespace

VisionBuildResult build_vision_geometry(::vision::Scene& scene) {
    // Full clear so repeated rebuilds (dynamic import/export) do not accumulate
    // orphaned meshes. clear_shapes() only drops instances_/groups_; the mesh
    // registry (mesh_map_/meshes_) must also be cleared, otherwise meshes of
    // removed objects survive across rebuilds and keep getting re-indexed and
    // re-uploaded by prepare_geometry() -> tidy_up_meshes()/upload(), leaking
    // GPU memory that grows monotonically with each import. The subsequent loop
    // re-registers every currently-present mesh via register_mesh() (hash-deduped).
    scene.clear_shapes();
    scene.geometry().data()->clear_meshes();

    auto& hub = SharedDataHub::instance();
    auto& actor_storage = hub.actor_storage();
    auto& profile_storage = hub.profile_storage();
    auto& optics_storage = hub.optics_storage();
    auto& geom_storage = hub.geometry_storage();
    auto& transform_storage = hub.model_transform_storage();

    VisionBuildResult result;

    // 用 cbegin/cend 显式取只读迭代器（共享读锁），避免非 const range-for 命中写迭代器。
    for (auto scene_it = hub.scene_storage().cbegin(); scene_it != hub.scene_storage().cend(); ++scene_it) {
        const auto& scene_dev = *scene_it;
        if (!scene_dev.enabled) continue;

        auto group = std::make_shared<::vision::ShapeGroup>();
        bool group_has_instances = false;

        for (auto actor_handle : scene_dev.actor_handles) {
            auto actor = actor_storage.try_acquire_read(actor_handle);
            if (!actor) continue;

            for (auto profile_handle : actor->profile_handles) {
                auto profile = profile_storage.try_acquire_read(profile_handle);
                if (!profile || profile->optics_handle == 0) continue;

                auto optics = optics_storage.try_acquire_read(profile->optics_handle);
                if (!optics || !optics->visible) continue;

                // Drive geometry lookup from the OpticsDevice's own handle to stay
                // consistent with optics_pipeline()/compute_vision_scene_signature().
                // The profile->geometry_handle guard alone is insufficient: the two
                // handles may diverge and indexing geometry by the wrong one silently
                // drops the object.
                if (optics->geometry_handle == 0) continue;

                auto geom = geom_storage.try_acquire_read(optics->geometry_handle);
                if (!geom) continue;

                // This object is a render candidate: it passed every visibility /
                // linkage filter and is expected to contribute geometry. Count it so
                // the caller can tell "empty scene" from "data not ready yet".
                ++result.candidate_count;

                // Build the object-to-world transform
                ::vision::float4x4 o2w = ::vision::make_float4x4(1.f);
                if (auto transform = transform_storage.try_acquire_read(geom->transform_handle)) {
                    ktm::fmat4x4 corona_mat = transform->compute_matrix();
                    // Corona/Native uses +Z-forward left-handed coordinates.
                    // Vision uses -Z-forward coordinates, so convert object
                    // transforms by F * M * F where F = diag(1, 1, -1, 1).
                    for (int col = 0; col < 4; ++col) {
                        for (int row = 0; row < 4; ++row) {
                            float value = corona_mat[col][row];
                            if (row == 2) value = -value;
                            if (col == 2) value = -value;
                            o2w[col][row] = value;
                        }
                    }
                }

                for (std::size_t mesh_index = 0; mesh_index < geom->mesh_handles.size(); ++mesh_index) {
                    auto& mesh_dev = geom->mesh_handles[mesh_index];
                    CpuMeshData cpu_mesh;
                    if (!load_cpu_mesh_from_resource(*geom, mesh_index, cpu_mesh) &&
                        !load_cpu_mesh_from_buffers(mesh_dev, cpu_mesh)) {
                        ++result.skipped_no_data;
                        CFW_LOG_WARNING(
                            "Vision geometry adapter: no CPU mesh data available, skipping mesh "
                            "(actor={}, geometry_handle={}, model_resource_handle={}, mesh_index={})",
                            actor_handle, optics->geometry_handle, geom->model_resource_handle,
                            mesh_index);
                        continue;
                    }

                    std::vector<::vision::Vertex> vertices;
                    vertices.reserve(cpu_mesh.vertices.size());
                    for (const auto& src_vertex : cpu_mesh.vertices) {
                        ::vision::Vertex v;
                        v.pos = {src_vertex.position[0], src_vertex.position[1], -src_vertex.position[2]};
                        v.n   = {src_vertex.normal[0], src_vertex.normal[1], -src_vertex.normal[2]};
                        v.uv  = {src_vertex.tex_coords[0], src_vertex.tex_coords[1]};
                        v.uv2 = {0.f, 0.f};
                        vertices.push_back(v);
                    }

                    std::vector<::vision::Triangle> triangles;
                    triangles.reserve(cpu_mesh.indices.size() / 3);
                    if (cpu_mesh.indices.size() < 3) {
                        continue;
                    }
                    for (std::size_t triangle_index = 0; triangle_index + 2 < cpu_mesh.indices.size(); triangle_index += 3) {
                        triangles.emplace_back(cpu_mesh.indices[triangle_index],
                                               cpu_mesh.indices[triangle_index + 2],
                                               cpu_mesh.indices[triangle_index + 1]);
                    }

                    // Create Vision Mesh. Device upload is owned by the shared
                    // scene GPU resource and runs in Pipeline::prepare_geometry().
                    auto mesh = std::make_shared<::vision::Mesh>(
                        std::move(vertices), std::move(triangles));
                    mesh = scene.geometry().data()->register_mesh(mesh);

                    // Create material and ShapeInstance
                    auto material = create_vision_material(*optics, mesh_dev);
                    if (!material) {
                        // Fallback so the instance always has a valid material id.
                        // Without this, fill_instances() leaves material_id unset and
                        // shading reads an invalid/empty material entry -> crash.
                        material = scene.obtain_black_body();
                    }
                    auto instance = std::make_shared<::vision::ShapeInstance>(mesh);
                    instance->set_o2w(o2w);
                    instance->set_material(material);
                    scene.add_material(material);

                    group->add_instance(*instance);
                    ++result.instance_count;
                    group_has_instances = true;
                }
            }
        }

        if (group_has_instances) {
            scene.add_shape(group);
        }
    }

    // Finalize: encode material/mesh IDs into instance handles and register with geometry.
    // BVH build + device upload are intentionally left to Pipeline::prepare_geometry(),
    // which runs reset_device_buffer() + upload(stream) + build_accel(stream) during prepare();
    // building here as well would just be a redundant full BVH rebuild.
    scene.fill_instances();
    scene.update_geometry_instances();
    return result;
}

}  // namespace Corona::Systems::Vision

#endif  // CORONA_ENABLE_VISION
