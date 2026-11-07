#pragma once

#include <corona/systems/network/lanchat_history_store.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <cstdint>
#include <string>
#include <vector>

namespace Corona::Systems::UI {

enum class LanChatMethod { StartRoom, StartLocalRoom, StopRoom, StopLocalRoom, JoinRoom,
    LeaveRoom, SendMessage, ListHistoryRooms, LoadHistoryRoom, GetLocalIp, AddAgent, RemoveAgent };
struct LanChatRequest {
    LanChatMethod method;
    std::string room, nickname, ip, text, name = "Agent", persona, agent_id;
    std::string message_kind = "chat", target_agent_id, source_user_id, correlation_id;
    std::uint16_t port = 27960;
    bool restore_history = false;
    std::optional<std::string> history_room;
    std::optional<nlohmann::json> metadata;
};
struct LanChatMessageSnapshot {
    std::string message_id, sender_id, room_id, from, text, sender_type, message_kind;
    std::string target_agent_id, source_user_id, correlation_id;
    std::uint64_t seq = 0, ts = 0;
    nlohmann::json metadata;
};
struct LanChatEventSnapshot {
    Corona::Network::LanChatEvent::Kind kind;
    LanChatMessageSnapshot message;
    std::vector<Corona::Network::LanChatMember> members;
    std::vector<LanChatMessageSnapshot> history;
    std::vector<Corona::Network::LanChatAgent> agents;
    std::string peer_id, room_id, code, error_message;
};
struct LanChatReply {
    LanChatMethod method = LanChatMethod::GetLocalIp;
    std::string error_code, error_message, error;
    bool ok = true, restored_history = false;
    std::string you, ip, room, mode, peer_id, agent_id, name, message_id;
    std::uint16_t port = 0;
    std::uint64_t seq = 0;
    std::vector<Corona::Network::LanChatMember> members;
    std::vector<Corona::Network::LanChatAgent> agents;
    std::vector<Corona::Network::LanChatHistoryRoomSummary> rooms;
    std::vector<LanChatMessageSnapshot> history;
};

LanChatReply execute_lanchat_operation(const LanChatRequest& request);

} // namespace Corona::Systems::UI
