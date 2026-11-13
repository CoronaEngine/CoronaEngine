#include "editor_actor_properties.h"

#include "animation_config.h"
#include "editor_actor_projection.h"
#include "scene_folder.h"
#include "editor_api_services.h"
#include "editor_camera_views.h"
#include "editor_paths.h"
#include "editor_scene_access.h"
#include "editor_text_utils.h"

#include <corona/systems/script/camera_follow_controller.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Corona::Systems::UI {

SceneReply apply_native_actor_properties(const SceneRequest& request) {
    SceneReply reply;
    const auto fail = [&](const char* code, const std::string& message) {
        reply.error_code = code; reply.error_message = message; return reply;
    };
    const auto& patch = request.properties;
    if (const auto invalid = ActorPropertiesService::validate_patch(patch))
        return fail(invalid.code.c_str(), invalid.message);
    auto* scene = request.archive_session ? native_editor_state().scene.get() : ensure_native_editor_scene();
    if (request.archive_session) {
        if (!scene || request.archive_session->empty() || *request.archive_session != scene->archive_session ||
            !native_scene_request_matches(*scene, patch.scene))
            return fail("PROJECT_CONTEXT_CHANGED", "Properties do not identify the active scene");
    } else scene = resolve_native_editor_scene_request(scene, normalize_route(patch.scene));
    auto* actor = find_native_actor(*scene, patch.actor);
    if (!actor) return fail("ACTOR_NOT_FOUND", "Actor not found: " + patch.actor);
    std::optional<std::string> new_name;
    if (patch.name) {
        std::vector<std::string> other_names;
        other_names.reserve(scene->actors.size());
        for (const auto& other : scene->actors) {
            if (&other != actor) other_names.push_back(other.name);
        }
        if (const auto invalid = ActorPropertiesService::validate_rename(*patch.name, scene->read_only, other_names))
            return fail(invalid.code.c_str(), invalid.message);
        new_name = ActorPropertiesService::normalized_name(*patch.name);
    }
    // Validate every component and derived value before importing assets or
    // mutating any property. A mixed invalid patch must never partly apply.
    if (patch.visible && !actor->optics) return fail("COMPONENT_NOT_FOUND", "Actor has no optics component");
    if (patch.mechanics && !actor->mechanics) return fail("COMPONENT_NOT_FOUND", "Actor has no mechanics component");
    if (patch.animation_sources && !actor_animation_player(*actor)) return fail("COMPONENT_NOT_FOUND", "Actor has no geometry");
    auto* camera = patch.camera_lock && patch.camera_lock->enabled ? find_native_camera(*scene, {}) : nullptr;
    if (patch.camera_lock && patch.camera_lock->enabled && (!camera || !camera->engine_camera || !actor->engine_actor))
        return fail("COMPONENT_NOT_FOUND", "Camera-lock requires an active camera and a loaded actor");
    auto position_offset = actor->camera_lock_position_offset;
    auto rotation_offset = actor->camera_lock_rotation_offset;
    if (patch.camera_lock) {
        position_offset = patch.camera_lock->position_offset.value_or(position_offset);
        rotation_offset = patch.camera_lock->rotation_offset.value_or(rotation_offset);
        if (patch.camera_lock->enabled && !patch.camera_lock->position_offset) {
            const auto camera_position = camera->engine_camera->get_pose().position;
            const auto position = actor->geometry ? actor->geometry->get_position() : actor->position;
            for (size_t index = 0; index < 3; ++index) position_offset[index] = camera_position[index] - position[index];
            if (!std::all_of(position_offset.begin(), position_offset.end(), [](float value) { return std::isfinite(value); }))
                return fail("INVALID_ARGUMENT", "Derived camera offset exceeds the finite range");
        }
    }
    std::optional<std::vector<std::string>> sources;
    if (patch.animation_sources) {
        try {
            sources = prepare_animation_sources(*scene, *patch.animation_sources, patch.transient_assets);
            request_actor_animation_sources(*scene, *actor, *sources);
        } catch (const std::exception& error) { return fail("INVALID_ARGUMENT", error.what()); }
    }
    if (patch.visible) actor->optics->set_properties({.visible = *patch.visible});
    if (patch.follow_camera) {
        actor->follow_camera = *patch.follow_camera;
        if (actor->engine_actor) actor->engine_actor->set_follow_camera(*patch.follow_camera);
    }
    if (patch.mechanics) {
        const auto& values = *patch.mechanics;
        if (values.mass) actor->mechanics->set_properties({.mass = *values.mass});
        if (values.restitution) actor->mechanics->set_properties({.restitution = *values.restitution});
        if (values.damping) actor->mechanics->set_properties({.damping = *values.damping});
        if (values.body_type) actor->mechanics->set_properties({.body_type = *values.body_type});
        if (values.linear_lock) actor->mechanics->set_properties({.linear_lock = std::array<bool, 3>{(*values.linear_lock)[0], (*values.linear_lock)[1], (*values.linear_lock)[2]}});
        if (values.angular_lock) actor->mechanics->set_properties({.angular_lock = std::array<bool, 3>{(*values.angular_lock)[0], (*values.angular_lock)[1], (*values.angular_lock)[2]}});
    }
    if (patch.camera_lock) {
        actor->camera_lock_enabled = patch.camera_lock->enabled;
        actor->camera_lock_position_offset = position_offset;
        actor->camera_lock_rotation_offset = rotation_offset;
        auto& controller = Corona::Systems::CameraFollowController::instance();
        if (patch.camera_lock->enabled) controller.set_target(actor->engine_actor->get_handle(),
            camera->engine_camera->get_handle(), position_offset[0], position_offset[1], position_offset[2]);
        else controller.clear_target();
    }
    if (new_name) actor->name = *new_name;
    actor->actor_version = std::max(actor->actor_version + 1, 1);
    ++scene->tree_revision;
    reply.write.properties = actor_properties_snapshot(*scene, *actor);
    reply.write.properties.requested_animation_sources = std::move(sources);
    return reply;
}

ActorPropertiesSnapshot actor_properties_snapshot(const NativeEditorScene& scene, const NativeEditorActor& actor) {
    ActorPropertiesSnapshot result;
    result.scene = scene.route; result.actor = actor.name; result.actor_guid = actor.actor_guid;
    result.actor_version = actor.actor_version; result.follow_camera = actor.follow_camera;
    if (actor.optics) result.visible = actor.optics->get_state().visible;
    if (actor.mechanics) {
        const auto mechanics_state = actor.mechanics->get_state();
        result.mechanics = ActorMechanicsSnapshot{mechanics_state.mass, mechanics_state.restitution,
            mechanics_state.damping, mechanics_state.body_type, mechanics_state.linear_lock,
            mechanics_state.angular_lock};
    }
    result.camera_lock = {actor.camera_lock_enabled, actor.camera_lock_position_offset, actor.camera_lock_rotation_offset};
    return result;
}

std::vector<std::string> prepare_animation_sources(const NativeEditorScene& scene, const std::vector<std::string>& value, bool memory_only) {
    auto sources = AnimationConfig::parse_sources(value);
    if (sources.empty() || scene.read_only || memory_only) return sources;
    SceneFolders::SceneAssetStore store(scene.project_root);
    for (auto& source : sources) {
        const auto path = resolve_project_path(scene.project_root, source);
        const auto imported = store.import_model(path); // also copies glTF buffers and other sidecars
        if (!imported.ok()) throw std::invalid_argument("Animation asset import failed: " + source);
        source = imported.main_route;
    }
    return AnimationConfig::parse_sources(sources); // content-addressed duplicates are invalid too
}

void request_actor_animation_sources(const NativeEditorScene& scene, const NativeEditorActor& actor,
                                     std::vector<std::string> sources) {
    auto player = actor_animation_player(actor);
    if (!player) throw std::invalid_argument("Actor has no geometry");
    std::vector<std::filesystem::path> paths;
    for (const auto& source : sources) {
        auto path = path_from_utf8(source);
        paths.push_back(path.is_absolute() ? path : scene.project_root / path);
    }
    player->set_sources(std::move(sources), std::move(paths));
}

}
  // namespace Corona::Systems::UI
