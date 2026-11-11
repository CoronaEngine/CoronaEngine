#pragma once

// Resolving a request to scene state: which scene it means, whether its archive session is
// still the active one, and where an actor lives.
//
// These are the questions every editor operation asks first. They used to be
// anonymous-namespace locals of the handler unit, which is why no other translation unit
// could resolve a request at all.

#include "native_api_types.h"
#include "native_editor_state.h"

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace Corona::Systems::UI {

/// Stored routes use forward slashes and no leading "./"; this is the one normaliser.
std::string normalize_route(std::string route);

std::filesystem::path resolve_project_path(const std::filesystem::path& project_root,
                                           const std::string& route);

/// The process scene, created on first use. Pass a project path when a request may open a
/// different project than the one currently loaded.
NativeEditorScene* ensure_native_editor_scene(const std::string& project_path_arg = {});

/// True when the request names this scene's route (or names no route at all).
bool native_scene_request_matches(const NativeEditorScene& scene,
                                  const std::string& scene_route_arg);

/// The scene the request addresses, or nullptr when the route belongs to another project.
NativeEditorScene* resolve_native_editor_scene_request(NativeEditorScene* scene,
                                                       const std::string& scene_route_arg);

/// The scene named by `request.args[scene_arg_index]`, resolved against the active one.
NativeEditorScene* scene_for_request_route(const NativeRequest& request,
                                           std::size_t scene_arg_index = 0);

/// False when the request carries an archive session that is no longer the active one.
bool matches_archive_session(const NativeEditorScene& scene, const nlohmann::json& request);

NativeEditorActor* find_native_actor(NativeEditorScene& scene, const std::string& actor_name);
NativeEditorActor* find_native_actor_by_guid(NativeEditorScene& scene,
                                             const std::string& actor_guid);

}  // namespace Corona::Systems::UI
