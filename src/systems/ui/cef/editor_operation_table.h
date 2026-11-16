#pragma once

// The Web surface, declared once.
//
// window.corona.<group>.<camel_case(name)> is the only thing this table projects: the
// ScriptRuntime contract it used to share was removed with the script runtime and will be
// redesigned. A row names the enum that owns its operation, so one table covers every
// namespace the surface publishes; the V8 readers for the scene rows are looked up by
// SceneMethod in cef_v8_binding.cpp.

#include "cef_operations.h"
#include "cef_scene_operations.h"
#include "editor_operation_methods.h"

#include <string>
#include <string_view>
#include <variant>

namespace Corona::Systems::UI {

/// One operation, whichever namespace owns it. `NativeEventKind` is an alternative because
/// the two window events are names the surface publishes too; they are installed by a
/// different V8 handler class, and the install loop branches on the alternative.
using EditorMethod = std::variant<SceneMethod, NetworkMethod, ProjectLauncherMethod,
                                  FilesMethod, ProjectSettingsMethod, WindowMethod,
                                  ScriptInputMethod, BridgeMethod, NativeEventKind>;

struct EditorOperationSpec {
    std::string_view group;
    std::string_view name;
    EditorMethod method;
    std::string_view summary;
};

inline constexpr EditorOperationSpec kEditorOperations[] = {
    {"scene", "read_scene", SceneMethod::ReadScene, "The scene tree, cameras and actor projection the Web reads."},
    {"scene", "read_active", SceneMethod::ReadActive, "Archive session, scene route, name, project path, tree revision, selection."},
    {"scene", "select_model_file", SceneMethod::SelectModelFile, "Native file dialog that answers the chosen model path."},
    {"scene", "import_resource_file", SceneMethod::ImportResourceFile, "Imports a resource file of the named type into the project."},
    {"scene", "read_model_nodes", SceneMethod::ReadModelNodes, "One page of a model's node hierarchy."},
    {"scene", "set_model_node_state", SceneMethod::SetModelNodeState, "Writes model node visibility and per-node overrides."},
    {"scene", "create_actor", SceneMethod::CreateActor, "Creates an actor from an asset path."},
    {"scene", "read_actor_properties", SceneMethod::ReadActorProperties, "The actor's own state: name, visibility, mechanics, camera lock, animation sources."},
    {"scene", "read_actor_rig", SceneMethod::ReadActorRig, "Skeleton nodes and IK chains for one actor."},
    {"scene", "set_actor_ik_chains", SceneMethod::SetActorIkChains, "Writes IK chains against the expected revision."},
    {"scene", "read_transforms", SceneMethod::ReadTransforms, "Transforms for several guids; unknown guids are skipped, not an error."},
    {"scene", "select_actor", SceneMethod::SelectActor, "Writes the editor selection the scene tree renders."},
    {"scene", "set_actor_transform", SceneMethod::SetActorTransform, "Writes transform components; omitted components keep their value."},
    {"scene", "update_actor", SceneMethod::UpdateActor, "Patches the actor's own state (not its body)."},
    {"scene", "delete_actor", SceneMethod::DeleteActor, "Removes an actor and reports what was removed."},
    {"scene", "rebind_actor_resource", SceneMethod::RebindActorResource, "Points an actor at a different resource path."},
    {"scene", "save", SceneMethod::Save, "Writes the scene through the archive service and reports unresolved actors."},
    {"scene", "read_environment", SceneMethod::GetEnvironment, "The scene environment: sun, grid, physics scalars."},
    {"scene", "set_environment", SceneMethod::SetEnvironment, "Patches the environment."},
    {"scene", "focus_actor", SceneMethod::FocusActor, "Moves a camera so it frames one actor."},
    {"scene", "update_camera", SceneMethod::UpdateCamera, "Patches camera properties and pose."},
    {"scene", "set_camera_render_settings", SceneMethod::SetCameraRenderSettings, "Writes Vision render settings for one camera."},
    {"scene", "read_camera_render_settings", SceneMethod::ReadCameraRenderSettings, "Reads Vision render settings for one camera."},
    {"scene", "create_camera", SceneMethod::CreateCamera, "Creates a camera in a scene."},
    {"scene", "delete_camera", SceneMethod::DeleteCamera, "Deletes a camera; the last remaining camera is protected."},
    {"audio", "play", SceneMethod::PlayAudioResource, "Plays the audio attached to an actor; the Web also plays a bare resource."},
    {"audio", "stop", SceneMethod::StopAudioResource, "Stops the audio attached to an actor; the Web also stops a bare resource."},
    {"app", "is_vision_available", SceneMethod::ReadVisionAvailability, "Answers whether Vision can render on this machine."},
    {"app", "close_process", WindowMethod::CloseProcess, "Closes the whole process; only the main window may ask."},
    {"viewport", "capture_screenshot", SceneMethod::CaptureScreenshot, "Captures one viewport frame to a file."},
    {"viewport", "configure", WindowMethod::ConfigureViewport, "Configures a viewport's mode, bounds and calibration."},
    {"network", "start_session", NetworkMethod::StartSession, "Starts a collaboration session as host or client."},
    {"network", "stop_session", NetworkMethod::StopSession, "Ends the running collaboration session."},
    {"network", "get_session_info", NetworkMethod::GetSessionInfo, "The running session's identity, role and port."},
    {"network", "get_discovered_peers", NetworkMethod::GetDiscoveredPeers, "The peers LAN discovery has reported so far."},
    {"network", "clear_discovered_peers", NetworkMethod::ClearDiscoveredPeers, "Forgets the peers discovered so far."},
    {"network", "search_lan", NetworkMethod::SearchLan, "Broadcasts a LAN discovery probe."},
    {"network", "connect_to_peer", NetworkMethod::ConnectToPeer, "Joins a peer's session by address, port and name."},
    {"network", "set_project_root", NetworkMethod::SetProjectRoot, "Scopes discovery to one project root."},
    {"project", "open_project", ProjectLauncherMethod::Open, "Opens a recent project in the editor."},
    {"project", "open_project_file", ProjectLauncherMethod::SelectFile, "Asks the native launcher for a project file to open."},
    {"project", "create_world_project", ProjectLauncherMethod::CreateWorld, "Creates a world project."},
    {"project", "create_multiplayer_project", ProjectLauncherMethod::CreateMultiplayer, "Creates a multiplayer project."},
    {"project", "get_recent_projects", ProjectLauncherMethod::GetRecent, "The recent-project list the launcher renders."},
    {"project", "get_project_load_status", ProjectLauncherMethod::GetLoadStatus, "Whether the project the launcher asked for has finished loading."},
    {"project", "open_document", FilesMethod::Open, "Opens a project document; the native scene may reload behind it."},
    {"project", "save_active_project_info", ProjectSettingsMethod::SaveInfo, "Writes the active project's metadata."},
    {"window", "close", WindowMethod::Close, "Closes a window or panel; the identity decides which."},
    {"window", "focus", WindowMethod::Focus, "Focuses the window the panel identity belongs to."},
    {"window", "create_panel", WindowMethod::CreatePanel, "Creates a panel and answers its identity."},
    {"window", "set_window_presentation", WindowMethod::SetWindowPresentation, "Moves a window to the named presentation state."},
    {"window", "on_closed", NativeEventKind::Closed, "Subscribes to the event a window publishes after it closes."},
    {"window", "on_close_requested", NativeEventKind::CloseRequested, "Subscribes to a window's close request before it closes."},
    {"input", "dispatch", ScriptInputMethod::Dispatch, "Dispatches one input event to a camera's viewport."},
    {"bridge", "call", BridgeMethod::Call, "The one Web -> Python request entry; Python owns every service behind it."},
};

static_assert(sizeof(kEditorOperations) / sizeof(kEditorOperations[0]) == 55,
              "the Web contract is fifty-five named operations");

/// The row for (group, name), or nullptr when the pair is not in the contract.
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
