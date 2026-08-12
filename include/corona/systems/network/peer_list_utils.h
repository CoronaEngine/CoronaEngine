#pragma once

#include <corona/systems/network/peer_manager.h>

#include <algorithm>
#include <vector>

namespace Corona::Network {

inline PeerManager::PeerInfo* erase_peer_and_refind(
    std::vector<PeerManager::PeerInfo>& peers,
    _ENetPeer* discarded_peer, _ENetPeer* retained_peer) {
    peers.erase(
        std::remove_if(peers.begin(), peers.end(),
            [discarded_peer](const PeerManager::PeerInfo& info) {
                return info.peer == discarded_peer;
            }),
        peers.end());
    const auto retained = std::find_if(
        peers.begin(), peers.end(),
        [retained_peer](const PeerManager::PeerInfo& info) {
            return info.peer == retained_peer;
        });
    return retained == peers.end() ? nullptr : &*retained;
}

}  // namespace Corona::Network
