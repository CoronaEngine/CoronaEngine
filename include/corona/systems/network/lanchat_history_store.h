#pragma once

#include <corona/systems/network/lanchat_state.h>

#include <filesystem>
#include <string>
#include <vector>

namespace Corona::Network {

struct LanChatHistoryRoomSummary {
    std::string session_id;
    std::string room_id;
    size_t message_count = 0;
    uint64_t last_timestamp_ms = 0;
    std::string last_sender_name;
    std::string last_text;
};

class LanChatHistoryStore {
public:
    explicit LanChatHistoryStore(std::filesystem::path root);

    [[nodiscard]] std::vector<LanChatHistoryRoomSummary> list_rooms() const;

    [[nodiscard]] std::vector<LanChatMessage> load_room(
        const std::string& room_id,
        size_t max_messages = 5000) const;

    void append_message(const std::string& room_id, const LanChatMessage& message) const;
    void save_agents(const std::string& room_id, const std::vector<LanChatAgent>& agents) const;

    [[nodiscard]] std::vector<LanChatAgent> load_agents(const std::string& room_id) const;

private:
    [[nodiscard]] std::filesystem::path room_file(const std::string& room_id) const;
    [[nodiscard]] std::filesystem::path agents_file(const std::string& room_id) const;
    [[nodiscard]] std::filesystem::path resolve_room_file(const std::string& room_or_session_id) const;
    [[nodiscard]] std::filesystem::path resolve_agents_file(const std::string& room_or_session_id) const;

    std::filesystem::path root_;
};

}  // namespace Corona::Network
