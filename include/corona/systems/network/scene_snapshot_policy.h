#pragma once

#include <string_view>

namespace Corona::Network {

enum class SceneSnapshotDisposition {
    PeerAcknowledgement,
    DiagnosticOnly,
};

inline SceneSnapshotDisposition classify_scene_snapshot(
    std::string_view snapshot_kind) {
    return snapshot_kind == "peer_ack"
        ? SceneSnapshotDisposition::PeerAcknowledgement
        : SceneSnapshotDisposition::DiagnosticOnly;
}

}  // namespace Corona::Network
