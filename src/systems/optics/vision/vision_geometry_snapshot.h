#pragma once

#include <corona/systems/optics/vision_scene_resource.h>
#include "vision_external_live_aabb.h"
#include "base/mgr/scene.h"
#include <unordered_set>

namespace Corona::Systems::Vision {

// Only values: in particular, Mesh inherits GPU buffers and must never be stored here.
struct VisionGeometrySnapshot {
    struct Material {
        uint64_t hash{};
        ::vision::MaterialDesc desc;
    };
    struct Mesh {
        ::vision::vector<::vision::Vertex> vertices;
        ::vision::vector<::vision::Triangle> triangles;
    };
    struct Instance {
        uint64_t identity{};
        size_t mesh{}; // Index within this immutable publication, never a persistent identity.
        std::string name;
        uint32_t material{::vision::InvalidUI32};
        std::optional<::vision::LightDesc> emission;
        uint32_t inside{::vision::InvalidUI32}, outside{::vision::InvalidUI32};
        std::array<float, 16> matrix{};
    };
    struct Group {
        uint64_t identity{};
        std::string shape_guid, shape_identity_key, name;
        std::vector<Instance> instances;
    };
    uint64_t source_revision{};
    std::vector<Mesh> meshes;
    std::vector<Material> materials;
    std::vector<Group> groups;
};

inline auto capture_geometry_snapshot(VisionSceneResource& resource, ::vision::Scene& scene) {
    auto snapshot = std::make_shared<VisionGeometrySnapshot>();
    snapshot->source_revision = resource.source_revision;
    std::unordered_set<uint64_t> identities;
    std::unordered_map<const ::vision::Mesh*, size_t> mesh_indices; // Only deduplicates this capture.
    std::unordered_map<const ::vision::Material*, uint32_t> material_indices;
    auto assign = [&](uint64_t& identity) {
        if (!identity || !identities.insert(identity).second) {
            identity = resource.next_geometry_identity++;
            identities.insert(identity);
        }
    };
    for (size_t index = 0; index < scene.groups().size(); ++index) {
        auto& group = scene.groups()[index];
        if (!group) continue;
        assign(group->geometry_sync_identity);
        VisionGeometrySnapshot::Group saved;
        saved.identity = group->geometry_sync_identity;
        saved.name = group->name();
        for (const auto& [actor, record] : resource.external_live_shapes_by_actor) {
            if (record.shape_index == static_cast<int>(index)) {
                saved.shape_guid = record.shape_guid;
                saved.shape_identity_key = record.shape_identity_key;
                break;
            }
        }
        group->for_each([&](::vision::SP<::vision::ShapeInstance> instance, ::vision::uint) {
            if (!instance || !instance->mesh()) throw std::runtime_error("invalid geometry instance");
            assign(instance->geometry_sync_identity);
            const auto& mesh = instance->mesh();
            auto [it, inserted] = mesh_indices.try_emplace(mesh.get(), snapshot->meshes.size());
            if (inserted) snapshot->meshes.push_back({mesh->vertices(), mesh->triangles()});
            const auto& h = instance->handle();
            uint32_t material_index = ::vision::InvalidUI32;
            if (instance->has_material()) {
                auto material = instance->material();
                auto [mi, added] = material_indices.try_emplace(material.get(), static_cast<uint32_t>(snapshot->materials.size()));
                if (added) snapshot->materials.push_back({material->hash(), material->source_desc()});
                material_index = mi->second;
            }
            std::optional<::vision::LightDesc> emission;
            if (instance->has_emission()) emission = instance->emission()->source_desc();
            saved.instances.push_back({instance->geometry_sync_identity, it->second, instance->name(),
                material_index, std::move(emission), h.inside_medium, h.outside_medium, aabb_matrix_values(instance->o2w())});
        });
        snapshot->groups.push_back(std::move(saved));
    }
    return snapshot;
}

// Stage and validate all CPU objects before touching the live runtime. Registry
// Materials and lights are recreated from CPU descriptors in the local runtime;
// medium IDs refer to the unchanged logical medium registry of this source.
struct StagedGeometry {
    ::vision::vector<::vision::SP<::vision::ShapeGroup>> groups;
    ::vision::vector<::vision::SP<::vision::Material>> new_materials;
    ::vision::vector<::vision::TLight> new_lights;
};

inline auto stage_geometry_snapshot(const VisionGeometrySnapshot& snapshot, ::vision::Scene& scene) {
    using namespace ::vision;
    vector<SP<Mesh>> meshes;
    for (const auto& saved : snapshot.meshes) {
        for (const auto& triangle : saved.triangles) {
            if (triangle.i >= saved.vertices.size() || triangle.j >= saved.vertices.size() ||
                triangle.k >= saved.vertices.size()) throw std::runtime_error("geometry index out of range");
        }
        meshes.push_back(std::make_shared<Mesh>(saved.vertices, saved.triangles));
    }
    // Validate mapping references before creating plugins or allocating GPU data.
    for (const auto& group : snapshot.groups) for (const auto& item : group.instances) {
        if (item.mesh >= meshes.size() ||
            (item.material != InvalidUI32 && item.material >= snapshot.materials.size()))
            throw std::runtime_error("geometry mapping out of range");
    }
    StagedGeometry staged;
    vector<SP<Material>> materials;
    for (const auto& item : snapshot.materials) {
        auto existing = scene.materials().find_if([&](const auto& value) { return value->hash() == item.hash; });
        if (existing != scene.materials().end()) materials.push_back(*existing);
        else {
            if (item.desc.sub_type.empty()) throw std::runtime_error("geometry material descriptor unavailable");
            auto material = Material::create_root(item.desc);
            materials.push_back(material);
            staged.new_materials.push_back(std::move(material));
        }
    }
    std::unordered_map<uint64_t, SP<IAreaLight>> existing_emissions;
    for (const auto& instance : scene.instances()) {
        if (instance->has_emission()) existing_emissions.emplace(instance->geometry_sync_identity, instance->emission());
    }
    uint flat_index = 0;
    for (const auto& saved : snapshot.groups) {
        auto group = std::make_shared<ShapeGroup>();
        group->geometry_sync_identity = saved.identity;
        group->set_name(saved.name);
        unsigned instance_index = 0;
        for (const auto& item : saved.instances) {
            if (item.mesh >= meshes.size()) throw std::runtime_error("geometry mesh mapping out of range");
            group->add_instance(ShapeInstance{meshes[item.mesh]});
            auto& instance = group->instance(instance_index++);
            instance.geometry_sync_identity = item.identity;
            instance.set_name(item.name);
            if (item.material != InvalidUI32) {
                instance.set_material(materials[item.material]);
            }
            if (item.emission) {
                auto existing = existing_emissions.find(item.identity);
                if (existing != existing_emissions.end()) instance.set_emission(existing->second);
                else {
                    auto desc = *item.emission;
                    desc.set_value("inst_id", flat_index);
                    auto light = Light::create_root(desc);
                    instance.set_emission(std::dynamic_pointer_cast<IAreaLight>(light.impl()));
                    staged.new_lights.push_back(std::move(light));
                }
            }
            auto bind_medium = [&](uint32_t id, bool inside) {
                if (id == InvalidUI32) return;
                auto medium = scene.mediums().find_if([&](const auto& value) {
                    return scene.mediums().encode_id(value->index(), value.get()) == id;
                });
                if (medium == scene.mediums().end()) throw std::runtime_error("geometry medium mapping unavailable");
                if (inside) instance.set_inside(*medium); else instance.set_outside(*medium);
            };
            bind_medium(item.inside, true); bind_medium(item.outside, false);
            instance.set_o2w(aabb_matrix(item.matrix));
            instance.init_aabb();
            group->aabb.extend(instance.aabb);
            ++flat_index;

        }
        staged.groups.push_back(std::move(group));
    }
    return staged;
}
}
