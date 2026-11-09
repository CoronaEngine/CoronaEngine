#include <corona/shared_data_hub.h>
#include <corona/systems/geometry/geometry_system.h>

#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

#include "geometry_internal.h"

namespace Corona::Systems {

// ============================================================================
// 查询接口（线程安全）
// ============================================================================

std::vector<std::uintptr_t> GeometrySystem::query_aabb(
    std::uintptr_t scene, const Spatial::AABB& box) const {
    std::shared_lock lock(impl_->mtx);
    std::vector<std::uintptr_t> out;

    auto it = impl_->scenes.find(scene);
    if (it == impl_->scenes.end() || it->second.tree.empty()) {
        return out;
    }

    out.reserve(it->second.tree.size() / 2);
    it->second.tree.query_aabb(box, out);

    std::vector<std::uintptr_t> filtered;
    filtered.reserve(out.size());
    const auto& state_map = it->second.actor_load_states;
    for (EntityId actor_handle : out) {
        auto state_it = state_map.find(actor_handle);
        if (state_it != state_map.end() && state_it->second == ActorLoadState::Loaded) {
            filtered.push_back(actor_handle);
        }
    }
    return filtered;
}

std::vector<std::uintptr_t> GeometrySystem::query_sphere(
    std::uintptr_t scene, const ktm::fvec3& center, float radius) const {
    std::shared_lock lock(impl_->mtx);
    std::vector<std::uintptr_t> out;

    auto it = impl_->scenes.find(scene);
    if (it == impl_->scenes.end() || it->second.tree.empty()) {
        return out;
    }

    out.reserve(it->second.tree.size() / 2);
    it->second.tree.query_sphere(center, radius, out);

    std::vector<std::uintptr_t> filtered;
    filtered.reserve(out.size());
    const auto& state_map = it->second.actor_load_states;
    for (EntityId actor_handle : out) {
        auto state_it = state_map.find(actor_handle);
        if (state_it != state_map.end() && state_it->second == ActorLoadState::Loaded) {
            filtered.push_back(actor_handle);
        }
    }
    return filtered;
}

std::vector<std::uintptr_t> GeometrySystem::query_frustum(
    std::uintptr_t scene, const Math::Frustum& frustum) const {
    std::shared_lock lock(impl_->mtx);
    std::vector<std::uintptr_t> out;

    auto it = impl_->scenes.find(scene);
    if (it == impl_->scenes.end() || it->second.tree.empty()) {
       return out;
    }

    out.reserve(it->second.tree.size() / 2);
    it->second.tree.query_if(
        [&](const Spatial::AABB& b) { return frustum.intersects(b); }, out);

    std::vector<std::uintptr_t> filtered;
    filtered.reserve(out.size());
    const auto& state_map = it->second.actor_load_states;
    for (EntityId actor_handle : out) {
        auto state_it = state_map.find(actor_handle);
        if (state_it != state_map.end() && state_it->second == ActorLoadState::Loaded) {
            filtered.push_back(actor_handle);
        }
    }
    return filtered;
}

std::vector<std::pair<std::uintptr_t, std::uintptr_t>> GeometrySystem::query_pairs(
    std::uintptr_t scene) const {
    std::shared_lock lock(impl_->mtx);
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> out;

    auto it = impl_->scenes.find(scene);
    if (it == impl_->scenes.end() || it->second.tree.size() < 2) {
        return out;
    }

    std::size_t n = it->second.tree.size();
    out.reserve(n * (n - 1) / 4); // 保守估计，实际碰撞对通常远小于最大值
    it->second.tree.collect_pairs(out);

    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> filtered;
    filtered.reserve(out.size());
    const auto& state_map = it->second.actor_load_states;
    for (const auto& pair : out) {
        auto state_a = state_map.find(pair.first);
        auto state_b = state_map.find(pair.second);
        if (state_a != state_map.end() && state_a->second == ActorLoadState::Loaded
            && state_b != state_map.end() && state_b->second == ActorLoadState::Loaded) {
            filtered.push_back(pair);
        }
    }

    return filtered;
}

std::vector<std::uintptr_t> GeometrySystem::query_visible_for_camera(
    std::uintptr_t scene, std::uintptr_t camera) const {
    auto& cam_storage = SharedDataHub::instance().camera_storage();
    auto cam_handle = cam_storage.try_acquire_read_nowait(camera);
    if (!cam_handle.valid()) {
        return {};
    }
    const auto frustum = Math::Frustum::from_camera(*cam_handle);
    return query_frustum(scene, frustum);
}

//加载状态查询
ActorLoadState GeometrySystem::get_actor_load_state(std::uintptr_t actor,std::uintptr_t scene) const {
    std::shared_lock lock(impl_->mtx);
    auto scene_it = impl_->scenes.find(scene);
    if (scene_it == impl_->scenes.end()) {
        return ActorLoadState::Unloaded;
    }
    auto actor_it = scene_it->second.actor_load_states.find(actor);
    return (actor_it != scene_it->second.actor_load_states.end()) ?
            actor_it->second : ActorLoadState::Unloaded;
}

// ============================================================================
// 统计
// ============================================================================

SceneStats GeometrySystem::stats(std::uintptr_t scene) const {
    std::shared_lock lock(impl_->mtx);
    auto it = impl_->scenes.find(scene);
    if (it == impl_->scenes.end()) {
        return SceneStats{};
    }
    std::lock_guard stats_lock(it->second.stats_mutex);
    SceneStats s = it->second.stats;
    s.octree_entries = it->second.tree.size();
    return s;
}


}  // namespace Corona::Systems

