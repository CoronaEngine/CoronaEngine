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

    const auto& indices = scene->get_mesh_indices(static_cast<uint32_t>(mesh_index));
    if (indices.empty()) {
        return false;
    }

    // ---- 蒙皮几何（P3）：顶点改用 GeometryDevice 每帧 CPU 蒙皮的输出 ----
    // skinned_cpu_vertices 是 GeometrySystem::update_skinned_geometry 每帧写入的
    // 单一数据源（per-mesh 原始字节，布局严格等同 Resource::Vertex 数组）。
    // 蒙皮只改顶点位置/法线、不改拓扑，故索引仍来自 Scene。
    // 若该 mesh 有蒙皮字节且数量与绑定顶点一致 → reinterpret 直接用；
    // 否则回退到 Scene 的绑定姿态顶点（静态网格 / 蒙皮数据尚未就绪）。
    const auto& bind_vertices = scene->get_mesh_vertices(static_cast<uint32_t>(mesh_index));
    if (geometry.is_skinned &&
        mesh_index < geometry.skinned_cpu_vertices.size() &&
        !geometry.skinned_cpu_vertices[mesh_index].empty()) {
        const auto& blob = geometry.skinned_cpu_vertices[mesh_index];
        constexpr std::size_t kVertexSize = sizeof(Corona::Resource::Vertex);
        if (blob.size() % kVertexSize == 0) {
            const std::size_t skinned_count = blob.size() / kVertexSize;
            // 与绑定顶点数一致才采用，防 P2 mesh 错位 / 拓扑不符
            if (skinned_count == bind_vertices.size()) {
                out_mesh.vertices.resize(skinned_count);
                std::memcpy(out_mesh.vertices.data(), blob.data(), blob.size());
                out_mesh.indices.assign(indices.begin(), indices.end());
                return true;
            }
        }
    }

    // 回退：绑定姿态顶点（静态网格，或蒙皮输出尚未就绪的首帧）
    if (bind_vertices.empty()) {
        return false;
    }
    out_mesh.vertices.assign(bind_vertices.begin(), bind_vertices.end());
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
    void* vertex_data = vertex_buffer->get_mapped_data();
    if (!vertex_data) {
        out_mesh.vertices.clear();
        return false;
    }
    std::memcpy(out_mesh.vertices.data(), vertex_data, vertex_bytes);

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
    void* index_data_ptr = index_buffer->get_mapped_data();
    if (!index_data_ptr) {
        out_mesh.vertices.clear();
        return false;
    }
    std::memcpy(index_data.data(), index_data_ptr, index_bytes);

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

[[nodiscard]] ::vision::float4x4 model_transform_to_vision_o2w(
    const Corona::ModelTransform& transform) {
    const ktm::fmat4x4 corona_mat = transform.compute_matrix();
    ::vision::float4x4 o2w = ::vision::make_float4x4(1.f);
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float value = corona_mat[col][row];
            if (row == 2) value = -value;
            if (col == 2) value = -value;
            o2w[col][row] = value;
        }
    }
    return o2w;
}

[[nodiscard]] bool append_actor_geometry_to_group(
    ::vision::Scene& scene,
    ::vision::ShapeGroup& group,
    std::uintptr_t actor_handle,
    VisionBuildResult& result) {
    auto& hub = SharedDataHub::instance();
    auto& actor_storage = hub.actor_storage();
    auto& profile_storage = hub.profile_storage();
    auto& optics_storage = hub.optics_storage();
    auto& geom_storage = hub.geometry_storage();
    auto& transform_storage = hub.model_transform_storage();

    auto actor = actor_storage.try_acquire_read(actor_handle);
    if (!actor) {
        return false;
    }

    bool group_has_instances = false;
    for (auto profile_handle : actor->profile_handles) {
        auto profile = profile_storage.try_acquire_read(profile_handle);
        if (!profile || profile->optics_handle == 0) {
            continue;
        }

        auto optics = optics_storage.try_acquire_read(profile->optics_handle);
        if (!optics || !optics->visible || optics->geometry_handle == 0) {
            continue;
        }

        auto geom = geom_storage.try_acquire_read(optics->geometry_handle);
        if (!geom) {
            continue;
        }

        // 跳过 GPU 资源未就绪的 actor（mesh_handles 为空 = Actor 首次加载中或已被卸载）。
        // 等价于 MeshSlot.valid=false。
        if (geom->mesh_handles.empty()) {
            ++result.skipped_no_data;
            continue;
        }

        ++result.candidate_count;

        ::vision::float4x4 o2w = ::vision::make_float4x4(1.f);
        if (auto transform = transform_storage.try_acquire_read(geom->transform_handle)) {
            o2w = model_transform_to_vision_o2w(*transform);
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
            for (std::size_t triangle_index = 0;
                 triangle_index + 2 < cpu_mesh.indices.size();
                 triangle_index += 3) {
                triangles.emplace_back(cpu_mesh.indices[triangle_index],
                                       cpu_mesh.indices[triangle_index + 2],
                                       cpu_mesh.indices[triangle_index + 1]);
            }

            auto mesh = std::make_shared<::vision::Mesh>(
                std::move(vertices), std::move(triangles));
            mesh = scene.geometry().data()->register_mesh(mesh);

            auto material = create_vision_material(*optics, mesh_dev);
            if (!material) {
                material = scene.obtain_black_body();
            }
            auto instance = std::make_shared<::vision::ShapeInstance>(mesh);
            instance->set_o2w(o2w);
            instance->set_material(material);
            scene.add_material(material);
            instance->init_aabb();

            group.aabb.extend(instance->aabb);
            group.add_instance(*instance);
            ++result.instance_count;
            group_has_instances = true;
        }
    }

    return group_has_instances;
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
    VisionBuildResult result;

    // 用 cbegin/cend 显式取只读迭代器（共享读锁），避免非 const range-for 命中写迭代器。
    for (auto scene_it = hub.scene_storage().cbegin(); scene_it != hub.scene_storage().cend(); ++scene_it) {
        const auto& scene_dev = *scene_it;
        if (!scene_dev.enabled) continue;

        auto group = std::make_shared<::vision::ShapeGroup>();
        bool group_has_instances = false;

        // 优先消费可见集合（视锥剔除结果），为空时回退到全量 actor 列表
        const auto& handles = scene_dev.visible_actor_handles.empty()
                                  ? scene_dev.actor_handles
                                  : scene_dev.visible_actor_handles;
        for (auto actor_handle : handles) {
            group_has_instances |=
                append_actor_geometry_to_group(scene, *group, actor_handle, result);
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

AddVisionShapeResult add_vision_shape_for_actor(
    ::vision::Scene& scene,
    std::uintptr_t actor_handle,
    const Corona::ExternalVisionBindingDevice& binding) {
    (void)binding;

    AddVisionShapeResult result;
    result.material_topology_before =
        scene.materials().topology_num();

    auto group = std::make_shared<::vision::ShapeGroup>();
    VisionBuildResult build_result;
    if (!append_actor_geometry_to_group(scene, *group, actor_handle, build_result)) {
        result.candidate_count = build_result.candidate_count;
        result.skipped_no_data = build_result.skipped_no_data;
        result.material_topology_after =
            scene.materials().topology_num();
        return result;
    }

    scene.add_shape(group);
    result.added = true;
    result.shape_index = static_cast<int>(scene.groups().size()) - 1;
    result.instance_count = build_result.instance_count;
    result.candidate_count = build_result.candidate_count;
    result.skipped_no_data = build_result.skipped_no_data;
    result.material_topology_after =
        scene.materials().topology_num();
    return result;
}

bool remove_vision_shape_for_actor(::vision::Scene& scene, unsigned int shape_index) {
    if (shape_index >= scene.groups().size()) {
        return false;
    }
    scene.remove_shape(shape_index);
    return true;
}

}  // namespace Corona::Systems::Vision

#endif  // CORONA_ENABLE_VISION
