#include "editor_scene_access.h"

#include "editor_paths.h"
#include "editor_text_utils.h"

#include <algorithm>
#include <string>

namespace Corona::Systems::UI {

std::string normalize_route(std::string route) {
    route = trim_ascii(std::move(route));
    std::replace(route.begin(), route.end(), '\\', '/');
    return route;
}

std::filesystem::path resolve_project_path(const std::filesystem::path& project_root,
                                           const std::string& route) {
    const auto route_path = path_from_utf8(route);
    if (route_path.is_absolute()) {
        return route_path;
    }
    return project_root / route_path;
}

NativeEditorScene* ensure_native_editor_scene(const std::string& project_path_arg) {
    auto& state = native_editor_state();
    (void)project_path_arg;
    if (!state.scene) {
        throw std::runtime_error("No committed native editor scene; open a project first");
    }
    return state.scene.get();
}

bool native_scene_request_matches(const NativeEditorScene& scene,
                                  const std::string& scene_route_arg) {
    const auto requested = normalize_route(scene_route_arg);
    if (requested.empty()) {
        return true;
    }

    // Relative requests identify only the current scene. Absolute requests must
    // include the current project root; matching by filename crosses projects.
    const auto requested_path = path_from_utf8(requested);
    if (requested_path.is_absolute() || requested_path.has_parent_path()) {
        return absolute_normalized_path(resolve_project_path(scene.project_root, requested)) ==
               absolute_normalized_path(scene.project_root / path_from_utf8(scene.route));
    }
    return requested == scene.route || requested == scene.name;

}

NativeEditorScene* resolve_native_editor_scene_request(NativeEditorScene* scene,
                                                       const std::string& scene_route_arg) {
    if (scene == nullptr || native_scene_request_matches(*scene, scene_route_arg)) {
        return scene;
    }

    throw std::runtime_error("Request does not identify the current scene: " + scene_route_arg);
}

NativeEditorScene* scene_for_request_route(const NativeRequest& request, std::size_t scene_arg_index) {
    auto* scene = ensure_native_editor_scene();
    const auto scene_route = normalize_route(arg_string(request.args, scene_arg_index));
    return resolve_native_editor_scene_request(scene, scene_route);
}

bool matches_archive_session(const NativeEditorScene& scene, const nlohmann::json& request) {
    return !scene.archive_session.empty() && request.value("archive_session", std::string{}) == scene.archive_session;
}

NativeEditorActor* find_native_actor(NativeEditorScene& scene, const std::string& actor_name) {
    for (auto& actor : scene.actors) {
        if (actor.name == actor_name || actor.actor_guid == actor_name ||
            (actor.engine_actor && std::to_string(actor.engine_actor->get_handle()) == actor_name))
        {
            return &actor;
        }
    }
    return nullptr;
}

NativeEditorActor* find_native_actor_by_guid(NativeEditorScene& scene,
                                            const std::string& actor_guid) {
    if (actor_guid.empty()) return nullptr;
    for (auto& actor : scene.actors) {
        if (actor.actor_guid == actor_guid) return &actor;
    }
    return nullptr;
}

}
  // namespace Corona::Systems::UI
