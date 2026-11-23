#pragma once

#include <corona/systems/network/protocol.h>
#include <corona/systems/network/lww_state.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>

namespace Corona::Network {

/// 身份索引快照的一行；定义在 `sync_codec.h`（这里只作参数类型声明）。
struct SyncIdentityEntry;

/**
 * @brief Poll-sync engine: dirty-poll the owner-thread sync frame → serialize →
 *        broadcast; receive versioned EDITOR_SYNC operations → LWW merge → write
 *        back.
 *
 * Threading contract (Task 2, spec §4.4):
 *  - The network thread NEVER touches the ECS registry. Component reads and the
 *    wire encoding happen on the systems DAG's tick boundary
 *    (`sync_boundary.h::request_sync_frame`); incoming remote state is handed to
 *    the same boundary (`submit_remote_state`) and applied there.
 *  - This class keeps the network-thread-only work: per-entity hash snapshots
 *    (dirty detection), LWW versions and packet framing.
 */
class SyncEngine {
public:
    using OnSyncOutgoing = std::function<void(const std::vector<uint8_t>& packet)>;
    using OnTargetedSyncOutgoing = std::function<void(
        const std::string& peer_id, const std::vector<uint8_t>& packet)>;
    using OnFullSyncRequest = std::function<void(const std::string& requesting_peer_id)>;
    using OnEditorOperationApplied = std::function<void(const EditorSyncOperation&)>;
    using ResolveActorGuidForEntity =
        std::function<std::string(StorageID storage_id, uint64_t entity_seq)>;
    using ResolveEntitySeqForActorGuid =
        std::function<std::optional<uint64_t>(StorageID storage_id,
                                              const std::string& actor_guid)>;
    using ResolveLocalOwnershipForEntity =
        std::function<std::optional<bool>(StorageID storage_id, uint64_t entity_seq)>;
    /// 身份索引快照（一次锁、O(actors)），拥有者线程用 O(1) 表解析每帧的身份。
    using ResolveIdentityIndex = std::function<std::vector<SyncIdentityEntry>()>;

    SyncEngine();
    ~SyncEngine();

    SyncEngine(const SyncEngine&) = delete;
    SyncEngine& operator=(const SyncEngine&) = delete;

    // ========================================================================
    // Lifecycle
    // ========================================================================

    /// Set the local peer id used for LWW versions.
    void initialize(const std::string& local_peer_id);

    /// Clear all tracking state.
    void shutdown();

    // ========================================================================
    // Outbound (local → network)
    // ========================================================================

    /**
     * @brief Ask the tick boundary for this frame's state, serialize dirty
     *        entries, and call on_outgoing for each versioned editor operation.
     *
     * Component reads + wire encoding happen on the owner thread
     * (`request_sync_frame`); this call only does dirty/LWW bookkeeping and
     * sends. Called every kSyncIntervalMs from NetworkSystem::update().
     */
    void poll_and_sync();

    /**
     * @brief Build a SYNC_FULL snapshot of ALL current state and call
     *        on_outgoing with the packet.  Sent to a newly joined peer.
     */
    void sync_full_to(const std::string& target_peer_id);

    /// Emit the current versioned editor state as idempotent operations.
    void emit_snapshot();
    std::vector<uint8_t> make_snapshot_request() const;

    // ========================================================================
    // Inbound (network → local)
    // ========================================================================

    /// Process a received editor operation or snapshot packet.
    void handle_incoming(const std::string& sender_peer_id,
                         const uint8_t* data, size_t len);

    // ========================================================================
    // Callbacks
    // ========================================================================

    void set_on_outgoing(OnSyncOutgoing cb);
    void set_on_targeted_outgoing(OnTargetedSyncOutgoing cb);
    void set_on_full_sync_request(OnFullSyncRequest cb);
    void set_on_editor_operation_applied(OnEditorOperationApplied cb);
    void set_identity_mapping_callbacks(ResolveActorGuidForEntity guid_for_entity,
                                        ResolveEntitySeqForActorGuid entity_for_guid,
                                        ResolveLocalOwnershipForEntity ownership_for_entity = {},
                                        ResolveIdentityIndex identity_index = {});

    /// Apply one versioned editor operation. Exposed for transport adapters
    /// and focused tests; normal network traffic enters via handle_incoming().
    LwwApplyResult apply_editor_operation(const EditorSyncOperation& operation);
    EditorSyncOperation make_local_delete(const std::string& actor_guid);
    EditorSyncOperation make_local_upsert(const std::string& actor_guid,
                                          const std::string& field_name,
                                          std::vector<uint8_t> value);

    /// Retry remote operations whose storage write was temporarily busy.
    void retry_pending_operations();

    /// Expire incomplete snapshot sessions and replay their queued incrementals.
    void maintain_snapshot_sessions(uint64_t now_ms);

private:
    bool apply_operation_to_storage(const EditorSyncOperation& operation);
    struct StorageAccessor;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace Corona::Network
