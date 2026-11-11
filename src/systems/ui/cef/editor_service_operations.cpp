// The editor services that do not edit the scene: project settings and the Web -> Python
// bridge. Both talk to the Python host and read the scene snapshot; neither runs any of
// the scene-edit helpers, which is what lets them live outside the handler unit.

#include "cef_operations.h"
#include "editor_api_services.h"
#include "editor_paths.h"
#include "native_api_types.h"
#include "native_editor_state.h"
#include "python_service_bridge.h"

#include <chrono>
#include <string>
#include <utility>

namespace Corona::Systems::UI {

namespace {

NativeResult save_project_info(const nlohmann::json& settings) {
    auto* scene = native_editor_state().scene.get();
    if (!scene) return native_failure("PROJECT_CONTEXT_CHANGED", 409);
    if (const auto invalid = ProjectScriptService::validate_document_identity(
            scene->archive_session, settings.value("archive_session", std::string{}), true)) {
        return native_failure(invalid.message, 409);
    }
    if (const auto invalid = ProjectScriptService::require_writable_project(scene->read_only)) {
        return native_failure(invalid.message, 403);
    }
    // Same call the handler unit wraps as project_archive_call(); inlined here because
    // the wrapper has another caller there and duplicating the symbol is worse.
    auto result = invoke_python_script_service(
        {"ProjectArchive", "update_metadata",
         nlohmann::json::array({{{"path", path_to_utf8(scene->project_root)}, {"settings", settings}}})},
        "python-archive", std::chrono::seconds(300));
    if (!result.success || !result.data.contains("name")) return native_failure("Project settings were not saved: " + result.error, 2);
    scene->name = result.data.at("name");
    scene->core_version = result.data.at("core_version");
    result.data["archive_session"] = scene->archive_session;
    return result;
}

}  // namespace

ProjectSettingsReply execute_project_settings_operation(const ProjectSettingsRequest& request) {
    NativeResult result;
    ProjectSettingsReply reply;
    const auto* scene = native_editor_state().scene.get();
    if (!scene || absolute_normalized_path(path_from_utf8(request.project_path)) != scene->project_root)
        result = native_failure("PROJECT_CONTEXT_CHANGED", 409);
    else result = save_project_info({{"archive_session", request.archive_session},
        {"name", request.name}, {"core_version", request.core_version}});
    if (!result.success) {
        reply.error_code = ProjectScriptService::project_settings_error_code(result.error_code);
        reply.error_message = result.error;
        return reply;
    }
    // update_metadata answered with the project's metadata; forward it rather than copying
    // every field into a struct and back.
    reply.value = result.data;
    return reply;
}

BridgeReply execute_bridge_operation(const std::string& envelope) {
    BridgeReply reply;
    if (envelope.empty()) {
        reply.error_code = "INVALID_ARGUMENT";
        reply.error_message = "Bridge envelope is empty";
        return reply;
    }
    const auto result = invoke_python_script_service(
        {"EditorBridge", "call", nlohmann::json::array({envelope})},
        "python-bridge",
        std::chrono::seconds(300));
    if (!result.success) {
        reply.error_code = result.error_code == 504 ? "EDITOR_API_TIMEOUT"
                           : result.error_code == 429 ? "SERVICE_BUSY"
                           : result.error_code == 503 ? "SERVICE_UNAVAILABLE"
                                                      : "SERVICE_FAILED";
        reply.error_message = result.error;
        return reply;
    }
    reply.json = result.data.is_string() ? result.data.get<std::string>() : result.data.dump();
    return reply;
}

}
  // namespace Corona::Systems::UI
