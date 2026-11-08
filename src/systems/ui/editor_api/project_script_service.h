#pragma once

#include <filesystem>
#include <string>

namespace Corona::Systems::UI::ProjectScriptService {

struct ValidationError {
    std::string code;
    std::string message;
    explicit operator bool() const { return !code.empty(); }
};

// Script file launches must name a project-relative .py file.
ValidationError validate_script_path(const std::filesystem::path& path);

// Launches are rejected when the project is read-only, the payload omits the
// project path, or the declared project differs from the active root.
ValidationError validate_script_project(bool read_only,
                                        bool has_project_path,
                                        bool project_path_matches_root);

// Defaults the launch target kind: an actor name selects the actor target,
// otherwise the whole project runs.
std::string default_target_type(const std::string& actor_name);

// Maps Python-bridge failure codes onto the frozen Web error codes for the
// Scripts namespace: 504 -> EDITOR_API_TIMEOUT, 409 -> PROJECT_CONTEXT_CHANGED,
// else NATIVE_FAILURE.
std::string bridge_error_code(int error_code);

// ProjectFiles only distinguishes timeouts from general native failures.
std::string project_files_error_code(int error_code);

// Project document / metadata requests must carry the active archive session
// (and, for documents, the active project root).
ValidationError validate_document_identity(const std::string& scene_archive_session,
                                           const std::string& payload_archive_session,
                                           bool project_path_matches);

// Persistence writes reject read-only projects.
ValidationError require_writable_project(bool read_only);

// ProjectSettings bridge mapping: 409 -> PROJECT_CONTEXT_CHANGED,
// 403 -> READ_ONLY_PROJECT, 504 -> EDITOR_API_TIMEOUT, else NATIVE_FAILURE.
std::string project_settings_error_code(int error_code);

}  // namespace Corona::Systems::UI::ProjectScriptService
