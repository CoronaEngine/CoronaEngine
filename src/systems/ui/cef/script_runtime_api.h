#pragma once

// The ScriptRuntime editor contract: one named entry per operation.
// Reached from Python through cef_py_bind.cpp; there is no name dispatch and no
// caller mask because the module boundary is the permission boundary.
//
// Every binding takes one JSON string holding the argument array. The array is the wire
// shape, and it is described below as data rather than prose: `kScriptOperations` is
// parsed by tests/tools/test_script_runtime_contract.py, which fails when the C++ table,
// the bindings and the Python adapter stop agreeing on a name or an argument position.
//
// A request object that carries `archive_session` is validated against the scene the
// editor currently owns; a mismatch answers 409 PROJECT_CONTEXT_CHANGED rather than
// silently writing to the wrong project.

#include "native_api_types.h"

#include <array>
#include <cstddef>

#include <nlohmann/json.hpp>

namespace Corona::Systems::UI {

/// One row of the contract: the binding name, its argument positions, and what it does.
///
/// `arguments` lists the JSON array positions in order; `[]` means the operation takes no
/// arguments. Optional fields are marked with `?` inside an object position.
struct ScriptOperationSpec {
    const char* name;
    const char* arguments;
    const char* summary;
};

inline constexpr ScriptOperationSpec kScriptOperations[] = {
    {"script_scene_get_environment", "[scene]",
     "The scene environment: sun, grid, physics scalars."},
    {"script_scene_read_active_scene", "[]",
     "Archive session, scene route, name, project path, tree revision, selection."},
    {"script_scene_read_actor", "[{archive_session, actor}]",
     "One actor by guid or name, projected the way the Web sees it."},
    {"script_scene_read_transforms", "[{archive_session, actors[]}]",
     "Transforms for several guids; unknown guids are skipped, not an error."},
    {"script_scene_read_archive_scene", "[{archive_session}]",
     "The persisted scene values: actors, cameras, environment, terrain."},
    {"script_scene_get_scene_snapshot", "[scene]",
     "The full scene snapshot built from native state."},
    {"script_scene_set_actor_transform", "[scene, actor, {position?, rotation?, scale?, archive_session?}]",
     "Writes transform components; omitted components keep their value."},
    {"script_scene_set_environment", "[scene, {archive_session?, sun?, grid?, physics?}]",
     "Patches the environment."},
    {"script_scene_set_actor_state", "[scene, actor, {archive_session?, name?, visible?, ...}]",
     "Patches the actor's own state (not its body)."},
    {"script_scene_set_actor_animation", "[scene, actor, animation, blend_seconds?]",
     "Requests an animation clip; fails with ANIMATION_NOT_READY when no clip is loaded."},
    {"script_scene_pause_actor_animation", "[scene, actor]",
     "Pauses the actor's animation player."},
    {"script_scene_resume_actor_animation", "[scene, actor]",
     "Resumes the actor's animation player."},
    {"script_scene_set_actor_physics", "[scene, actor, {archive_session?, mass?, body_type?, ...}]",
     "Patches the actor's body properties."},
    {"script_scene_set_camera_pose", "[scene, camera, {position?, forward?, up?, fov?, ...}]",
     "Writes a camera pose. The script channel may write pose; it may not retarget cameras."},
    {"script_scene_create_actor", "[scene, source_path, actor_type, {archive_session?, ...}]",
     "Creates an actor from an asset path; the script channel owns no assets, so it never "
     "imports one."},
    {"script_scene_remove_actor", "[scene, actor]",
     "Removes an actor and reports what was removed."},
};

static_assert(std::size(kScriptOperations) == 16,
              "the ScriptRuntime contract is exactly sixteen named operations");

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
