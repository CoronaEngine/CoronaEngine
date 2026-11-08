#include <corona/systems/network/file_transfer.h>
#include <corona/systems/network/lww_state.h>
#include <corona/systems/network/network_identity.h>
#include <corona/systems/network/network_system.h>
#include <corona/systems/network/protocol.h>
#include <corona/systems/network/sync_engine.h>
#include <corona/shared_data_hub.h>
#include <corona/utils/scene_path_key.h>

#include <cstring>
#include <chrono>
#include <functional>
#include <type_traits>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failed = 0;

void expect_true(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failed;
    }
}


void test_editor_sync_upsert_round_trips_with_lww_version() {
    Corona::Network::EditorSyncOperation op;
    op.kind = Corona::Network::EditorSyncOperationKind::Upsert;
    op.actor_guid = "actor-1";
    op.field_name = "transform.position";
    op.value = {1, 2, 3, 4};
    op.version.counter = 42;
    op.version.writer_peer_id = "peer-b";

    auto packet = Corona::Network::build_editor_sync_operation(op);
    auto decoded = Corona::Network::parse_editor_sync_operation(
        packet.data(), packet.size());
    expect_true(decoded.has_value(), "editor sync upsert parses");
    if (!decoded) return;
    expect_true(decoded->kind == op.kind, "editor sync operation kind round trips");
    expect_true(decoded->actor_guid == op.actor_guid, "editor sync actor guid round trips");
    expect_true(decoded->field_name == op.field_name, "editor sync field name round trips");
    expect_true(decoded->value == op.value, "editor sync value round trips");
    expect_true(decoded->version.counter == op.version.counter &&
                    decoded->version.writer_peer_id == op.version.writer_peer_id,
                "editor sync LWW version round trips");
}

void test_editor_sync_rejects_truncated_and_oversized_packets() {
    Corona::Network::EditorSyncOperation op;
    op.kind = Corona::Network::EditorSyncOperationKind::Delete;
    op.actor_guid = "actor-1";
    op.version.counter = 1;
    op.version.writer_peer_id = "peer-a";
    auto packet = Corona::Network::build_editor_sync_operation(op);

    expect_true(!Corona::Network::parse_editor_sync_operation(
                    packet.data(), packet.size() - 1),
                "editor sync rejects truncated packet");

    op.actor_guid.assign(Corona::Network::kMaxEditorSyncStringBytes + 1, 'x');
    expect_true(Corona::Network::build_editor_sync_operation(op).empty(),
                "editor sync rejects oversized actor guid");
}

void test_editor_sync_has_distinct_message_type() {
    expect_true(Corona::Network::MessageType::EDITOR_SYNC !=
                    Corona::Network::MessageType::SYNC_DIRTY,
                "editor sync message type is distinct from legacy dirty sync");
}

void test_network_channels_partition_control_transform_bulk_and_realtime() {
    expect_true(Corona::Network::channel_for_message_type(
                    Corona::Network::MessageType::EDITOR_SYNC) ==
                    Corona::Network::kChannelReliable,
                "editor sync uses control channel");
    expect_true(Corona::Network::channel_for_message_type(
                    Corona::Network::MessageType::ACTOR_TRANSFORM_UPDATE) ==
                    Corona::Network::kChannelTransform,
                "actor transform uses transform channel");
    expect_true(Corona::Network::channel_for_message_type(
                    Corona::Network::MessageType::EDITOR_SNAPSHOT_CHUNK) ==
                    Corona::Network::kChannelBulk,
                "snapshot chunks use bulk channel");
    expect_true(Corona::Network::channel_for_message_type(
                    Corona::Network::MessageType::FILE_CHUNK) ==
                    Corona::Network::kChannelBulk,
                "file chunks use bulk channel");
    expect_true(Corona::Network::channel_for_message_type(
                    Corona::Network::MessageType::HEARTBEAT) ==
                    Corona::Network::kChannelRealtime,
                "heartbeat uses realtime channel");
    expect_true(Corona::Network::kChannelBulk != Corona::Network::kChannelReliable,
                "bulk channel is isolated from control channel");

    Corona::Network::EditorSyncOperation transform;
    transform.kind = Corona::Network::EditorSyncOperationKind::Upsert;
    transform.actor_guid = "actor-transform";
    transform.field_name = "actor.transform";
    transform.value = {1, 2, 3};
    transform.version = {1, "peer-a"};
    const auto transform_packet =
        Corona::Network::build_editor_sync_operation(transform);
    expect_true(Corona::Network::channel_for_packet(
                    transform_packet.data(), transform_packet.size()) ==
                    Corona::Network::kChannelTransform,
                "versioned actor transform uses transform channel");
}

void test_bulk_poll_budget_limits_packets_and_bytes() {
    Corona::Network::BulkPollBudget budget;
    for (size_t i = 0; i < Corona::Network::kBulkPacketsPerPollBudget; ++i) {
        expect_true(budget.try_consume(1), "bulk budget accepts packet under count limit");
    }
    expect_true(!budget.try_consume(1), "bulk budget rejects packet over count limit");

    Corona::Network::BulkPollBudget bytes;
    expect_true(bytes.try_consume(Corona::Network::kBulkBytesPerPollBudget / 2),
                "bulk budget accepts first byte batch");
    expect_true(!bytes.try_consume(Corona::Network::kBulkBytesPerPollBudget / 2 + 1),
                "bulk budget rejects byte batch over limit");
}

void test_lww_state_merges_by_version_and_field() {
    Corona::Network::LwwState state("peer-a");
    Corona::Network::LwwVersion first{1, "peer-a"};
    Corona::Network::LwwVersion second{2, "peer-b"};
    expect_true(state.apply_upsert("actor", "x", {1}, first) ==
                    Corona::Network::LwwApplyResult::Applied,
                "lww applies first value");
    expect_true(state.apply_upsert("actor", "x", {2}, first) ==
                    Corona::Network::LwwApplyResult::Ignored,
                "lww ignores duplicate version");
    expect_true(state.apply_upsert("actor", "x", {3}, second) ==
                    Corona::Network::LwwApplyResult::Applied,
                "lww applies newer version");
    expect_true(state.apply_upsert("actor", "y", {4}, first) ==
                    Corona::Network::LwwApplyResult::Applied,
                "lww merges independent field");
    expect_true(state.value("actor", "x") == std::vector<uint8_t>{3} &&
                    state.value("actor", "y") == std::vector<uint8_t>{4},
                "lww stores field values independently");
}

void test_lww_state_tombstone_blocks_old_updates() {
    Corona::Network::LwwState state("peer-a");
    expect_true(state.apply_upsert("actor", "x", {1}, {3, "peer-a"}) ==
                    Corona::Network::LwwApplyResult::Applied,
                "lww seed value before delete");
    expect_true(state.apply_delete("actor", {4, "peer-a"}) ==
                    Corona::Network::LwwApplyResult::Applied,
                "lww applies delete tombstone");
    expect_true(state.apply_upsert("actor", "x", {2}, {3, "peer-b"}) ==
                    Corona::Network::LwwApplyResult::Ignored,
                "lww tombstone blocks stale resurrection");
    expect_true(state.is_deleted("actor"), "lww retains delete tombstone");
}

void test_lww_state_advances_lamport_counter() {
    Corona::Network::LwwState state("peer-a");
    expect_true(state.next_local_version().counter == 1,
                "lww local counter starts at one");
    state.observe({9, "peer-b"});
    expect_true(state.next_local_version().counter == 10,
                "lww local counter advances past remote version");
}

void test_sync_engine_applies_versioned_editor_operation() {
    Corona::Network::SyncEngine sync;
    sync.initialize("local-peer");
    Corona::Network::EditorSyncOperation op;
    op.kind = Corona::Network::EditorSyncOperationKind::Upsert;
    op.actor_guid = "actor-sync";
    op.field_name = "name";
    op.value = {'o', 'k'};
    op.version = {7, "remote-peer"};
    expect_true(sync.apply_editor_operation(op) ==
                    Corona::Network::LwwApplyResult::Applied,
                "sync engine applies versioned editor operation");
    expect_true(sync.apply_editor_operation(op) ==
                    Corona::Network::LwwApplyResult::Ignored,
                "sync engine ignores duplicate editor operation");
    sync.shutdown();
}

void test_sync_engine_delete_tombstone_is_idempotent() {
    Corona::Network::SyncEngine sync;
    sync.initialize("local-peer");
    auto op = sync.make_local_delete("actor-delete");
    expect_true(sync.apply_editor_operation(op) ==
                    Corona::Network::LwwApplyResult::Ignored,
                "sync engine ignores duplicate delete tombstone");
    sync.shutdown();
}

void test_sync_engine_does_not_apply_delete_older_than_actor_field() {
    Corona::Network::SyncEngine sync;
    sync.initialize("local-peer");
    int deletes = 0;
    sync.set_on_editor_operation_applied(
        [&](const Corona::Network::EditorSyncOperation& operation) {
            if (operation.kind == Corona::Network::EditorSyncOperationKind::Delete) {
                ++deletes;
            }
        });
    Corona::Network::EditorSyncOperation field;
    field.kind = Corona::Network::EditorSyncOperationKind::Upsert;
    field.actor_guid = "actor-live";
    field.field_name = "actor.create";
    field.value = {1};
    field.version = {5, "peer-a"};
    expect_true(sync.apply_editor_operation(field) ==
                    Corona::Network::LwwApplyResult::Applied,
                "new actor field applies before stale delete");
    Corona::Network::EditorSyncOperation deletion;
    deletion.kind = Corona::Network::EditorSyncOperationKind::Delete;
    deletion.actor_guid = field.actor_guid;
    deletion.version = {4, "peer-b"};
    expect_true(sync.apply_editor_operation(deletion) ==
                    Corona::Network::LwwApplyResult::Applied,
                "older tombstone remains as a stale update barrier");
    expect_true(deletes == 0,
                "delete older than current actor field does not delete actor");
    sync.shutdown();
}

void test_sync_engine_emits_versioned_snapshot() {
    Corona::Network::SyncEngine sync;
    std::vector<std::vector<uint8_t>> packets;
    sync.initialize("local-peer");
    Corona::Network::EditorSyncOperation op;
    op.kind = Corona::Network::EditorSyncOperationKind::Upsert;
    op.actor_guid = "actor-snapshot";
    op.field_name = "name";
    op.value = {'s'};
    op.version = {4, "local-peer"};
    expect_true(sync.apply_editor_operation(op) ==
                    Corona::Network::LwwApplyResult::Applied,
                "snapshot test seeds LWW state");
    sync.set_on_outgoing([&](const std::vector<uint8_t>& packet) {
        packets.push_back(packet);
    });
    sync.sync_full_to("peer-b");
    bool found = false;
    for (const auto& packet : packets) {
        auto decoded = Corona::Network::parse_editor_sync_operation(packet.data(), packet.size());
        if (decoded && decoded->actor_guid == "actor-snapshot") found = true;
    }
    expect_true(found, "snapshot emits seeded versioned operation");
    sync.shutdown();
}

void test_sync_engine_creates_versioned_actor_upsert() {
    Corona::Network::SyncEngine sync;
    sync.initialize("local-peer");
    auto op = sync.make_local_upsert("actor-create", "actor.create", {1, 2});
    expect_true(op.version.counter == 1 && op.version.writer_peer_id == "local-peer",
                "actor create uses local LWW version");
    expect_true(sync.apply_editor_operation(op) ==
                    Corona::Network::LwwApplyResult::Ignored,
                "actor create duplicate is ignored");
    sync.shutdown();
}

void test_sync_engine_does_not_retry_logical_actor_state() {
    Corona::Network::SyncEngine sync;
    sync.initialize("local-peer");
    int applied = 0;
    sync.set_on_editor_operation_applied(
        [&](const Corona::Network::EditorSyncOperation&) { ++applied; });

    Corona::Network::EditorSyncOperation op;
    op.kind = Corona::Network::EditorSyncOperationKind::Upsert;
    op.actor_guid = "actor-state";
    op.field_name = "actor.state";
    op.value = {1, 2, 3};
    op.version = {2, "remote-peer"};
    expect_true(sync.apply_editor_operation(op) ==
                    Corona::Network::LwwApplyResult::Applied,
                "logical actor state applies through LWW");
    sync.retry_pending_operations();
    expect_true(applied == 1,
                "logical actor state is not retained as a storage retry");
    sync.shutdown();
}

void test_peer_manager_targeted_send_rejects_unknown_peer() {
    Corona::Network::PeerManager manager;
    const uint8_t byte = 1;
    expect_true(!manager.send_to_peer_id("missing", Corona::Network::kChannelReliable,
                                        &byte, sizeof(byte), true),
                "targeted peer send rejects unknown peer");
}

void test_peer_manager_callbacks_can_stop_host() {
    using Corona::Network::PeerManager;
    for (int callback = 0; callback < 3; ++callback) {
        PeerManager server;
        PeerManager client;
        bool stopped = false;
        const auto stop = [&] { stopped = true; server.stop(); };
        if (callback == 0) server.set_on_peer_connected([&](const auto&) { stop(); });
        if (callback == 1) server.set_on_data_received([&](const auto&, const auto*, auto) { stop(); });
        if (callback == 2) server.set_on_peer_disconnected([&](const auto&) { stop(); });
        const bool started = server.start(0, "Callback Server") && client.start(0, "Callback Client");
        expect_true(started, "callback stop test starts owned localhost peers");
        if (!started) continue;
        client.connect_to_peer("127.0.0.1", server.listen_port(), "Callback Server", true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool triggered = false;
        while (!stopped && std::chrono::steady_clock::now() < deadline) {
            server.poll();
            client.poll();
            if (!triggered && callback != 0 && server.peer_count() && client.peer_count()) {
                const auto peers = client.peers();
                if (!peers.empty() && peers.front().hello_done) {
                    if (callback == 1) {
                        const uint8_t data = 0xff;
                        client.broadcast(Corona::Network::kChannelReliable, &data, sizeof(data), true);
                    } else {
                        client.disconnect_peer(peers.front().id);
                    }
                    triggered = true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect_true(stopped, "connected/data/disconnected callback stops its owned host without invalidating poll");
        server.poll();
        server.stop();
    }
}

void test_editor_snapshot_request_has_schema_version() {
    auto packet = Corona::Network::build_editor_snapshot_request();
    expect_true(packet.size() == 2 &&
                    packet[0] == static_cast<uint8_t>(Corona::Network::MessageType::EDITOR_SNAPSHOT_REQUEST) &&
                    packet[1] == Corona::Network::kEditorSnapshotSchemaVersion,
                "snapshot request carries schema version");
}

void test_editor_snapshot_begin_and_end_carry_session_metadata() {
    const auto begin = Corona::Network::build_editor_snapshot_begin(17, 3, 513);
    Corona::Network::BufferReader begin_reader(begin.data(), begin.size());
    expect_true(static_cast<Corona::Network::MessageType>(begin_reader.read_u8()) ==
                    Corona::Network::MessageType::EDITOR_SNAPSHOT_BEGIN,
                "snapshot begin message type");
    expect_true(begin_reader.read_u8() == Corona::Network::kEditorSnapshotSchemaVersion &&
                    begin_reader.read_u32() == 17 &&
                    begin_reader.read_u16() == 3 &&
                    begin_reader.read_u32() == 513,
                "snapshot begin carries id chunk and operation totals");

    const auto end = Corona::Network::build_editor_snapshot_end(17);
    Corona::Network::BufferReader end_reader(end.data(), end.size());
    expect_true(static_cast<Corona::Network::MessageType>(end_reader.read_u8()) ==
                    Corona::Network::MessageType::EDITOR_SNAPSHOT_END &&
                    end_reader.read_u8() == Corona::Network::kEditorSnapshotSchemaVersion &&
                    end_reader.read_u32() == 17,
                "snapshot end carries schema and snapshot id");
}

void test_editor_snapshot_chunk_round_trips_operation_packets() {
    Corona::Network::EditorSyncOperation op;
    op.kind = Corona::Network::EditorSyncOperationKind::Upsert;
    op.actor_guid = "actor-chunk";
    op.field_name = "name";
    op.value = {'x'};
    op.version = {3, "peer-a"};
    auto encoded = Corona::Network::build_editor_sync_operation(op);
    auto chunk = Corona::Network::build_editor_snapshot_chunk(9, 0, 1, {encoded});
    expect_true(chunk.size() > encoded.size(), "snapshot chunk wraps operation packet");
    expect_true(chunk[0] == static_cast<uint8_t>(Corona::Network::MessageType::EDITOR_SNAPSHOT_CHUNK),
                "snapshot chunk message type");
}

void test_editor_snapshot_accepts_operation_from_original_writer() {
    Corona::Network::SyncEngine sync;
    sync.initialize("receiver");
    Corona::Network::EditorSyncOperation op;
    op.kind = Corona::Network::EditorSyncOperationKind::Upsert;
    op.actor_guid = "actor-forwarded";
    op.field_name = "name";
    op.value = {'x'};
    op.version = {5, "original-writer"};
    const auto encoded = Corona::Network::build_editor_sync_operation(op);
    const auto chunk = Corona::Network::build_editor_snapshot_chunk(
        11, 0, 1, {encoded});

    int applied = 0;
    sync.set_on_editor_operation_applied(
        [&](const Corona::Network::EditorSyncOperation& operation) {
            if (operation.actor_guid == op.actor_guid &&
                operation.version.writer_peer_id == op.version.writer_peer_id) {
                ++applied;
            }
        });
    const auto begin = Corona::Network::build_editor_snapshot_begin(11, 1, 1);
    const auto end = Corona::Network::build_editor_snapshot_end(11);
    sync.handle_incoming("snapshot-sender", begin.data(), begin.size());
    sync.handle_incoming("snapshot-sender", chunk.data(), chunk.size());
    expect_true(applied == 0, "snapshot does not apply before end marker");
    sync.handle_incoming("snapshot-sender", end.data(), end.size());

    expect_true(applied == 1,
                "snapshot preserves and accepts the original field writer");
    sync.shutdown();
}

void test_editor_snapshot_missing_chunk_does_not_mutate_state() {
    Corona::Network::SyncEngine sync;
    sync.initialize("receiver");
    int applied = 0;
    sync.set_on_editor_operation_applied(
        [&](const Corona::Network::EditorSyncOperation&) { ++applied; });
    const auto begin = Corona::Network::build_editor_snapshot_begin(12, 2, 1);
    const auto end = Corona::Network::build_editor_snapshot_end(12);
    sync.handle_incoming("snapshot-sender", begin.data(), begin.size());
    sync.handle_incoming("snapshot-sender", end.data(), end.size());
    expect_true(applied == 0, "incomplete snapshot does not mutate state");
    sync.shutdown();
}

void test_editor_snapshot_queues_sender_incremental_until_complete() {
    Corona::Network::SyncEngine sync;
    sync.initialize("receiver");
    std::vector<std::string> applied_fields;
    sync.set_on_editor_operation_applied(
        [&](const Corona::Network::EditorSyncOperation& operation) {
            applied_fields.push_back(operation.field_name);
        });
    Corona::Network::EditorSyncOperation snapshot_op;
    snapshot_op.kind = Corona::Network::EditorSyncOperationKind::Upsert;
    snapshot_op.actor_guid = "actor-queued";
    snapshot_op.field_name = "actor.state";
    snapshot_op.value = {1};
    snapshot_op.version = {4, "original-writer"};
    Corona::Network::EditorSyncOperation incremental = snapshot_op;
    incremental.field_name = "actor.transform";
    incremental.value = {2};
    incremental.version = {5, "snapshot-sender"};
    const auto encoded_snapshot =
        Corona::Network::build_editor_sync_operation(snapshot_op);
    const auto chunk = Corona::Network::build_editor_snapshot_chunk(
        13, 0, 1, {encoded_snapshot});
    const auto begin = Corona::Network::build_editor_snapshot_begin(13, 1, 1);
    const auto end = Corona::Network::build_editor_snapshot_end(13);
    const auto encoded_incremental =
        Corona::Network::build_editor_sync_operation(incremental);

    sync.handle_incoming("snapshot-sender", begin.data(), begin.size());
    sync.handle_incoming("snapshot-sender", encoded_incremental.data(),
                         encoded_incremental.size());
    sync.handle_incoming("snapshot-sender", chunk.data(), chunk.size());
    expect_true(applied_fields.empty(),
                "sender incremental remains queued during snapshot");
    sync.handle_incoming("snapshot-sender", end.data(), end.size());
    expect_true(applied_fields ==
                    std::vector<std::string>{"actor.state", "actor.transform"},
                "snapshot applies before queued sender incremental replay");
    sync.shutdown();
}

void test_editor_snapshot_timeout_replays_incremental_and_requests_retry() {
    Corona::Network::SyncEngine sync;
    sync.initialize("receiver");
    int applied = 0;
    int requests = 0;
    sync.set_on_editor_operation_applied(
        [&](const Corona::Network::EditorSyncOperation&) { ++applied; });
    sync.set_on_targeted_outgoing(
        [&](const std::string& peer_id, const std::vector<uint8_t>& packet) {
            if (peer_id == "snapshot-sender" && packet.size() == 2 &&
                packet[0] == static_cast<uint8_t>(
                    Corona::Network::MessageType::EDITOR_SNAPSHOT_REQUEST)) {
                ++requests;
            }
        });
    const auto begin = Corona::Network::build_editor_snapshot_begin(14, 1, 1);
    Corona::Network::EditorSyncOperation incremental;
    incremental.kind = Corona::Network::EditorSyncOperationKind::Upsert;
    incremental.actor_guid = "actor-timeout";
    incremental.field_name = "actor.state";
    incremental.value = {3};
    incremental.version = {6, "snapshot-sender"};
    const auto encoded = Corona::Network::build_editor_sync_operation(incremental);
    sync.handle_incoming("snapshot-sender", begin.data(), begin.size());
    sync.handle_incoming("snapshot-sender", encoded.data(), encoded.size());

    sync.maintain_snapshot_sessions(UINT64_MAX);

    expect_true(applied == 1,
                "snapshot timeout replays queued sender incremental");
    expect_true(requests == 1,
                "snapshot timeout requests a replacement snapshot");
    sync.shutdown();
}

void test_editor_snapshot_conflicting_duplicate_recovers_incremental() {
    Corona::Network::SyncEngine sync;
    sync.initialize("receiver");
    int applied = 0;
    int requests = 0;
    sync.set_on_editor_operation_applied(
        [&](const Corona::Network::EditorSyncOperation&) { ++applied; });
    sync.set_on_targeted_outgoing(
        [&](const std::string&, const std::vector<uint8_t>& packet) {
            if (packet.size() == 2 &&
                packet[0] == static_cast<uint8_t>(
                    Corona::Network::MessageType::EDITOR_SNAPSHOT_REQUEST)) {
                ++requests;
            }
        });
    Corona::Network::EditorSyncOperation first;
    first.kind = Corona::Network::EditorSyncOperationKind::Upsert;
    first.actor_guid = "actor-conflict";
    first.field_name = "actor.state";
    first.value = {1};
    first.version = {2, "original-writer"};
    auto second = first;
    second.value = {2};
    second.version = {3, "other-writer"};
    Corona::Network::EditorSyncOperation incremental = first;
    incremental.field_name = "actor.transform";
    incremental.version = {4, "snapshot-sender"};
    const auto begin = Corona::Network::build_editor_snapshot_begin(15, 1, 1);
    const auto first_chunk = Corona::Network::build_editor_snapshot_chunk(
        15, 0, 1, {Corona::Network::build_editor_sync_operation(first)});
    const auto conflicting_chunk = Corona::Network::build_editor_snapshot_chunk(
        15, 0, 1, {Corona::Network::build_editor_sync_operation(second)});
    const auto encoded_incremental =
        Corona::Network::build_editor_sync_operation(incremental);

    sync.handle_incoming("snapshot-sender", begin.data(), begin.size());
    sync.handle_incoming("snapshot-sender", encoded_incremental.data(),
                         encoded_incremental.size());
    sync.handle_incoming("snapshot-sender", first_chunk.data(), first_chunk.size());
    sync.handle_incoming("snapshot-sender", conflicting_chunk.data(),
                         conflicting_chunk.size());

    expect_true(applied == 1,
                "conflicting duplicate chunk replays queued incremental");
    expect_true(requests == 1,
                "conflicting duplicate chunk requests replacement snapshot");
    sync.shutdown();
}

void test_sync_full_to_sends_begin_chunk_end() {
    Corona::Network::SyncEngine sync;
    sync.initialize("sender");
    (void)sync.make_local_upsert("actor-session", "actor.state", {1});
    std::vector<Corona::Network::MessageType> types;
    sync.set_on_targeted_outgoing(
        [&](const std::string& peer_id, const std::vector<uint8_t>& packet) {
            if (peer_id == "receiver" && !packet.empty()) {
                types.push_back(static_cast<Corona::Network::MessageType>(packet[0]));
            }
        });

    sync.sync_full_to("receiver");

    expect_true(types.size() == 3 &&
                    types[0] == Corona::Network::MessageType::EDITOR_SNAPSHOT_BEGIN &&
                    types[1] == Corona::Network::MessageType::EDITOR_SNAPSHOT_CHUNK &&
                    types[2] == Corona::Network::MessageType::EDITOR_SNAPSHOT_END,
                "targeted snapshot sends begin chunk and end in order");
    sync.shutdown();
}

void test_first_peer_receives_snapshot_over_real_channels() {
    using namespace Corona::Network;
    PeerManager sender;
    PeerManager receiver;
    SyncEngine source;
    SyncEngine destination;
    source.initialize("snapshot-writer");
    destination.initialize("snapshot-reader");
    (void)source.make_local_upsert("before-connect", "actor.state", {1});
    int applied = 0;
    destination.set_on_editor_operation_applied([&](const EditorSyncOperation& operation) {
        if (operation.actor_guid == "before-connect") ++applied;
    });
    source.set_on_targeted_outgoing([&](const auto& peer, const auto& packet) {
        sender.send_to_peer_id(peer, kChannelReliable, packet.data(), packet.size(), true);
    });
    sender.set_on_peer_connected([&](const auto& peer) { source.sync_full_to(peer.id); });
    receiver.set_on_data_received([&](const auto& peer, const auto* data, auto size) {
        destination.handle_incoming(peer, data, size);
    });
    const bool started = sender.start(0, "Snapshot Sender") && receiver.start(0, "Snapshot Receiver");
    expect_true(started, "snapshot test starts owned localhost peers");
    if (started) {
        receiver.connect_to_peer("127.0.0.1", sender.listen_port(), "Snapshot Sender", true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!applied && std::chrono::steady_clock::now() < deadline) {
            sender.poll();
            receiver.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect_true(applied == 1, "first peer applies preexisting state with real control/bulk routing");
    }
    sender.stop(); receiver.stop();
    source.shutdown(); destination.shutdown();
}

void test_file_request_carries_transfer_id() {
    constexpr uint64_t transfer_id = 0x1122334455667788ull;
    auto packet = Corona::Network::build_file_request(transfer_id, "Resource/mesh.obj");

    Corona::Network::BufferReader reader(packet.data(), packet.size());
    expect_true(static_cast<Corona::Network::MessageType>(reader.read_u8()) ==
                    Corona::Network::MessageType::FILE_REQUEST,
                "file request message type");
    expect_true(reader.read_u64() == transfer_id, "file request transfer id");
    const auto path_len = reader.read_u16();
    expect_true(reader.read_string(path_len) == "Resource/mesh.obj",
                "file request path payload");
}

void test_actor_create_carries_actor_guid() {
    float transform[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    Corona::Network::ActorCreatePacked optics{};
    const std::string actor_guid = "actor-1234";

    auto packet = Corona::Network::build_actor_create(
        actor_guid, "Scene/main.scene", "Resource/mesh.obj",
        transform, &optics, sizeof(optics));

    Corona::Network::BufferReader reader(packet.data(), packet.size());
    expect_true(static_cast<Corona::Network::MessageType>(reader.read_u8()) ==
                    Corona::Network::MessageType::ACTOR_CREATE,
                "actor create message type");

    const auto guid_len = reader.read_u16();
    expect_true(reader.read_string(guid_len) == actor_guid,
                "actor create actor guid payload");

    const auto scene_len = reader.read_u16();
    expect_true(reader.read_string(scene_len) == "Scene/main.scene",
                "actor create scene payload");

    const auto path_len = reader.read_u16();
    expect_true(reader.read_string(path_len) == "Resource/mesh.obj",
                "actor create model path payload");
}

void test_actor_create_unpack_preserves_wire_transform() {
    float transform[9] = {10, 20, 30, 1, 2, 3, 4, 5, 6};
    Corona::Network::ActorCreatePacked optics{};
    optics.visible = true;
    optics.bEnableLighting = true;

    auto packet = Corona::Network::build_actor_create(
        "actor-xform", "Scene/main.scene", "Resource/mesh.obj",
        transform, &optics, sizeof(optics));

    Corona::Network::BufferReader reader(packet.data(), packet.size());
    (void)reader.read_u8();
    reader.read_string(reader.read_u16());  // actor_guid
    reader.read_string(reader.read_u16());  // scene_name
    reader.read_string(reader.read_u16());  // model_path

    const float* wire_transform = reinterpret_cast<const float*>(reader.data + reader.pos);
    reader.pos += 36;

    Corona::Network::ActorCreatePacked unpacked{};
    std::memcpy(&unpacked, reader.data + reader.pos, sizeof(unpacked));
    std::memcpy(unpacked.transform, wire_transform, 36);

    for (int i = 0; i < 9; ++i) {
        expect_true(unpacked.transform[i] == transform[i],
                    "actor create unpack preserves transform");
    }
    expect_true(unpacked.visible, "actor create unpack preserves optics visible");
    expect_true(unpacked.bEnableLighting,
                "actor create unpack preserves optics lighting flag");
}

void test_actor_create_carries_dependency_paths() {
    float transform[9] = {};
    Corona::Network::ActorCreatePacked optics{};
    const std::vector<std::string> deps{
        "Resource/mesh.mtl",
        "Resource/texture.png",
    };

    auto packet = Corona::Network::build_actor_create(
        "actor-deps", "Scene/main.scene", "Resource/mesh.obj",
        transform, &optics, sizeof(optics), deps);

    Corona::Network::BufferReader reader(packet.data(), packet.size());
    expect_true(static_cast<Corona::Network::MessageType>(reader.read_u8()) ==
                    Corona::Network::MessageType::ACTOR_CREATE,
                "actor create dependency message type");
    reader.read_string(reader.read_u16());  // actor_guid
    reader.read_string(reader.read_u16());  // scene_name
    reader.read_string(reader.read_u16());  // model_path
    reader.pos += 36 + sizeof(optics);

    expect_true(reader.read_u16() == deps.size(), "actor create dependency count");
    expect_true(reader.read_string(reader.read_u16()) == deps[0],
                "actor create first dependency path");
    expect_true(reader.read_string(reader.read_u16()) == deps[1],
                "actor create second dependency path");
}

void test_actor_create_carries_actor_json_payload() {
    float transform[9] = {};
    Corona::Network::ActorCreatePacked optics{};
    const std::vector<std::string> deps{
        "Resource/mesh.mtl",
    };
    const std::string actor_json =
        "{\"name\":\"Display Chair\",\"visible\":false,\"scene_framework\":{\"kind\":\"room_box\"}}";

    auto packet = Corona::Network::build_actor_create(
        "actor-json", "Scene/main.scene", "Resource/mesh.obj",
        transform, &optics, sizeof(optics), deps, actor_json);

    Corona::Network::BufferReader reader(packet.data(), packet.size());
    expect_true(static_cast<Corona::Network::MessageType>(reader.read_u8()) ==
                    Corona::Network::MessageType::ACTOR_CREATE,
                "actor create json message type");
    reader.read_string(reader.read_u16());  // actor_guid
    reader.read_string(reader.read_u16());  // scene_name
    reader.read_string(reader.read_u16());  // model_path
    reader.pos += 36 + sizeof(optics);
    expect_true(reader.read_u16() == deps.size(), "actor create json dependency count");
    reader.read_string(reader.read_u16());  // dependency
    expect_true(reader.read_string(reader.read_u32()) == actor_json,
                "actor create full actor json payload");
}

void test_actor_transform_update_carries_transform_and_correlation() {
    float transform[9] = {1, 2, 3, 0.1f, 0.2f, 0.3f, 2, 2, 2};
    auto packet = Corona::Network::build_actor_transform_update(
        "actor-xform", "Scene/main.scene", transform, "user-a", "gm-1");

    Corona::Network::BufferReader reader(packet.data(), packet.size());
    expect_true(static_cast<Corona::Network::MessageType>(reader.read_u8()) ==
                    Corona::Network::MessageType::ACTOR_TRANSFORM_UPDATE,
                "actor transform update message type");
    expect_true(reader.read_string(reader.read_u16()) == "actor-xform",
                "actor transform actor guid payload");
    expect_true(reader.read_string(reader.read_u16()) == "Scene/main.scene",
                "actor transform scene payload");
    const float* wire_transform = reinterpret_cast<const float*>(reader.data + reader.pos);
    for (int i = 0; i < 9; ++i) {
        expect_true(wire_transform[i] == transform[i],
                    "actor transform preserves transform values");
    }
    reader.pos += 36;
    expect_true(reader.read_string(reader.read_u16()) == "user-a",
                "actor transform source user payload");
    expect_true(reader.read_string(reader.read_u16()) == "gm-1",
                "actor transform correlation payload");
}

void test_actor_delete_carries_scene_guid_and_name() {
    auto packet = Corona::Network::build_actor_delete(
        "actor-delete", "Scene/main.scene", "chair");

    Corona::Network::BufferReader reader(packet.data(), packet.size());
    expect_true(static_cast<Corona::Network::MessageType>(reader.read_u8()) ==
                    Corona::Network::MessageType::ACTOR_DELETE,
                "actor delete message type");
    expect_true(reader.read_string(reader.read_u16()) == "actor-delete",
                "actor delete actor guid payload");
    expect_true(reader.read_string(reader.read_u16()) == "Scene/main.scene",
                "actor delete scene payload");
    expect_true(reader.read_string(reader.read_u16()) == "chair",
                "actor delete actor name payload");
}

void test_actor_scene_snapshot_request_carries_scene() {
    auto packet = Corona::Network::build_actor_scene_snapshot_request("Scene/main.scene");

    Corona::Network::BufferReader reader(packet.data(), packet.size());
    expect_true(static_cast<Corona::Network::MessageType>(reader.read_u8()) ==
                    Corona::Network::MessageType::ACTOR_SCENE_SNAPSHOT_REQUEST,
                "actor scene snapshot request message type");
    expect_true(reader.read_string(reader.read_u16()) == "Scene/main.scene",
                "actor scene snapshot request scene payload");
}

void test_actor_scene_snapshot_carries_json_payload() {
    const std::string json = "{\"actors\":[{\"actor_guid\":\"actor-chair\"}]}";
    auto packet = Corona::Network::build_actor_scene_snapshot("Scene/main.scene", json);

    Corona::Network::BufferReader reader(packet.data(), packet.size());
    expect_true(static_cast<Corona::Network::MessageType>(reader.read_u8()) ==
                    Corona::Network::MessageType::ACTOR_SCENE_SNAPSHOT,
                "actor scene snapshot message type");
    expect_true(reader.read_string(reader.read_u16()) == "Scene/main.scene",
                "actor scene snapshot scene payload");
    expect_true(reader.read_string(reader.read_u32()) == json,
                "actor scene snapshot json payload");
}

void test_actor_state_update_carries_guid_scene_and_json() {
    const std::string json = "{\"name\":\"Display Chair\"}";
    auto packet = Corona::Network::build_actor_state_update(
        "actor-chair", "Scene/main.scene", json);

    Corona::Network::BufferReader reader(packet.data(), packet.size());
    expect_true(static_cast<Corona::Network::MessageType>(reader.read_u8()) ==
                    Corona::Network::MessageType::ACTOR_STATE_UPDATE,
                "actor state update message type");
    expect_true(reader.read_string(reader.read_u16()) == "actor-chair",
                "actor state update actor guid payload");
    expect_true(reader.read_string(reader.read_u16()) == "Scene/main.scene",
                "actor state update scene payload");
    expect_true(reader.read_string(reader.read_u32()) == json,
                "actor state update json payload");
}

void test_file_chunk_carries_transfer_id_and_offset() {
    constexpr uint64_t transfer_id = 0x8877665544332211ull;
    constexpr uint32_t total_size = 4096;
    constexpr uint32_t offset = 1024;
    constexpr uint32_t chunk_index = 2;
    constexpr uint32_t chunk_count = 4;
    const std::vector<uint8_t> bytes{1, 2, 3, 4};

    auto packet = Corona::Network::build_file_chunk(
        transfer_id, "Resource/mesh.obj", total_size, offset, chunk_index,
        chunk_count, bytes.data(), static_cast<uint32_t>(bytes.size()));

    Corona::Network::BufferReader reader(packet.data(), packet.size());
    expect_true(static_cast<Corona::Network::MessageType>(reader.read_u8()) ==
                    Corona::Network::MessageType::FILE_CHUNK,
                "file chunk message type");
    expect_true(reader.read_u64() == transfer_id, "file chunk transfer id");
    const auto path_len = reader.read_u16();
    expect_true(reader.read_string(path_len) == "Resource/mesh.obj",
                "file chunk path payload");
    expect_true(reader.read_u32() == total_size, "file chunk total size");
    expect_true(reader.read_u32() == offset, "file chunk offset");
    expect_true(reader.read_u32() == chunk_index, "file chunk index");
    expect_true(reader.read_u32() == chunk_count, "file chunk count");
    expect_true(reader.read_u32() == bytes.size(), "file chunk data length");
}

void test_ownership_claim_carries_actor_guid() {
    auto packet = Corona::Network::build_ownership_claim("actor-owner");

    Corona::Network::BufferReader reader(packet.data(), packet.size());
    expect_true(static_cast<Corona::Network::MessageType>(reader.read_u8()) ==
                    Corona::Network::MessageType::OWNERSHIP_CLAIM,
                "ownership claim message type");
    expect_true(reader.read_string(reader.read_u16()) == "actor-owner",
                "ownership claim actor guid payload");
}


void test_project_relative_path_validation() {
    const std::filesystem::path root = "D:/project/root";

    auto valid = Corona::Network::resolve_project_relative_path(root, "Resource/mesh.obj");
    expect_true(valid.has_value(), "valid project relative path accepted");
    expect_true(valid->filename() == "mesh.obj", "valid path resolved");

    expect_true(!Corona::Network::resolve_project_relative_path(root, "../escape.obj").has_value(),
                "parent traversal rejected");
    expect_true(!Corona::Network::resolve_project_relative_path(root, "D:/tmp/escape.obj").has_value(),
                "absolute path rejected");
    expect_true(!Corona::Network::resolve_project_relative_path(root, "").has_value(),
                "empty path rejected");
}

void test_project_relative_path_uses_utf8_for_non_ascii_segments() {
    const auto root = std::filesystem::temp_directory_path() /
        "corona_network_utf8_path_test";
    const std::string relative =
        reinterpret_cast<const char*>(u8"Resource/models/绿植/base.obj");
    const auto expected = root /
        std::filesystem::u8path(relative);
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(expected.parent_path());
    {
        std::ofstream file(expected, std::ios::binary);
        file << "mesh";
    }

    auto resolved = Corona::Network::resolve_project_relative_path(
        root, relative);
    expect_true(resolved.has_value(), "utf8 project relative path accepted");
    expect_true(resolved && std::filesystem::exists(*resolved),
                "utf8 project relative path resolves to existing file");

    std::filesystem::remove_all(root);
}

void test_network_system_session_role_defaults_to_none() {
    Corona::Systems::NetworkSystem sys;

    expect_true(sys.session_role() == Corona::Systems::NetworkSystem::SessionRole::None,
                "network system default session role is none");
    expect_true(sys.session_role_name() == "none",
                "network system default session role label");
}

void test_network_system_repeated_start_preserves_active_role() {
    Corona::Systems::NetworkSystem sys;

    expect_true(sys.start_session("client", 0, 0,
                                  Corona::Systems::NetworkSystem::SessionRole::Client),
                "network system starts client session on ephemeral port");
    expect_true(sys.session_role() == Corona::Systems::NetworkSystem::SessionRole::Client,
                "network system active role starts as client");

    expect_true(sys.start_session("host", 0, 27960,
                                  Corona::Systems::NetworkSystem::SessionRole::Host),
                "network system repeated start succeeds idempotently");
    expect_true(sys.session_role() == Corona::Systems::NetworkSystem::SessionRole::Client,
                "network system repeated start keeps active client role");

    sys.stop_session();
}


void test_actor_device_follow_camera_defaults_false_and_round_trips() {
    auto& hub = Corona::SharedDataHub::instance();

    auto actor = hub.actor_storage().allocate();

    {
        auto a = hub.actor_storage().acquire_read(actor);
        expect_true(!a->follow_camera, "actor follow-camera flag defaults to false");
    }
    {
        auto a = hub.actor_storage().acquire_write(actor);
        a->follow_camera = true;
    }
    {
        auto a = hub.actor_storage().acquire_read(actor);
        expect_true(a->follow_camera, "actor follow-camera flag round trips");
    }

    hub.actor_storage().deallocate(actor);
}

void test_actor_metadata_guid_round_trips_and_clears() {
    auto& hub = Corona::SharedDataHub::instance();

    auto actor = hub.actor_storage().allocate();
    hub.set_actor_guid(actor, "actor-meta-guid");
    expect_true(hub.actor_guid(actor) == "actor-meta-guid",
                "actor metadata guid round trips");

    hub.set_actor_guid(actor, "");
    expect_true(hub.actor_guid(actor).empty(),
                "actor metadata guid clears on empty value");

    hub.actor_storage().deallocate(actor);
}


void test_actor_metadata_handle_cleanup_removes_guid() {
    auto& hub = Corona::SharedDataHub::instance();

    auto actor = hub.actor_storage().allocate();
    hub.set_actor_guid(actor, "actor-cleanup");

    hub.clear_actor_metadata(actor);
    expect_true(hub.actor_guid(actor).empty(),
                "actor metadata cleanup removes guid");

    hub.actor_storage().deallocate(actor);
    auto reused = hub.actor_storage().allocate();
    expect_true(hub.actor_guid(reused).empty(),
                "fresh or reused actor handle has no stale guid");
    hub.actor_storage().deallocate(reused);
}


void test_network_identity_registry_resolves_actor_components() {
    auto& hub = Corona::SharedDataHub::instance();

    auto transform = hub.model_transform_storage().allocate();
    auto geometry = hub.geometry_storage().allocate();
    auto optics = hub.optics_storage().allocate();
    auto profile = hub.profile_storage().allocate();
    auto actor = hub.actor_storage().allocate();

    {
        auto g = hub.geometry_storage().acquire_write(geometry);
        g->transform_handle = transform;
    }
    {
        auto o = hub.optics_storage().acquire_write(optics);
        o->geometry_handle = geometry;
    }
    {
        auto p = hub.profile_storage().acquire_write(profile);
        p->optics_handle = optics;
    }
    {
        auto a = hub.actor_storage().acquire_write(actor);
        a->profile_handles.push_back(profile);
    }

    Corona::Network::NetworkIdentityRegistry registry(hub);
    expect_true(registry.register_actor("actor-guid", actor),
                "actor guid registration succeeds");

    auto resolved = registry.resolve_actor("actor-guid");
    expect_true(resolved.has_value(), "actor guid resolves");
    expect_true(resolved->actor_handle == actor, "actor handle resolved");
    expect_true(resolved->profile_handle == profile, "profile handle resolved");
    expect_true(resolved->geometry_handle == geometry, "geometry handle resolved through optics");
    expect_true(resolved->transform_handle == transform, "transform handle resolved");
    expect_true(resolved->optics_handle == optics, "optics handle resolved");
    expect_true(resolved->transform_seq == hub.model_transform_storage().seq_id(transform),
                "transform seq resolved");

    hub.actor_storage().deallocate(actor);
    hub.profile_storage().deallocate(profile);
    hub.optics_storage().deallocate(optics);
    hub.geometry_storage().deallocate(geometry);
    hub.model_transform_storage().deallocate(transform);
}

void test_network_identity_registry_tracks_local_ownership() {
    auto& hub = Corona::SharedDataHub::instance();

    auto transform = hub.model_transform_storage().allocate();
    auto geometry = hub.geometry_storage().allocate();
    auto profile = hub.profile_storage().allocate();
    auto actor = hub.actor_storage().allocate();

    {
        auto g = hub.geometry_storage().acquire_write(geometry);
        g->transform_handle = transform;
    }
    {
        auto p = hub.profile_storage().acquire_write(profile);
        p->geometry_handle = geometry;
    }
    {
        auto a = hub.actor_storage().acquire_write(actor);
        a->profile_handles.push_back(profile);
    }

    Corona::Network::NetworkIdentityRegistry registry(hub);
    expect_true(registry.register_actor("actor-remote-owner", actor, false),
                "remote-owned actor guid registration succeeds");
    const auto transform_seq = hub.model_transform_storage().seq_id(transform);
    auto ownership = registry.local_ownership_for_storage_seq(
        Corona::Network::StorageID::ST_MODEL_TRANSFORM, transform_seq);
    expect_true(ownership.has_value(), "registered actor ownership resolves");
    expect_true(ownership && !*ownership, "registered actor can be remote-owned");

    hub.actor_storage().deallocate(actor);
    hub.profile_storage().deallocate(profile);
    hub.geometry_storage().deallocate(geometry);
    hub.model_transform_storage().deallocate(transform);
}

void test_network_identity_registry_applies_pending_ownership_override() {
    auto& hub = Corona::SharedDataHub::instance();

    auto transform = hub.model_transform_storage().allocate();
    auto geometry = hub.geometry_storage().allocate();
    auto profile = hub.profile_storage().allocate();
    auto actor = hub.actor_storage().allocate();

    {
        auto g = hub.geometry_storage().acquire_write(geometry);
        g->transform_handle = transform;
    }
    {
        auto p = hub.profile_storage().acquire_write(profile);
        p->geometry_handle = geometry;
    }
    {
        auto a = hub.actor_storage().acquire_write(actor);
        a->profile_handles.push_back(profile);
    }

    Corona::Network::NetworkIdentityRegistry registry(hub);
    registry.set_actor_ownership("actor-pending-owner", false);
    expect_true(registry.register_actor("actor-pending-owner", actor, true),
                "actor registration succeeds with pending ownership override");
    auto resolved = registry.resolve_actor("actor-pending-owner");
    expect_true(resolved.has_value(), "actor with pending ownership resolves");
    expect_true(resolved && !resolved->locally_owned,
                "pending remote ownership overrides registration default");

    hub.actor_storage().deallocate(actor);
    hub.profile_storage().deallocate(profile);
    hub.geometry_storage().deallocate(geometry);
    hub.model_transform_storage().deallocate(transform);
}

void test_sync_engine_marks_actor_dirty_entries_with_guid() {
    auto& hub = Corona::SharedDataHub::instance();

    auto transform = hub.model_transform_storage().allocate();
    auto geometry = hub.geometry_storage().allocate();
    auto profile = hub.profile_storage().allocate();
    auto actor = hub.actor_storage().allocate();

    {
        auto t = hub.model_transform_storage().acquire_write(transform);
        t->position.x = 42.0f;
    }
    {
        auto g = hub.geometry_storage().acquire_write(geometry);
        g->transform_handle = transform;
    }
    {
        auto p = hub.profile_storage().acquire_write(profile);
        p->geometry_handle = geometry;
    }
    {
        auto a = hub.actor_storage().acquire_write(actor);
        a->profile_handles.push_back(profile);
    }

    Corona::Network::NetworkIdentityRegistry registry(hub);
    expect_true(registry.register_actor("actor-guid-sync", actor),
                "actor guid registration for sync succeeds");

    Corona::Network::SyncEngine sync;
    std::vector<std::vector<uint8_t>> outgoing;
    sync.initialize("local-peer");
    sync.set_identity_mapping_callbacks(
        [&](Corona::Network::StorageID sid, uint64_t seq) {
            return registry.actor_guid_for_storage_seq(sid, seq);
        },
        [&](Corona::Network::StorageID sid, const std::string& guid)
            -> std::optional<uint64_t> {
            return registry.storage_seq_for_actor_guid(sid, guid);
        });
    sync.set_on_outgoing([&](const std::vector<uint8_t>& packet) {
        outgoing.push_back(packet);
    });
    sync.poll_and_sync();

    expect_true(!outgoing.empty(), "sync packet exists");
    if (outgoing.empty()) {
        sync.shutdown();
        hub.actor_storage().deallocate(actor);
        hub.profile_storage().deallocate(profile);
        hub.geometry_storage().deallocate(geometry);
        hub.model_transform_storage().deallocate(transform);
        return;
    }

    bool found_actor_operation = false;
    for (const auto& packet : outgoing) {
        auto operation = Corona::Network::parse_editor_sync_operation(
            packet.data(), packet.size());
        if (operation && operation->actor_guid == "actor-guid-sync" &&
            operation->field_name == "xform") {
            found_actor_operation = true;
        }
    }
    expect_true(found_actor_operation, "editor sync transform carries actor guid");

    sync.shutdown();
    hub.actor_storage().deallocate(actor);
    hub.profile_storage().deallocate(profile);
    hub.geometry_storage().deallocate(geometry);
    hub.model_transform_storage().deallocate(transform);
}

void test_sync_engine_emits_optics_but_not_local_handles() {
    auto& hub = Corona::SharedDataHub::instance();

    auto transform = hub.model_transform_storage().allocate();
    auto model_resource = hub.model_resource_storage().allocate();
    auto geometry = hub.geometry_storage().allocate();
    auto optics = hub.optics_storage().allocate();
    auto profile = hub.profile_storage().allocate();
    auto actor = hub.actor_storage().allocate();

    {
        auto mr = hub.model_resource_storage().acquire_write(model_resource);
        mr->model_id = 7;
    }
    {
        auto g = hub.geometry_storage().acquire_write(geometry);
        g->transform_handle = transform;
        g->model_resource_handle = model_resource;
    }
    {
        auto o = hub.optics_storage().acquire_write(optics);
        o->geometry_handle = geometry;
    }
    {
        auto p = hub.profile_storage().acquire_write(profile);
        p->geometry_handle = geometry;
        p->optics_handle = optics;
    }
    {
        auto a = hub.actor_storage().acquire_write(actor);
        a->profile_handles.push_back(profile);
    }

    Corona::Network::NetworkIdentityRegistry registry(hub);
    expect_true(registry.register_actor("actor-transform-only", actor),
                "actor guid registration for transform-only sync succeeds");

    Corona::Network::SyncEngine sync;
    std::vector<std::vector<uint8_t>> outgoing;
    sync.initialize("local-peer");
    sync.set_identity_mapping_callbacks(
        [&](Corona::Network::StorageID sid, uint64_t seq) {
            return registry.actor_guid_for_storage_seq(sid, seq);
        },
        [&](Corona::Network::StorageID sid, const std::string& guid)
            -> std::optional<uint64_t> {
            return registry.storage_seq_for_actor_guid(sid, guid);
        });
    sync.set_on_outgoing([&](const std::vector<uint8_t>& packet) {
        outgoing.push_back(packet);
    });
    sync.poll_and_sync();

    expect_true(!outgoing.empty(), "editor sync emits logical state packets");
    bool found_optics = false;
    for (const auto& packet : outgoing) {
        auto op = Corona::Network::parse_editor_sync_operation(packet.data(), packet.size());
        if (op && op->actor_guid == "actor-transform-only" && op->field_name == "optics") {
            found_optics = true;
            expect_true(op->value.size() > 0, "optics operation carries logical fields");
        }
    }
    expect_true(found_optics, "sync emits optics editor operation");

    sync.shutdown();
    hub.actor_storage().deallocate(actor);
    hub.profile_storage().deallocate(profile);
    hub.optics_storage().deallocate(optics);
    hub.geometry_storage().deallocate(geometry);
    hub.model_resource_storage().deallocate(model_resource);
    hub.model_transform_storage().deallocate(transform);
}

void test_sync_engine_does_not_emit_remote_owned_transform() {
    auto& hub = Corona::SharedDataHub::instance();

    auto transform = hub.model_transform_storage().allocate();
    auto geometry = hub.geometry_storage().allocate();
    auto profile = hub.profile_storage().allocate();
    auto actor = hub.actor_storage().allocate();

    {
        auto t = hub.model_transform_storage().acquire_write(transform);
        t->position.x = 7.0f;
    }
    {
        auto g = hub.geometry_storage().acquire_write(geometry);
        g->transform_handle = transform;
    }
    {
        auto p = hub.profile_storage().acquire_write(profile);
        p->geometry_handle = geometry;
    }
    {
        auto a = hub.actor_storage().acquire_write(actor);
        a->profile_handles.push_back(profile);
    }

    Corona::Network::NetworkIdentityRegistry registry(hub);
    expect_true(registry.register_actor("actor-remote-transform", actor, false),
                "remote-owned actor registration for sync succeeds");

    Corona::Network::SyncEngine sync;
    std::vector<uint8_t> outgoing;
    sync.initialize("local-peer");
    sync.set_identity_mapping_callbacks(
        [&](Corona::Network::StorageID sid, uint64_t seq) {
            return registry.actor_guid_for_storage_seq(sid, seq);
        },
        [&](Corona::Network::StorageID sid, const std::string& guid)
            -> std::optional<uint64_t> {
            return registry.storage_seq_for_actor_guid(sid, guid);
        },
        [&](Corona::Network::StorageID sid, uint64_t seq)
            -> std::optional<bool> {
            return registry.local_ownership_for_storage_seq(sid, seq);
        });
    sync.set_on_outgoing([&](const std::vector<uint8_t>& packet) {
        outgoing = packet;
    });
    sync.poll_and_sync();

    bool found_remote_owned_transform = false;
    if (!outgoing.empty()) {
        Corona::Network::BufferReader reader(outgoing.data(), outgoing.size());
        (void)reader.read_u8();
        (void)reader.read_u32();
        (void)reader.read_u64();
        uint32_t count = reader.read_u32();
        for (uint32_t i = 0; i < count; ++i) {
            auto sid = static_cast<Corona::Network::StorageID>(reader.read_u16());
            (void)reader.read_u64();
            uint16_t key_len = reader.read_u16();
            uint16_t value_len = reader.read_u16();
            std::string key = reader.read_string(key_len);
            reader.pos += value_len;
            if (sid == Corona::Network::StorageID::ST_MODEL_TRANSFORM &&
                key == "actor:actor-remote-transform:xform") {
                found_remote_owned_transform = true;
            }
        }
    }
    expect_true(!found_remote_owned_transform,
                "sync dirty does not emit remote-owned actor transform");

    sync.shutdown();
    hub.actor_storage().deallocate(actor);
    hub.profile_storage().deallocate(profile);
    hub.geometry_storage().deallocate(geometry);
    hub.model_transform_storage().deallocate(transform);
}

void test_sync_engine_does_not_emit_unregistered_transform_when_mapping_enabled() {
    auto& hub = Corona::SharedDataHub::instance();

    auto transform = hub.model_transform_storage().allocate();
    {
        auto t = hub.model_transform_storage().acquire_write(transform);
        t->position.x = 11.0f;
    }

    Corona::Network::SyncEngine sync;
    std::vector<uint8_t> outgoing;
    sync.initialize("local-peer");
    sync.set_identity_mapping_callbacks(
        [](Corona::Network::StorageID, uint64_t) {
            return std::string{};
        },
        [](Corona::Network::StorageID, const std::string&)
            -> std::optional<uint64_t> {
            return std::nullopt;
        },
        [](Corona::Network::StorageID, uint64_t)
            -> std::optional<bool> {
            return std::nullopt;
        });
    sync.set_on_outgoing([&](const std::vector<uint8_t>& packet) {
        outgoing = packet;
    });
    sync.poll_and_sync();

    bool found_unregistered_transform = false;
    if (!outgoing.empty()) {
        Corona::Network::BufferReader reader(outgoing.data(), outgoing.size());
        (void)reader.read_u8();
        (void)reader.read_u32();
        (void)reader.read_u64();
        uint32_t count = reader.read_u32();
        for (uint32_t i = 0; i < count; ++i) {
            auto sid = static_cast<Corona::Network::StorageID>(reader.read_u16());
            (void)reader.read_u64();
            uint16_t key_len = reader.read_u16();
            uint16_t value_len = reader.read_u16();
            std::string key = reader.read_string(key_len);
            reader.pos += value_len;
            if (sid == Corona::Network::StorageID::ST_MODEL_TRANSFORM &&
                key == "xform") {
                found_unregistered_transform = true;
            }
        }
    }
    expect_true(!found_unregistered_transform,
                "sync dirty does not emit unregistered transform when mapping is enabled");

    sync.shutdown();
    hub.model_transform_storage().deallocate(transform);
}

void test_sync_engine_drops_unresolved_actor_transform() {
    auto& hub = Corona::SharedDataHub::instance();

    auto transform = hub.model_transform_storage().allocate();
    {
        auto t = hub.model_transform_storage().acquire_write(transform);
        t->position.x = 1.0f;
        t->position.y = 2.0f;
        t->position.z = 3.0f;
        t->scale.x = 1.0f;
        t->scale.y = 1.0f;
        t->scale.z = 1.0f;
    }
    const auto local_seq = hub.model_transform_storage().seq_id(transform);

    Corona::Network::SyncEngine sync;
    sync.initialize("local-peer");
    sync.set_identity_mapping_callbacks(
        [](Corona::Network::StorageID, uint64_t) {
            return std::string{};
        },
        [](Corona::Network::StorageID, const std::string&)
            -> std::optional<uint64_t> {
            return std::nullopt;
        },
        [](Corona::Network::StorageID, uint64_t)
            -> std::optional<bool> {
            return std::nullopt;
        });

    const float remote_transform[9] = {
        99.0f, 98.0f, 97.0f,
        0.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 1.0f,
    };
    const std::string key = "actor:missing-actor:xform";
    auto entry = Corona::Network::build_dirty_entries(
        Corona::Network::StorageID::ST_MODEL_TRANSFORM,
        local_seq,
        key.c_str(),
        static_cast<uint16_t>(key.size()),
        remote_transform,
        sizeof(remote_transform));
    auto packet = Corona::Network::build_sync_dirty(1, 1, entry, 1);
    sync.handle_incoming("remote-peer", packet.data(), packet.size());

    {
        auto t = hub.model_transform_storage().acquire_read(transform);
        expect_true(t->position.x == 1.0f &&
                        t->position.y == 2.0f &&
                        t->position.z == 3.0f,
                    "sync dirty drops unresolved actor transform");
    }

    sync.shutdown();
    hub.model_transform_storage().deallocate(transform);
}

void test_sync_engine_rejects_legacy_dirty_for_resolved_actor() {
    auto& hub = Corona::SharedDataHub::instance();
    auto transform = hub.model_transform_storage().allocate();
    {
        auto t = hub.model_transform_storage().acquire_write(transform);
        t->position.x = 4.0f;
        t->position.y = 5.0f;
        t->position.z = 6.0f;
        t->scale.x = 1.0f;
        t->scale.y = 1.0f;
        t->scale.z = 1.0f;
    }
    const auto local_seq = hub.model_transform_storage().seq_id(transform);
    Corona::Network::SyncEngine sync;
    sync.initialize("local-peer");
    sync.set_identity_mapping_callbacks(
        [](Corona::Network::StorageID, uint64_t) {
            return std::string("actor-resolved");
        },
        [local_seq](Corona::Network::StorageID storage_id, const std::string& guid)
            -> std::optional<uint64_t> {
            if (storage_id == Corona::Network::StorageID::ST_MODEL_TRANSFORM &&
                guid == "actor-resolved") return local_seq;
            return std::nullopt;
        });
    const float remote_transform[9] = {
        90.0f, 91.0f, 92.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
    const std::string key = "actor:actor-resolved:xform";
    const auto entry = Corona::Network::build_dirty_entries(
        Corona::Network::StorageID::ST_MODEL_TRANSFORM, local_seq,
        key.c_str(), static_cast<uint16_t>(key.size()),
        remote_transform, sizeof(remote_transform));
    const auto packet = Corona::Network::build_sync_dirty(1, 1, entry, 1);

    sync.handle_incoming("legacy-peer", packet.data(), packet.size());

    {
        auto t = hub.model_transform_storage().acquire_read(transform);
        expect_true(t->position.x == 4.0f && t->position.y == 5.0f &&
                        t->position.z == 6.0f,
                    "legacy dirty cannot bypass LWW for resolved actor");
    }
    sync.shutdown();
    hub.model_transform_storage().deallocate(transform);
}

}  // namespace

int main() {
    test_editor_sync_upsert_round_trips_with_lww_version();
    test_editor_sync_rejects_truncated_and_oversized_packets();
    test_editor_sync_has_distinct_message_type();
    test_network_channels_partition_control_transform_bulk_and_realtime();
    test_bulk_poll_budget_limits_packets_and_bytes();
    test_lww_state_merges_by_version_and_field();
    test_lww_state_tombstone_blocks_old_updates();
    test_lww_state_advances_lamport_counter();
    test_sync_engine_applies_versioned_editor_operation();
    test_sync_engine_delete_tombstone_is_idempotent();
    test_sync_engine_does_not_apply_delete_older_than_actor_field();
    test_sync_engine_emits_versioned_snapshot();
    test_sync_engine_creates_versioned_actor_upsert();
    test_sync_engine_does_not_retry_logical_actor_state();
    test_peer_manager_targeted_send_rejects_unknown_peer();
    test_peer_manager_callbacks_can_stop_host();
    test_editor_snapshot_request_has_schema_version();
    test_editor_snapshot_begin_and_end_carry_session_metadata();
    test_editor_snapshot_chunk_round_trips_operation_packets();
    test_editor_snapshot_accepts_operation_from_original_writer();
    test_editor_snapshot_missing_chunk_does_not_mutate_state();
    test_editor_snapshot_queues_sender_incremental_until_complete();
    test_editor_snapshot_timeout_replays_incremental_and_requests_retry();
    test_editor_snapshot_conflicting_duplicate_recovers_incremental();
    test_sync_full_to_sends_begin_chunk_end();
    test_first_peer_receives_snapshot_over_real_channels();
    test_actor_create_carries_actor_guid();
    test_actor_create_unpack_preserves_wire_transform();
    test_actor_create_carries_dependency_paths();
    test_actor_create_carries_actor_json_payload();
    test_actor_transform_update_carries_transform_and_correlation();
    test_actor_delete_carries_scene_guid_and_name();
    test_actor_scene_snapshot_request_carries_scene();
    test_actor_scene_snapshot_carries_json_payload();
    test_actor_state_update_carries_guid_scene_and_json();
    test_file_request_carries_transfer_id();
    test_file_chunk_carries_transfer_id_and_offset();
    test_ownership_claim_carries_actor_guid();
    test_project_relative_path_validation();
    test_project_relative_path_uses_utf8_for_non_ascii_segments();
    test_network_system_session_role_defaults_to_none();
    test_network_system_repeated_start_preserves_active_role();
    test_actor_device_follow_camera_defaults_false_and_round_trips();
    test_actor_metadata_guid_round_trips_and_clears();
    test_actor_metadata_handle_cleanup_removes_guid();
    test_network_identity_registry_resolves_actor_components();
    test_network_identity_registry_tracks_local_ownership();
    test_network_identity_registry_applies_pending_ownership_override();
    test_sync_engine_marks_actor_dirty_entries_with_guid();
    test_sync_engine_emits_optics_but_not_local_handles();
    test_sync_engine_does_not_emit_remote_owned_transform();
    test_sync_engine_does_not_emit_unregistered_transform_when_mapping_enabled();
    test_sync_engine_drops_unresolved_actor_transform();
    test_sync_engine_rejects_legacy_dirty_for_resolved_actor();

    if (g_failed != 0) {
        std::cerr << g_failed << " network protocol test(s) failed\n";
        return 1;
    }
    return 0;
}
