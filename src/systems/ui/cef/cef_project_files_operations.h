#pragma once

#include <array>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace Corona::Systems::UI {

enum class ProjectFilesMethod { Save, Load };
struct ProjectFilesRequest {
    ProjectFilesMethod method;
    std::string archive_session, project_path, code;
    // script kind, target type, scene, actor. The storage owner derives its key.
    std::array<std::string, 4> identity;
    // Opaque Blockly document values, never serialized text or V8 handles.
    nlohmann::json document;
};
struct ProjectFilesReply {
    std::string error_code, error_message;
    std::string status, code, message, revision, code_path, document_path;
    std::optional<nlohmann::json> document;
};

ProjectFilesReply execute_project_files_operation(const ProjectFilesRequest& request);

} // namespace Corona::Systems::UI
