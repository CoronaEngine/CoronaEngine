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
struct ProjectSettingsReply {
    std::string error_code, error_message;
    /// The saved project's metadata, as the archive service answered it. One declaration of the
    /// shape - and it is the side that owns the file.
    nlohmann::json value = nlohmann::json();
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

struct ProjectLauncherReply {
    ProjectLauncherMethod method = ProjectLauncherMethod::GetVersion;
    std::string error_code, error_message;
    /// The reply value, as JSON.
    ///
    /// Every method's value is produced as JSON already - by the archive service, by the load
    /// status query, or here. Carrying it as JSON keeps one declaration of each shape instead of
    /// a struct that mirrors it and a converter that rebuilds it.
    nlohmann::json value = nlohmann::json();
};

ProjectLauncherReply execute_project_launcher_operation(const ProjectLauncherRequest& request);

}  // namespace Corona::Systems::UI
