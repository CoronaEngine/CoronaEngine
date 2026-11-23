#include "editor_api_services.h"

namespace Corona::Systems::UI::ProjectScriptService {

std::string bridge_error_code(int error_code) {
    if (error_code == 504) return "EDITOR_API_TIMEOUT";
    if (error_code == 409) return "PROJECT_CONTEXT_CHANGED";
    return "NATIVE_FAILURE";
}

std::string project_files_error_code(int error_code) {
    return error_code == 504 ? "EDITOR_API_TIMEOUT" : "NATIVE_FAILURE";
}

ValidationError validate_document_identity(const std::string& scene_archive_session,
                                           const std::string& payload_archive_session,
                                           bool project_path_matches) {
    if (scene_archive_session.empty() || payload_archive_session != scene_archive_session ||
        !project_path_matches) {
        return {"PROJECT_CONTEXT_CHANGED", "PROJECT_CONTEXT_CHANGED"};
    }
    return {};
}

ValidationError require_writable_project(bool read_only) {
    if (read_only) return {"READ_ONLY_PROJECT", "READ_ONLY_PROJECT"};
    return {};
}

std::string project_settings_error_code(int error_code) {
    if (error_code == 409) return "PROJECT_CONTEXT_CHANGED";
    if (error_code == 403) return "READ_ONLY_PROJECT";
    if (error_code == 504) return "EDITOR_API_TIMEOUT";
    return "NATIVE_FAILURE";
}

}  // namespace Corona::Systems::UI::ProjectScriptService
