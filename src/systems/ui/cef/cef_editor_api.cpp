#include "cef_editor_api.h"

#include <corona/kernel/core/i_logger.h>

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Corona::Systems::UI {
namespace {

constexpr std::uint32_t caller_mask(EditorApiCaller caller) {
    return static_cast<std::uint32_t>(caller);
}

constexpr std::uint32_t all_callers() {
    return caller_mask(EditorApiCaller::Cef) | caller_mask(EditorApiCaller::PythonScript);
}

constexpr EditorApiReturnSpec returns(EditorApiValueType type) {
    return EditorApiReturnSpec{type};
}

constexpr EditorApiParamSpec param(const char* name, EditorApiValueType type, bool optional = false) {
    return EditorApiParamSpec{name, type, optional};
}

constexpr std::array<EditorApiParamSpec, 0> kNoParams = {};

constexpr std::array<EditorApiParamSpec, 1> kSceneNameParam = {{
    param("scene_name", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 2> kSceneSaveParams = {{
    param("scene_name", EditorApiValueType::String),
    param("snapshot", EditorApiValueType::Object, true),
}};

constexpr std::array<EditorApiParamSpec, 1> kSceneNameOptionalParam = {{
    param("scene_name", EditorApiValueType::String, true),
}};

constexpr std::array<EditorApiParamSpec, 1> kPathParam = {{
    param("path", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 2> kOpenProjectParams = {{
    param("path", EditorApiValueType::String),
    param("options", EditorApiValueType::Object, true),
}};

constexpr std::array<EditorApiParamSpec, 1> kPathOptionalParam = {{
    param("path", EditorApiValueType::String, true),
}};

constexpr std::array<EditorApiParamSpec, 1> kObjectPayloadParam = {{
    param("payload", EditorApiValueType::Object),
}};

constexpr std::array<EditorApiParamSpec, 1> kAnyPayloadParam = {{
    param("payload", EditorApiValueType::Any),
}};

constexpr std::array<EditorApiParamSpec, 2> kEditorApiRegisterCallbackParams = {{
    param("event_name", EditorApiValueType::String),
    param("callback_spec", EditorApiValueType::Object, true),
}};

constexpr std::array<EditorApiParamSpec, 1> kCallbackTokenParam = {{
    param("callback_token", EditorApiValueType::Integer),
}};

constexpr std::array<EditorApiParamSpec, 2> kAiToolGenerateHintParams = {{
    param("element_type", EditorApiValueType::String),
    param("context", EditorApiValueType::Object, true),
}};

constexpr std::array<EditorApiParamSpec, 2> kMainViewImportResourceFileParams = {{
    param("scene_name", EditorApiValueType::String),
    param("file_type", EditorApiValueType::String, true),
}};

constexpr std::array<EditorApiParamSpec, 2> kMainViewUpdateViewToolStateParams = {{
    param("tool_id", EditorApiValueType::String),
    param("enabled", EditorApiValueType::Boolean),
}};

constexpr std::array<EditorApiParamSpec, 2> kFileManagerCreateFolderParams = {{
    param("path", EditorApiValueType::String),
    param("folder_name", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 3> kFileManagerCreateFileParams = {{
    param("path", EditorApiValueType::String),
    param("file_name", EditorApiValueType::String),
    param("file_type", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 2> kFileManagerRenameItemParams = {{
    param("old_path", EditorApiValueType::String),
    param("new_name", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 2> kFileManagerOpenFileParams = {{
    param("path", EditorApiValueType::String),
    param("file_type", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 1> kCallerParam = {{
    param("caller", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 4> kResourceSearchFuzzySearchParams = {{
    param("query", EditorApiValueType::String),
    param("top_k", EditorApiValueType::Integer),
    param("type_filter", EditorApiValueType::String, true),
    param("caller", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 4> kResourceSearchImageSearchParams = {{
    param("image_b64", EditorApiValueType::String),
    param("top_k", EditorApiValueType::Integer),
    param("threshold", EditorApiValueType::Number),
    param("caller", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 2> kResourceSearchMarkIndexDirtyParams = {{
    param("reason", EditorApiValueType::String),
    param("caller", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 3> kResourceSearchFocusActorParams = {{
    param("scene_name", EditorApiValueType::String),
    param("actor_name", EditorApiValueType::String),
    param("caller", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 1> kActorGuidParam = {{
    param("actor_guid", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 1> kProjectRootParam = {{
    param("project_root", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 1> kPausedParam = {{
    param("paused", EditorApiValueType::Boolean),
}};

constexpr std::array<EditorApiParamSpec, 4> kNetworkStartSessionParams = {{
    param("instance_name", EditorApiValueType::String),
    param("project_id", EditorApiValueType::Integer),
    param("port", EditorApiValueType::Integer),
    param("role", EditorApiValueType::String, true),
}};

constexpr std::array<EditorApiParamSpec, 3> kNetworkConnectToPeerParams = {{
    param("ip", EditorApiValueType::String),
    param("port", EditorApiValueType::Integer),
    param("peer_name", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 4> kNetworkBroadcastActorCreateParams = {{
    param("actor_guid", EditorApiValueType::String),
    param("scene_name", EditorApiValueType::String),
    param("model_path", EditorApiValueType::String),
    param("actor_data", EditorApiValueType::Any, true),
}};

constexpr std::array<EditorApiParamSpec, 3> kNetworkActorStateUpdateParams = {{
    param("actor_guid", EditorApiValueType::String),
    param("scene_name", EditorApiValueType::String),
    param("actor_data", EditorApiValueType::Any, true),
}};

constexpr std::array<EditorApiParamSpec, 3> kNetworkBroadcastActorDeleteParams = {{
    param("actor_guid", EditorApiValueType::String),
    param("scene_name", EditorApiValueType::String),
    param("actor_name", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 2> kNetworkBroadcastSceneSnapshotParams = {{
    param("scene_name", EditorApiValueType::String),
    param("snapshot", EditorApiValueType::Any, true),
}};

constexpr std::array<EditorApiParamSpec, 3> kNetworkRegisterActorIdentityParams = {{
    param("actor_guid", EditorApiValueType::String),
    param("actor_handle", EditorApiValueType::Any),
    param("locally_owned", EditorApiValueType::Boolean, true),
}};

constexpr std::array<EditorApiParamSpec, 1> kActorNameParam = {{
    param("actor_name", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 1> kResourceIdParam = {{
    param("resource_id", EditorApiValueType::Any),
}};

constexpr std::array<EditorApiParamSpec, 2> kSceneActorParams = {{
    param("scene_name", EditorApiValueType::String),
    param("actor_name", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 4> kSceneDatasActorOperationParams = {{
    param("scene_name", EditorApiValueType::String),
    param("actor_name", EditorApiValueType::String),
    param("operation", EditorApiValueType::String),
    param("vector", EditorApiValueType::Any, true),
}};

constexpr std::array<EditorApiParamSpec, 3> kSceneDatasSelectModelFileParams = {{
    param("scene_name", EditorApiValueType::String),
    param("actor_name", EditorApiValueType::String),
    param("file_type", EditorApiValueType::String, true),
}};

constexpr std::array<EditorApiParamSpec, 2> kSceneCameraParams = {{
    param("scene_name", EditorApiValueType::String),
    param("camera_name", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 4> kSceneToolsCreateActorParams = {{
    param("scene_name", EditorApiValueType::String),
    param("obj_path", EditorApiValueType::String),
    param("actor_type", EditorApiValueType::String, true),
    param("actor_data", EditorApiValueType::Any, true),
}};

constexpr std::array<EditorApiParamSpec, 3> kSceneToolsRenameActorParams = {{
    param("scene_name", EditorApiValueType::String),
    param("actor_name", EditorApiValueType::String),
    param("name", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 3> kSceneToolsFocusActorParams = {{
    param("scene_name", EditorApiValueType::String),
    param("actor_name", EditorApiValueType::String),
    param("camera_name", EditorApiValueType::String, true),
}};

constexpr std::array<EditorApiParamSpec, 3> kSceneToolsSelectActorParams = {{
    param("scene_name", EditorApiValueType::String),
    param("actor_type", EditorApiValueType::String),
    param("actor_name", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 3> kSceneToolsSetRenderBackendParams = {{
    param("mode", EditorApiValueType::String),
    param("scene_name", EditorApiValueType::String, true),
    param("camera_name", EditorApiValueType::String, true),
}};

constexpr std::array<EditorApiParamSpec, 2> kSceneToolsCameraOptionalParams = {{
    param("scene_name", EditorApiValueType::String, true),
    param("camera_name", EditorApiValueType::String, true),
}};

constexpr std::array<EditorApiParamSpec, 3> kSceneToolsSetVisionRenderModeParams = {{
    param("scene_name", EditorApiValueType::String),
    param("camera_name", EditorApiValueType::String, true),
    param("mode", EditorApiValueType::String, true),
}};

constexpr std::array<EditorApiParamSpec, 4> kSceneToolsSetSsatViewViewerParams = {{
    param("scene_name", EditorApiValueType::String),
    param("camera_name", EditorApiValueType::String, true),
    param("mode", EditorApiValueType::String),
    param("view_index", EditorApiValueType::Integer),
}};

constexpr std::array<EditorApiParamSpec, 2> kSceneToolsReloadSceneParams = {{
    param("scene_name", EditorApiValueType::String),
    param("project_path", EditorApiValueType::String, true),
}};

constexpr std::array<EditorApiParamSpec, 3> kSceneToolsRebindActorResourceParams = {{
    param("scene_name", EditorApiValueType::String),
    param("actor_guid", EditorApiValueType::String),
    param("path", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 2> kSceneToolsCreateCameraViewParams = {{
    param("scene_name", EditorApiValueType::String),
    param("name", EditorApiValueType::String, true),
}};

constexpr std::array<EditorApiParamSpec, 3> kSceneToolsRenameCameraViewParams = {{
    param("scene_name", EditorApiValueType::String),
    param("camera_name", EditorApiValueType::String),
    param("name", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 3> kSceneToolsUpdateCameraViewParams = {{
    param("scene_name", EditorApiValueType::String),
    param("camera_name", EditorApiValueType::String),
    param("state", EditorApiValueType::Object),
}};

constexpr std::array<EditorApiParamSpec, 3> kSceneToolsSunDirectionParams = {{
    param("scene_name", EditorApiValueType::String),
    param("enabled", EditorApiValueType::Boolean),
    param("direction", EditorApiValueType::Array),
}};

constexpr std::array<EditorApiParamSpec, 2> kSceneToolsFloorGridParams = {{
    param("scene_name", EditorApiValueType::String),
    param("enabled", EditorApiValueType::Boolean),
}};

constexpr std::array<EditorApiParamSpec, 5> kSceneToolsSetPhysicsParams = {{
    param("scene_name", EditorApiValueType::String),
    param("gravity", EditorApiValueType::Array, true),
    param("floor_y", EditorApiValueType::Number, true),
    param("floor_restitution", EditorApiValueType::Number, true),
    param("fixed_dt", EditorApiValueType::Number, true),
}};

constexpr std::array<EditorApiParamSpec, 3> kSceneToolsSaveScreenshotParams = {{
    param("scene_name", EditorApiValueType::String),
    param("path", EditorApiValueType::String),
    param("camera_name", EditorApiValueType::String, true),
}};

constexpr std::array<EditorApiParamSpec, 3> kSceneToolsSetOutputModeParams = {{
    param("scene_name", EditorApiValueType::String),
    param("camera_name", EditorApiValueType::String, true),
    param("mode", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 3> kSceneToolsSetCameraBoolParams = {{
    param("scene_name", EditorApiValueType::String),
    param("camera_name", EditorApiValueType::String, true),
    param("enabled", EditorApiValueType::Boolean),
}};

constexpr std::array<EditorApiParamSpec, 5> kSceneToolsPickActorParams = {{
    param("scene_name", EditorApiValueType::String),
    param("x", EditorApiValueType::Number),
    param("y", EditorApiValueType::Number),
    param("viewport_width", EditorApiValueType::Number),
    param("viewport_height", EditorApiValueType::Number),
}};

constexpr std::array<EditorApiParamSpec, 2> kSceneToolsPlayAudioParams = {{
    param("resource_id", EditorApiValueType::Any),
    param("loop", EditorApiValueType::Boolean, true),
}};

constexpr std::array<EditorApiParamSpec, 2> kSceneToolsActorPlayAudioParams = {{
    param("actor_name", EditorApiValueType::String),
    param("loop", EditorApiValueType::Boolean, true),
}};

constexpr std::array<EditorApiParamSpec, 5> kScratchExecutePythonCodeParams = {{
    param("code", EditorApiValueType::String),
    param("mode", EditorApiValueType::Integer),
    param("scene_name", EditorApiValueType::String, true),
    param("actor_name", EditorApiValueType::String, true),
    param("target_type", EditorApiValueType::String, true),
}};

constexpr std::array<EditorApiParamSpec, 1> kScratchStopScriptParams = {{
    param("restore_state", EditorApiValueType::Boolean, true),
}};

constexpr std::array<EditorApiParamSpec, 3> kScratchKeyEventParams = {{
    param("key", EditorApiValueType::String),
    param("modifiers", EditorApiValueType::String, true),
    param("display_key", EditorApiValueType::String, true),
}};

constexpr std::array<EditorApiParamSpec, 2> kScratchKeyReleaseParams = {{
    param("key", EditorApiValueType::String),
    param("display_key", EditorApiValueType::String, true),
}};

constexpr std::array<EditorApiParamSpec, 9> kScratchMouseEventParams = {{
    param("event_type", EditorApiValueType::String),
    param("button", EditorApiValueType::String, true),
    param("x", EditorApiValueType::Number),
    param("y", EditorApiValueType::Number),
    param("viewport_x", EditorApiValueType::Number, true),
    param("viewport_y", EditorApiValueType::Number, true),
    param("viewport_width", EditorApiValueType::Number, true),
    param("viewport_height", EditorApiValueType::Number, true),
    param("picked_actor", EditorApiValueType::String, true),
}};

#define EDITOR_API_METHOD0_WRAPPED(module, function, js_path, python_path, return_type) \
    {#module "." #function, #module, #function, nullptr, 0, returns(return_type), js_path, python_path, true, all_callers()}

#define EDITOR_API_METHOD1_WRAPPED(module, function, js_path, python_path, param0_name, param0_type, return_type) \
    {#module "." #function, #module, #function, kSceneNameParam.data(), kSceneNameParam.size(), returns(return_type), js_path, python_path, true, all_callers()}

#define EDITOR_API_METHOD_SCHEMA_WRAPPED(module, function, params_array, js_path, python_path, return_type) \
    {#module "." #function, #module, #function, params_array.data(), params_array.size(), returns(return_type), js_path, python_path, true, all_callers()}

#define EDITOR_API_METHOD0(module, function, return_type) \
    EDITOR_API_METHOD0_WRAPPED(module, function, "", "", return_type)

#define EDITOR_API_METHOD1(module, function, param0_name, param0_type, return_type) \
    EDITOR_API_METHOD1_WRAPPED(module, function, "", "", param0_name, param0_type, return_type)

#define EDITOR_API_METHOD_SCHEMA(module, function, params_array, return_type) \
    EDITOR_API_METHOD_SCHEMA_WRAPPED(module, function, params_array, "", "", return_type)

constexpr std::array<EditorApiMethodSpec, 144> kEditorApiMethods = {{
    EDITOR_API_METHOD_SCHEMA_WRAPPED(AITool, submit_request, kObjectPayloadParam, "ai.submitRequest", "ai.submit_request", EditorApiValueType::Any),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(AITool, generate_hint, kAiToolGenerateHintParams, "ai.generateHint", "ai.generate_hint", EditorApiValueType::Any),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(AITool, read_local_file_as_base64, kPathParam, "ai.readLocalFileAsBase64", "ai.read_local_file_as_base64", EditorApiValueType::Any),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(AITool, send_message_to_ai_stream, kAnyPayloadParam, "ai.sendMessageToAIStream", "ai.send_message_to_ai_stream", EditorApiValueType::Any),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(CoronaEditor, close_process, kNoParams, "app.closeProcess", "app.close_process", EditorApiValueType::Null),
    EDITOR_API_METHOD_SCHEMA(EditorApi, register_callback, kEditorApiRegisterCallbackParams, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(EditorApi, unregister_callback, kCallbackTokenParam, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(EditorApi, list_events, kNoParams, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(EditorApi, list_methods, kNoParams, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(FileManager, create_file, kFileManagerCreateFileParams, EditorApiValueType::Boolean),
    EDITOR_API_METHOD_SCHEMA(FileManager, create_folder, kFileManagerCreateFolderParams, EditorApiValueType::Boolean),
    EDITOR_API_METHOD_SCHEMA(FileManager, delete_item, kPathParam, EditorApiValueType::Boolean),
    EDITOR_API_METHOD_SCHEMA(FileManager, get_file_tree, kPathOptionalParam, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(FileManager, get_files, kPathOptionalParam, EditorApiValueType::Array),
    EDITOR_API_METHOD_SCHEMA(FileManager, get_project_info, kNoParams, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(FileManager, open_file, kFileManagerOpenFileParams, EditorApiValueType::Boolean),
    EDITOR_API_METHOD_SCHEMA(FileManager, rename_item, kFileManagerRenameItemParams, EditorApiValueType::Boolean),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, add_agent, kObjectPayloadParam, "lanChat.addAgent", "lan_chat.add_agent", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, get_history, kNoParams, "lanChat.getHistory", "lan_chat.get_history", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, get_local_ip, kNoParams, "lanChat.getLocalIp", "lan_chat.get_local_ip", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, join_room, kObjectPayloadParam, "lanChat.joinRoom", "lan_chat.join_room", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, leave_room, kNoParams, "lanChat.leaveRoom", "lan_chat.leave_room", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, list_agents, kNoParams, "lanChat.listAgents", "lan_chat.list_agents", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, list_history_rooms, kNoParams, "lanChat.listHistoryRooms", "lan_chat.list_history_rooms", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, load_history_room, kObjectPayloadParam, "lanChat.loadHistoryRoom", "lan_chat.load_history_room", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, remove_agent, kObjectPayloadParam, "lanChat.removeAgent", "lan_chat.remove_agent", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, send_message, kObjectPayloadParam, "lanChat.sendMessage", "lan_chat.send_message", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, start_local_room, kObjectPayloadParam, "lanChat.startLocalRoom", "lan_chat.start_local_room", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, start_room, kObjectPayloadParam, "lanChat.startRoom", "lan_chat.start_room", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, stop_local_room, kNoParams, "lanChat.stopLocalRoom", "lan_chat.stop_local_room", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, stop_room, kNoParams, "lanChat.stopRoom", "lan_chat.stop_room", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(MainView, get_menu_data, kNoParams, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(MainView, import_resource_file, kMainViewImportResourceFileParams, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(MainView, on_init, kPathOptionalParam, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(MainView, run_project, kPathOptionalParam, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(MainView, scene_save, kSceneSaveParams, "main.sceneSave", "main.scene_save", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(MainView, update_view_tool_state, kMainViewUpdateViewToolStateParams, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, broadcast_actor_create, kNetworkBroadcastActorCreateParams, "network.broadcastActorCreate", "network.broadcast_actor_create", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, broadcast_actor_delete, kNetworkBroadcastActorDeleteParams, "network.broadcastActorDelete", "network.broadcast_actor_delete", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, broadcast_actor_scene_snapshot, kNetworkBroadcastSceneSnapshotParams, "network.broadcastSceneSnapshot", "network.broadcast_scene_snapshot", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, broadcast_actor_state_update, kNetworkActorStateUpdateParams, "network.broadcastActorStateUpdate", "network.broadcast_actor_state_update", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, broadcast_actor_transform, kNetworkActorStateUpdateParams, "network.broadcastActorTransform", "network.broadcast_actor_transform", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, claim_actor_ownership, kActorGuidParam, "network.claimActorOwnership", "network.claim_actor_ownership", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, connect_to_peer, kNetworkConnectToPeerParams, "network.connectToPeer", "network.connect_to_peer", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, get_peer_count, kNoParams, "network.getPeerCount", "network.get_peer_count", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, get_session_info, kNoParams, "network.getSessionInfo", "network.get_session_info", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, poll_pending_actor_create, kNoParams, "network.pollPendingActorCreate", "network.poll_pending_actor_create", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, poll_pending_actor_delete, kNoParams, "network.pollPendingActorDelete", "network.poll_pending_actor_delete", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, poll_pending_actor_scene_snapshot, kNoParams, "network.pollPendingSceneSnapshot", "network.poll_pending_scene_snapshot", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, poll_pending_actor_scene_snapshot_request, kNoParams, "network.pollPendingSceneSnapshotRequest", "network.poll_pending_scene_snapshot_request", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, poll_pending_actor_state_update, kNoParams, "network.pollPendingActorStateUpdate", "network.poll_pending_actor_state_update", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, poll_pending_actor_transform, kNoParams, "network.pollPendingActorTransform", "network.poll_pending_actor_transform", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, register_actor_identity, kNetworkRegisterActorIdentityParams, "network.registerActorIdentity", "network.register_actor_identity", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, request_actor_scene_snapshot, kSceneNameParam, "network.requestSceneSnapshot", "network.request_scene_snapshot", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, set_project_root, kProjectRootParam, "network.setProjectRoot", "network.set_project_root", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, set_sync_paused, kPausedParam, "network.setSyncPaused", "network.set_sync_paused", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, start_session, kNetworkStartSessionParams, "network.startSession", "network.start_session", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, stop_session, kNoParams, "network.stopSession", "network.stop_session", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(ProjectLauncher, browse_folder, kPathOptionalParam, EditorApiValueType::String),
    EDITOR_API_METHOD0_WRAPPED(ProjectLauncher, choose_portable_scene_target, "project.choosePortableSceneTarget", "project.choose_portable_scene_target", EditorApiValueType::String),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(ProjectLauncher, cleanup_portable_scene_assets, kObjectPayloadParam, "project.cleanupPortableSceneAssets", "project.cleanup_portable_scene_assets", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(ProjectLauncher, create_multiplayer_project, kObjectPayloadParam, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(ProjectLauncher, create_project, kObjectPayloadParam, EditorApiValueType::String),
    EDITOR_API_METHOD_SCHEMA(ProjectLauncher, create_world_project, kObjectPayloadParam, EditorApiValueType::Object),
    EDITOR_API_METHOD0_WRAPPED(ProjectLauncher, get_app_version, "project.getAppVersion", "project.get_app_version", EditorApiValueType::String),
    EDITOR_API_METHOD_SCHEMA(ProjectLauncher, get_default_project_path, kNoParams, EditorApiValueType::String),
    EDITOR_API_METHOD_SCHEMA(ProjectLauncher, get_project_load_status, kNoParams, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(ProjectLauncher, get_recent_projects, kNoParams, EditorApiValueType::Array),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(ProjectLauncher, migrate_legacy_scene, kObjectPayloadParam, "project.migrateLegacyScene", "project.migrate_legacy_scene", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(ProjectLauncher, import_portable_asset, kObjectPayloadParam, "project.importPortableAsset", "project.import_portable_asset", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(ProjectLauncher, open_project, kOpenProjectParams, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(ProjectLauncher, open_project_file, kNoParams, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(ProjectLauncher, validate_portable_scene, kObjectPayloadParam, "project.validatePortableScene", "project.validate_portable_scene", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(ProjectLauncher, set_project_mode, kObjectPayloadParam, EditorApiValueType::Boolean),
    EDITOR_API_METHOD_SCHEMA(ProjectSettings, browse_scene_file, kNoParams, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(ProjectSettings, get_active_project_info, kNoParams, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(ProjectSettings, save_active_project_info, kObjectPayloadParam, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(ResourceSearch, focus_actor, kResourceSearchFocusActorParams, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(ResourceSearch, fuzzy_search, kResourceSearchFuzzySearchParams, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(ResourceSearch, get_stats, kCallerParam, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(ResourceSearch, image_search, kResourceSearchImageSearchParams, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(ResourceSearch, list_types, kCallerParam, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(ResourceSearch, mark_index_dirty, kResourceSearchMarkIndexDirtyParams, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(ResourceSearch, prepare_index, kCallerParam, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(ResourceSearch, rebuild_index, kCallerParam, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneDatas, actor_operation, kSceneDatasActorOperationParams, "", "scene_datas.actor_operation", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneDatas, get_actor, kSceneActorParams, "", "scene_datas.get_actor", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneDatas, get_scene, kSceneNameOptionalParam, "", "scene_datas.get_scene", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(SceneDatas, save_actor, kSceneActorParams, EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA(SceneDatas, select_model_file, kSceneDatasSelectModelFileParams, EditorApiValueType::String),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, actor_play_audio, kSceneToolsActorPlayAudioParams, "sceneTools.actorPlayAudio", "scene_tools.actor_play_audio", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, actor_stop_audio, kActorNameParam, "sceneTools.actorStopAudio", "scene_tools.actor_stop_audio", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, close_camera_view, kSceneCameraParams, "sceneTools.closeCameraView", "scene_tools.close_camera_view", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, create_actor, kSceneToolsCreateActorParams, "sceneTools.createActor", "scene_tools.create_actor", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, create_camera_view, kSceneToolsCreateCameraViewParams, "sceneTools.createCameraView", "scene_tools.create_camera_view", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, create_scene, kSceneNameParam, "sceneTools.createScene", "scene_tools.create_scene", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, delete_camera, kSceneCameraParams, "sceneTools.deleteCamera", "scene_tools.delete_camera", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, floor_grid, kSceneToolsFloorGridParams, "sceneTools.floorGrid", "scene_tools.floor_grid", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, focus_actor, kSceneToolsFocusActorParams, "sceneTools.focusActor", "scene_tools.focus_actor", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, get_output_mode, kSceneToolsCameraOptionalParams, "sceneTools.getOutputMode", "scene_tools.get_output_mode", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, get_physics_params, kSceneNameParam, "sceneTools.getPhysicsParams", "scene_tools.get_physics_params", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, get_render_backend, kSceneToolsCameraOptionalParams, "sceneTools.getRenderBackend", "scene_tools.get_render_backend", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, get_shadow_cascade_debug, kSceneToolsCameraOptionalParams, "sceneTools.getShadowCascadeDebug", "scene_tools.get_shadow_cascade_debug", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, get_ssao_enabled, kSceneToolsCameraOptionalParams, "sceneTools.getSsaoEnabled", "scene_tools.get_ssao_enabled", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, get_vision_render_mode, kSceneToolsCameraOptionalParams, "sceneTools.getVisionRenderMode", "scene_tools.get_vision_render_mode", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, get_ssat_view_viewer, kSceneToolsCameraOptionalParams, "sceneTools.getSsatViewViewer", "scene_tools.get_ssat_view_viewer", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, is_vision_available, kNoParams, "sceneTools.isVisionAvailable", "scene_tools.is_vision_available", EditorApiValueType::Object),
    EDITOR_API_METHOD1_WRAPPED(SceneTools, list_actor_tree, "scene.listActorTree", "scene.list_actor_tree", "scene_name", EditorApiValueType::String, EditorApiValueType::Array),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, list_camera_views, kSceneNameParam, "sceneTools.listCameraViews", "scene_tools.list_camera_views", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, list_scene_tree, kSceneNameParam, "sceneTools.listSceneTree", "scene_tools.list_scene_tree", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, load_vision_scene, kPathParam, "sceneTools.loadVisionScene", "scene_tools.load_vision_scene", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, open_actor, kSceneActorParams, "sceneTools.openActor", "scene_tools.open_actor", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, open_camera_view, kSceneCameraParams, "sceneTools.openCameraView", "scene_tools.open_camera_view", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, pick_actor_at_pixel, kSceneToolsPickActorParams, "sceneTools.pickActor", "scene_tools.pick_actor", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, play_audio, kSceneToolsPlayAudioParams, "sceneTools.playAudio", "scene_tools.play_audio", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, reload_scene, kSceneToolsReloadSceneParams, "sceneTools.reloadScene", "scene_tools.reload_scene", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, rebind_actor_resource, kSceneToolsRebindActorResourceParams, "sceneTools.rebindActorResource", "scene_tools.rebind_actor_resource", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, remove_actor, kSceneActorParams, "sceneTools.removeActor", "scene_tools.remove_actor", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, rename_actor, kSceneToolsRenameActorParams, "sceneTools.renameActor", "scene_tools.rename_actor", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, rename_camera_view, kSceneToolsRenameCameraViewParams, "sceneTools.renameCameraView", "scene_tools.rename_camera_view", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, save_screenshot, kSceneToolsSaveScreenshotParams, "sceneTools.saveScreenshot", "scene_tools.save_screenshot", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, select_actor, kSceneToolsSelectActorParams, "sceneTools.selectActor", "scene.select_actor", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, select_screenshot_path, kSceneToolsCameraOptionalParams, "sceneTools.selectScreenshotPath", "scene_tools.select_screenshot_path", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, set_output_mode, kSceneToolsSetOutputModeParams, "sceneTools.setOutputMode", "scene_tools.set_output_mode", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, set_physics_params, kSceneToolsSetPhysicsParams, "sceneTools.setPhysicsParams", "scene_tools.set_physics_params", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, set_render_backend, kSceneToolsSetRenderBackendParams, "sceneTools.setRenderBackend", "scene_tools.set_render_backend", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, set_shadow_cascade_debug, kSceneToolsSetCameraBoolParams, "sceneTools.setShadowCascadeDebug", "scene_tools.set_shadow_cascade_debug", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, set_ssao_enabled, kSceneToolsSetCameraBoolParams, "sceneTools.setSsaoEnabled", "scene_tools.set_ssao_enabled", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, set_vision_render_mode, kSceneToolsSetVisionRenderModeParams, "sceneTools.setVisionRenderMode", "scene_tools.set_vision_render_mode", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, set_ssat_view_viewer, kSceneToolsSetSsatViewViewerParams, "sceneTools.setSsatViewViewer", "scene_tools.set_ssat_view_viewer", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, stop_audio, kResourceIdParam, "sceneTools.stopAudio", "scene_tools.stop_audio", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, sun_direction, kSceneToolsSunDirectionParams, "sceneTools.sunDirection", "scene_tools.sun_direction", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, update_camera_view, kSceneToolsUpdateCameraViewParams, "sceneTools.updateCameraView", "scene_tools.update_camera_view", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(ScratchTool, execute_python_code, kScratchExecutePythonCodeParams, "scratch.executePythonCode", "scratch.execute_python_code", EditorApiValueType::Any),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(ScratchTool, get_game_preview_status, kNoParams, "scratch.getGamePreviewStatus", "scratch.get_game_preview_status", EditorApiValueType::Any),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(ScratchTool, get_script_status, kNoParams, "scratch.getScriptStatus", "scratch.get_script_status", EditorApiValueType::Any),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(ScratchTool, key_event, kScratchKeyEventParams, "scratch.sendKeyEvent", "scratch.send_key_event", EditorApiValueType::Any),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(ScratchTool, key_release, kScratchKeyReleaseParams, "scratch.sendKeyUpEvent", "scratch.send_key_up_event", EditorApiValueType::Any),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(ScratchTool, load_blockly_target, kObjectPayloadParam, "scratch.loadBlocklyTarget", "scratch.load_blockly_target", EditorApiValueType::Any),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(ScratchTool, mouse_event, kScratchMouseEventParams, "scratch.sendMouseEvent", "scratch.send_mouse_event", EditorApiValueType::Any),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(ScratchTool, save_blockly_target, kObjectPayloadParam, "scratch.saveBlocklyTarget", "scratch.save_blockly_target", EditorApiValueType::Any),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(ScratchTool, start_game_preview, kObjectPayloadParam, "scratch.startGamePreview", "scratch.start_game_preview", EditorApiValueType::Any),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(ScratchTool, stop_game_preview, kNoParams, "scratch.stopGamePreview", "scratch.stop_game_preview", EditorApiValueType::Any),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(ScratchTool, stop_script_execution, kScratchStopScriptParams, "scratch.stopScriptExecution", "scratch.stop_script_execution", EditorApiValueType::Any),
}};

#undef EDITOR_API_METHOD_SCHEMA
#undef EDITOR_API_METHOD_SCHEMA_WRAPPED
#undef EDITOR_API_METHOD1
#undef EDITOR_API_METHOD1_WRAPPED
#undef EDITOR_API_METHOD0
#undef EDITOR_API_METHOD0_WRAPPED

constexpr std::array<EditorApiEventSpec, 20> kEditorApiEvents = {{
    {"AI.chunk", EditorApiValueType::String, all_callers(), "events.onAiChunk", "events.on_ai_chunk"},
    {"Editor.logBatch", EditorApiValueType::Array, all_callers(), "events.onLogBatch", "events.on_log_batch"},
    {"LANChat.event", EditorApiValueType::Object, all_callers(), "events.onLanChatEvent", "events.on_lan_chat_event"},
    {"Network.actorDeleteSyncBroadcastRequested", EditorApiValueType::Object, all_callers(), "events.onNetworkActorDeleteSyncBroadcastRequested", "events.on_network_actor_delete_sync_broadcast_requested"},
    {"Network.actorOwnershipClaimed", EditorApiValueType::Object, all_callers(), "events.onNetworkActorOwnershipClaimed", "events.on_network_actor_ownership_claimed"},
    {"Network.actorStateSyncBroadcastRequested", EditorApiValueType::Object, all_callers(), "events.onNetworkActorStateSyncBroadcastRequested", "events.on_network_actor_state_sync_broadcast_requested"},
    {"Network.actorSyncBroadcastRequested", EditorApiValueType::Object, all_callers(), "events.onNetworkActorSyncBroadcastRequested", "events.on_network_actor_sync_broadcast_requested"},
    {"Network.actorTransformSyncBroadcastRequested", EditorApiValueType::Object, all_callers(), "events.onNetworkActorTransformSyncBroadcastRequested", "events.on_network_actor_transform_sync_broadcast_requested"},
    {"Network.assetImportCompleted", EditorApiValueType::Object, all_callers(), "events.onNetworkAssetImportCompleted", "events.on_network_asset_import_completed"},
    {"Network.fileSyncStatusChanged", EditorApiValueType::Object, all_callers(), "events.onNetworkFileSyncStatusChanged", "events.on_network_file_sync_status_changed"},
    {"Network.syncPauseRequested", EditorApiValueType::Object, all_callers(), "events.onNetworkSyncPauseRequested", "events.on_network_sync_pause_requested"},
    {"SceneTools.actorPickResult", EditorApiValueType::Object, all_callers(), "events.onActorPickResult", "events.on_actor_pick_result"},
    {"SceneTools.actorChanged", EditorApiValueType::Object, all_callers(), "events.onActorChanged", "events.on_actor_changed"},
    {"SceneTools.actorSelectionChanged", EditorApiValueType::Object, all_callers(), "events.onActorSelectionChanged", "events.on_actor_selection_changed"},
    {"SceneTools.actorTransformUpdated", EditorApiValueType::Object, all_callers(), "events.onActorTransformUpdated", "events.on_actor_transform_updated"},
    {"SceneTools.focusPoseResult", EditorApiValueType::Object, all_callers(), "events.onFocusPoseResult", "events.on_focus_pose_result"},
    {"SceneTools.sceneAdded", EditorApiValueType::Object, all_callers(), "events.onSceneAdded", "events.on_scene_added"},
    {"SceneTools.sceneRenamed", EditorApiValueType::Object, all_callers(), "events.onSceneRenamed", "events.on_scene_renamed"},
    {"SceneTools.sceneTreeChanged", EditorApiValueType::Object, all_callers(), "events.onSceneTreeChanged", "events.on_scene_tree_changed"},
    {"ProjectLauncher.projectOpened", EditorApiValueType::Object, all_callers(), "events.onProjectOpened", "events.on_project_opened"},
}};

std::atomic<std::uint64_t> g_next_callback_token{1};
std::mutex g_callback_mutex;
std::mutex g_python_script_service_dispatcher_mutex;
PyObject* g_python_script_service_dispatcher = nullptr;

struct CallbackRecord {
    std::uint64_t token = 0;
    std::string event_name;
    nlohmann::json callback_spec = nlohmann::json::object();
    NativeContext context;
    bool python_script = false;
    PyObject* python_callable = nullptr;
};

std::unordered_map<std::uint64_t, CallbackRecord> g_callbacks;

bool caller_allowed(const EditorApiMethodSpec& spec, EditorApiCaller caller) {
    return (spec.allowed_callers & caller_mask(caller)) != 0u;
}

bool event_caller_allowed(const EditorApiEventSpec& spec, EditorApiCaller caller) {
    return (spec.allowed_callers & caller_mask(caller)) != 0u;
}

bool json_matches_type(const nlohmann::json& value, EditorApiValueType type) {
    switch (type) {
        case EditorApiValueType::Any:
            return true;
        case EditorApiValueType::Null:
            return value.is_null();
        case EditorApiValueType::Boolean:
            return value.is_boolean();
        case EditorApiValueType::Integer:
            return value.is_number_integer() || value.is_number_unsigned();
        case EditorApiValueType::Number:
            return value.is_number();
        case EditorApiValueType::String:
            return value.is_string();
        case EditorApiValueType::Object:
            return value.is_object();
        case EditorApiValueType::Array:
            return value.is_array();
    }
    return false;
}

const char* value_type_name(EditorApiValueType type) {
    switch (type) {
        case EditorApiValueType::Any:
            return "any";
        case EditorApiValueType::Null:
            return "null";
        case EditorApiValueType::Boolean:
            return "boolean";
        case EditorApiValueType::Integer:
            return "integer";
        case EditorApiValueType::Number:
            return "number";
        case EditorApiValueType::String:
            return "string";
        case EditorApiValueType::Object:
            return "object";
        case EditorApiValueType::Array:
            return "array";
    }
    return "unknown";
}

NativeResult validate_editor_api_args(const EditorApiMethodSpec& spec, const nlohmann::json& args) {
    const auto normalized_args = args.is_null() ? nlohmann::json::array() : args;
    if (!normalized_args.is_array()) {
        return native_failure(std::string("invalid Editor API arguments for ") + spec.api_name +
                                  ": args must be an array",
                              400,
                              "editor-api");
    }
    if (spec.params == nullptr) {
        // Unspecified Editor API schema: keep existing argument compatibility
        // while modules are migrated to explicit C++ specs one by one.
        return native_success(nlohmann::json::object(), "editor-api");
    }
    if (normalized_args.size() > spec.param_count) {
        return native_failure(std::string("invalid Editor API arguments for ") + spec.api_name +
                                  ": too many arguments",
                              400,
                              "editor-api");
    }
    for (std::size_t index = 0; index < spec.param_count; ++index) {
        const auto& param_spec = spec.params[index];
        if (index >= normalized_args.size()) {
            if (param_spec.optional) {
                continue;
            }
            return native_failure(std::string("invalid Editor API arguments for ") + spec.api_name +
                                      ": missing " + param_spec.name,
                                  400,
                                  "editor-api");
        }
        const auto& value = normalized_args[index];
        if (value.is_null() && param_spec.optional) {
            continue;
        }
        if (!json_matches_type(value, param_spec.type)) {
            return native_failure(std::string("invalid Editor API arguments for ") + spec.api_name +
                                      ": " + param_spec.name + " must be " +
                                      value_type_name(param_spec.type),
                                  400,
                                  "editor-api");
        }
    }
    return native_success(nlohmann::json::object(), "editor-api");
}

NativeResult validate_editor_api_result(const EditorApiMethodSpec& spec, const nlohmann::json& data) {
    if (!json_matches_type(data, spec.return_spec.type)) {
        return native_failure(std::string("invalid Editor API result for ") + spec.api_name +
                                  ": result must be " + value_type_name(spec.return_spec.type),
                              500,
                              "editor-api");
    }
    return native_success(nlohmann::json::object(), "editor-api");
}

bool validate_editor_api_event_payload(const EditorApiEventSpec& spec,
                                       const nlohmann::json& payload) {
    if (json_matches_type(payload, spec.payload_type)) {
        return true;
    }
    CFW_LOG_WARNING("invalid Editor API event payload for {}: expected {}",
                    spec.event_name,
                    value_type_name(spec.payload_type));
    return false;
}

NativeResult invoke_native_api_method(const EditorApiMethodSpec& spec,
                                      const nlohmann::json& args,
                                      const NativeContext& context) {
    NativeRequest native_request;
    native_request.module = spec.native_module;
    native_request.function = spec.native_function;
    native_request.args = args.is_null() ? nlohmann::json::array() : args;

    auto result = NativeApiRegistry::instance().dispatch(native_request, context);
    if (!result) {
        return native_failure(std::string(spec.api_name) + " has no native implementation",
                              500,
                              "editor-api");
    }
    result->route = "editor-api";
    return *result;
}

std::optional<int> browser_identifier(const NativeContext& context) {
    if (!context.browser) {
        return std::nullopt;
    }
    return context.browser->GetIdentifier();
}

void release_python_callback(PyObject* callback) {
    if (!callback) {
        return;
    }
    if (!Py_IsInitialized()) {
        return;
    }
    PyGILState_STATE state = PyGILState_Ensure();
    Py_XDECREF(callback);
    PyGILState_Release(state);
}

std::size_t emit_callbacks(std::string_view event_name,
                           const nlohmann::json& payload,
                           bool python_script) {
    if (python_script) {
        if (!Py_IsInitialized()) {
            return 0;
        }
        PyGILState_STATE state = PyGILState_Ensure();
        std::vector<std::pair<std::uint64_t, PyObject*>> callbacks;
        {
            std::lock_guard<std::mutex> lock(g_callback_mutex);
            for (const auto& [_, record] : g_callbacks) {
                if (record.python_script && record.event_name == event_name && record.python_callable) {
                    Py_XINCREF(record.python_callable);
                    callbacks.emplace_back(record.token, record.python_callable);
                }
            }
        }

        std::size_t emitted = 0;
        const auto payload_text = payload.dump();
        const auto event_text = std::string(event_name);
        for (const auto& [_, callback] : callbacks) {
            PyObject* py_payload = PyUnicode_FromString(payload_text.c_str());
            PyObject* py_event = PyUnicode_FromString(event_text.c_str());
            PyObject* py_args = (py_payload && py_event) ? PyTuple_Pack(2, py_payload, py_event) : nullptr;
            Py_XDECREF(py_payload);
            Py_XDECREF(py_event);

            PyObject* result = py_args ? PyObject_CallObject(callback, py_args) : nullptr;
            Py_XDECREF(py_args);
            Py_XDECREF(callback);
            if (!result) {
                PyErr_Print();
                continue;
            }
            Py_DECREF(result);
            ++emitted;
        }
        PyGILState_Release(state);
        return emitted;
    }

    std::vector<CallbackRecord> records;
    {
        std::lock_guard<std::mutex> lock(g_callback_mutex);
        for (const auto& [_, record] : g_callbacks) {
            if (!record.python_script && record.event_name == event_name) {
                records.push_back(record);
            }
        }
    }

    std::size_t emitted = 0;
    for (const auto& record : records) {
        if (record.context.frame) {
            nlohmann::json event_payload;
            event_payload["event"] = record.event_name;
            event_payload["payload"] = payload;
            event_payload["token"] = record.token;
            const auto script = "window.__coronaEditorApiDispatch && "
                                "window.__coronaEditorApiDispatch(" +
                                event_payload.dump() + ");";
            record.context.frame->ExecuteJavaScript(script, record.context.frame->GetURL(), 0);
            ++emitted;
        }
    }
    return emitted;
}

std::string python_script_request_json(const NativeRequest& request) {
    nlohmann::json payload;
    payload["module"] = request.module;
    payload["function"] = request.function;
    payload["args"] = request.args.is_null() ? nlohmann::json::array() : request.args;
    return payload.dump();
}

std::string python_script_error_message(const nlohmann::json& payload) {
    if (auto it = payload.find("error"); it != payload.end() && it->is_string()) {
        return it->get<std::string>();
    }
    if (auto it = payload.find("message"); it != payload.end() && it->is_string()) {
        return it->get<std::string>();
    }
    return "Python script service returned an error";
}

}  // namespace

EditorApiRegistry& EditorApiRegistry::instance() {
    static EditorApiRegistry registry;
    return registry;
}

const EditorApiMethodSpec* EditorApiRegistry::find(std::string_view api_name) const {
    for (const auto& spec : kEditorApiMethods) {
        if (api_name == spec.api_name) {
            return &spec;
        }
    }
    return nullptr;
}

std::vector<EditorApiMethodSpec> EditorApiRegistry::list_methods() const {
    return {kEditorApiMethods.begin(), kEditorApiMethods.end()};
}

std::vector<EditorApiEventSpec> EditorApiRegistry::list_events() const {
    return {kEditorApiEvents.begin(), kEditorApiEvents.end()};
}

NativeResult EditorApiRegistry::invoke(const EditorApiRequest& request,
                                       const NativeContext& context) const {
    const auto* spec = find(request.api_name);
    if (!spec) {
        return native_failure(request.api_name + " is not defined by C++ Editor API",
                              404,
                              "editor-api");
    }
    if (!caller_allowed(*spec, request.caller)) {
        return native_failure(request.api_name + " is not allowed for this caller",
                              403,
                              "editor-api");
    }
    auto args_validation = validate_editor_api_args(*spec, request.args);
    if (!args_validation.success) {
        return args_validation;
    }
    auto result = invoke_native_api_method(*spec, request.args, context);
    if (!result.success) {
        return result;
    }
    auto result_validation = validate_editor_api_result(*spec, result.data);
    if (!result_validation.success) {
        return result_validation;
    }
    return result;
}

NativeResult CefEditorApiEndpoint::invoke(const std::string& api_name,
                                          const nlohmann::json& args,
                                          const NativeContext& context) {
    return EditorApiRegistry::instance().invoke({api_name, args, EditorApiCaller::Cef}, context);
}

NativeResult PythonScriptApiClientEndpoint::invoke(const std::string& api_name,
                                                   const nlohmann::json& args,
                                                   const NativeContext& context) {
    return EditorApiRegistry::instance().invoke({api_name, args, EditorApiCaller::PythonScript}, context);
}

std::optional<EditorApiRequest> parse_editor_api_request(const nlohmann::json& payload,
                                                         EditorApiCaller caller) {
    if (!payload.is_object()) {
        return std::nullopt;
    }
    const auto api_it = payload.find("api");
    if (api_it == payload.end() || !api_it->is_string()) {
        return std::nullopt;
    }

    EditorApiRequest request;
    request.api_name = api_it->get<std::string>();
    request.caller = caller;
    if (auto args_it = payload.find("args"); args_it != payload.end()) {
        request.args = args_it->is_null() ? nlohmann::json::array() : *args_it;
    }
    return request;
}

EditorApiCallbackRegistry& EditorApiCallbackRegistry::instance() {
    static EditorApiCallbackRegistry registry;
    return registry;
}

std::uint64_t EditorApiCallbackRegistry::register_cef_callback(
    const std::string& event_name,
    const nlohmann::json& callback_spec,
    const NativeContext& context) {
    const auto event_spec = find_editor_api_event(event_name);
    if (!event_spec || !event_caller_allowed(*event_spec, EditorApiCaller::Cef)) {
        return 0;
    }
    const auto token = g_next_callback_token.fetch_add(1);
    CallbackRecord record;
    record.token = token;
    record.event_name = event_name;
    record.callback_spec = callback_spec;
    record.context = context;
    record.python_script = false;
    std::lock_guard<std::mutex> lock(g_callback_mutex);
    g_callbacks[token] = std::move(record);
    return token;
}

std::uint64_t EditorApiCallbackRegistry::register_python_script_callback(
    const std::string& event_name,
    const nlohmann::json& callback_spec,
    const NativeContext& context) {
    const auto event_spec = find_editor_api_event(event_name);
    if (!event_spec || !event_caller_allowed(*event_spec, EditorApiCaller::PythonScript)) {
        return 0;
    }
    const auto token = g_next_callback_token.fetch_add(1);
    CallbackRecord record;
    record.token = token;
    record.event_name = event_name;
    record.callback_spec = callback_spec;
    record.context = context;
    record.python_script = true;
    std::lock_guard<std::mutex> lock(g_callback_mutex);
    g_callbacks[token] = std::move(record);
    return token;
}

std::uint64_t EditorApiCallbackRegistry::register_python_script_callback_callable(
    const std::string& event_name,
    PyObject* callback) {
    const auto event_spec = find_editor_api_event(event_name);
    if (!event_spec || !event_caller_allowed(*event_spec, EditorApiCaller::PythonScript)) {
        return 0;
    }
    if (!Py_IsInitialized() || !PyCallable_Check(callback)) {
        return 0;
    }

    Py_XINCREF(callback);
    const auto token = g_next_callback_token.fetch_add(1);
    CallbackRecord record;
    record.token = token;
    record.event_name = event_name;
    record.callback_spec = {{"transport", "python-script"}};
    record.python_script = true;
    record.python_callable = callback;
    std::lock_guard<std::mutex> lock(g_callback_mutex);
    g_callbacks[token] = std::move(record);
    return token;
}

bool EditorApiCallbackRegistry::unregister(std::uint64_t callback_token) {
    PyObject* python_callable = nullptr;
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(g_callback_mutex);
        const auto it = g_callbacks.find(callback_token);
        if (it == g_callbacks.end()) {
            return false;
        }
        python_callable = it->second.python_callable;
        g_callbacks.erase(it);
        removed = true;
    }
    release_python_callback(python_callable);
    return removed;
}

void EditorApiCallbackRegistry::clear_cef_callbacks_for_browser(int browser_id) {
    std::lock_guard<std::mutex> lock(g_callback_mutex);
    for (auto it = g_callbacks.begin(); it != g_callbacks.end();) {
        const auto record_browser_id = browser_identifier(it->second.context);
        if (!it->second.python_script && record_browser_id && *record_browser_id == browser_id) {
            it = g_callbacks.erase(it);
        } else {
            ++it;
        }
    }
}

void EditorApiCallbackRegistry::clear_python_script_callbacks() {
    std::vector<PyObject*> callbacks;
    {
        std::lock_guard<std::mutex> lock(g_callback_mutex);
        for (auto it = g_callbacks.begin(); it != g_callbacks.end();) {
            if (it->second.python_script) {
                callbacks.push_back(it->second.python_callable);
                it = g_callbacks.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto* callback : callbacks) {
        release_python_callback(callback);
    }
}

std::size_t EditorApiCallbackRegistry::emit_editor_api_event(std::string_view event_name,
                                                             const nlohmann::json& payload) {
    const auto event_spec = find_editor_api_event(event_name);
    if (!event_spec || !event_caller_allowed(*event_spec, EditorApiCaller::Cef) ||
        !validate_editor_api_event_payload(*event_spec, payload)) {
        return 0;
    }
    return emit_callbacks(event_name, payload, false);
}

std::size_t EditorApiCallbackRegistry::emit_python_script_event(std::string_view event_name,
                                                                const nlohmann::json& payload) {
    const auto event_spec = find_editor_api_event(event_name);
    if (!event_spec || !event_caller_allowed(*event_spec, EditorApiCaller::PythonScript) ||
        !validate_editor_api_event_payload(*event_spec, payload)) {
        return 0;
    }
    return emit_callbacks(event_name, payload, true);
}

std::optional<EditorApiEventSpec> find_editor_api_event(std::string_view event_name) {
    for (const auto& event_spec : kEditorApiEvents) {
        if (event_name == event_spec.event_name) {
            return event_spec;
        }
    }
    return std::nullopt;
}

std::size_t emit_editor_api_event(std::string_view event_name, const nlohmann::json& payload) {
    return EditorApiCallbackRegistry::instance().emit_editor_api_event(event_name, payload);
}

std::size_t emit_python_script_event(std::string_view event_name, const nlohmann::json& payload) {
    return EditorApiCallbackRegistry::instance().emit_python_script_event(event_name, payload);
}

void register_python_script_service_dispatcher(PyObject* dispatcher) {
    if (!Py_IsInitialized()) {
        return;
    }

    PyGILState_STATE state = PyGILState_Ensure();
    PyObject* old_dispatcher = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_python_script_service_dispatcher_mutex);
        if (dispatcher && PyCallable_Check(dispatcher)) {
            Py_INCREF(dispatcher);
            old_dispatcher = g_python_script_service_dispatcher;
            g_python_script_service_dispatcher = dispatcher;
        }
    }
    Py_XDECREF(old_dispatcher);
    PyGILState_Release(state);
}

void unregister_python_script_service_dispatcher() {
    clear_python_script_callbacks();
    if (!Py_IsInitialized()) {
        std::lock_guard<std::mutex> lock(g_python_script_service_dispatcher_mutex);
        g_python_script_service_dispatcher = nullptr;
        return;
    }

    PyGILState_STATE state = PyGILState_Ensure();
    PyObject* old_dispatcher = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_python_script_service_dispatcher_mutex);
        old_dispatcher = g_python_script_service_dispatcher;
        g_python_script_service_dispatcher = nullptr;
    }
    Py_XDECREF(old_dispatcher);
    PyGILState_Release(state);
}

bool python_script_service_dispatcher_registered() {
    std::lock_guard<std::mutex> lock(g_python_script_service_dispatcher_mutex);
    return g_python_script_service_dispatcher != nullptr;
}

std::uint64_t register_python_script_callback_callable(const std::string& event_name,
                                                       PyObject* callback) {
    return EditorApiCallbackRegistry::instance().register_python_script_callback_callable(event_name,
                                                                                        callback);
}

void clear_python_script_callbacks() {
    EditorApiCallbackRegistry::instance().clear_python_script_callbacks();
}

NativeResult invoke_python_script_service(const NativeRequest& request, const char* route) {
    const std::string route_name = route && *route ? route : "python-script";
    if (!Py_IsInitialized()) {
        return native_failure("Python script runtime is not initialized",
                              503,
                              route_name);
    }

    PyGILState_STATE state = PyGILState_Ensure();
    PyObject* dispatcher = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_python_script_service_dispatcher_mutex);
        dispatcher = g_python_script_service_dispatcher;
        Py_XINCREF(dispatcher);
    }
    if (!dispatcher) {
        PyGILState_Release(state);
        return native_failure("Python script service dispatcher is not registered",
                              503,
                              route_name);
    }

    PyObject* py_request = PyUnicode_FromString(python_script_request_json(request).c_str());
    PyObject* py_args = py_request ? PyTuple_Pack(1, py_request) : nullptr;
    Py_XDECREF(py_request);

    PyObject* object = py_args ? PyObject_CallObject(dispatcher, py_args) : nullptr;
    Py_DECREF(dispatcher);
    Py_XDECREF(py_args);

    if (!object) {
        PyErr_Print();
        PyGILState_Release(state);
        return native_failure("Python script function call failed",
                              500,
                              route_name);
    }

    PyObject* string_object = PyUnicode_Check(object) ? object : PyObject_Str(object);
    const char* result_chars = string_object ? PyUnicode_AsUTF8(string_object) : nullptr;
    const std::string result_text = result_chars ? result_chars : "";
    if (string_object != object) {
        Py_XDECREF(string_object);
    }
    Py_DECREF(object);
    PyGILState_Release(state);

    const auto parsed = nlohmann::json::parse(result_text, nullptr, false);
    if (parsed.is_discarded()) {
        return native_success(result_text, route_name);
    }
    if (parsed.is_object() && parsed.value("success", true) == false) {
        return native_failure(python_script_error_message(parsed),
                              500,
                              route_name);
    }
    if (parsed.is_object()) {
        if (auto it = parsed.find("data"); it != parsed.end()) {
            return native_success(*it, route_name);
        }
    }
    return native_success(parsed, route_name);
}

}  // namespace Corona::Systems::UI

