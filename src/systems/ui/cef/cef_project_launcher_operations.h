#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Corona::Systems::UI {

enum class ProjectLauncherMethod { GetVersion, GetRecent, GetLoadStatus, CreateWorld, CreateMultiplayer, SelectFile, Open };
struct ProjectLauncherRequest {
    ProjectLauncherMethod method;
    std::string prompt, role = "guest", path, load_policy = "prompt";
};
struct CreatedProjectSnapshot { std::string name, path, role; };
struct SelectedProjectSnapshot { std::string name, path; };
struct ProjectOpenDiagnostic {
    std::optional<std::string> severity, stage, code, message, path, actor_name, actor_guid,
        shape_type, resource_path, field, camera_id;
    std::optional<bool> recoverable, requires_confirmation;
    std::optional<std::uint64_t> count;
};
struct OpenProjectSnapshot {
    bool ok = false;
    std::string status, path;
    std::optional<std::string> message;
    std::optional<bool> degraded;
    std::optional<std::uint64_t> unresolved_actor_count;
    std::optional<SelectedProjectSnapshot> scene;
    std::optional<std::vector<ProjectOpenDiagnostic>> diagnostics;
};
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
    OpenProjectSnapshot opened;
};

ProjectLauncherReply execute_project_launcher_operation(const ProjectLauncherRequest& request);

} // namespace Corona::Systems::UI
