#pragma once

#include <corona/systems/network/protocol.h>

#include <cstdint>

namespace Corona::Network {

enum class PeerLivenessAction {
    None,
    SendHeartbeat,
    Disconnect,
};

struct PeerLivenessState {
    uint64_t last_receive_ms = 0;
    uint64_t last_heartbeat_sent_ms = 0;
    bool disconnecting = false;
};

inline PeerLivenessAction peer_liveness_action(const PeerLivenessState& state,
                                                uint64_t now_ms) {
    if (state.disconnecting) return PeerLivenessAction::None;
    if (state.last_receive_ms != 0 &&
        now_ms - state.last_receive_ms >= static_cast<uint64_t>(kPeerTimeoutMs)) {
        return PeerLivenessAction::Disconnect;
    }
    if (state.last_heartbeat_sent_ms == 0 ||
        now_ms - state.last_heartbeat_sent_ms >=
            static_cast<uint64_t>(kHeartbeatIntervalMs)) {
        return PeerLivenessAction::SendHeartbeat;
    }
    return PeerLivenessAction::None;
}

}  // namespace Corona::Network
