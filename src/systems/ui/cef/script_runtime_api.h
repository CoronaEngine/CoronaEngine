#pragma once

// The internal handlers behind the ScriptRuntime contract.
//
// One function per operation: it takes the packed `NativeRequest` and returns a
// `NativeResult`. The public spelling lives in `editor_operation_table.h`, the typed
// argument packing in `script_operation_entry.h`, and the Python binding in
// `cef_py_bind.cpp`; nothing here knows a name string and there is no name dispatch.
//
// A request that carries `archive_session` is validated against the scene the editor
// currently owns; a mismatch answers 409 PROJECT_CONTEXT_CHANGED rather than silently
// writing to the wrong project.
//
// Failures answer with a numeric class - 409 stale archive, 403 read-only, 404 missing actor,
// 400 malformed argument, 2 everything else - plus the scene layer's symbolic code in
// `data.code`. The Python binding raises that symbol as `EditorApiError.code`.

#include "native_api_types.h"

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
NativeResult script_scene_play_actor_audio(const NativeRequest& request);
NativeResult script_scene_stop_actor_audio(const NativeRequest& request);
NativeResult script_scene_remove_actor(const NativeRequest& request);

}  // namespace Corona::Systems::UI
