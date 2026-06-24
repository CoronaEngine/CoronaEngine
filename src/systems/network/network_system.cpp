#include <corona/events/network_system_events.h>
#include <corona/kernel/core/i_logger.h>
#include <corona/kernel/event/i_event_bus.h>
#include <corona/kernel/event/i_event_stream.h>
#include <corona/systems/network/file_transfer.h>
#include <corona/systems/network/lanchat_history_store.h>
#include <corona/systems/network/network_identity.h>
#include <corona/systems/network/network_system.h>
#include <corona/shared_data_hub.h>

#include <chrono>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace Corona::Systems {

// ============================================================================
// Impl
// ============================================================================

struct NetworkSystem::Impl {
    Kernel::ISystemContext* ctx = nullptr;

    // Subsystems
    Network::PeerManager peer_manager;
    Network::SyncEngine sync_engine;
    Network::NetworkIdentityRegistry identity_registry{SharedDataHub::instance()};
    Network::LanChatState lanchat;
    std::function<void(const std::string&)> lanchat_event_callback;
    std::deque<NetworkSystem::LanChatRoomEvent> pending_lanchat_room_events;
    std::string lanchat_nickname;
    std::string lanchat_history_session_id;
    uint64_t next_lanchat_message_id = 1;
    std::unordered_map<std::string, std::string> lanchat_member_by_peer;
    bool lanchat_join_pending = false;
    bool lanchat_join_member_snapshot_received = false;
    bool lanchat_join_history_snapshot_received = false;
    std::chrono::steady_clock::time_point lanchat_join_started;
    bool stop_session_after_peer_disconnect = false;

    // State
    SessionState session_state{SessionState::Idle};
    SessionRole session_role{SessionRole::None};
    std::string instance_name;
    std::string host_address;
    uint16_t host_port = 0;
    uint64_t project_id = 0;
    uint16_t port = Network::kDefaultPort;

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
        std::string actor_guid;
        std::string scene_name;
        std::string model_path;
        std::vector<std::string> dependency_paths;
        std::string actor_json;
        std::vector<uint64_t> transfer_ids;
        uint32_t remaining_files = 0;
        Clock::time_point create_time;
        Clock::time_point last_activity_time;
        Network::ActorCreatePacked actor_packed;
    };
    std::unordered_map<uint64_t, PendingFileTransfer> pending_file_transfer_groups;
    std::unordered_map<uint64_t, uint64_t> transfer_to_group;
    uint64_t next_transfer_id = 1;
};

namespace {
constexpr auto kTransferTimeout = std::chrono::seconds(30);
constexpr auto kOutgoingCacheTtl = std::chrono::minutes(2);
constexpr auto kLanChatJoinTimeout = std::chrono::seconds(5);
std::atomic<uint64_t> g_lanchat_history_session_counter{1};

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string sanitize_history_session_part(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "room" : out;
}

std::string make_lanchat_history_session_id(const std::string& room_id) {
    std::ostringstream out;
    out << sanitize_history_session_part(room_id)
        << "__session__"
        << now_ms()
        << "__"
        << g_lanchat_history_session_counter.fetch_add(1, std::memory_order_relaxed);
    return out.str();
}

std::string json_escape(const std::string& input) {
    std::ostringstream out;
    for (char ch : input) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(static_cast<unsigned char>(ch));
            } else {
                out << ch;
            }
            break;
        }
    }
    return out.str();
}

std::string json_string(const std::string& value) {
    return "\"" + json_escape(value) + "\"";
}

std::string member_names_json(const std::vector<Network::LanChatMember>& members) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < members.size(); ++i) {
        if (i > 0) out << ",";
        out << json_string(members[i].nickname);
    }
    out << "]";
    return out.str();
}

std::string member_details_json(const std::vector<Network::LanChatMember>& members) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < members.size(); ++i) {
        const auto& member = members[i];
        if (i > 0) out << ",";
        out << "{\"member_id\":" << json_string(member.member_id)
            << ",\"nickname\":" << json_string(member.nickname)
            << ",\"status\":" << json_string(member.status)
            << "}";
    }
    out << "]";
    return out.str();
}

std::string member_update_event_json(const std::vector<Network::LanChatMember>& members,
                                     const std::string& local_peer_id) {
    std::ostringstream out;
    out << "{\"channel\":\"lanchat\",\"event\":\"member_update\""
        << ",\"peer_id\":" << json_string(local_peer_id)
        << ",\"members\":" << member_names_json(members)
        << ",\"member_details\":" << member_details_json(members)
        << "}";
    return out.str();
}

std::string history_json(const std::vector<Network::LanChatMessage>& history) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < history.size(); ++i) {
        const auto& message = history[i];
        if (i > 0) out << ",";
        out << "{\"message_id\":" << json_string(message.message_id)
            << ",\"sender_id\":" << json_string(message.sender_id)
            << ",\"room_id\":" << json_string(message.room_id)
            << ",\"seq\":" << message.seq
            << ",\"from\":" << json_string(message.sender_name)
            << ",\"text\":" << json_string(message.text)
            << ",\"ts\":" << (message.timestamp_ms / 1000)
            << ",\"sender_type\":" << json_string(message.sender_type)
            << ",\"message_kind\":" << json_string(message.message_kind)
            << ",\"target_agent_id\":" << json_string(message.target_agent_id)
            << ",\"source_user_id\":" << json_string(message.source_user_id)
            << ",\"correlation_id\":" << json_string(message.correlation_id)
            << ",\"metadata_json\":" << json_string(message.metadata_json)
            << "}";
    }
    out << "]";
    return out.str();
}

std::string agents_json(const std::vector<Network::LanChatAgent>& agents) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < agents.size(); ++i) {
        const auto& agent = agents[i];
        if (i > 0) out << ",";
        out << "{\"agent_id\":" << json_string(agent.agent_id)
            << ",\"name\":" << json_string(agent.name)
            << ",\"persona\":" << json_string(agent.persona)
            << ",\"owner\":" << json_string(agent.owner_id)
            << "}";
    }
    out << "]";
    return out.str();
}

std::string message_event_json(const Network::LanChatMessage& message) {
    std::ostringstream out;
    out << "{\"channel\":\"lanchat\",\"event\":\"message\""
        << ",\"message_id\":" << json_string(message.message_id)
        << ",\"sender_id\":" << json_string(message.sender_id)
        << ",\"room_id\":" << json_string(message.room_id)
        << ",\"seq\":" << message.seq
        << ",\"from\":" << json_string(message.sender_name)
        << ",\"text\":" << json_string(message.text)
        << ",\"ts\":" << (message.timestamp_ms / 1000)
        << ",\"sender_type\":" << json_string(message.sender_type)
        << ",\"message_kind\":" << json_string(message.message_kind)
        << ",\"target_agent_id\":" << json_string(message.target_agent_id)
        << ",\"source_user_id\":" << json_string(message.source_user_id)
        << ",\"correlation_id\":" << json_string(message.correlation_id)
        << ",\"metadata_json\":" << json_string(message.metadata_json)
        << "}";
    return out.str();
}

std::string history_snapshot_event_json(const std::vector<Network::LanChatMessage>& history) {
    std::ostringstream out;
    out << "{\"channel\":\"lanchat\",\"event\":\"history_snapshot\""
        << ",\"history\":" << history_json(history)
        << "}";
    return out.str();
}

std::string lanchat_error_event_json(const std::string& code,
                                     const std::string& message = {}) {
    std::ostringstream out;
    out << "{\"channel\":\"lanchat\",\"event\":\"error\""
        << ",\"code\":" << json_string(code);
    if (!message.empty()) {
        out << ",\"message\":" << json_string(message);
    }
    out << "}";
    return out.str();
}

std::filesystem::path lanchat_history_root_from_project(const std::string& project_root) {
    const auto base = project_root.empty()
        ? std::filesystem::current_path()
        : std::filesystem::path(project_root);
    return base / "Saved" / "LANChat" / "history";
}

bool read_chat_message(Network::BufferReader& r, Network::LanChatMessage& message,
                       bool structured = false) {
    if (!r.has_remaining(2)) return false;
    uint16_t message_id_len = r.read_u16();
    if (!r.has_remaining(message_id_len + 2)) return false;
    message.message_id = r.read_string(message_id_len);
    uint16_t sender_id_len = r.read_u16();
    if (!r.has_remaining(sender_id_len + 2)) return false;
    message.sender_id = r.read_string(sender_id_len);
    uint16_t room_id_len = r.read_u16();
    if (!r.has_remaining(room_id_len + 8 + 2)) return false;
    message.room_id = r.read_string(room_id_len);
    message.seq = r.read_u64();
    uint16_t sender_name_len = r.read_u16();
    if (!r.has_remaining(sender_name_len + 2)) return false;
    message.sender_name = r.read_string(sender_name_len);
    uint16_t text_len = r.read_u16();
    if (!r.has_remaining(text_len + 8)) return false;
    message.text = r.read_string(text_len);
    message.timestamp_ms = r.read_u64();
    message.sender_type = "user";
    message.message_kind = "chat";
    if (!structured) {
        return true;
    }
    if (r.has_remaining(2)) {
        uint16_t sender_type_len = r.read_u16();
        if (!r.has_remaining(sender_type_len)) return false;
        message.sender_type = r.read_string(sender_type_len);
    }
    if (r.has_remaining(2)) {
        uint16_t message_kind_len = r.read_u16();
        if (!r.has_remaining(message_kind_len)) return false;
        message.message_kind = r.read_string(message_kind_len);
    }
    if (r.has_remaining(2)) {
        uint16_t target_agent_len = r.read_u16();
        if (!r.has_remaining(target_agent_len)) return false;
        message.target_agent_id = r.read_string(target_agent_len);
    }
    if (r.has_remaining(2)) {
        uint16_t source_user_len = r.read_u16();
        if (!r.has_remaining(source_user_len)) return false;
        message.source_user_id = r.read_string(source_user_len);
    }
    if (r.has_remaining(2)) {
        uint16_t correlation_len = r.read_u16();
        if (!r.has_remaining(correlation_len)) return false;
        message.correlation_id = r.read_string(correlation_len);
    }
    if (r.has_remaining(2)) {
        uint16_t metadata_len = r.read_u16();
        if (!r.has_remaining(metadata_len)) return false;
        message.metadata_json = r.read_string(metadata_len);
    }
    return true;
}

std::vector<uint8_t> build_chat_packet_for_type(
    const Network::LanChatMessage& message,
    Network::MessageType type) {
    Network::MessageType wire_type = type;
    if (type == Network::MessageType::CHAT_MESSAGE) {
        wire_type = Network::MessageType::CHAT_MESSAGE_V2;
    } else if (type == Network::MessageType::CHAT_AGENT_REPLY) {
        wire_type = Network::MessageType::CHAT_AGENT_REPLY_V2;
    }
    return Network::build_chat_message_v2(
        wire_type,
        message.message_id, message.sender_id, message.room_id, message.seq,
        message.sender_name, message.text, message.timestamp_ms,
        message.sender_type, message.message_kind, message.target_agent_id,
        message.source_user_id, message.correlation_id, message.metadata_json);
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

bool send_lanchat_join_to_ready_peer(Network::PeerManager& peer_manager,
                                     const std::string& ip,
                                     uint16_t port,
                                     const std::vector<uint8_t>& packet) {
    if (packet.empty()) return false;
    const auto peers = peer_manager.peers();
    for (const auto& peer : peers) {
        if (peer.peer && peer.connected && peer.hello_done &&
            peer_matches_endpoint(peer, ip, port)) {
            peer_manager.send_to(peer.peer, Network::kChannelReliable,
                                 packet.data(), packet.size(), true);
            return true;
        }
    }
    return false;
}

bool send_to_peer_id(Network::PeerManager& peer_manager,
                     const std::string& peer_id,
                     const std::vector<uint8_t>& packet) {
    if (packet.empty()) return false;
    const auto* peer_info = peer_manager.find_peer(peer_id);
    if (!peer_info || !peer_info->peer) return false;
    peer_manager.send_to(
        peer_info->peer, Network::kChannelReliable,
        packet.data(), packet.size(), true);
    return true;
}

void complete_lanchat_join_if_ready(bool& pending,
                                    bool member_snapshot_received,
                                    bool history_snapshot_received) {
    if (pending && member_snapshot_received && history_snapshot_received) {
        pending = false;
    }
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

    // Wire up PeerManager → SyncEngine inbound path
    impl_->peer_manager.set_on_data_received(
        [this](const std::string& peer_id, const uint8_t* data, size_t len) {
            // Route: SYNC_DIRTY/SYNC_FULL/HEARTBEAT → sync engine
            //         ACTOR_CREATE/FILE_REQUEST/FILE_CHUNK → custom handler
            if (len >= 1) {
                using Network::MessageType;
                auto mt = static_cast<MessageType>(data[0]);
                if (mt == MessageType::SYNC_DIRTY || mt == MessageType::SYNC_FULL ||
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
    if (impl_->session_state != SessionState::Active) return;

    auto now = Impl::Clock::now();

    // Poll ENet events every tick
    impl_->peer_manager.poll();

    if (impl_->stop_session_after_peer_disconnect) {
        impl_->stop_session_after_peer_disconnect = false;
        stop_session();
        return;
    }

    if (impl_->lanchat_join_pending &&
        now - impl_->lanchat_join_started > kLanChatJoinTimeout) {
        const std::string code = impl_->peer_manager.peer_count() == 0
            ? "HOST_UNREACHABLE"
            : "JOIN_TIMEOUT";
        CFW_LOG_WARNING("NetworkSystem: LANChat join timed out — room='{}' code={}",
                        impl_->lanchat.room_id(), code);
        clear_lanchat_room_state();
        if (impl_->lanchat_event_callback) {
            impl_->lanchat_event_callback(lanchat_error_event_json(code));
        }
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
                "NetworkSystem: Pending file group timed out — actor='{}' model='{}' "
                "remaining={} transfers={}",
                it->second.actor_guid, it->second.model_path,
                it->second.remaining_files, it->second.transfer_ids.size());
            for (uint64_t transfer_id : it->second.transfer_ids) {
                impl_->transfer_to_group.erase(transfer_id);
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

    impl_->session_state = SessionState::Starting;
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

    // 2. SyncEngine
    impl_->sync_engine.initialize(impl_->peer_manager.local_peer_id());

    impl_->session_state = SessionState::Active;
    impl_->last_sync_time = Impl::Clock::now();

    // Publish event
    if (impl_->ctx && impl_->ctx->event_bus()) {
        Events::NetworkHostStartedEvent ev{port};
        impl_->ctx->event_bus()->publish(ev);
    }

    CFW_LOG_INFO("NetworkSystem: Session active — listening on port {} role={}",
                 port, session_role_label(impl_->session_role));
    return true;
}

void NetworkSystem::stop_session() {
    if (impl_->session_state != SessionState::Active &&
        impl_->session_state != SessionState::Error) return;

    notify_lanchat_room_closed();
    impl_->peer_manager.stop();
    impl_->sync_engine.shutdown();
    impl_->identity_registry.clear();
    impl_->incoming_transfers.clear();
    impl_->outgoing_cache.clear();
    impl_->pending_actor_creates.clear();
    impl_->pending_actor_transform_updates.clear();
    impl_->pending_actor_deletes.clear();
    impl_->pending_actor_scene_snapshot_requests.clear();
    impl_->pending_actor_scene_snapshots.clear();
    impl_->pending_actor_state_updates.clear();
    impl_->pending_file_transfer_groups.clear();
    impl_->transfer_to_group.clear();
    clear_lanchat_room_state();
    impl_->stop_session_after_peer_disconnect = false;

    impl_->session_state = SessionState::Idle;
    impl_->session_role = SessionRole::None;
    impl_->host_address.clear();
    impl_->host_port = 0;

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

bool NetworkSystem::is_connected_host_peer(
    const Network::PeerManager::PeerInfo& info) const {
    if (impl_->host_address.empty() || impl_->host_port == 0) return false;
    return peer_matches_endpoint(info, impl_->host_address, impl_->host_port);
}

bool NetworkSystem::is_message_from_connected_host(
    const std::string& sender_peer_id) const {
    if (impl_->host_address.empty() || impl_->host_port == 0) return false;
    const auto* peer_info = impl_->peer_manager.find_peer(sender_peer_id);
    if (peer_info && is_connected_host_peer(*peer_info)) return true;
    return endpoint_matches(sender_peer_id, impl_->host_address, impl_->host_port);
}

bool NetworkSystem::send_to_connected_host_peer(
    const std::vector<uint8_t>& packet) {
    if (packet.empty() || impl_->host_address.empty() || impl_->host_port == 0) {
        return false;
    }
    for (const auto& peer : impl_->peer_manager.peers()) {
        if (peer.peer && peer.connected && peer.hello_done &&
            is_connected_host_peer(peer)) {
            impl_->peer_manager.send_to(
                peer.peer, Network::kChannelReliable,
                packet.data(), packet.size(), true);
            return true;
        }
    }
    return false;
}

void NetworkSystem::notify_lanchat_room_closed() {
    const std::string room_id = impl_->lanchat.room_id();
    if (room_id.empty()) {
        return;
    }
    impl_->pending_lanchat_room_events.push_back({"room_closed", room_id});
    if (impl_->pending_lanchat_room_events.size() > 32) {
        impl_->pending_lanchat_room_events.pop_front();
    }
    const std::string event_json =
        "{\"channel\":\"lanchat\",\"event\":\"room_closed\",\"room_id\":" +
        json_string(room_id) + "}";
    if (impl_->lanchat_event_callback) {
        impl_->lanchat_event_callback(event_json);
    }
}

void NetworkSystem::clear_lanchat_room_state() {
    impl_->lanchat.close_room();
    impl_->lanchat_member_by_peer.clear();
    impl_->lanchat_nickname.clear();
    impl_->lanchat_history_session_id.clear();
    impl_->lanchat_join_pending = false;
    impl_->lanchat_join_member_snapshot_received = false;
    impl_->lanchat_join_history_snapshot_received = false;
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

void NetworkSystem::set_lanchat_event_callback(std::function<void(const std::string&)> callback) {
    impl_->lanchat_event_callback = std::move(callback);
}

bool NetworkSystem::lanchat_start_local_room(const std::string& room_id,
                                             const std::string& nickname) {
    const std::string display_name = nickname.empty() ? "房主" : nickname;
    impl_->lanchat_nickname = display_name;
    impl_->lanchat_member_by_peer.clear();
    impl_->lanchat_join_pending = false;
    impl_->lanchat_join_member_snapshot_received = false;
    impl_->lanchat_join_history_snapshot_received = false;
    const bool opened = impl_->lanchat.open_room(room_id, "local-single-player", display_name);
    if (opened) {
        impl_->lanchat_history_session_id = make_lanchat_history_session_id(
            impl_->lanchat.room_id());
    }
    return opened;
}

bool NetworkSystem::lanchat_start_room(const std::string& room_id,
                                       const std::string& nickname,
                                       uint16_t port) {
    const std::string display_name = nickname.empty() ? "房主" : nickname;
    if (impl_->session_state != SessionState::Active) {
        if (!start_session(display_name, 0, port, SessionRole::Host)) {
            return false;
        }
    }
    impl_->lanchat_nickname = display_name;
    impl_->lanchat_member_by_peer.clear();
    impl_->lanchat_join_pending = false;
    impl_->lanchat_join_member_snapshot_received = false;
    impl_->lanchat_join_history_snapshot_received = false;
    const std::string peer_id = local_peer_id();
    const bool opened = impl_->lanchat.open_room(room_id, peer_id, display_name);
    if (opened) {
        impl_->lanchat_history_session_id = make_lanchat_history_session_id(
            impl_->lanchat.room_id());
    }
    return opened;
}

void NetworkSystem::lanchat_stop_local_room() {
    clear_lanchat_room_state();
}

bool NetworkSystem::lanchat_join_room(const std::string& ip,
                                      uint16_t port,
                                      const std::string& room_id,
                                      const std::string& nickname) {
    const std::string display_name = nickname.empty() ? "Guest" : nickname;
    const bool was_active_client =
        impl_->session_state == SessionState::Active &&
        impl_->session_role == SessionRole::Client;
    const uint16_t effective_port =
        was_active_client && impl_->host_address == ip && impl_->host_port != 0
            ? impl_->host_port
            : port;
    if (impl_->session_state != SessionState::Active) {
        if (!start_session(display_name, 0, 0, SessionRole::Client)) {
            return false;
        }
    }
    impl_->lanchat_nickname = display_name;
    impl_->lanchat_member_by_peer.clear();
    impl_->lanchat.open_room(room_id, local_peer_id(), display_name);
    impl_->lanchat_history_session_id = make_lanchat_history_session_id(
        impl_->lanchat.room_id());
    impl_->lanchat_join_pending = true;
    impl_->lanchat_join_member_snapshot_received = false;
    impl_->lanchat_join_history_snapshot_received = false;
    impl_->lanchat_join_started = Impl::Clock::now();
    auto join_packet = Network::build_chat_join(
        impl_->lanchat.room_id(), local_peer_id(), impl_->lanchat_nickname);
    if (!send_lanchat_join_to_ready_peer(
            impl_->peer_manager, ip, effective_port, join_packet) &&
        !connect_to_peer(ip, effective_port, display_name)) {
        impl_->lanchat.close_room();
        impl_->lanchat_history_session_id.clear();
        impl_->lanchat_join_pending = false;
        return false;
    }
    return true;
}

void NetworkSystem::lanchat_leave_room() {
    if (!impl_->lanchat.room_id().empty() && impl_->session_state == SessionState::Active) {
        auto packet = Network::build_chat_leave(impl_->lanchat.room_id(), local_peer_id());
        if (impl_->session_role == SessionRole::Client) {
            send_to_connected_host_peer(packet);
        } else {
            impl_->peer_manager.broadcast(Network::kChannelReliable, packet.data(), packet.size(), true);
        }
    }
    clear_lanchat_room_state();
}

Network::LanChatMessageResult NetworkSystem::lanchat_send_message(const std::string& text) {
    return lanchat_send_message_ex(text, "chat");
}

Network::LanChatMessageResult NetworkSystem::lanchat_send_message_ex(
    const std::string& text,
    const std::string& message_kind,
    const std::string& target_agent_id,
    const std::string& source_user_id,
    const std::string& correlation_id,
    const std::string& metadata_json) {
    if (impl_->session_role == SessionRole::Client && impl_->peer_manager.peer_count() == 0) {
        return {false, {}, "CONNECTING"};
    }

    const uint64_t timestamp = now_ms();
    const std::string peer_id = local_peer_id();
    const std::string sender_id = peer_id.empty() ? "local-single-player" : peer_id;
    const std::string message_id =
        sender_id + ":" + std::to_string(timestamp) + ":" +
        std::to_string(impl_->next_lanchat_message_id++);
    const std::string local_sender_type =
        impl_->session_role == SessionRole::Host ? "host" : "user";

    if (impl_->session_role == SessionRole::Client) {
        Network::LanChatMessage message;
        message.message_id = message_id;
        message.sender_id = sender_id;
        message.sender_name = impl_->lanchat_nickname;
        message.room_id = impl_->lanchat.room_id();
        message.text = text;
        message.seq = 0;
        message.timestamp_ms = timestamp;
        message.sender_type = local_sender_type;
        message.message_kind = message_kind.empty() ? "chat" : message_kind;
        message.target_agent_id = target_agent_id;
        message.source_user_id = source_user_id.empty() ? sender_id : source_user_id;
        message.correlation_id = correlation_id;
        message.metadata_json = metadata_json;
        auto packet = build_chat_packet_for_type(message, Network::MessageType::CHAT_MESSAGE);
        if (!send_to_connected_host_peer(packet)) {
            return {false, {}, "CONNECTING"};
        }
        return {true, message, {}};
    }

    auto result = impl_->lanchat.record_message_ex(
        message_id, sender_id, impl_->lanchat_nickname, text, timestamp,
        local_sender_type, message_kind.empty() ? "chat" : message_kind, target_agent_id,
        source_user_id.empty() ? sender_id : source_user_id, correlation_id,
        metadata_json);
    if (!result.accepted) {
        return result;
    }
    persist_lanchat_message(result.message);

    auto packet = build_chat_packet_for_type(result.message, Network::MessageType::CHAT_MESSAGE);
    if (impl_->session_state == SessionState::Active) {
        impl_->peer_manager.broadcast(Network::kChannelReliable, packet.data(), packet.size(), true);
    }
    if (impl_->lanchat_event_callback) {
        impl_->lanchat_event_callback(message_event_json(result.message));
    }
    impl_->lanchat.enqueue_agent_triggers_for_message(result.message, sender_id);
    return result;
}

Network::LanChatMessageResult NetworkSystem::lanchat_send_agent_reply(
    const std::string& agent_id,
    const std::string& agent_name,
    const std::string& text) {
    return lanchat_send_agent_reply_ex(
        agent_id, agent_name, text, "agent", "agent_reply");
}

Network::LanChatMessageResult NetworkSystem::lanchat_send_agent_reply_ex(
    const std::string& agent_id,
    const std::string& agent_name,
    const std::string& text,
    const std::string& sender_type,
    const std::string& message_kind,
    const std::string& target_agent_id,
    const std::string& source_user_id,
    const std::string& correlation_id,
    const std::string& metadata_json) {
    const uint64_t timestamp = now_ms();
    const std::string peer_id = local_peer_id();
    const std::string sender_id = agent_id.empty()
        ? (peer_id.empty() ? "local-single-player" : peer_id)
        : agent_id;
    const std::string sender_name = agent_name.empty() ? "Agent" : agent_name;
    const std::string message_id =
        sender_id + ":" + std::to_string(timestamp) + ":" +
        std::to_string(impl_->next_lanchat_message_id++);

    if (impl_->session_role == SessionRole::Client) {
        Network::LanChatMessage message;
        message.message_id = message_id;
        message.sender_id = sender_id;
        message.sender_name = sender_name;
        message.room_id = impl_->lanchat.room_id();
        message.text = text;
        message.seq = 0;
        message.timestamp_ms = timestamp;
        message.sender_type = sender_type.empty() ? "agent" : sender_type;
        message.message_kind = message_kind.empty() ? "agent_reply" : message_kind;
        message.target_agent_id = target_agent_id;
        message.source_user_id = source_user_id;
        message.correlation_id = correlation_id;
        message.metadata_json = metadata_json;
        auto packet = build_chat_packet_for_type(message, Network::MessageType::CHAT_AGENT_REPLY);
        if (!send_to_connected_host_peer(packet)) {
            return {false, {}, "CONNECTING"};
        }
        return {true, message, {}};
    }

    auto result = impl_->lanchat.record_message_ex(
        message_id, sender_id, sender_name, text, timestamp,
        sender_type.empty() ? "agent" : sender_type,
        message_kind.empty() ? "agent_reply" : message_kind,
        target_agent_id, source_user_id, correlation_id, metadata_json);
    if (!result.accepted) {
        return result;
    }
    persist_lanchat_message(result.message);

    auto packet = build_chat_packet_for_type(result.message, Network::MessageType::CHAT_AGENT_REPLY);
    if (impl_->session_state == SessionState::Active) {
        impl_->peer_manager.broadcast(Network::kChannelReliable, packet.data(), packet.size(), true);
    }
    if (impl_->lanchat_event_callback) {
        impl_->lanchat_event_callback(message_event_json(result.message));
    }
    return result;
}

Network::LanChatMessageResult NetworkSystem::lanchat_send_system_message_to_host_ex(
    const std::string& sender_id,
    const std::string& sender_name,
    const std::string& text,
    const std::string& message_kind,
    const std::string& correlation_id,
    const std::string& metadata_json) {
    if (impl_->session_role != SessionRole::Host) {
        return {false, {}, "NOT_HOST"};
    }
    return lanchat_send_system_message_to_user_ex(
        local_peer_id(),
        sender_id,
        sender_name,
        text,
        message_kind,
        correlation_id,
        metadata_json);
}

Network::LanChatMessageResult NetworkSystem::lanchat_send_system_message_to_user_ex(
    const std::string& target_user_id,
    const std::string& sender_id,
    const std::string& sender_name,
    const std::string& text,
    const std::string& message_kind,
    const std::string& correlation_id,
    const std::string& metadata_json) {
    const std::string local_id = local_peer_id();
    if (!target_user_id.empty() && target_user_id != local_id && target_user_id != "host") {
        return {false, {}, "TARGET_NOT_LOCAL"};
    }

    const uint64_t timestamp = now_ms();
    const std::string effective_sender_id = sender_id.empty() ? "system" : sender_id;
    const std::string effective_sender_name = sender_name.empty() ? "系统" : sender_name;
    const std::string message_id =
        effective_sender_id + ":" + std::to_string(timestamp) + ":" +
        std::to_string(impl_->next_lanchat_message_id++);
    auto result = impl_->lanchat.record_message_ex(
        message_id,
        effective_sender_id,
        effective_sender_name,
        text,
        timestamp,
        "system",
        message_kind.empty() ? "action_status" : message_kind,
        {},
        target_user_id.empty() ? local_id : target_user_id,
        correlation_id,
        metadata_json);
    if (result.accepted && impl_->lanchat_event_callback) {
        impl_->lanchat_event_callback(message_event_json(result.message));
    }
    if (result.accepted) {
        persist_lanchat_message(result.message);
    }
    return result;
}

Network::LanChatResult NetworkSystem::lanchat_register_agent(const std::string& agent_id,
                                                             const std::string& name,
                                                             const std::string& persona,
                                                             const std::string& owner_id) {
    const std::string peer_id = local_peer_id();
    const std::string owner = owner_id.empty()
        ? (peer_id.empty() ? "local-single-player" : peer_id)
        : owner_id;
    if (impl_->session_role == SessionRole::Client && impl_->session_state == SessionState::Active) {
        auto packet = Network::build_chat_agent_register(
            impl_->lanchat.room_id(), agent_id, name, persona, owner);
        if (!send_to_connected_host_peer(packet)) {
            return {false, "CONNECTING"};
        }
        return {true, {}};
    }

    auto result = impl_->lanchat.register_agent(agent_id, name, persona, owner);
    if (result.ok) {
        persist_lanchat_agents(impl_->lanchat.room_id());
    }
    if (result.ok && impl_->session_state == SessionState::Active) {
        auto packet = Network::build_chat_agent_register(
            impl_->lanchat.room_id(), agent_id, name, persona, owner);
        impl_->peer_manager.broadcast(Network::kChannelReliable, packet.data(), packet.size(), true);
    }
    if (result.ok && impl_->lanchat_event_callback) {
        impl_->lanchat_event_callback(
            "{\"channel\":\"lanchat\",\"event\":\"agent_roster\",\"agents\":" +
            agents_json(impl_->lanchat.agents()) + "}");
    }
    return result;
}

Network::LanChatResult NetworkSystem::lanchat_remove_agent(const std::string& agent_id) {
    if (impl_->session_role == SessionRole::Client && impl_->session_state == SessionState::Active) {
        auto packet = Network::build_chat_agent_remove(impl_->lanchat.room_id(), agent_id);
        if (!send_to_connected_host_peer(packet)) {
            return {false, "CONNECTING"};
        }
        return {true, {}};
    }

    auto result = impl_->lanchat.remove_agent(agent_id);
    if (result.ok) {
        persist_lanchat_agents(impl_->lanchat.room_id());
    }
    if (result.ok && impl_->session_state == SessionState::Active) {
        auto packet = Network::build_chat_agent_remove(impl_->lanchat.room_id(), agent_id);
        impl_->peer_manager.broadcast(Network::kChannelReliable, packet.data(), packet.size(), true);
    }
    if (result.ok && impl_->lanchat_event_callback) {
        impl_->lanchat_event_callback(
            "{\"channel\":\"lanchat\",\"event\":\"agent_roster\",\"agents\":" +
            agents_json(impl_->lanchat.agents()) + "}");
    }
    return result;
}

const std::vector<Network::LanChatMember>& NetworkSystem::lanchat_members() const {
    return impl_->lanchat.members();
}

const std::vector<Network::LanChatMessage>& NetworkSystem::lanchat_history() const {
    return impl_->lanchat.history();
}

bool NetworkSystem::lanchat_restore_history_room(const std::string& room_id) {
    if (room_id.empty() || impl_->lanchat.room_id().empty()) return false;
    const auto root = lanchat_history_root_from_project(impl_->project_root);
    Network::LanChatHistoryStore store(root);
    auto history = store.load_room(room_id);
    auto agents = store.load_agents(room_id);
    impl_->lanchat.apply_history_snapshot(history);
    for (const auto& agent : agents) {
        impl_->lanchat.register_agent(
            agent.agent_id, agent.name, agent.persona, agent.owner_id);
    }
    CFW_LOG_INFO("NetworkSystem: LANChat explicitly restored selected history — room='{}' messages={} agents={}",
                 room_id, history.size(), agents.size());
    return true;
}

std::vector<Network::LanChatHistoryRoomSummary> NetworkSystem::lanchat_history_rooms() const {
    Network::LanChatHistoryStore store(lanchat_history_root_from_project(impl_->project_root));
    return store.list_rooms();
}

std::vector<Network::LanChatMessage> NetworkSystem::lanchat_load_history_room(
    const std::string& room_id,
    size_t max_messages) const {
    Network::LanChatHistoryStore store(lanchat_history_root_from_project(impl_->project_root));
    return store.load_room(room_id, max_messages);
}

std::vector<Network::LanChatAgent> NetworkSystem::lanchat_load_history_agents(
    const std::string& room_id) const {
    Network::LanChatHistoryStore store(lanchat_history_root_from_project(impl_->project_root));
    return store.load_agents(room_id);
}

const std::vector<Network::LanChatAgent>& NetworkSystem::lanchat_agents() const {
    return impl_->lanchat.agents();
}

std::optional<Network::LanChatAgentTrigger> NetworkSystem::lanchat_pop_agent_trigger() {
    if (impl_->session_role == SessionRole::Client) {
        return std::nullopt;
    }
    return impl_->lanchat.pop_agent_trigger();
}

std::optional<Network::LanChatMessage> NetworkSystem::lanchat_pop_coordinator_sync_message() {
    const bool allow_generation_start = impl_->session_role != SessionRole::Client;
    return impl_->lanchat.pop_coordinator_sync_message(allow_generation_start);
}

std::optional<NetworkSystem::LanChatRoomEvent> NetworkSystem::lanchat_pop_room_event() {
    if (impl_->pending_lanchat_room_events.empty()) {
        return std::nullopt;
    }
    auto event = impl_->pending_lanchat_room_events.front();
    impl_->pending_lanchat_room_events.pop_front();
    return event;
}

Network::LanChatResult NetworkSystem::lanchat_lock_object(const std::string& object_id,
                                                          const std::string& user_id,
                                                          const std::string& operation,
                                                          uint64_t now_ms_value) {
    return impl_->lanchat.lock_object(object_id, user_id, operation, now_ms_value);
}

Network::LanChatResult NetworkSystem::lanchat_unlock_object(const std::string& object_id,
                                                            const std::string& user_id) {
    return impl_->lanchat.unlock_object(object_id, user_id);
}

std::string NetworkSystem::lanchat_locked_by(const std::string& object_id,
                                             uint64_t now_ms_value) {
    return impl_->lanchat.locked_by(object_id, now_ms_value);
}

void NetworkSystem::lanchat_broadcast_intent(const std::string& user_id,
                                             const std::string& tooltip,
                                             const std::array<float, 3>& position,
                                             const std::string& status,
                                             uint64_t now_ms_value) {
    impl_->lanchat.broadcast_intent(user_id, tooltip, position, status, now_ms_value);
}

std::string NetworkSystem::lanchat_check_preview_collision(
    const std::string& user_id,
    const std::array<float, 3>& position,
    float delta,
    uint64_t now_ms_value) {
    return impl_->lanchat.check_preview_collision(user_id, position, delta, now_ms_value);
}

void NetworkSystem::broadcast_actor_create(const std::string& actor_guid,
                                           const std::string& scene_name,
                                           const std::string& model_path,
                                           const std::vector<std::string>& dependency_paths,
                                           const float* transform,
                                           const void* optics_packed, size_t optics_size,
                                           const std::string& actor_json) {
    if (impl_->session_state != SessionState::Active) return;
    if (impl_->peer_manager.peer_count() == 0) {
        CFW_LOG_DEBUG("NetworkSystem: No peers — skipping actor create broadcast");
        return;
    }
    auto pkt = Network::build_actor_create(actor_guid, scene_name, model_path, transform,
                                           optics_packed, optics_size, dependency_paths,
                                           actor_json);
    impl_->peer_manager.broadcast(Network::kChannelReliable, pkt.data(), pkt.size(), true);
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
    if (impl_->peer_manager.peer_count() == 0) {
        CFW_LOG_DEBUG("NetworkSystem: No peers — skipping actor transform broadcast");
        return;
    }
    auto pkt = Network::build_actor_transform_update(
        actor_guid, scene_name, transform, source_user_id, correlation_id);
    impl_->peer_manager.broadcast(Network::kChannelReliable, pkt.data(), pkt.size(), true);
    CFW_LOG_INFO("NetworkSystem: Broadcast actor transform — actor='{}' scene='{}' corr='{}'",
                 actor_guid, scene_name, correlation_id);
}

void NetworkSystem::broadcast_actor_delete(const std::string& actor_guid,
                                           const std::string& scene_name,
                                           const std::string& actor_name) {
    if (impl_->session_state != SessionState::Active) return;
    if (actor_guid.empty() && actor_name.empty()) return;
    if (impl_->peer_manager.peer_count() == 0) {
        CFW_LOG_DEBUG("NetworkSystem: No peers — skipping actor delete broadcast");
        return;
    }
    auto pkt = Network::build_actor_delete(actor_guid, scene_name, actor_name);
    impl_->peer_manager.broadcast(Network::kChannelReliable, pkt.data(), pkt.size(), true);
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
    if (impl_->peer_manager.peer_count() == 0) {
        CFW_LOG_DEBUG("NetworkSystem: No peers — skipping actor state update broadcast");
        return;
    }
    auto pkt = Network::build_actor_state_update(actor_guid, scene_name, actor_json);
    impl_->peer_manager.broadcast(Network::kChannelReliable, pkt.data(), pkt.size(), true);
    CFW_LOG_INFO("NetworkSystem: Broadcast actor state update — actor='{}' scene='{}' bytes={}",
                 actor_guid, scene_name, actor_json.size());
}

bool NetworkSystem::has_pending_transfers() const {
    return !impl_->pending_actor_creates.empty() ||
           !impl_->incoming_transfers.empty() ||
           !impl_->pending_file_transfer_groups.empty();
}

void NetworkSystem::set_sync_paused(bool paused) {
    impl_->sync_paused = paused;
}

bool NetworkSystem::pop_pending_actor_create(std::string& actor_guid,
                                              std::string& scene_name,
                                              std::string& model_path,
                                              void* actor_packed_out, size_t packed_size,
                                              std::string* actor_json_out) {
    if (impl_->pending_actor_creates.empty()) return false;
    auto& pa = impl_->pending_actor_creates.front();
    actor_guid = pa.actor_guid;
    scene_name = pa.scene_name;
    model_path = pa.model_path;
    if (actor_packed_out && packed_size <= sizeof(Network::ActorCreatePacked)) {
        std::memcpy(actor_packed_out, &pa.actor_packed, packed_size);
    }
    if (actor_json_out) {
        *actor_json_out = pa.actor_json;
    }
    impl_->pending_actor_creates.erase(impl_->pending_actor_creates.begin());
    return true;
}

bool NetworkSystem::pop_pending_actor_transform_update(std::string& actor_guid,
                                                       std::string& scene_name,
                                                       float* transform_out,
                                                       size_t transform_count,
                                                       std::string& source_user_id,
                                                       std::string& correlation_id) {
    if (impl_->pending_actor_transform_updates.empty()) return false;
    auto& update = impl_->pending_actor_transform_updates.front();
    actor_guid = update.actor_guid;
    scene_name = update.scene_name;
    source_user_id = update.source_user_id;
    correlation_id = update.correlation_id;
    if (transform_out && transform_count >= 9) {
        std::memcpy(transform_out, update.transform, sizeof(update.transform));
    }
    impl_->pending_actor_transform_updates.erase(
        impl_->pending_actor_transform_updates.begin());
    return true;
}

bool NetworkSystem::pop_pending_actor_delete(std::string& actor_guid,
                                             std::string& scene_name,
                                             std::string& actor_name) {
    if (impl_->pending_actor_deletes.empty()) return false;
    auto& pending = impl_->pending_actor_deletes.front();
    actor_guid = pending.actor_guid;
    scene_name = pending.scene_name;
    actor_name = pending.actor_name;
    impl_->pending_actor_deletes.erase(impl_->pending_actor_deletes.begin());
    return true;
}

bool NetworkSystem::pop_pending_actor_scene_snapshot_request(std::string& scene_name) {
    if (impl_->pending_actor_scene_snapshot_requests.empty()) return false;
    auto& pending = impl_->pending_actor_scene_snapshot_requests.front();
    scene_name = pending.scene_name;
    impl_->pending_actor_scene_snapshot_requests.erase(
        impl_->pending_actor_scene_snapshot_requests.begin());
    return true;
}

bool NetworkSystem::pop_pending_actor_scene_snapshot(std::string& scene_name,
                                                     std::string& snapshot_json) {
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
    if (impl_->pending_actor_state_updates.empty()) return false;
    auto& pending = impl_->pending_actor_state_updates.front();
    actor_guid = pending.actor_guid;
    scene_name = pending.scene_name;
    actor_json = pending.actor_json;
    impl_->pending_actor_state_updates.erase(impl_->pending_actor_state_updates.begin());
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
    impl_->project_root = project_root;
}

void NetworkSystem::persist_lanchat_message(const Network::LanChatMessage& message) {
    if (message.room_id.empty() || message.message_id.empty()) return;
    const auto root = lanchat_history_root_from_project(impl_->project_root);
    Network::LanChatHistoryStore store(root);
    const std::string history_id = impl_->lanchat_history_session_id.empty()
        ? message.room_id
        : impl_->lanchat_history_session_id;
    store.append_message(history_id, message);
}

void NetworkSystem::persist_lanchat_agents(const std::string& room_id) {
    if (room_id.empty()) return;
    const auto root = lanchat_history_root_from_project(impl_->project_root);
    Network::LanChatHistoryStore store(root);
    const std::string history_id = impl_->lanchat_history_session_id.empty()
        ? room_id
        : impl_->lanchat_history_session_id;
    store.save_agents(history_id, impl_->lanchat.agents());
}

// ============================================================================
// Callbacks
// ============================================================================

void NetworkSystem::on_peer_connected(const Network::PeerManager::PeerInfo& info) {
    CFW_LOG_INFO("NetworkSystem: Peer connected — {} ({})", info.id, info.name);

    if (impl_->session_role == SessionRole::Client && !impl_->lanchat.room_id().empty()) {
        auto packet = Network::build_chat_join(
            impl_->lanchat.room_id(), local_peer_id(), impl_->lanchat_nickname);
        send_lanchat_join_to_ready_peer(
            impl_->peer_manager, impl_->host_address, impl_->host_port, packet);
    }

    if (impl_->ctx && impl_->ctx->event_bus()) {
        Events::PeerConnectedEvent ev{info.id, info.name};
        impl_->ctx->event_bus()->publish(ev);
    }
}

void NetworkSystem::on_peer_disconnected(const Network::PeerManager::PeerInfo& info) {
    CFW_LOG_INFO("NetworkSystem: Peer disconnected — {} ({})", info.id, info.name);

    if (impl_->session_role == SessionRole::Host && !impl_->lanchat.room_id().empty()) {
        auto it = impl_->lanchat_member_by_peer.find(info.id);
        if (it != impl_->lanchat_member_by_peer.end()) {
            impl_->lanchat.leave_member(it->second);
            impl_->lanchat_member_by_peer.erase(it);
            auto packet = Network::build_chat_member_update(
                impl_->lanchat.room_id(), impl_->lanchat.members());
            impl_->peer_manager.broadcast(
                Network::kChannelReliable, packet.data(), packet.size(), true);
            if (impl_->lanchat_event_callback) {
                impl_->lanchat_event_callback(
                    member_update_event_json(impl_->lanchat.members(), local_peer_id()));
            }
        }
    }

    if (impl_->session_role == SessionRole::Client &&
        is_connected_host_peer(info)) {
        notify_lanchat_room_closed();
        clear_lanchat_room_state();
        impl_->stop_session_after_peer_disconnect = true;
    }

    if (impl_->ctx && impl_->ctx->event_bus()) {
        Events::PeerDisconnectedEvent ev{info.id};
        impl_->ctx->event_bus()->publish(ev);
    }
}

void NetworkSystem::on_data_received(const std::string& peer_id,
                                     const uint8_t* data, size_t len) {
    impl_->sync_engine.handle_incoming(peer_id, data, len);

    if (impl_->ctx && impl_->ctx->event_bus()) {
        Events::RemoteSyncReceivedEvent ev{peer_id};
        impl_->ctx->event_bus()->publish(ev);
    }
}

void NetworkSystem::on_custom_message(const std::string& sender_peer_id,
                                        const uint8_t* data, size_t len) {
    if (len < 1) return;
    using Network::MessageType;
    auto mt = static_cast<MessageType>(data[0]);

    if (mt == MessageType::ACTOR_CREATE) {
        Network::BufferReader r(data + 1, len - 1);
        uint16_t guid_len = r.read_u16();
        std::string actor_guid = r.read_string(guid_len);
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

            if (!actor_guid.empty() && impl_->identity_registry.resolve_actor(actor_guid)) {
                CFW_LOG_DEBUG(
                    "NetworkSystem: Ignoring duplicate ACTOR_CREATE for registered actor='{}' scene='{}'",
                    actor_guid, scene_name);
                return;
            }

            auto pending_file_transfer_for_actor = [&]() {
                for (auto& [_, group] : impl_->pending_file_transfer_groups) {
                    if (group.actor_guid == actor_guid &&
                        group.scene_name == scene_name &&
                        group.model_path == model_path) {
                        group.dependency_paths = dependency_paths;
                        group.actor_json = actor_json;
                        group.actor_packed = actor_packed;
                        group.last_activity_time = Impl::Clock::now();
                        return true;
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
                Impl::PendingAction pa;
                pa.actor_guid = actor_guid;
                pa.scene_name = scene_name;
                pa.model_path = model_path;
                pa.dependency_paths = dependency_paths;
                pa.actor_json = actor_json;
                pa.actor_packed = actor_packed;
                upsert_pending_actor_create(std::move(pa));
            } else {
                uint64_t group_id = impl_->next_transfer_id++;
                Impl::PendingFileTransfer group;
                group.actor_guid = actor_guid;
                group.scene_name = scene_name;
                group.model_path = model_path;
                group.dependency_paths = dependency_paths;
                group.actor_json = actor_json;
                group.actor_packed = actor_packed;
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
            std::erase_if(impl_->pending_actor_creates,
                          [&](const Impl::PendingAction& action) {
                              return action.actor_guid == pending.actor_guid;
                          });
            std::erase_if(impl_->pending_actor_transform_updates,
                          [&](const Impl::PendingTransformUpdate& update) {
                              return update.actor_guid == pending.actor_guid;
                          });
            for (auto it = impl_->pending_file_transfer_groups.begin();
                 it != impl_->pending_file_transfer_groups.end(); ) {
                if (it->second.actor_guid == pending.actor_guid) {
                    for (uint64_t transfer_id : it->second.transfer_ids) {
                        impl_->transfer_to_group.erase(transfer_id);
                    }
                    it = impl_->pending_file_transfer_groups.erase(it);
                } else {
                    ++it;
                }
            }
        }
        impl_->pending_actor_deletes.push_back(pending);
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
        auto upsert_pending_actor_scene_snapshot = [&](Impl::PendingActorSceneSnapshot snapshot) {
            auto it = std::find_if(
                impl_->pending_actor_scene_snapshots.begin(),
                impl_->pending_actor_scene_snapshots.end(),
                [&](const Impl::PendingActorSceneSnapshot& existing) {
                    return existing.scene_name == snapshot.scene_name;
                });
            if (it != impl_->pending_actor_scene_snapshots.end()) {
                *it = std::move(snapshot);
                return;
            }
            impl_->pending_actor_scene_snapshots.push_back(std::move(snapshot));
        };
        upsert_pending_actor_scene_snapshot(std::move(pending));
        CFW_LOG_INFO(
            "NetworkSystem: Received ACTOR_SCENE_SNAPSHOT from {} — scene='{}' bytes={}",
            sender_peer_id, pending_scene_name, pending_snapshot_size);
    } else if (mt == MessageType::ACTOR_STATE_UPDATE) {
        Network::BufferReader r(data + 1, len - 1);
        if (!r.has_remaining(2)) return;
        uint16_t guid_len = r.read_u16();
        if (!r.has_remaining(guid_len + 2)) return;
        Impl::PendingActorStateUpdate pending;
        pending.actor_guid = r.read_string(guid_len);
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
    } else if (mt == MessageType::CHAT_JOIN) {
        if (impl_->session_role != SessionRole::Host) return;
        Network::BufferReader r(data + 1, len - 1);
        if (!r.has_remaining(2)) return;
        uint16_t room_len = r.read_u16();
        if (!r.has_remaining(room_len + 2)) return;
        std::string room_id = r.read_string(room_len);
        uint16_t member_len = r.read_u16();
        if (!r.has_remaining(member_len + 2)) return;
        std::string member_id = r.read_string(member_len);
        uint16_t nick_len = r.read_u16();
        if (!r.has_remaining(nick_len)) return;
        std::string nickname = r.read_string(nick_len);

        if (impl_->lanchat.room_id().empty()) {
            auto reject = Network::build_chat_join_reject(
                room_id, "ROOM_NOT_FOUND", "LANChat room is not open");
            send_to_peer_id(impl_->peer_manager, sender_peer_id, reject);
            CFW_LOG_WARNING(
                "NetworkSystem: Rejected LANChat join from {} — no room open",
                sender_peer_id);
            return;
        }
        if (impl_->lanchat.room_id() != room_id) {
            auto reject = Network::build_chat_join_reject(
                room_id, "ROOM_MISMATCH", "LANChat room id does not match host");
            send_to_peer_id(impl_->peer_manager, sender_peer_id, reject);
            CFW_LOG_WARNING(
                "NetworkSystem: Rejected LANChat join from {} — requested='{}' current='{}'",
                sender_peer_id, room_id, impl_->lanchat.room_id());
            return;
        }
        impl_->lanchat.join_member(member_id, nickname, now_ms());
        impl_->lanchat_member_by_peer[sender_peer_id] = member_id;
        if (impl_->lanchat_event_callback) {
            impl_->lanchat_event_callback(
                member_update_event_json(impl_->lanchat.members(), local_peer_id()));
        }
        if (impl_->session_role == SessionRole::Host) {
            auto members_packet = Network::build_chat_member_update(
                impl_->lanchat.room_id(), impl_->lanchat.members());
            impl_->peer_manager.broadcast(
                Network::kChannelReliable, members_packet.data(), members_packet.size(), true);
            const auto* peer_info = impl_->peer_manager.find_peer(sender_peer_id);
            if (peer_info && peer_info->peer) {
                auto history_packet = Network::build_chat_history_snapshot_v2(
                    impl_->lanchat.room_id(), impl_->lanchat.history());
                impl_->peer_manager.send_to(
                    peer_info->peer, Network::kChannelReliable,
                    history_packet.data(), history_packet.size(), true);
            } else {
                CFW_LOG_WARNING(
                    "NetworkSystem: LANChat join peer {} unavailable; skipped history snapshot",
                    sender_peer_id);
            }
            for (const auto& agent : impl_->lanchat.agents()) {
                auto packet = Network::build_chat_agent_register(
                    impl_->lanchat.room_id(), agent.agent_id, agent.name,
                    agent.persona, agent.owner_id);
                if (peer_info && peer_info->peer) {
                    impl_->peer_manager.send_to(
                        peer_info->peer, Network::kChannelReliable,
                        packet.data(), packet.size(), true);
                }
            }
        }
    } else if (mt == MessageType::CHAT_JOIN_REJECT) {
        if (impl_->session_role != SessionRole::Client) return;
        if (!is_message_from_connected_host(sender_peer_id)) return;
        Network::BufferReader r(data + 1, len - 1);
        if (!r.has_remaining(2)) return;
        uint16_t room_len = r.read_u16();
        if (!r.has_remaining(room_len + 2)) return;
        std::string room_id = r.read_string(room_len);
        uint16_t code_len = r.read_u16();
        if (!r.has_remaining(code_len + 2)) return;
        std::string code = r.read_string(code_len);
        uint16_t reason_len = r.read_u16();
        if (!r.has_remaining(reason_len)) return;
        std::string reason = r.read_string(reason_len);
        if (!impl_->lanchat.room_id().empty() && impl_->lanchat.room_id() != room_id) return;
        impl_->lanchat.close_room();
        impl_->lanchat_member_by_peer.clear();
        impl_->lanchat_join_pending = false;
        impl_->lanchat_join_member_snapshot_received = false;
        impl_->lanchat_join_history_snapshot_received = false;
        if (impl_->lanchat_event_callback) {
            impl_->lanchat_event_callback(lanchat_error_event_json(code, reason));
        }
    } else if (mt == MessageType::CHAT_LEAVE) {
        Network::BufferReader r(data + 1, len - 1);
        if (!r.has_remaining(2)) return;
        uint16_t room_len = r.read_u16();
        if (!r.has_remaining(room_len + 2)) return;
        std::string room_id = r.read_string(room_len);
        uint16_t member_len = r.read_u16();
        if (!r.has_remaining(member_len)) return;
        std::string member_id = r.read_string(member_len);
        if (impl_->lanchat.room_id() != room_id) return;
        if (impl_->session_role == SessionRole::Client && member_id != local_peer_id()) {
            const auto* peer_info = impl_->peer_manager.find_peer(sender_peer_id);
            if (peer_info && is_connected_host_peer(*peer_info)) {
                notify_lanchat_room_closed();
                clear_lanchat_room_state();
                impl_->stop_session_after_peer_disconnect = true;
                return;
            }
        }
        impl_->lanchat.leave_member(member_id);
        for (auto it = impl_->lanchat_member_by_peer.begin();
             it != impl_->lanchat_member_by_peer.end(); ) {
            if (it->first == sender_peer_id || it->second == member_id) {
                it = impl_->lanchat_member_by_peer.erase(it);
            } else {
                ++it;
            }
        }
        if (impl_->lanchat_event_callback) {
            impl_->lanchat_event_callback(
                member_update_event_json(impl_->lanchat.members(), local_peer_id()));
        }
        if (impl_->session_role == SessionRole::Host) {
            auto packet = Network::build_chat_member_update(
                impl_->lanchat.room_id(), impl_->lanchat.members());
            impl_->peer_manager.broadcast(
                Network::kChannelReliable, packet.data(), packet.size(), true);
        }
    } else if (mt == MessageType::CHAT_MEMBER_UPDATE) {
        if (impl_->session_role == SessionRole::Client && !is_message_from_connected_host(sender_peer_id)) return;
        Network::BufferReader r(data + 1, len - 1);
        if (!r.has_remaining(2)) return;
        uint16_t room_len = r.read_u16();
        if (!r.has_remaining(room_len + 2)) return;
        std::string room_id = r.read_string(room_len);
        uint16_t member_count = r.read_u16();
        std::vector<Network::LanChatMember> members;
        members.reserve(member_count);
        for (uint16_t i = 0; i < member_count; ++i) {
            if (!r.has_remaining(2)) return;
            uint16_t member_id_len = r.read_u16();
            if (!r.has_remaining(member_id_len + 2)) return;
            Network::LanChatMember member;
            member.member_id = r.read_string(member_id_len);
            uint16_t nickname_len = r.read_u16();
            if (!r.has_remaining(nickname_len + 2)) return;
            member.nickname = r.read_string(nickname_len);
            uint16_t status_len = r.read_u16();
            if (!r.has_remaining(status_len + 8)) return;
            member.status = r.read_string(status_len);
            member.last_seen_ms = r.read_u64();
            members.push_back(std::move(member));
        }
        if (impl_->lanchat.room_id() != room_id) return;
        impl_->lanchat.apply_member_snapshot(members);
        for (const auto& member : members) {
            if (member.member_id == local_peer_id() && !member.nickname.empty()) {
                impl_->lanchat_nickname = member.nickname;
                break;
            }
        }
        if (impl_->session_role == SessionRole::Client && impl_->lanchat_join_pending) {
            impl_->lanchat_join_member_snapshot_received = true;
            complete_lanchat_join_if_ready(
                impl_->lanchat_join_pending,
                impl_->lanchat_join_member_snapshot_received,
                impl_->lanchat_join_history_snapshot_received);
        }
        if (impl_->lanchat_event_callback) {
            impl_->lanchat_event_callback(
                member_update_event_json(impl_->lanchat.members(), local_peer_id()));
        }
    } else if (mt == MessageType::CHAT_HISTORY_SNAPSHOT ||
               mt == MessageType::CHAT_HISTORY_SNAPSHOT_V2) {
        if (impl_->session_role == SessionRole::Host) return;
        if (impl_->session_role == SessionRole::Client && !is_message_from_connected_host(sender_peer_id)) return;
        Network::BufferReader r(data + 1, len - 1);
        if (!r.has_remaining(2)) return;
        uint16_t room_len = r.read_u16();
        if (!r.has_remaining(room_len + 2)) return;
        std::string room_id = r.read_string(room_len);
        uint16_t message_count = r.read_u16();
        std::vector<Network::LanChatMessage> history;
        history.reserve(message_count);
        const bool structured = mt == MessageType::CHAT_HISTORY_SNAPSHOT_V2;
        for (uint16_t i = 0; i < message_count; ++i) {
            Network::LanChatMessage message;
            if (!read_chat_message(r, message, structured)) return;
            history.push_back(std::move(message));
        }
        if (impl_->lanchat.room_id() != room_id) return;
        impl_->lanchat.apply_history_snapshot(history);
        if (impl_->session_role == SessionRole::Client && impl_->lanchat_join_pending) {
            impl_->lanchat_join_history_snapshot_received = true;
            complete_lanchat_join_if_ready(
                impl_->lanchat_join_pending,
                impl_->lanchat_join_member_snapshot_received,
                impl_->lanchat_join_history_snapshot_received);
        }
        if (impl_->lanchat_event_callback) {
            impl_->lanchat_event_callback(
                history_snapshot_event_json(impl_->lanchat.history()));
        }
    } else if (mt == MessageType::CHAT_MESSAGE || mt == MessageType::CHAT_AGENT_REPLY ||
               mt == MessageType::CHAT_MESSAGE_V2 || mt == MessageType::CHAT_AGENT_REPLY_V2) {
        if (impl_->session_role == SessionRole::Client && !is_message_from_connected_host(sender_peer_id)) return;
        Network::BufferReader r(data + 1, len - 1);
        Network::LanChatMessage message;
        const bool structured =
            mt == MessageType::CHAT_MESSAGE_V2 || mt == MessageType::CHAT_AGENT_REPLY_V2;
        if (!read_chat_message(r, message, structured)) return;
        if (!structured && mt == MessageType::CHAT_AGENT_REPLY) {
            message.sender_type = "agent";
            message.message_kind = "agent_reply";
        }

        if (impl_->lanchat.room_id() != message.room_id) return;
        Network::LanChatMessageResult result;
        if (impl_->session_role == SessionRole::Host) {
            result = impl_->lanchat.record_message_ex(
                message.message_id, message.sender_id, message.sender_name,
                message.text, message.timestamp_ms, message.sender_type,
                message.message_kind, message.target_agent_id, message.source_user_id,
                message.correlation_id, message.metadata_json);
        } else {
            result = impl_->lanchat.apply_remote_message(message);
        }
        if (result.accepted && impl_->lanchat_event_callback) {
            impl_->lanchat_event_callback(message_event_json(result.message));
        }
        if (result.accepted && impl_->session_role == SessionRole::Host) {
            persist_lanchat_message(result.message);
        }
        if (result.accepted &&
            (mt == MessageType::CHAT_MESSAGE || mt == MessageType::CHAT_MESSAGE_V2)) {
            const bool allow_agent_execution = impl_->session_role != SessionRole::Client;
            const bool allow_generation_start = allow_agent_execution;
            impl_->lanchat.enqueue_agent_triggers_for_message(
                result.message,
                local_peer_id(),
                /*is_agent_reply=*/false,
                allow_agent_execution,
                allow_generation_start);
        }
        if (result.accepted && impl_->session_role == SessionRole::Host) {
            auto packet = build_chat_packet_for_type(result.message, mt);
            impl_->peer_manager.broadcast(
                Network::kChannelReliable, packet.data(), packet.size(), true);
        }
    } else if (mt == MessageType::CHAT_AGENT_REGISTER) {
        if (impl_->session_role == SessionRole::Client && !is_message_from_connected_host(sender_peer_id)) return;
        Network::BufferReader r(data + 1, len - 1);
        if (!r.has_remaining(2)) return;
        uint16_t room_len = r.read_u16();
        if (!r.has_remaining(room_len + 2)) return;
        std::string room_id = r.read_string(room_len);
        uint16_t agent_id_len = r.read_u16();
        if (!r.has_remaining(agent_id_len + 2)) return;
        std::string agent_id = r.read_string(agent_id_len);
        uint16_t name_len = r.read_u16();
        if (!r.has_remaining(name_len + 2)) return;
        std::string name = r.read_string(name_len);
        uint16_t persona_len = r.read_u16();
        if (!r.has_remaining(persona_len + 2)) return;
        std::string persona = r.read_string(persona_len);
        uint16_t owner_len = r.read_u16();
        if (!r.has_remaining(owner_len)) return;
        std::string owner = r.read_string(owner_len);
        if (impl_->lanchat.room_id() != room_id) return;
        auto result = impl_->lanchat.register_agent(agent_id, name, persona, owner);
        if (result.ok && impl_->lanchat_event_callback) {
            impl_->lanchat_event_callback(
                "{\"channel\":\"lanchat\",\"event\":\"agent_roster\",\"agents\":" +
                agents_json(impl_->lanchat.agents()) + "}");
        }
        if (result.ok && impl_->session_role == SessionRole::Host) {
            impl_->peer_manager.broadcast(Network::kChannelReliable, data, len, true);
        }
    } else if (mt == MessageType::CHAT_AGENT_REMOVE) {
        if (impl_->session_role == SessionRole::Client && !is_message_from_connected_host(sender_peer_id)) return;
        Network::BufferReader r(data + 1, len - 1);
        if (!r.has_remaining(2)) return;
        uint16_t room_len = r.read_u16();
        if (!r.has_remaining(room_len + 2)) return;
        std::string room_id = r.read_string(room_len);
        uint16_t agent_id_len = r.read_u16();
        if (!r.has_remaining(agent_id_len)) return;
        std::string agent_id = r.read_string(agent_id_len);
        if (impl_->lanchat.room_id() != room_id) return;
        auto result = impl_->lanchat.remove_agent(agent_id);
        if (result.ok && impl_->lanchat_event_callback) {
            impl_->lanchat_event_callback(
                "{\"channel\":\"lanchat\",\"event\":\"agent_roster\",\"agents\":" +
                agents_json(impl_->lanchat.agents()) + "}");
        }
        if (result.ok && impl_->session_role == SessionRole::Host) {
            impl_->peer_manager.broadcast(Network::kChannelReliable, data, len, true);
        }
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

        Impl::PendingAction pa;
        pa.actor_guid = ft_it->second.actor_guid;
        pa.scene_name = ft_it->second.scene_name;
        pa.model_path = ft_it->second.model_path;
        pa.dependency_paths = ft_it->second.dependency_paths;
        pa.actor_json = ft_it->second.actor_json;
        pa.actor_packed = ft_it->second.actor_packed;
        impl_->pending_actor_creates.push_back(pa);
        impl_->pending_file_transfer_groups.erase(ft_it);
    }
}

}  // namespace Corona::Systems
