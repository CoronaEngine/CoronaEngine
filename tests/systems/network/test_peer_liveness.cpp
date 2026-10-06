#include <corona/systems/network/peer_liveness.h>
#include <corona/systems/network/peer_list_utils.h>

#include <cstdint>

#include <iostream>

namespace {

int g_failed = 0;

void expect_true(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message;
        ++g_failed;
    }
}

void test_receive_timeout_ignores_recent_heartbeat_send() {
    Corona::Network::PeerLivenessState state;
    state.last_receive_ms = 1000;
    state.last_heartbeat_sent_ms = 3500;

    expect_true(Corona::Network::peer_liveness_action(state, 4000) ==
                    Corona::Network::PeerLivenessAction::Disconnect,
                __func__);
}

void test_disconnecting_peer_does_not_repeat_disconnect_request() {
    Corona::Network::PeerLivenessState state;
    state.last_receive_ms = 1000;
    state.disconnecting = true;

    expect_true(Corona::Network::peer_liveness_action(state, 5000) ==
                    Corona::Network::PeerLivenessAction::None,
                __func__);
}

void test_erasing_duplicate_refinds_retained_peer() {
    using PeerInfo = Corona::Network::PeerManager::PeerInfo;
    auto* discarded = reinterpret_cast<_ENetPeer*>(static_cast<uintptr_t>(1));
    auto* retained = reinterpret_cast<_ENetPeer*>(static_cast<uintptr_t>(2));
    std::vector<PeerInfo> peers(2);
    peers[0].peer = discarded;
    peers[1].peer = retained;
    peers[1].outbound = true;

    auto* retained_info = Corona::Network::erase_peer_and_refind(
        peers, discarded, retained);

    expect_true(peers.size() == 1 && retained_info != nullptr &&
                    retained_info->peer == retained && retained_info->outbound,
                __func__);
}

}  // namespace

int main() {
    test_receive_timeout_ignores_recent_heartbeat_send();
    test_disconnecting_peer_does_not_repeat_disconnect_request();
    test_erasing_duplicate_refinds_retained_peer();
    return g_failed == 0 ? 0 : 1;
}
