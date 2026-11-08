#include "project_script_service.h"

namespace Corona::Systems::UI::ProjectScriptService {

ValidationError validate_script_path(const std::filesystem::path& path) {
    if (path.empty() || path.has_root_path() || path.extension() != ".py") {
        return {"INVALID_ARGUMENT", "Script path must be a project-relative .py file"};
    }
    return {};
}

ValidationError validate_script_project(bool read_only,
                                        bool has_project_path,
                                        bool project_path_matches_root) {
    if (read_only || !has_project_path || !project_path_matches_root) {
        return {"PROJECT_CONTEXT_CHANGED", "Script project changed or is read-only"};
    }
    return {};
}

std::string default_target_type(const std::string& actor_name) {
    return actor_name.empty() ? "project" : "actor";
}

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
