#pragma once

#include <corona/systems/network/protocol.h>

#include <cstdint>
#include <mutex>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Corona::Network {

enum class LwwApplyResult : uint8_t {
    Applied,
    Ignored,
    Invalid,
};

inline int compare_lww_version(const LwwVersion& lhs, const LwwVersion& rhs) {
    if (lhs.counter != rhs.counter) {
        return lhs.counter < rhs.counter ? -1 : 1;
    }
    if (lhs.writer_peer_id == rhs.writer_peer_id) return 0;
    return lhs.writer_peer_id < rhs.writer_peer_id ? -1 : 1;
}

inline bool operator==(const LwwVersion& lhs, const LwwVersion& rhs) {
    return lhs.counter == rhs.counter && lhs.writer_peer_id == rhs.writer_peer_id;
}

inline bool operator!=(const LwwVersion& lhs, const LwwVersion& rhs) {
    return !(lhs == rhs);
}

inline bool operator<(const LwwVersion& lhs, const LwwVersion& rhs) {
    return compare_lww_version(lhs, rhs) < 0;
}

class LwwState {
public:
    struct SnapshotEntry {
        EditorSyncOperation operation;
    };
    explicit LwwState(std::string local_peer_id)
        : local_peer_id_(std::move(local_peer_id)) {}

    LwwState(const LwwState&) = delete;
    LwwState& operator=(const LwwState&) = delete;

    void set_local_peer_id(std::string peer_id) {
        std::lock_guard lock(mutex_);
        local_peer_id_ = std::move(peer_id);
    }

    [[nodiscard]] LwwVersion next_local_version() {
        std::lock_guard lock(mutex_);
        return {++lamport_counter_, local_peer_id_};
    }

    void observe(const LwwVersion& version) {
        std::lock_guard lock(mutex_);
        if (version.counter > lamport_counter_) lamport_counter_ = version.counter;
    }

    LwwApplyResult apply_upsert(const std::string& actor_guid,
                                const std::string& field_name,
                                std::vector<uint8_t> value,
                                const LwwVersion& version) {
        if (actor_guid.empty() || field_name.empty() || version.writer_peer_id.empty()) {
            return LwwApplyResult::Invalid;
        }
        std::lock_guard lock(mutex_);
        if (version.counter > lamport_counter_) lamport_counter_ = version.counter;
        auto tombstone = tombstones_.find(actor_guid);
        if (tombstone != tombstones_.end() &&
            compare_lww_version(version, tombstone->second) <= 0) {
            return LwwApplyResult::Ignored;
        }
        const auto key = make_key(actor_guid, field_name);
        auto current = fields_.find(key);
        if (current != fields_.end() &&
            compare_lww_version(version, current->second.version) <= 0) {
            return LwwApplyResult::Ignored;
        }
        fields_[key] = FieldValue{std::move(value), version};
        return LwwApplyResult::Applied;
    }

    LwwApplyResult apply_delete(const std::string& actor_guid,
                                const LwwVersion& version) {
        if (actor_guid.empty() || version.writer_peer_id.empty()) {
            return LwwApplyResult::Invalid;
        }
        std::lock_guard lock(mutex_);
        if (version.counter > lamport_counter_) lamport_counter_ = version.counter;
        auto current = tombstones_.find(actor_guid);
        if (current != tombstones_.end() &&
            compare_lww_version(version, current->second) <= 0) {
            return LwwApplyResult::Ignored;
        }
        tombstones_[actor_guid] = version;
        return LwwApplyResult::Applied;
    }

    [[nodiscard]] std::vector<uint8_t> value(const std::string& actor_guid,
                                              const std::string& field_name) const {
        std::lock_guard lock(mutex_);
        auto it = fields_.find(make_key(actor_guid, field_name));
        return it == fields_.end() ? std::vector<uint8_t>{} : it->second.value;
    }

    [[nodiscard]] bool is_deleted(const std::string& actor_guid) const {
        std::lock_guard lock(mutex_);
        return tombstones_.find(actor_guid) != tombstones_.end();
    }

    [[nodiscard]] std::vector<EditorSyncOperation> snapshot() const {
        std::lock_guard lock(mutex_);
        std::vector<EditorSyncOperation> result;
        result.reserve(fields_.size() + tombstones_.size());
        for (const auto& [key, field] : fields_) {
            const auto sep = key.find('\x1f');
            if (sep == std::string::npos) continue;
            EditorSyncOperation op;
            op.kind = EditorSyncOperationKind::Upsert;
            op.actor_guid = key.substr(0, sep);
            op.field_name = key.substr(sep + 1);
            op.value = field.value;
            op.version = field.version;
            result.push_back(std::move(op));
        }
        for (const auto& [actor_guid, version] : tombstones_) {
            EditorSyncOperation op;
            op.kind = EditorSyncOperationKind::Delete;
            op.actor_guid = actor_guid;
            op.version = version;
            result.push_back(std::move(op));
        }
        std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.actor_guid != rhs.actor_guid) return lhs.actor_guid < rhs.actor_guid;
            if (lhs.kind != rhs.kind) {
                return lhs.kind == EditorSyncOperationKind::Delete;
            }
            return lhs.field_name < rhs.field_name;
        });
        return result;
    }

private:
    struct FieldValue {
        std::vector<uint8_t> value;
        LwwVersion version;
    };

    static std::string make_key(const std::string& actor_guid,
                                const std::string& field_name) {
        return actor_guid + '\x1f' + field_name;
    }

    std::string local_peer_id_;
    mutable std::mutex mutex_;
    uint64_t lamport_counter_ = 0;
    std::unordered_map<std::string, FieldValue> fields_;
    std::unordered_map<std::string, LwwVersion> tombstones_;
};

}  // namespace Corona::Network
