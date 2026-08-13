#pragma once

#include <corona/systems/network/protocol.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

// Forward declarations
struct _ENetHost;

namespace Corona::Network {

/**
 * @brief LAN UDP broadcast-based peer discovery.
 *
 * Broadcasts a CORONA discovery packet every kDiscoveryIntervalMs and listens
 * for broadcasts from other instances on the same network.  Discovered peers
 * are reported via callback so the PeerManager can establish ENet connections.
 */
class Discovery {
public:
    /// Callback invoked when a remote peer is discovered.
    /// @param ip  Remote peer's IP address (string form).
    /// @param name Instance name (for UI display).
    /// @param project_id  Project identifier hash.
    /// @param listen_port ENet port advertised by the peer.
    /// @param session_role 0=none, 1=host, 2=client.
    using OnPeerDiscovered = std::function<void(const std::string& ip,
                                                const std::string& name,
                                                uint64_t project_id,
                                                uint16_t listen_port,
                                                uint8_t session_role)>;

    Discovery();
    ~Discovery();

    // Non-copyable, movable
    Discovery(const Discovery&) = delete;
    Discovery& operator=(const Discovery&) = delete;
    Discovery(Discovery&&) = delete;
    Discovery& operator=(Discovery&&) = delete;

    /**
     * @brief Start broadcasting and listening for discovery packets.
     * @param port          UDP port to bind / broadcast to.
     * @param instance_name Human-readable instance name (max 31 chars).
     * @param project_id    Project identifier hash (same-project filtering).
     * @return true on success.
     */
    bool start(uint16_t port, const std::string& instance_name,
               uint64_t project_id, uint16_t listen_port, uint8_t session_role);

    /// Stop discovery and release the socket.
    void stop();

    /// Set the callback for newly-discovered peers.
    void set_on_peer_discovered(OnPeerDiscovered cb);

    /// Call every tick — reads incoming broadcast packets and fires callbacks.
    void poll();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace Corona::Network
