#include <corona/systems/network/sync_engine.h>
#include <corona/systems/network/sync_boundary.h>
#include <corona/systems/network/sync_codec.h>

#include <horizon/core/logging.h>

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Corona::Network {

// ============================================================================
// Per-type serialize / deserialize / hash helpers
//
// Task 2：这些函数搬到了 `sync_codec.h`（线上布局 + 哈希 + 编解码，纯值）。
// `sync_engine.cpp` 不再包含 ECS 头、不再 `Ecs::view` / `Ecs::get` / 写组件：
// 出站序列化由拥有者线程在 tick 边界上完成（`sync_boundary.h`），入站写入变成
// 同一条边界上的命令。这里保留网络线程该做的事：dirty 记账、LWW 版本、收发字节。
// ============================================================================

namespace {

uint64_t editor_now_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count());
}

}  // anonymous namespace

// ============================================================================
// Impl
// ============================================================================

struct SyncEngine::Impl {
    std::string local_peer_id;
    LwwState lww_state;
    std::deque<EditorSyncOperation> pending_editor_operations;

    struct IncomingEditorSnapshot {
        uint32_t snapshot_id = 0;
        uint16_t chunk_total = 0;
        uint32_t operation_total = 0;
        uint32_t received_operation_count = 0;
        uint64_t last_activity_ms = 0;
        std::vector<bool> received_chunks;
        std::vector<std::vector<uint8_t>> raw_chunks;
        std::vector<std::vector<EditorSyncOperation>> chunk_operations;
        std::vector<EditorSyncOperation> queued_incremental;
    };
    std::unordered_map<std::string, IncomingEditorSnapshot> incoming_editor_snapshots;

    // Snapshot: key → hash of last-synced value
    std::unordered_map<std::string, std::string> last_synced;
    mutable std::mutex snapshot_mutex;

    /// 刚提交给拥有者线程的远端 environment 值（线上字节）。
    ///
    /// environment 操作是全局 `__scene__` 状态、线上不带实体序号，所以网络线程在
    /// 提交时无法预写 `last_synced` 的 key（key 含本端环境实体的 seq）。下一帧构建
    /// 完成后（同一边界先应用写入、再构建帧），若帧里出现同一份字节，就在那里补记
    /// 对应 key 并跳过发送 —— 否则会把自己刚收到的远端值再广播回去一次。
    std::optional<std::vector<uint8_t>> applied_environment_value;

    // Callbacks
    OnSyncOutgoing on_outgoing;
    OnTargetedSyncOutgoing on_targeted_outgoing;
    OnFullSyncRequest on_full_sync_request;
    OnEditorOperationApplied on_editor_operation_applied;
    ResolveActorGuidForEntity guid_for_entity;
    ResolveEntitySeqForActorGuid entity_for_guid;
    ResolveLocalOwnershipForEntity ownership_for_entity;

    // Sequence counter for outgoing packets
    uint32_t seq = 0;

    // True while we are applying remote data — suppress outgoing re-broadcast
    bool syncing_from_remote = false;

    void recover_incomplete_snapshot(
        const std::string& sender_peer_id,
        IncomingEditorSnapshot snapshot,
        const std::function<LwwApplyResult(const EditorSyncOperation&)>& apply,
        bool request_retry = true) {
        for (const auto& operation : snapshot.queued_incremental) {
            (void)apply(operation);
        }
        if (request_retry && on_targeted_outgoing) {
            auto request = build_editor_snapshot_request();
            on_targeted_outgoing(sender_peer_id, request);
        }
    }

    Impl() : lww_state("uninitialized") {}

    std::string make_key(StorageID sid, uint64_t entity_seq, const std::string& field) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%u-%016llx-%s",
                      static_cast<unsigned>(sid),
                      static_cast<unsigned long long>(entity_seq),
                      field.c_str());
        return {buf};
    }

    struct ResolvedDirtyKey {
        uint64_t entity_seq = 0;
        std::string field;
        bool valid = true;
    };

    ResolvedDirtyKey resolve_dirty_key(StorageID sid, uint64_t remote_entity_seq,
                                       const std::string& key) const {
        constexpr const char* kPrefix = "actor:";
        const std::string prefix{kPrefix};
        if (key.rfind(prefix, 0) == 0) {
            auto field_sep = key.rfind(':');
            if (field_sep != std::string::npos && field_sep > prefix.size()) {
                auto guid = key.substr(prefix.size(), field_sep - prefix.size());
                auto field = key.substr(field_sep + 1);
                if (entity_for_guid) {
                    auto local_seq = entity_for_guid(sid, guid);
                    if (local_seq) {
                        return {*local_seq, field};
                    }
                }
                return {0, field, false};
            }
        }
        return {remote_entity_seq, key};
    }

    bool check_snapshot(const std::string& snap_key, const std::string& cur_hash) {
        std::lock_guard lock(snapshot_mutex);
        auto it = last_synced.find(snap_key);
        if (it != last_synced.end() && it->second == cur_hash) return false;
        last_synced[snap_key] = cur_hash;
        return true;
    }

    void set_snapshot(const std::string& snap_key, const std::string& cur_hash) {
        std::lock_guard lock(snapshot_mutex);
        last_synced[snap_key] = cur_hash;
    }
};

// ============================================================================

SyncEngine::SyncEngine() : impl_(std::make_unique<Impl>()) {}

SyncEngine::~SyncEngine() = default;

void SyncEngine::initialize(const std::string& local_peer_id) {
    impl_->local_peer_id = local_peer_id;
    impl_->lww_state.set_local_peer_id(local_peer_id);
    impl_->last_synced.clear();
    impl_->pending_editor_operations.clear();
    impl_->incoming_editor_snapshots.clear();
    impl_->seq = 0;
}

LwwApplyResult SyncEngine::apply_editor_operation(
    const EditorSyncOperation& operation) {
    const auto result = operation.kind == EditorSyncOperationKind::Delete
        ? impl_->lww_state.apply_delete(operation.actor_guid, operation.version)
        : impl_->lww_state.apply_upsert(operation.actor_guid, operation.field_name,
                                        operation.value, operation.version);
    if (result != LwwApplyResult::Applied) return result;
    if (operation.kind == EditorSyncOperationKind::Delete) {
        if (impl_->lww_state.is_deleted(operation.actor_guid) &&
            impl_->on_editor_operation_applied) {
            impl_->on_editor_operation_applied(operation);
        }
        return result;
    }

    if (!apply_operation_to_storage(operation)) {
        impl_->pending_editor_operations.push_back(operation);
    }
    if (impl_->on_editor_operation_applied) {
        impl_->on_editor_operation_applied(operation);
    }
    return result;
}

EditorSyncOperation SyncEngine::make_local_delete(const std::string& actor_guid) {
    EditorSyncOperation op;
    op.kind = EditorSyncOperationKind::Delete;
    op.actor_guid = actor_guid;
    op.version = impl_->lww_state.next_local_version();
    (void)impl_->lww_state.apply_delete(op.actor_guid, op.version);
    return op;
}

EditorSyncOperation SyncEngine::make_local_upsert(
    const std::string& actor_guid, const std::string& field_name,
    std::vector<uint8_t> value) {
    EditorSyncOperation op;
    op.kind = EditorSyncOperationKind::Upsert;
    op.actor_guid = actor_guid;
    op.field_name = field_name;
    op.value = std::move(value);
    op.version = impl_->lww_state.next_local_version();
    (void)impl_->lww_state.apply_upsert(op.actor_guid, op.field_name,
                                         op.value, op.version);
    return op;
}

bool SyncEngine::apply_operation_to_storage(const EditorSyncOperation& operation) {
    if (operation.kind == EditorSyncOperationKind::Delete) return true;
    if (operation.field_name == "actor.create" ||
        operation.field_name == "actor.state" ||
        operation.field_name == "actor.transform") return true;

    // Task 2：远端状态的落地不再由网络线程写组件。这里只把线上字节解码成值，
    // 交给 tick 边界上的拥有者线程应用（`sync_boundary.h`）。`entity_seq` 直接来自
    // 身份注册表（seq → handle 恒等于 seq + 1，见 `handle_for_seq`），所以网络线程
    // 不需要遍历 registry 建 seq → handle 映射表。
    //
    // 返回值语义保持迁移前不变：true = 这条操作已交出/已应用（不再重试）；
    // false = 暂时没有实体/被 DAG 拒绝，保留在 `pending_editor_operations` 里重试。

    if (operation.field_name == "xform") {
        if (!impl_->entity_for_guid) return false;
        const auto seq = impl_->entity_for_guid(StorageID::ST_MODEL_TRANSFORM,
                                                operation.actor_guid);
        if (!seq) return false;
        TransformState decoded{};
        if (!decode_transform(operation.value.data(),
                              static_cast<uint16_t>(operation.value.size()), decoded)) {
            // 畸形载荷：旧实现是 deserialize 静默 no-op 但仍标 applied，保持一致。
            return true;
        }
        RemoteStateWrite write{};
        write.storage = StorageID::ST_MODEL_TRANSFORM;
        write.entity_seq = *seq;
        write.transform = decoded;
        if (!submit_remote_state(write)) return false;
        // 脏值哈希按"目标值"记账：下一轮 poll 只有真的与目标不一致时才回传，
        // 因此不会把自己刚收到的远程值再广播一次。
        impl_->set_snapshot(impl_->make_key(StorageID::ST_MODEL_TRANSFORM, *seq, "xform"),
                            hash_transform(decoded, *seq));
        return true;
    }

    if (operation.field_name == "optics") {
        if (!impl_->entity_for_guid) return false;
        const auto seq = impl_->entity_for_guid(StorageID::ST_OPTICS, operation.actor_guid);
        if (!seq) return false;
        OpticsPacked decoded{};
        if (!decode_optics(operation.value.data(),
                           static_cast<uint16_t>(operation.value.size()), decoded)) {
            // 畸形载荷（长度不是 sizeof(OpticsPacked)）：与旧实现的静默 no-op 一致。
            return true;
        }
        RemoteStateWrite write{};
        write.storage = StorageID::ST_OPTICS;
        write.entity_seq = *seq;
        write.optics = decoded;
        if (!submit_remote_state(write)) return false;
        impl_->set_snapshot(impl_->make_key(StorageID::ST_OPTICS, *seq, "optics"),
                            hash_optics(decoded, *seq));
        return true;
    }

    if (operation.actor_guid == "__scene__" && operation.field_name == "environment") {
        EnvironmentState decoded{};
        if (!decode_environment(operation.value.data(),
                                static_cast<uint16_t>(operation.value.size()), decoded)) {
            // 畸形载荷（长度不是 20）：与旧实现的静默 no-op 一致。
            return true;
        }
        RemoteStateWrite write{};
        write.storage = StorageID::ST_ENVIRONMENT;
        write.environment = decoded;
        // `__scene__` 是全局状态，线上不带实体序号：拥有者线程取第一个
        // EnvironmentDevice（迁移前同样取"任意一个"）。目标不存在时这条会在边界上
        // 被丢弃并计入 `remote_writes_dropped`，而不是让网络线程去猜一个 seq。
        if (!submit_remote_state(write)) return false;
        // environment 的 `last_synced` key 含本端环境实体的 seq，网络线程不知道它；
        // 记下线上字节，下一帧里出现同一份值时在 `poll_and_sync()` 补记 key 并跳过。
        impl_->applied_environment_value = encode_environment(decoded);
        return true;
    }

    return false;
}

void SyncEngine::retry_pending_operations() {
    for (auto it = impl_->pending_editor_operations.begin();
         it != impl_->pending_editor_operations.end();) {
        if (apply_operation_to_storage(*it)) {
            it = impl_->pending_editor_operations.erase(it);
        } else {
            ++it;
        }
    }
}

void SyncEngine::maintain_snapshot_sessions(uint64_t now_ms) {
    for (auto it = impl_->incoming_editor_snapshots.begin();
         it != impl_->incoming_editor_snapshots.end();) {
        if (now_ms - it->second.last_activity_ms < kEditorSnapshotTimeoutMs) {
            ++it;
            continue;
        }
        const auto sender_peer_id = it->first;
        auto session = std::move(it->second);
        it = impl_->incoming_editor_snapshots.erase(it);
        impl_->recover_incomplete_snapshot(
            sender_peer_id, std::move(session),
            [this](const EditorSyncOperation& operation) {
                return apply_editor_operation(operation);
            });
    }
}

void SyncEngine::shutdown() {
    impl_->last_synced.clear();
    impl_->pending_editor_operations.clear();
    impl_->incoming_editor_snapshots.clear();
}

// ============================================================================
// Outbound
// ============================================================================

void SyncEngine::poll_and_sync() {
    if (impl_->syncing_from_remote) return;

    maintain_snapshot_sessions(editor_now_ms());
    retry_pending_operations();

    // Task 2：组件读取与线上编码在**拥有者线程**的 tick 边界上完成
    //（`request_sync_frame()` 复用结构命令的存活性等待）。网络线程只做 dirty
    // 记账、LWW 版本与发包，不再 `Ecs::view` / `Ecs::get`。
    SyncFrameRequestOptions options;
    options.guid_for_entity = impl_->guid_for_entity;
    options.ownership_for_entity = impl_->ownership_for_entity;
    auto frame = request_sync_frame(std::move(options));
    if (!frame) {
        // DAG 停机 / 已判定不推进：本 tick 不序列化。已入队的远端写入仍在通道里，
        // 下一次边界恢复时照常应用。
        return;
    }

    // --- ModelTransform ---
    for (auto& row : frame->transforms) {
        const auto snap_key = impl_->make_key(StorageID::ST_MODEL_TRANSFORM,
                                              row.entity_seq, "xform");
        if (!impl_->check_snapshot(snap_key, row.hash)) continue;
        EditorSyncOperation op;
        op.kind = EditorSyncOperationKind::Upsert;
        op.actor_guid = row.actor_guid;
        op.field_name = "xform";
        op.value = std::move(row.value);
        op.version = impl_->lww_state.next_local_version();
        (void)impl_->lww_state.apply_upsert(op.actor_guid, op.field_name,
                                             op.value, op.version);
        auto packet = build_editor_sync_operation(op);
        if (!packet.empty() && impl_->on_outgoing) impl_->on_outgoing(packet);
    }

    // Geometry and model resource handles remain local; optics editor state is
    // serialized below without its process-local geometry handle.
    // Actor creation and file transfer build those local objects on each peer;
    // syncing handle-bearing structures afterward can overwrite valid local
    // links with seq/handle values from another process and make meshes vanish.

    // --- OpticsDevice (logical material/editor state; geometry handle excluded) ---
    for (auto& row : frame->optics) {
        const auto snap_key = impl_->make_key(StorageID::ST_OPTICS,
                                              row.entity_seq, "optics");
        if (!impl_->check_snapshot(snap_key, row.hash)) continue;
        EditorSyncOperation op;
        op.kind = EditorSyncOperationKind::Upsert;
        op.actor_guid = row.actor_guid;
        op.field_name = "optics";
        op.value = std::move(row.value);
        op.version = impl_->lww_state.next_local_version();
        (void)impl_->lww_state.apply_upsert(op.actor_guid, op.field_name,
                                             op.value, op.version);
        auto packet = build_editor_sync_operation(op);
        if (!packet.empty() && impl_->on_outgoing) impl_->on_outgoing(packet);
    }

    // --- EnvironmentDevice ---
    for (auto& row : frame->environments) {
        const auto snap_key = impl_->make_key(StorageID::ST_ENVIRONMENT,
                                              row.entity_seq, "env");
        // 刚提交的远端 environment 值：key 里的 seq 网络线程无法预知，在这里补记。
        if (impl_->applied_environment_value) {
            if (*impl_->applied_environment_value == row.value) {
                impl_->set_snapshot(snap_key, row.hash);
                impl_->applied_environment_value.reset();
                continue;
            }
        }
        if (!impl_->check_snapshot(snap_key, row.hash)) continue;
        EditorSyncOperation op;
        op.kind = EditorSyncOperationKind::Upsert;
        op.actor_guid = "__scene__";
        op.field_name = "environment";
        op.value = std::move(row.value);
        op.version = impl_->lww_state.next_local_version();
        (void)impl_->lww_state.apply_upsert(op.actor_guid, op.field_name,
                                             op.value, op.version);
        auto packet = build_editor_sync_operation(op);
        if (!packet.empty() && impl_->on_outgoing) impl_->on_outgoing(packet);
    }
    // 只用来抑制紧邻的一帧：没匹配上（值已被本地改动 / 目标环境实体不存在）
    // 就丢掉，避免无限期压着一条过期的期望。
    impl_->applied_environment_value.reset();
}

void SyncEngine::sync_full_to(const std::string& target_peer_id) {
    // A peer may connect before the first 16 ms sync tick. Seed the LWW table
    // from current local state without broadcasting those seed operations.
    auto outgoing = std::move(impl_->on_outgoing);
    impl_->on_outgoing = [](const std::vector<uint8_t>&) {};
    poll_and_sync();
    impl_->on_outgoing = std::move(outgoing);
    const auto operations = impl_->lww_state.snapshot();
    if (!target_peer_id.empty() && impl_->on_targeted_outgoing) {
        const auto total = static_cast<uint16_t>(std::max<size_t>(
            1, (operations.size() + kEditorSnapshotChunkOperations - 1) /
                   kEditorSnapshotChunkOperations));
        const auto snapshot_id = impl_->seq++;
        auto begin_packet = build_editor_snapshot_begin(
            snapshot_id, total, static_cast<uint32_t>(operations.size()));
        if (begin_packet.empty()) return;
        impl_->on_targeted_outgoing(target_peer_id, begin_packet);
        for (uint16_t index = 0; index < total; ++index) {
            std::vector<std::vector<uint8_t>> encoded;
            const auto begin = static_cast<size_t>(index) * kEditorSnapshotChunkOperations;
            const auto end = std::min(operations.size(), begin + kEditorSnapshotChunkOperations);
            for (size_t i = begin; i < end; ++i) {
                auto packet = build_editor_sync_operation(operations[i]);
                if (!packet.empty()) encoded.push_back(std::move(packet));
            }
            auto chunk = build_editor_snapshot_chunk(snapshot_id, index, total, encoded);
            if (chunk.empty()) return;
            impl_->on_targeted_outgoing(target_peer_id, chunk);
        }
        auto end_packet = build_editor_snapshot_end(snapshot_id);
        if (!end_packet.empty()) impl_->on_targeted_outgoing(target_peer_id, end_packet);
        return;
    }
    emit_snapshot();
}

void SyncEngine::emit_snapshot() {
    if (!impl_->on_outgoing) return;
    for (const auto& operation : impl_->lww_state.snapshot()) {
        auto packet = build_editor_sync_operation(operation);
        if (!packet.empty()) impl_->on_outgoing(packet);
    }
}

std::vector<uint8_t> SyncEngine::make_snapshot_request() const {
    return build_editor_snapshot_request();
}

// ============================================================================
// Inbound
// ============================================================================

void SyncEngine::handle_incoming(const std::string& sender_peer_id,
                                 const uint8_t* data, size_t len) {
    if (len < 2) return;

    if (data[0] == static_cast<uint8_t>(MessageType::EDITOR_SYNC)) {
        auto operation = parse_editor_sync_operation(data, len);
        if (!operation || operation->version.writer_peer_id != sender_peer_id) return;
        auto snapshot = impl_->incoming_editor_snapshots.find(sender_peer_id);
        if (snapshot != impl_->incoming_editor_snapshots.end()) {
            if (snapshot->second.queued_incremental.size() >=
                kMaxEditorSyncOperations) return;
            snapshot->second.queued_incremental.push_back(std::move(*operation));
            snapshot->second.last_activity_ms = editor_now_ms();
            return;
        }
        (void)apply_editor_operation(*operation);
        return;
    }
    if (data[0] == static_cast<uint8_t>(MessageType::EDITOR_SNAPSHOT_REQUEST)) {
        if (len != 2 || data[1] != kEditorSnapshotSchemaVersion ||
            !impl_->on_full_sync_request) return;
        impl_->on_full_sync_request(sender_peer_id);
        return;
    }
    if (data[0] == static_cast<uint8_t>(MessageType::EDITOR_SNAPSHOT_BEGIN)) {
        if (len != 12) return;
        BufferReader r(data, len);
        if (r.read_u8() != static_cast<uint8_t>(MessageType::EDITOR_SNAPSHOT_BEGIN) ||
            r.read_u8() != kEditorSnapshotSchemaVersion) return;
        Impl::IncomingEditorSnapshot snapshot;
        snapshot.snapshot_id = r.read_u32();
        snapshot.chunk_total = r.read_u16();
        snapshot.operation_total = r.read_u32();
        snapshot.last_activity_ms = editor_now_ms();
        if (snapshot.chunk_total == 0 ||
            snapshot.chunk_total > kMaxEditorSnapshotChunks ||
            snapshot.operation_total > kMaxEditorSyncOperations) return;
        auto existing = impl_->incoming_editor_snapshots.find(sender_peer_id);
        if (existing != impl_->incoming_editor_snapshots.end()) {
            if (existing->second.snapshot_id == snapshot.snapshot_id &&
                existing->second.chunk_total == snapshot.chunk_total &&
                existing->second.operation_total == snapshot.operation_total) {
                return;
            }
            auto old_session = std::move(existing->second);
            impl_->incoming_editor_snapshots.erase(existing);
            impl_->recover_incomplete_snapshot(
                sender_peer_id, std::move(old_session),
                [this](const EditorSyncOperation& operation) {
                    return apply_editor_operation(operation);
                }, false);
        }
        snapshot.received_chunks.resize(snapshot.chunk_total, false);
        snapshot.raw_chunks.resize(snapshot.chunk_total);
        snapshot.chunk_operations.resize(snapshot.chunk_total);
        impl_->incoming_editor_snapshots[sender_peer_id] = std::move(snapshot);
        return;
    }
    if (data[0] == static_cast<uint8_t>(MessageType::EDITOR_SNAPSHOT_CHUNK)) {
        if (len > kMaxEditorSnapshotChunkBytes) return;
        BufferReader r(data, len);
        if (!r.has_remaining(1 + 1 + 4 + 2 + 2 + 2) ||
            r.read_u8() != static_cast<uint8_t>(MessageType::EDITOR_SNAPSHOT_CHUNK) ||
            r.read_u8() != kEditorSnapshotSchemaVersion) return;
        const auto snapshot_id = r.read_u32();
        const auto index = r.read_u16();
        const auto total = r.read_u16();
        const auto count = r.read_u16();
        if (total == 0 || index >= total || count > kEditorSnapshotChunkOperations) return;
        auto snapshot = impl_->incoming_editor_snapshots.find(sender_peer_id);
        if (snapshot == impl_->incoming_editor_snapshots.end() ||
            snapshot->second.snapshot_id != snapshot_id ||
            snapshot->second.chunk_total != total) return;
        std::vector<EditorSyncOperation> operations;
        operations.reserve(count);
        for (uint16_t i = 0; i < count; ++i) {
            if (!r.has_remaining(4)) return;
            const auto operation_len = r.read_u32();
            if (operation_len == 0 || operation_len > kMaxEditorSyncValueBytes ||
                !r.has_remaining(operation_len)) return;
            const auto* operation_data = r.data + r.pos;
            auto operation = parse_editor_sync_operation(operation_data, operation_len);
            if (!operation) return;
            // A snapshot forwards the winning version it currently stores.
            // Its writer may therefore be a third peer rather than the peer
            // transporting this snapshot chunk.
            operations.push_back(std::move(*operation));
            r.pos += operation_len;
        }
        if (r.pos != len) return;
        auto& session = snapshot->second;
        session.last_activity_ms = editor_now_ms();
        const std::vector<uint8_t> raw(data, data + len);
        if (session.received_chunks[index]) {
            if (session.raw_chunks[index] != raw) {
                auto failed_session = std::move(session);
                impl_->incoming_editor_snapshots.erase(snapshot);
                impl_->recover_incomplete_snapshot(
                    sender_peer_id, std::move(failed_session),
                    [this](const EditorSyncOperation& operation) {
                        return apply_editor_operation(operation);
                    });
            }
            return;
        }
        if (operations.size() >
            session.operation_total - session.received_operation_count) {
            auto failed_session = std::move(session);
            impl_->incoming_editor_snapshots.erase(snapshot);
            impl_->recover_incomplete_snapshot(
                sender_peer_id, std::move(failed_session),
                [this](const EditorSyncOperation& operation) {
                    return apply_editor_operation(operation);
                });
            return;
        }
        session.received_chunks[index] = true;
        session.received_operation_count += static_cast<uint32_t>(operations.size());
        session.raw_chunks[index] = raw;
        session.chunk_operations[index] = std::move(operations);
        return;
    }
    if (data[0] == static_cast<uint8_t>(MessageType::EDITOR_SNAPSHOT_END)) {
        if (len != 6) return;
        BufferReader r(data, len);
        if (r.read_u8() != static_cast<uint8_t>(MessageType::EDITOR_SNAPSHOT_END) ||
            r.read_u8() != kEditorSnapshotSchemaVersion) return;
        const auto snapshot_id = r.read_u32();
        auto snapshot = impl_->incoming_editor_snapshots.find(sender_peer_id);
        if (snapshot == impl_->incoming_editor_snapshots.end() ||
            snapshot->second.snapshot_id != snapshot_id) return;
        auto session = std::move(snapshot->second);
        impl_->incoming_editor_snapshots.erase(snapshot);
        const bool complete =
            std::find(session.received_chunks.begin(), session.received_chunks.end(),
                      false) == session.received_chunks.end();
        if (complete &&
            session.received_operation_count == session.operation_total) {
            for (const auto& operations : session.chunk_operations) {
                for (const auto& operation : operations) {
                    (void)apply_editor_operation(operation);
                }
            }
            for (const auto& operation : session.queued_incremental) {
                (void)apply_editor_operation(operation);
            }
        } else {
            impl_->recover_incomplete_snapshot(
                sender_peer_id, std::move(session),
                [this](const EditorSyncOperation& operation) {
                    return apply_editor_operation(operation);
                });
        }
        return;
    }

    BufferReader r(data, len);
    auto type = static_cast<MessageType>(r.read_u8());

    if (type == MessageType::SYNC_DIRTY) {
        // Legacy dirty packets carry no LWW version and must never mutate
        // collaborative editor state. New peers use EDITOR_SYNC exclusively.
        return;
    }
    else if (type == MessageType::SYNC_FULL) {
        // Legacy SYNC_FULL has no LWW versions and its old parser treated the
        // first entry byte as a message type. Reject it; new peers use the
        // versioned snapshot request/chunk protocol above.
        return;
    }
    else if (type == MessageType::HEARTBEAT) {
        // No action needed
    }
}

// ============================================================================
void SyncEngine::set_on_outgoing(OnSyncOutgoing cb) {
    impl_->on_outgoing = std::move(cb);
}

void SyncEngine::set_on_targeted_outgoing(OnTargetedSyncOutgoing cb) {
    impl_->on_targeted_outgoing = std::move(cb);
}

void SyncEngine::set_on_full_sync_request(OnFullSyncRequest cb) {
    impl_->on_full_sync_request = std::move(cb);
}

void SyncEngine::set_on_editor_operation_applied(OnEditorOperationApplied cb) {
    impl_->on_editor_operation_applied = std::move(cb);
}

void SyncEngine::set_identity_mapping_callbacks(
    ResolveActorGuidForEntity guid_for_entity,
    ResolveEntitySeqForActorGuid entity_for_guid,
    ResolveLocalOwnershipForEntity ownership_for_entity) {
    impl_->guid_for_entity = std::move(guid_for_entity);
    impl_->entity_for_guid = std::move(entity_for_guid);
    impl_->ownership_for_entity = std::move(ownership_for_entity);
}

}  // namespace Corona::Network
