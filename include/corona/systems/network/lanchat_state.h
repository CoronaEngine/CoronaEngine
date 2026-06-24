#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace Corona::Network {

struct LanChatResult {
    bool ok = false;
    std::string error;

    explicit operator bool() const { return ok; }
};

struct LanChatMessage {
    std::string message_id;
    std::string sender_id;
    std::string sender_name;
    std::string room_id;
    std::string text;
    uint64_t seq = 0;
    uint64_t timestamp_ms = 0;
    std::string sender_type = "user";     // user | agent | gm | system
    std::string message_kind = "chat";    // chat | agent_reply | progress | gm_proposal | confirmation | action_status | error
    std::string target_agent_id;
    std::string source_user_id;
    std::string correlation_id;
    std::string metadata_json;
};

struct LanChatMessageResult {
    bool accepted = false;
    LanChatMessage message;
    std::string error;
};

struct LanChatMember {
    std::string member_id;
    std::string nickname;
    std::string status = "online";
    uint64_t last_seen_ms = 0;
};

struct LanChatAgent {
    std::string agent_id;
    std::string name;
    std::string persona;
    std::string owner_id;
};

struct LanChatAgentTrigger {
    std::string trigger_id;
    std::string message_id;
    std::string room_id;
    std::string sender_id;
    std::string sender_name;
    std::string sender_type = "user";
    std::string message_kind = "chat";
    std::string target_agent_id;
    std::string source_user_id;
    std::string correlation_id;
    std::string metadata_json;
    std::string agent_id;
    std::string agent_name;
    std::string persona;
    std::string text;
    std::vector<LanChatMessage> history;
};

struct LanChatIntent {
    std::string user_id;
    std::string tooltip;
    std::array<float, 3> position{};
    std::string status;
    uint64_t timestamp_ms = 0;
};

class LanChatState {
public:
    bool open_room(const std::string& room_id,
                   const std::string& local_peer_id,
                   const std::string& host_name);
    void close_room();

    [[nodiscard]] const std::string& room_id() const { return room_id_; }
    [[nodiscard]] uint64_t next_seq() const { return next_seq_; }

    LanChatResult join_member(const std::string& member_id,
                              const std::string& nickname,
                              uint64_t now_ms = 0);
    LanChatResult leave_member(const std::string& member_id);
    void apply_member_snapshot(const std::vector<LanChatMember>& members);
    [[nodiscard]] const std::vector<LanChatMember>& members() const { return members_; }

    LanChatMessageResult record_message(const std::string& message_id,
                                        const std::string& sender_id,
                                        const std::string& sender_name,
                                        const std::string& text,
                                        uint64_t timestamp_ms);
    LanChatMessageResult record_message_ex(const std::string& message_id,
                                           const std::string& sender_id,
                                           const std::string& sender_name,
                                           const std::string& text,
                                           uint64_t timestamp_ms,
                                           const std::string& sender_type,
                                           const std::string& message_kind,
                                           const std::string& target_agent_id = {},
                                           const std::string& source_user_id = {},
                                           const std::string& correlation_id = {},
                                           const std::string& metadata_json = {});
    LanChatMessageResult apply_remote_message(const LanChatMessage& message);
    void apply_history_snapshot(const std::vector<LanChatMessage>& history);
    [[nodiscard]] const std::vector<LanChatMessage>& history() const { return history_; }

    LanChatResult register_agent(const std::string& agent_id,
                                 const std::string& name,
                                 const std::string& persona,
                                 const std::string& owner_id);
    LanChatResult remove_agent(const std::string& agent_id);
    [[nodiscard]] const std::vector<LanChatAgent>& agents() const { return agents_; }

    void enqueue_agent_triggers_for_message(const LanChatMessage& message,
                                            const std::string& local_peer_id,
                                            bool is_agent_reply = false,
                                            bool allow_agent_execution = true,
                                            bool allow_generation_start = true);
    [[nodiscard]] std::optional<LanChatAgentTrigger> pop_agent_trigger();
    [[nodiscard]] std::optional<LanChatMessage> pop_coordinator_sync_message(
        bool allow_generation_start = true);

    LanChatResult lock_object(const std::string& object_id,
                              const std::string& user_id,
                              const std::string& operation,
                              uint64_t now_ms);
    LanChatResult unlock_object(const std::string& object_id,
                                const std::string& user_id);
    [[nodiscard]] std::string locked_by(const std::string& object_id,
                                        uint64_t now_ms);

    void broadcast_intent(const std::string& user_id,
                          const std::string& tooltip,
                          const std::array<float, 3>& position,
                          const std::string& status,
                          uint64_t now_ms);
    [[nodiscard]] std::string check_preview_collision(
        const std::string& user_id,
        const std::array<float, 3>& position,
        float delta,
        uint64_t now_ms);

private:
    struct ObjectLock {
        std::string object_id;
        std::string user_id;
        std::string operation;
        uint64_t expires_at_ms = 0;
    };

    static constexpr uint64_t kLockTtlMs = 30000;
    static constexpr uint64_t kIntentTtlMs = 60000;

    [[nodiscard]] bool has_message_id(const std::string& message_id) const;
    void sort_history_and_advance_sequence();
    [[nodiscard]] std::vector<LanChatMember>::iterator find_member(const std::string& member_id);
    [[nodiscard]] std::vector<LanChatAgent>::iterator find_agent(const std::string& agent_id);
    [[nodiscard]] std::vector<ObjectLock>::iterator find_lock(const std::string& object_id);
    [[nodiscard]] std::vector<LanChatIntent>::iterator find_intent(const std::string& user_id);
    void enqueue_coordinator_sync_message(const LanChatMessage& message);

    std::string room_id_;
    uint64_t next_seq_ = 1;
    std::vector<LanChatMember> members_;
    std::vector<LanChatMessage> history_;
    std::vector<LanChatAgent> agents_;
    mutable std::mutex agent_trigger_mutex_;
    std::deque<LanChatAgentTrigger> pending_agent_triggers_;
    std::unordered_set<std::string> triggered_agent_keys_;
    mutable std::mutex coordinator_sync_mutex_;
    std::deque<LanChatMessage> pending_coordinator_sync_messages_;
    std::unordered_set<std::string> coordinator_sync_message_ids_;
    std::vector<ObjectLock> locks_;
    std::vector<LanChatIntent> intents_;
};

}  // namespace Corona::Network
