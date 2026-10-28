#include "vision_geometry_adapter.h"

#include <horizon/core/logging.h>
#include <corona/resource/resource_manager.h>
#include <corona/resource/types/scene.h>
#include <corona/shared_data_hub.h>
#include <corona/systems/geometry/model_node_geometry.h>
#include <corona/systems/geometry/model_node_import.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>
#include <unordered_map>
#include <unordered_set>

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
                                               const Resource::Scene& source,
                                               std::size_t mesh_index,
                                               CpuMeshData& out_mesh) -> bool {
    if (mesh_index >= source.data.meshes.size()) {
        return false;
    }

    const auto& indices = source.get_mesh_indices(static_cast<uint32_t>(mesh_index));
    if (indices.empty()) {
        return false;
    }

    // ---- 蒙皮几何：读取 GeometryDevice 每帧 GPU 蒙皮的 LOD0 回读 ----
    // skinned_cpu_vertices 由 MechanicsSystem::update_skinned_geometry 写入，
    // 为 per-mesh 原始字节，布局严格等同 Resource::Vertex 数组。
    // 蒙皮只改顶点位置/法线、不改拓扑，故索引仍来自 Scene。
    // 若该 mesh 有蒙皮字节且数量与绑定顶点一致 → reinterpret 直接用；
    // 否则回退到 Scene 的绑定姿态顶点（静态网格 / 蒙皮数据尚未就绪）。
    const auto& bind_vertices = source.get_mesh_vertices(static_cast<uint32_t>(mesh_index));
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

// Geometry is staged independently of raster residency. Holding copied CPU data
// also lets an asynchronous import fail without clearing the last complete scene.
struct PreparedMesh {
    CpuMeshData data;
    std::array<float, 4> color{1.f, 1.f, 1.f, 1.f};
    std::shared_ptr<const MeshMaterialDevice> material;
    uint32_t material_index{};
};

struct PreparedInstance {
    std::size_t mesh_index{};
    std::size_t draw_index{};
    ::vision::float4x4 o2w{::vision::make_float4x4(1.f)};
};

struct PreparedGeometry {
    std::uintptr_t geometry_handle{};
    OpticsDevice optics;
    std::unordered_map<std::size_t, PreparedMesh> meshes;
    std::vector<PreparedInstance> instances;
};

using PreparedGroup = std::vector<PreparedGeometry>;

[[nodiscard]] ::vision::float4x4 matrix_to_vision(const ktm::fmat4x4& source) {
    ::vision::float4x4 result = ::vision::make_float4x4(1.f);
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            result[column][row] = source[column][row] *
                ((row == 2) != (column == 2) ? -1.f : 1.f);
        }
    }
    return result;
}

[[nodiscard]] bool valid_mesh(const CpuMeshData& mesh) {
    if (mesh.vertices.empty() || mesh.indices.empty() || mesh.indices.size() % 3 != 0) {
        return false;
    }
    for (const auto index : mesh.indices) {
        if (index >= mesh.vertices.size()) return false;
    }
    for (const auto& vertex : mesh.vertices) {
        for (int axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(vertex.position[axis])) return false;
        }
    }
    return true;
}

void stage_actor_geometry(std::uintptr_t actor_handle,
                          PreparedGroup& group,
                          VisionBuildResult& result) {
    auto& hub = SharedDataHub::instance();
    auto actor = hub.actor_storage().try_acquire_read(actor_handle);
    if (!actor) {
        ++result.skipped_no_data;
        return;
    }

    for (const auto profile_handle : actor->profile_handles) {
        auto profile = hub.profile_storage().try_acquire_read(profile_handle);
        if (!profile) {
            ++result.skipped_no_data;
            continue;
        }
        if (profile->optics_handle == 0) continue;
        auto optics = hub.optics_storage().try_acquire_read(profile->optics_handle);
        if (!optics) {
            ++result.skipped_no_data;
            continue;
        }
        if (!optics->visible || optics->geometry_handle == 0) continue;
        ++result.candidate_count;
        auto geometry = hub.geometry_storage().try_acquire_read(optics->geometry_handle);
        if (!geometry) {
            ++result.skipped_no_data;
            continue;
        }
        if (geometry->gpu_build_state == GeometryDevice::GpuBuildState::Failed) {
            ++result.invalid_data;
            continue;
        }

        Resource::ReadHandle<Resource::Scene> source;
        if (geometry->model_resource_handle != 0) {
            auto resource = hub.model_resource_storage().try_acquire_read(geometry->model_resource_handle);
            if (resource && resource->model_id != 0) {
                source = Resource::ResourceManager::get_instance().acquire_read<Resource::Scene>(resource->model_id);
            }
        }
        if (!source && geometry->mesh_handles.empty()) {
            ++result.skipped_no_data;
            continue;
        }

        PreparedGeometry prepared;
        prepared.geometry_handle = optics->geometry_handle;
        prepared.optics = *optics;
        auto object_to_world = ::vision::make_float4x4(1.f);
        if (geometry->transform_handle != 0) {
            auto transform = hub.model_transform_storage().try_acquire_read(geometry->transform_handle);
            if (!transform) {
                ++result.skipped_no_data;
                continue;
            }
            object_to_world = matrix_to_vision(transform->compute_matrix());
        }

        // Runtime instances include edited node transforms/visibility. Before the
        // raster upload exists, use the identical imported CPU instance table.
        const auto* current_instances = &geometry->mesh_instances;
        std::optional<GeometryDevice> imported_node_storage;
        if (current_instances->empty() && source && !geometry->model_node_overrides.empty()) {
            // Persisted node edits may arrive before the native GPU build. Apply
            // the same node-delta code to CPU instances rather than briefly
            // exposing unedited source geometry in Vision.
            auto& imported_nodes = imported_node_storage.emplace();
            imported_nodes.model_nodes = ModelNodes::from_scene(source->data);
            imported_nodes.model_node_overrides = geometry->model_node_overrides;
            imported_nodes.node_instance_prefix = source->data.node_instance_prefix;
            imported_nodes.has_node_instance_prefix = source->data.has_node_instance_prefix;
            for (const auto& instance : source->data.mesh_instances) {
                MeshInstanceDevice converted;
                converted.mesh_index = instance.mesh_index;
                converted.node_index = instance.node_index;
                converted.transform = ModelNodes::ktm_matrix(instance.transform);
                converted.original_transform = converted.transform;
                imported_nodes.mesh_instances.push_back(converted);
            }
            if (imported_nodes.mesh_instances.empty()) {
                for (std::uint32_t mesh_index = 0; mesh_index < source->data.meshes.size(); ++mesh_index) {
                    MeshInstanceDevice instance;
                    instance.mesh_index = mesh_index;
                    instance.node_delta_prefix = !source->data.meshes[mesh_index].bone_weights.empty() && source->data.skeleton
                        ? source->data.skeleton->global_inverse : source->data.node_instance_prefix;
                    for (std::uint32_t node_index = 0; node_index < source->data.nodes.size(); ++node_index) {
                        if (source->data.nodes[node_index].mesh_index == mesh_index) {
                            instance.node_index = node_index;
                            break;
                        }
                    }
                    imported_nodes.mesh_instances.push_back(instance);
                }
            }
            if (!ModelNodes::apply_overrides(imported_nodes)) {
                ++result.invalid_data;
                continue;
            }
            current_instances = &imported_nodes.mesh_instances;
        }
        const bool runtime_instances = !current_instances->empty();
        const bool source_instances = !runtime_instances && source && !source->data.mesh_instances.empty();
        const std::size_t draw_count = runtime_instances ? current_instances->size()
            : source_instances ? source->data.mesh_instances.size()
            : source ? source->data.meshes.size() : geometry->mesh_handles.size();
        if (draw_count == 0) {
            ++result.invalid_data;
            continue;
        }
        std::unordered_set<std::size_t> unavailable_meshes;
        for (std::size_t draw_index = 0; draw_index < draw_count; ++draw_index) {
            std::size_t mesh_index = draw_index;
            auto local = ::vision::make_float4x4(1.f);
            if (runtime_instances) {
                const auto& instance = (*current_instances)[draw_index];
                if (!instance.visible) continue;
                mesh_index = instance.mesh_index;
                local = matrix_to_vision(instance.transform);
            } else if (source_instances) {
                const auto& instance = source->data.mesh_instances[draw_index];
                mesh_index = instance.mesh_index;
                ktm::fmat4x4 source_matrix = ktm::fmat4x4::from_eye();
                for (int column = 0; column < 4; ++column)
                    for (int row = 0; row < 4; ++row)
                        source_matrix[column][row] = instance.transform[column * 4 + row];
                local = matrix_to_vision(source_matrix);
            }
            if (unavailable_meshes.contains(mesh_index)) continue;
            if (!prepared.meshes.contains(mesh_index)) {
                PreparedMesh mesh;
                const MeshDevice* native_mesh = mesh_index < geometry->mesh_handles.size()
                    ? &geometry->mesh_handles[mesh_index] : nullptr;
                const bool source_mesh = source && mesh_index < source->data.meshes.size();
                if (!source_mesh && !native_mesh) {
                    ++result.invalid_data;
                    unavailable_meshes.insert(mesh_index);
                    continue;
                }
                const bool loaded = (source_mesh && load_cpu_mesh_from_resource(*geometry, *source, mesh_index, mesh.data)) ||
                    (native_mesh && load_cpu_mesh_from_buffers(*native_mesh, mesh.data));
                if (!loaded || !valid_mesh(mesh.data)) {
                    // Existing CPU assets with malformed triangles are permanent
                    // failures; missing mapped raster data can become ready later.
                    if (source_mesh || loaded) ++result.invalid_data;
                    else ++result.skipped_no_data;
                    unavailable_meshes.insert(mesh_index);
                    CFW_LOG_WARNING("Vision geometry adapter: unavailable/invalid mesh "
                                    "(actor={}, geometry={}, mesh={}, cpu_source={})",
                                    actor_handle, optics->geometry_handle, mesh_index, source_mesh);
                    continue;
                }
                if (native_mesh) {
                    mesh.color = native_mesh->materialColor;
                    mesh.material=native_mesh->material;
                    mesh.material_index=native_mesh->materialIndex;
                } else if (source_mesh) {
                    const auto material_index = source->data.meshes[mesh_index].material_index;
                    if (material_index < source->data.materials.size()) {
                        mesh.color = source->data.materials[material_index].base_color;
                        auto material=std::make_shared<MeshMaterialDevice>();
                        material->source=source->data.materials[material_index];
                        mesh.material=std::move(material);
                        mesh.material_index=material_index;
                    }
                }
                prepared.meshes.emplace(mesh_index, std::move(mesh));
            }
            prepared.instances.push_back({mesh_index, draw_index, object_to_world * local});
            ++result.instance_count;
        }
        if (!prepared.instances.empty()) group.push_back(std::move(prepared));
    }
}

std::vector<PreparedGroup> stage_scene_geometry(VisionBuildResult& result) {
    std::vector<PreparedGroup> groups;
    auto& scenes = SharedDataHub::instance().scene_storage();
    for (auto scene_it = scenes.cbegin(); scene_it != scenes.cend(); ++scene_it) {
        const auto& scene = *scene_it;
        if (!scene.enabled) continue;
        PreparedGroup group;
        // Raster frustum culling must not remove offscreen shadows, reflections
        // or indirect-light geometry from the ray-tracing scene.
        for (const auto actor : scene.actor_handles) stage_actor_geometry(actor, group, result);
        if (!group.empty()) groups.push_back(std::move(group));
    }
    return groups;
}

void append_prepared_group(::vision::Scene& scene, PreparedGroup& prepared_group) {
    auto group = std::make_shared<::vision::ShapeGroup>();
    unsigned instance_index=static_cast<unsigned>(scene.instances().size());
    for (auto& prepared : prepared_group) {
        std::unordered_map<std::size_t, std::shared_ptr<::vision::Mesh>> meshes;
        std::unordered_map<std::size_t, std::shared_ptr<::vision::Material>> materials;
        for (auto& [mesh_index, source] : prepared.meshes) {
            std::vector<::vision::Vertex> vertices;
            vertices.reserve(source.data.vertices.size());
            for (const auto& vertex : source.data.vertices) {
                ::vision::Vertex converted;
                converted.pos = {vertex.position[0], vertex.position[1], -vertex.position[2]};
                converted.n = {vertex.normal[0], vertex.normal[1], -vertex.normal[2]};
                converted.uv = {vertex.tex_coords[0], vertex.tex_coords[1]};
                converted.uv2 = {0.f, 0.f};
                vertices.push_back(converted);
            }
            std::vector<::vision::Triangle> triangles;
            triangles.reserve(source.data.indices.size() / 3);
            for (std::size_t i = 0; i < source.data.indices.size(); i += 3) {
                triangles.emplace_back(source.data.indices[i], source.data.indices[i + 2], source.data.indices[i + 1]);
            }
            auto mesh = std::make_shared<::vision::Mesh>(std::move(vertices), std::move(triangles));
            meshes.emplace(mesh_index, scene.geometry().data()->register_mesh(mesh));
            MeshDevice material_source;
            material_source.materialColor = source.color;
            material_source.material=source.material;
            material_source.materialIndex=source.material_index;
            auto material = create_vision_material(prepared.optics, material_source);
            if (!material) material = scene.obtain_black_body();
            material=scene.material_registry().register_(std::move(material));
            materials.emplace(mesh_index, std::move(material));
        }
        for (const auto& source : prepared.instances) {
            auto instance = std::make_shared<::vision::ShapeInstance>(meshes.at(source.mesh_index));
            instance->set_o2w(source.o2w);
            instance->set_name("corona-node:" + std::to_string(prepared.geometry_handle) + ":" + std::to_string(source.draw_index));
            instance->set_material(materials.at(source.mesh_index));
            MeshDevice material_source;
            const auto& mesh=prepared.meshes.at(source.mesh_index);
            material_source.material=mesh.material;
            material_source.materialIndex=mesh.material_index;
            attach_vision_emission(scene,*instance,prepared.optics,material_source,instance_index++);
            instance->init_aabb();
            group->aabb.extend(instance->aabb);
            group->add_instance(*instance);
        }
    }
    scene.add_shape(group);
}

}  // namespace

VisionBuildResult inspect_vision_geometry() {
    VisionBuildResult result;
    (void)stage_scene_geometry(result);
    return result;
}

VisionBuildResult build_vision_geometry(::vision::Scene& scene) {
    VisionBuildResult result;
    auto groups = stage_scene_geometry(result);
    if (!result.complete()) {
        result.instance_count = 0; // Existing scene is retained; nothing was added.
        return result;
    }

    scene.clear_shapes();
    scene.geometry().data()->clear_meshes();
    for (auto& group : groups) append_prepared_group(scene, group);
    scene.material_registry().remove_unused_elements();
    scene.fill_instances();
    scene.update_geometry_instances();
    return result;
}



}  // namespace Corona::Systems::Vision
