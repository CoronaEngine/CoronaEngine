#include <corona/systems/network/collaboration_state.h>
#include <algorithm>

namespace Corona::Network {

CollaborationResult CollaborationState::lock_object(const std::string& object_id,
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

CollaborationResult CollaborationState::unlock_object(const std::string& object_id,
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

std::string CollaborationState::locked_by(const std::string& object_id, uint64_t now_ms) {
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

void CollaborationState::broadcast_intent(const std::string& user_id,
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

std::string CollaborationState::check_preview_collision(const std::string& user_id,
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

std::vector<CollaborationState::ObjectLock>::iterator CollaborationState::find_lock(const std::string& object_id) {
    return std::find_if(locks_.begin(), locks_.end(), [&](const auto& lock) {
        return lock.object_id == object_id;
    });
}

std::vector<CollaborationIntent>::iterator CollaborationState::find_intent(const std::string& user_id) {
    return std::find_if(intents_.begin(), intents_.end(), [&](const auto& intent) {
        return intent.user_id == user_id;
    });
}


}  // namespace Corona::Network
