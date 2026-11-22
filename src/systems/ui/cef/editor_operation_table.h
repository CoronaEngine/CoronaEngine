#pragma once

// The Web surface, declared once: one row per `window.corona.<group>.<name>`, the name published
// verbatim. A row names the enum that owns the operation; the V8 readers for the scene rows are
// looked up by SceneMethod in cef_v8_binding.cpp.

#include "cef_operations.h"
#include "cef_scene_operations.h"
#include "editor_operation_methods.h"

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
};

inline constexpr EditorOperationSpec kEditorOperations[] = {
    {"scene", "read_scene", SceneMethod::ReadScene},
    {"scene", "read_active", SceneMethod::ReadActive},
    {"scene", "select_model_file", SceneMethod::SelectModelFile},
    {"scene", "import_resource_file", SceneMethod::ImportResourceFile},
    {"scene", "read_model_nodes", SceneMethod::ReadModelNodes},
    {"scene", "set_model_node_state", SceneMethod::SetModelNodeState},
    {"scene", "create_actor", SceneMethod::CreateActor},
    {"scene", "read_actor_properties", SceneMethod::ReadActorProperties},
    {"scene", "read_actor_rig", SceneMethod::ReadActorRig},
    {"scene", "set_actor_ik_chains", SceneMethod::SetActorIkChains},
    {"scene", "read_transforms", SceneMethod::ReadTransforms},
    {"scene", "select_actor", SceneMethod::SelectActor},
    {"scene", "set_actor_transform", SceneMethod::SetActorTransform},
    {"scene", "update_actor", SceneMethod::UpdateActor},
    {"scene", "delete_actor", SceneMethod::DeleteActor},
    {"scene", "rebind_actor_resource", SceneMethod::RebindActorResource},
    {"scene", "save", SceneMethod::Save},
    {"scene", "read_environment", SceneMethod::GetEnvironment},
    {"scene", "set_environment", SceneMethod::SetEnvironment},
    {"scene", "focus_actor", SceneMethod::FocusActor},
    {"scene", "update_camera", SceneMethod::UpdateCamera},
    {"scene", "set_camera_render_settings", SceneMethod::SetCameraRenderSettings},
    {"scene", "read_camera_render_settings", SceneMethod::ReadCameraRenderSettings},
    {"scene", "create_camera", SceneMethod::CreateCamera},
    {"scene", "delete_camera", SceneMethod::DeleteCamera},
    {"audio", "play", SceneMethod::PlayAudioResource},
    {"audio", "stop", SceneMethod::StopAudioResource},
    {"app", "close_process", WindowMethod::CloseProcess},
    {"viewport", "capture_screenshot", SceneMethod::CaptureScreenshot},
    {"viewport", "configure", WindowMethod::ConfigureViewport},
    {"network", "start_session", NetworkMethod::StartSession},
    {"network", "stop_session", NetworkMethod::StopSession},
    {"network", "get_session_info", NetworkMethod::GetSessionInfo},
    {"network", "get_discovered_peers", NetworkMethod::GetDiscoveredPeers},
    {"network", "clear_discovered_peers", NetworkMethod::ClearDiscoveredPeers},
    {"network", "search_lan", NetworkMethod::SearchLan},
    {"network", "connect_to_peer", NetworkMethod::ConnectToPeer},
    {"network", "set_project_root", NetworkMethod::SetProjectRoot},
    {"project", "open_project", ProjectLauncherMethod::Open},
    {"project", "open_project_file", ProjectLauncherMethod::SelectFile},
    {"project", "create_world_project", ProjectLauncherMethod::CreateWorld},
    {"project", "create_multiplayer_project", ProjectLauncherMethod::CreateMultiplayer},
    {"project", "get_recent_projects", ProjectLauncherMethod::GetRecent},
    {"project", "get_project_load_status", ProjectLauncherMethod::GetLoadStatus},
    {"project", "open_document", FilesMethod::Open},
    {"project", "save_active_project_info", ProjectSettingsMethod::SaveInfo},
    {"window", "close", WindowMethod::Close},
    {"window", "focus", WindowMethod::Focus},
    {"window", "create_panel", WindowMethod::CreatePanel},
    {"window", "set_window_presentation", WindowMethod::SetWindowPresentation},
    {"window", "on_closed", NativeEventKind::Closed},
    {"window", "on_close_requested", NativeEventKind::CloseRequested},
    {"input", "dispatch", ScriptInputMethod::Dispatch},
    {"bridge", "call", BridgeMethod::Call},
};

static_assert(sizeof(kEditorOperations) / sizeof(kEditorOperations[0]) == 54,
              "the Web contract is fifty-four named operations");

}  // namespace Corona::Systems::UI
