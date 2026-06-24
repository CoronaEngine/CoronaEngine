#pragma once

#include <corona/spatial/octree.h>
#include <corona/systems/geometry/geometry_system.h>

#include <algorithm>
#include <cstdint>
#include <future>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include <ktm/ktm.h>

namespace Corona::Systems::GeometryInternal {

[[nodiscard]] inline ktm::fvec3 make_fvec3(float x, float y, float z) {
    ktm::fvec3 value;
    value[0] = x;
    value[1] = y;
    value[2] = z;
    return value;
}

[[nodiscard]] inline ktm::fvec4 make_fvec4(float x, float y, float z, float w) {
    ktm::fvec4 value;
    value[0] = x;
    value[1] = y;
    value[2] = z;
    value[3] = w;
    return value;
}

[[nodiscard]] inline ktm::fvec3 transform_local_point_to_world(const Corona::ModelTransform& transform,
                                                        const ktm::fvec3& local_point) {
    const ktm::fmat4x4 matrix = transform.compute_matrix();
    const ktm::fvec4 local_h = make_fvec4(local_point[0], local_point[1], local_point[2], 1.0f);
    const ktm::fvec4 world_h = matrix * local_h;
    return make_fvec3(world_h[0], world_h[1], world_h[2]);
}

inline void world_aabb_from_local_bounds(const Corona::ModelTransform& transform,
                                         const ktm::fvec3& local_min,
                                         const ktm::fvec3& local_max,
                                         Spatial::AABB& out_world_aabb) {
    const ktm::fvec3 corners[8] = {
        make_fvec3(local_min[0], local_min[1], local_min[2]),
        make_fvec3(local_max[0], local_min[1], local_min[2]),
        make_fvec3(local_min[0], local_max[1], local_min[2]),
        make_fvec3(local_max[0], local_max[1], local_min[2]),
        make_fvec3(local_min[0], local_min[1], local_max[2]),
        make_fvec3(local_max[0], local_min[1], local_max[2]),
        make_fvec3(local_min[0], local_max[1], local_max[2]),
        make_fvec3(local_max[0], local_max[1], local_max[2]),
    };

    const ktm::fvec3 first_corner = transform_local_point_to_world(transform, corners[0]);
    out_world_aabb.min = first_corner;
    out_world_aabb.max = first_corner;

    for (int i = 1; i < 8; ++i) {
        const ktm::fvec3 world_corner = transform_local_point_to_world(transform, corners[i]);
        out_world_aabb.min[0] = std::min(out_world_aabb.min[0], world_corner[0]);
        out_world_aabb.min[1] = std::min(out_world_aabb.min[1], world_corner[1]);
        out_world_aabb.min[2] = std::min(out_world_aabb.min[2], world_corner[2]);
        out_world_aabb.max[0] = std::max(out_world_aabb.max[0], world_corner[0]);
        out_world_aabb.max[1] = std::max(out_world_aabb.max[1], world_corner[1]);
        out_world_aabb.max[2] = std::max(out_world_aabb.max[2], world_corner[2]);
    }
}


}  // namespace Corona::Systems::GeometryInternal

namespace Corona::Systems {

struct GeometrySystem::Impl {
    using Payload = std::uintptr_t;
    using OctreeEntry = Spatial::Octree<Payload>::Entry;

    struct SceneState {
        Spatial::Octree<Payload>                            tree;
        std::unordered_map<Payload,Spatial::AABB> actor_to_entry; //Actor到AABB映射
        std::unordered_map<Payload, std::uint32_t>          invisible_frames;
        SceneVisibilityConfig                               cfg;
        SceneStats                                          stats;
        mutable std::mutex                                  stats_mutex;
        std::unordered_map<Payload,ActorLoadState>          actor_load_states;

        std::unordered_map<Payload,std::future<std::uint64_t>> loading_tasks;
        std::unordered_map<Payload,std::future<bool>>       unloading_tasks;
        std::unordered_map<Payload,int>                     unload_retry_counts; //卸载重试次数
    };

    mutable std::shared_mutex                               mtx;
    std::unordered_map<std::uintptr_t /*scene*/, SceneState> scenes;
    std::unordered_map<Payload, bool>                       offline_actors;
    std::vector<Kernel::EventId>                            event_subscriptions;
    Kernel::ISystemContext*                                 ctx = nullptr;

    // ========================================
    // 动态减面（LOD）相关状态
    // ========================================
    struct LODCacheEntry {
        std::vector<LODMeshBuffers> levels;
        std::uint64_t model_id = 0;  // 用于检测模型变更（比地址指针可靠，不受 slot 复用影响）
    };

    MeshSimplificationConfig           simplification_cfg;
    mutable std::shared_mutex          lod_cache_mutex;
    std::unordered_map<uint64_t, LODCacheEntry> lod_cache;

    // 共享占位纹理：所有无纹理 mesh 共用，生命周期与 Impl 一致
    // 使用 unique_ptr 避免 static 局部变量在 GPU device 析构后才析构
    std::unique_ptr<Horizon::HardwareImage> shared_placeholder_texture;

    [[nodiscard]] static uint64_t make_lod_key(std::uintptr_t geometry_handle,
                                               uint32_t       mesh_index) {
        return (static_cast<uint64_t>(geometry_handle) << 32) | mesh_index;
    }

    SceneState& get_or_create(std::uintptr_t scene) {
        auto [it, inserted] = scenes.try_emplace(scene);
        return it->second;
    }
};

}  // namespace Corona::Systems

