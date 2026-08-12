#pragma once

#include <corona/systems/network/lww_state.h>

#include <optional>
#include <vector>

namespace Corona::Network {

inline std::optional<EditorSyncOperation> make_actor_editor_upsert(
    LwwState& state, const std::string& actor_guid,
    const std::string& field_name, const std::vector<uint8_t>& legacy_packet,
    MessageType expected_type) {
    if (legacy_packet.empty() ||
        legacy_packet.front() != static_cast<uint8_t>(expected_type)) {
        return std::nullopt;
    }
    EditorSyncOperation operation;
    operation.kind = EditorSyncOperationKind::Upsert;
    operation.actor_guid = actor_guid;
    operation.field_name = field_name;
    operation.value.assign(legacy_packet.begin() + 1, legacy_packet.end());
    operation.version = state.next_local_version();
    if (state.apply_upsert(operation.actor_guid, operation.field_name,
                           operation.value, operation.version) !=
        LwwApplyResult::Applied) {
        return std::nullopt;
    }
    return operation;
}

inline std::vector<uint8_t> rebuild_actor_editor_packet(
    const EditorSyncOperation& operation, MessageType legacy_type) {
    if (operation.kind != EditorSyncOperationKind::Upsert ||
        operation.value.empty()) {
        return {};
    }
    std::vector<uint8_t> packet;
    packet.reserve(operation.value.size() + 1);
    packet.push_back(static_cast<uint8_t>(legacy_type));
    packet.insert(packet.end(), operation.value.begin(), operation.value.end());
    return packet;
}

}  // namespace Corona::Network
