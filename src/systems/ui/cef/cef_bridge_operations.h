#pragma once

#include <string>

namespace Corona::Systems::UI {

// The single Web -> Python entry point.
//
// C++ owns transport only: it hands the envelope to the Python service and
// returns whatever that service produced.  The request schema, the argument
// validation and the service allowlist all live on the Python side, so the
// editor exposes exactly one function here and stays free of business branches.
struct BridgeReply {
    std::string json;
    std::string error_code;
    std::string error_message;
};

// Invoked on the CEF UI thread only.
BridgeReply execute_bridge_operation(const std::string& envelope);

}  // namespace Corona::Systems::UI
