#include "mechanics_internal.h"
#include "ik_runtime.h"

namespace Corona::Systems {
using namespace MechanicsInternal;

namespace {
ktm::fvec3 transform_point(const ktm::fmat4x4& m, const ktm::fvec3& p) {
    return make_fvec3(m[0][0]*p.x + m[1][0]*p.y + m[2][0]*p.z + m[3][0],
                      m[0][1]*p.x + m[1][1]*p.y + m[2][1]*p.z + m[3][1],
                      m[0][2]*p.x + m[1][2]*p.y + m[2][2]*p.z + m[3][2]);
}
bool invertible_transform(const ModelTransform& t) {
    return std::isfinite(t.scale.x) && std::isfinite(t.scale.y) && std::isfinite(t.scale.z) &&
           std::abs(t.scale.x) > 1e-8f && std::abs(t.scale.y) > 1e-8f && std::abs(t.scale.z) > 1e-8f;
}
std::array<float, 3> as_array(const ktm::fvec3& p) { return {p.x, p.y, p.z}; }
ktm::fvec3 as_vector(const std::array<float, 3>& p) { return make_fvec3(p[0], p[1], p[2]); }
} // namespace

void MechanicsSystem::refresh_foot_ground_cache() {
    auto& hub = SharedDataHub::instance();
    impl_->ik_geometry_scenes.clear();
    impl_->ik_geometry_actors.clear();
    impl_->ik_scene_floors.clear();
    std::unordered_set<std::uintptr_t> alive;
    struct SceneSnapshot { std::uintptr_t handle; std::uintptr_t environment; std::vector<std::uintptr_t> actors; };
    std::vector<SceneSnapshot> scenes;
    const auto& scene_storage = hub.scene_storage();
    for (const auto& scene : scene_storage) {
        if (scene.enabled) scenes.push_back({reinterpret_cast<std::uintptr_t>(&scene), scene.environment, scene.actor_handles});
    }
    for (const auto& scene : scenes) {
        float floor = 0.0f;
        if (auto environment = hub.environment_storage().try_acquire_read(scene.environment)) floor = environment->floor_y;
        if (std::isfinite(floor)) impl_->ik_scene_floors[scene.handle] = floor;
        for (auto actor_handle : scene.actors) {
            if (impl_->shutdown_requested.load(std::memory_order_acquire)) return;
            const auto geometries = hub.resolve_actor_geometry_handles(actor_handle);
            for (auto gh : geometries) {
                impl_->ik_geometry_scenes[gh] = scene.handle;
                impl_->ik_geometry_actors[gh] = actor_handle;
            }
            std::vector<std::uintptr_t> profiles;
            { auto actor = hub.actor_storage().try_acquire_read(actor_handle); if (actor) profiles = actor->profile_handles; }
            for (auto ph : profiles) {
                std::uintptr_t mechanics_handle = 0;
                { auto profile = hub.profile_storage().try_acquire_read(ph); if (profile) mechanics_handle = profile->mechanics_handle; }
                if (!mechanics_handle) continue;
                std::uintptr_t gh = 0;
                { auto body = hub.mechanics_storage().try_acquire_read(mechanics_handle);
                  if (!body || body->body_type == BodyType::Phantom) continue;
                  gh = body->geometry_handle; }
                if (!gh || alive.count(gh)) continue;
                std::uintptr_t resource_handle = 0, transform_handle = 0;
                { auto geometry = hub.geometry_storage().try_acquire_read(gh);
                  if (!geometry || geometry->gpu_build_state != GeometryDevice::GpuBuildState::Ready || geometry->mesh_handles.empty()) continue;
                  resource_handle = geometry->model_resource_handle;
                  transform_handle = geometry->transform_handle; }
                std::uint64_t model_id = 0;
                { auto resource = hub.model_resource_storage().try_acquire_read(resource_handle); if (resource) model_id = resource->model_id; }
                if (!model_id) continue;
                ModelTransform transform;
                { auto tx = hub.model_transform_storage().try_acquire_read(transform_handle); if (!tx || !invertible_transform(*tx)) continue; transform = *tx; }
                auto& ground = impl_->foot_ground_cache[gh];
                if (ground.model_id != model_id || ground.mesh.triangles.empty()) {
                    ground = {};
                    auto resource = Resource::ResourceManager::get_instance().acquire_read<Resource::Scene>(model_id);
                    if (!resource.valid()) continue;
                    ground.uses_skin = resource->data.skeleton.has_value();
                    ensure_collision_mesh(model_id, *resource, impl_->collision_mesh_cache,
                                          &impl_->static_triangle_index_cache, &impl_->static_triangle_bone_cache);
                    auto mesh = impl_->collision_mesh_cache.find(model_id);
                    if (mesh == impl_->collision_mesh_cache.end()) continue;
                    ground.mesh = mesh->second;
                    ground.model_id = model_id;
                }
                // Animated supports use the most recent shared CPU collision pose.
                if (ground.uses_skin) {
                    auto skin = impl_->skinned_collision_cache.find(gh);
                    // Bind vertices of a skin are not normalized output-model vertices.
                    // Wait for a pose of this model rather than probing stale/bind data.
                    if (skin == impl_->skinned_collision_cache.end() ||
                        !impl_->skinned_collision_model_ids.contains(gh) || impl_->skinned_collision_model_ids.at(gh) != model_id) continue;
                    if (ground.mesh.vertices.size() != skin->second.vertices.size() ||
                        ground.mesh.triangles != skin->second.triangles) ground.octree = {};
                    ground.mesh = skin->second;
                }
                if (ground.mesh.triangles.empty()) continue;
                ground.scene = scene.handle;
                ground.actor = actor_handle;
                ground.model_to_world = transform.compute_matrix();
                ground.world_to_model = ktm::inverse(ground.model_to_world);
                transform_vertices_to_world(ground.mesh.vertices, transform, ground.world_vertices);
                if (ground.octree.empty()) ground.octree.build(ground.world_vertices, ground.mesh);
                else ground.octree.refit(ground.world_vertices, ground.mesh);
                alive.insert(gh);
            }
        }
    }
    std::erase_if(impl_->foot_ground_cache, [&](const auto& item) { return !alive.count(item.first); });
}

void MechanicsSystem::update_foot_targets(std::uintptr_t geom_handle,
    const Resource::SkeletonData& skeleton,
    const std::vector<std::array<float, 16>>& locals,
    std::vector<Resource::IkChain>& chains, float dt) {
    auto& hub = SharedDataHub::instance();
    std::uintptr_t transform_handle = 0;
    { auto geometry = hub.geometry_storage().try_acquire_read(geom_handle); if (geometry) transform_handle = geometry->transform_handle; }
    ktm::fmat4x4 model_to_world{}, world_to_model{};
    bool have_transform = false;
    { auto transform = hub.model_transform_storage().try_acquire_read(transform_handle);
      if (transform && invertible_transform(*transform)) {
          model_to_world = transform->compute_matrix();
          world_to_model = ktm::inverse(model_to_world);
          have_transform = true;
      } }
    const auto scene_it = impl_->ik_geometry_scenes.find(geom_handle);
    const auto actor_it = impl_->ik_geometry_actors.find(geom_handle);
    for (auto& chain : chains) {
        if (chain.mode != Resource::IkChain::Mode::FootPlant) continue;
        std::optional<std::array<float, 3>> target;
        const auto bone = ik_bone_model_position(skeleton, locals, chain.end_node);
        if (!chain.enabled || chain.weight <= 0 || !bone || !have_transform || scene_it == impl_->ik_geometry_scenes.end()) {
            advance_ik_activation(chain, target, dt);
            continue;
        }
        auto probe = transform_point(model_to_world, as_vector(*bone));
        const float height = std::max(0.0f, chain.foot_height);
        probe.y -= height; // Probe the sole; ankle clearance must not consume the drop range.
        const float max_drop = std::clamp(chain.probe_max_drop, 0.0f, 100.0f);
        float best_y = probe.y - max_drop;
        ktm::fvec3 normal = make_fvec3(0, 1, 0);
        std::uintptr_t support = 0;
        bool found = false;
        auto floor = impl_->ik_scene_floors.find(scene_it->second);
        if (floor != impl_->ik_scene_floors.end() && floor->second >= best_y && floor->second <= probe.y + 0.05f) {
            best_y = floor->second;
            found = true;
        }
        for (const auto& [gh, ground] : impl_->foot_ground_cache) {
            if (gh == geom_handle || ground.scene != scene_it->second ||
                (actor_it != impl_->ik_geometry_actors.end() && ground.actor == actor_it->second)) continue;
            float y = 0;
            ktm::fvec3 hit_normal{};
            if (ground.octree.query_ground_height(ground.world_vertices, ground.mesh, probe, max_drop, y, &hit_normal) &&
                (!found || y > best_y)) {
                best_y = y;
                normal = hit_normal;
                support = gh;
                found = true;
            }
        }
        const float enter = std::max(0.0f, chain.plant_threshold);
        const float release = std::max(0.12f, enter * 1.5f);
        const float gap = probe.y - best_y;
        // A support anchor is acquired only during the low/stance part of the animation.
        // While planted it follows the support transform, rather than animation XZ.
        bool keep_anchor = found && chain.runtime.grounded && chain.runtime.ground_handle == support;
        auto anchor = make_fvec3(probe.x, best_y, probe.z);
        if (keep_anchor) {
            anchor = as_vector(chain.runtime.anchor_local);
            if (support) anchor = transform_point(impl_->foot_ground_cache.at(support).model_to_world, anchor);
            const float dx = probe.x - anchor.x, dz = probe.z - anchor.z;
            const float max_drift = std::max(0.05f, max_drop * 0.5f);
            keep_anchor = gap <= release && dx*dx + dz*dz <= max_drift*max_drift && std::abs(anchor.y-best_y) <= release;
        }
        if (foot_can_plant(chain, found, keep_anchor, gap)) {
            if (!keep_anchor) {
                anchor = make_fvec3(probe.x, best_y, probe.z);
                chain.runtime.anchor_local = as_array(support
                    ? transform_point(impl_->foot_ground_cache.at(support).world_to_model, anchor) : anchor);
            }
            chain.runtime.ground_handle = support;
            chain.runtime.normal = as_array(normal);
            anchor.y += height;
            target = as_array(transform_point(world_to_model, anchor));
        }
        advance_ik_activation(chain, target, dt);
    }
}

} // namespace Corona::Systems
