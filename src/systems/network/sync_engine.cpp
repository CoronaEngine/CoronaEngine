#include <corona/systems/network/sync_engine.h>

#include <corona/shared_data_hub.h>
#include <corona/kernel/core/i_logger.h>

#include <chrono>
#include <cstring>
#include <mutex>

namespace Corona::Network {

// ============================================================================
// Per-type serialize / deserialize / hash helpers
// ============================================================================

namespace {

// ---- ModelTransform helpers ----
std::string hash_mt(const ModelTransform& t, uint64_t entity_seq) {
    uint64_t h = entity_seq;
    auto mix = [&](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) h = h * 31 + b[i];
    };
    mix(&t.position, sizeof(t.position));
    mix(&t.euler_rotation, sizeof(t.euler_rotation));
    mix(&t.scale, sizeof(t.scale));

    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return {buf};
}

void serialize_mt(const ModelTransform& t, uint64_t entity_seq,
                  const std::string& key,
                  std::vector<uint8_t>& entries) {
    float xform[9];
    xform[0] = t.position.x; xform[1] = t.position.y; xform[2] = t.position.z;
    xform[3] = t.euler_rotation.x; xform[4] = t.euler_rotation.y; xform[5] = t.euler_rotation.z;
    xform[6] = t.scale.x; xform[7] = t.scale.y; xform[8] = t.scale.z;

    auto entry = build_dirty_entries(StorageID::ST_MODEL_TRANSFORM, entity_seq,
                                     key.c_str(), static_cast<uint16_t>(key.size()),
                                     xform, sizeof(xform));
    entries.insert(entries.end(), entry.begin(), entry.end());
}

void deserialize_mt(ModelTransform& t, const uint8_t* value, uint16_t value_len) {
    if (value_len != 36) return;
    const auto* f = reinterpret_cast<const float*>(value);
    t.position.x = f[0]; t.position.y = f[1]; t.position.z = f[2];
    t.euler_rotation.x = f[3]; t.euler_rotation.y = f[4]; t.euler_rotation.z = f[5];
    t.scale.x = f[6]; t.scale.y = f[7]; t.scale.z = f[8];
}

// ---- ModelResource helpers ----
std::string hash_mr(const ModelResource& r, uint64_t entity_seq) {
    uint64_t h = entity_seq;
    h = h * 31 + r.model_id;
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return {buf};
}

void serialize_mr(const ModelResource& r, uint64_t entity_seq,
                  const std::string& key,
                  std::vector<uint8_t>& entries) {
    auto entry = build_dirty_entries(StorageID::ST_MODEL_RESOURCE, entity_seq,
                                     key.c_str(), static_cast<uint16_t>(key.size()),
                                     &r.model_id, sizeof(r.model_id));
    entries.insert(entries.end(), entry.begin(), entry.end());
}

void deserialize_mr(ModelResource& r, const uint8_t* value, uint16_t value_len) {
    if (value_len != 8) return;
    r.model_id = *reinterpret_cast<const uint64_t*>(value);
}

// ---- GeometryDevice helpers ----
// Serialize only pointer references (transform_handle, model_resource_handle).
// GPU data (mesh_handles with HardwareBuffer/Image) is created locally via
// the same deterministic code path on every peer and must NOT be synced.
std::string hash_geo(const GeometryDevice& g, uint64_t entity_seq) {
    uint64_t h = entity_seq;
    h = h * 31 + g.transform_handle;
    h = h * 31 + g.model_resource_handle;
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return {buf};
}

void serialize_geo(const GeometryDevice& g, uint64_t entity_seq,
                   const std::string& key,
                   std::vector<uint8_t>& entries) {
    uint64_t data[2];
    data[0] = static_cast<uint64_t>(g.transform_handle);
    data[1] = static_cast<uint64_t>(g.model_resource_handle);
    auto entry = build_dirty_entries(StorageID::ST_GEOMETRY, entity_seq,
                                     key.c_str(), static_cast<uint16_t>(key.size()),
                                     data, sizeof(data));
    entries.insert(entries.end(), entry.begin(), entry.end());
}

void deserialize_geo(GeometryDevice& g, const uint8_t* value, uint16_t value_len) {
    if (value_len != 16) return;
    const auto* d = reinterpret_cast<const uint64_t*>(value);
    // Only update if handles are zero (unlinked). Once linked they are stable.
    if (g.transform_handle == 0) g.transform_handle = static_cast<std::uintptr_t>(d[0]);
    if (g.model_resource_handle == 0) g.model_resource_handle = static_cast<std::uintptr_t>(d[1]);
}

// ---- OpticsDevice helpers ----
struct OpticsPacked {
    bool visible;
    bool bEnableLighting;
    float metallic;
    float roughness;
    float subsurface;
    float specular;
    float specularTint;
    float anisotropic;
    float sheen;
    float sheenTint;
    float clearcoat;
    float clearcoatGloss;
    float ambient[3];
    float diffuse[3];
    float specular_color[3];
    float shininess;
};

std::string hash_opt(const OpticsDevice& o, uint64_t entity_seq) {
    uint64_t h = entity_seq;
    auto mix = [&](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) h = h * 31 + b[i];
    };
    mix(&o.visible, sizeof(o.visible));
    mix(&o.bEnableLighting, sizeof(o.bEnableLighting));
    mix(&o.metallic, sizeof(o.metallic));
    mix(&o.roughness, sizeof(o.roughness));
    mix(&o.subsurface, sizeof(o.subsurface));
    mix(&o.specular, sizeof(o.specular));
    mix(&o.specularTint, sizeof(o.specularTint));
    mix(&o.anisotropic, sizeof(o.anisotropic));
    mix(&o.sheen, sizeof(o.sheen));
    mix(&o.sheenTint, sizeof(o.sheenTint));
    mix(&o.clearcoat, sizeof(o.clearcoat));
    mix(&o.clearcoatGloss, sizeof(o.clearcoatGloss));
    mix(&o.ambient, sizeof(o.ambient));
    mix(&o.diffuse, sizeof(o.diffuse));
    mix(&o.specular_color, sizeof(o.specular_color));
    mix(&o.shininess, sizeof(o.shininess));
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return {buf};
}

void serialize_opt(const OpticsDevice& o, uint64_t entity_seq,
                   const std::string& key,
                   std::vector<uint8_t>& entries) {
    OpticsPacked p;
    p.visible = o.visible;
    p.bEnableLighting = o.bEnableLighting;
    p.metallic = o.metallic;
    p.roughness = o.roughness;
    p.subsurface = o.subsurface;
    p.specular = o.specular;
    p.specularTint = o.specularTint;
    p.anisotropic = o.anisotropic;
    p.sheen = o.sheen;
    p.sheenTint = o.sheenTint;
    p.clearcoat = o.clearcoat;
    p.clearcoatGloss = o.clearcoatGloss;
    p.ambient[0] = o.ambient.x;  p.ambient[1] = o.ambient.y;  p.ambient[2] = o.ambient.z;
    p.diffuse[0] = o.diffuse.x;  p.diffuse[1] = o.diffuse.y;  p.diffuse[2] = o.diffuse.z;
    p.specular_color[0] = o.specular_color.x; p.specular_color[1] = o.specular_color.y; p.specular_color[2] = o.specular_color.z;
    p.shininess = o.shininess;
    auto entry = build_dirty_entries(StorageID::ST_OPTICS, entity_seq,
                                     key.c_str(), static_cast<uint16_t>(key.size()),
                                     &p, sizeof(p));
    entries.insert(entries.end(), entry.begin(), entry.end());
}

void deserialize_opt(OpticsDevice& o, const uint8_t* value, uint16_t value_len) {
    if (value_len != sizeof(OpticsPacked)) return;
    const auto* p = reinterpret_cast<const OpticsPacked*>(value);
    o.visible = p->visible;
    o.bEnableLighting = p->bEnableLighting;
    o.metallic = p->metallic;
    o.roughness = p->roughness;
    o.subsurface = p->subsurface;
    o.specular = p->specular;
    o.specularTint = p->specularTint;
    o.anisotropic = p->anisotropic;
    o.sheen = p->sheen;
    o.sheenTint = p->sheenTint;
    o.clearcoat = p->clearcoat;
    o.clearcoatGloss = p->clearcoatGloss;
    o.ambient = ktm::fvec3{p->ambient[0], p->ambient[1], p->ambient[2]};
    o.diffuse = ktm::fvec3{p->diffuse[0], p->diffuse[1], p->diffuse[2]};
    o.specular_color = ktm::fvec3{p->specular_color[0], p->specular_color[1], p->specular_color[2]};
    o.shininess = p->shininess;
}

// ---- EnvironmentDevice helpers ----
std::string hash_env(const EnvironmentDevice& e, uint64_t entity_seq) {
    uint64_t h = entity_seq;
    auto mix = [&](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) h = h * 31 + b[i];
    };
    mix(&e.sun_position, sizeof(e.sun_position));
    mix(&e.sun_intensity, sizeof(e.sun_intensity));
    mix(&e.exposure, sizeof(e.exposure));
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return {buf};
}

void serialize_env(const EnvironmentDevice& e, uint64_t entity_seq,
                   const std::string& key,
                   std::vector<uint8_t>& entries) {
    float data[5];
    data[0] = e.sun_position.x; data[1] = e.sun_position.y; data[2] = e.sun_position.z;
    data[3] = e.sun_intensity;
    data[4] = e.exposure;

    auto entry = build_dirty_entries(StorageID::ST_ENVIRONMENT, entity_seq,
                                     key.c_str(), static_cast<uint16_t>(key.size()),
                                     data, sizeof(data));
    entries.insert(entries.end(), entry.begin(), entry.end());
}

void deserialize_env(EnvironmentDevice& e, const uint8_t* value, uint16_t value_len) {
    if (value_len != 20) return;
    const auto* f = reinterpret_cast<const float*>(value);
    e.sun_position.x = f[0]; e.sun_position.y = f[1]; e.sun_position.z = f[2];
    e.sun_intensity = f[3];
    e.exposure = f[4];
}

}  // anonymous namespace

// ============================================================================
// Impl
// ============================================================================

struct SyncEngine::Impl {
    SharedDataHub& hub;
    std::string local_peer_id;
    LwwState lww_state;

    // Snapshot: key → hash of last-synced value
    std::unordered_map<std::string, std::string> last_synced;
    mutable std::mutex snapshot_mutex;

    // Entity mapping: seq_id → ObjectId for each storage, rebuilt each tick.
    using ObjMap = std::unordered_map<uint64_t, std::uintptr_t>;

    ObjMap mt_seq_to_id;   // ModelTransform
    ObjMap mr_seq_to_id;   // ModelResource
    ObjMap geo_seq_to_id;  // GeometryDevice
    ObjMap opt_seq_to_id;  // OpticsDevice
    ObjMap env_seq_to_id;  // EnvironmentDevice

    // Callbacks
    OnSyncOutgoing on_outgoing;
    OnFullSyncRequest on_full_sync_request;
    ResolveActorGuidForEntity guid_for_entity;
    ResolveEntitySeqForActorGuid entity_for_guid;
    ResolveLocalOwnershipForEntity ownership_for_entity;

    // Sequence counter for outgoing packets
    uint32_t seq = 0;

    // True while we are applying remote data — suppress outgoing re-broadcast
    bool syncing_from_remote = false;

    Impl(SharedDataHub& h) : hub(h), lww_state("uninitialized") {}

    void rebuild_entity_maps() {
        mt_seq_to_id.clear();
        {
            auto& store = hub.model_transform_storage();
            for (auto it = store.cbegin(); it != store.cend(); ++it) {
                auto obj_id = reinterpret_cast<std::uintptr_t>(&(*it));
                mt_seq_to_id[store.seq_id(obj_id)] = obj_id;
            }
        }

        mr_seq_to_id.clear();
        {
            auto& store = hub.model_resource_storage();
            for (auto it = store.cbegin(); it != store.cend(); ++it) {
                auto obj_id = reinterpret_cast<std::uintptr_t>(&(*it));
                mr_seq_to_id[store.seq_id(obj_id)] = obj_id;
            }
        }

        geo_seq_to_id.clear();
        {
            auto& store = hub.geometry_storage();
            for (auto it = store.cbegin(); it != store.cend(); ++it) {
                auto obj_id = reinterpret_cast<std::uintptr_t>(&(*it));
                geo_seq_to_id[store.seq_id(obj_id)] = obj_id;
            }
        }

        opt_seq_to_id.clear();
        {
            auto& store = hub.optics_storage();
            for (auto it = store.cbegin(); it != store.cend(); ++it) {
                auto obj_id = reinterpret_cast<std::uintptr_t>(&(*it));
                opt_seq_to_id[store.seq_id(obj_id)] = obj_id;
            }
        }

        env_seq_to_id.clear();
        {
            auto& store = hub.environment_storage();
            for (auto it = store.cbegin(); it != store.cend(); ++it) {
                auto obj_id = reinterpret_cast<std::uintptr_t>(&(*it));
                env_seq_to_id[store.seq_id(obj_id)] = obj_id;
            }
        }
    }

    std::string make_key(StorageID sid, uint64_t entity_seq, const std::string& field) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%u-%016llx-%s",
                      static_cast<unsigned>(sid),
                      static_cast<unsigned long long>(entity_seq),
                      field.c_str());
        return {buf};
    }

    struct ResolvedDirtyKey {
        uint64_t entity_seq = 0;
        std::string field;
        bool valid = true;
    };

    ResolvedDirtyKey resolve_dirty_key(StorageID sid, uint64_t remote_entity_seq,
                                       const std::string& key) const {
        constexpr const char* kPrefix = "actor:";
        const std::string prefix{kPrefix};
        if (key.rfind(prefix, 0) == 0) {
            auto field_sep = key.rfind(':');
            if (field_sep != std::string::npos && field_sep > prefix.size()) {
                auto guid = key.substr(prefix.size(), field_sep - prefix.size());
                auto field = key.substr(field_sep + 1);
                if (entity_for_guid) {
                    auto local_seq = entity_for_guid(sid, guid);
                    if (local_seq) {
                        return {*local_seq, field};
                    }
                }
                return {0, field, false};
            }
        }
        return {remote_entity_seq, key};
    }

    bool check_snapshot(const std::string& snap_key, const std::string& cur_hash) {
        std::lock_guard lock(snapshot_mutex);
        auto it = last_synced.find(snap_key);
        if (it != last_synced.end() && it->second == cur_hash) return false;
        last_synced[snap_key] = cur_hash;
        return true;
    }

    void set_snapshot(const std::string& snap_key, const std::string& cur_hash) {
        std::lock_guard lock(snapshot_mutex);
        last_synced[snap_key] = cur_hash;
    }
};

// ============================================================================

SyncEngine::SyncEngine() : impl_(std::make_unique<Impl>(SharedDataHub::instance())) {}

SyncEngine::~SyncEngine() = default;

void SyncEngine::initialize(const std::string& local_peer_id) {
    impl_->local_peer_id = local_peer_id;
    impl_->lww_state.set_local_peer_id(local_peer_id);
    impl_->last_synced.clear();
    impl_->seq = 0;
}

LwwApplyResult SyncEngine::apply_editor_operation(
    const EditorSyncOperation& operation) {
    const auto result = operation.kind == EditorSyncOperationKind::Delete
        ? impl_->lww_state.apply_delete(operation.actor_guid, operation.version)
        : impl_->lww_state.apply_upsert(operation.actor_guid, operation.field_name,
                                        operation.value, operation.version);
    if (result != LwwApplyResult::Applied || operation.kind == EditorSyncOperationKind::Delete) {
        return result;
    }

    impl_->rebuild_entity_maps();
    auto& hub = impl_->hub;
    auto apply_to_transform = [&]() {
        if (!impl_->entity_for_guid) return;
        auto seq = impl_->entity_for_guid(StorageID::ST_MODEL_TRANSFORM, operation.actor_guid);
        if (!seq) return;
        auto it = impl_->mt_seq_to_id.find(*seq);
        if (it == impl_->mt_seq_to_id.end()) return;
        auto handle = hub.model_transform_storage().try_acquire_write(it->second);
        if (handle.valid()) {
            deserialize_mt(*handle, operation.value.data(),
                           static_cast<uint16_t>(operation.value.size()));
            impl_->set_snapshot(impl_->make_key(StorageID::ST_MODEL_TRANSFORM,
                                                 *seq, "xform"),
                                hash_mt(*handle, *seq));
        }
    };
    if (operation.field_name == "xform") apply_to_transform();
    if (operation.actor_guid == "__scene__" && operation.field_name == "environment") {
        auto& store = hub.environment_storage();
        if (!impl_->env_seq_to_id.empty()) {
            auto seq_it = impl_->env_seq_to_id.begin();
            auto seq = seq_it->first;
            auto handle = store.try_acquire_write(seq_it->second);
            if (handle.valid()) {
                deserialize_env(*handle, operation.value.data(),
                                static_cast<uint16_t>(operation.value.size()));
                impl_->set_snapshot(impl_->make_key(StorageID::ST_ENVIRONMENT, seq, "env"),
                                    hash_env(*handle, seq));
            }
        }
    }
    return result;
}

void SyncEngine::shutdown() {
    impl_->last_synced.clear();
}

// ============================================================================
// Outbound
// ============================================================================

void SyncEngine::poll_and_sync() {
    if (impl_->syncing_from_remote) return;

    impl_->rebuild_entity_maps();

    auto& hub = impl_->hub;
    std::vector<uint8_t> entries_payload;
    uint32_t dirty_count = 0;

    // --- ModelTransform ---
    {
        auto& store = hub.model_transform_storage();
        for (auto it = store.cbegin(); it != store.cend(); ++it) {
            const ModelTransform& data = *it;
            auto obj_id = reinterpret_cast<std::uintptr_t>(&data);
            auto ent_seq = store.seq_id(obj_id);

            std::string wire_key = "xform";
            if (impl_->guid_for_entity) {
                auto actor_guid = impl_->guid_for_entity(
                    StorageID::ST_MODEL_TRANSFORM, ent_seq);
                if (actor_guid.empty()) {
                    continue;
                }
                wire_key = "actor:" + actor_guid + ":xform";
            }

            if (impl_->ownership_for_entity) {
                auto locally_owned = impl_->ownership_for_entity(
                    StorageID::ST_MODEL_TRANSFORM, ent_seq);
                if (locally_owned && !*locally_owned) {
                    continue;
                }
            }

            std::string cur_hash = hash_mt(data, ent_seq);
            std::string snap_key = impl_->make_key(StorageID::ST_MODEL_TRANSFORM, ent_seq, "xform");

            if (impl_->check_snapshot(snap_key, cur_hash)) {
                if (wire_key.rfind("actor:", 0) == 0 && impl_->on_outgoing) {
                    EditorSyncOperation op;
                    op.kind = EditorSyncOperationKind::Upsert;
                    op.actor_guid = wire_key.substr(6, wire_key.size() - 6 - 6);
                    op.field_name = "xform";
                    op.value.resize(36);
                    float values[9] = {data.position.x, data.position.y, data.position.z,
                                       data.euler_rotation.x, data.euler_rotation.y, data.euler_rotation.z,
                                       data.scale.x, data.scale.y, data.scale.z};
                    std::memcpy(op.value.data(), values, sizeof(values));
                    op.version = impl_->lww_state.next_local_version();
                    auto packet = build_editor_sync_operation(op);
                    if (!packet.empty()) impl_->on_outgoing(packet);
                } else {
                    serialize_mt(data, ent_seq, wire_key, entries_payload);
                    ++dirty_count;
                }
            }
        }
    }

    // Geometry, model resource, and optics are intentionally not synced here.
    // Actor creation and file transfer build those local objects on each peer;
    // syncing handle-bearing structures afterward can overwrite valid local
    // links with seq/handle values from another process and make meshes vanish.

    // --- EnvironmentDevice ---
    {
        auto& store = hub.environment_storage();
        for (auto it = store.cbegin(); it != store.cend(); ++it) {
            const EnvironmentDevice& data = *it;
            auto obj_id = reinterpret_cast<std::uintptr_t>(&data);
            auto ent_seq = store.seq_id(obj_id);

            std::string cur_hash = hash_env(data, ent_seq);
            std::string snap_key = impl_->make_key(StorageID::ST_ENVIRONMENT, ent_seq, "env");

            if (impl_->check_snapshot(snap_key, cur_hash)) {
                if (impl_->on_outgoing) {
                    EditorSyncOperation op;
                    op.kind = EditorSyncOperationKind::Upsert;
                    op.actor_guid = "__scene__";
                    op.field_name = "environment";
                    op.value.resize(20);
                    float values[5] = {data.sun_position.x, data.sun_position.y,
                                       data.sun_position.z, data.sun_intensity,
                                       data.exposure};
                    std::memcpy(op.value.data(), values, sizeof(values));
                    op.version = impl_->lww_state.next_local_version();
                    auto packet = build_editor_sync_operation(op);
                    if (!packet.empty()) impl_->on_outgoing(packet);
                } else {
                    serialize_env(data, ent_seq, "env", entries_payload);
                    ++dirty_count;
                }
            }
        }
    }

    if (dirty_count == 0) return;

    using namespace std::chrono;
    auto now_ms = duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();

    auto pkt = build_sync_dirty(impl_->seq++, static_cast<uint64_t>(now_ms),
                                entries_payload, dirty_count);

    if (impl_->on_outgoing) {
        impl_->on_outgoing(pkt);
    }
}

void SyncEngine::sync_full_to(const std::string& /*target_peer_id*/) {
    CFW_LOG_DEBUG("SyncEngine: sync_full_to not yet implemented");
}

// ============================================================================
// Inbound
// ============================================================================

void SyncEngine::handle_incoming(const std::string& sender_peer_id,
                                 const uint8_t* data, size_t len) {
    if (len < 2) return;

    if (data[0] == static_cast<uint8_t>(MessageType::EDITOR_SYNC)) {
        auto operation = parse_editor_sync_operation(data, len);
        if (!operation || operation->version.writer_peer_id != sender_peer_id) return;
        (void)apply_editor_operation(*operation);
        return;
    }

    BufferReader r(data, len);
    auto type = static_cast<MessageType>(r.read_u8());

    if (type == MessageType::SYNC_DIRTY) {
        if (!r.has_remaining(16)) return;
        (void)r.read_u32();  // seq
        (void)r.read_u64();  // remote_ts
        uint32_t count = r.read_u32();

        impl_->rebuild_entity_maps();
        auto& hub = impl_->hub;

        for (uint32_t i = 0; i < count; ++i) {
            if (!r.has_remaining(2 + 8 + 2 + 2)) break;
            auto storage_id = static_cast<StorageID>(r.read_u16());
            uint64_t entity_seq = r.read_u64();
            uint16_t key_len = r.read_u16();
            uint16_t value_len = r.read_u16();

            if (!r.has_remaining(key_len + value_len)) break;
            std::string key = r.read_string(key_len);
            const uint8_t* value_ptr = r.data + r.pos;
            r.read_string(value_len);
            const auto resolved_key = impl_->resolve_dirty_key(storage_id, entity_seq, key);
            if (!resolved_key.valid) {
                continue;
            }
            const uint64_t target_seq = resolved_key.entity_seq;
            const std::string& field_key = resolved_key.field;

            impl_->syncing_from_remote = true;

            switch (storage_id) {
            case StorageID::ST_MODEL_TRANSFORM: {
                auto& store = hub.model_transform_storage();
                auto map_it = impl_->mt_seq_to_id.find(target_seq);
                if (map_it != impl_->mt_seq_to_id.end()) {
                    // Blocking write — must not silently drop transform updates
                    // or remote objects will jitter.  Render threads hold shared
                    // read locks only briefly; the block is O(µs).
                    auto handle = store.try_acquire_write(map_it->second);
                    if (handle.valid()) {
                        deserialize_mt(*handle, value_ptr, value_len);
                        impl_->set_snapshot(
                            impl_->make_key(storage_id, target_seq, field_key),
                            hash_mt(*handle, target_seq));
                    }
                }
                break;
            }
            case StorageID::ST_MODEL_RESOURCE: {
                // Model resources are created by the actor/file-transfer path.
                // Ignore remote handle-bearing updates to avoid breaking local links.
                break;
            }
            case StorageID::ST_GEOMETRY: {
                // Geometry stores local object handles; actor creation owns it.
                break;
            }
            case StorageID::ST_OPTICS: {
                // Keep optics local for now; transform-only object sync is safer.
                break;
            }
            case StorageID::ST_ENVIRONMENT: {
                auto& store = hub.environment_storage();
                auto map_it = impl_->env_seq_to_id.find(target_seq);
                if (map_it != impl_->env_seq_to_id.end()) {
                    auto handle = store.try_acquire_write(map_it->second);
                    if (handle.valid()) {
                        deserialize_env(*handle, value_ptr, value_len);
                        impl_->set_snapshot(
                            impl_->make_key(storage_id, target_seq, field_key),
                            hash_env(*handle, target_seq));
                    }
                }
                break;
            }
            default:
                break;
            }

            impl_->syncing_from_remote = false;
        }
    }
    else if (type == MessageType::SYNC_FULL) {
        if (!r.has_remaining(8)) return;
        (void)r.read_u32();  // seq
        uint32_t count = r.read_u32();
        handle_incoming(sender_peer_id, data + 1 + 8, len - 1 - 8);
    }
    else if (type == MessageType::HEARTBEAT) {
        // No action needed
    }
}

// ============================================================================
void SyncEngine::set_on_outgoing(OnSyncOutgoing cb) {
    impl_->on_outgoing = std::move(cb);
}

void SyncEngine::set_on_full_sync_request(OnFullSyncRequest cb) {
    impl_->on_full_sync_request = std::move(cb);
}

void SyncEngine::set_identity_mapping_callbacks(
    ResolveActorGuidForEntity guid_for_entity,
    ResolveEntitySeqForActorGuid entity_for_guid,
    ResolveLocalOwnershipForEntity ownership_for_entity) {
    impl_->guid_for_entity = std::move(guid_for_entity);
    impl_->entity_for_guid = std::move(entity_for_guid);
    impl_->ownership_for_entity = std::move(ownership_for_entity);
}

}  // namespace Corona::Network
