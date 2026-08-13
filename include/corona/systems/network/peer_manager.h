#pragma once

#include <corona/systems/network/protocol.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// fwd
struct _ENetHost;
struct _ENetPeer;
struct _ENetPacket;

namespace Corona::Network {

/**
 * @brief Manages the ENet host and all peer connections.
 *
 * One ENetHost per instance (server + client in one).  Connections form
 * a full-mesh among participants on the same LAN project.
 */
class PeerManager {
public:
    /// Metadata for one connected peer.
    struct PeerInfo {
        std::string id;         // current lookup key (= stable_id after HELLO)
        std::string stable_id;  // "name@ip:listen_port" — identical on both ends
        std::string writer_id;  // authenticated HELLO identity: "name@listen_port"
        std::string name;       // instance_name (from HELLO)
        _ENetPeer* peer = nullptr;
        bool connected = false;
        bool hello_done = false;  // true once HELLO exchanged and peer rekeyed
        bool outbound = false;    // true if WE initiated this connection
        uint64_t last_receive_ms = 0;
        uint64_t last_heartbeat_sent_ms = 0;
        bool disconnecting = false;
    };

    /// Called when a new peer connects (after the HELLO handshake completes).
    using OnPeerConnected = std::function<void(const PeerInfo&)>;

    /// Called when a peer disconnects (timeout or explicit leave).
    using OnPeerDisconnected = std::function<void(const PeerInfo&)>;

    /// Called when a sync/data message arrives on any ENet channel.
    using OnDataReceived = std::function<void(const std::string& peer_id,
                                              const uint8_t* data, size_t len)>;

    PeerManager();
    ~PeerManager();

    PeerManager(const PeerManager&) = delete;
    PeerManager& operator=(const PeerManager&) = delete;

    // ========================================================================
    // Lifecycle
    // ========================================================================

    /**
     * @brief Create the ENet host and start listening on `port`.
     * @param port           UDP port for ENet.
     * @param instance_name  Local instance name (used as peer_id tiebreaker).
     * @return true on success.
     */
    bool start(uint16_t port, const std::string& instance_name);

    /// Disconnect all peers and destroy the ENet host.
    void stop();

    // ========================================================================
    // Peering
    // ========================================================================

    /**
     * @brief Connect to a remote peer by IP/port.
     * If the peer is already connected or connecting, it is ignored.
     * Unless force=true, applies the ID-ordering rule to avoid duplicate links.
     */
    void connect_to_peer(const std::string& ip, uint16_t port,
                         const std::string& peer_name, bool force = false);

    /// Disconnect a specific peer by ID.
    void disconnect_peer(const std::string& peer_id);

    /// Number of currently connected peers.
    [[nodiscard]] size_t peer_count() const;

    /// Get the list of connected peers.
    [[nodiscard]] std::vector<PeerInfo> peers() const;

    /// Look up a peer by ID.
    [[nodiscard]] const PeerInfo* find_peer(const std::string& peer_id) const;

    /// Resolve the authenticated LWW writer identity for a ready peer.
    [[nodiscard]] std::string writer_peer_id(const std::string& peer_id) const;

    /// Local peer ID string ("ip:port").
    [[nodiscard]] const std::string& local_peer_id() const;

    /// The next outgoing packet sequence number (per-peer seq would be better,
    /// but a global monotonic counter is fine for debugging).
    [[nodiscard]] uint32_t next_seq();

    // ========================================================================
    // Sending
    // ========================================================================

    /// Send to ALL connected peers.
    void broadcast(int channel, const void* data, size_t len, bool reliable);

    /// Send to a single peer (by ENetPeer pointer — from PeerInfo).
    void send_to(_ENetPeer* peer, int channel, const void* data, size_t len,
                 bool reliable);

    /// Send to a ready peer identified by its stable lookup ID.
    bool send_to_peer_id(const std::string& peer_id, int channel,
                         const void* data, size_t len, bool reliable);

    // ========================================================================
    // Polling
    // ========================================================================

    /// Process incoming ENet events.  Call from the system thread each tick.
    void poll();

    // ========================================================================
    // Callbacks
    // ========================================================================

    void set_on_peer_connected(OnPeerConnected cb);
    void set_on_peer_disconnected(OnPeerDisconnected cb);
    void set_on_data_received(OnDataReceived cb);

private:
    void handle_connect(_ENetPeer* peer, const std::string& remote_id,
                        const std::string& name);
    void handle_disconnect(const PeerInfo& info);
    void handle_hello(_ENetPeer* peer, const uint8_t* data, size_t len);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace Corona::Network
