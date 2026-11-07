#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Corona::Systems::UI {

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

} // namespace Corona::Systems::UI
