#pragma once

// Serialising scene state for the contracts: the camera list, the scene tree values and
// the environment payload. Pure projection - nothing here mutates the scene.

#include "native_editor_state.h"

#include <nlohmann/json.hpp>

namespace Corona::Systems::UI {

nlohmann::json camera_to_json(const NativeEditorCamera& camera);
nlohmann::json scene_to_json(const NativeEditorScene& scene);
nlohmann::json archive_scene_values(const NativeEditorScene& scene);
EnvironmentSnapshot native_environment_snapshot(const NativeEditorScene& scene);
nlohmann::json native_scene_environment_payload(const NativeEditorScene& scene);

// The viewport session the editor is currently showing, and whether the scene asks for a
// camera's view window to be open.
std::string current_viewport_session();
bool requested_camera_view_open(const NativeEditorCamera& camera);

}  // namespace Corona::Systems::UI
