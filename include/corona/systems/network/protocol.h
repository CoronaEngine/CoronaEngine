#pragma once

#include <cstdint>
#include <cstring>
#include <corona/systems/network/lanchat_state.h>
#include <string>
#include <optional>
#include <vector>

namespace Corona::Network {

// ============================================================================
// Protocol version — increment when binary format changes
// ============================================================================
constexpr uint8_t kProtocolVersion = 1;

// ============================================================================
// Default port for ENet communication
// ============================================================================
constexpr uint16_t kDefaultPort = 27960;

// ============================================================================
// Legacy discovery constants. NetworkSystem no longer starts LAN broadcast
// discovery; direct IP joining is the active connection path.
// ============================================================================
constexpr uint16_t kDiscoveryPortOffset = 1;

// ============================================================================
// Legacy discovery broadcast interval (ms)
// ============================================================================
constexpr int kDiscoveryIntervalMs = 500;

// ============================================================================
// Peer heartbeat interval (ms) and timeout (ms)
// ============================================================================
constexpr int kHeartbeatIntervalMs = 1000;
constexpr int kPeerTimeoutMs = 3000;

// ============================================================================
// Sync tick interval (ms) — ~60 Hz dirty polling
// ============================================================================
constexpr int kSyncIntervalMs = 16;

// ============================================================================
// ENet channel allocation
// ============================================================================
constexpr int kChannelReliable = 0;    // SYNC_DIRTY, SYNC_FULL
constexpr int kChannelUnreliable = 1;  // HEARTBEAT

// Bounds for editor-state packets. Values are deliberately conservative so a
// malformed peer cannot force unbounded allocations during decode.
constexpr uint16_t kMaxEditorSyncStringBytes = 1024;
constexpr uint32_t kMaxEditorSyncValueBytes = 1024 * 1024;
constexpr uint32_t kMaxEditorSyncOperations = 4096;
constexpr uint8_t kEditorSyncSchemaVersion = 1;
constexpr uint8_t kEditorSnapshotSchemaVersion = 2;
constexpr uint32_t kEditorSnapshotChunkOperations = 256;
constexpr uint32_t kMaxEditorSnapshotChunkBytes = 1024 * 1024;
constexpr uint16_t kMaxEditorSnapshotChunks =
    static_cast<uint16_t>(kMaxEditorSyncOperations);
constexpr uint64_t kEditorSnapshotTimeoutMs = 5000;

// ============================================================================
// Message types (single byte prefix on every packet)
// ============================================================================
enum class MessageType : uint8_t {
    SYNC_DIRTY    = 0x01,  // Incremental dirty sync
    SYNC_FULL     = 0x02,  // Full state snapshot (new peer joins)
    HEARTBEAT     = 0x03,  // Keep-alive
    HELLO         = 0x04,  // Post-connect handshake: exchange stable identity
    EDITOR_SYNC   = 0x05,  // Versioned collaborative editor state operation
    EDITOR_SNAPSHOT_REQUEST = 0x06, // Request current versioned editor state
    EDITOR_SNAPSHOT_CHUNK = 0x07, // Chunked versioned editor state
    EDITOR_SNAPSHOT_BEGIN = 0x08, // Begin bounded editor snapshot session
    EDITOR_SNAPSHOT_END = 0x09, // End editor snapshot session
    ACTOR_CREATE  = 0x10,  // Actor creation event (scene_name + model_path + transform + optics)
    FILE_REQUEST  = 0x11,  // Request model file from peer
    FILE_CHUNK    = 0x12,  // File chunk transfer
    OWNERSHIP_CLAIM = 0x13, // Actor ownership handoff claim
    ACTOR_TRANSFORM_UPDATE = 0x14, // Actor transform delta (demo-grade SceneDelta)
    ACTOR_DELETE = 0x15, // Actor deletion event (actor_guid + scene_name + actor_name)
    ACTOR_SCENE_SNAPSHOT_REQUEST = 0x16, // Request host actor scene snapshot
    ACTOR_SCENE_SNAPSHOT = 0x17, // Actor scene snapshot JSON payload
    ACTOR_STATE_UPDATE = 0x18, // Lightweight actor metadata/state JSON payload
    CHAT_JOIN     = 0x20,  // LANChat room join/member snapshot
    CHAT_LEAVE    = 0x21,  // LANChat room leave
    CHAT_MESSAGE  = 0x22,  // LANChat user/agent message
    CHAT_MEMBER_UPDATE = 0x23, // LANChat member status update
    CHAT_AGENT_REGISTER = 0x24, // LANChat agent roster add/update
    CHAT_AGENT_REMOVE = 0x25, // LANChat agent roster remove
    CHAT_AGENT_TRIGGER = 0x26, // LANChat agent invocation request
    CHAT_AGENT_REPLY = 0x27, // LANChat agent invocation reply
    CHAT_HISTORY_SNAPSHOT = 0x28, // LANChat full message history snapshot
    CHAT_JOIN_REJECT = 0x29, // LANChat join rejected by host
    CHAT_MESSAGE_V2 = 0x2A, // LANChat structured user/system message
    CHAT_AGENT_REPLY_V2 = 0x2B, // LANChat structured agent/system reply
    CHAT_HISTORY_SNAPSHOT_V2 = 0x2C, // LANChat structured history snapshot
};

// ============================================================================
// Storage ID — maps to a SharedDataHub Storage type
// ============================================================================
enum class StorageID : uint16_t {
    ST_MODEL_TRANSFORM = 0,
    ST_GEOMETRY        = 1,
    ST_OPTICS          = 2,
    ST_MECHANICS       = 3,
    ST_ACOUSTICS       = 4,
    ST_SCENE           = 5,
    ST_CAMERA          = 6,
    ST_ACTOR           = 7,
    ST_ENVIRONMENT     = 8,
    ST_MODEL_RESOURCE  = 9,
};

enum class EditorSyncOperationKind : uint8_t {
    Upsert = 1,
    Delete = 2,
};

struct LwwVersion {
    uint64_t counter = 0;
    std::string writer_peer_id;
};

struct EditorSyncOperation {
    EditorSyncOperationKind kind = EditorSyncOperationKind::Upsert;
    std::string actor_guid;
    std::string field_name;
    std::vector<uint8_t> value;
    LwwVersion version;
};

// ============================================================================
// Discovery broadcast packet (UDP, plain struct, fixed layout)
// ============================================================================
struct DiscoveryPacket {
    char magic[6] = {'C','O','R','O','N','A'};  // Magic identifier
    uint8_t protocol_version = kProtocolVersion;
    char instance_name[32] = {};
    uint64_t project_id = 0;
};

// ============================================================================
// SYNC_DIRTY header (binary, after MessageType byte)
// ============================================================================
struct SyncDirtyHeader {
    uint32_t seq = 0;
    uint64_t timestamp_ms = 0;
    uint32_t count = 0;
};

// ============================================================================
// A single dirty entry header (followed by key/value payload)
// ============================================================================
struct SyncDirtyEntryHeader {
    uint16_t storage_id = 0;
    uint64_t entity_id = 0;
    uint16_t key_len = 0;
    uint16_t value_len = 0;
    // Followed by: char[key_len] key, char[value_len] value
};

// ============================================================================
// SYNC_FULL header — full state snapshot
// ============================================================================
struct SyncFullHeader {
    uint32_t seq = 0;
    uint32_t count = 0;
    // Followed by count entries in same format as SYNC_DIRTY
};

// ============================================================================
// Peer ID helper — unique per instance, used for LWW tiebreaking
// ============================================================================
inline std::string make_peer_id(const char* ip, uint16_t port) {
    return std::string(ip) + ":" + std::to_string(port);
}

// ============================================================================
// Compact binary serialization helpers
// ============================================================================

inline void write_u8(std::vector<uint8_t>& buf, uint8_t v) { buf.push_back(v); }
inline void write_u16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
inline void write_u32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}
inline void write_u64(std::vector<uint8_t>& buf, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    }
}
inline void write_bytes(std::vector<uint8_t>& buf, const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    buf.insert(buf.end(), p, p + len);
}
inline void write_string(std::vector<uint8_t>& buf, const std::string& s) {
    write_u16(buf, static_cast<uint16_t>(s.size()));
    write_bytes(buf, s.data(), s.size());
}
inline void write_string_u32(std::vector<uint8_t>& buf, const std::string& s) {
    write_u32(buf, static_cast<uint32_t>(s.size()));
    write_bytes(buf, s.data(), s.size());
}

struct BufferReader {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;

    BufferReader(const void* d, size_t s)
        : data(static_cast<const uint8_t*>(d)), size(s) {}

    bool has_remaining(size_t n) const { return pos + n <= size; }

    uint8_t read_u8() {
        return data[pos++];
    }
    uint16_t read_u16() {
        uint16_t v = data[pos] | (static_cast<uint16_t>(data[pos + 1]) << 8);
        pos += 2;
        return v;
    }
    uint32_t read_u32() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(data[pos + i]) << (i * 8);
        pos += 4;
        return v;
    }
    uint64_t read_u64() {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(data[pos + i]) << (i * 8);
        pos += 8;
        return v;
    }
    std::string read_string(size_t len) {
        std::string s(reinterpret_cast<const char*>(data + pos), len);
        pos += len;
        return s;
    }
};

inline bool valid_editor_sync_operation(const EditorSyncOperation& op) {
    if (op.actor_guid.empty() || op.actor_guid.size() > kMaxEditorSyncStringBytes ||
        op.version.writer_peer_id.empty() ||
        op.version.writer_peer_id.size() > kMaxEditorSyncStringBytes) {
        return false;
    }
    if (op.kind == EditorSyncOperationKind::Upsert) {
        if (op.field_name.empty() || op.field_name.size() > kMaxEditorSyncStringBytes ||
            op.value.size() > kMaxEditorSyncValueBytes) {
            return false;
        }
    } else if (op.kind == EditorSyncOperationKind::Delete) {
        if (!op.field_name.empty() || !op.value.empty()) return false;
    } else {
        return false;
    }
    return true;
}

inline std::vector<uint8_t> build_editor_sync_operation(
    const EditorSyncOperation& op) {
    if (!valid_editor_sync_operation(op)) return {};
    std::vector<uint8_t> buf;
    buf.reserve(1 + 1 + 1 + 2 + op.actor_guid.size() + 2 + op.field_name.size() +
                4 + op.value.size() + 8 + 2 + op.version.writer_peer_id.size());
    write_u8(buf, static_cast<uint8_t>(MessageType::EDITOR_SYNC));
    write_u8(buf, kEditorSyncSchemaVersion);
    write_u8(buf, static_cast<uint8_t>(op.kind));
    write_u16(buf, static_cast<uint16_t>(op.actor_guid.size()));
    write_bytes(buf, op.actor_guid.data(), op.actor_guid.size());
    write_u16(buf, static_cast<uint16_t>(op.field_name.size()));
    write_bytes(buf, op.field_name.data(), op.field_name.size());
    write_u32(buf, static_cast<uint32_t>(op.value.size()));
    write_bytes(buf, op.value.data(), op.value.size());
    write_u64(buf, op.version.counter);
    write_u16(buf, static_cast<uint16_t>(op.version.writer_peer_id.size()));
    write_bytes(buf, op.version.writer_peer_id.data(), op.version.writer_peer_id.size());
    return buf;
}

inline std::optional<EditorSyncOperation> parse_editor_sync_operation(
    const uint8_t* data, size_t len) {
    if (!data || len < 1 + 1 + 1 + 2 + 2 + 4 + 8 + 2) return std::nullopt;
    BufferReader r(data, len);
    if (r.read_u8() != static_cast<uint8_t>(MessageType::EDITOR_SYNC)) return std::nullopt;
    if (r.read_u8() != kEditorSyncSchemaVersion) return std::nullopt;
    EditorSyncOperation op;
    op.kind = static_cast<EditorSyncOperationKind>(r.read_u8());
    if (!r.has_remaining(2)) return std::nullopt;
    const auto guid_len = r.read_u16();
    if (guid_len == 0 || guid_len > kMaxEditorSyncStringBytes || !r.has_remaining(guid_len)) return std::nullopt;
    op.actor_guid = r.read_string(guid_len);
    if (!r.has_remaining(2)) return std::nullopt;
    const auto field_len = r.read_u16();
    if (field_len > kMaxEditorSyncStringBytes || !r.has_remaining(field_len)) return std::nullopt;
    op.field_name = r.read_string(field_len);
    if (!r.has_remaining(4)) return std::nullopt;
    const auto value_len = r.read_u32();
    if (value_len > kMaxEditorSyncValueBytes || !r.has_remaining(value_len)) return std::nullopt;
    op.value.assign(r.data + r.pos, r.data + r.pos + value_len);
    r.pos += value_len;
    if (!r.has_remaining(8 + 2)) return std::nullopt;
    op.version.counter = r.read_u64();
    const auto writer_len = r.read_u16();
    if (writer_len == 0 || writer_len > kMaxEditorSyncStringBytes || !r.has_remaining(writer_len)) return std::nullopt;
    op.version.writer_peer_id = r.read_string(writer_len);
    if (r.pos != r.size || !valid_editor_sync_operation(op)) return std::nullopt;
    return op;
}

// ============================================================================
// SYNC_DIRTY message builder
// ============================================================================
inline std::vector<uint8_t> build_sync_dirty(
    uint32_t seq,
    uint64_t timestamp_ms,
    const std::vector<uint8_t>& entries_payload,
    uint32_t entry_count)
{
    std::vector<uint8_t> buf;
    buf.reserve(1 + 16 + entries_payload.size());

    write_u8(buf, static_cast<uint8_t>(MessageType::SYNC_DIRTY));
    write_u32(buf, seq);
    write_u64(buf, timestamp_ms);
    write_u32(buf, entry_count);
    write_bytes(buf, entries_payload.data(), entries_payload.size());

    return buf;
}

// ============================================================================
// SYNC_DIRTY entry builder helper
// ============================================================================
inline std::vector<uint8_t> build_dirty_entries(
    StorageID storage_id,
    uint64_t entity_id,
    const char* key, uint16_t key_len,
    const void* value, uint16_t value_len)
{
    std::vector<uint8_t> buf;
    buf.reserve(4 + 8 + 2 + 2 + key_len + value_len);

    write_u16(buf, static_cast<uint16_t>(storage_id));
    write_u64(buf, entity_id);
    write_u16(buf, key_len);
    write_u16(buf, value_len);
    write_bytes(buf, key, key_len);
    write_bytes(buf, value, value_len);

    return buf;
}

// ============================================================================
// SYNC_FULL message builder
// ============================================================================
inline std::vector<uint8_t> build_sync_full(
    uint32_t seq,
    const std::vector<uint8_t>& entries_payload,
    uint32_t entry_count)
{
    std::vector<uint8_t> buf;
    buf.reserve(1 + 4 + 4 + entries_payload.size());

    write_u8(buf, static_cast<uint8_t>(MessageType::SYNC_FULL));
    write_u32(buf, seq);
    write_u32(buf, entry_count);
    write_bytes(buf, entries_payload.data(), entries_payload.size());

    return buf;
}

// ============================================================================
// Heartbeat packet
// ============================================================================
inline std::vector<uint8_t> build_heartbeat(uint32_t seq) {
    std::vector<uint8_t> buf;
    buf.reserve(1 + 4);
    write_u8(buf, static_cast<uint8_t>(MessageType::HEARTBEAT));
    write_u32(buf, seq);
    return buf;
}

inline std::vector<uint8_t> build_editor_snapshot_request() {
    return {static_cast<uint8_t>(MessageType::EDITOR_SNAPSHOT_REQUEST),
            kEditorSnapshotSchemaVersion};
}

inline std::vector<uint8_t> build_editor_snapshot_begin(
    uint32_t snapshot_id, uint16_t chunk_total, uint32_t operation_total) {
    if (chunk_total == 0 || chunk_total > kMaxEditorSnapshotChunks ||
        operation_total > kMaxEditorSyncOperations) return {};
    std::vector<uint8_t> buf;
    write_u8(buf, static_cast<uint8_t>(MessageType::EDITOR_SNAPSHOT_BEGIN));
    write_u8(buf, kEditorSnapshotSchemaVersion);
    write_u32(buf, snapshot_id);
    write_u16(buf, chunk_total);
    write_u32(buf, operation_total);
    return buf;
}

inline std::vector<uint8_t> build_editor_snapshot_end(uint32_t snapshot_id) {
    std::vector<uint8_t> buf;
    write_u8(buf, static_cast<uint8_t>(MessageType::EDITOR_SNAPSHOT_END));
    write_u8(buf, kEditorSnapshotSchemaVersion);
    write_u32(buf, snapshot_id);
    return buf;
}

inline std::vector<uint8_t> build_editor_snapshot_chunk(
    uint32_t snapshot_id, uint16_t chunk_index, uint16_t chunk_total,
    const std::vector<std::vector<uint8_t>>& operations) {
    if (chunk_total == 0 || chunk_index >= chunk_total ||
        operations.size() > kEditorSnapshotChunkOperations) return {};
    std::vector<uint8_t> buf;
    write_u8(buf, static_cast<uint8_t>(MessageType::EDITOR_SNAPSHOT_CHUNK));
    write_u8(buf, kEditorSnapshotSchemaVersion);
    write_u32(buf, snapshot_id);
    write_u16(buf, chunk_index);
    write_u16(buf, chunk_total);
    write_u16(buf, static_cast<uint16_t>(operations.size()));
    for (const auto& operation : operations) {
        if (operation.empty() || operation.size() > kMaxEditorSyncValueBytes) return {};
        write_u32(buf, static_cast<uint32_t>(operation.size()));
        write_bytes(buf, operation.data(), operation.size());
        if (buf.size() > kMaxEditorSnapshotChunkBytes) return {};
    }
    return buf;
}

// ============================================================================
// HELLO handshake packet
// ============================================================================
// Sent immediately after an ENet connection is established, in both directions.
// Carries the sender's stable identity so the receiver can rekey the peer:
// ENet's inbound CONNECT event only exposes the remote's ephemeral source port,
// not its listen port, so peer ids would otherwise be asymmetric between the
// two ends. After HELLO, both ends agree on a routable connection id of
// "name@ip:listen_port" and authenticate LWW writes with "name@listen_port".
//   [1B type=0x04] [2B name_len] [instance_name] [2B listen_port]
// ============================================================================
inline std::vector<uint8_t> build_hello(const std::string& instance_name,
                                        uint16_t listen_port) {
    std::vector<uint8_t> buf;
    buf.reserve(1 + 2 + instance_name.size() + 2);
    write_u8(buf, static_cast<uint8_t>(MessageType::HELLO));
    write_string(buf, instance_name);
    write_u16(buf, listen_port);
    return buf;
}

// ============================================================================
// ACTOR_CREATE message builder
// ============================================================================
// Wire format:
//   [1B type] [2B actor_guid_len] [actor_guid]
//   [2B scene_name_len] [scene_name] [2B model_path_len] [model_path]
//   [36B transform (9 floats)] [ActorCreatePacked payload]
//   [2B dependency_count] [[2B dependency_len] [dependency]]*
//   Optional v2 tail: [4B actor_json_len] [actor_json]
// The payload keeps legacy optics fields in ActorCreatePacked layout; receivers
// must copy the leading 36B wire transform into ActorCreatePacked::transform.
// ============================================================================
struct ActorCreatePacked {
    float transform[9];  // pos(3) + rot(3) + scale(3)
    // OpticsPacked — see sync_engine.cpp
    bool visible;
    bool bEnableLighting;
    float metallic;
    float roughness;
    float subsurface;
    float specular;
    float specularTint;
    float anisotropic;
    float sheen;
    float sheenTint;
    float clearcoat;
    float clearcoatGloss;
    float ambient[3];
    float diffuse[3];
    float specular_color[3];
    float shininess;
};

inline std::vector<uint8_t> build_actor_create(
    const std::string& actor_guid,
    const std::string& scene_name,
    const std::string& model_path,
    const float* transform,  // 9 floats
    const void* optics_packed, size_t optics_size,
    const std::vector<std::string>& dependency_paths = {},
    const std::string& actor_json = {})
{
    std::vector<uint8_t> buf;
    size_t deps_size = 2;
    for (const auto& dep : dependency_paths) {
        deps_size += 2 + dep.size();
    }
    buf.reserve(1 + 2 + actor_guid.size() + 2 + scene_name.size() +
                2 + model_path.size() + 36 + optics_size + deps_size +
                4 + actor_json.size());

    write_u8(buf, static_cast<uint8_t>(MessageType::ACTOR_CREATE));
    write_string(buf, actor_guid);
    write_string(buf, scene_name);
    write_string(buf, model_path);
    write_bytes(buf, transform, 36);
    write_bytes(buf, optics_packed, optics_size);
    write_u16(buf, static_cast<uint16_t>(dependency_paths.size()));
    for (const auto& dep : dependency_paths) {
        write_string(buf, dep);
    }
    write_string_u32(buf, actor_json);

    return buf;
}

// ============================================================================
// ACTOR_TRANSFORM_UPDATE message builder
// ============================================================================
// Wire format:
//   [1B type] [2B actor_guid_len] [actor_guid]
//   [2B scene_name_len] [scene_name] [36B transform (9 floats)]
//   [2B source_user_id_len] [source_user_id]
//   [2B correlation_id_len] [correlation_id]
// ============================================================================
inline std::vector<uint8_t> build_actor_transform_update(
    const std::string& actor_guid,
    const std::string& scene_name,
    const float* transform,  // 9 floats
    const std::string& source_user_id = {},
    const std::string& correlation_id = {})
{
    std::vector<uint8_t> buf;
    buf.reserve(1 + 2 + actor_guid.size() + 2 + scene_name.size() + 36 +
                2 + source_user_id.size() + 2 + correlation_id.size());
    write_u8(buf, static_cast<uint8_t>(MessageType::ACTOR_TRANSFORM_UPDATE));
    write_string(buf, actor_guid);
    write_string(buf, scene_name);
    write_bytes(buf, transform, 36);
    write_string(buf, source_user_id);
    write_string(buf, correlation_id);
    return buf;
}

// ============================================================================
// ACTOR_DELETE message builder
// ============================================================================
// Wire format:
//   [1B type] [2B actor_guid_len] [actor_guid]
//   [2B scene_name_len] [scene_name] [2B actor_name_len] [actor_name]
// ============================================================================
inline std::vector<uint8_t> build_actor_delete(
    const std::string& actor_guid,
    const std::string& scene_name,
    const std::string& actor_name)
{
    std::vector<uint8_t> buf;
    buf.reserve(1 + 2 + actor_guid.size() + 2 + scene_name.size() +
                2 + actor_name.size());
    write_u8(buf, static_cast<uint8_t>(MessageType::ACTOR_DELETE));
    write_string(buf, actor_guid);
    write_string(buf, scene_name);
    write_string(buf, actor_name);
    return buf;
}

// ============================================================================
// ACTOR_SCENE_SNAPSHOT_REQUEST message builder
// ============================================================================
// Wire format:
//   [1B type] [2B scene_name_len] [scene_name]
// ============================================================================
inline std::vector<uint8_t> build_actor_scene_snapshot_request(
    const std::string& scene_name)
{
    std::vector<uint8_t> buf;
    buf.reserve(1 + 2 + scene_name.size());
    write_u8(buf, static_cast<uint8_t>(MessageType::ACTOR_SCENE_SNAPSHOT_REQUEST));
    write_string(buf, scene_name);
    return buf;
}

// ============================================================================
// ACTOR_SCENE_SNAPSHOT message builder
// ============================================================================
// Wire format:
//   [1B type] [2B scene_name_len] [scene_name] [4B json_len] [json]
// ============================================================================
inline std::vector<uint8_t> build_actor_scene_snapshot(
    const std::string& scene_name,
    const std::string& snapshot_json)
{
    std::vector<uint8_t> buf;
    buf.reserve(1 + 2 + scene_name.size() + 4 + snapshot_json.size());
    write_u8(buf, static_cast<uint8_t>(MessageType::ACTOR_SCENE_SNAPSHOT));
    write_string(buf, scene_name);
    write_string_u32(buf, snapshot_json);
    return buf;
}

// ============================================================================
// ACTOR_STATE_UPDATE message builder
// ============================================================================
// Wire format:
//   [1B type] [2B actor_guid_len] [actor_guid]
//   [2B scene_name_len] [scene_name] [4B json_len] [json]
// ============================================================================
inline std::vector<uint8_t> build_actor_state_update(
    const std::string& actor_guid,
    const std::string& scene_name,
    const std::string& actor_json)
{
    std::vector<uint8_t> buf;
    buf.reserve(1 + 2 + actor_guid.size() + 2 + scene_name.size() +
                4 + actor_json.size());
    write_u8(buf, static_cast<uint8_t>(MessageType::ACTOR_STATE_UPDATE));
    write_string(buf, actor_guid);
    write_string(buf, scene_name);
    write_string_u32(buf, actor_json);
    return buf;
}

// ============================================================================
// FILE_REQUEST message builder
// ============================================================================
// Wire format:
//   [1B type] [8B transfer_id] [2B model_path_len] [model_path]
inline std::vector<uint8_t> build_file_request(uint64_t transfer_id,
                                               const std::string& model_path) {
    std::vector<uint8_t> buf;
    buf.reserve(1 + 8 + 2 + model_path.size());
    write_u8(buf, static_cast<uint8_t>(MessageType::FILE_REQUEST));
    write_u64(buf, transfer_id);
    write_string(buf, model_path);
    return buf;
}

// ============================================================================
// OWNERSHIP_CLAIM message builder
// ============================================================================
// Wire format:
//   [1B type] [2B actor_guid_len] [actor_guid]
// ============================================================================
inline std::vector<uint8_t> build_ownership_claim(const std::string& actor_guid) {
    std::vector<uint8_t> buf;
    buf.reserve(1 + 2 + actor_guid.size());
    write_u8(buf, static_cast<uint8_t>(MessageType::OWNERSHIP_CLAIM));
    write_string(buf, actor_guid);
    return buf;
}

// ============================================================================
// LANChat message builders
// ============================================================================
// CHAT_MESSAGE wire format:
//   [1B type] [2B message_id_len] [message_id]
//   [2B sender_id_len] [sender_id] [2B room_id_len] [room_id]
//   [8B seq] [2B sender_name_len] [sender_name]
//   [2B text_len] [text] [8B timestamp_ms]
// ============================================================================
inline std::vector<uint8_t> build_chat_message(
    const std::string& message_id,
    const std::string& sender_id,
    const std::string& room_id,
    uint64_t seq,
    const std::string& sender_name,
    const std::string& text,
    uint64_t timestamp_ms)
{
    std::vector<uint8_t> buf;
    buf.reserve(1 + 2 + message_id.size() + 2 + sender_id.size() +
                2 + room_id.size() + 8 + 2 + sender_name.size() +
                2 + text.size() + 8);
    write_u8(buf, static_cast<uint8_t>(MessageType::CHAT_MESSAGE));
    write_string(buf, message_id);
    write_string(buf, sender_id);
    write_string(buf, room_id);
    write_u64(buf, seq);
    write_string(buf, sender_name);
    write_string(buf, text);
    write_u64(buf, timestamp_ms);
    return buf;
}

inline std::vector<uint8_t> build_chat_message_v2(
    MessageType type,
    const std::string& message_id,
    const std::string& sender_id,
    const std::string& room_id,
    uint64_t seq,
    const std::string& sender_name,
    const std::string& text,
    uint64_t timestamp_ms,
    const std::string& sender_type = "user",
    const std::string& message_kind = "chat",
    const std::string& target_agent_id = {},
    const std::string& source_user_id = {},
    const std::string& correlation_id = {},
    const std::string& metadata_json = {})
{
    std::vector<uint8_t> buf;
    buf.reserve(1 + 2 + message_id.size() + 2 + sender_id.size() +
                2 + room_id.size() + 8 + 2 + sender_name.size() +
                2 + text.size() + 8 + 2 + sender_type.size() +
                2 + message_kind.size() + 2 + target_agent_id.size() +
                2 + source_user_id.size() + 2 + correlation_id.size() +
                2 + metadata_json.size());
    write_u8(buf, static_cast<uint8_t>(type));
    write_string(buf, message_id);
    write_string(buf, sender_id);
    write_string(buf, room_id);
    write_u64(buf, seq);
    write_string(buf, sender_name);
    write_string(buf, text);
    write_u64(buf, timestamp_ms);
    write_string(buf, sender_type);
    write_string(buf, message_kind);
    write_string(buf, target_agent_id);
    write_string(buf, source_user_id);
    write_string(buf, correlation_id);
    write_string(buf, metadata_json);
    return buf;
}

inline std::vector<uint8_t> build_chat_join(
    const std::string& room_id,
    const std::string& member_id,
    const std::string& nickname)
{
    std::vector<uint8_t> buf;
    buf.reserve(1 + 2 + room_id.size() + 2 + member_id.size() + 2 + nickname.size());
    write_u8(buf, static_cast<uint8_t>(MessageType::CHAT_JOIN));
    write_string(buf, room_id);
    write_string(buf, member_id);
    write_string(buf, nickname);
    return buf;
}

inline std::vector<uint8_t> build_chat_leave(
    const std::string& room_id,
    const std::string& member_id)
{
    std::vector<uint8_t> buf;
    buf.reserve(1 + 2 + room_id.size() + 2 + member_id.size());
    write_u8(buf, static_cast<uint8_t>(MessageType::CHAT_LEAVE));
    write_string(buf, room_id);
    write_string(buf, member_id);
    return buf;
}

inline std::vector<uint8_t> build_chat_member_update(
    const std::string& room_id,
    const std::vector<LanChatMember>& members)
{
    size_t payload_size = 1 + 2 + room_id.size() + 2;
    for (const auto& member : members) {
        payload_size += 2 + member.member_id.size();
        payload_size += 2 + member.nickname.size();
        payload_size += 2 + member.status.size();
        payload_size += 8;
    }

    std::vector<uint8_t> buf;
    buf.reserve(payload_size);
    write_u8(buf, static_cast<uint8_t>(MessageType::CHAT_MEMBER_UPDATE));
    write_string(buf, room_id);
    write_u16(buf, static_cast<uint16_t>(members.size()));
    for (const auto& member : members) {
        write_string(buf, member.member_id);
        write_string(buf, member.nickname);
        write_string(buf, member.status);
        write_u64(buf, member.last_seen_ms);
    }
    return buf;
}

inline std::vector<uint8_t> build_chat_history_snapshot(
    const std::string& room_id,
    const std::vector<LanChatMessage>& history)
{
    size_t payload_size = 1 + 2 + room_id.size() + 2;
    for (const auto& message : history) {
        payload_size += 2 + message.message_id.size();
        payload_size += 2 + message.sender_id.size();
        payload_size += 2 + message.room_id.size();
        payload_size += 8;
        payload_size += 2 + message.sender_name.size();
        payload_size += 2 + message.text.size();
        payload_size += 8;
    }

    std::vector<uint8_t> buf;
    buf.reserve(payload_size);
    write_u8(buf, static_cast<uint8_t>(MessageType::CHAT_HISTORY_SNAPSHOT));
    write_string(buf, room_id);
    write_u16(buf, static_cast<uint16_t>(history.size()));
    for (const auto& message : history) {
        write_string(buf, message.message_id);
        write_string(buf, message.sender_id);
        write_string(buf, message.room_id);
        write_u64(buf, message.seq);
        write_string(buf, message.sender_name);
        write_string(buf, message.text);
        write_u64(buf, message.timestamp_ms);
    }
    return buf;
}

inline std::vector<uint8_t> build_chat_history_snapshot_v2(
    const std::string& room_id,
    const std::vector<LanChatMessage>& history)
{
    size_t payload_size = 1 + 2 + room_id.size() + 2;
    for (const auto& message : history) {
        payload_size += 2 + message.message_id.size();
        payload_size += 2 + message.sender_id.size();
        payload_size += 2 + message.room_id.size();
        payload_size += 8;
        payload_size += 2 + message.sender_name.size();
        payload_size += 2 + message.text.size();
        payload_size += 8;
        payload_size += 2 + message.sender_type.size();
        payload_size += 2 + message.message_kind.size();
        payload_size += 2 + message.target_agent_id.size();
        payload_size += 2 + message.source_user_id.size();
        payload_size += 2 + message.correlation_id.size();
        payload_size += 2 + message.metadata_json.size();
    }

    std::vector<uint8_t> buf;
    buf.reserve(payload_size);
    write_u8(buf, static_cast<uint8_t>(MessageType::CHAT_HISTORY_SNAPSHOT_V2));
    write_string(buf, room_id);
    write_u16(buf, static_cast<uint16_t>(history.size()));
    for (const auto& message : history) {
        write_string(buf, message.message_id);
        write_string(buf, message.sender_id);
        write_string(buf, message.room_id);
        write_u64(buf, message.seq);
        write_string(buf, message.sender_name);
        write_string(buf, message.text);
        write_u64(buf, message.timestamp_ms);
        write_string(buf, message.sender_type);
        write_string(buf, message.message_kind);
        write_string(buf, message.target_agent_id);
        write_string(buf, message.source_user_id);
        write_string(buf, message.correlation_id);
        write_string(buf, message.metadata_json);
    }
    return buf;
}

inline std::vector<uint8_t> build_chat_join_reject(
    const std::string& room_id,
    const std::string& code,
    const std::string& reason)
{
    std::vector<uint8_t> buf;
    buf.reserve(1 + 2 + room_id.size() + 2 + code.size() + 2 + reason.size());
    write_u8(buf, static_cast<uint8_t>(MessageType::CHAT_JOIN_REJECT));
    write_string(buf, room_id);
    write_string(buf, code);
    write_string(buf, reason);
    return buf;
}

inline std::vector<uint8_t> build_chat_agent_register(
    const std::string& room_id,
    const std::string& agent_id,
    const std::string& name,
    const std::string& persona,
    const std::string& owner_id)
{
    std::vector<uint8_t> buf;
    buf.reserve(1 + 2 + room_id.size() + 2 + agent_id.size() + 2 + name.size() +
                2 + persona.size() + 2 + owner_id.size());
    write_u8(buf, static_cast<uint8_t>(MessageType::CHAT_AGENT_REGISTER));
    write_string(buf, room_id);
    write_string(buf, agent_id);
    write_string(buf, name);
    write_string(buf, persona);
    write_string(buf, owner_id);
    return buf;
}

inline std::vector<uint8_t> build_chat_agent_remove(
    const std::string& room_id,
    const std::string& agent_id)
{
    std::vector<uint8_t> buf;
    buf.reserve(1 + 2 + room_id.size() + 2 + agent_id.size());
    write_u8(buf, static_cast<uint8_t>(MessageType::CHAT_AGENT_REMOVE));
    write_string(buf, room_id);
    write_string(buf, agent_id);
    return buf;
}

// ============================================================================
// FILE_CHUNK message builder
// ============================================================================
// Wire format:
//   [1B type] [8B transfer_id] [2B model_path_len] [model_path]
//   [4B total_size] [4B offset] [4B chunk_index] [4B chunk_count]
//   [4B chunk_data_len] [chunk_data]
// ============================================================================
inline std::vector<uint8_t> build_file_chunk(
    uint64_t transfer_id,
    const std::string& model_path,
    uint32_t total_size,
    uint32_t offset,
    uint32_t chunk_index,
    uint32_t chunk_count,
    const void* chunk_data, uint32_t chunk_data_len)
{
    std::vector<uint8_t> buf;
    buf.reserve(1 + 8 + 2 + model_path.size() + 4 + 4 + 4 + 4 + 4 + chunk_data_len);

    write_u8(buf, static_cast<uint8_t>(MessageType::FILE_CHUNK));
    write_u64(buf, transfer_id);
    write_string(buf, model_path);
    write_u32(buf, total_size);
    write_u32(buf, offset);
    write_u32(buf, chunk_index);
    write_u32(buf, chunk_count);
    write_u32(buf, chunk_data_len);
    write_bytes(buf, chunk_data, chunk_data_len);

    return buf;
}

}  // namespace Corona::Network
