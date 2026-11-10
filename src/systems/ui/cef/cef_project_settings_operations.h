#pragma once

#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace Corona::Systems::UI {

// Saving project metadata updates the native scene's cached name and core
// version, so this single route stays in C++. Document reads/commits and the
// active-project projection run behind the Web -> Python bridge.
enum class ProjectSettingsMethod { SaveInfo };
struct ProjectSettingsRequest {
    ProjectSettingsMethod method;
    std::string archive_session, project_path, name, core_version;
};
struct ProjectInfoSnapshot {
    std::string name, mode, prompt, core_version, create_time, last_opened;
    std::string project_path, archive_session, source_path;
    bool read_only = false;
};
struct ProjectSettingsReply {
    std::string error_code, error_message;
    std::optional<ProjectInfoSnapshot> info;
};

ProjectSettingsReply execute_project_settings_operation(const ProjectSettingsRequest& request);

} // namespace Corona::Systems::UI
