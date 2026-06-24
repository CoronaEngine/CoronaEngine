#include <corona/systems/network/lanchat_state.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>

namespace Corona::Network {
namespace {

constexpr size_t kAgentTriggerHistoryLimit = 20;

bool is_mention_boundary(char ch) {
    const auto uch = static_cast<unsigned char>(ch);
    if (uch >= 0x80) {
        return true;
    }
    if (std::isspace(uch) != 0) {
        return true;
    }
    switch (ch) {
    case '.':
    case ',':
    case ';':
    case ':':
    case '!':
    case '?':
    case ')':
    case ']':
    case '}':
    case '"':
    case '\'':
        return true;
    default:
        return false;
    }
}

bool mentions_agent(const std::string& text, const std::string& agent_name) {
    if (agent_name.empty()) {
        return false;
    }

    const std::string needle = "@" + agent_name;
    size_t pos = text.find(needle);
    while (pos != std::string::npos) {
        const size_t end = pos + needle.size();
        if (end >= text.size() || is_mention_boundary(text[end])) {
            return true;
        }
        pos = text.find(needle, pos + 1);
    }
    return false;
}

bool nickname_is_used_by_other_member(const std::vector<LanChatMember>& members,
                                      const std::string& member_id,
                                      const std::string& nickname) {
    return std::any_of(members.begin(), members.end(), [&](const auto& member) {
        return member.member_id != member_id && member.nickname == nickname;
    });
}

std::string unique_member_nickname(const std::vector<LanChatMember>& members,
                                   const std::string& member_id,
                                   const std::string& requested_nickname) {
    const std::string base = requested_nickname.empty() ? "Guest" : requested_nickname;
    if (!nickname_is_used_by_other_member(members, member_id, base)) {
        return base;
    }

    for (uint32_t suffix = 1; suffix < 10000; ++suffix) {
        const std::string candidate = base + "_" + std::to_string(suffix);
        if (!nickname_is_used_by_other_member(members, member_id, candidate)) {
            return candidate;
        }
    }
    return base + "_" + std::to_string(members.size());
}

bool is_gm_target(const LanChatMessage& message) {
    return message.target_agent_id == "gm" ||
           message.target_agent_id == "GM" ||
           mentions_agent(message.text, "GM");
}

bool contains_any(const std::string& text, std::initializer_list<const char*> needles) {
    for (const char* needle : needles) {
        if (needle != nullptr && text.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool is_generation_start_for_coordinator(const LanChatMessage& message) {
    const std::string message_kind = message.message_kind.empty() ? "chat" : message.message_kind;
    const std::string sender_type = message.sender_type.empty() ? "user" : message.sender_type;
    if (message_kind != "chat" || (sender_type != "user" && sender_type != "host")) {
        return false;
    }
    if (contains_any(message.metadata_json, {
            "\"draft_action\":\"generate\"",
            "\"draft_action\": \"generate\"",
            "\"action_type\":\"start_generation\"",
            "\"action_type\": \"start_generation\"",
        })) {
        return true;
    }
    return contains_any(message.text, {
        "确认开始",
        "直接生成",
        "开始生成",
        "开始执行",
        "执行生成",
        "按照方案执行生成",
        "按方案执行生成",
        "就按方案生成",
        "按这个方案生成",
        "按照方案生成",
        "开始搭建",
        "开始布置",
    });
}

void enqueue_virtual_gm_trigger(LanChatState& state,
                                const LanChatMessage& message,
                                std::deque<LanChatAgentTrigger>& pending,
                                std::unordered_set<std::string>& triggered_keys,
                                std::mutex& mutex) {
    LanChatAgentTrigger trigger;
    trigger.trigger_id = message.message_id + ":gm";
    trigger.message_id = message.message_id;
    trigger.room_id = message.room_id;
    trigger.sender_id = message.sender_id;
    trigger.sender_name = message.sender_name;
    trigger.sender_type = message.sender_type;
    trigger.message_kind = message.message_kind;
    trigger.target_agent_id = message.target_agent_id.empty() ? "gm" : message.target_agent_id;
    trigger.source_user_id = message.source_user_id;
    trigger.correlation_id = message.correlation_id;
    trigger.metadata_json = message.metadata_json;
    trigger.agent_id = "gm";
    trigger.agent_name = "GM";
    trigger.persona = "GM";
    trigger.text = message.text;

    const auto& history = state.history();
    const size_t start = history.size() > kAgentTriggerHistoryLimit
        ? history.size() - kAgentTriggerHistoryLimit
        : 0;
    trigger.history.assign(history.begin() + static_cast<std::ptrdiff_t>(start),
                           history.end());

    std::lock_guard<std::mutex> lock(mutex);
    if (!triggered_keys.insert(trigger.trigger_id).second) {
        return;
    }
    pending.push_back(std::move(trigger));
}

}  // namespace

bool LanChatState::open_room(const std::string& room_id,
                             const std::string& local_peer_id,
                             const std::string& host_name) {
    close_room();
    room_id_ = room_id;
    next_seq_ = 1;
    return join_member(local_peer_id, host_name).ok;
}

void LanChatState::close_room() {
    room_id_.clear();
    next_seq_ = 1;
    members_.clear();
    history_.clear();
    agents_.clear();
    {
        std::lock_guard<std::mutex> lock(agent_trigger_mutex_);
        pending_agent_triggers_.clear();
        triggered_agent_keys_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(coordinator_sync_mutex_);
        pending_coordinator_sync_messages_.clear();
        coordinator_sync_message_ids_.clear();
    }
    locks_.clear();
    intents_.clear();
}

LanChatResult LanChatState::join_member(const std::string& member_id,
                                        const std::string& nickname,
                                        uint64_t now_ms) {
    if (member_id.empty()) {
        return {false, "member_id is required"};
    }

    const std::string final_nickname = unique_member_nickname(members_, member_id, nickname);
    auto it = find_member(member_id);
    if (it == members_.end()) {
        members_.push_back({member_id, final_nickname, "online", now_ms});
    } else {
        it->nickname = final_nickname;
        it->status = "online";
        it->last_seen_ms = now_ms;
    }
    return {true, {}};
}

LanChatResult LanChatState::leave_member(const std::string& member_id) {
    auto it = find_member(member_id);
    if (it == members_.end()) {
        return {false, "member not found"};
    }
    members_.erase(it);
    return {true, {}};
}

void LanChatState::apply_member_snapshot(const std::vector<LanChatMember>& members) {
    members_ = members;
}

LanChatMessageResult LanChatState::record_message(const std::string& message_id,
                                                  const std::string& sender_id,
                                                  const std::string& sender_name,
                                                  const std::string& text,
                                                  uint64_t timestamp_ms) {
    return record_message_ex(
        message_id, sender_id, sender_name, text, timestamp_ms,
        "user", "chat");
}

LanChatMessageResult LanChatState::record_message_ex(const std::string& message_id,
                                                     const std::string& sender_id,
                                                     const std::string& sender_name,
                                                     const std::string& text,
                                                     uint64_t timestamp_ms,
                                                     const std::string& sender_type,
                                                     const std::string& message_kind,
                                                     const std::string& target_agent_id,
                                                     const std::string& source_user_id,
                                                     const std::string& correlation_id,
                                                     const std::string& metadata_json) {
    if (message_id.empty()) {
        return {false, {}, "message_id is required"};
    }
    if (has_message_id(message_id)) {
        return {false, {}, "duplicate message_id"};
    }

    LanChatMessage message;
    message.message_id = message_id;
    message.sender_id = sender_id;
    message.sender_name = sender_name;
    message.room_id = room_id_;
    message.text = text;
    message.seq = next_seq_++;
    message.timestamp_ms = timestamp_ms;
    message.sender_type = sender_type.empty() ? "user" : sender_type;
    message.message_kind = message_kind.empty() ? "chat" : message_kind;
    message.target_agent_id = target_agent_id;
    message.source_user_id = source_user_id;
    message.correlation_id = correlation_id;
    message.metadata_json = metadata_json;

    history_.push_back(message);
    enqueue_coordinator_sync_message(message);
    return {true, message, {}};
}

LanChatMessageResult LanChatState::apply_remote_message(const LanChatMessage& message) {
    if (message.message_id.empty()) {
        return {false, {}, "message_id is required"};
    }
    auto existing = std::find_if(history_.begin(), history_.end(), [&](const auto& item) {
        return item.message_id == message.message_id;
    });
    if (existing != history_.end()) {
        const bool same_payload =
            existing->sender_id == message.sender_id &&
            existing->sender_name == message.sender_name &&
            existing->room_id == message.room_id &&
            existing->text == message.text &&
            existing->seq == message.seq &&
            existing->timestamp_ms == message.timestamp_ms &&
            existing->sender_type == message.sender_type &&
            existing->message_kind == message.message_kind &&
            existing->target_agent_id == message.target_agent_id &&
            existing->source_user_id == message.source_user_id &&
            existing->correlation_id == message.correlation_id &&
            existing->metadata_json == message.metadata_json;
        if (same_payload) {
            return {false, *existing, "duplicate message_id"};
        }
        if (existing->seq != 0 && message.seq != 0 && message.seq < existing->seq) {
            return {false, *existing, "stale message_id"};
        }
        *existing = message;
        sort_history_and_advance_sequence();
        enqueue_coordinator_sync_message(*existing);
        return {true, *existing, {}};
    }

    history_.push_back(message);
    sort_history_and_advance_sequence();
    enqueue_coordinator_sync_message(message);
    return {true, message, {}};
}

void LanChatState::apply_history_snapshot(const std::vector<LanChatMessage>& history) {
    history_ = history;
    sort_history_and_advance_sequence();
}

LanChatResult LanChatState::register_agent(const std::string& agent_id,
                                           const std::string& name,
                                           const std::string& persona,
                                           const std::string& owner_id) {
    if (agent_id.empty()) {
        return {false, "agent_id is required"};
    }

    auto it = find_agent(agent_id);
    auto duplicate_name = std::find_if(agents_.begin(), agents_.end(), [&](const auto& agent) {
        return agent.agent_id != agent_id && agent.name == name;
    });
    if (duplicate_name != agents_.end()) {
        return {false, "agent name already exists"};
    }

    if (it == agents_.end()) {
        agents_.push_back({agent_id, name, persona, owner_id});
    } else {
        it->name = name;
        it->persona = persona;
        it->owner_id = owner_id;
    }
    return {true, {}};
}

void LanChatState::enqueue_agent_triggers_for_message(const LanChatMessage& message,
                                                      const std::string& local_peer_id,
                                                      bool is_agent_reply,
                                                      bool allow_agent_execution,
                                                      bool allow_generation_start) {
    if (is_agent_reply || message.message_id.empty() || local_peer_id.empty()) {
        return;
    }
    const bool is_confirmation = message.message_kind == "confirmation";
    if (!message.message_kind.empty() && message.message_kind != "chat" && !is_confirmation) {
        return;
    }
    if (!message.sender_type.empty() &&
        message.sender_type != "user" &&
        message.sender_type != "host") {
        return;
    }
    if (!allow_agent_execution) {
        return;
    }
    if (!allow_generation_start && is_generation_start_for_coordinator(message)) {
        return;
    }

    if (is_gm_target(message)) {
        enqueue_virtual_gm_trigger(*this, message, pending_agent_triggers_,
                                   triggered_agent_keys_, agent_trigger_mutex_);
        return;
    }

    std::vector<const LanChatAgent*> local_agents;
    local_agents.reserve(agents_.size());
    for (const auto& agent : agents_) {
        if (agent.owner_id != local_peer_id || agent.agent_id == message.sender_id) {
            continue;
        }
        local_agents.push_back(&agent);
    }

    auto enqueue_local_agent = [&](const LanChatAgent& agent) {
        LanChatAgentTrigger trigger;
        trigger.trigger_id = message.message_id + ":" + agent.agent_id;
        trigger.message_id = message.message_id;
        trigger.room_id = message.room_id;
        trigger.sender_id = message.sender_id;
        trigger.sender_name = message.sender_name;
        trigger.sender_type = message.sender_type;
        trigger.message_kind = message.message_kind;
        trigger.target_agent_id = message.target_agent_id;
        trigger.source_user_id = message.source_user_id;
        trigger.correlation_id = message.correlation_id;
        trigger.metadata_json = message.metadata_json;
        trigger.agent_id = agent.agent_id;
        trigger.agent_name = agent.name;
        trigger.persona = agent.persona;
        trigger.text = message.text;

        const size_t start = history_.size() > kAgentTriggerHistoryLimit
            ? history_.size() - kAgentTriggerHistoryLimit
            : 0;
        trigger.history.assign(history_.begin() + static_cast<std::ptrdiff_t>(start),
                               history_.end());

        std::lock_guard<std::mutex> lock(agent_trigger_mutex_);
        if (!triggered_agent_keys_.insert(trigger.trigger_id).second) {
            return;
        }
        pending_agent_triggers_.push_back(std::move(trigger));
    };

    if (!message.target_agent_id.empty()) {
        for (const auto* agent : local_agents) {
            if (agent->agent_id == message.target_agent_id ||
                agent->name == message.target_agent_id) {
                enqueue_local_agent(*agent);
                return;
            }
        }
        return;
    }

    bool mentioned_any_local_agent = false;
    for (const auto* agent : local_agents) {
        if (mentions_agent(message.text, agent->name)) {
            mentioned_any_local_agent = true;
            break;
        }
    }

    for (const auto* agent_ptr : local_agents) {
        const auto& agent = *agent_ptr;
        const bool mentioned = mentions_agent(message.text, agent.name);
        const bool implicit_single_agent =
            !mentioned_any_local_agent && local_agents.size() == 1;
        if (!mentioned && !implicit_single_agent) {
            continue;
        }

        enqueue_local_agent(agent);
    }
}

std::optional<LanChatAgentTrigger> LanChatState::pop_agent_trigger() {
    std::lock_guard<std::mutex> lock(agent_trigger_mutex_);
    if (pending_agent_triggers_.empty()) {
        return std::nullopt;
    }
    auto trigger = std::move(pending_agent_triggers_.front());
    pending_agent_triggers_.pop_front();
    return trigger;
}

std::optional<LanChatMessage> LanChatState::pop_coordinator_sync_message(
    bool allow_generation_start) {
    std::lock_guard<std::mutex> lock(coordinator_sync_mutex_);
    while (!pending_coordinator_sync_messages_.empty()) {
        auto message = std::move(pending_coordinator_sync_messages_.front());
        pending_coordinator_sync_messages_.pop_front();
        if (!allow_generation_start && is_generation_start_for_coordinator(message)) {
            continue;
        }
        return message;
    }
    return std::nullopt;
}

void LanChatState::enqueue_coordinator_sync_message(const LanChatMessage& message) {
    if (message.message_id.empty()) {
        return;
    }
    const std::string message_kind = message.message_kind.empty() ? "chat" : message.message_kind;
    const std::string sender_type = message.sender_type.empty() ? "user" : message.sender_type;
    if (message_kind != "chat") {
        return;
    }
    if (sender_type != "user" && sender_type != "host") {
        return;
    }

    std::lock_guard<std::mutex> lock(coordinator_sync_mutex_);
    if (!coordinator_sync_message_ids_.insert(message.message_id).second) {
        return;
    }
    pending_coordinator_sync_messages_.push_back(message);
}

LanChatResult LanChatState::remove_agent(const std::string& agent_id) {
    auto it = find_agent(agent_id);
    if (it == agents_.end()) {
        return {false, "agent not found"};
    }
    agents_.erase(it);
    return {true, {}};
}

LanChatResult LanChatState::lock_object(const std::string& object_id,
                                        const std::string& user_id,
                                        const std::string& operation,
                                        uint64_t now_ms) {
    if (object_id.empty() || user_id.empty()) {
        return {false, "object_id and user_id are required"};
    }

    auto it = find_lock(object_id);
    if (it != locks_.end() && it->expires_at_ms > now_ms && it->user_id != user_id) {
        return {false, "object already locked"};
    }
    if (it == locks_.end()) {
        locks_.push_back({object_id, user_id, operation, now_ms + kLockTtlMs});
    } else {
        it->user_id = user_id;
        it->operation = operation;
        it->expires_at_ms = now_ms + kLockTtlMs;
    }
    return {true, {}};
}

LanChatResult LanChatState::unlock_object(const std::string& object_id,
                                          const std::string& user_id) {
    auto it = find_lock(object_id);
    if (it == locks_.end()) {
        return {false, "object is not locked"};
    }
    if (it->user_id != user_id) {
        return {false, "lock owned by another user"};
    }
    locks_.erase(it);
    return {true, {}};
}

std::string LanChatState::locked_by(const std::string& object_id, uint64_t now_ms) {
    auto it = find_lock(object_id);
    if (it == locks_.end()) {
        return {};
    }
    if (it->expires_at_ms <= now_ms) {
        locks_.erase(it);
        return {};
    }
    return it->user_id;
}

void LanChatState::broadcast_intent(const std::string& user_id,
                                    const std::string& tooltip,
                                    const std::array<float, 3>& position,
                                    const std::string& status,
                                    uint64_t now_ms) {
    auto it = find_intent(user_id);
    if (it == intents_.end()) {
        intents_.push_back({user_id, tooltip, position, status, now_ms});
    } else {
        it->tooltip = tooltip;
        it->position = position;
        it->status = status;
        it->timestamp_ms = now_ms;
    }
}

std::string LanChatState::check_preview_collision(const std::string& user_id,
                                                  const std::array<float, 3>& position,
                                                  float delta,
                                                  uint64_t now_ms) {
    intents_.erase(std::remove_if(intents_.begin(), intents_.end(), [now_ms](const auto& intent) {
                       return intent.timestamp_ms + kIntentTtlMs <= now_ms;
                   }),
                   intents_.end());

    const float max_distance_sq = delta * delta;
    for (const auto& intent : intents_) {
        if (intent.user_id == user_id) {
            continue;
        }
        const float dx = intent.position[0] - position[0];
        const float dz = intent.position[2] - position[2];
        if (dx * dx + dz * dz <= max_distance_sq) {
            return intent.user_id;
        }
    }
    return {};
}

bool LanChatState::has_message_id(const std::string& message_id) const {
    return std::any_of(history_.begin(), history_.end(), [&](const auto& message) {
        return message.message_id == message_id;
    });
}

void LanChatState::sort_history_and_advance_sequence() {
    std::sort(history_.begin(), history_.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.seq != rhs.seq) {
            return lhs.seq < rhs.seq;
        }
        return lhs.message_id < rhs.message_id;
    });
    uint64_t max_seq = 0;
    for (const auto& message : history_) {
        max_seq = std::max(max_seq, message.seq);
    }
    next_seq_ = std::max(next_seq_, max_seq + 1);
}

std::vector<LanChatMember>::iterator LanChatState::find_member(const std::string& member_id) {
    return std::find_if(members_.begin(), members_.end(), [&](const auto& member) {
        return member.member_id == member_id;
    });
}

std::vector<LanChatAgent>::iterator LanChatState::find_agent(const std::string& agent_id) {
    return std::find_if(agents_.begin(), agents_.end(), [&](const auto& agent) {
        return agent.agent_id == agent_id;
    });
}

std::vector<LanChatState::ObjectLock>::iterator LanChatState::find_lock(const std::string& object_id) {
    return std::find_if(locks_.begin(), locks_.end(), [&](const auto& lock) {
        return lock.object_id == object_id;
    });
}

std::vector<LanChatIntent>::iterator LanChatState::find_intent(const std::string& user_id) {
    return std::find_if(intents_.begin(), intents_.end(), [&](const auto& intent) {
        return intent.user_id == user_id;
    });
}

}  // namespace Corona::Network
