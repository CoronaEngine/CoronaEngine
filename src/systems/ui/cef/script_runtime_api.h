#pragma once

// The ScriptRuntime editor contract: one named entry per operation.
// Reached from Python through cef_py_bind.cpp; there is no name dispatch and no
// caller mask because the module boundary is the permission boundary.

#include "cef_editor_native_api_registry.h"

#include <nlohmann/json.hpp>

namespace Corona::Systems::UI {

NativeResult script_scene_get_environment(const NativeRequest& request);
NativeResult script_scene_read_active_scene(const NativeRequest& request);
NativeResult script_scene_read_actor(const NativeRequest& request);
NativeResult script_scene_read_transforms(const NativeRequest& request);
NativeResult script_scene_read_archive_scene(const NativeRequest& request);
NativeResult script_scene_get_scene_snapshot(const NativeRequest& request);
NativeResult script_scene_set_actor_transform(const NativeRequest& request);
NativeResult script_scene_set_environment(const NativeRequest& request);
NativeResult script_scene_set_actor_state(const NativeRequest& request);
NativeResult script_scene_set_actor_animation(const NativeRequest& request);
NativeResult script_scene_pause_actor_animation(const NativeRequest& request);
NativeResult script_scene_resume_actor_animation(const NativeRequest& request);
NativeResult script_scene_set_actor_physics(const NativeRequest& request);
NativeResult script_scene_set_camera_pose(const NativeRequest& request);
NativeResult script_scene_create_actor(const NativeRequest& request);
NativeResult script_scene_remove_actor(const NativeRequest& request);

}  // namespace Corona::Systems::UI
