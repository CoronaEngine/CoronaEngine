#include <corona/systems/network/file_transfer.h>
#include <corona/systems/network/lanchat_history_store.h>
#include <corona/systems/network/lanchat_state.h>
#include <corona/systems/network/network_identity.h>
#include <corona/systems/network/network_system.h>
#include <corona/systems/network/protocol.h>
#include <corona/systems/network/sync_engine.h>
#include <corona/shared_data_hub.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

int g_failed = 0;

void expect_true(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failed;
    }
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

void test_lanchat_message_carries_identity_and_sequence() {
    auto packet = Corona::Network::build_chat_message(
        "msg-1", "peer-a", "room-a", 42, "Alice", "hello", 12345);

    Corona::Network::BufferReader reader(packet.data(), packet.size());
    expect_true(static_cast<Corona::Network::MessageType>(reader.read_u8()) ==
                    Corona::Network::MessageType::CHAT_MESSAGE,
                "chat message type");
    expect_true(reader.read_string(reader.read_u16()) == "msg-1",
                "chat message id payload");
    expect_true(reader.read_string(reader.read_u16()) == "peer-a",
                "chat message sender id payload");
    expect_true(reader.read_string(reader.read_u16()) == "room-a",
                "chat message room id payload");
    expect_true(reader.read_u64() == 42, "chat message sequence payload");
    expect_true(reader.read_string(reader.read_u16()) == "Alice",
                "chat message sender name payload");
    expect_true(reader.read_string(reader.read_u16()) == "hello",
                "chat message text payload");
    expect_true(reader.read_u64() == 12345, "chat message timestamp payload");
}

void test_lanchat_v2_message_carries_structured_metadata() {
    auto packet = Corona::Network::build_chat_message_v2(
        Corona::Network::MessageType::CHAT_AGENT_REPLY_V2,
        "msg-2", "agent-a", "room-a", 43, "SceneBot", "working", 12346,
        "agent", "progress", "agent-a", "user-a", "corr-1", "{\"phase\":\"OBJECTS\"}");

    Corona::Network::BufferReader reader(packet.data(), packet.size());
    expect_true(static_cast<Corona::Network::MessageType>(reader.read_u8()) ==
                    Corona::Network::MessageType::CHAT_AGENT_REPLY_V2,
                "chat v2 message type");
    expect_true(reader.read_string(reader.read_u16()) == "msg-2",
                "chat v2 message id payload");
    expect_true(reader.read_string(reader.read_u16()) == "agent-a",
                "chat v2 sender id payload");
    expect_true(reader.read_string(reader.read_u16()) == "room-a",
                "chat v2 room id payload");
    expect_true(reader.read_u64() == 43, "chat v2 sequence payload");
    expect_true(reader.read_string(reader.read_u16()) == "SceneBot",
                "chat v2 sender name payload");
    expect_true(reader.read_string(reader.read_u16()) == "working",
                "chat v2 text payload");
    expect_true(reader.read_u64() == 12346, "chat v2 timestamp payload");
    expect_true(reader.read_string(reader.read_u16()) == "agent",
                "chat v2 sender type payload");
    expect_true(reader.read_string(reader.read_u16()) == "progress",
                "chat v2 message kind payload");
    expect_true(reader.read_string(reader.read_u16()) == "agent-a",
                "chat v2 target agent payload");
    expect_true(reader.read_string(reader.read_u16()) == "user-a",
                "chat v2 source user payload");
    expect_true(reader.read_string(reader.read_u16()) == "corr-1",
                "chat v2 correlation payload");
    expect_true(reader.read_string(reader.read_u16()) == "{\"phase\":\"OBJECTS\"}",
                "chat v2 metadata payload");
}

void test_lanchat_member_update_carries_full_snapshot() {
    const std::vector<Corona::Network::LanChatMember> members{
        {"host-peer", "房主", "online", 100},
        {"guest-peer", "Alice", "online", 200},
    };
    auto packet = Corona::Network::build_chat_member_update("room-a", members);

    Corona::Network::BufferReader reader(packet.data(), packet.size());
    expect_true(static_cast<Corona::Network::MessageType>(reader.read_u8()) ==
                    Corona::Network::MessageType::CHAT_MEMBER_UPDATE,
                "chat member update message type");
    expect_true(reader.read_string(reader.read_u16()) == "room-a",
                "chat member update room id payload");
    expect_true(reader.read_u16() == 2, "chat member update count payload");
    expect_true(reader.read_string(reader.read_u16()) == "host-peer",
                "chat member update host id payload");
    expect_true(reader.read_string(reader.read_u16()) == "房主",
                "chat member update host nickname payload");
    expect_true(reader.read_string(reader.read_u16()) == "online",
                "chat member update host status payload");
    expect_true(reader.read_u64() == 100, "chat member update host last seen payload");
    expect_true(reader.read_string(reader.read_u16()) == "guest-peer",
                "chat member update guest id payload");
    expect_true(reader.read_string(reader.read_u16()) == "Alice",
                "chat member update guest nickname payload");
    expect_true(reader.read_string(reader.read_u16()) == "online",
                "chat member update guest status payload");
    expect_true(reader.read_u64() == 200, "chat member update guest last seen payload");
}

void test_lanchat_history_snapshot_carries_full_history() {
    const std::vector<Corona::Network::LanChatMessage> history{
        {"msg-1", "host-peer", "房主", "room-a", "hello", 1, 1000},
        {"msg-2", "guest-peer", "Alice", "room-a", "hi", 2, 2000},
    };
    auto packet = Corona::Network::build_chat_history_snapshot("room-a", history);

    Corona::Network::BufferReader reader(packet.data(), packet.size());
    expect_true(static_cast<Corona::Network::MessageType>(reader.read_u8()) ==
                    Corona::Network::MessageType::CHAT_HISTORY_SNAPSHOT,
                "chat history snapshot message type");
    expect_true(reader.read_string(reader.read_u16()) == "room-a",
                "chat history snapshot room id payload");
    expect_true(reader.read_u16() == 2, "chat history snapshot count payload");
    expect_true(reader.read_string(reader.read_u16()) == "msg-1",
                "chat history first message id payload");
    expect_true(reader.read_string(reader.read_u16()) == "host-peer",
                "chat history first sender id payload");
    expect_true(reader.read_string(reader.read_u16()) == "room-a",
                "chat history first room id payload");
    expect_true(reader.read_u64() == 1, "chat history first sequence payload");
    expect_true(reader.read_string(reader.read_u16()) == "房主",
                "chat history first sender name payload");
    expect_true(reader.read_string(reader.read_u16()) == "hello",
                "chat history first text payload");
    expect_true(reader.read_u64() == 1000, "chat history first timestamp payload");
    expect_true(reader.read_string(reader.read_u16()) == "msg-2",
                "chat history second message id payload");
}

void test_lanchat_v2_history_snapshot_preserves_message_kind() {
    Corona::Network::LanChatMessage progress;
    progress.message_id = "msg-progress";
    progress.sender_id = "agent-a";
    progress.sender_name = "SceneBot";
    progress.room_id = "room-a";
    progress.text = "生成进度 50%";
    progress.seq = 1;
    progress.timestamp_ms = 1000;
    progress.sender_type = "agent";
    progress.message_kind = "progress";
    progress.target_agent_id = "agent-a";
    progress.source_user_id = "user-a";
    progress.correlation_id = "corr-1";
    progress.metadata_json = "{\"phase\":\"OBJECTS\"}";

    auto packet = Corona::Network::build_chat_history_snapshot_v2("room-a", {progress});
    Corona::Network::BufferReader reader(packet.data(), packet.size());
    expect_true(static_cast<Corona::Network::MessageType>(reader.read_u8()) ==
                    Corona::Network::MessageType::CHAT_HISTORY_SNAPSHOT_V2,
                "chat v2 history snapshot type");
    expect_true(reader.read_string(reader.read_u16()) == "room-a",
                "chat v2 history room id");
    expect_true(reader.read_u16() == 1, "chat v2 history count");
    expect_true(reader.read_string(reader.read_u16()) == "msg-progress",
                "chat v2 history message id");
    expect_true(reader.read_string(reader.read_u16()) == "agent-a",
                "chat v2 history sender id");
    expect_true(reader.read_string(reader.read_u16()) == "room-a",
                "chat v2 history room id in message");
    expect_true(reader.read_u64() == 1, "chat v2 history seq");
    expect_true(reader.read_string(reader.read_u16()) == "SceneBot",
                "chat v2 history sender name");
    expect_true(reader.read_string(reader.read_u16()) == "生成进度 50%",
                "chat v2 history text");
    expect_true(reader.read_u64() == 1000, "chat v2 history timestamp");
    expect_true(reader.read_string(reader.read_u16()) == "agent",
                "chat v2 history sender type");
    expect_true(reader.read_string(reader.read_u16()) == "progress",
                "chat v2 history message kind");
}

void test_lanchat_join_reject_carries_error_code() {
    auto packet = Corona::Network::build_chat_join_reject(
        "room-a", "ROOM_NOT_FOUND", "room is not open");

    Corona::Network::BufferReader reader(packet.data(), packet.size());
    expect_true(static_cast<Corona::Network::MessageType>(reader.read_u8()) ==
                    Corona::Network::MessageType::CHAT_JOIN_REJECT,
                "chat join reject message type");
    expect_true(reader.read_string(reader.read_u16()) == "room-a",
                "chat join reject room id payload");
    expect_true(reader.read_string(reader.read_u16()) == "ROOM_NOT_FOUND",
                "chat join reject code payload");
    expect_true(reader.read_string(reader.read_u16()) == "room is not open",
                "chat join reject reason payload");
}

void test_lanchat_state_deduplicates_messages_and_tracks_agents() {
    Corona::Network::LanChatState state;
    expect_true(state.open_room("room-a", "host-peer", "房主"),
                "lanchat state opens room");
    expect_true(state.join_member("peer-b", "Alice").ok,
                "lanchat state joins member");

    auto first = state.record_message("msg-1", "peer-b", "Alice", "hello", 10);
    auto duplicate = state.record_message("msg-1", "peer-b", "Alice", "hello again", 11);
    expect_true(first.accepted, "lanchat state accepts first message");
    expect_true(!duplicate.accepted, "lanchat state rejects duplicate message id");
    expect_true(state.history().size() == 1, "lanchat state keeps one message");
    expect_true(state.history().front().seq == 1, "lanchat state assigns sequence");
    expect_true(state.history().front().text == "hello", "lanchat state preserves first text");

    expect_true(state.register_agent("agent-1", "Designer", "persona", "peer-b").ok,
                "lanchat state registers agent");
    expect_true(state.agents().size() == 1, "lanchat state tracks agent roster");
    expect_true(state.remove_agent("agent-1").ok, "lanchat state removes agent");
    expect_true(state.agents().empty(), "lanchat state agent roster is empty");
}

void test_lanchat_state_assigns_unique_duplicate_member_names() {
    Corona::Network::LanChatState state;
    expect_true(state.open_room("room-a", "host-peer", "Alice"),
                "lanchat state opens room with host nickname");
    expect_true(state.join_member("peer-b", "Alice").ok,
                "lanchat state accepts first duplicate nickname");
    expect_true(state.join_member("peer-c", "Alice").ok,
                "lanchat state accepts second duplicate nickname");
    expect_true(state.join_member("peer-d", "Alice_1").ok,
                "lanchat state accepts duplicate suffixed nickname");

    const auto& members = state.members();
    expect_true(members.size() == 4, "lanchat state keeps all duplicate-name members");
    expect_true(members[0].nickname == "Alice", "host keeps original duplicate base name");
    expect_true(members[1].nickname == "Alice_1", "first duplicate gets _1 suffix");
    expect_true(members[2].nickname == "Alice_2", "second duplicate gets _2 suffix");
    expect_true(members[3].nickname == "Alice_1_1",
                "duplicate explicit suffixed nickname gets its own suffix");
}

void test_lanchat_state_updates_authoritative_message_and_history_snapshot() {
    Corona::Network::LanChatState state;
    state.open_room("room-a", "guest-peer", "Alice");

    Corona::Network::LanChatMessage provisional;
    provisional.message_id = "msg-1";
    provisional.sender_id = "guest-peer";
    provisional.sender_name = "Alice";
    provisional.room_id = "room-a";
    provisional.text = "hello";
    provisional.seq = 0;
    provisional.timestamp_ms = 1000;
    auto first = state.apply_remote_message(provisional);
    expect_true(first.accepted, "lanchat state accepts provisional message");

    Corona::Network::LanChatMessage authoritative = provisional;
    authoritative.seq = 7;
    authoritative.timestamp_ms = 1001;
    auto updated = state.apply_remote_message(authoritative);
    expect_true(updated.accepted, "lanchat state updates duplicate authoritative message");
    expect_true(state.history().size() == 1, "lanchat state keeps one authoritative message");
    expect_true(state.history().front().seq == 7, "lanchat state updates authoritative sequence");

    const std::vector<Corona::Network::LanChatMessage> snapshot{
        {"msg-2", "host-peer", "房主", "room-a", "before", 1, 900},
        authoritative,
    };
    state.apply_history_snapshot(snapshot);
    expect_true(state.history().size() == 2, "lanchat state applies full history snapshot");
    expect_true(state.history()[0].message_id == "msg-2",
                "lanchat state sorts history snapshot by sequence");
    expect_true(state.history()[1].message_id == "msg-1",
                "lanchat state keeps authoritative snapshot message");
    expect_true(state.next_seq() == 8, "lanchat state advances next sequence from snapshot");
}

void test_lanchat_state_queues_plain_chat_for_coordinator_sync() {
    Corona::Network::LanChatState state;
    state.open_room("room-a", "host-peer", "Host");

    auto message = state.record_message(
        "msg-coord", "user-a", "Alice", "第一批后加一座天使雕塑", 1000);
    expect_true(message.accepted, "lanchat state accepts coordinator sync chat");

    auto sync = state.pop_coordinator_sync_message();
    expect_true(sync.has_value(), "lanchat state queues ordinary chat for coordinator sync");
    expect_true(sync->message_id == "msg-coord", "coordinator sync carries message id");
    expect_true(sync->text == "第一批后加一座天使雕塑", "coordinator sync carries text");
    expect_true(!state.pop_coordinator_sync_message().has_value(),
                "coordinator sync queue drains once");
}

void test_lanchat_state_queues_host_chat_for_coordinator_sync() {
    Corona::Network::LanChatState state;
    state.open_room("room-a", "host-peer", "Host");

    auto message = state.record_message_ex(
        "msg-host", "host-peer", "Host", "房主确认先做室外入口", 1000,
        "host", "chat");
    expect_true(message.accepted, "lanchat state accepts host chat");

    auto sync = state.pop_coordinator_sync_message();
    expect_true(sync.has_value(), "lanchat state queues host chat for coordinator sync");
    expect_true(sync->sender_type == "host", "coordinator sync preserves host sender type");
}

void test_lanchat_state_filters_internal_messages_from_coordinator_sync() {
    Corona::Network::LanChatState state;
    state.open_room("room-a", "host-peer", "Host");

    auto progress = state.record_message_ex(
        "msg-progress", "system", "System", "内部进度", 1000,
        "system", "action_status");
    auto agent = state.record_message_ex(
        "msg-agent", "agent-a", "SceneBot", "我来处理", 1001,
        "agent", "agent_reply");

    expect_true(progress.accepted, "lanchat state accepts progress message");
    expect_true(agent.accepted, "lanchat state accepts agent message");
    expect_true(!state.pop_coordinator_sync_message().has_value(),
                "coordinator sync filters agent and system messages");
}

void test_lanchat_state_can_filter_generation_start_for_client_sync() {
    Corona::Network::LanChatState host_state;
    host_state.open_room("room-a", "host-peer", "Host");
    auto host_generate = host_state.apply_remote_message({
        "msg-host-generate", "host-peer", "Host", "room-a", "开始生成", 1, 1000,
        "host", "chat", "", "", "", "{\"draft_action\":\"generate\"}"
    });
    expect_true(host_generate.accepted, "remote generation chat accepted for host policy check");
    auto host_sync = host_state.pop_coordinator_sync_message();
    expect_true(host_sync.has_value(), "host coordinator sync still yields generation start");
    expect_true(host_sync->message_id == "msg-host-generate",
                "host coordinator sync keeps generation start message");

    Corona::Network::LanChatState state;
    state.open_room("room-a", "client-peer", "Guest");

    auto generate = state.apply_remote_message({
        "msg-generate", "host-peer", "Host", "room-a", "开始生成", 1, 1000,
        "host", "chat", "", "", "", "{\"draft_action\":\"generate\"}"
    });
    auto regular = state.apply_remote_message({
        "msg-chat", "host-peer", "Host", "room-a", "补充一个床头灯", 2, 1001,
        "host", "chat", "", "", "", "{\"draft_action\":\"supplement\"}"
    });
    expect_true(generate.accepted, "remote generation chat accepted into history");
    expect_true(regular.accepted, "remote supplement chat accepted into history");

    auto sync = state.pop_coordinator_sync_message(/*allow_generation_start=*/false);
    expect_true(sync.has_value(), "client coordinator sync skips generation start and keeps later chat");
    expect_true(sync->message_id == "msg-chat", "client coordinator sync yields next non-generation chat");
    expect_true(!state.pop_coordinator_sync_message(/*allow_generation_start=*/false).has_value(),
                "client coordinator sync drains filtered generation start");
}

void test_lanchat_state_host_message_can_trigger_local_agent() {
    Corona::Network::LanChatState state;
    state.open_room("room-a", "host-peer", "Host");
    expect_true(state.register_agent("agent-1", "SceneBot", "scene helper", "host-peer").ok,
                "lanchat state registers host-owned agent");

    auto message = state.record_message_ex(
        "msg-host-agent", "host-peer", "Host", "@SceneBot 整理一下方案", 1000,
        "host", "chat");
    expect_true(message.accepted, "lanchat state accepts host agent mention");

    state.enqueue_agent_triggers_for_message(message.message, "host-peer");
    auto trigger = state.pop_agent_trigger();
    expect_true(trigger.has_value(), "host chat can trigger local agent");
    expect_true(trigger->sender_type == "host", "host trigger preserves sender type");
}

void test_lanchat_state_applies_authoritative_member_snapshot() {
    Corona::Network::LanChatState state;
    state.open_room("room-a", "guest-peer", "Alice");
    state.join_member("stale-peer", "Stale");

    const std::vector<Corona::Network::LanChatMember> members{
        {"host-peer", "房主", "online", 100},
        {"guest-peer", "Alice", "online", 200},
    };
    state.apply_member_snapshot(members);

    expect_true(state.members().size() == 2,
                "lanchat state replaces stale members with snapshot");
    expect_true(state.members()[0].member_id == "host-peer",
                "lanchat state snapshot keeps host member");
    expect_true(state.members()[1].member_id == "guest-peer",
                "lanchat state snapshot keeps guest member");
}

void test_lanchat_state_enqueues_local_agent_trigger_from_mention() {
    Corona::Network::LanChatState state;
    state.open_room("room-a", "local-peer", "Host");
    expect_true(state.register_agent("agent-1", "SceneBot", "scene helper", "local-peer").ok,
                "lanchat state registers local agent");
    expect_true(state.register_agent("agent-2", "RemoteBot", "remote helper", "remote-peer").ok,
                "lanchat state registers remote agent");

    auto message = state.record_message(
        "msg-mention", "user-peer", "Alice", "@SceneBot 111 @RemoteBot", 1000);
    expect_true(message.accepted, "lanchat state accepts mentioned message");

    state.enqueue_agent_triggers_for_message(message.message, "local-peer");
    auto trigger = state.pop_agent_trigger();
    expect_true(trigger.has_value(), "lanchat state enqueues local owned agent trigger");
    expect_true(trigger->message_id == "msg-mention", "lanchat trigger carries message id");
    expect_true(trigger->agent_id == "agent-1", "lanchat trigger carries local agent id");
    expect_true(trigger->agent_name == "SceneBot", "lanchat trigger carries agent name");
    expect_true(trigger->persona == "scene helper", "lanchat trigger carries persona");
    expect_true(trigger->text == "@SceneBot 111 @RemoteBot", "lanchat trigger carries source text");
    expect_true(trigger->sender_type == "user", "lanchat trigger carries sender type");
    expect_true(trigger->message_kind == "chat", "lanchat trigger carries message kind");
    expect_true(trigger->history.size() == 1, "lanchat trigger carries recent history");
    expect_true(!state.pop_agent_trigger().has_value(),
                "lanchat state does not trigger remote owned agent");
}

void test_lanchat_state_structured_target_overrides_text_mentions() {
    Corona::Network::LanChatState state;
    state.open_room("room-a", "local-peer", "Host");
    expect_true(state.register_agent("agent-1", "SceneBot", "scene helper", "local-peer").ok,
                "lanchat state registers first local agent");
    expect_true(state.register_agent("agent-2", "HelperBot", "helper", "local-peer").ok,
                "lanchat state registers second local agent");

    auto message = state.record_message_ex(
        "msg-targeted", "user-peer", "Alice", "@SceneBot @HelperBot 只问第二个", 1000,
        "user", "chat", "agent-2", "user-peer", "corr-1", "{}");
    expect_true(message.accepted, "lanchat state accepts targeted agent message");

    state.enqueue_agent_triggers_for_message(message.message, "local-peer");
    auto trigger = state.pop_agent_trigger();
    expect_true(trigger.has_value(), "structured target enqueues selected local agent");
    expect_true(trigger->agent_id == "agent-2",
                "structured target overrides extra text mentions");
    expect_true(trigger->correlation_id == "corr-1",
                "structured target trigger preserves correlation id");
    expect_true(!state.pop_agent_trigger().has_value(),
                "structured target does not enqueue mentioned non-target agents");
}

void test_lanchat_state_implicit_trigger_only_for_single_local_agent() {
    Corona::Network::LanChatState state;
    state.open_room("room-a", "local-peer", "Host");
    expect_true(state.register_agent("agent-1", "SceneBot", "scene helper", "local-peer").ok,
                "lanchat state registers single local agent");

    auto direct = state.record_message("msg-direct", "user-peer", "Alice", "生成一个广场", 1000);
    state.enqueue_agent_triggers_for_message(direct.message, "local-peer");
    auto trigger = state.pop_agent_trigger();
    expect_true(trigger.has_value(), "single local agent receives direct non-mention message");
    expect_true(trigger->agent_id == "agent-1", "implicit trigger targets the only local agent");
    expect_true(trigger->text == "生成一个广场", "implicit trigger carries source text");

    expect_true(state.register_agent("agent-2", "HelperBot", "helper", "local-peer").ok,
                "lanchat state registers second local agent");
    auto ambiguous = state.record_message("msg-ambiguous", "user-peer", "Alice", "再加一个喷泉", 1001);
    state.enqueue_agent_triggers_for_message(ambiguous.message, "local-peer");
    expect_true(!state.pop_agent_trigger().has_value(),
                "multiple local agents require explicit mention");
}

void test_lanchat_state_can_filter_generation_start_agent_trigger_for_client() {
    Corona::Network::LanChatState host_state;
    host_state.open_room("room-a", "host-peer", "Host");
    expect_true(host_state.register_agent("agent-1", "SceneBot", "scene helper", "host-peer").ok,
                "host registers local agent for generation trigger policy check");

    auto host_generate = host_state.record_message_ex(
        "msg-host-generate", "host-peer", "Host", "开始生成", 1000,
        "host", "chat", "", "", "", "{\"draft_action\":\"generate\"}");
    expect_true(host_generate.accepted, "host accepts generation start chat");

    host_state.enqueue_agent_triggers_for_message(host_generate.message, "host-peer");
    auto host_trigger = host_state.pop_agent_trigger();
    expect_true(host_trigger.has_value(), "host policy keeps generation start agent trigger");
    expect_true(host_trigger->message_id == "msg-host-generate",
                "host generation trigger preserves source message");

    Corona::Network::LanChatState client_state;
    client_state.open_room("room-a", "client-peer", "Guest");
    expect_true(client_state.register_agent("agent-1", "SceneBot", "scene helper", "client-peer").ok,
                "client registers single local agent");

    auto client_generate = client_state.apply_remote_message({
        "msg-client-generate", "host-peer", "Host", "room-a", "开始生成", 1, 1000,
        "host", "chat", "", "", "", "{\"draft_action\":\"generate\"}"
    });
    expect_true(client_generate.accepted, "client accepts remote generation start into history");

    client_state.enqueue_agent_triggers_for_message(
        client_generate.message,
        "client-peer",
        /*is_agent_reply=*/false,
        /*allow_agent_execution=*/true,
        /*allow_generation_start=*/false);
    expect_true(!client_state.pop_agent_trigger().has_value(),
                "client policy filters generation start agent trigger");

    auto supplement = client_state.apply_remote_message({
        "msg-client-supplement", "host-peer", "Host", "room-a", "补充一个床头灯", 2, 1001,
        "host", "chat", "", "", "", "{\"draft_action\":\"supplement\"}"
    });
    client_state.enqueue_agent_triggers_for_message(
        supplement.message,
        "client-peer",
        /*is_agent_reply=*/false,
        /*allow_agent_execution=*/true,
        /*allow_generation_start=*/false);
    auto supplement_trigger = client_state.pop_agent_trigger();
    expect_true(supplement_trigger.has_value(), "client policy keeps non-generation agent trigger");
    expect_true(supplement_trigger->message_id == "msg-client-supplement",
                "client non-generation trigger preserves source message");
}

void test_lanchat_state_can_disable_agent_execution_for_client_policy() {
    Corona::Network::LanChatState host_state;
    host_state.open_room("room-a", "host-peer", "Host");
    expect_true(host_state.register_agent("agent-1", "SceneBot", "scene helper", "host-peer").ok,
                "host registers local agent for execution policy check");

    auto host_chat = host_state.record_message_ex(
        "msg-host-chat", "user-peer", "Alice", "你是谁", 1000,
        "user", "chat", "", "", "", "{\"draft_action\":\"chat\"}");
    expect_true(host_chat.accepted, "host accepts ordinary chat for execution policy check");
    host_state.enqueue_agent_triggers_for_message(host_chat.message, "host-peer");
    expect_true(host_state.pop_agent_trigger().has_value(),
                "host policy keeps ordinary agent trigger");

    Corona::Network::LanChatState client_state;
    client_state.open_room("room-a", "client-peer", "Guest");
    expect_true(client_state.register_agent("agent-1", "SceneBot", "scene helper", "client-peer").ok,
                "client registers local agent for execution policy check");

    auto client_chat = client_state.apply_remote_message({
        "msg-client-chat", "host-peer", "Host", "room-a", "你是谁", 1, 1000,
        "host", "chat", "", "", "", "{\"draft_action\":\"chat\"}"
    });
    expect_true(client_chat.accepted, "client accepts remote ordinary chat into history");
    client_state.enqueue_agent_triggers_for_message(
        client_chat.message,
        "client-peer",
        /*is_agent_reply=*/false,
        /*allow_agent_execution=*/false,
        /*allow_generation_start=*/false);
    expect_true(!client_state.pop_agent_trigger().has_value(),
                "client policy disables ordinary agent trigger execution");
}

void test_lanchat_state_deduplicates_agent_triggers() {
    Corona::Network::LanChatState state;
    state.open_room("room-a", "local-peer", "Host");
    state.register_agent("agent-1", "SceneBot", "scene helper", "local-peer");

    auto message = state.record_message("msg-1", "user-peer", "Alice", "@SceneBot 111", 1000);
    state.enqueue_agent_triggers_for_message(message.message, "local-peer");
    state.enqueue_agent_triggers_for_message(message.message, "local-peer");

    expect_true(state.pop_agent_trigger().has_value(),
                "lanchat state returns first agent trigger");
    expect_true(!state.pop_agent_trigger().has_value(),
                "lanchat state suppresses duplicate agent trigger");
}

void test_lanchat_state_does_not_trigger_agent_reply_or_duplicate_names() {
    Corona::Network::LanChatState state;
    state.open_room("room-a", "local-peer", "Host");
    expect_true(state.register_agent("agent-1", "SceneBot", "scene helper", "local-peer").ok,
                "lanchat state registers first agent name");
    expect_true(!state.register_agent("agent-2", "SceneBot", "other helper", "local-peer").ok,
                "lanchat state rejects duplicate agent name");

    Corona::Network::LanChatMessage reply;
    reply.message_id = "reply-1";
    reply.sender_id = "agent-1";
    reply.sender_name = "SceneBot";
    reply.room_id = "room-a";
    reply.text = "@SceneBot done";
    reply.seq = 1;
    reply.timestamp_ms = 1000;

    state.enqueue_agent_triggers_for_message(reply, "local-peer", true);
    expect_true(!state.pop_agent_trigger().has_value(),
                "lanchat state does not enqueue triggers for agent replies");
}

void test_lanchat_state_does_not_trigger_structured_progress_messages() {
    Corona::Network::LanChatState state;
    state.open_room("room-a", "local-peer", "Host");
    state.register_agent("agent-1", "SceneBot", "scene helper", "local-peer");

    auto progress = state.record_message_ex(
        "progress-1", "agent-1", "SceneBot", "@SceneBot working", 1000,
        "agent", "progress", "agent-1", "user-peer", "corr-1", "{}");
    expect_true(progress.accepted, "lanchat state accepts structured progress");

    state.enqueue_agent_triggers_for_message(progress.message, "local-peer");
    expect_true(!state.pop_agent_trigger().has_value(),
                "structured progress does not trigger agents");
}

void test_lanchat_state_enqueues_virtual_gm_from_mention_and_target() {
    Corona::Network::LanChatState state;
    state.open_room("room-a", "local-peer", "Host");
    state.register_agent("agent-1", "SceneBot", "scene helper", "local-peer");

    auto gm_chat = state.record_message_ex(
        "gm-chat-1", "user-peer", "Alice", "@GM 整理一下大家的想法", 1000,
        "user", "chat", "gm", "user-peer", "", "{}");
    expect_true(gm_chat.accepted, "lanchat accepts @GM chat message");

    state.enqueue_agent_triggers_for_message(gm_chat.message, "local-peer");
    auto trigger = state.pop_agent_trigger();
    expect_true(trigger.has_value(), "virtual GM receives @GM chat trigger");
    expect_true(trigger->agent_id == "gm", "virtual GM trigger has gm id");
    expect_true(trigger->agent_name == "GM", "virtual GM trigger has GM name");
    expect_true(trigger->target_agent_id == "gm", "virtual GM trigger carries target_agent_id");
    expect_true(!state.pop_agent_trigger().has_value(),
                "@GM does not also trigger ordinary local agents");

    auto confirmation = state.record_message_ex(
        "gm-confirm-1", "user-peer", "Alice", "@GM 确认 gm-1", 1001,
        "user", "confirmation", "gm", "user-peer", "gm-1", "{\"decision\":\"confirm\"}");
    expect_true(confirmation.accepted, "lanchat accepts structured GM confirmation");
    state.enqueue_agent_triggers_for_message(confirmation.message, "local-peer");
    auto confirm_trigger = state.pop_agent_trigger();
    expect_true(confirm_trigger.has_value(), "structured confirmation can reach virtual GM");
    expect_true(confirm_trigger->message_kind == "confirmation",
                "virtual GM confirmation trigger preserves message kind");
    expect_true(confirm_trigger->correlation_id == "gm-1",
                "virtual GM confirmation trigger preserves correlation id");
}

void test_lanchat_state_tracks_locks_and_preview_conflicts() {
    Corona::Network::LanChatState state;
    state.open_room("room-a", "host-peer", "房主");

    expect_true(state.lock_object("chair", "alice", "move", 1000).ok,
                "lanchat state locks object");
    expect_true(!state.lock_object("chair", "bob", "move", 1001).ok,
                "lanchat state rejects conflicting lock");
    expect_true(state.locked_by("chair", 1002) == "alice",
                "lanchat state reports lock owner");
    expect_true(state.unlock_object("chair", "alice").ok,
                "lanchat state unlocks object");
    expect_true(state.locked_by("chair", 1003).empty(),
                "lanchat state clears lock owner");

    state.broadcast_intent("alice", "move chair", {1.0f, 0.0f, 1.0f}, "moving", 2000);
    auto conflict = state.check_preview_collision("bob", {1.2f, 0.0f, 1.1f}, 0.5f, 2001);
    expect_true(conflict == "alice", "lanchat state detects preview conflict");
    auto no_conflict = state.check_preview_collision("alice", {1.2f, 0.0f, 1.1f}, 0.5f, 2001);
    expect_true(no_conflict.empty(), "lanchat state ignores same user preview");
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

void test_lanchat_start_room_reuses_active_network_session_role() {
    Corona::Systems::NetworkSystem sys;

    expect_true(sys.start_session("host", 0, 0,
                                  Corona::Systems::NetworkSystem::SessionRole::Host),
                "network system starts host session before lanchat room");
    expect_true(sys.lanchat_start_room("room-id", "host-user", 27960),
                "lanchat start room reuses active network session");
    expect_true(sys.session_role() == Corona::Systems::NetworkSystem::SessionRole::Host,
                "lanchat start room keeps active host role");

    sys.stop_session();
}

void test_lanchat_history_persists_for_local_room() {
    const auto root = std::filesystem::temp_directory_path() /
        "corona_lanchat_history_persistence_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    {
        Corona::Systems::NetworkSystem sys;
        sys.set_project_root(root.string());
        expect_true(sys.lanchat_start_local_room("single-default", "Host"),
                    "local lanchat room starts before history write");
        auto sent = sys.lanchat_send_message_ex(
            "继续生成夜市场景", "confirmation", "gm", "local-single-player",
            "gm-42", "{\"decision\":\"confirm\"}");
        expect_true(sent.accepted, "local lanchat message accepted for persistence");
        auto agent_result = sys.lanchat_register_agent(
            "agent-elder", "长者", "沉稳、传统、实用", "local-single-player");
        expect_true(agent_result.ok, "local lanchat agent accepted for persistence");
        expect_true(sys.lanchat_history().size() == 1,
                    "local lanchat history has first message");
        sys.lanchat_stop_local_room();
    }

    {
        Corona::Systems::NetworkSystem sys;
        sys.set_project_root(root.string());
        expect_true(sys.lanchat_start_local_room("single-default", "Host"),
                    "local lanchat room starts without auto-loading history");
        const auto& history = sys.lanchat_history();
        const auto& agents = sys.lanchat_agents();
        expect_true(history.empty(),
                    "local lanchat room does not auto-enter persisted history");
        expect_true(agents.empty(),
                    "local lanchat room does not auto-enter persisted agents");

        const auto loaded_history = sys.lanchat_load_history_room("single-default");
        const auto loaded_agents = sys.lanchat_load_history_agents("single-default");
        expect_true(loaded_history.size() == 1,
                    "explicit lanchat history load returns persisted messages");
        expect_true(loaded_agents.size() == 1,
                    "explicit lanchat history load returns persisted agents");
        expect_true(sys.lanchat_restore_history_room("single-default"),
                    "explicit lanchat history restore applies selected history");
        expect_true(sys.lanchat_history().size() == 1,
                    "selected lanchat history becomes active room content");
        expect_true(sys.lanchat_agents().size() == 1,
                    "selected lanchat history restores active room agents");
        if (!loaded_history.empty()) {
            expect_true(loaded_history.front().text == "继续生成夜市场景",
                        "explicit lanchat history load preserves text");
            expect_true(loaded_history.front().message_kind == "confirmation",
                        "explicit lanchat history load preserves message kind");
            expect_true(loaded_history.front().target_agent_id == "gm",
                        "explicit lanchat history load preserves target agent");
            expect_true(loaded_history.front().correlation_id == "gm-42",
                        "explicit lanchat history load preserves correlation id");
            expect_true(loaded_history.front().metadata_json == "{\"decision\":\"confirm\"}",
                        "explicit lanchat history load preserves metadata json");
        }
        if (!loaded_agents.empty()) {
            expect_true(loaded_agents.front().agent_id == "agent-elder",
                        "explicit lanchat history load preserves agent id");
            expect_true(loaded_agents.front().persona == "沉稳、传统、实用",
                        "explicit lanchat history load preserves agent persona");
        }
        sys.lanchat_stop_local_room();
    }

    std::filesystem::remove_all(root);
}

void test_lanchat_history_uses_one_file_per_room_session() {
    const auto root = std::filesystem::temp_directory_path() /
        "corona_lanchat_history_session_file_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    {
        Corona::Systems::NetworkSystem sys;
        sys.set_project_root(root.string());
        expect_true(sys.lanchat_start_local_room("single-default", "Host"),
                    "first local lanchat session starts");
        auto sent = sys.lanchat_send_message("第一轮会话");
        expect_true(sent.accepted, "first session message accepted");
        sys.lanchat_stop_local_room();
    }

    {
        Corona::Systems::NetworkSystem sys;
        sys.set_project_root(root.string());
        expect_true(sys.lanchat_start_local_room("single-default", "Host"),
                    "second local lanchat session starts");
        auto sent = sys.lanchat_send_message("第二轮会话");
        expect_true(sent.accepted, "second session message accepted");
        sys.lanchat_stop_local_room();
    }

    {
        Corona::Systems::NetworkSystem sys;
        sys.set_project_root(root.string());
        const auto rooms = sys.lanchat_history_rooms();
        expect_true(rooms.size() == 2,
                    "same lanchat room id creates one persisted file per session");
        if (rooms.size() >= 2) {
            expect_true(rooms[0].room_id == "single-default",
                        "history summary preserves display room id");
            expect_true(!rooms[0].session_id.empty(),
                        "history summary exposes session id");
            expect_true(rooms[0].session_id != rooms[1].session_id,
                        "history sessions have distinct file ids");
            const auto latest = sys.lanchat_load_history_room(rooms[0].session_id);
            const auto older = sys.lanchat_load_history_room(rooms[1].session_id);
            expect_true(latest.size() == 1 && older.size() == 1,
                        "each persisted session file contains only its own messages");
        }
    }

    std::filesystem::remove_all(root);
}

void test_lanchat_history_store_lists_rooms_with_summaries() {
    const auto root = std::filesystem::temp_directory_path() /
        "corona_lanchat_history_list_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    Corona::Network::LanChatHistoryStore store(root / "Saved" / "LANChat" / "history");

    Corona::Network::LanChatMessage first{};
    first.message_id = "single:1";
    first.sender_id = "local-single-player";
    first.sender_name = "房主";
    first.room_id = "single-default";
    first.text = "第一条单人历史";
    first.seq = 1;
    first.timestamp_ms = 1000;
    store.append_message(first.room_id, first);

    Corona::Network::LanChatMessage latest{};
    latest.message_id = "single:2";
    latest.sender_id = "system";
    latest.sender_name = "系统";
    latest.room_id = "single-default";
    latest.text = "最后一条单人历史";
    latest.seq = 2;
    latest.timestamp_ms = 3000;
    latest.message_kind = "action_status";
    store.append_message(latest.room_id, latest);

    Corona::Network::LanChatMessage other{};
    other.message_id = "multi:1";
    other.sender_id = "host";
    other.sender_name = "房主";
    other.room_id = "room-42";
    other.text = "多人房间历史";
    other.seq = 1;
    other.timestamp_ms = 2000;
    store.append_message(other.room_id, other);

    const auto rooms = store.list_rooms();
    expect_true(rooms.size() == 2, "history store lists persisted rooms");
    if (rooms.size() >= 2) {
        expect_true(!rooms[0].session_id.empty(),
                    "history store summary records session file id");
        expect_true(rooms[0].room_id == "single-default",
                    "history store sorts rooms by newest message");
        expect_true(rooms[0].message_count == 2,
                    "history store summary records message count");
        expect_true(rooms[0].last_timestamp_ms == 3000,
                    "history store summary records last timestamp");
        expect_true(rooms[0].last_text == "最后一条单人历史",
                    "history store summary records last message text");
        expect_true(rooms[1].room_id == "room-42",
                    "history store includes older rooms");
    }

    std::filesystem::remove_all(root);
}

void test_lanchat_history_store_persists_agent_roster() {
    const auto root = std::filesystem::temp_directory_path() /
        "corona_lanchat_history_agents_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    Corona::Network::LanChatHistoryStore store(root / "Saved" / "LANChat" / "history");

    std::vector<Corona::Network::LanChatAgent> agents;
    agents.push_back({"agent-elder", "长者", "沉稳、传统、实用", "local-single-player"});
    agents.push_back({"agent-merchant", "商人", "交易、摊位、动线", "local-single-player"});

    store.save_agents("single-default", agents);

    const auto loaded = store.load_agents("single-default");
    expect_true(loaded.size() == 2, "history store persists lanchat agent roster");
    if (loaded.size() >= 2) {
        expect_true(loaded[0].agent_id == "agent-elder",
                    "history store preserves first agent id");
        expect_true(loaded[0].name == "长者",
                    "history store preserves first agent name");
        expect_true(loaded[0].persona == "沉稳、传统、实用",
                    "history store preserves first agent persona");
        expect_true(loaded[0].owner_id == "local-single-player",
                    "history store preserves first agent owner");
        expect_true(loaded[1].agent_id == "agent-merchant",
                    "history store preserves second agent id");
    }

    store.save_agents("single-default", {agents.front()});
    const auto replaced = store.load_agents("single-default");
    expect_true(replaced.size() == 1,
                "history store replaces stale agent roster snapshots");

    std::filesystem::remove_all(root);
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

void test_external_vision_binding_round_trips_and_clears() {
    auto& hub = Corona::SharedDataHub::instance();

    auto actor = hub.actor_storage().allocate();
    Corona::ExternalVisionBindingDevice binding{};
    binding.source_path = "D:/vision/scene.json";
    binding.shape_guid = "shape-chair";
    binding.shape_index = 7;
    binding.json_path = "/scene/shapes/7";
    binding.shape_type = "model";
    binding.shape_identity_key = "guid:shape-chair";
    binding.model_path = "D:/vision/chair.obj";

    hub.set_external_vision_binding(actor, binding);
    expect_true(hub.has_external_vision_binding(actor),
                "external Vision binding is present after set");
    auto stored = hub.external_vision_binding(actor);
    expect_true(stored.has_value(), "external Vision binding can be read");
    expect_true(stored && stored->enabled, "external Vision binding is enabled");
    expect_true(stored && stored->source_path == binding.source_path,
                "external Vision binding source path round trips");
    expect_true(stored && stored->shape_guid == binding.shape_guid,
                "external Vision binding shape guid round trips");
    expect_true(stored && stored->shape_index == binding.shape_index,
                "external Vision binding shape index round trips");

    hub.clear_external_vision_binding(actor);
    expect_true(!hub.has_external_vision_binding(actor),
                "external Vision binding clears");
    expect_true(!hub.external_vision_binding(actor).has_value(),
                "cleared external Vision binding is absent");

    hub.actor_storage().deallocate(actor);
}

void test_actor_metadata_handle_cleanup_removes_guid_and_binding() {
    auto& hub = Corona::SharedDataHub::instance();

    auto actor = hub.actor_storage().allocate();
    hub.set_actor_guid(actor, "actor-cleanup");

    Corona::ExternalVisionBindingDevice binding{};
    binding.source_path = "D:/vision/scene.json";
    binding.shape_guid = "shape-cleanup";
    hub.set_external_vision_binding(actor, binding);

    hub.clear_actor_metadata(actor);
    expect_true(hub.actor_guid(actor).empty(),
                "actor metadata cleanup removes guid");
    expect_true(!hub.has_external_vision_binding(actor),
                "actor metadata cleanup removes external Vision binding");

    hub.actor_storage().deallocate(actor);
    auto reused = hub.actor_storage().allocate();
    expect_true(hub.actor_guid(reused).empty(),
                "fresh or reused actor handle has no stale guid");
    expect_true(!hub.has_external_vision_binding(reused),
                "fresh or reused actor handle has no stale external Vision binding");
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
    std::vector<uint8_t> outgoing;
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
        outgoing = packet;
    });
    sync.poll_and_sync();

    expect_true(!outgoing.empty(), "sync dirty packet exists");
    if (outgoing.empty()) {
        sync.shutdown();
        hub.actor_storage().deallocate(actor);
        hub.profile_storage().deallocate(profile);
        hub.geometry_storage().deallocate(geometry);
        hub.model_transform_storage().deallocate(transform);
        return;
    }

    bool found_actor_key = false;
    Corona::Network::BufferReader reader(outgoing.data(), outgoing.size());
    expect_true(static_cast<Corona::Network::MessageType>(reader.read_u8()) ==
                    Corona::Network::MessageType::SYNC_DIRTY,
                "sync dirty packet emitted");
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
            key == "actor:actor-guid-sync:xform") {
            found_actor_key = true;
        }
    }
    expect_true(found_actor_key, "sync dirty transform key carries actor guid");

    sync.shutdown();
    hub.actor_storage().deallocate(actor);
    hub.profile_storage().deallocate(profile);
    hub.geometry_storage().deallocate(geometry);
    hub.model_transform_storage().deallocate(transform);
}

void test_sync_engine_does_not_emit_geometry_resource_or_optics_entries() {
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
    std::vector<uint8_t> outgoing;
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
        outgoing = packet;
    });
    sync.poll_and_sync();

    expect_true(!outgoing.empty(), "transform-only sync dirty packet exists");
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
            reader.pos += key_len + value_len;
            expect_true(sid != Corona::Network::StorageID::ST_GEOMETRY,
                        "sync dirty does not emit geometry entries");
            expect_true(sid != Corona::Network::StorageID::ST_MODEL_RESOURCE,
                        "sync dirty does not emit model resource entries");
            expect_true(sid != Corona::Network::StorageID::ST_OPTICS,
                        "sync dirty does not emit optics entries");
        }
    }

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

}  // namespace

int main() {
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
    test_lanchat_message_carries_identity_and_sequence();
    test_lanchat_v2_message_carries_structured_metadata();
    test_lanchat_member_update_carries_full_snapshot();
    test_lanchat_history_snapshot_carries_full_history();
    test_lanchat_v2_history_snapshot_preserves_message_kind();
    test_lanchat_join_reject_carries_error_code();
    test_lanchat_state_deduplicates_messages_and_tracks_agents();
    test_lanchat_state_assigns_unique_duplicate_member_names();
    test_lanchat_state_updates_authoritative_message_and_history_snapshot();
    test_lanchat_state_queues_plain_chat_for_coordinator_sync();
    test_lanchat_state_queues_host_chat_for_coordinator_sync();
    test_lanchat_state_filters_internal_messages_from_coordinator_sync();
    test_lanchat_state_can_filter_generation_start_for_client_sync();
    test_lanchat_state_host_message_can_trigger_local_agent();
    test_lanchat_state_applies_authoritative_member_snapshot();
    test_lanchat_state_enqueues_local_agent_trigger_from_mention();
    test_lanchat_state_structured_target_overrides_text_mentions();
    test_lanchat_state_implicit_trigger_only_for_single_local_agent();
    test_lanchat_state_can_filter_generation_start_agent_trigger_for_client();
    test_lanchat_state_can_disable_agent_execution_for_client_policy();
    test_lanchat_state_deduplicates_agent_triggers();
    test_lanchat_state_does_not_trigger_agent_reply_or_duplicate_names();
    test_lanchat_state_does_not_trigger_structured_progress_messages();
    test_lanchat_state_enqueues_virtual_gm_from_mention_and_target();
    test_lanchat_state_tracks_locks_and_preview_conflicts();
    test_project_relative_path_validation();
    test_project_relative_path_uses_utf8_for_non_ascii_segments();
    test_network_system_session_role_defaults_to_none();
    test_network_system_repeated_start_preserves_active_role();
    test_lanchat_start_room_reuses_active_network_session_role();
    test_lanchat_history_persists_for_local_room();
    test_lanchat_history_uses_one_file_per_room_session();
    test_lanchat_history_store_lists_rooms_with_summaries();
    test_lanchat_history_store_persists_agent_roster();
    test_actor_device_follow_camera_defaults_false_and_round_trips();
    test_actor_metadata_guid_round_trips_and_clears();
    test_external_vision_binding_round_trips_and_clears();
    test_actor_metadata_handle_cleanup_removes_guid_and_binding();
    test_network_identity_registry_resolves_actor_components();
    test_network_identity_registry_tracks_local_ownership();
    test_network_identity_registry_applies_pending_ownership_override();
    test_sync_engine_marks_actor_dirty_entries_with_guid();
    test_sync_engine_does_not_emit_geometry_resource_or_optics_entries();
    test_sync_engine_does_not_emit_remote_owned_transform();
    test_sync_engine_does_not_emit_unregistered_transform_when_mapping_enabled();
    test_sync_engine_drops_unresolved_actor_transform();

    if (g_failed != 0) {
        std::cerr << g_failed << " network protocol test(s) failed\n";
        return 1;
    }
    return 0;
}
