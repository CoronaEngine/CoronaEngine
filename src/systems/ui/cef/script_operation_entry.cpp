#include "script_operation_entry.h"

#include "editor_scene_access.h"
#include "native_editor_state.h"
#include "script_runtime_api.h"

namespace Corona::Systems::UI {
namespace {

NativeRequest make_request(const char* operation, nlohmann::json args) {
    NativeRequest request;
    request.module = "ScriptRuntime";
    request.function = operation;
    request.args = std::move(args);
    return request;
}

nlohmann::json object_or_empty(const nlohmann::json& value) {
    return value.is_object() ? value : nlohmann::json::object();
}

std::string string_or_empty(const nlohmann::json& value, const char* key) {
    if (!value.is_object()) return {};
    const auto it = value.find(key);
    if (it == value.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

}  // namespace

NativeResult script_entry_read_environment(std::string_view scene, const nlohmann::json& scope) {
    return script_scene_get_environment(
        make_request("read_environment", nlohmann::json::array({std::string(scene), object_or_empty(scope)})));
}

NativeResult script_entry_read_active() {
    return script_scene_read_active_scene(make_request("read_active", nlohmann::json::array()));
}

NativeResult script_entry_read_actor(const nlohmann::json& request) {
    return script_scene_read_actor(make_request("read_actor", nlohmann::json::array({object_or_empty(request)})));
}

NativeResult script_entry_read_transforms(const nlohmann::json& request) {
    return script_scene_read_transforms(
        make_request("read_transforms", nlohmann::json::array({object_or_empty(request)})));
}

NativeResult script_entry_read_archive_scene(const nlohmann::json& request) {
    return script_scene_read_archive_scene(
        make_request("read_archive_scene", nlohmann::json::array({object_or_empty(request)})));
}

NativeResult script_entry_read_scene(const nlohmann::json& request) {
    const auto scene_route = string_or_empty(request, "scene");
    if (scene_route.empty()) {
        return native_scene_failure("INVALID_ARGUMENT", "read_scene needs a scene route");
    }
    // The Web reader rejects a missing session by refusing the arguments; the script
    // channel answers the same way it answers a stale one, but with a different class so
    // "not passed" (400) stays distinguishable from "passed for another archive" (409).
    if (string_or_empty(request, "archive_session").empty()) {
        return native_scene_failure("INVALID_ARGUMENT", "read_scene needs an archive_session");
    }
    const auto* active = native_editor_state().scene.get();
    if (!active || !matches_archive_session(*active, request)) {
        return native_scene_failure("PROJECT_CONTEXT_CHANGED", "PROJECT_CONTEXT_CHANGED");
    }
    return script_scene_get_scene_snapshot(make_request("read_scene", nlohmann::json::array({scene_route})));
}

NativeResult script_entry_set_actor_transform(std::string_view scene, std::string_view actor,
                                              const nlohmann::json& patch) {
    return script_scene_set_actor_transform(make_request(
        "set_actor_transform", nlohmann::json::array({std::string(scene), std::string(actor), object_or_empty(patch)})));
}

NativeResult script_entry_set_environment(std::string_view scene, const nlohmann::json& patch) {
    return script_scene_set_environment(
        make_request("set_environment", nlohmann::json::array({std::string(scene), object_or_empty(patch)})));
}

NativeResult script_entry_update_actor(std::string_view scene, std::string_view actor,
                                       const nlohmann::json& patch) {
    return script_scene_set_actor_state(make_request(
        "update_actor", nlohmann::json::array({std::string(scene), std::string(actor), object_or_empty(patch)})));
}

NativeResult script_entry_set_actor_animation(std::string_view scene, std::string_view actor,
                                              std::string_view animation, double blend_seconds) {
    return script_scene_set_actor_animation(make_request(
        "set_actor_animation",
        nlohmann::json::array({std::string(scene), std::string(actor), std::string(animation), blend_seconds})));
}

NativeResult script_entry_pause_actor_animation(std::string_view scene, std::string_view actor) {
    return script_scene_pause_actor_animation(
        make_request("pause_actor_animation", nlohmann::json::array({std::string(scene), std::string(actor)})));
}

NativeResult script_entry_resume_actor_animation(std::string_view scene, std::string_view actor) {
    return script_scene_resume_actor_animation(
        make_request("resume_actor_animation", nlohmann::json::array({std::string(scene), std::string(actor)})));
}

NativeResult script_entry_set_actor_physics(std::string_view scene, std::string_view actor,
                                            const nlohmann::json& patch) {
    return script_scene_set_actor_physics(make_request(
        "set_actor_physics", nlohmann::json::array({std::string(scene), std::string(actor), object_or_empty(patch)})));
}

NativeResult script_entry_set_camera_pose(std::string_view scene, std::string_view camera,
                                          const nlohmann::json& patch) {
    return script_scene_set_camera_pose(make_request(
        "set_camera_pose", nlohmann::json::array({std::string(scene), std::string(camera), object_or_empty(patch)})));
}

NativeResult script_entry_create_actor(std::string_view scene, std::string_view path,
                                       const nlohmann::json& options) {
    // The handler takes the actor type as its own position and the rest as the snapshot;
    // the mirrored call site passes one options object, exactly like the Web one.
    auto snapshot = object_or_empty(options);
    std::string actor_type{"model"};
    if (const auto it = snapshot.find("actor_type"); it != snapshot.end() && it->is_string()) {
        actor_type = it->get<std::string>();
        snapshot.erase(it);
    }
    return script_scene_create_actor(make_request(
        "create_actor",
        nlohmann::json::array({std::string(scene), std::string(path), std::move(actor_type), std::move(snapshot)})));
}

NativeResult script_entry_play_actor_audio(const nlohmann::json& request) {
    auto payload = object_or_empty(request);
    auto args = nlohmann::json::array(
        {string_or_empty(payload, "scene"), string_or_empty(payload, "actor"), std::move(payload)});
    return script_scene_play_actor_audio(make_request("play", std::move(args)));
}

NativeResult script_entry_stop_actor_audio(const nlohmann::json& request) {
    auto payload = object_or_empty(request);
    auto args = nlohmann::json::array(
        {string_or_empty(payload, "scene"), string_or_empty(payload, "actor"), std::move(payload)});
    return script_scene_stop_actor_audio(make_request("stop", std::move(args)));
}

NativeResult script_entry_delete_actor(std::string_view scene, std::string_view actor,
                                       const nlohmann::json& scope) {
    return script_scene_remove_actor(make_request(
        "delete_actor", nlohmann::json::array({std::string(scene), std::string(actor), object_or_empty(scope)})));
}

}  // namespace Corona::Systems::UI
