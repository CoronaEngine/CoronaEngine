#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Corona::Network {
struct CollaborationResult {
    bool ok = false;
    std::string error;
};

struct CollaborationIntent {
    std::string user_id, tooltip;
    std::array<float, 3> position{};
    std::string status;
    uint64_t timestamp_ms = 0;
};

// Local advisory locks and placement previews. No chat or transport ownership.
class CollaborationState {
public:
    CollaborationResult lock_object(const std::string& object_id, const std::string& user_id,
                                    const std::string& operation, uint64_t now_ms);
    CollaborationResult unlock_object(const std::string& object_id, const std::string& user_id);
    std::string locked_by(const std::string& object_id, uint64_t now_ms);
    void broadcast_intent(const std::string& user_id, const std::string& tooltip,
                          const std::array<float, 3>& position, const std::string& status, uint64_t now_ms);
    std::string check_preview_collision(const std::string& user_id, const std::array<float, 3>& position,
                                        float delta, uint64_t now_ms);
    void clear() { locks_.clear(); intents_.clear(); }

private:
    struct ObjectLock {
        std::string object_id, user_id, operation;
        uint64_t expires_at_ms = 0;
    };
    static constexpr uint64_t kLockTtlMs = 30000;
    static constexpr uint64_t kIntentTtlMs = 60000;
    std::vector<ObjectLock>::iterator find_lock(const std::string& object_id);
    std::vector<CollaborationIntent>::iterator find_intent(const std::string& user_id);
    std::vector<ObjectLock> locks_;
    std::vector<CollaborationIntent> intents_;
};
}  // namespace Corona::Network
