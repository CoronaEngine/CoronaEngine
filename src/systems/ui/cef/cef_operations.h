#pragma once

// The operation routes that stay in C++ because they touch native state the Python services
// cannot own: reloading the scene when a project file opens, updating cached project metadata,
// the network session, the project launcher, and the single Web -> Python bridge entry.
//
// One header on purpose: the two translation units that implement these (the native API
// handlers and the V8 bindings) include every one of them together, so five separate headers
// only meant five include lines and five places to look. Kept free of JSON headers and of CEF
// types so the contract stays transport-agnostic - the guards assert exactly that.

#include <cstdint>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

#include <vector>

namespace Corona::Systems::UI {

// ============================================================================
// Project files: opening a file may reload the native scene, so this single
// route stays in C++. Every other project file operation lives behind the
// Web -> Python bridge.
// ============================================================================
enum class FilesMethod { Open };
struct FilesRequest {
    FilesMethod method;
    std::string archive_session, project_path, path, file_type;
};
struct FilesReply {
    std::string error_code, error_message, archive_session, project_path;
    bool ok = false;
};

FilesReply execute_files_operation(const FilesRequest& request);

// ============================================================================
// The single Web -> Python entry point.
//
// C++ owns transport only: it hands the envelope to the Python service and returns whatever
// that service produced. The request schema, the argument validation and the service
// allowlist all live on the Python side, so the editor exposes exactly one function here and
// stays free of business branches.
// ============================================================================
struct BridgeReply {
    std::string json;
    std::string error_code;
    std::string error_message;
};

// Invoked on the CEF UI thread only.
BridgeReply execute_bridge_operation(const std::string& envelope);

// ============================================================================
// Project settings: saving project metadata updates the native scene's cached
// name and core version, so this single route stays in C++. Document reads and
// commits and the active-project projection run behind the bridge.
// ============================================================================
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

// ============================================================================
// Network session and discovery.
// ============================================================================
enum class NetworkMethod { StartSession, StopSession, GetSessionInfo, GetDiscoveredPeers,
    ClearDiscoveredPeers, SearchLan, ConnectToPeer, SetProjectRoot };
struct NetworkRequest {
    NetworkMethod method;
    std::string name, ip, project_root, role = "host";
    std::uint64_t project_id = 0;
    std::uint16_t port = 27960;
};
struct NetworkPeerSnapshot {
    std::string ip, name, role, stable_id;
    std::uint16_t port = 0;
    std::uint64_t project_id = 0, last_seen_ms = 0;
};
struct NetworkSessionSnapshot {
    bool active = false;
    std::string role, host_address, local_ip;
    std::uint64_t peer_count = 0;
    std::uint16_t host_port = 0, listen_port = 0;
    std::vector<NetworkPeerSnapshot> discovered_peers;
};
struct NetworkReply {
    NetworkMethod method = NetworkMethod::GetSessionInfo;
    std::string error_code, error_message;
    bool ok = true;
    NetworkSessionSnapshot session;
    std::vector<NetworkPeerSnapshot> peers;
    std::uint64_t seeded_actor_count = 0, discovered_count = 0;
};

NetworkReply execute_network_operation(const NetworkRequest& request);

// ============================================================================
// Project launcher: version, recent projects, load status and project creation.
// ============================================================================
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
struct ProjectLoadSnapshot {
    bool active = false, loading = false, archive_service_ready = false;
    std::string path, scene, archive_session;
    std::uint64_t total = 0, ready = 0, failed = 0, pending = 0;
    double progress = 0;
};
struct ProjectLauncherReply {
    ProjectLauncherMethod method = ProjectLauncherMethod::GetVersion;
    std::string error_code, error_message;
    /// The reply for the methods whose value is entirely Python's: the version string and the
    /// recent-project array. Carried as JSON so its shape is declared once, where it is produced.
    nlohmann::json value = nlohmann::json();
    ProjectLoadSnapshot load;
    CreatedProjectSnapshot created;
    SelectedProjectSnapshot selected;
    OpenProjectSnapshot opened;
};

ProjectLauncherReply execute_project_launcher_operation(const ProjectLauncherRequest& request);

}  // namespace Corona::Systems::UI
