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

namespace Corona {
    class SharedDataHub;
}

namespace Corona::Network {

/**
 * @brief Poll-sync engine: dirty-poll SharedDataHub → serialize → broadcast;
 *        receive remote SYNC_DIRTY/SYNC_FULL → LWW merge → write back.
 *
 * Architecture assumptions:
 *  - Dirty detection is done by keeping a per-entity hash snapshot and
 *    comparing current data vs that snapshot each tick.
 *  - Editor updates use LwwState versions (Lamport counter + writer peer ID).
 */
class SyncEngine {
public:
    using OnSyncOutgoing = std::function<void(const std::vector<uint8_t>& packet)>;
    using OnFullSyncRequest = std::function<void(const std::string& requesting_peer_id)>;
    using OnEditorOperationApplied = std::function<void(const EditorSyncOperation&)>;
    using ResolveActorGuidForEntity =
        std::function<std::string(StorageID storage_id, uint64_t entity_seq)>;
    using ResolveEntitySeqForActorGuid =
        std::function<std::optional<uint64_t>(StorageID storage_id,
                                              const std::string& actor_guid)>;
    using ResolveLocalOwnershipForEntity =
        std::function<std::optional<bool>(StorageID storage_id, uint64_t entity_seq)>;

    SyncEngine();
    ~SyncEngine();

    SyncEngine(const SyncEngine&) = delete;
    SyncEngine& operator=(const SyncEngine&) = delete;

    // ========================================================================
    // Lifecycle
    // ========================================================================

    /// Attach to the SharedDataHub singleton.
    void initialize(const std::string& local_peer_id);

    /// Clear all tracking state.
    void shutdown();

    // ========================================================================
    // Outbound (local → network)
    // ========================================================================

    /**
     * @brief Poll all tracked storages, serialize dirty entries, and call
     *        on_outgoing for each SYNC_DIRTY packet to broadcast.
     *
     * Called every kSyncIntervalMs from NetworkSystem::update().
     */
    void poll_and_sync();

    /**
     * @brief Build a SYNC_FULL snapshot of ALL current state and call
     *        on_outgoing with the packet.  Sent to a newly joined peer.
     */
    void sync_full_to(const std::string& /*target_peer_id*/);

    // ========================================================================
    // Inbound (network → local)
    // ========================================================================

    /// Process a received SYNC_DIRTY or SYNC_FULL packet.
    void handle_incoming(const std::string& sender_peer_id,
                         const uint8_t* data, size_t len);

    // ========================================================================
    // Callbacks
    // ========================================================================

    void set_on_outgoing(OnSyncOutgoing cb);
    void set_on_full_sync_request(OnFullSyncRequest cb);
    void set_on_editor_operation_applied(OnEditorOperationApplied cb);
    void set_identity_mapping_callbacks(ResolveActorGuidForEntity guid_for_entity,
                                        ResolveEntitySeqForActorGuid entity_for_guid,
                                        ResolveLocalOwnershipForEntity ownership_for_entity = {});

    /// Apply one versioned editor operation. Exposed for transport adapters
    /// and focused tests; normal network traffic enters via handle_incoming().
    LwwApplyResult apply_editor_operation(const EditorSyncOperation& operation);
    EditorSyncOperation make_local_delete(const std::string& actor_guid);

    /// Retry remote operations whose storage write was temporarily busy.
    void retry_pending_operations();

private:
    bool apply_operation_to_storage(const EditorSyncOperation& operation);
    struct StorageAccessor;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace Corona::Network
