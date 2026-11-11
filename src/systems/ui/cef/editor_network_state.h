#pragma once

// The network snapshot the editor reads: session and peer projections plus the local
// address the session advertises. Winsock lives behind this header - no caller has to
// know how the address is detected.

#include "cef_operations.h"

#include <vector>

namespace Corona::Systems::UI {

NetworkSessionSnapshot read_network_session();
std::vector<NetworkPeerSnapshot> read_network_peers();

}  // namespace Corona::Systems::UI
