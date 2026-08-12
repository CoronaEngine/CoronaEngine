#include <corona/systems/network/peer_manager.h>

#include <enet/enet.h>
#include <corona/kernel/core/i_logger.h>
#include <corona/systems/network/peer_liveness.h>
#include <corona/systems/network/peer_list_utils.h>

#include <algorithm>
#include <chrono>
#include <mutex>

namespace Corona::Network {

namespace {
uint64_t peer_now_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count());
}
}

struct PeerManager::Impl {
    ENetHost* host = nullptr;
    bool enet_initialized = false;
    std::string local_id;
    std::string instance_name;
    uint16_t listen_port = 0;

    // Peer tracking
    std::vector<PeerInfo> peer_list;
    mutable std::mutex peer_mutex;

    // Sequence counter
    std::atomic<uint32_t> sequence{0};

    // Callbacks
    OnPeerConnected on_connected;
    OnPeerDisconnected on_disconnected;
    OnDataReceived on_data;

    // ------------------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------------------

    PeerInfo* find_peer_unsafe(_ENetPeer* raw) {
        for (auto& p : peer_list) {
            if (p.peer == raw) return &p;
        }
        return nullptr;
    }

    PeerInfo* find_peer_by_id_unsafe(const std::string& id) {
        for (auto& p : peer_list) {
            if (p.id == id) return &p;
        }
        return nullptr;
    }

    PeerInfo* find_peer_by_stable_id_unsafe(const std::string& sid) {
        for (auto& p : peer_list) {
            if (p.hello_done && p.stable_id == sid) return &p;
        }
        return nullptr;
    }
};

// ============================================================================
PeerManager::PeerManager() : impl_(std::make_unique<Impl>()) {}

PeerManager::~PeerManager() { stop(); }

// ============================================================================
bool PeerManager::start(uint16_t port, const std::string& instance_name) {
    if (impl_->host) {
        CFW_LOG_WARNING("PeerManager: Already started");
        return true;
    }

    // Initialize ENet (refcounted internally by ENet on most platforms; we
    // guard with our own flag to pair exactly one init with one deinit).
    if (!impl_->enet_initialized) {
        if (enet_initialize() != 0) {
            CFW_LOG_ERROR("PeerManager: enet_initialize() failed");
            return false;
        }
        impl_->enet_initialized = true;
    }

    impl_->instance_name = instance_name;
    impl_->listen_port = port;

    // Local stable id: "name@port". The IP is filled per-peer from the
    // observed remote address; locally we only need name+port for HELLO.
    impl_->local_id = instance_name + "@" + std::to_string(port);

    // Create the ENet host
    // address.port = port, maxPeers = 64, maxChannels = 2
    ENetAddress addr{};
    addr.host = ENET_HOST_ANY;
    addr.port = port;

    impl_->host = enet_host_create(
        &addr,      // listen address
        64,         // max peers
        2,          // channel count
        0,          // incoming bandwidth (unlimited)
        0           // outgoing bandwidth (unlimited)
    );

    if (!impl_->host) {
        CFW_LOG_ERROR("PeerManager: Failed to create ENet host on port {}", port);
        return false;
    }

    return true;
}

void PeerManager::stop() {
    if (!impl_->host) {
        // Even with no host, release ENet if we initialized it.
        if (impl_->enet_initialized) {
            enet_deinitialize();
            impl_->enet_initialized = false;
        }
        return;
    }

    // Disconnect all peers (forceful, no wait)
    {
        std::lock_guard lock(impl_->peer_mutex);
        for (auto& p : impl_->peer_list) {
            if (p.peer) enet_peer_disconnect_now(p.peer, 0);
        }
    }

    // Flush
    enet_host_flush(impl_->host);

    // Destroy host (any remaining peers are forcefully dropped)
    enet_host_destroy(impl_->host);
    impl_->host = nullptr;

    {
        std::lock_guard lock(impl_->peer_mutex);
        impl_->peer_list.clear();
    }

    // Release ENet (and its WinSock refcount) now that the host is gone.
    if (impl_->enet_initialized) {
        enet_deinitialize();
        impl_->enet_initialized = false;
    }
}

// ============================================================================
void PeerManager::connect_to_peer(const std::string& ip, uint16_t port,
                                  const std::string& peer_name, bool force) {
    if (!impl_->host) {
        CFW_LOG_WARNING("PeerManager: Cannot connect — host not started");
        return;
    }

    // Reject empty / wildcard IPs (but allow loopback for same-machine testing)
    if (ip.empty() || ip == "0.0.0.0") {
        CFW_LOG_WARNING("PeerManager: Reject connection to invalid IP '{}'", ip);
        return;
    }

    std::string remote_id = make_peer_id(ip.c_str(), port);

    // Reject self-connection (same IP:port as our own listen address)
    if (remote_id == impl_->local_id) {
        CFW_LOG_WARNING("PeerManager: Reject self-connection to {}", remote_id);
        return;
    }

    // Connection ordering (auto-discovery only): when two peers discover each
    // other, exactly one must initiate, else they either both wait (deadlock)
    // or both connect (duplicate pair). We order by instance name, which is
    // symmetric and available on both sides: the lexicographically smaller
    // name waits for the inbound connection, the larger initiates.
    // force=true (manual connect) always initiates.
    if (!force && !peer_name.empty() && !impl_->instance_name.empty()
        && impl_->instance_name != peer_name) {
        if (impl_->instance_name < peer_name) {
            return;
        }
    }

    // Check if already connected
    {
        std::lock_guard lock(impl_->peer_mutex);
        if (impl_->find_peer_by_id_unsafe(remote_id)) return;
    }

    ENetAddress addr{};
    enet_address_set_host(&addr, ip.c_str());
    addr.port = port;

    ENetPeer* peer = enet_host_connect(impl_->host, &addr, 2, 0);
    if (!peer) {
        CFW_LOG_ERROR("PeerManager: Failed to initiate connection to {}", remote_id);
        return;
    }

    // Store pending peer info (data is the remote peer ID string so we can
    // match it on the connect event)
    {
        std::lock_guard lock(impl_->peer_mutex);
        PeerInfo info;
        info.id = remote_id;
        info.name = peer_name;
        info.peer = peer;
        info.connected = false;
        info.outbound = true;  // we initiated this connection
        impl_->peer_list.push_back(info);
    }
}

void PeerManager::disconnect_peer(const std::string& peer_id) {
    std::lock_guard lock(impl_->peer_mutex);
    auto* p = impl_->find_peer_by_id_unsafe(peer_id);
    if (p && p->peer) {
        enet_peer_disconnect(p->peer, 0);
    }
}

// ============================================================================
size_t PeerManager::peer_count() const {
    std::lock_guard lock(impl_->peer_mutex);
    size_t count = 0;
    for (const auto& p : impl_->peer_list) {
        if (p.connected && p.hello_done) ++count;
    }
    return count;
}

std::vector<PeerManager::PeerInfo> PeerManager::peers() const {
    std::lock_guard lock(impl_->peer_mutex);
    return impl_->peer_list;
}

const PeerManager::PeerInfo* PeerManager::find_peer(
    const std::string& peer_id) const {
    std::lock_guard lock(impl_->peer_mutex);
    return impl_->find_peer_by_id_unsafe(peer_id);
}

const std::string& PeerManager::local_peer_id() const {
    return impl_->local_id;
}

uint32_t PeerManager::next_seq() {
    return impl_->sequence.fetch_add(1);
}

// ============================================================================
void PeerManager::broadcast(int channel, const void* data, size_t len,
                            bool reliable) {
    if (!impl_->host || len == 0) return;

    ENetPacket* pkt = enet_packet_create(
        data, len,
        reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED);

    enet_host_broadcast(impl_->host, channel, pkt);
    // enet_host_broadcast takes ownership — no need to destroy pkt
}

void PeerManager::send_to(_ENetPeer* peer, int channel,
                          const void* data, size_t len, bool reliable) {
    if (!peer || len == 0) return;

    ENetPacket* pkt = enet_packet_create(
        data, len,
        reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED);

    enet_peer_send(peer, channel, pkt);
}

bool PeerManager::send_to_peer_id(const std::string& peer_id, int channel,
                                  const void* data, size_t len, bool reliable) {
    if (peer_id.empty() || !data || len == 0 || !impl_->host) return false;
    _ENetPeer* peer = nullptr;
    {
        std::lock_guard lock(impl_->peer_mutex);
        auto* info = impl_->find_peer_by_id_unsafe(peer_id);
        if (!info || !info->connected || !info->hello_done) return false;
        peer = info->peer;
    }
    if (!peer) return false;
    send_to(peer, channel, data, len, reliable);
    return true;
}

// ============================================================================
void PeerManager::poll() {
    if (!impl_->host) return;

    const auto now = peer_now_ms();

    ENetEvent event;
    while (enet_host_service(impl_->host, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            // ENet only exposes the remote's ephemeral source port here, not
            // its listen port, so this id is provisional. We send HELLO and
            // rekey to a stable id once the peer's HELLO arrives.
            char ip[64];
            enet_address_get_host_ip(&event.peer->address, ip, sizeof(ip));
            std::string remote_id = make_peer_id(ip, event.peer->address.port);

            {
                std::lock_guard lock(impl_->peer_mutex);
                auto* info = impl_->find_peer_unsafe(event.peer);
                if (!info) {
                    // Inbound connection (or pending entry keyed differently):
                    // store a provisional entry keyed by ephemeral ip:port.
                    PeerInfo pinfo;
                    pinfo.id = remote_id;
                    pinfo.name = remote_id;
                    pinfo.peer = event.peer;
                    pinfo.connected = true;
                    pinfo.last_receive_ms = now;
                    impl_->peer_list.push_back(pinfo);
                } else {
                    info->connected = true;
                    info->peer = event.peer;
                    info->last_receive_ms = now;
                }
            }

            // Send our HELLO (stable identity) immediately. on_connected is
            // deferred until we receive the peer's HELLO and rekey.
            auto hello = build_hello(impl_->instance_name, impl_->listen_port);
            send_to(event.peer, kChannelReliable, hello.data(), hello.size(), true);
            break;
        }

        case ENET_EVENT_TYPE_DISCONNECT: {
            PeerInfo disconnected;
            bool found = false;
            {
                std::lock_guard lock(impl_->peer_mutex);
                auto* info = impl_->find_peer_unsafe(event.peer);
                if (info) {
                    disconnected = *info;
                    found = info->hello_done;  // only notify if it was fully up
                    impl_->peer_list.erase(
                        std::remove_if(impl_->peer_list.begin(), impl_->peer_list.end(),
                            [&](const PeerInfo& p) { return p.peer == event.peer; }),
                        impl_->peer_list.end());
                }
            }
            if (found) handle_disconnect(disconnected);
            break;
        }

        case ENET_EVENT_TYPE_RECEIVE: {
            // Intercept HELLO internally; everything else goes to on_data.
            bool is_hello = event.packet->dataLength >= 1 &&
                static_cast<MessageType>(event.packet->data[0]) == MessageType::HELLO;

            if (is_hello) {
                handle_hello(event.peer, event.packet->data, event.packet->dataLength);
            } else {
                std::string peer_id;
                bool ready = false;
                {
                    std::lock_guard lock(impl_->peer_mutex);
                    auto* info = impl_->find_peer_unsafe(event.peer);
                    if (info) {
                        info->last_receive_ms = now;
                        peer_id = info->id;
                        ready = info->hello_done;
                    }
                }
                if (event.packet->dataLength >= 1 &&
                    static_cast<MessageType>(event.packet->data[0]) == MessageType::HEARTBEAT) {
                    enet_packet_destroy(event.packet);
                    break;
                }
                // Drop data from peers that haven't completed HELLO yet
                if (ready && !peer_id.empty() && impl_->on_data) {
                    impl_->on_data(peer_id, event.packet->data,
                                   event.packet->dataLength);
                }
            }
            enet_packet_destroy(event.packet);
            break;
        }

        default:
            break;
        }
    }

    // Process queued receive events before checking liveness so a packet
    // arriving on the timeout boundary refreshes last_receive_ms first.
    const auto maintenance_now = peer_now_ms();
    std::vector<_ENetPeer*> timed_out;
    {
        std::lock_guard lock(impl_->peer_mutex);
        for (auto& info : impl_->peer_list) {
            if (!info.connected || !info.hello_done || !info.peer) continue;
            if (info.last_receive_ms == 0) info.last_receive_ms = maintenance_now;
            const auto action = peer_liveness_action(
                {info.last_receive_ms, info.last_heartbeat_sent_ms, info.disconnecting},
                maintenance_now);
            if (action == PeerLivenessAction::Disconnect) {
                info.disconnecting = true;
                timed_out.push_back(info.peer);
                continue;
            }
            if (action == PeerLivenessAction::SendHeartbeat) {
                auto heartbeat = build_heartbeat(next_seq());
                send_to(info.peer, kChannelUnreliable,
                        heartbeat.data(), heartbeat.size(), false);
                info.last_heartbeat_sent_ms = maintenance_now;
            }
        }
    }
    for (auto* peer : timed_out) {
        if (peer) enet_peer_disconnect(peer, 0);
    }
}

// ============================================================================
void PeerManager::handle_hello(ENetPeer* peer, const uint8_t* data, size_t len) {
    BufferReader r(data + 1, len - 1);
    if (!r.has_remaining(2)) return;
    uint16_t name_len = r.read_u16();
    if (!r.has_remaining(name_len + 2)) return;
    std::string remote_name = r.read_string(name_len);
    uint16_t remote_listen_port = r.read_u16();

    // Build stable id from the OBSERVED remote IP + advertised listen port +
    // name. The IP is reliable (it's the real source address); only the port
    // in the CONNECT event was ephemeral, which is why HELLO carries the
    // listen port explicitly.
    char ip[64];
    enet_address_get_host_ip(&peer->address, ip, sizeof(ip));
    std::string stable_id = remote_name + "@" + std::string(ip) + ":" +
                            std::to_string(remote_listen_port);

    PeerInfo notify_info;
    bool should_notify = false;
    bool should_drop = false;
    {
        std::lock_guard lock(impl_->peer_mutex);

        auto* existing = impl_->find_peer_by_stable_id_unsafe(stable_id);
        auto* self = impl_->find_peer_unsafe(peer);

        if (existing && self && existing->peer != peer) {
            // Cross-connect duplicate (A→B and B→A simultaneously).
            // Deterministic tiebreak: keep the connection where the end with
            // the lexicographically smaller stable_id is the one that
            // INITIATED (outbound). On that end, existing is the one we
            // initiated, so we DROP `self` (the inbound duplicate).
            // On the OTHER end, `self` IS the outbound one, so `existing`
            // won't be found yet (we're holding the only mutex lock across
            // both connections... actually no — these are two separate
            // enet_host_service events; they run sequentially on the same
            // thread so there IS no race).  The simplest correct rule:
            // `self->outbound` takes priority; drop inbound extras.
            if (self->outbound) {
                // SELF is our outbound — keep it, drop existing
                auto* discarded_peer = existing->peer;
                auto* retained_peer = self->peer;
                existing->peer = nullptr;
                existing->connected = false;
                existing->hello_done = false;
                // Erase may invalidate all vector pointers, so locate the
                // retained connection again before updating it.
                self = erase_peer_and_refind(
                    impl_->peer_list, discarded_peer, retained_peer);
                if (discarded_peer) enet_peer_disconnect_later(discarded_peer, 0);
                if (!self) return;
                // Rekey self
                self->stable_id = stable_id;
                self->id = stable_id;
                self->name = remote_name;
                self->hello_done = true;
                notify_info = *self;
                should_notify = true;
                // The duplicate inbound connection that delivered THIS HELLO
                // will be dropped below via should_drop check on `existing`
            } else {
                // SELF is an inbound duplicate — drop it
                should_drop = true;
            }
        } else if (self && self->hello_done) {
            // Already handled — duplicate HELLO (shouldn't happen)
            return;
        } else if (self) {
            self->stable_id = stable_id;
            self->id = stable_id;       // rekey lookup key to the stable id
            self->name = remote_name;
            self->hello_done = true;
            self->last_receive_ms = peer_now_ms();
            notify_info = *self;
            should_notify = true;
        }
    }

    if (should_drop) {
        CFW_LOG_DEBUG("PeerManager: Duplicate connection to {} — dropping extra", stable_id);
        enet_peer_disconnect_later(peer, 0);
        return;
    }

    if (should_notify) {
        CFW_LOG_DEBUG("PeerManager: HELLO from {} — peer rekeyed and ready", stable_id);
        handle_connect(peer, notify_info.id, notify_info.name);
    }
}

// ============================================================================
void PeerManager::handle_connect(ENetPeer* /*peer*/,
                                 const std::string& remote_id,
                                 const std::string& name) {
    CFW_LOG_INFO("PeerManager: Peer connected — {} ({})", remote_id, name);
    if (impl_->on_connected) {
        PeerInfo info;
        info.id = remote_id;
        info.name = name;
        info.connected = true;
        impl_->on_connected(info);
    }
}

void PeerManager::handle_disconnect(const PeerInfo& info) {
    CFW_LOG_INFO("PeerManager: Peer disconnected — {} ({})", info.id, info.name);
    if (impl_->on_disconnected) {
        impl_->on_disconnected(info);
    }
}

// ============================================================================
void PeerManager::set_on_peer_connected(OnPeerConnected cb) {
    impl_->on_connected = std::move(cb);
}

void PeerManager::set_on_peer_disconnected(OnPeerDisconnected cb) {
    impl_->on_disconnected = std::move(cb);
}

void PeerManager::set_on_data_received(OnDataReceived cb) {
    impl_->on_data = std::move(cb);
}

}  // namespace Corona::Network
