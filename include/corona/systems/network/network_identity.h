#pragma once

#include <corona/shared_data_hub.h>
#include <corona/systems/network/protocol.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace Corona::Network {

struct ActorNetworkIdentity {
    std::string actor_guid;
    bool locally_owned = true;
    std::uintptr_t actor_handle = 0;
    std::uintptr_t profile_handle = 0;
    std::uintptr_t geometry_handle = 0;
    std::uintptr_t transform_handle = 0;
    std::uintptr_t model_resource_handle = 0;
    std::uintptr_t optics_handle = 0;
    std::uintptr_t mechanics_handle = 0;
    std::uintptr_t acoustics_handle = 0;
    std::int64_t actor_seq = -1;
    std::int64_t profile_seq = -1;
    std::int64_t geometry_seq = -1;
    std::int64_t transform_seq = -1;
    std::int64_t model_resource_seq = -1;
    std::int64_t optics_seq = -1;
    std::int64_t mechanics_seq = -1;
    std::int64_t acoustics_seq = -1;
};

/**
 * @brief GUID ↔ 存储句柄的映射表
 *
 * 线程安全：这张表同时被三类线程访问 ——
 *   - CEF UI 线程 / 引擎主线程：注册与所有权变更（编辑器创建/删除 actor、
 *     协作同步应用远端变更）；
 *   - 网络系统线程：每 tick 用 (storage_id, storage_seq) 反查 guid（sync engine），
 *     以及会话停止时的 clear()。
 * 因此 actors_ / ownership_overrides_ 的每一次访问都必须在 mutex_ 之下。此前没有任何
 * 同步：插入与全表扫描并发会在迭代中 rehash，clear() 与扫描并发更是直接的 UB。
 *
 * 锁序：mutex_ 之下**不得**再取 SharedDataHub 的槽位锁（build_identity() 在加锁前
 * 完成），所以本锁永远是最内层。
 */
class NetworkIdentityRegistry {
public:
    explicit NetworkIdentityRegistry(SharedDataHub& hub) : hub_(hub) {}

    bool register_actor(const std::string& actor_guid,
                        std::uintptr_t actor_handle,
                        bool locally_owned = true) {
        if (actor_guid.empty() || actor_handle == 0) return false;
        // 句柄快照在锁外采集：build_identity 会取若干槽位锁。
        auto identity = build_identity(actor_guid, actor_handle);
        if (!identity) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        auto override_it = ownership_overrides_.find(actor_guid);
        identity->locally_owned = override_it != ownership_overrides_.end()
            ? override_it->second
            : locally_owned;
        actors_[actor_guid] = *identity;
        return true;
    }

    std::optional<ActorNetworkIdentity> resolve_actor(
        const std::string& actor_guid) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = actors_.find(actor_guid);
        if (it == actors_.end()) return std::nullopt;
        return it->second;
    }

    std::string actor_guid_for_storage_seq(StorageID storage_id,
                                           std::uint64_t storage_seq) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [guid, identity] : actors_) {
            if (identity_seq(identity, storage_id) == static_cast<std::int64_t>(storage_seq)) {
                return guid;
            }
        }
        return {};
    }

    std::optional<bool> local_ownership_for_storage_seq(
        StorageID storage_id,
        std::uint64_t storage_seq) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [guid, identity] : actors_) {
            if (identity_seq(identity, storage_id) == static_cast<std::int64_t>(storage_seq)) {
                return identity.locally_owned;
            }
        }
        return std::nullopt;
    }

    std::optional<std::uint64_t> storage_seq_for_actor_guid(
        StorageID storage_id,
        const std::string& actor_guid) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = actors_.find(actor_guid);
        if (it == actors_.end()) return std::nullopt;
        auto seq = identity_seq(it->second, storage_id);
        if (seq < 0) return std::nullopt;
        return static_cast<std::uint64_t>(seq);
    }

    void unregister_actor(const std::string& actor_guid) {
        std::lock_guard<std::mutex> lock(mutex_);
        actors_.erase(actor_guid);
    }

    void set_actor_ownership(const std::string& actor_guid, bool locally_owned) {
        if (actor_guid.empty()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        ownership_overrides_[actor_guid] = locally_owned;
        auto it = actors_.find(actor_guid);
        if (it != actors_.end()) {
            it->second.locally_owned = locally_owned;
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        actors_.clear();
        ownership_overrides_.clear();
    }

private:
    std::optional<ActorNetworkIdentity> build_identity(
        const std::string& actor_guid,
        std::uintptr_t actor_handle) const {
        auto actor = hub_.actor_storage().try_acquire_read(actor_handle);
        if (!actor.valid() || actor->profile_handles.empty()) return std::nullopt;

        ActorNetworkIdentity out;
        out.actor_guid = actor_guid;
        out.actor_handle = actor_handle;
        out.actor_seq = hub_.actor_storage().seq_id(actor_handle);

        out.profile_handle = actor->profile_handles.front();
        out.profile_seq = hub_.profile_storage().seq_id(out.profile_handle);

        auto profile = hub_.profile_storage().try_acquire_read(out.profile_handle);
        if (!profile.valid()) return std::nullopt;

        out.geometry_handle = profile->geometry_handle;
        out.optics_handle = profile->optics_handle;
        out.mechanics_handle = profile->mechanics_handle;
        out.acoustics_handle = profile->acoustics_handle;

        if (out.geometry_handle == 0) {
            out.geometry_handle = geometry_from_component_handles(out);
        }

        if (out.geometry_handle != 0) {
            out.geometry_seq = hub_.geometry_storage().seq_id(out.geometry_handle);
            auto geometry = hub_.geometry_storage().try_acquire_read(out.geometry_handle);
            if (geometry.valid()) {
                out.transform_handle = geometry->transform_handle;
                out.model_resource_handle = geometry->model_resource_handle;
            }
        }

        if (out.transform_handle != 0) {
            out.transform_seq = hub_.model_transform_storage().seq_id(out.transform_handle);
        }
        if (out.model_resource_handle != 0) {
            out.model_resource_seq = hub_.model_resource_storage().seq_id(out.model_resource_handle);
        }
        if (out.optics_handle != 0) {
            out.optics_seq = hub_.optics_storage().seq_id(out.optics_handle);
        }
        if (out.mechanics_handle != 0) {
            out.mechanics_seq = hub_.mechanics_storage().seq_id(out.mechanics_handle);
        }
        if (out.acoustics_handle != 0) {
            out.acoustics_seq = hub_.acoustics_storage().seq_id(out.acoustics_handle);
        }

        return out;
    }

    std::uintptr_t geometry_from_component_handles(
        const ActorNetworkIdentity& identity) const {
        if (identity.optics_handle != 0) {
            auto optics = hub_.optics_storage().try_acquire_read(identity.optics_handle);
            if (optics.valid() && optics->geometry_handle != 0) {
                return optics->geometry_handle;
            }
        }
        if (identity.mechanics_handle != 0) {
            auto mechanics = hub_.mechanics_storage().try_acquire_read(identity.mechanics_handle);
            if (mechanics.valid() && mechanics->geometry_handle != 0) {
                return mechanics->geometry_handle;
            }
        }
        if (identity.acoustics_handle != 0) {
            auto acoustics = hub_.acoustics_storage().try_acquire_read(identity.acoustics_handle);
            if (acoustics.valid() && acoustics->geometry_handle != 0) {
                return acoustics->geometry_handle;
            }
        }
        return 0;
    }

    static std::int64_t identity_seq(const ActorNetworkIdentity& identity,
                                     StorageID storage_id) {
        switch (storage_id) {
        case StorageID::ST_ACTOR:
            return identity.actor_seq;
        case StorageID::ST_GEOMETRY:
            return identity.geometry_seq;
        case StorageID::ST_MODEL_TRANSFORM:
            return identity.transform_seq;
        case StorageID::ST_MODEL_RESOURCE:
            return identity.model_resource_seq;
        case StorageID::ST_OPTICS:
            return identity.optics_seq;
        case StorageID::ST_MECHANICS:
            return identity.mechanics_seq;
        case StorageID::ST_ACOUSTICS:
            return identity.acoustics_seq;
        default:
            return -1;
        }
    }

    SharedDataHub& hub_;
    /// 保护 actors_ 与 ownership_overrides_（见类注释的线程模型与锁序）。
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ActorNetworkIdentity> actors_;
    std::unordered_map<std::string, bool> ownership_overrides_;
};

}  // namespace Corona::Network
