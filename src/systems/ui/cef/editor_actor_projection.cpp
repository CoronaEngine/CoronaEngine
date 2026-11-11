#include "editor_actor_projection.h"

#include "animation_config.h"
#include "editor_network_state.h"
#include "editor_paths.h"
#include "editor_text_utils.h"
#include "ik_chain_config.h"
#include "native_material_config.h"
#include "ui_resource_read.h"

#include <corona/resource/resource_manager.h>
#include <corona/resource/types/scene.h>
#include <corona/shared_data_hub.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Corona::Systems::UI {

namespace {

bool aabb_has_usable_extent(const std::array<float, 6>& aabb) {
    bool has_extent = false;
    for (size_t axis = 0; axis < 3; ++axis) {
        const float min_v = aabb[axis];
        const float max_v = aabb[axis + 3];
        if (!std::isfinite(min_v) || !std::isfinite(max_v) || max_v < min_v) {
            return false;
        }
        if (std::abs(max_v - min_v) > 1e-5f) {
            has_extent = true;
        }
    }
    return has_extent;
}

}  // namespace


const char* actor_load_status_name(ActorLoadStatus status) {
    switch (status) {
        case ActorLoadStatus::Loaded: return "loaded";
        case ActorLoadStatus::MissingResource: return "missing_resource";
        case ActorLoadStatus::DecodeFailed: return "decode_failed";
        case ActorLoadStatus::UnsupportedResource: return "unsupported_resource";
    }
    return "decode_failed";
}

std::string body_type_name(const Corona::API::Mechanics& mechanics) {
    return mechanics.get_state().body_type;
}

std::string actor_file_extension(const std::string& route) {
    auto ext = path_from_utf8(route).extension().string();
    if (!ext.empty() && ext.front() == '.') {
        ext.erase(ext.begin());
    }
    return ext;
}

nlohmann::json aabb_to_json(const std::array<float, 6>& aabb) {
    return nlohmann::json::array({
        aabb[0], aabb[1], aabb[2], aabb[3], aabb[4], aabb[5],
    });
}


std::optional<std::array<float, 6>> native_actor_local_aabb(const NativeEditorActor& actor,
                                                            bool best_effort) {
    if (!actor.geometry) {
        return std::nullopt;
    }
    // The editor UI thread must never wait on an engine lock: a loader holds a
    // resource entry lock across its whole parse, which used to freeze the editor
    // permanently.  Asking for the AABB best effort means "no bounds this refresh"
    // instead of "no editor".
    std::array<float, 6> result{};
    if (best_effort) {
        const auto value = actor.geometry->try_get_aabb();
        if (!value) {
            return std::nullopt;
        }
        result = *value;
    } else {
        const auto local = actor.geometry->get_aabb();
        if (local.size() < 6) {
            return std::nullopt;
        }
        for (size_t index = 0; index < 6; ++index) {
            result[index] = local[index];
        }
    }
    if (!aabb_has_usable_extent(result)) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::array<float, 6>> native_actor_world_aabb(const NativeEditorActor& actor,
                                                           bool best_effort) {
    const auto local = native_actor_local_aabb(actor, best_effort);
    if (!local || !actor.geometry) {
        return std::nullopt;
    }
    auto& hub = Corona::SharedDataHub::instance();
    auto geometry = hub.geometry_storage().try_acquire_read(actor.geometry->get_handle());
    if (!geometry) return std::nullopt;
    auto transform = hub.model_transform_storage().try_acquire_read(geometry->transform_handle);
    if (!transform) return std::nullopt;
    const auto matrix = transform->compute_matrix();
    const float infinity = std::numeric_limits<float>::infinity();
    std::array<float, 6> result{infinity,infinity,infinity,-infinity,-infinity,-infinity};
    for (unsigned int corner = 0; corner < 8; ++corner) {
        const std::array<float, 4> point{(*local)[(corner&1) ? 3 : 0],
            (*local)[(corner&2) ? 4 : 1], (*local)[(corner&4) ? 5 : 2], 1};
        for (int axis = 0; axis < 3; ++axis) {
            float value = 0;
            for (int c = 0; c < 4; ++c) value += matrix[c][axis] * point[c];
            result[axis] = std::min(result[axis], value);
            result[axis+3] = std::max(result[axis+3], value);
        }
    }
    if (!aabb_has_usable_extent(result)) {
        return std::nullopt;
    }
    return result;
}

std::shared_ptr<Corona::Resource::AnimationPlayer> actor_animation_player(const NativeEditorActor& actor) {
    if (!actor.geometry) return {};
    auto geometry = Corona::SharedDataHub::instance().geometry_storage().try_acquire_read(actor.geometry->get_handle());
    return geometry ? geometry->animation_player : nullptr;
}

nlohmann::json native_actor_ik_configuration(const NativeEditorActor& actor) {
    if (!actor.geometry) return actor.persisted_ik_chains;
    std::vector<Corona::Resource::IkChain> chains;
    {
        auto geometry = Corona::SharedDataHub::instance().geometry_storage().try_acquire_read(
            actor.geometry->get_handle());
        if (!geometry) return actor.persisted_ik_chains;
        chains = geometry->ik_chains;
    }
    if (chains.empty()) return nlohmann::json::array();
    const auto model_id = actor.geometry->get_model_id();
    if (model_id != 0) {
        auto resource = read_resource_for_ui<Corona::Resource::Scene>(model_id);
        if (resource.valid() && resource->data.skeleton) {
            return IkChainConfig::serialize(chains, &*resource->data.skeleton);
        }
    }
    return IkChainConfig::serialize(chains);
}



nlohmann::json actor_to_json(const NativeEditorScene& scene, const NativeEditorActor& actor,
                            bool best_effort = CefCurrentlyOn(TID_UI)) {
    nlohmann::json item;
    item["name"] = actor.name;
    item["archive_session"] = scene.archive_session;
    item["actor_guid"] = actor.actor_guid;
    item["handle"] = actor.engine_actor ? actor.engine_actor->get_handle() : 0;
    item["path"] = actor.route;
    item["route"] = actor.route;
    item["scene"] = scene.route;
    item["type"] = actor_file_extension(actor.route);
    item["model"] = actor.route;
    item["model_dependencies"] = nlohmann::json::array();
    item["actor_type"] = actor.actor_type;
    if (actor.actor_type == "actor") item["resolved_model"] = actor.resolved_asset_path;
    item["ik_chains"] = native_actor_ik_configuration(actor);
    if (auto player = actor_animation_player(actor)) item.update(AnimationConfig::serialize(player->snapshot()));
    else {
        Corona::Resource::AnimationSnapshot animation;
        animation.sources = actor.persisted_animation_sources;
        animation.requested_sources = animation.sources;
        animation.load_status = "error";
        animation.load_error = "ACTOR_HAS_NO_GEOMETRY";
        item.update(AnimationConfig::serialize(animation));
    }
    item["model_node_overrides"] = actor.persisted_model_node_overrides;
    item["model_node_resource"] = actor.model_node_resource;
    item["model_node_unmatched"] = actor.model_node_unmatched;
    item["load_status"] = actor_load_status_name(actor.load_status);
    if (actor.load_status != ActorLoadStatus::Loaded) {
        item["load_error"] = {
            {"code", actor.load_error_code},
            {"message", actor.load_error_message},
            {"resource_path", actor.route},
            {"path", actor.resolved_asset_path},
        };
    } else {
        item["load_error"] = nullptr;
    }
    item["entity_id"] = actor.runtime_entity_id;
    item["asset_id"] = actor.asset_id;
    item["model_ref"] = actor.model_ref;
    item["entity_type"] = actor.entity_type;
    item["semantic_role"] = actor.semantic_role;
    item["source_plan_id"] = actor.source_plan_id;
    item["source_batch_id"] = actor.source_batch_id;
    item["source_scene_version"] = std::max(actor.source_scene_version, 1);
    item["actor_version"] = std::max(actor.actor_version, 1);
    item["version"] = std::max(actor.actor_version, 1);
    if (actor.actor_type == "audio") {
        item["audio_resource_id"] = std::to_string(actor.audio_resource_id);
    }
    item["body_type"] = actor.load_status == ActorLoadStatus::Loaded && actor.mechanics
                           ? body_type_name(*actor.mechanics)
                           : actor.persisted_body_type;
    item["visible"] = actor.load_status == ActorLoadStatus::Loaded && actor.optics
                          ? actor.optics->get_state().visible
                          : actor.persisted_visible;
    item["script"] = "";
    item["follow_camera"] = actor.follow_camera;
    item["render_space"] = actor.follow_camera ? "ui" : "scene";
    item["geometry"] = {
        {"position", actor.geometry ? actor.geometry->get_position() : actor.position},
        {"rotation", actor.geometry ? actor.geometry->get_rotation() : actor.rotation},
        {"scale", actor.geometry ? actor.geometry->get_scale() : actor.scale},
    };
    const auto local_aabb = native_actor_local_aabb(actor, best_effort);
    const auto world_aabb = native_actor_world_aabb(actor, best_effort);
    item["local_aabb"] = local_aabb ? aabb_to_json(*local_aabb) : nlohmann::json(nullptr);
    item["world_aabb"] = world_aabb ? aabb_to_json(*world_aabb) : nlohmann::json(nullptr);
    item["aabb"] = item["world_aabb"];
    item["bounds_ready"] = static_cast<bool>(world_aabb);
    const auto render_status = actor.geometry
        ? actor.geometry->get_render_status()
        : Corona::API::GeometryRenderStatus{};
    item["render_status_observed"] = render_status.observed;
    item["render_ready"] = render_status.ready;
    item["collision_ready"] = render_status.collision_ready;
    item["render_failed"] = render_status.failed;
    item["gpu_build_state"] = render_status.gpu_build_state;
    item["mesh_count"] = render_status.mesh_count;
    item["renderable_mesh_count"] = render_status.renderable_mesh_count;
    item["invalid_mesh_count"] = render_status.invalid_mesh_count;
    item["size"] = world_aabb
        ? nlohmann::json::array({
            (*world_aabb)[3] - (*world_aabb)[0],
            (*world_aabb)[4] - (*world_aabb)[1],
            (*world_aabb)[5] - (*world_aabb)[2],
        })
        : nlohmann::json::array({0.0f, 0.0f, 0.0f});
    if (actor.mechanics) {
        const auto mechanics_state = actor.mechanics->get_state();
        item["mechanics"] = {
            {"mass", mechanics_state.mass},
            {"restitution", mechanics_state.restitution},
            {"damping", mechanics_state.damping},
            {"body_type", actor.load_status == ActorLoadStatus::Loaded
                             ? mechanics_state.body_type
                             : actor.persisted_body_type},
            {"linear_lock", {mechanics_state.linear_lock[0], mechanics_state.linear_lock[1],
                             mechanics_state.linear_lock[2]}},
            {"angular_lock", {mechanics_state.angular_lock[0], mechanics_state.angular_lock[1],
                              mechanics_state.angular_lock[2]}},
        };
    }
    if (actor.optics) {
        const auto optics_state = actor.optics->get_state();
        item["optics"] = {
            {"diffuse", optics_state.diffuse},
            {"metallic", optics_state.metallic},
            {"roughness", optics_state.roughness},
            {"specular", optics_state.specular},
            {"shininess", optics_state.shininess},
        };
        if (actor.persisted_optics.emission) {
            item["optics"]["emission"] = *actor.persisted_optics.emission;
        }
    }
    item["material"] = {{"source", "model"}, {"overrides", NativeMaterialConfig::resolve(actor.persisted_optics)}};
    if (actor.persisted_optics.material.contains("principled"))
        item["material"]["principled"] = actor.persisted_optics.material["principled"];
    if (!actor.persisted_optics.texture.empty()) item["material"]["texture"] = actor.persisted_optics.texture;
    item["camera_lock"] = {
        {"enabled", actor.camera_lock_enabled},
        {"lock_to_camera", actor.camera_lock_enabled},
        {"position_offset", actor.camera_lock_position_offset},
        {"rotation_offset", actor.camera_lock_rotation_offset},
    };
    return item;
}

}
  // namespace Corona::Systems::UI
