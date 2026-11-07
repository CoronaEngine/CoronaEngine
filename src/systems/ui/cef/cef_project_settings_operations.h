#pragma once

#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace Corona::Systems::UI {

enum class ProjectSettingsMethod { ReadDocument, CommitDocument, GetInfo, SaveInfo };
struct ProjectSettingsRequest {
    ProjectSettingsMethod method;
    std::string archive_session, project_path, document, revision, name, core_version;
    nlohmann::json content;
};
struct ProjectInfoSnapshot {
    std::string name, mode, prompt, core_version, create_time, last_opened;
    std::string project_path, archive_session, source_path;
    bool read_only = false;
};
struct ProjectSettingsReply {
    std::string error_code, error_message;
    std::optional<ProjectInfoSnapshot> info;
    bool success = false, corrupt = false;
    std::string document_error, revision;
    std::optional<nlohmann::json> content;
};

ProjectSettingsReply execute_project_settings_operation(const ProjectSettingsRequest& request);

} // namespace Corona::Systems::UI
