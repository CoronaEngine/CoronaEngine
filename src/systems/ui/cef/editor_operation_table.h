#pragma once

// The one place an editor operation's name is declared.
//
// Two projections read this table and nothing else:
//   - `window.corona.<group>.<camel_case(name)>`  (Web, installed by cef_v8_binding.cpp)
//   - `CoronaEngine.<group>.<name>`               (ScriptRuntime, installed by cef_py_bind.cpp)
//
// The camelCase spelling is derived, never written down twice: a rename here renames the
// Web method too, so the two surfaces cannot drift apart. `web` rows carry the SceneMethod
// the V8 dispatcher switches on; `script` rows are the restricted subset role scripts may
// call. A row may be neither (unused today) but not both absent.
//
// The table deliberately has no CEF types: cef_py_bind.cpp includes it without V8, and the
// argument readers stay in cef_v8_binding.cpp where CefV8Value is available.
//
// `script_arity` is the number of documented positions (optional ones included); it is the
// table's own consistency check, not a runtime gate - nanobind enforces the real signature.

#include "cef_scene_operations.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace Corona::Systems::UI {

struct EditorOperationSpec {
    std::string_view group;             ///< "scene" / "audio" / "app" / "viewport"
    std::string_view name;              ///< canonical name, snake_case
    std::optional<SceneMethod> method;  ///< the Web projection's operation; nullopt when script-only
    bool web;
    bool script;
    std::size_t script_arity;           ///< documented positions, optional ones included
    std::string_view arguments;         ///< documented argument shape, e.g. "[scene, {archive_session?}]"
    std::string_view summary;
};

inline constexpr EditorOperationSpec kEditorOperations[] = {
    // ---- scene -------------------------------------------------------------------------
    {"scene", "read_scene", SceneMethod::ReadScene, true, true, 1,
     "[{archive_session, scene, actors?, cameras?}]",
     "The scene tree, cameras and actor projection the Web reads."},
    {"scene", "read_active", SceneMethod::ReadActive, true, true, 0,
     "[]",
     "Archive session, scene route, name, project path, tree revision, selection."},
    {"scene", "select_model_file", SceneMethod::SelectModelFile, true, false, 0,
     "[]",
     "Native file dialog that answers the chosen model path."},
    {"scene", "import_resource_file", SceneMethod::ImportResourceFile, true, false, 0,
     "[]",
     "Imports a resource file of the named type into the project."},
    {"scene", "read_model_nodes", SceneMethod::ReadModelNodes, true, false, 0,
     "[]",
     "One page of a model's node hierarchy."},
    {"scene", "set_model_node_state", SceneMethod::SetModelNodeState, true, false, 0,
     "[]",
     "Writes model node visibility and per-node overrides."},
    {"scene", "create_actor", SceneMethod::CreateActor, true, true, 3,
     "[scene, path, {archive_session, actor_type?, actor_name?, audio_resource_id?}]",
     "Creates an actor from an asset path."},
    {"scene", "read_actor", std::nullopt, false, true, 1,
     "[{archive_session, actor}]",
     "One actor by guid or name, projected the way the Web sees it."},
    {"scene", "read_actor_properties", SceneMethod::ReadActorProperties, true, false, 0,
     "[]",
     "The actor's own state: name, visibility, mechanics, camera lock, animation sources."},
    {"scene", "read_actor_rig", SceneMethod::ReadActorRig, true, false, 0,
     "[]",
     "Skeleton nodes and IK chains for one actor."},
    {"scene", "set_actor_ik_chains", SceneMethod::SetActorIkChains, true, false, 0,
     "[]",
     "Writes IK chains against the expected revision."},
    {"scene", "read_transforms", SceneMethod::ReadTransforms, true, true, 1,
     "[{archive_session, actors[]}]",
     "Transforms for several guids; unknown guids are skipped, not an error."},
    {"scene", "select_actor", SceneMethod::SelectActor, true, false, 0,
     "[]",
     "Writes the editor selection the scene tree renders."},
    {"scene", "set_actor_transform", SceneMethod::SetActorTransform, true, true, 3,
     "[scene, actor, {archive_session, position?, rotation?, scale?}]",
     "Writes transform components; omitted components keep their value."},
    {"scene", "update_actor", SceneMethod::UpdateActor, true, true, 3,
     "[scene, actor, {archive_session, name?, visible?, ...}]",
     "Patches the actor's own state (not its body)."},
    {"scene", "delete_actor", SceneMethod::DeleteActor, true, true, 3,
     "[scene, actor, {archive_session}]",
     "Removes an actor and reports what was removed."},
    {"scene", "rebind_actor_resource", SceneMethod::RebindActorResource, true, false, 0,
     "[]",
     "Points an actor at a different resource path."},
    {"scene", "save", SceneMethod::Save, true, false, 0,
     "[]",
     "Writes the scene through the archive service and reports unresolved actors."},
    {"scene", "read_environment", SceneMethod::GetEnvironment, true, true, 2,
     "[scene, {archive_session}]",
     "The scene environment: sun, grid, physics scalars."},
    {"scene", "set_environment", SceneMethod::SetEnvironment, true, true, 2,
     "[scene, {archive_session?, sun?, grid?, physics?}]",
     "Patches the environment."},
    {"scene", "focus_actor", SceneMethod::FocusActor, true, false, 0,
     "[]",
     "Moves a camera so it frames one actor."},
    {"scene", "update_camera", SceneMethod::UpdateCamera, true, false, 0,
     "[]",
     "Patches camera properties and pose."},
    {"scene", "set_camera_render_settings", SceneMethod::SetCameraRenderSettings, true, false, 0,
     "[]",
     "Writes Vision render settings for one camera."},
    {"scene", "read_camera_render_settings", SceneMethod::ReadCameraRenderSettings, true, false, 0,
     "[]",
     "Reads Vision render settings for one camera."},
    {"scene", "create_camera", SceneMethod::CreateCamera, true, false, 0,
     "[]",
     "Creates a camera in a scene."},
    {"scene", "delete_camera", SceneMethod::DeleteCamera, true, false, 0,
     "[]",
     "Deletes a camera; the last remaining camera is protected."},
    {"scene", "read_archive_scene", std::nullopt, false, true, 1,
     "[{archive_session}]",
     "The persisted scene values: actors, cameras, environment, terrain."},
    {"scene", "set_actor_animation", std::nullopt, false, true, 4,
     "[scene, actor, animation, blend_seconds?]",
     "Requests an animation clip; fails with ANIMATION_NOT_READY when no clip is loaded."},
    {"scene", "pause_actor_animation", std::nullopt, false, true, 2,
     "[scene, actor]",
     "Pauses the actor's animation player."},
    {"scene", "resume_actor_animation", std::nullopt, false, true, 2,
     "[scene, actor]",
     "Resumes the actor's animation player."},
    {"scene", "set_actor_physics", std::nullopt, false, true, 3,
     "[scene, actor, {archive_session?, mass?, body_type?, ...}]",
     "Patches the actor's body properties."},
    // ---- audio -------------------------------------------------------------------------
    {"audio", "play", SceneMethod::PlayAudioResource, true, true, 1,
     "[{scene, actor, archive_session?, loop?}]",
     "Plays the audio attached to an actor; the Web also plays a bare resource."},
    {"audio", "stop", SceneMethod::StopAudioResource, true, true, 1,
     "[{scene, actor, archive_session?}]",
     "Stops the audio attached to an actor; the Web also stops a bare resource."},
    // ---- app ---------------------------------------------------------------------------
    {"app", "is_vision_available", SceneMethod::ReadVisionAvailability, true, false, 0,
     "[]",
     "Answers whether Vision can render on this machine."},
    // ---- viewport ----------------------------------------------------------------------
    {"viewport", "capture_screenshot", SceneMethod::CaptureScreenshot, true, false, 0,
     "[]",
     "Captures one viewport frame to a file."},
    {"viewport", "set_camera_pose", std::nullopt, false, true, 3,
     "[scene, camera, {position?, forward?, up?, fov?, ...}]",
     "Writes a camera pose. The script channel may write pose; it may not retarget cameras."},
};

static_assert(sizeof(kEditorOperations) / sizeof(kEditorOperations[0]) == 36,
              "the editor contract is thirty-six named operations");

/// The row for `(group, name)`, or nullptr when the pair is not in the contract.
constexpr const EditorOperationSpec* find_editor_operation(std::string_view group,
                                                           std::string_view name) {
    for (const auto& spec : kEditorOperations) {
        if (spec.group == group && spec.name == name) return &spec;
    }
    return nullptr;
}

/// "read_actor_rig" -> "readActorRig". The Web method name is derived, never declared.
inline std::string camel_case(std::string_view snake) {
    std::string result;
    result.reserve(snake.size());
    bool upper = false;
    for (const char ch : snake) {
        if (ch == '_') {
            upper = true;
            continue;
        }
        result.push_back(upper && ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 'a' + 'A') : ch);
        upper = false;
    }
    return result;
}

}  // namespace Corona::Systems::UI
