#pragma once

#include <corona/systems/optics/vision_scene_resource.h>
#include "base/shape.h"
#include <cstring>
#include <span>

namespace Corona::Systems::Vision {

inline std::array<float, 16> aabb_matrix_values(const ::vision::float4x4& matrix) {
    std::array<float, 16> values{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) values[col * 4 + row] = matrix[col][row];
    }
    return values;
}

inline ::vision::float4x4 aabb_matrix(const std::array<float, 16>& values) {
    auto matrix = ::vision::make_float4x4(1.f);
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) matrix[col][row] = values[col * 4 + row];
    }
    return matrix;
}

inline bool aabb_equal(const ::vision::Box3f& a, const ::vision::Box3f& b) {
    for (int i = 0; i < 3; ++i) {
        if (a.lower[i] != b.lower[i] || a.upper[i] != b.upper[i]) return false;
    }
    return true;
}

// Mesh exposes writable vectors (including retained references), so neither a
// pointer/count check nor Hashable's cached hash is a geometry change signal.
// Compare the actual bytes, without transforming vertices or widening bounds.
struct ExternalLiveAabbState {
    struct MeshData {
        std::shared_ptr<::vision::Mesh> mesh;
        std::vector<std::byte> vertices;
        std::vector<std::byte> triangles;
    };
    struct Instance {
        std::shared_ptr<::vision::ShapeInstance> instance;
        std::shared_ptr<::vision::Mesh> mesh;
        std::array<float, 16> matrix;
        ::vision::Box3f bounds;
    };
    std::shared_ptr<::vision::ShapeGroup> group;
    std::vector<Instance> instances;
    std::unordered_map<const ::vision::Mesh*, MeshData> meshes;
    ::vision::Box3f bounds;

    template<typename Vector>
    static auto bytes(const Vector& values) {
        return std::as_bytes(std::span(values.data(), values.size()));
    }
    static bool equal_bytes(const std::vector<std::byte>& saved, std::span<const std::byte> current) {
        return saved.size() == current.size() &&
               (saved.empty() || std::memcmp(saved.data(), current.data(), saved.size()) == 0);
    }
    bool same_instance(std::size_t index, const std::shared_ptr<::vision::ShapeInstance>& instance) const {
        return index < instances.size() && instances[index].instance == instance;
    }
    bool geometry_matches(const std::shared_ptr<::vision::ShapeGroup>& current) const {
        if (group != current) return false;
        bool matches = true;
        std::size_t count = 0;
        current->for_each([&](::vision::SP<::vision::ShapeInstance> instance, ::vision::uint index) {
            ++count;
            matches &= same_instance(index, instance) &&
                       (!instance || instances[index].mesh == instance->mesh());
        });
        if (!matches || count != instances.size()) return false;
        for (const auto& [key, data] : meshes) {
            if (!equal_bytes(data.vertices, bytes(data.mesh->vertices())) ||
                !equal_bytes(data.triangles, bytes(data.mesh->triangles()))) return false;
        }
        return true;
    }
    bool state_matches(const VisionSceneResource& resource, std::uintptr_t actor, int shape,
                       std::size_t signature) const {
        if (!aabb_equal(bounds, group->aabb)) return false;
        for (std::size_t i = 0; i < instances.size(); ++i) {
            const auto& saved = instances[i];
            if (!saved.instance) continue;
            const auto* logical = resource.find_logical_instance({shape, static_cast<int>(i)});
            if (!logical || logical->actor_handle != actor || logical->transform_signature != signature ||
                logical->object_to_world != saved.matrix ||
                aabb_matrix_values(saved.instance->o2w()) != saved.matrix ||
                !aabb_equal(saved.instance->aabb, saved.bounds)) return false;
        }
        return true;
    }
    void capture(const std::shared_ptr<::vision::ShapeGroup>& current, bool geometry_changed) {
        group = current;
        bounds = group->aabb;
        instances.clear();
        if (geometry_changed) meshes.clear();
        group->for_each([&](::vision::SP<::vision::ShapeInstance> instance, ::vision::uint) {
            if (!instance) { instances.push_back({}); return; }
            instances.push_back({instance, instance->mesh(), aabb_matrix_values(instance->o2w()), instance->aabb});
            const auto mesh = instance->mesh();
            if (!meshes.contains(mesh.get())) {
                const auto vertices = bytes(mesh->vertices());
                const auto triangles = bytes(mesh->triangles());
                meshes.emplace(mesh.get(), MeshData{mesh, {vertices.begin(), vertices.end()},
                                                   {triangles.begin(), triangles.end()}});
            }
        });
    }
};

struct ExternalLiveAabbCache {
    std::uint64_t generation{0};
    std::unordered_map<std::uintptr_t, std::shared_ptr<ExternalLiveAabbState>> actors;
    std::unordered_map<uint64_t, std::shared_ptr<ExternalLiveAabbState>> loaded_groups;
};

struct ExternalLiveGroupSyncResult {
    bool changed{false};
    bool geometry_changed{false};
    std::size_t aabb_updates{0};
};

inline ExternalLiveGroupSyncResult sync_external_live_group(
    VisionSceneResource& resource, ExternalLiveAabbCache& cache, std::uintptr_t actor, int shape,
    const std::shared_ptr<::vision::ShapeGroup>& group, std::size_t normal_signature,
    std::size_t target_signature, const ::vision::float4x4& target_o2w,
    bool hidden, bool original_shape) {
    if (cache.generation != resource.external_live_cache_generation) {
        cache.actors.clear();
        cache.generation = resource.external_live_cache_generation;
    }
    auto& state = cache.actors[actor];
    if (!state) {
        if (auto loaded = cache.loaded_groups.find(group->geometry_sync_identity);
            loaded != cache.loaded_groups.end()) {
            state = std::move(loaded->second);
            cache.loaded_groups.erase(loaded);
        }
    }
    const auto signature = resource.external_live_transform_signatures.find(actor);
    const bool first_sync = signature == resource.external_live_transform_signatures.end();
    const bool signature_changed = first_sync || signature->second != target_signature;
    const bool geometry_matches = state && state->geometry_matches(group);
    if (!signature_changed && geometry_matches && state->state_matches(resource, actor, shape, target_signature)) {
        return {};
    }

    ExternalLiveGroupSyncResult result;
    result.geometry_changed = state && !geometry_matches;
    // Repair original/logical instance membership before applying transforms.
    std::size_t count = 0;
    group->for_each([&](::vision::SP<::vision::ShapeInstance> instance, ::vision::uint index) {
        ++count;
        const VisionLogicalInstanceKey key{shape, static_cast<int>(index)};
        if (state && !state->same_instance(index, instance)) {
            resource.external_live_original_instances.erase(key);
            resource.logical_instances.erase(key);
        }
        if (instance && original_shape) {
            resource.cache_external_live_original_instance({key, actor, normal_signature,
                                                            aabb_matrix_values(instance->o2w())});
        }
    });
    auto obsolete = [&](const auto& pair) {
        return pair.first.shape_index == shape && pair.first.instance_index >= static_cast<int>(count);
    };
    std::erase_if(resource.logical_instances, obsolete);
    std::erase_if(resource.external_live_original_instances, obsolete);
    if (original_shape) resource.external_live_original_transform_signatures.try_emplace(actor, normal_signature);
    const bool restore = original_shape && !hidden &&
        resource.external_live_original_transform_signatures.at(actor) == normal_signature;

    group->aabb = {};
    bool logical_changed = false;
    group->for_each([&](::vision::SP<::vision::ShapeInstance> instance, ::vision::uint index) {
        if (!instance) return;
        const VisionLogicalInstanceKey key{shape, static_cast<int>(index)};
        auto values = aabb_matrix_values(target_o2w);
        if (restore) values = resource.external_live_original_instances.at(key).object_to_world;
        logical_changed |= resource.upsert_logical_instance({key, actor, target_signature, values});
        instance->set_o2w(aabb_matrix(values));
        instance->init_aabb();
        ++result.aabb_updates;
        group->aabb.extend(instance->aabb);
    });
    resource.external_live_transform_signatures[actor] = target_signature;
    // The loader/add-shape path has already uploaded a first visible instance.
    result.changed = result.geometry_changed ||
        ((hidden || !first_sync) && (signature_changed || logical_changed));
    if (!state) state = std::make_shared<ExternalLiveAabbState>();
    state->capture(group, !geometry_matches);
    return result;
}

}  // namespace Corona::Systems::Vision
