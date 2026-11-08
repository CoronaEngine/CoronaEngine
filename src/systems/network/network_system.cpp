#include <corona/events/network_system_events.h>
#include <horizon/core/logging.h>
#include <corona/kernel/event/i_event_bus.h>
#include <corona/kernel/event/i_event_stream.h>
#include <corona/systems/network/file_transfer.h>
#include <corona/systems/network/network_identity.h>
#include <corona/systems/network/network_system.h>
#include <corona/systems/network/actor_editor_sync.h>
#include <corona/systems/network/actor_editor_payload.h>
#include <corona/systems/network/scoped_bool_override.h>
#include <corona/systems/network/scene_snapshot_policy.h>
#include <corona/shared_data_hub.h>

#include <chrono>
#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <mutex>
#include <unordered_map>

namespace Corona::Systems {

// ============================================================================
// Impl
// ============================================================================

struct NetworkSystem::Impl {
    Kernel::ISystemContext* ctx = nullptr;

    // Subsystems
    Network::PeerManager peer_manager;
    Network::Discovery discovery;
    Network::SyncEngine sync_engine;
    Network::NetworkIdentityRegistry identity_registry{SharedDataHub::instance()};
    Network::CollaborationState collaboration;
    bool stop_session_after_peer_disconnect = false;

    // State
    SessionState session_state{SessionState::Idle};
    SessionRole session_role{SessionRole::None};
    std::string instance_name;
    std::string host_address;
    uint16_t host_port = 0;
    uint64_t project_id = 0;
    uint16_t port = Network::kDefaultPort;
    std::unordered_map<std::string, DiscoveredPeer> discovered_peers;
    bool discovery_only = false;

    // Timing for sync ticks
    using Clock = std::chrono::steady_clock;
    Clock::time_point last_sync_time;

    // Event subscription IDs
    std::vector<Kernel::EventId> event_subscriptions;

    // File transfer state
    struct IncomingTransfer {
        uint64_t transfer_id = 0;
        std::string model_path;
        std::string sender_peer_id;  // isolate chunks from multi-sender
        uint32_t total_size = 0;
        uint32_t chunk_count = 0;
        std::vector<bool> received_chunks;
        std::vector<uint8_t> buffer;
        Clock::time_point last_chunk_time;
        bool complete = false;
    };
    // key = sender_peer_id + "/" + transfer_id (first responder wins per transfer)
    std::unordered_map<std::string, IncomingTransfer> incoming_transfers;

    // Outgoing transfer: for each model_path, cache the file data
    // so we don't re-read on every FILE_REQUEST
    struct CachedFileData {
        std::vector<uint8_t> data;
        Clock::time_point load_time;
    };
    std::unordered_map<std::string, CachedFileData> outgoing_cache;

    // Project root for file write destination
    std::string project_root;

    // Sync pause (suppress poll_and_sync during incoming actor creation)
    bool sync_paused = false;
    bool applying_versioned_actor_message = false;

    // Network callbacks run independently from the engine/UI update thread.
    // All deferred editor work is exchanged through this mutex-protected set
    // of queues and transfer groups.
    mutable std::recursive_mutex pending_mutex;

    // Deferred actions to execute in update() (avoid GIL in network thread)
    struct PendingAction {
        std::string actor_guid;
        std::string scene_name;
        std::string model_path;
        std::vector<std::string> dependency_paths;
        std::string actor_json;
        Network::ActorCreatePacked actor_packed;
    };
    std::vector<PendingAction> pending_actor_creates;

    struct PendingTransformUpdate {
        std::string actor_guid;
        std::string scene_name;
        float transform[9] = {0,0,0, 0,0,0, 1,1,1};
        std::string source_user_id;
        std::string correlation_id;
    };
    std::vector<PendingTransformUpdate> pending_actor_transform_updates;

    struct PendingActorDelete {
        std::string actor_guid;
        std::string scene_name;
        std::string actor_name;
    };
    std::vector<PendingActorDelete> pending_actor_deletes;

    struct PendingActorSceneSnapshotRequest {
        std::string scene_name;
    };
    std::vector<PendingActorSceneSnapshotRequest> pending_actor_scene_snapshot_requests;

    struct PendingActorSceneSnapshot {
        std::string scene_name;
        std::string snapshot_json;
    };
    std::vector<PendingActorSceneSnapshot> pending_actor_scene_snapshots;

    struct PendingActorStateUpdate {
        std::string actor_guid;
        std::string scene_name;
        std::string actor_json;
    };
    std::vector<PendingActorStateUpdate> pending_actor_state_updates;

    // Pending file transfers: model_path → actor data from the original
    // ACTOR_CREATE that triggered the transfer.  When the file arrives,
    // we reconstruct the PendingAction without requiring a re-send.
    struct PendingFileTransfer {
        std::string asset_id;
        std::string model_path;
        std::vector<std::string> dependency_paths;
        std::vector<PendingAction> actor_actions;
        std::vector<uint64_t> transfer_ids;
        uint32_t remaining_files = 0;
        Clock::time_point create_time;
        Clock::time_point last_activity_time;
    };
    struct CachedIncomingAsset {
        std::string model_path;
        std::vector<std::string> dependency_paths;
    };
    std::unordered_map<uint64_t, PendingFileTransfer> pending_file_transfer_groups;
    std::unordered_map<uint64_t, uint64_t> transfer_to_group;
    std::unordered_map<std::string, uint64_t> asset_to_transfer_group;
    std::unordered_map<std::string, CachedIncomingAsset> received_asset_cache;
    uint64_t next_transfer_id = 1;
};

namespace {
constexpr auto kTransferTimeout = std::chrono::seconds(30);
constexpr auto kOutgoingCacheTtl = std::chrono::minutes(2);


std::string actor_asset_id(const std::string& actor_json) {
    if (actor_json.empty()) return {};
    const auto document = nlohmann::json::parse(actor_json, nullptr, false);
    if (document.is_discarded() || !document.is_object()) return {};
    const auto it = document.find("asset_id");
    if (it == document.end() || !it->is_string()) return {};
    auto asset_id = it->get<std::string>();
    if (asset_id.empty() || asset_id.size() > 512) return {};
    return asset_id;
}

bool endpoint_matches(const std::string& peer_id,
                      const std::string& ip,
                      uint16_t port) {
    if (peer_id.empty() || ip.empty() || port == 0) return false;
    const std::string endpoint = ip + ":" + std::to_string(port);
    if (peer_id == endpoint) return true;
    if (peer_id.size() < endpoint.size()) return false;
    return peer_id.compare(peer_id.size() - endpoint.size(),
                           endpoint.size(), endpoint) == 0;
}

bool peer_matches_endpoint(const Network::PeerManager::PeerInfo& peer,
                           const std::string& ip,
                           uint16_t port) {
    return endpoint_matches(peer.id, ip, port) ||
           endpoint_matches(peer.stable_id, ip, port);
}

std::string_view session_role_label(NetworkSystem::SessionRole role) {
    switch (role) {
    case NetworkSystem::SessionRole::Host:
        return "host";
    case NetworkSystem::SessionRole::Client:
        return "client";
    case NetworkSystem::SessionRole::None:
    default:
        return "none";
    }
}
}  // namespace

// ============================================================================
// Lifecycle
// ============================================================================

NetworkSystem::NetworkSystem() : impl_(std::make_unique<Impl>()) {
    set_target_fps(60);
}

NetworkSystem::~NetworkSystem() = default;

bool NetworkSystem::initialize(Kernel::ISystemContext* ctx) {
    impl_->ctx = ctx;
    CFW_LOG_NOTICE("NetworkSystem: Initializing (ENet LAN collaborative editing)");

    // Wire up SyncEngine → PeerManager outbound path
    impl_->sync_engine.set_on_outgoing([this](const std::vector<uint8_t>& packet) {
        impl_->peer_manager.broadcast(
            Network::kChannelReliable,
            packet.data(), packet.size(),
            true);
    });
    impl_->sync_engine.set_on_targeted_outgoing(
        [this](const std::string& peer_id, const std::vector<uint8_t>& packet) {
            impl_->peer_manager.send_to_peer_id(peer_id, Network::kChannelReliable,
                                                packet.data(), packet.size(), true);
        });
    impl_->sync_engine.set_on_full_sync_request(
        [this](const std::string& peer_id) {
            impl_->sync_engine.sync_full_to(peer_id);
        });
    impl_->sync_engine.set_identity_mapping_callbacks(
        [this](Network::StorageID storage_id, uint64_t entity_seq) {
            return impl_->identity_registry.actor_guid_for_storage_seq(storage_id, entity_seq);
        },
        [this](Network::StorageID storage_id, const std::string& actor_guid)
            -> std::optional<uint64_t> {
            return impl_->identity_registry.storage_seq_for_actor_guid(storage_id, actor_guid);
        },
        [this](Network::StorageID storage_id, uint64_t entity_seq)
            -> std::optional<bool> {
            return impl_->identity_registry.local_ownership_for_storage_seq(
                storage_id, entity_seq);
        });
    impl_->sync_engine.set_on_editor_operation_applied(
        [this](const Network::EditorSyncOperation& operation) {
            std::lock_guard lock(impl_->pending_mutex);
            if (operation.kind == Network::EditorSyncOperationKind::Upsert &&
                (operation.field_name == "actor.create" ||
                 operation.field_name == "actor.state" ||
                 operation.field_name == "actor.transform")) {
                if (operation.field_name == "actor.create") {
                    std::erase_if(impl_->pending_actor_deletes,
                                  [&](const Impl::PendingActorDelete& pending) {
                                      return pending.actor_guid == operation.actor_guid;
                                  });
                }
                const auto legacy_type = operation.field_name == "actor.create"
                    ? Network::MessageType::ACTOR_CREATE
                    : operation.field_name == "actor.state"
                        ? Network::MessageType::ACTOR_STATE_UPDATE
                        : Network::MessageType::ACTOR_TRANSFORM_UPDATE;
                auto packet = Network::rebuild_actor_editor_packet(operation, legacy_type);
                if (packet.empty()) return;
                Network::ScopedBoolOverride guard(
                    impl_->applying_versioned_actor_message, true);
                on_custom_message(operation.version.writer_peer_id,
                                  packet.data(), packet.size());
                return;
            }
            if (operation.kind != Network::EditorSyncOperationKind::Delete) return;
            const auto& guid = operation.actor_guid;
            std::erase_if(impl_->pending_actor_creates,
                          [&](const Impl::PendingAction& action) {
                              return action.actor_guid == guid;
                          });
            std::erase_if(impl_->pending_actor_transform_updates,
                          [&](const Impl::PendingTransformUpdate& update) {
                              return update.actor_guid == guid;
                          });
            std::erase_if(impl_->pending_actor_state_updates,
                          [&](const Impl::PendingActorStateUpdate& update) {
                              return update.actor_guid == guid;
                          });
            std::erase_if(impl_->pending_actor_deletes,
                          [&](const Impl::PendingActorDelete& pending) {
                              return pending.actor_guid == guid;
                          });
            impl_->pending_actor_deletes.push_back(
                Impl::PendingActorDelete{guid, {}, {}});
        });

    // Wire up PeerManager → SyncEngine inbound path
    impl_->peer_manager.set_on_data_received(
        [this](const std::string& peer_id, const uint8_t* data, size_t len) {
            // Route: SYNC_DIRTY/SYNC_FULL/HEARTBEAT → sync engine
            //         ACTOR_CREATE/FILE_REQUEST/FILE_CHUNK → custom handler
            if (len >= 1) {
                using Network::MessageType;
                auto mt = static_cast<MessageType>(data[0]);
                if (mt == MessageType::SYNC_DIRTY || mt == MessageType::SYNC_FULL ||
                    mt == MessageType::EDITOR_SYNC ||
                    mt == MessageType::EDITOR_SNAPSHOT_REQUEST ||
                    mt == MessageType::EDITOR_SNAPSHOT_BEGIN ||
                    mt == MessageType::EDITOR_SNAPSHOT_CHUNK ||
                    mt == MessageType::EDITOR_SNAPSHOT_END ||
                    mt == MessageType::HEARTBEAT) {
                    on_data_received(peer_id, data, len);
                } else {
                    on_custom_message(peer_id, data, len);
                }
            }
        });

    // Wire up PeerManager connect/disconnect → events
    impl_->peer_manager.set_on_peer_connected(
        [this](const Network::PeerManager::PeerInfo& info) {
            on_peer_connected(info);
        });

    impl_->peer_manager.set_on_peer_disconnected(
        [this](const Network::PeerManager::PeerInfo& info) {
            on_peer_disconnected(info);
        });

    return true;
}

void NetworkSystem::update() {
    std::lock_guard pending_lock(impl_->pending_mutex);
    impl_->discovery.poll();
    if (impl_->session_state != SessionState::Active) return;

    auto now = Impl::Clock::now();

    // Poll ENet events every tick
    impl_->peer_manager.poll();

    if (impl_->stop_session_after_peer_disconnect) {
        impl_->stop_session_after_peer_disconnect = false;
        stop_session();
        return;
    }

    // Sync engine tick (~60 Hz) — paused during remote actor creation
    // to ensure both peers build identical storage layouts (seq_id alignment).
    if (!impl_->sync_paused && now - impl_->last_sync_time >=
        std::chrono::milliseconds(Network::kSyncIntervalMs)) {
        impl_->sync_engine.poll_and_sync();
        impl_->last_sync_time = now;
    }

    for (auto it = impl_->incoming_transfers.begin();
         it != impl_->incoming_transfers.end(); ) {
        if (now - it->second.last_chunk_time > kTransferTimeout) {
            CFW_LOG_WARNING("NetworkSystem: Incoming transfer timed out — id={} path='{}'",
                            it->second.transfer_id, it->second.model_path);
            it = impl_->incoming_transfers.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = impl_->pending_file_transfer_groups.begin();
         it != impl_->pending_file_transfer_groups.end(); ) {
        if (Network::has_file_group_timed_out(
                it->second.create_time, it->second.last_activity_time,
                now, kTransferTimeout)) {
            CFW_LOG_WARNING(
                "NetworkSystem: Pending file group timed out — asset='{}' actors={} model='{}' "
                "remaining={} transfers={}",
                it->second.asset_id, it->second.actor_actions.size(), it->second.model_path,
                it->second.remaining_files, it->second.transfer_ids.size());
            for (uint64_t transfer_id : it->second.transfer_ids) {
                impl_->transfer_to_group.erase(transfer_id);
            }
            if (!it->second.asset_id.empty()) {
                impl_->asset_to_transfer_group.erase(it->second.asset_id);
            }
            it = impl_->pending_file_transfer_groups.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = impl_->outgoing_cache.begin(); it != impl_->outgoing_cache.end(); ) {
        if (now - it->second.load_time > kOutgoingCacheTtl) {
            it = impl_->outgoing_cache.erase(it);
        } else {
            ++it;
        }
    }
}

void NetworkSystem::shutdown() {
    CFW_LOG_NOTICE("NetworkSystem: Shutting down...");
    stop_session();

    // Unsubscribe events
    if (impl_->ctx && impl_->ctx->event_bus()) {
        for (auto id : impl_->event_subscriptions) {
            impl_->ctx->event_bus()->unsubscribe(id);
        }
    }
    impl_->event_subscriptions.clear();
}

// ============================================================================
// Public API
// ============================================================================

bool NetworkSystem::start_session(const std::string& instance_name,
                                  uint64_t project_id, uint16_t port,
                                  SessionRole role) {
    if (impl_->session_state == SessionState::Active) {
        CFW_LOG_WARNING("NetworkSystem: Session already active; keeping role={} and ignoring requested role={}",
                        session_role_label(impl_->session_role), session_role_label(role));
        return true;
    }

    if (impl_->discovery_only) {
        impl_->discovery.stop();
        impl_->discovered_peers.clear();
    }
    impl_->session_state = SessionState::Starting;
    impl_->discovery_only = false;
    impl_->session_role = role == SessionRole::None ? SessionRole::Host : role;
    impl_->instance_name = instance_name;
    impl_->host_address.clear();
    impl_->host_port = 0;
    impl_->project_id = project_id;
    impl_->port = port;

    CFW_LOG_INFO("NetworkSystem: Starting manual-IP session '{}' on port {} role={} (project={:x})",
                 instance_name, port, session_role_label(impl_->session_role), project_id);

    // 1. PeerManager
    if (!impl_->peer_manager.start(port, instance_name)) {
        impl_->session_state = SessionState::Error;
        impl_->session_role = SessionRole::None;
        CFW_LOG_ERROR("NetworkSystem: Failed to start PeerManager");
        return false;
    }
    impl_->port = impl_->peer_manager.listen_port();

    impl_->discovery.set_on_peer_discovered(
        [this](const std::string& ip, const std::string& name,
               uint64_t discovered_project, uint16_t listen_port,
               uint8_t role) {
            if ((impl_->project_id != 0 && discovered_project != impl_->project_id) ||
                listen_port == 0) return;
            const auto now = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    Impl::Clock::now().time_since_epoch()).count());
            DiscoveredPeer peer;
            peer.ip = ip;
            peer.name = name;
            peer.port = listen_port;
            peer.project_id = discovered_project;
            peer.role = role == 1 ? SessionRole::Host :
                        role == 2 ? SessionRole::Client : SessionRole::None;
            peer.stable_id = name + "@" + ip + ":" +
                             std::to_string(listen_port);
            peer.last_seen_ms = now;
            impl_->discovered_peers[peer.stable_id] = std::move(peer);
        });
    if (!impl_->discovery.start(Network::kDefaultPort + Network::kDiscoveryPortOffset,
                                instance_name, project_id, impl_->port,
                                impl_->session_role == SessionRole::Host ? 1 : 2)) {
        CFW_LOG_WARNING("NetworkSystem: LAN discovery unavailable; manual IP remains available");
    }

    // 2. SyncEngine
    impl_->sync_engine.initialize(impl_->peer_manager.local_peer_id());

    impl_->session_state = SessionState::Active;
    impl_->last_sync_time = Impl::Clock::now();

    // Publish event
    if (impl_->ctx && impl_->ctx->event_bus()) {
        Events::NetworkHostStartedEvent ev{impl_->port};
        impl_->ctx->event_bus()->publish(ev);
    }

    CFW_LOG_INFO("NetworkSystem: Session active — listening on port {} role={}",
                 impl_->port, session_role_label(impl_->session_role));
    return true;
}

void NetworkSystem::stop_session() {
    if (impl_->session_state != SessionState::Active &&
        impl_->session_state != SessionState::Error &&
        !impl_->discovery_only) return;

    impl_->peer_manager.stop();
    impl_->discovery.stop();
    impl_->sync_engine.shutdown();
    impl_->identity_registry.clear();
    impl_->incoming_transfers.clear();
    impl_->outgoing_cache.clear();
    {
        std::lock_guard lock(impl_->pending_mutex);
        impl_->pending_actor_creates.clear();
        impl_->pending_actor_transform_updates.clear();
        impl_->pending_actor_deletes.clear();
        impl_->pending_actor_scene_snapshot_requests.clear();
        impl_->pending_actor_scene_snapshots.clear();
        impl_->pending_actor_state_updates.clear();
        impl_->pending_file_transfer_groups.clear();
        impl_->transfer_to_group.clear();
        impl_->asset_to_transfer_group.clear();
        impl_->received_asset_cache.clear();
    }
    impl_->collaboration.clear();
    impl_->stop_session_after_peer_disconnect = false;

    impl_->session_state = SessionState::Idle;
    impl_->session_role = SessionRole::None;
    impl_->host_address.clear();
    impl_->host_port = 0;
    impl_->discovered_peers.clear();
    impl_->discovery_only = false;

    // Publish event
    if (impl_->ctx && impl_->ctx->event_bus()) {
        Events::NetworkHostStoppedEvent ev;
        impl_->ctx->event_bus()->publish(ev);
    }

    CFW_LOG_INFO("NetworkSystem: Session stopped");
}

NetworkSystem::SessionState NetworkSystem::session_state() const {
    return impl_->session_state;
}

NetworkSystem::SessionRole NetworkSystem::session_role() const {
    return impl_->session_role;
}

std::string_view NetworkSystem::session_role_name() const {
    return Corona::Systems::session_role_label(impl_->session_role);
}

const std::string& NetworkSystem::host_address() const {
    return impl_->host_address;
}

uint16_t NetworkSystem::host_port() const {
    return impl_->host_port;
}

uint16_t NetworkSystem::session_port() const {
    return impl_->port;
}

size_t NetworkSystem::peer_count() const {
    return impl_->peer_manager.peer_count();
}

std::string NetworkSystem::local_peer_id() const {
    return impl_->peer_manager.local_peer_id();
}

bool NetworkSystem::start_discovery(const std::string& instance_name,
                                    uint64_t project_id) {
    if (impl_->session_state == SessionState::Idle) {
        impl_->discovery_only = true;
        impl_->project_id = project_id;
    }
    impl_->discovery.set_on_peer_discovered(
        [this](const std::string& ip, const std::string& name,
               uint64_t discovered_project, uint16_t listen_port,
               uint8_t role) {
            if ((impl_->project_id != 0 && discovered_project != impl_->project_id) ||
                listen_port == 0) return;
            const auto now = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    Impl::Clock::now().time_since_epoch()).count());
            DiscoveredPeer peer;
            peer.ip = ip;
            peer.name = name;
            peer.port = listen_port;
            peer.project_id = discovered_project;
            peer.role = role == 1 ? SessionRole::Host :
                        role == 2 ? SessionRole::Client : SessionRole::None;
            peer.stable_id = name + "@" + ip + ":" + std::to_string(listen_port);
            peer.last_seen_ms = now;
            impl_->discovered_peers[peer.stable_id] = std::move(peer);
        });
    if (impl_->discovery.start(
            Network::kDefaultPort + Network::kDiscoveryPortOffset,
            instance_name, project_id, 0, 0)) {
        return true;
    }
    return false;
}

std::vector<NetworkSystem::DiscoveredPeer> NetworkSystem::discovered_peers() const {
    const auto now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Impl::Clock::now().time_since_epoch()).count());
    std::vector<DiscoveredPeer> result;
    for (const auto& [key, peer] : impl_->discovered_peers) {
        if (now >= peer.last_seen_ms && now - peer.last_seen_ms <= 5000) {
            result.push_back(peer);
        }
    }
    return result;
}

void NetworkSystem::clear_discovered_peers() {
    impl_->discovered_peers.clear();
}

bool NetworkSystem::is_connected_host_peer(
    const Network::PeerManager::PeerInfo& info) const {
    if (impl_->host_address.empty() || impl_->host_port == 0) return false;
    return peer_matches_endpoint(info, impl_->host_address, impl_->host_port);
}

bool NetworkSystem::connect_to_peer(const std::string& ip, uint16_t port,
                                    const std::string& peer_name) {
    if (impl_->session_state != SessionState::Active) {
        CFW_LOG_WARNING("NetworkSystem: Cannot connect — session not active");
        return false;
    }
    if (ip.empty()) {
        CFW_LOG_WARNING("NetworkSystem: Cannot connect — host IP is empty");
        return false;
    }
    impl_->peer_manager.connect_to_peer(ip, port, peer_name, /*force=*/true);
    impl_->session_role = SessionRole::Client;
    impl_->host_address = ip;
    impl_->host_port = port;
    return true;
}

Network::CollaborationResult NetworkSystem::lock_object(const std::string& object_id,
                                                          const std::string& user_id,
                                                          const std::string& operation,
                                                          uint64_t now_ms_value) {
    return impl_->collaboration.lock_object(object_id, user_id, operation, now_ms_value);
}

Network::CollaborationResult NetworkSystem::unlock_object(const std::string& object_id,
                                                            const std::string& user_id) {
    return impl_->collaboration.unlock_object(object_id, user_id);
}

std::string NetworkSystem::locked_by(const std::string& object_id,
                                             uint64_t now_ms_value) {
    return impl_->collaboration.locked_by(object_id, now_ms_value);
}

void NetworkSystem::broadcast_intent(const std::string& user_id,
                                             const std::string& tooltip,
                                             const std::array<float, 3>& position,
                                             const std::string& status,
                                             uint64_t now_ms_value) {
    impl_->collaboration.broadcast_intent(user_id, tooltip, position, status, now_ms_value);
}

std::string NetworkSystem::check_preview_collision(
    const std::string& user_id,
    const std::array<float, 3>& position,
    float delta,
    uint64_t now_ms_value) {
    return impl_->collaboration.check_preview_collision(user_id, position, delta, now_ms_value);
}

void NetworkSystem::broadcast_actor_create(const std::string& actor_guid,
                                           const std::string& scene_name,
                                           const std::string& model_path,
                                           const std::vector<std::string>& dependency_paths,
                                           const float* transform,
                                           const void* optics_packed, size_t optics_size,
                                           const std::string& actor_json) {
    if (impl_->session_state != SessionState::Active) return;
    if (actor_guid.empty()) return;
    const auto logical_actor_json = Network::logical_actor_editor_json(actor_json);
    if (!logical_actor_json) {
        CFW_LOG_WARNING("NetworkSystem: Rejected malformed actor create JSON - actor='{}'",
                        actor_guid);
        return;
    }
    auto pkt = Network::build_actor_create(actor_guid, scene_name, model_path, transform,
                                            optics_packed, optics_size, dependency_paths,
                                            *logical_actor_json);
    if (pkt.empty()) return;
    std::vector<uint8_t> create_payload(pkt.begin() + 1, pkt.end());
    auto versioned = impl_->sync_engine.make_local_upsert(
        actor_guid, "actor.create", std::move(create_payload));
    auto versioned_packet = Network::build_editor_sync_operation(versioned);
    if (impl_->peer_manager.peer_count() > 0 && !versioned_packet.empty()) {
        impl_->peer_manager.broadcast(Network::kChannelReliable,
                                      versioned_packet.data(), versioned_packet.size(), true);
    }
    CFW_LOG_INFO("NetworkSystem: Broadcast actor create — actor='{}' scene='{}' model='{}' deps={}",
                 actor_guid, scene_name, model_path, dependency_paths.size());
}

void NetworkSystem::broadcast_actor_transform_update(const std::string& actor_guid,
                                                     const std::string& scene_name,
                                                     const float* transform,
                                                     const std::string& source_user_id,
                                                     const std::string& correlation_id) {
    if (impl_->session_state != SessionState::Active) return;
    if (actor_guid.empty() || transform == nullptr) return;
    auto pkt = Network::build_actor_transform_update(
        actor_guid, scene_name, transform, source_user_id, correlation_id);
    if (pkt.empty()) return;
    std::vector<uint8_t> transform_payload(pkt.begin() + 1, pkt.end());
    auto versioned = impl_->sync_engine.make_local_upsert(
        actor_guid, "actor.transform", std::move(transform_payload));
    auto versioned_packet = Network::build_editor_sync_operation(versioned);
    if (impl_->peer_manager.peer_count() > 0 && !versioned_packet.empty()) {
        impl_->peer_manager.broadcast(Network::kChannelReliable,
                                      versioned_packet.data(), versioned_packet.size(), true);
    }
    CFW_LOG_INFO("NetworkSystem: Broadcast actor transform — actor='{}' scene='{}' corr='{}'",
                 actor_guid, scene_name, correlation_id);
}

void NetworkSystem::broadcast_actor_delete(const std::string& actor_guid,
                                           const std::string& scene_name,
                                           const std::string& actor_name) {
    if (impl_->session_state != SessionState::Active) return;
    if (actor_guid.empty()) return;
    auto lww_delete = impl_->sync_engine.make_local_delete(actor_guid);
    auto lww_packet = Network::build_editor_sync_operation(lww_delete);
    if (impl_->peer_manager.peer_count() > 0 && !lww_packet.empty()) {
        impl_->peer_manager.broadcast(Network::kChannelReliable,
                                      lww_packet.data(), lww_packet.size(), true);
    }
    CFW_LOG_INFO("NetworkSystem: Broadcast actor delete — actor='{}' name='{}' scene='{}'",
                 actor_guid, actor_name, scene_name);
}

void NetworkSystem::request_actor_scene_snapshot(const std::string& scene_name) {
    if (impl_->session_state != SessionState::Active) return;
    if (scene_name.empty()) return;
    if (impl_->peer_manager.peer_count() == 0) {
        CFW_LOG_DEBUG("NetworkSystem: No peers — skipping actor scene snapshot request");
        return;
    }
    auto pkt = Network::build_actor_scene_snapshot_request(scene_name);
    impl_->peer_manager.broadcast(Network::kChannelReliable, pkt.data(), pkt.size(), true);
    CFW_LOG_INFO("NetworkSystem: Requested actor scene snapshot — scene='{}'", scene_name);
}

void NetworkSystem::broadcast_actor_scene_snapshot(const std::string& scene_name,
                                                   const std::string& snapshot_json) {
    if (impl_->session_state != SessionState::Active) return;
    if (scene_name.empty()) return;
    if (impl_->peer_manager.peer_count() == 0) {
        CFW_LOG_DEBUG("NetworkSystem: No peers — skipping actor scene snapshot broadcast");
        return;
    }
    auto pkt = Network::build_actor_scene_snapshot(scene_name, snapshot_json);
    impl_->peer_manager.broadcast(Network::kChannelReliable, pkt.data(), pkt.size(), true);
    CFW_LOG_INFO("NetworkSystem: Broadcast actor scene snapshot — scene='{}' bytes={}",
                 scene_name, snapshot_json.size());
}

void NetworkSystem::broadcast_actor_state_update(const std::string& actor_guid,
                                                 const std::string& scene_name,
                                                 const std::string& actor_json) {
    if (impl_->session_state != SessionState::Active) return;
    if (actor_guid.empty() || scene_name.empty()) return;
    const auto logical_actor_json = Network::logical_actor_editor_json(actor_json);
    if (!logical_actor_json) {
        CFW_LOG_WARNING("NetworkSystem: Rejected malformed actor state JSON - actor='{}'",
                        actor_guid);
        return;
    }
    auto pkt = Network::build_actor_state_update(actor_guid, scene_name, *logical_actor_json);
    if (pkt.empty()) return;
    std::vector<uint8_t> state_payload(pkt.begin() + 1, pkt.end());
    auto versioned = impl_->sync_engine.make_local_upsert(
        actor_guid, "actor.state", std::move(state_payload));
    auto versioned_packet = Network::build_editor_sync_operation(versioned);
    if (impl_->peer_manager.peer_count() > 0 && !versioned_packet.empty()) {
        impl_->peer_manager.broadcast(Network::kChannelReliable,
                                      versioned_packet.data(), versioned_packet.size(), true);
    }
    CFW_LOG_INFO("NetworkSystem: Broadcast actor state update — actor='{}' scene='{}' bytes={}",
                 actor_guid, scene_name, actor_json.size());
}

bool NetworkSystem::has_pending_transfers() const {
    std::lock_guard lock(impl_->pending_mutex);
    return !impl_->pending_actor_creates.empty() ||
           !impl_->incoming_transfers.empty() ||
           !impl_->pending_file_transfer_groups.empty();
}

void NetworkSystem::set_sync_paused(bool paused) {
    impl_->sync_paused = paused;
    if (!paused) {
        impl_->sync_engine.retry_pending_operations();
    }
}

bool NetworkSystem::pop_pending_actor_create(std::string& actor_guid,
                                              std::string& scene_name,
                                              std::string& model_path,
                                              void* actor_packed_out, size_t packed_size,
                                              std::string* actor_json_out) {
    if (!peek_pending_actor_create(actor_guid, scene_name, model_path,
                                   actor_packed_out, packed_size, actor_json_out)) {
        return false;
    }
    return ack_pending_actor_create(actor_guid);
}

bool NetworkSystem::peek_pending_actor_create(std::string& actor_guid,
                                               std::string& scene_name,
                                               std::string& model_path,
                                               void* actor_packed_out,
                                               size_t packed_size,
                                               std::string* actor_json_out) const {
    std::lock_guard lock(impl_->pending_mutex);
    if (impl_->pending_actor_creates.empty()) return false;
    const auto& pa = impl_->pending_actor_creates.front();
    actor_guid = pa.actor_guid;
    scene_name = pa.scene_name;
    model_path = pa.model_path;
    if (actor_packed_out && packed_size <= sizeof(Network::ActorCreatePacked)) {
        std::memcpy(actor_packed_out, &pa.actor_packed, packed_size);
    }
    if (actor_json_out) {
        *actor_json_out = pa.actor_json;
    }
    return true;
}

bool NetworkSystem::ack_pending_actor_create(const std::string& actor_guid) {
    std::lock_guard lock(impl_->pending_mutex);
    if (impl_->pending_actor_creates.empty() ||
        impl_->pending_actor_creates.front().actor_guid != actor_guid) return false;
    impl_->pending_actor_creates.erase(impl_->pending_actor_creates.begin());
    return true;
}

bool NetworkSystem::process_pending_actor_create(
    const PendingActorCreateProcessor& processor) {
    if (!processor) return false;
    std::lock_guard lock(impl_->pending_mutex);
    if (impl_->pending_actor_creates.empty()) return false;
    const auto& pending = impl_->pending_actor_creates.front();
    bool consume = false;
    try {
        consume = processor(pending.actor_guid, pending.scene_name, pending.model_path,
                            pending.actor_packed, pending.actor_json);
    } catch (...) {
        return true;
    }
    if (consume) impl_->pending_actor_creates.erase(impl_->pending_actor_creates.begin());
    return true;
}

bool NetworkSystem::pop_pending_actor_transform_update(std::string& actor_guid,
                                                       std::string& scene_name,
                                                       float* transform_out,
                                                       size_t transform_count,
                                                       std::string& source_user_id,
                                                       std::string& correlation_id) {
    if (!peek_pending_actor_transform_update(actor_guid, scene_name, transform_out,
                                             transform_count, source_user_id,
                                             correlation_id)) return false;
    return ack_pending_actor_transform_update(actor_guid);
}

bool NetworkSystem::peek_pending_actor_transform_update(
    std::string& actor_guid, std::string& scene_name,
    float* transform_out, size_t transform_count,
    std::string& source_user_id, std::string& correlation_id) const {
    std::lock_guard lock(impl_->pending_mutex);
    if (impl_->pending_actor_transform_updates.empty()) return false;
    const auto& update = impl_->pending_actor_transform_updates.front();
    actor_guid = update.actor_guid;
    scene_name = update.scene_name;
    source_user_id = update.source_user_id;
    correlation_id = update.correlation_id;
    if (transform_out && transform_count >= 9) {
        std::memcpy(transform_out, update.transform, sizeof(update.transform));
    }
    return true;
}

bool NetworkSystem::ack_pending_actor_transform_update(const std::string& actor_guid) {
    std::lock_guard lock(impl_->pending_mutex);
    if (impl_->pending_actor_transform_updates.empty() ||
        impl_->pending_actor_transform_updates.front().actor_guid != actor_guid) return false;
    impl_->pending_actor_transform_updates.erase(
        impl_->pending_actor_transform_updates.begin());
    return true;
}

bool NetworkSystem::process_pending_actor_transform_update(
    const PendingActorTransformProcessor& processor) {
    if (!processor) return false;
    std::lock_guard lock(impl_->pending_mutex);
    if (impl_->pending_actor_transform_updates.empty()) return false;
    const auto& pending = impl_->pending_actor_transform_updates.front();
    bool consume = false;
    try {
        consume = processor(pending.actor_guid, pending.scene_name, pending.transform,
                            pending.source_user_id, pending.correlation_id);
    } catch (...) {
        return true;
    }
    if (consume) {
        impl_->pending_actor_transform_updates.erase(
            impl_->pending_actor_transform_updates.begin());
    }
    return true;
}

bool NetworkSystem::pop_pending_actor_delete(std::string& actor_guid,
                                             std::string& scene_name,
                                             std::string& actor_name) {
    if (!peek_pending_actor_delete(actor_guid, scene_name, actor_name)) return false;
    return ack_pending_actor_delete(actor_guid);
}

bool NetworkSystem::peek_pending_actor_delete(std::string& actor_guid,
                                               std::string& scene_name,
                                               std::string& actor_name) const {
    std::lock_guard lock(impl_->pending_mutex);
    if (impl_->pending_actor_deletes.empty()) return false;
    const auto& pending = impl_->pending_actor_deletes.front();
    actor_guid = pending.actor_guid;
    scene_name = pending.scene_name;
    actor_name = pending.actor_name;
    return true;
}

bool NetworkSystem::ack_pending_actor_delete(const std::string& actor_guid) {
    std::lock_guard lock(impl_->pending_mutex);
    if (impl_->pending_actor_deletes.empty() ||
        impl_->pending_actor_deletes.front().actor_guid != actor_guid) return false;
    impl_->pending_actor_deletes.erase(impl_->pending_actor_deletes.begin());
    return true;
}

bool NetworkSystem::process_pending_actor_delete(
    const PendingActorDeleteProcessor& processor) {
    if (!processor) return false;
    std::lock_guard lock(impl_->pending_mutex);
    if (impl_->pending_actor_deletes.empty()) return false;
    const auto& pending = impl_->pending_actor_deletes.front();
    bool consume = false;
    try {
        consume = processor(pending.actor_guid, pending.scene_name, pending.actor_name);
    } catch (...) {
        return true;
    }
    if (consume) impl_->pending_actor_deletes.erase(impl_->pending_actor_deletes.begin());
    return true;
}

bool NetworkSystem::pop_pending_actor_scene_snapshot_request(std::string& scene_name) {
    std::lock_guard lock(impl_->pending_mutex);
    if (impl_->pending_actor_scene_snapshot_requests.empty()) return false;
    auto& pending = impl_->pending_actor_scene_snapshot_requests.front();
    scene_name = pending.scene_name;
    impl_->pending_actor_scene_snapshot_requests.erase(
        impl_->pending_actor_scene_snapshot_requests.begin());
    return true;
}

bool NetworkSystem::pop_pending_actor_scene_snapshot(std::string& scene_name,
                                                     std::string& snapshot_json) {
    std::lock_guard lock(impl_->pending_mutex);
    if (impl_->pending_actor_scene_snapshots.empty()) return false;
    auto& pending = impl_->pending_actor_scene_snapshots.front();
    scene_name = pending.scene_name;
    snapshot_json = pending.snapshot_json;
    impl_->pending_actor_scene_snapshots.erase(
        impl_->pending_actor_scene_snapshots.begin());
    return true;
}

bool NetworkSystem::pop_pending_actor_state_update(std::string& actor_guid,
                                                   std::string& scene_name,
                                                   std::string& actor_json) {
    if (!peek_pending_actor_state_update(actor_guid, scene_name, actor_json)) return false;
    return ack_pending_actor_state_update(actor_guid);
}

bool NetworkSystem::peek_pending_actor_state_update(std::string& actor_guid,
                                                     std::string& scene_name,
                                                     std::string& actor_json) const {
    std::lock_guard lock(impl_->pending_mutex);
    if (impl_->pending_actor_state_updates.empty()) return false;
    const auto& pending = impl_->pending_actor_state_updates.front();
    actor_guid = pending.actor_guid;
    scene_name = pending.scene_name;
    actor_json = pending.actor_json;
    return true;
}

bool NetworkSystem::ack_pending_actor_state_update(const std::string& actor_guid) {
    std::lock_guard lock(impl_->pending_mutex);
    if (impl_->pending_actor_state_updates.empty() ||
        impl_->pending_actor_state_updates.front().actor_guid != actor_guid) return false;
    impl_->pending_actor_state_updates.erase(impl_->pending_actor_state_updates.begin());
    return true;
}

bool NetworkSystem::process_pending_actor_state_update(
    const PendingActorStateProcessor& processor) {
    if (!processor) return false;
    std::lock_guard lock(impl_->pending_mutex);
    if (impl_->pending_actor_state_updates.empty()) return false;
    const auto& pending = impl_->pending_actor_state_updates.front();
    bool consume = false;
    try {
        consume = processor(pending.actor_guid, pending.scene_name, pending.actor_json);
    } catch (...) {
        return true;
    }
    if (consume) impl_->pending_actor_state_updates.erase(impl_->pending_actor_state_updates.begin());
    return true;
}

bool NetworkSystem::register_actor_identity(const std::string& actor_guid,
                                            std::uintptr_t actor_handle,
                                            bool locally_owned) {
    const bool ok = impl_->identity_registry.register_actor(
        actor_guid, actor_handle, locally_owned);
    if (ok) {
        CFW_LOG_INFO("NetworkSystem: Registered actor identity — actor='{}' handle={} owner={}",
                     actor_guid, actor_handle, locally_owned ? "local" : "remote");
    } else {
        CFW_LOG_WARNING("NetworkSystem: Failed to register actor identity — actor='{}' handle={}",
                        actor_guid, actor_handle);
    }
    return ok;
}

std::optional<Network::ActorNetworkIdentity> NetworkSystem::resolve_actor_identity(
    const std::string& actor_guid) const {
    return impl_->identity_registry.resolve_actor(actor_guid);
}

bool NetworkSystem::claim_actor_ownership(const std::string& actor_guid) {
    if (actor_guid.empty()) return false;
    impl_->identity_registry.set_actor_ownership(actor_guid, true);
    if (impl_->session_state == SessionState::Active && impl_->peer_manager.peer_count() > 0) {
        auto pkt = Network::build_ownership_claim(actor_guid);
        impl_->peer_manager.broadcast(Network::kChannelReliable, pkt.data(), pkt.size(), true);
    }
    CFW_LOG_INFO("NetworkSystem: Claimed actor ownership — actor='{}'", actor_guid);
    return true;
}

void NetworkSystem::set_project_root(const std::string& project_root) {
    std::lock_guard lock(impl_->pending_mutex);
    if (impl_->project_root != project_root) {
        impl_->incoming_transfers.clear();
        impl_->pending_actor_creates.clear();
        impl_->pending_actor_transform_updates.clear();
        impl_->pending_actor_deletes.clear();
        impl_->pending_actor_scene_snapshot_requests.clear();
        impl_->pending_actor_scene_snapshots.clear();
        impl_->pending_actor_state_updates.clear();
        impl_->pending_file_transfer_groups.clear();
        impl_->transfer_to_group.clear();
        impl_->asset_to_transfer_group.clear();
        impl_->received_asset_cache.clear();
    }
    impl_->project_root = project_root;
}

// ============================================================================
// Callbacks
// ============================================================================

void NetworkSystem::on_peer_connected(const Network::PeerManager::PeerInfo& info) {
    CFW_LOG_INFO("NetworkSystem: Peer connected — {} ({})", info.id, info.name);

    // Snapshot is idempotent and versioned, and is sent only to this peer.
    impl_->sync_engine.sync_full_to(info.id);
    auto request = impl_->sync_engine.make_snapshot_request();
    impl_->peer_manager.send_to_peer_id(info.id, Network::kChannelReliable,
                                        request.data(), request.size(), true);

    if (impl_->ctx && impl_->ctx->event_bus()) {
        Events::PeerConnectedEvent ev{info.id, info.name};
        impl_->ctx->event_bus()->publish(ev);
    }
}

void NetworkSystem::on_peer_disconnected(const Network::PeerManager::PeerInfo& info) {
    CFW_LOG_INFO("NetworkSystem: Peer disconnected — {} ({})", info.id, info.name);

    if (impl_->session_role == SessionRole::Client &&
        is_connected_host_peer(info)) {
        impl_->collaboration.clear();
        impl_->stop_session_after_peer_disconnect = true;
    }

    if (impl_->ctx && impl_->ctx->event_bus()) {
        Events::PeerDisconnectedEvent ev{info.id};
        impl_->ctx->event_bus()->publish(ev);
    }
}

void NetworkSystem::on_data_received(const std::string& peer_id,
                                     const uint8_t* data, size_t len) {
    std::string authenticated_sender = peer_id;
    if (len >= 1 &&
        static_cast<Network::MessageType>(data[0]) ==
            Network::MessageType::EDITOR_SYNC) {
        authenticated_sender = impl_->peer_manager.writer_peer_id(peer_id);
        if (authenticated_sender.empty()) return;
    }
    impl_->sync_engine.handle_incoming(authenticated_sender, data, len);

    if (impl_->ctx && impl_->ctx->event_bus()) {
        Events::RemoteSyncReceivedEvent ev{peer_id};
        impl_->ctx->event_bus()->publish(ev);
    }
}

void NetworkSystem::on_custom_message(const std::string& sender_peer_id,
                                        const uint8_t* data, size_t len) {
    std::lock_guard pending_lock(impl_->pending_mutex);
    if (len < 1) return;
    using Network::MessageType;
    auto mt = static_cast<MessageType>(data[0]);

    if (mt == MessageType::ACTOR_CREATE) {
        Network::BufferReader r(data + 1, len - 1);
        if (!r.has_remaining(2)) return;
        uint16_t guid_len = r.read_u16();
        if (!r.has_remaining(guid_len)) return;
        std::string actor_guid = r.read_string(guid_len);
        if (!impl_->applying_versioned_actor_message && !actor_guid.empty()) {
            Network::EditorSyncOperation legacy_create;
            legacy_create.kind = Network::EditorSyncOperationKind::Upsert;
            legacy_create.actor_guid = actor_guid;
            legacy_create.field_name = "actor.create";
            legacy_create.value.assign(data + 1, data + len);
            legacy_create.version = {0, sender_peer_id};
            (void)impl_->sync_engine.apply_editor_operation(legacy_create);
            return;
        }
        uint16_t sn_len = r.read_u16();
        std::string scene_name = r.read_string(sn_len);
        uint16_t mp_len = r.read_u16();
        std::string model_path = r.read_string(mp_len);

        if (r.has_remaining(36 + sizeof(Network::ActorCreatePacked))) {
            const float* transform = reinterpret_cast<const float*>(r.data + r.pos);
            r.pos += 36;
            Network::ActorCreatePacked actor_packed{};
            std::memcpy(&actor_packed, r.data + r.pos, sizeof(actor_packed));
            std::memcpy(actor_packed.transform, transform, 36);
            r.pos += sizeof(Network::ActorCreatePacked);

            std::vector<std::string> dependency_paths;
            if (r.has_remaining(2)) {
                uint16_t dep_count = r.read_u16();
                for (uint16_t i = 0; i < dep_count; ++i) {
                    if (!r.has_remaining(2)) break;
                    uint16_t dep_len = r.read_u16();
                    if (!r.has_remaining(dep_len)) break;
                    dependency_paths.push_back(r.read_string(dep_len));
                }
            }
            std::string actor_json;
            if (r.has_remaining(4)) {
                uint32_t json_len = r.read_u32();
                if (r.has_remaining(json_len)) {
                    actor_json = r.read_string(json_len);
                }
            }

            CFW_LOG_INFO("NetworkSystem: Received ACTOR_CREATE from {} — actor='{}' scene='{}' model='{}' deps={}",
                         sender_peer_id, actor_guid, scene_name, model_path, dependency_paths.size());

            const std::string asset_id = actor_asset_id(actor_json);

            if (!actor_guid.empty() && impl_->identity_registry.resolve_actor(actor_guid)) {
                CFW_LOG_DEBUG(
                    "NetworkSystem: Ignoring duplicate ACTOR_CREATE for registered actor='{}' scene='{}'",
                    actor_guid, scene_name);
                return;
            }

            auto pending_file_transfer_for_actor = [&]() {
                for (auto& [_, group] : impl_->pending_file_transfer_groups) {
                    for (auto& action : group.actor_actions) {
                        if (action.actor_guid == actor_guid &&
                            action.scene_name == scene_name) {
                            action.actor_json = actor_json;
                            action.actor_packed = actor_packed;
                            group.last_activity_time = Impl::Clock::now();
                            return true;
                        }
                    }
                }
                return false;
            };
            if (pending_file_transfer_for_actor()) {
                CFW_LOG_DEBUG(
                    "NetworkSystem: Coalesced duplicate ACTOR_CREATE while files are pending — actor='{}' scene='{}'",
                    actor_guid, scene_name);
                return;
            }


            auto upsert_pending_actor_create = [&](Impl::PendingAction action) {
                auto it = std::find_if(
                    impl_->pending_actor_creates.begin(),
                    impl_->pending_actor_creates.end(),
                    [&](const Impl::PendingAction& existing) {
                        return existing.actor_guid == action.actor_guid &&
                               existing.scene_name == action.scene_name &&
                               existing.model_path == action.model_path;
                    });
                if (it != impl_->pending_actor_creates.end()) {
                    *it = std::move(action);
                    return;
                }
                impl_->pending_actor_creates.push_back(std::move(action));
            };

            auto file_exists = [this](const std::string& path) {
                auto full_path = Network::resolve_project_relative_path(
                    impl_->project_root, path);
                return full_path && std::filesystem::exists(*full_path) &&
                       std::filesystem::is_regular_file(*full_path);
            };

            auto make_pending_action = [&](const std::string& resolved_model_path,
                                           const std::vector<std::string>& resolved_dependencies) {
                Impl::PendingAction action;
                action.actor_guid = actor_guid;
                action.scene_name = scene_name;
                action.model_path = resolved_model_path;
                action.dependency_paths = resolved_dependencies;
                action.actor_json = actor_json;
                action.actor_packed = actor_packed;
                return action;
            };

            if (!asset_id.empty()) {
                auto cached = impl_->received_asset_cache.find(asset_id);
                if (cached != impl_->received_asset_cache.end()) {
                    if (file_exists(cached->second.model_path)) {
                        upsert_pending_actor_create(make_pending_action(
                            cached->second.model_path, cached->second.dependency_paths));
                        CFW_LOG_INFO(
                            "NetworkSystem: Reused received asset — asset='{}' actor='{}' scene='{}'",
                            asset_id, actor_guid, scene_name);
                        return;
                    }
                    impl_->received_asset_cache.erase(cached);
                }

                auto pending_group = impl_->asset_to_transfer_group.find(asset_id);
                if (pending_group != impl_->asset_to_transfer_group.end()) {
                    auto group = impl_->pending_file_transfer_groups.find(pending_group->second);
                    if (group != impl_->pending_file_transfer_groups.end()) {
                        group->second.actor_actions.push_back(
                            make_pending_action(group->second.model_path,
                                                group->second.dependency_paths));
                        group->second.last_activity_time = Impl::Clock::now();
                        CFW_LOG_INFO(
                            "NetworkSystem: Coalesced actor onto asset transfer — asset='{}' actor='{}' waiters={}",
                            asset_id, actor_guid, group->second.actor_actions.size());
                        return;
                    }
                    impl_->asset_to_transfer_group.erase(pending_group);
                }
            }

            std::vector<std::string> missing_paths;
            if (!file_exists(model_path)) {
                missing_paths.push_back(model_path);
            }
            for (const auto& dep : dependency_paths) {
                if (!file_exists(dep)) {
                    missing_paths.push_back(dep);
                }
            }

            if (missing_paths.empty()) {
                upsert_pending_actor_create(make_pending_action(model_path, dependency_paths));
                if (!asset_id.empty()) {
                    impl_->received_asset_cache[asset_id] = {model_path, dependency_paths};
                }
            } else {
                uint64_t group_id = impl_->next_transfer_id++;
                Impl::PendingFileTransfer group;
                group.asset_id = asset_id;
                group.model_path = model_path;
                group.dependency_paths = dependency_paths;
                group.actor_actions.push_back(make_pending_action(model_path, dependency_paths));
                group.remaining_files = static_cast<uint32_t>(missing_paths.size());
                group.create_time = Impl::Clock::now();
                group.last_activity_time = group.create_time;

                auto send_request = [&](uint64_t transfer_id, const std::string& path) {
                    auto pkt = Network::build_file_request(transfer_id, path);
                    const auto* peer_info = impl_->peer_manager.find_peer(sender_peer_id);
                    if (peer_info && peer_info->peer) {
                        CFW_LOG_INFO(
                            "NetworkSystem: Requesting missing file from {} — id={} path='{}'",
                            sender_peer_id, transfer_id, path);
                        impl_->peer_manager.send_to(peer_info->peer, Network::kChannelReliable,
                                                    pkt.data(), pkt.size(), true);
                    } else {
                        CFW_LOG_WARNING(
                            "NetworkSystem: FILE_REQUEST peer {} unavailable, broadcasting — id={} path='{}'",
                            sender_peer_id, transfer_id, path);
                        impl_->peer_manager.broadcast(Network::kChannelReliable,
                                                      pkt.data(), pkt.size(), true);
                    }
                };

                std::vector<std::pair<uint64_t, std::string>> requests;
                for (const auto& path : missing_paths) {
                    uint64_t transfer_id = impl_->next_transfer_id++;
                    group.transfer_ids.push_back(transfer_id);
                    impl_->transfer_to_group[transfer_id] = group_id;
                    requests.emplace_back(transfer_id, path);
                }
                impl_->pending_file_transfer_groups[group_id] = std::move(group);
                if (!asset_id.empty()) {
                    impl_->asset_to_transfer_group[asset_id] = group_id;
                }
                for (const auto& [transfer_id, path] : requests) {
                    send_request(transfer_id, path);
                }
            }
        }
    } else if (mt == MessageType::ACTOR_TRANSFORM_UPDATE) {
        Network::BufferReader r(data + 1, len - 1);
        if (!r.has_remaining(2)) return;
        uint16_t guid_len = r.read_u16();
        if (!r.has_remaining(guid_len + 2)) return;
        std::string actor_guid = r.read_string(guid_len);
        if (!impl_->applying_versioned_actor_message && !actor_guid.empty()) {
            Network::EditorSyncOperation legacy_transform;
            legacy_transform.kind = Network::EditorSyncOperationKind::Upsert;
            legacy_transform.actor_guid = actor_guid;
            legacy_transform.field_name = "actor.transform";
            legacy_transform.value.assign(data + 1, data + len);
            legacy_transform.version = {0, sender_peer_id};
            (void)impl_->sync_engine.apply_editor_operation(legacy_transform);
            return;
        }
        uint16_t scene_len = r.read_u16();
        if (!r.has_remaining(scene_len + 36)) return;
        std::string scene_name = r.read_string(scene_len);

        Impl::PendingTransformUpdate update;
        update.actor_guid = actor_guid;
        update.scene_name = scene_name;
        std::memcpy(update.transform, r.data + r.pos, sizeof(update.transform));
        r.pos += sizeof(update.transform);
        if (r.has_remaining(2)) {
            uint16_t source_len = r.read_u16();
            if (r.has_remaining(source_len)) {
                update.source_user_id = r.read_string(source_len);
            }
        }
        if (r.has_remaining(2)) {
            uint16_t corr_len = r.read_u16();
            if (r.has_remaining(corr_len)) {
                update.correlation_id = r.read_string(corr_len);
            }
        }
        impl_->pending_actor_transform_updates.push_back(update);
        CFW_LOG_INFO("NetworkSystem: Received ACTOR_TRANSFORM_UPDATE from {} — actor='{}' scene='{}' corr='{}'",
                     sender_peer_id, actor_guid, scene_name, update.correlation_id);
    } else if (mt == MessageType::ACTOR_DELETE) {
        Network::BufferReader r(data + 1, len - 1);
        if (!r.has_remaining(2)) return;
        uint16_t guid_len = r.read_u16();
        if (!r.has_remaining(guid_len + 2)) return;
        Impl::PendingActorDelete pending;
        pending.actor_guid = r.read_string(guid_len);
        uint16_t scene_len = r.read_u16();
        if (!r.has_remaining(scene_len + 2)) return;
        pending.scene_name = r.read_string(scene_len);
        uint16_t name_len = r.read_u16();
        if (!r.has_remaining(name_len)) return;
        pending.actor_name = r.read_string(name_len);
        if (!pending.actor_guid.empty()) {
            Network::EditorSyncOperation legacy_delete;
            legacy_delete.kind = Network::EditorSyncOperationKind::Delete;
            legacy_delete.actor_guid = pending.actor_guid;
            legacy_delete.version = {0, sender_peer_id};
            (void)impl_->sync_engine.apply_editor_operation(legacy_delete);
            return;
        }
        CFW_LOG_INFO(
            "NetworkSystem: Received ACTOR_DELETE from {} — actor='{}' name='{}' scene='{}'",
            sender_peer_id, pending.actor_guid, pending.actor_name, pending.scene_name);
    } else if (mt == MessageType::ACTOR_SCENE_SNAPSHOT_REQUEST) {
        if (impl_->session_role != SessionRole::Host) return;
        Network::BufferReader r(data + 1, len - 1);
        if (!r.has_remaining(2)) return;
        uint16_t scene_len = r.read_u16();
        if (!r.has_remaining(scene_len)) return;
        Impl::PendingActorSceneSnapshotRequest pending;
        pending.scene_name = r.read_string(scene_len);
        impl_->pending_actor_scene_snapshot_requests.push_back(pending);
        CFW_LOG_INFO(
            "NetworkSystem: Received ACTOR_SCENE_SNAPSHOT_REQUEST from {} — scene='{}'",
            sender_peer_id, pending.scene_name);
    } else if (mt == MessageType::ACTOR_SCENE_SNAPSHOT) {
        Network::BufferReader r(data + 1, len - 1);
        if (!r.has_remaining(2)) return;
        uint16_t scene_len = r.read_u16();
        if (!r.has_remaining(scene_len + 4)) return;
        Impl::PendingActorSceneSnapshot pending;
        pending.scene_name = r.read_string(scene_len);
        uint32_t json_len = r.read_u32();
        if (!r.has_remaining(json_len)) return;
        pending.snapshot_json = r.read_string(json_len);
        const std::string pending_scene_name = pending.scene_name;
        const size_t pending_snapshot_size = pending.snapshot_json.size();
        try {
            const auto snapshot = nlohmann::json::parse(pending.snapshot_json);

            const std::string snapshot_kind =
                snapshot.value("snapshot_kind", std::string{"host_snapshot"});
            if (Network::classify_scene_snapshot(snapshot_kind) ==
                Network::SceneSnapshotDisposition::PeerAcknowledgement) {

                CFW_LOG_INFO(
                    "NetworkSystem: Received peer scene snapshot acknowledgement from {} — scene='{}' bytes={}",
                    sender_peer_id, pending_scene_name, pending_snapshot_size);
                return;
            }

        } catch (const nlohmann::json::exception&) {
            CFW_LOG_WARNING(
                "NetworkSystem: Ignoring invalid actor scene snapshot metadata from {}",
                sender_peer_id);
        }
        CFW_LOG_INFO(
            "NetworkSystem: Received diagnostic ACTOR_SCENE_SNAPSHOT from {} — scene='{}' bytes={}",
            sender_peer_id, pending_scene_name, pending_snapshot_size);
    } else if (mt == MessageType::ACTOR_STATE_UPDATE) {
        Network::BufferReader r(data + 1, len - 1);
        if (!r.has_remaining(2)) return;
        uint16_t guid_len = r.read_u16();
        if (!r.has_remaining(guid_len + 2)) return;
        Impl::PendingActorStateUpdate pending;
        pending.actor_guid = r.read_string(guid_len);
        if (!impl_->applying_versioned_actor_message && !pending.actor_guid.empty()) {
            Network::EditorSyncOperation legacy_state;
            legacy_state.kind = Network::EditorSyncOperationKind::Upsert;
            legacy_state.actor_guid = pending.actor_guid;
            legacy_state.field_name = "actor.state";
            legacy_state.value.assign(data + 1, data + len);
            legacy_state.version = {0, sender_peer_id};
            (void)impl_->sync_engine.apply_editor_operation(legacy_state);
            return;
        }
        uint16_t scene_len = r.read_u16();
        if (!r.has_remaining(scene_len + 4)) return;
        pending.scene_name = r.read_string(scene_len);
        uint32_t json_len = r.read_u32();
        if (!r.has_remaining(json_len)) return;
        pending.actor_json = r.read_string(json_len);
        impl_->pending_actor_state_updates.push_back(pending);
        CFW_LOG_INFO(
            "NetworkSystem: Received ACTOR_STATE_UPDATE from {} — actor='{}' scene='{}' bytes={}",
            sender_peer_id, pending.actor_guid, pending.scene_name, pending.actor_json.size());
    } else if (mt == MessageType::FILE_REQUEST) {
        handle_file_request(sender_peer_id, data, len);
    } else if (mt == MessageType::FILE_CHUNK) {
        handle_file_chunk(sender_peer_id, data, len);
    } else if (mt == MessageType::OWNERSHIP_CLAIM) {
        Network::BufferReader r(data + 1, len - 1);
        if (!r.has_remaining(2)) return;
        uint16_t guid_len = r.read_u16();
        if (!r.has_remaining(guid_len)) return;
        std::string actor_guid = r.read_string(guid_len);
        impl_->identity_registry.set_actor_ownership(actor_guid, false);
        CFW_LOG_INFO("NetworkSystem: Peer {} claimed actor ownership — actor='{}'",
                     sender_peer_id, actor_guid);

    }
}

void NetworkSystem::handle_file_request(const std::string& sender_peer_id,
                                        const uint8_t* data, size_t len) {
    if (len < 1 + 8 + 2) return;
    Network::BufferReader r(data + 1, len - 1);
    uint64_t transfer_id = r.read_u64();
    uint16_t mp_len = r.read_u16();
    if (!r.has_remaining(mp_len)) return;
    std::string model_path = r.read_string(mp_len);

    auto full_path = Network::resolve_project_relative_path(
        impl_->project_root, model_path);
    if (!full_path) {
        CFW_LOG_ERROR("NetworkSystem: Reject unsafe FILE_REQUEST path '{}' id={} from {}",
                      model_path, transfer_id, sender_peer_id);
        return;
    }

    CFW_LOG_INFO("NetworkSystem: Received FILE_REQUEST from {} — id={} path='{}'",
                 sender_peer_id, transfer_id, model_path);

    auto& cache = impl_->outgoing_cache[model_path];
    if (cache.data.empty()) {
        std::ifstream file(*full_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            CFW_LOG_ERROR("NetworkSystem: Cannot open file '{}' for FILE_REQUEST id={} from {}",
                          Utils::path_to_utf8(*full_path), transfer_id, sender_peer_id);
            impl_->outgoing_cache.erase(model_path);
            return;
        }
        cache.data.resize(static_cast<size_t>(file.tellg()));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(cache.data.data()), cache.data.size());
        cache.load_time = Impl::Clock::now();
    }

    if (cache.data.empty()) return;

    constexpr uint32_t kChunkSize = 512 * 1024; // 512KB
    uint32_t total_size = static_cast<uint32_t>(cache.data.size());
    uint32_t chunk_count = (total_size + kChunkSize - 1) / kChunkSize;

    // Find sender's peer
    const auto* peer_info = impl_->peer_manager.find_peer(sender_peer_id);
    if (!peer_info || !peer_info->peer) {
        CFW_LOG_ERROR("NetworkSystem: Cannot find peer {} for FILE_CHUNK send id={} path='{}'",
                      sender_peer_id, transfer_id, model_path);
        return;
    }

    CFW_LOG_INFO(
        "NetworkSystem: Sending FILE_CHUNK batch to {} — id={} path='{}' size={} chunks={}",
        sender_peer_id, transfer_id, model_path, total_size, chunk_count);

    for (uint32_t i = 0; i < chunk_count; ++i) {
        uint32_t offset = i * kChunkSize;
        uint32_t chunk_len = std::min(kChunkSize, total_size - offset);

        auto pkt = Network::build_file_chunk(
            transfer_id, model_path, total_size, offset, i, chunk_count,
            cache.data.data() + offset, chunk_len);

        impl_->peer_manager.send_to(peer_info->peer, Network::kChannelReliable,
                                    pkt.data(), pkt.size(), true);
    }
}

void NetworkSystem::handle_file_chunk(const std::string& sender_peer_id,
                                      const uint8_t* data, size_t len) {
    std::lock_guard pending_lock(impl_->pending_mutex);
    if (len < 1 + 8 + 2) return;
    Network::BufferReader r(data + 1, len - 1);

    uint64_t transfer_id = r.read_u64();
    uint16_t mp_len = r.read_u16();
    if (!r.has_remaining(mp_len)) return;
    std::string model_path = r.read_string(mp_len);

    if (!r.has_remaining(4 + 4 + 4 + 4 + 4)) return;
    uint32_t total_size = r.read_u32();
    uint32_t offset = r.read_u32();
    uint32_t chunk_index = r.read_u32();
    uint32_t chunk_count = r.read_u32();
    uint32_t chunk_data_len = r.read_u32();

    if (!r.has_remaining(chunk_data_len)) return;
    const uint8_t* chunk_data = r.data + r.pos;

    if (chunk_index >= chunk_count) return;

    if (chunk_index == 0) {
        CFW_LOG_INFO(
            "NetworkSystem: Receiving FILE_CHUNK batch from {} — id={} path='{}' size={} chunks={}",
            sender_peer_id, transfer_id, model_path, total_size, chunk_count);
    }

    // Get or create transfer state — isolate by sender+transfer_id to prevent
    // chunk interleaving when multiple peers respond to one FILE_REQUEST.
    std::string tx_key = Network::make_transfer_key(sender_peer_id, transfer_id);
    auto& tx = impl_->incoming_transfers[tx_key];
    if (tx.model_path.empty()) {
        tx.transfer_id = transfer_id;
        tx.model_path = model_path;
        tx.sender_peer_id = sender_peer_id;
        tx.total_size = total_size;
        tx.chunk_count = chunk_count;
        tx.received_chunks.resize(chunk_count, false);
        tx.buffer.resize(total_size);
        tx.last_chunk_time = Impl::Clock::now();
    }

    if (tx.model_path != model_path || tx.total_size != total_size ||
        tx.chunk_count != chunk_count) {
        CFW_LOG_ERROR("NetworkSystem: Inconsistent FILE_CHUNK transfer {}", transfer_id);
        impl_->incoming_transfers.erase(tx_key);
        return;
    }

    // Write chunk into buffer
    if (offset + chunk_data_len <= total_size) {
        std::memcpy(tx.buffer.data() + offset, chunk_data, chunk_data_len);
        tx.received_chunks[chunk_index] = true;
        tx.last_chunk_time = Impl::Clock::now();
        auto group_id_it = impl_->transfer_to_group.find(transfer_id);
        if (group_id_it != impl_->transfer_to_group.end()) {
            auto ft_it = impl_->pending_file_transfer_groups.find(group_id_it->second);
            if (ft_it != impl_->pending_file_transfer_groups.end()) {
                ft_it->second.last_activity_time = tx.last_chunk_time;
            }
        }
    }

    // Check if all chunks received
    bool all_received = true;
    for (bool rcvd : tx.received_chunks) {
        if (!rcvd) { all_received = false; break; }
    }

    if (!all_received) {
        return;
    }

    // All chunks received — write to disk
    auto dest = Network::resolve_project_relative_path(impl_->project_root, model_path);
    if (!dest) {
        CFW_LOG_ERROR("NetworkSystem: Reject unsafe FILE_CHUNK path '{}'", model_path);
        impl_->incoming_transfers.erase(tx_key);
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(dest->parent_path(), ec);

    auto tmp_dest = *dest;
    tmp_dest += ".part";
    std::ofstream out(tmp_dest, std::ios::binary);
    if (out.is_open()) {
        out.write(reinterpret_cast<const char*>(tx.buffer.data()), total_size);
        out.close();
        std::filesystem::rename(tmp_dest, *dest, ec);
        if (ec) {
            std::filesystem::remove(tmp_dest);
            CFW_LOG_ERROR("NetworkSystem: Failed to finalize file '{}'", dest->string());
            impl_->incoming_transfers.erase(tx_key);
            return;
        }
    } else {
        CFW_LOG_ERROR("NetworkSystem: Failed to write file '{}'", dest->string());
        impl_->incoming_transfers.erase(tx_key);
        return;
    }

    impl_->incoming_transfers.erase(tx_key);

    auto group_id_it = impl_->transfer_to_group.find(transfer_id);
    if (group_id_it != impl_->transfer_to_group.end()) {
        uint64_t group_id = group_id_it->second;
        impl_->transfer_to_group.erase(group_id_it);
        auto ft_it = impl_->pending_file_transfer_groups.find(group_id);
        if (ft_it == impl_->pending_file_transfer_groups.end()) {
            return;
        }
        ft_it->second.last_activity_time = Impl::Clock::now();
        if (ft_it->second.remaining_files > 0) {
            --ft_it->second.remaining_files;
        }
        CFW_LOG_INFO(
            "NetworkSystem: Completed FILE_CHUNK transfer — id={} path='{}' remaining={}",
            transfer_id, model_path, ft_it->second.remaining_files);
        if (ft_it->second.remaining_files != 0) {
            return;
        }


        for (auto& action : ft_it->second.actor_actions) {
            action.model_path = ft_it->second.model_path;
            action.dependency_paths = ft_it->second.dependency_paths;
            impl_->pending_actor_creates.push_back(std::move(action));
        }
        if (!ft_it->second.asset_id.empty()) {
            impl_->received_asset_cache[ft_it->second.asset_id] = {
                ft_it->second.model_path, ft_it->second.dependency_paths};
            impl_->asset_to_transfer_group.erase(ft_it->second.asset_id);
        }
        impl_->pending_file_transfer_groups.erase(ft_it);
    }
}

}  // namespace Corona::Systems
