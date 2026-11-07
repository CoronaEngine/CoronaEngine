#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Corona::Systems::UI {

enum class ProjectLauncherMethod { GetVersion, GetRecent, GetLoadStatus, CreateWorld, CreateMultiplayer, SelectFile };
struct ProjectLauncherRequest {
    ProjectLauncherMethod method;
    std::string prompt, role = "guest";
};
struct CreatedProjectSnapshot { std::string name, path, role; };
struct SelectedProjectSnapshot { std::string name, path; };
struct RecentProjectSnapshot {
    std::string name, path, last_edited;
    bool exists = false;
};
struct ProjectLoadSnapshot {
    bool active = false, loading = false, archive_service_ready = false;
    std::string path, scene, archive_session;
    std::uint64_t total = 0, ready = 0, failed = 0, pending = 0;
    double progress = 0;
};
struct ProjectLauncherReply {
    ProjectLauncherMethod method = ProjectLauncherMethod::GetVersion;
    std::string error_code, error_message, version;
    std::vector<RecentProjectSnapshot> recent;
    ProjectLoadSnapshot load;
    CreatedProjectSnapshot created;
    SelectedProjectSnapshot selected;
};

ProjectLauncherReply execute_project_launcher_operation(const ProjectLauncherRequest& request);

} // namespace Corona::Systems::UI
