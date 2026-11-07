#include <corona/systems/ui/file_script_api.h>
#include "cef_editor_api.h"

#include "editor_dispatch.h"

#include <horizon/core/logging.h>
#include <corona/systems/script/python_runtime_coordinator.h>

#include <include/cef_task.h>
#include <include/base/cef_callback.h>
#include <wrapper/cef_closure_task.h>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
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

constexpr std::uint32_t cef_and_script_runtime_callers() {
    return caller_mask(EditorApiCaller::Cef) | caller_mask(EditorApiCaller::ScriptRuntime);
}

constexpr std::uint32_t animation_callers() {
    return all_callers() | caller_mask(EditorApiCaller::ScriptRuntime);
}

constexpr std::uint32_t non_cef_animation_callers() {
    return caller_mask(EditorApiCaller::PythonScript) | caller_mask(EditorApiCaller::ScriptRuntime);
}

constexpr std::uint32_t non_cef_script_runtime_callers() {
    return caller_mask(EditorApiCaller::ScriptRuntime);
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

constexpr std::array<EditorApiParamSpec, 3> kSceneActorTransformParams = {{
    param("scene_name", EditorApiValueType::String),
    param("actor_name", EditorApiValueType::String),
    param("transform", EditorApiValueType::Object),
}};

constexpr std::array<EditorApiParamSpec, 3> kSceneActorPhysicsParams = {{
    param("scene_name", EditorApiValueType::String),
    param("actor_name", EditorApiValueType::String),
    param("physics", EditorApiValueType::Object),
}};

constexpr std::array<EditorApiParamSpec, 3> kSceneActorStateParams = {{
    param("scene_name", EditorApiValueType::String),
    param("actor_name", EditorApiValueType::String),
    param("state", EditorApiValueType::Object),
}};

constexpr std::array<EditorApiParamSpec, 2> kSceneEnvironmentStateParams = {{
    param("scene_name", EditorApiValueType::String),
    param("state", EditorApiValueType::Object),
}};

constexpr std::array<EditorApiParamSpec, 3> kSceneActorCameraLockParams = {{
    param("scene_name", EditorApiValueType::String),
    param("actor_name", EditorApiValueType::String),
    param("camera_lock", EditorApiValueType::Object),
}};

constexpr std::array<EditorApiParamSpec, 4> kViewportCaptureParams = {{
    param("scene_name", EditorApiValueType::String),
    param("camera_name", EditorApiValueType::String),
    param("camera", EditorApiValueType::Object),
    param("output_path", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 3> kViewportSetCameraPoseParams = {{
    param("scene_name", EditorApiValueType::String),
    param("camera_name", EditorApiValueType::String),
    param("camera", EditorApiValueType::Object),
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

constexpr std::array<EditorApiParamSpec, 2> kMainViewImportResourceFileParams = {{
    param("scene_name", EditorApiValueType::String),
    param("file_type", EditorApiValueType::String, true),
}};

constexpr std::array<EditorApiParamSpec, 3> kFileManagerCreateFolderParams = {{
    param("path", EditorApiValueType::String),
    param("folder_name", EditorApiValueType::String),
    param("scope", EditorApiValueType::Object),
}};

constexpr std::array<EditorApiParamSpec, 4> kFileManagerCreateFileParams = {{
    param("path", EditorApiValueType::String),
    param("file_name", EditorApiValueType::String),
    param("file_type", EditorApiValueType::String),
    param("scope", EditorApiValueType::Object),
}};

constexpr std::array<EditorApiParamSpec, 3> kFileManagerRenameItemParams = {{
    param("old_path", EditorApiValueType::String),
    param("new_name", EditorApiValueType::String),
    param("scope", EditorApiValueType::Object),
}};

constexpr std::array<EditorApiParamSpec, 2> kFileManagerDeleteParams = {{
    param("path", EditorApiValueType::String),
    param("scope", EditorApiValueType::Object),
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

constexpr std::array<EditorApiParamSpec, 3> kNetworkLockObjectParams = {{
    param("object_id", EditorApiValueType::String),
    param("user_id", EditorApiValueType::String),
    param("operation", EditorApiValueType::String, true),
}};

constexpr std::array<EditorApiParamSpec, 2> kNetworkUnlockObjectParams = {{
    param("object_id", EditorApiValueType::String),
    param("user_id", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 1> kNetworkLockOwnerParams = {{
    param("object_id", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 4> kNetworkIntentParams = {{
    param("user_id", EditorApiValueType::String),
    param("tooltip", EditorApiValueType::String),
    param("position", EditorApiValueType::Array),
    param("status", EditorApiValueType::String, true),
}};

constexpr std::array<EditorApiParamSpec, 3> kNetworkPreviewCollisionParams = {{
    param("user_id", EditorApiValueType::String),
    param("position", EditorApiValueType::Array),
    param("delta", EditorApiValueType::Number, true),
}};

constexpr std::array<EditorApiParamSpec, 1> kActorNameParam = {{
    param("actor_name", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 1> kResourceIdParam = {{
    param("resource_id", EditorApiValueType::Any),
}};

constexpr std::array<EditorApiParamSpec, 2> kReadTreeParams = {{
    param("scene_name", EditorApiValueType::String),
    param("query", EditorApiValueType::Object, true),
}};

constexpr std::array<EditorApiParamSpec, 3> kSaveActorParams = {{
    param("scene_name", EditorApiValueType::String),
    param("actor_name", EditorApiValueType::String),
    param("scope", EditorApiValueType::Object, true),
}};

constexpr std::array<EditorApiParamSpec, 2> kSceneActorParams = {{
    param("scene_name", EditorApiValueType::String),
    param("actor_name", EditorApiValueType::String),
}};

constexpr std::array<EditorApiParamSpec, 4> kSceneToolsSetIkChainsParams = {{
    param("scene_name", EditorApiValueType::String),
    param("actor_name", EditorApiValueType::String),
    param("chains", EditorApiValueType::Array),
    param("expected_revision", EditorApiValueType::Number, true),
}};

constexpr std::array<EditorApiParamSpec, 4> kSceneToolsSetAnimationParams = {{
    param("scene_name", EditorApiValueType::String),
    param("actor_name", EditorApiValueType::String),
    param("animation", EditorApiValueType::String),
    param("blend_seconds", EditorApiValueType::Number, true),
}};

constexpr std::array<EditorApiParamSpec, 3> kSceneToolsSelectModelFileParams = {{
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

constexpr std::array<EditorApiParamSpec, 4> kSceneToolsSelectActorParams = {{
    param("scene_name", EditorApiValueType::String),
    param("actor_type", EditorApiValueType::String),
    param("actor_name", EditorApiValueType::String),
    param("context", EditorApiValueType::Object, true),
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

constexpr std::array<EditorApiParamSpec, 3> kSceneToolsSetVisionDenoiserParams = {{
    param("scene_name", EditorApiValueType::String),
    param("camera_name", EditorApiValueType::String, true),
    param("type", EditorApiValueType::String, true),
}};

constexpr std::array<EditorApiParamSpec, 3> kSceneToolsSetReblurSettingsParams = {{
    param("scene_name", EditorApiValueType::String),
    param("camera_name", EditorApiValueType::String),
    param("settings", EditorApiValueType::Object),
}};

constexpr std::array<EditorApiParamSpec, 2> kSceneToolsReloadSceneParams = {{
    param("scene_name", EditorApiValueType::String),
    param("project_path", EditorApiValueType::String, true),
}};

constexpr std::array<EditorApiParamSpec, 4> kSceneToolsRebindActorResourceParams = {{
    param("scene_name", EditorApiValueType::String),
    param("actor_guid", EditorApiValueType::String),
    param("path", EditorApiValueType::String),
    param("scope", EditorApiValueType::Object),
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

constexpr std::array<EditorApiParamSpec, 4> kModelNodeStateParams = {{
    param("scene_name", EditorApiValueType::String),
    param("actor_name", EditorApiValueType::String),
    param("node_index", EditorApiValueType::Integer),
    param("state", EditorApiValueType::Object),
}};
constexpr std::array<EditorApiParamSpec, 5> kModelNodePageParams = {{
    param("scene_name", EditorApiValueType::String),
    param("actor_name", EditorApiValueType::String),
    param("parent_index", EditorApiValueType::Integer, true),
    param("offset", EditorApiValueType::Integer, true),
    param("limit", EditorApiValueType::Integer, true),
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




#define EDITOR_API_METHOD0_WRAPPED(module, function, js_path, python_path, return_type) \
    {#module "." #function, #module, #function, nullptr, 0, returns(return_type), js_path, python_path, true, all_callers()}

#define EDITOR_API_METHOD1_WRAPPED(module, function, js_path, python_path, param0_name, param0_type, return_type) \
    {#module "." #function, #module, #function, kSceneNameParam.data(), kSceneNameParam.size(), returns(return_type), js_path, python_path, true, all_callers()}

#define EDITOR_API_METHOD_SCHEMA_WRAPPED(module, function, params_array, js_path, python_path, return_type) \
    {#module "." #function, #module, #function, params_array.data(), params_array.size(), returns(return_type), js_path, python_path, true, all_callers()}

#define EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(module, function, params_array, js_path, python_path, return_type, callers) \
    {#module "." #function, #module, #function, params_array.data(), params_array.size(), returns(return_type), js_path, python_path, true, callers}

#define EDITOR_API_METHOD0(module, function, return_type) \
    EDITOR_API_METHOD0_WRAPPED(module, function, "", "", return_type)

#define EDITOR_API_METHOD1(module, function, param0_name, param0_type, return_type) \
    EDITOR_API_METHOD1_WRAPPED(module, function, "", "", param0_name, param0_type, return_type)

#define EDITOR_API_METHOD_SCHEMA(module, function, params_array, return_type) \
    EDITOR_API_METHOD_SCHEMA_WRAPPED(module, function, params_array, "", "", return_type)

// Deduce the manifest size from its entries.  Keeping a second hand-maintained
// count here makes MSVC diagnostics point at the whole initializer when the
// manifest grows, instead of identifying the actual entry that drifted.
constexpr auto kEditorApiMethods = std::to_array<EditorApiMethodSpec>({
    EDITOR_API_METHOD_SCHEMA_WRAPPED(CoronaEditor, close_process, kNoParams, "app.closeProcess", "app.close_process", EditorApiValueType::Null),
    EDITOR_API_METHOD_SCHEMA(EditorApi, list_events, kNoParams, EditorApiValueType::Object),
    // ScriptRuntime discovers its allowed methods through this same manifest.
    // Individual calls still enforce each method's caller mask.
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(EditorApi, list_methods, kNoParams, "", "", EditorApiValueType::Object, animation_callers()),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(FileManager, create_file, kFileManagerCreateFileParams, "", "files.create_file", EditorApiValueType::Object, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(FileManager, create_folder, kFileManagerCreateFolderParams, "", "files.create_folder", EditorApiValueType::Boolean, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(FileManager, delete_item, kFileManagerDeleteParams, "", "files.delete_item", EditorApiValueType::Boolean, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(FileManager, get_file_tree, kPathOptionalParam, "", "files.get_file_tree", EditorApiValueType::Object, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(FileManager, get_files, kPathOptionalParam, "", "files.get_files", EditorApiValueType::Array, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(FileManager, get_project_info, kNoParams, "", "files.get_project_info", EditorApiValueType::Object, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(FileManager, open_file, kFileManagerOpenFileParams, "", "files.open_file", EditorApiValueType::Boolean, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(FileManager, rename_item, kFileManagerRenameItemParams, "", "files.rename_item", EditorApiValueType::Boolean, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
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
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, send_agent_reply, kObjectPayloadParam, "lanChat.sendAgentReply", "lan_chat.send_agent_reply", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, send_system_message, kObjectPayloadParam, "lanChat.sendSystemMessage", "lan_chat.send_system_message", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, send_system_message_to_host, kObjectPayloadParam, "lanChat.sendSystemMessageToHost", "lan_chat.send_system_message_to_host", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, send_system_message_to_user, kObjectPayloadParam, "lanChat.sendSystemMessageToUser", "lan_chat.send_system_message_to_user", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, poll_agent_trigger, kNoParams, "lanChat.pollAgentTrigger", "lan_chat.poll_agent_trigger", EditorApiValueType::Any),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, poll_coordinator_sync_message, kNoParams, "lanChat.pollCoordinatorSyncMessage", "lan_chat.poll_coordinator_sync_message", EditorApiValueType::Any),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, poll_room_event, kNoParams, "lanChat.pollRoomEvent", "lan_chat.poll_room_event", EditorApiValueType::Any),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, poll_sync_event, kNoParams, "lanChat.pollSyncEvent", "lan_chat.poll_sync_event", EditorApiValueType::Any),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, start_local_room, kObjectPayloadParam, "lanChat.startLocalRoom", "lan_chat.start_local_room", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, start_room, kObjectPayloadParam, "lanChat.startRoom", "lan_chat.start_room", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, stop_local_room, kNoParams, "lanChat.stopLocalRoom", "lan_chat.stop_local_room", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(LANChat, stop_room, kNoParams, "lanChat.stopRoom", "lan_chat.stop_room", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(MainView, import_resource_file, kMainViewImportResourceFileParams, "", "main.import_resource_file", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(MainView, on_init, kPathOptionalParam, "", "main.on_init", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(MainView, scene_save, kSceneSaveParams, "main.sceneSave", "main.scene_save", EditorApiValueType::Object, non_cef_animation_callers()),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, broadcast_actor_create, kNetworkBroadcastActorCreateParams, "network.broadcastActorCreate", "network.broadcast_actor_create", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, broadcast_actor_delete, kNetworkBroadcastActorDeleteParams, "network.broadcastActorDelete", "network.broadcast_actor_delete", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, broadcast_actor_scene_snapshot, kNetworkBroadcastSceneSnapshotParams, "network.broadcastSceneSnapshot", "network.broadcast_scene_snapshot", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, broadcast_actor_state_update, kNetworkActorStateUpdateParams, "network.broadcastActorStateUpdate", "network.broadcast_actor_state_update", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, broadcast_actor_transform, kNetworkActorStateUpdateParams, "network.broadcastActorTransform", "network.broadcast_actor_transform", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, claim_actor_ownership, kActorGuidParam, "network.claimActorOwnership", "network.claim_actor_ownership", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, connect_to_peer, kNetworkConnectToPeerParams, "network.connectToPeer", "network.connect_to_peer", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, lock_object, kNetworkLockObjectParams, "network.lockObject", "network.lock_object", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, unlock_object, kNetworkUnlockObjectParams, "network.unlockObject", "network.unlock_object", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, get_lock_owner, kNetworkLockOwnerParams, "network.getLockOwner", "network.get_lock_owner", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, broadcast_intent, kNetworkIntentParams, "network.broadcastIntent", "network.broadcast_intent", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, check_preview_collision, kNetworkPreviewCollisionParams, "network.checkPreviewCollision", "network.check_preview_collision", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, get_peer_count, kNoParams, "network.getPeerCount", "network.get_peer_count", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, get_discovered_peers, kNoParams, "network.getDiscoveredPeers", "network.get_discovered_peers", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, clear_discovered_peers, kNoParams, "network.clearDiscoveredPeers", "network.clear_discovered_peers", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(Network, search_lan, kNoParams, "network.searchLan", "network.search_lan", EditorApiValueType::Object),
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
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(ProjectLauncher, browse_folder, kPathOptionalParam, "", "project.browse_folder", EditorApiValueType::String, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(ProjectLauncher, cleanup_portable_scene_assets, kObjectPayloadParam, "", "project.cleanup_portable_scene_assets", EditorApiValueType::Object, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(ProjectLauncher, create_multiplayer_project, kObjectPayloadParam, "", "project.create_multiplayer_project", EditorApiValueType::Object, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(ProjectLauncher, create_project, kObjectPayloadParam, "", "project.create_project", EditorApiValueType::String, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(ProjectLauncher, create_world_project, kObjectPayloadParam, "", "project.create_world_project", EditorApiValueType::Object, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(ProjectLauncher, get_app_version, kNoParams, "", "project.get_app_version", EditorApiValueType::String, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(ProjectLauncher, get_default_project_path, kNoParams, "", "project.get_default_project_path", EditorApiValueType::String, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(ProjectLauncher, get_project_load_status, kNoParams, "", "project.get_project_load_status", EditorApiValueType::Object, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(ProjectLauncher, get_recent_projects, kNoParams, "", "project.get_recent_projects", EditorApiValueType::Array, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(ProjectLauncher, import_portable_asset, kObjectPayloadParam, "", "project.import_portable_asset", EditorApiValueType::Object, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED(ProjectLauncher, open_project, kOpenProjectParams, "project.openProject", "project.open_project", EditorApiValueType::Object),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(ProjectLauncher, open_project_file, kNoParams, "", "project.open_project_file", EditorApiValueType::Object, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(ProjectLauncher, validate_portable_scene, kObjectPayloadParam, "", "project.validate_portable_scene", EditorApiValueType::Object, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(ProjectSettings, read_project_document, kObjectPayloadParam, "", "project_settings.read_project_document", EditorApiValueType::Object, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(ProjectSettings, commit_project_document, kObjectPayloadParam, "", "project_settings.commit_project_document", EditorApiValueType::Object, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(ProjectSettings, get_active_project_info, kNoParams, "", "project_settings.get_active_project_info", EditorApiValueType::Object, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(ProjectSettings, save_active_project_info, kObjectPayloadParam, "", "project_settings.save_active_project_info", EditorApiValueType::Object, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(ResourceSearch, fuzzy_search, kResourceSearchFuzzySearchParams, "", "resource_search.fuzzy_search", EditorApiValueType::Object, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(ResourceSearch, get_stats, kCallerParam, "", "resource_search.get_stats", EditorApiValueType::Object, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(ResourceSearch, image_search, kResourceSearchImageSearchParams, "", "resource_search.image_search", EditorApiValueType::Object, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(ResourceSearch, list_types, kCallerParam, "", "resource_search.list_types", EditorApiValueType::Object, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(ResourceSearch, mark_index_dirty, kResourceSearchMarkIndexDirtyParams, "", "resource_search.mark_index_dirty", EditorApiValueType::Object, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(ResourceSearch, prepare_index, kCallerParam, "", "resource_search.prepare_index", EditorApiValueType::Object, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(ResourceSearch, rebuild_index, kCallerParam, "", "resource_search.rebuild_index", EditorApiValueType::Object, all_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, actor_play_audio, kSceneToolsActorPlayAudioParams, "", "scene_tools.actor_play_audio", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, actor_stop_audio, kActorNameParam, "", "scene_tools.actor_stop_audio", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, close_camera_view, kSceneCameraParams, "", "scene_tools.close_camera_view", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, create_actor, kSceneToolsCreateActorParams, "", "scene_tools.create_actor", EditorApiValueType::Object, non_cef_animation_callers()),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, create_camera_view, kSceneToolsCreateCameraViewParams, "", "scene_tools.create_camera_view", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, delete_camera, kSceneCameraParams, "", "scene_tools.delete_camera", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, floor_grid, kSceneToolsFloorGridParams, "", "scene_tools.floor_grid", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, focus_actor, kSceneToolsFocusActorParams, "", "scene_tools.focus_actor", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, get_output_mode, kSceneToolsCameraOptionalParams, "", "scene_tools.get_output_mode", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, get_physics_params, kSceneNameParam, "", "scene_tools.get_physics_params", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, get_render_backend, kSceneToolsCameraOptionalParams, "", "scene_tools.get_render_backend", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, get_vision_render_mode, kSceneToolsCameraOptionalParams, "", "scene_tools.get_vision_render_mode", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, get_vision_denoise, kSceneToolsCameraOptionalParams, "", "scene_tools.get_vision_denoise", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, get_vision_accumulation, kSceneToolsCameraOptionalParams, "", "scene_tools.get_vision_accumulation", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, get_vision_stable_planes, kSceneToolsCameraOptionalParams, "", "scene_tools.get_vision_stable_planes", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, is_vision_available, kNoParams, "", "scene_tools.is_vision_available", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, read_active_scene, kNoParams, "", "scene.read_active_scene", EditorApiValueType::Object, (animation_callers() & ~caller_mask(EditorApiCaller::Cef)) | caller_mask(EditorApiCaller::Native)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, read_actor, kObjectPayloadParam, "", "scene.read_actor", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, read_transforms, kObjectPayloadParam, "", "scene.read_transforms", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, read_archive_scene, kObjectPayloadParam, "", "scene.read_archive_scene", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, get_scene_snapshot, kSceneNameOptionalParam, "", "scene.get_snapshot", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, get_environment, kSceneNameParam, "", "scene.get_environment", EditorApiValueType::Object, caller_mask(EditorApiCaller::ScriptRuntime)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, set_environment, kSceneEnvironmentStateParams, "", "scene.set_environment", EditorApiValueType::Object, caller_mask(EditorApiCaller::ScriptRuntime)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, set_actor_transform, kSceneActorTransformParams, "", "scene.set_actor_transform", EditorApiValueType::Object, caller_mask(EditorApiCaller::ScriptRuntime)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, set_actor_state, kSceneActorStateParams, "", "scene_tools.set_actor_state", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript) | caller_mask(EditorApiCaller::ScriptRuntime)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, set_actor_physics, kSceneActorPhysicsParams, "", "scene_tools.set_actor_physics", EditorApiValueType::Object, caller_mask(EditorApiCaller::ScriptRuntime)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, set_actor_camera_lock, kSceneActorCameraLockParams, "", "scene_tools.set_actor_camera_lock", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, save_actor, kSaveActorParams, "sceneTools.saveActor", "scene_tools.save_actor", EditorApiValueType::Object, non_cef_script_runtime_callers()),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, list_actor_tree, kReadTreeParams, "scene.listActorTree", "scene.list_actor_tree", EditorApiValueType::Array, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, list_camera_views, kSceneNameParam, "", "scene_tools.list_camera_views", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, list_scene_tree, kReadTreeParams, "", "scene_tools.list_scene_tree", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript) | caller_mask(EditorApiCaller::ScriptRuntime)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, open_camera_view, kSceneCameraParams, "", "scene_tools.open_camera_view", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, pick_actor_at_pixel, kSceneToolsPickActorParams, "", "scene_tools.pick_actor", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, play_audio, kSceneToolsPlayAudioParams, "", "scene_tools.play_audio", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, reload_scene, kSceneToolsReloadSceneParams, "", "scene_tools.reload_scene", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, rebind_actor_resource, kSceneToolsRebindActorResourceParams, "", "scene_tools.rebind_actor_resource", EditorApiValueType::Object, cef_and_script_runtime_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, remove_actor, kSceneActorParams, "", "scene_tools.remove_actor", EditorApiValueType::Object, cef_and_script_runtime_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, rename_actor, kSceneToolsRenameActorParams, "", "scene_tools.rename_actor", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, rename_camera_view, kSceneToolsRenameCameraViewParams, "", "scene_tools.rename_camera_view", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, save_screenshot, kSceneToolsSaveScreenshotParams, "", "scene_tools.save_screenshot", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, select_actor, kSceneToolsSelectActorParams, "", "scene.select_actor", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, select_screenshot_path, kSceneToolsCameraOptionalParams, "", "scene_tools.select_screenshot_path", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, set_output_mode, kSceneToolsSetOutputModeParams, "", "scene_tools.set_output_mode", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, set_physics_params, kSceneToolsSetPhysicsParams, "", "scene_tools.set_physics_params", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, set_render_backend, kSceneToolsSetRenderBackendParams, "", "scene_tools.set_render_backend", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, set_vision_render_mode, kSceneToolsSetVisionRenderModeParams, "", "scene_tools.set_vision_render_mode", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, set_vision_denoise, kSceneToolsSetCameraBoolParams, "", "scene_tools.set_vision_denoise", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, set_vision_denoiser, kSceneToolsSetVisionDenoiserParams, "", "scene_tools.set_vision_denoiser", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, set_reblur_settings, kSceneToolsSetReblurSettingsParams, "", "scene_tools.set_reblur_settings", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, set_pt_settings, kSceneToolsSetReblurSettingsParams, "", "scene_tools.set_pt_settings", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, set_restir_settings, kSceneToolsSetReblurSettingsParams, "", "scene_tools.set_restir_settings", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, set_relax_settings, kSceneToolsSetReblurSettingsParams, "", "scene_tools.set_relax_settings", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, get_pt_settings, kSceneCameraParams, "", "scene_tools.get_pt_settings", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, get_restir_settings, kSceneCameraParams, "", "scene_tools.get_restir_settings", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, get_reblur_settings, kSceneCameraParams, "", "scene_tools.get_reblur_settings", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, get_relax_settings, kSceneCameraParams, "", "scene_tools.get_relax_settings", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, set_vision_accumulation, kSceneToolsSetCameraBoolParams, "", "scene_tools.set_vision_accumulation", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, set_vision_stable_planes, kSceneToolsSetCameraBoolParams, "", "scene_tools.set_vision_stable_planes", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, stop_audio, kResourceIdParam, "", "scene_tools.stop_audio", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, sun_direction, kSceneToolsSunDirectionParams, "", "scene_tools.sun_direction", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, get_actor_skeleton_leaves, kSceneActorParams, "", "scene_tools.get_actor_skeleton_leaves", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, set_actor_animation, kSceneToolsSetAnimationParams, "", "scene_tools.set_actor_animation", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, pause_actor_animation, kSceneActorParams, "", "scene_tools.pause_actor_animation", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, resume_actor_animation, kSceneActorParams, "", "scene_tools.resume_actor_animation", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, get_model_nodes, kModelNodePageParams, "sceneTools.getModelNodes", "scene_tools.get_model_nodes", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, set_model_node_state, kModelNodeStateParams, "sceneTools.setModelNodeState", "scene_tools.set_model_node_state", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, get_actor_ik_chains, kSceneActorParams, "sceneTools.getActorIkChains", "scene_tools.get_actor_ik_chains", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, set_actor_ik_chains, kSceneToolsSetIkChainsParams, "sceneTools.setActorIkChains", "scene_tools.set_actor_ik_chains", EditorApiValueType::Object, animation_callers() & ~caller_mask(EditorApiCaller::Cef)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, update_camera_view, kSceneToolsUpdateCameraViewParams, "", "scene_tools.update_camera_view", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, capture_viewport, kViewportCaptureParams, "", "viewport.capture", EditorApiValueType::Object, caller_mask(EditorApiCaller::PythonScript)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, set_camera_pose, kViewportSetCameraPoseParams, "", "viewport.set_camera_pose", EditorApiValueType::Object, caller_mask(EditorApiCaller::ScriptRuntime)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(Scripts, run_path, kObjectPayloadParam, "", "", EditorApiValueType::Object, caller_mask(EditorApiCaller::Native)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(Scripts, run_file, kObjectPayloadParam, "", "", EditorApiValueType::Object, caller_mask(EditorApiCaller::Native)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(Scripts, get_run, kObjectPayloadParam, "", "", EditorApiValueType::Object, caller_mask(EditorApiCaller::Native)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(Scripts, stop, kObjectPayloadParam, "", "", EditorApiValueType::Object, caller_mask(EditorApiCaller::Native)),
    EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(Scripts, get_status, kNoParams, "", "", EditorApiValueType::Object, caller_mask(EditorApiCaller::Native)),
});

#undef EDITOR_API_METHOD_SCHEMA
#undef EDITOR_API_METHOD_SCHEMA_WRAPPED
#undef EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS
#undef EDITOR_API_METHOD1
#undef EDITOR_API_METHOD1_WRAPPED
#undef EDITOR_API_METHOD0
#undef EDITOR_API_METHOD0_WRAPPED

constexpr auto kEditorApiEvents = std::to_array<EditorApiEventSpec>({
    {"LANChat.event", EditorApiValueType::Object, all_callers(), "events.onLanChatEvent", "events.on_lan_chat_event"},
    {"Network.actorDeleteSyncBroadcastRequested", EditorApiValueType::Object, all_callers(), "events.onNetworkActorDeleteSyncBroadcastRequested", "events.on_network_actor_delete_sync_broadcast_requested"},
    {"Network.actorOwnershipClaimed", EditorApiValueType::Object, all_callers(), "events.onNetworkActorOwnershipClaimed", "events.on_network_actor_ownership_claimed"},
    {"Network.actorStateSyncBroadcastRequested", EditorApiValueType::Object, all_callers(), "events.onNetworkActorStateSyncBroadcastRequested", "events.on_network_actor_state_sync_broadcast_requested"},
    {"Network.actorSyncBroadcastRequested", EditorApiValueType::Object, all_callers(), "events.onNetworkActorSyncBroadcastRequested", "events.on_network_actor_sync_broadcast_requested"},
    {"Network.actorTransformSyncBroadcastRequested", EditorApiValueType::Object, all_callers(), "events.onNetworkActorTransformSyncBroadcastRequested", "events.on_network_actor_transform_sync_broadcast_requested"},
    {"Network.assetImportCompleted", EditorApiValueType::Object, all_callers(), "events.onNetworkAssetImportCompleted", "events.on_network_asset_import_completed"},
    {"Network.fileSyncStatusChanged", EditorApiValueType::Object, all_callers(), "events.onNetworkFileSyncStatusChanged", "events.on_network_file_sync_status_changed"},
    {"Network.syncPauseRequested", EditorApiValueType::Object, all_callers(), "events.onNetworkSyncPauseRequested", "events.on_network_sync_pause_requested"},
});

static_assert([] {
    for (const auto& event : kEditorApiEvents) {
        if (!event.event_name || !event.event_name[0] || !event.allowed_callers ||
            !event.js_wrapper || !event.js_wrapper[0] ||
            !event.python_wrapper || !event.python_wrapper[0]) return false;
    }
    return true;
}(), "Editor API events must not contain empty entries");

std::atomic<std::uint64_t> g_next_callback_token{1};
std::mutex g_callback_mutex;
std::atomic<bool> g_python_script_service_dispatcher_registered{false};

struct CallbackRecord {
    std::uint64_t token = 0;
    std::string event_name;
    nlohmann::json callback_spec = nlohmann::json::object();
    NativeContext context;
    bool python_script = false;
    PyObject* python_callable = nullptr;
    CefRefPtr<CefV8Context> v8_context;
    CefRefPtr<CefV8Value> v8_callback;
    std::optional<int> v8_browser_id;
};

std::unordered_map<std::uint64_t, CallbackRecord> g_callbacks;

CefRefPtr<CefV8Value> event_value(const nlohmann::json& value) {
    if (value.is_null()) return CefV8Value::CreateNull();
    if (value.is_boolean()) return CefV8Value::CreateBool(value.get<bool>());
    if (value.is_number_integer()) return CefV8Value::CreateDouble(value.get<double>());
    if (value.is_number_unsigned()) return CefV8Value::CreateDouble(value.get<double>());
    if (value.is_number_float()) return CefV8Value::CreateDouble(value.get<double>());
    if (value.is_string()) return CefV8Value::CreateString(value.get<std::string>());
    if (value.is_array()) {
        auto array = CefV8Value::CreateArray(static_cast<int>(value.size()));
        for (int index = 0; index < static_cast<int>(value.size()); ++index)
            array->SetValue(index, event_value(value[index]));
        return array;
    }
    auto object = CefV8Value::CreateObject(nullptr, nullptr);
    for (auto it = value.begin(); it != value.end(); ++it)
        object->SetValue(it.key(), event_value(it.value()), V8_PROPERTY_ATTRIBUTE_NONE);
    return object;
}

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

namespace {

/// 把执行投递到 CEF UI 线程（编辑器状态的唯一 owner）。
class EditorOwnerTask final : public CefTask {
   public:
    explicit EditorOwnerTask(std::function<void()> fn) : fn_(std::move(fn)) {}
    void Execute() override {
        if (fn_) fn_();
    }

   private:
    std::function<void()> fn_;
    IMPLEMENT_REFCOUNTING(EditorOwnerTask);
};

/// 安装 CEF 实现的派发钩子（只装一次）。装不上时所有跨线程调用都会落到
/// `EditorOwnerRun::PostFailed`，而不是悄悄跑在错误的线程上。
void ensure_editor_dispatch_hooks() {
    static std::once_flag once;
    std::call_once(once, [] {
        Detail::EditorDispatchHooks hooks;
        hooks.is_owner_thread = [] { return CefCurrentlyOn(TID_UI); };
        hooks.post_to_owner = [](std::function<void()> task) {
            auto cef_task = CefRefPtr<CefTask>(new EditorOwnerTask(std::move(task)));
            return CefPostTask(TID_UI, cef_task);
        };
        Detail::set_editor_dispatch_hooks(std::move(hooks));
    });
}

/// 派发的等待上限。超出后返回"编辑器忙"，绝不无限等待：调用方（Python/脚本）可以
/// 重试，而 owner 线程不会因为某个卡住的任务被永久堵死。
constexpr std::chrono::milliseconds kEditorDispatchTimeout{5000};

/// 真正执行 handler（必须在 owner 线程上，或在 `editor_state_mutex` 保护下）。
NativeResult dispatch_native_api_method(const EditorApiMethodSpec& spec,
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

/// 按 route 把整个 handler 交给 owner 线程（或按交接协议就地）执行。
///
/// 参数与返回值都走 `call_editor_dispatch`：它保证"返回空 ⇒ handler 绝不会再被执行"，
/// 因此按值捕获之后不会出现"超时返回了、任务稍后又跑到已退栈的栈帧上"。
NativeResult run_native_api_method(const EditorApiMethodSpec& spec,
                                   const nlohmann::json& args,
                                   const NativeContext& context,
                                   Detail::EditorDispatchRoute route) {
    ensure_editor_dispatch_hooks();

    Detail::EditorDispatchReport report;
    auto result = Detail::call_editor_dispatch(
        [spec, args, context] { return dispatch_native_api_method(spec, args, context); },
        route,
        kEditorDispatchTimeout,
        &report);

    if (report.ran_inline_on_caller && route == Detail::EditorDispatchRoute::InlineBusy) {
        // owner 线程正阻塞等 Python：此刻再从 Python 线程等 owner 线程就是互等，所以
        // 就地执行。仍然串行（持 editor_state_mutex），记一条 WARN 便于观察频率。
        static std::atomic<std::uint64_t> busy_dispatch_count{0};
        const auto count = busy_dispatch_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1 || count % 100 == 0) {
            CFW_LOG_WARNING(
                "Editor API '{}' ran on the calling thread (#{}) because the editor thread "
                "is waiting on the Python runtime; the call is still serialized",
                spec.api_name, count);
        }
    }

    if (result) {
        return std::move(*result);
    }

    const bool timed_out = report.run == Detail::EditorOwnerRun::Timeout;
    CFW_LOG_WARNING("Editor API '{}' could not run on the editor thread ({} after {}ms)",
                    spec.api_name,
                    timed_out ? "timeout" : "post failed",
                    static_cast<long long>(kEditorDispatchTimeout.count()));
    return native_failure(std::string(spec.api_name) +
                              (timed_out ? " timed out waiting for the editor thread"
                                         : " could not be posted to the editor thread"),
                          timed_out ? 504 : 503,
                          "editor-api");
}

}  // namespace

NativeResult invoke_native_api_method(const EditorApiMethodSpec& spec,
                                      const nlohmann::json& args,
                                      const NativeContext& context) {
    ensure_editor_dispatch_hooks();
    // 编辑器场景状态只有一个 owner 线程：绘制并派发输入的那个 CEF UI 线程。handler
    // 因此可以安全地对状态做"单线程"假设（缓存、表、unique_ptr 成员），前提是它们
    // 全部在那个线程执行——这正是这里把每一次 handler 调用都收口到 owner 线程的原因。
    // 注意顺序：`on_editor_owner_thread()` 会记录"见过 owner 线程"，所以必须先于
    // `editor_owner_thread_seen()` 求值（函数实参的求值顺序是不指定的）。
    const bool on_owner_thread = Detail::on_editor_owner_thread();
    const auto route = Detail::decide_editor_dispatch_route(
        on_owner_thread,
        Detail::editor_thread_waiting_for_python(),
        Detail::editor_owner_thread_seen());
    return run_native_api_method(spec, args, context, route);
}

std::optional<int> browser_identifier(const NativeContext& context) {
    if (!context.browser) {
        return std::nullopt;
    }
    return context.browser->GetIdentifier();
}

Script::Python::PythonRuntimeResponse execute_editor_python_callback(
    const Script::Python::PythonRuntimeRequest& request) {
    using Script::Python::PythonRuntimeResponse;
    if (!PyGILState_Check()) {
        return PythonRuntimeResponse::failure("editor callback executed without the GIL");
    }

    PyObject* callback = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_callback_mutex);
        const auto it = g_callbacks.find(request.callback_token);
        if (it == g_callbacks.end() || !it->second.python_script || !it->second.python_callable) {
            return request.function == "release"
                       ? PythonRuntimeResponse::success()
                       : PythonRuntimeResponse::failure("editor callback token is no longer registered");
        }
        callback = it->second.python_callable;
        if (request.function == "release") {
            g_callbacks.erase(it);
        } else {
            Py_INCREF(callback);
        }
    }

    if (request.function == "release") {
        Py_DECREF(callback);
        return PythonRuntimeResponse::success();
    }

    const auto envelope = nlohmann::json::parse(request.payload_json, nullptr, false);
    if (envelope.is_discarded() || !envelope.is_object()) {
        Py_DECREF(callback);
        return PythonRuntimeResponse::failure("editor callback payload is invalid");
    }
    const auto payload_text = envelope.value("payload", nlohmann::json::object()).dump();
    const auto event_text = envelope.value("event", std::string{});
    if (request.cancelled()) {
        Py_DECREF(callback);
        return PythonRuntimeResponse::timeout();
    }
    PyObject* py_payload = PyUnicode_FromString(payload_text.c_str());
    PyObject* py_event = PyUnicode_FromString(event_text.c_str());
    PyObject* py_args = (py_payload && py_event) ? PyTuple_Pack(2, py_payload, py_event) : nullptr;
    Py_XDECREF(py_payload);
    Py_XDECREF(py_event);
    PyObject* result = request.cancelled() || !py_args
                           ? nullptr
                           : PyObject_CallObject(callback, py_args);
    Py_XDECREF(py_args);
    Py_DECREF(callback);
    if (request.cancelled()) {
        Py_XDECREF(result);
        return PythonRuntimeResponse::timeout();
    }
    if (!result) {
        PyErr_Print();
        return PythonRuntimeResponse::failure("editor callback raised an exception");
    }
    Py_DECREF(result);
    return PythonRuntimeResponse::success();
}

bool enqueue_editor_python_callback(std::uint64_t callback_token,
                                    std::string_view event_name,
                                    const nlohmann::json& payload,
                                    std::string function = "invoke") {
    auto* coordinator = Script::Python::active_python_runtime_coordinator();
    if (!coordinator) return false;
    Script::Python::PythonRuntimeRequest request;
    request.kind = Script::Python::PythonRuntimeRequestKind::Callback;
    request.source = "EditorApi";
    request.function = std::move(function);
    request.callback_token = callback_token;
    request.payload_json = nlohmann::json{{"event", event_name}, {"payload", payload}}.dump();
    request.handler = &execute_editor_python_callback;
    return coordinator->submit(std::move(request)).accepted;
}

void emit_v8_callback(CallbackRecord record, nlohmann::json payload) {
    if (!record.v8_context->IsValid() || !record.v8_context->Enter()) return;
    CefV8ValueList arguments;
    arguments.push_back(event_value(payload));
    arguments.push_back(CefV8Value::CreateString(record.event_name));
    record.v8_callback->ExecuteFunction(nullptr, arguments);
    record.v8_context->Exit();
}

std::size_t emit_callbacks(std::string_view event_name,
                           const nlohmann::json& payload,
                           bool python_script) {
    if (python_script) {
        std::vector<std::uint64_t> callback_tokens;
        {
            std::lock_guard<std::mutex> lock(g_callback_mutex);
            for (const auto& [_, record] : g_callbacks) {
                if (record.python_script && record.event_name == event_name && record.python_callable) {
                    callback_tokens.push_back(record.token);
                }
            }
        }

        std::size_t emitted = 0;
        for (const auto callback_token : callback_tokens) {
            if (enqueue_editor_python_callback(callback_token, event_name, payload)) ++emitted;
        }
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
        if (record.v8_context && record.v8_callback) {
            record.v8_context->GetTaskRunner()->PostTask(
                CefCreateClosureTask(base::BindOnce(&emit_v8_callback, record, payload)));
            ++emitted;
        }
    }
    return emitted;
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
    auto caller_context = context;
    caller_context.script_runtime = request.caller == EditorApiCaller::ScriptRuntime;
    auto result = invoke_native_api_method(*spec, request.args, caller_context);
    if (!result.success) {
        return result;
    }
    auto result_validation = validate_editor_api_result(*spec, result.data);
    if (!result_validation.success) {
        return result_validation;
    }
    return result;
}

nlohmann::json invoke_file_script_api(const std::string& api, const nlohmann::json& args) {
    const auto result = EditorApiRegistry::instance().invoke({api, args, EditorApiCaller::Native}, {});
    return {{"success", result.success}, {"code", result.success ? 0 : result.error_code},
            {"data", result.data}, {"error", result.error}};
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

NativeResult ScriptRuntimeApiClientEndpoint::invoke(const std::string& api_name,
                                                    const nlohmann::json& args,
                                                    const NativeContext& context) {
    return EditorApiRegistry::instance().invoke({api_name, args, EditorApiCaller::ScriptRuntime}, context);
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

std::uint64_t EditorApiCallbackRegistry::register_cef_v8_callback(
    const std::string& event_name,
    CefRefPtr<CefV8Context> context,
    CefRefPtr<CefV8Value> callback) {
    const auto event_spec = find_editor_api_event(event_name);
    if (!event_spec || !event_caller_allowed(*event_spec, EditorApiCaller::Cef) ||
        !context || !context->IsValid() || !callback || !callback->IsFunction()) {
        return 0;
    }
    const auto token = g_next_callback_token.fetch_add(1);
    CallbackRecord record;
    record.token = token;
    record.event_name = event_name;
    record.v8_context = context;
    record.v8_callback = callback;
    if (const auto browser = context->GetBrowser()) {
        record.v8_browser_id = browser->GetIdentifier();
    }
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
    if (!PyGILState_Check() || !PyCallable_Check(callback)) {
        return 0;
    }

    Py_INCREF(callback);
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
    {
        std::lock_guard<std::mutex> lock(g_callback_mutex);
        const auto it = g_callbacks.find(callback_token);
        if (it == g_callbacks.end()) {
            return false;
        }
        if (it->second.python_script && it->second.python_callable) {
            return enqueue_editor_python_callback(callback_token,
                                                  it->second.event_name,
                                                  nlohmann::json::object(),
                                                  "release");
        }
        g_callbacks.erase(it);
    }
    return true;
}

void EditorApiCallbackRegistry::clear_cef_callbacks_for_browser(int browser_id) {
    std::lock_guard<std::mutex> lock(g_callback_mutex);
    for (auto it = g_callbacks.begin(); it != g_callbacks.end();) {
        const auto record_browser_id = it->second.python_script
            ? browser_identifier(it->second.context)
            : it->second.v8_browser_id;
        if (!it->second.python_script && record_browser_id && *record_browser_id == browser_id) {
            it = g_callbacks.erase(it);
        } else {
            ++it;
        }
    }
}

void EditorApiCallbackRegistry::clear_python_script_callbacks() {
    std::vector<std::pair<std::uint64_t, std::string>> callbacks;
    {
        std::lock_guard<std::mutex> lock(g_callback_mutex);
        for (const auto& [token, record] : g_callbacks) {
            if (record.python_script && record.python_callable) {
                callbacks.emplace_back(token, record.event_name);
            }
        }
    }
    if (PyGILState_Check()) {
        for (const auto& [token, _] : callbacks) {
            Script::Python::PythonRuntimeRequest request;
            request.function = "release";
            request.callback_token = token;
            (void)execute_editor_python_callback(request);
        }
        return;
    }
    for (const auto& [token, event_name] : callbacks) {
        enqueue_editor_python_callback(token, event_name, nlohmann::json::object(), "release");
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
    g_python_script_service_dispatcher_registered.store(
        PyGILState_Check() && dispatcher && PyCallable_Check(dispatcher),
        std::memory_order_release);
}

void unregister_python_script_service_dispatcher() {
    clear_python_script_callbacks();
    g_python_script_service_dispatcher_registered.store(false, std::memory_order_release);
}

bool python_script_service_dispatcher_registered() {
    return g_python_script_service_dispatcher_registered.load(std::memory_order_acquire);
}

std::uint64_t register_python_script_callback_callable(const std::string& event_name,
                                                       PyObject* callback) {
    return EditorApiCallbackRegistry::instance().register_python_script_callback_callable(event_name,
                                                                                        callback);
}

void clear_python_script_callbacks() {
    EditorApiCallbackRegistry::instance().clear_python_script_callbacks();
}

NativeResult invoke_python_script_service(const NativeRequest& request, const char* route,
                                         std::chrono::milliseconds timeout) {
    const std::string route_name = route && *route ? route : "python-script";
    auto* coordinator = Script::Python::active_python_runtime_coordinator();
    if (!coordinator) {
        return native_failure("Python script runtime is not initialized",
                              503,
                              route_name);
    }

    Script::Python::PythonRuntimeRequest runtime_request;
    runtime_request.kind = Script::Python::PythonRuntimeRequestKind::ServiceCall;
    runtime_request.source = route_name;
    runtime_request.module = request.module;
    runtime_request.function = request.function;
    runtime_request.service_args = std::make_shared<const nlohmann::json>(
        request.args.is_null() ? nlohmann::json::array() : request.args);
    // 从这里到 submit_and_wait 返回为止，owner 线程不在碰编辑器状态：于是 Python 消费
    // 线程此刻若回调编辑器 API，可以就地执行（见 editor_dispatch.h 的三段交接协议）。
    Detail::EditorWaitingForPythonScope waiting_for_python;
    const auto response = coordinator->submit_and_wait(std::move(runtime_request),
                                                       timeout);
    if (response.status != Script::Python::PythonRuntimeResponseStatus::Success) {
        int error_code = 500;
        if (response.status == Script::Python::PythonRuntimeResponseStatus::Timeout) {
            error_code = 504;
        } else if (response.status == Script::Python::PythonRuntimeResponseStatus::QueueFull) {
            error_code = 429;
        } else if (response.status == Script::Python::PythonRuntimeResponseStatus::RuntimeStopping) {
            error_code = 503;
        }
        return native_failure(response.error.empty() ? "Python script function call failed" : response.error,
                              error_code,
                              route_name);
    }

    if (!response.service_result) {
        return native_failure("Python script service returned no structured result", 500, route_name);
    }
    const auto& parsed = *response.service_result;
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
