#include <corona/systems/network/peer_liveness.h>

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

}  // namespace

int main() {
    test_receive_timeout_ignores_recent_heartbeat_send();
    test_disconnecting_peer_does_not_repeat_disconnect_request();
    return g_failed == 0 ? 0 : 1;
}
