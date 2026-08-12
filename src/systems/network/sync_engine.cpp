#include <corona/systems/network/sync_engine.h>

#include <corona/shared_data_hub.h>
#include <corona/kernel/core/i_logger.h>

#include <chrono>
#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>

namespace Corona::Network {

// ============================================================================
// Per-type serialize / deserialize / hash helpers
// ============================================================================

namespace {

uint64_t editor_now_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count());
}

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
    std::deque<EditorSyncOperation> pending_editor_operations;

    struct IncomingEditorSnapshot {
        uint32_t snapshot_id = 0;
        uint16_t chunk_total = 0;
        uint32_t operation_total = 0;
        uint32_t received_operation_count = 0;
        uint64_t last_activity_ms = 0;
        std::vector<bool> received_chunks;
        std::vector<std::vector<uint8_t>> raw_chunks;
        std::vector<std::vector<EditorSyncOperation>> chunk_operations;
        std::vector<EditorSyncOperation> queued_incremental;
    };
    std::unordered_map<std::string, IncomingEditorSnapshot> incoming_editor_snapshots;

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
    OnTargetedSyncOutgoing on_targeted_outgoing;
    OnFullSyncRequest on_full_sync_request;
    OnEditorOperationApplied on_editor_operation_applied;
    ResolveActorGuidForEntity guid_for_entity;
    ResolveEntitySeqForActorGuid entity_for_guid;
    ResolveLocalOwnershipForEntity ownership_for_entity;

    // Sequence counter for outgoing packets
    uint32_t seq = 0;

    // True while we are applying remote data — suppress outgoing re-broadcast
    bool syncing_from_remote = false;

    void recover_incomplete_snapshot(
        const std::string& sender_peer_id,
        IncomingEditorSnapshot snapshot,
        const std::function<LwwApplyResult(const EditorSyncOperation&)>& apply,
        bool request_retry = true) {
        for (const auto& operation : snapshot.queued_incremental) {
            (void)apply(operation);
        }
        if (request_retry && on_targeted_outgoing) {
            auto request = build_editor_snapshot_request();
            on_targeted_outgoing(sender_peer_id, request);
        }
    }

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
    impl_->pending_editor_operations.clear();
    impl_->incoming_editor_snapshots.clear();
    impl_->seq = 0;
}

LwwApplyResult SyncEngine::apply_editor_operation(
    const EditorSyncOperation& operation) {
    const auto result = operation.kind == EditorSyncOperationKind::Delete
        ? impl_->lww_state.apply_delete(operation.actor_guid, operation.version)
        : impl_->lww_state.apply_upsert(operation.actor_guid, operation.field_name,
                                        operation.value, operation.version);
    if (result != LwwApplyResult::Applied) return result;
    if (operation.kind == EditorSyncOperationKind::Delete) {
        if (impl_->lww_state.is_deleted(operation.actor_guid) &&
            impl_->on_editor_operation_applied) {
            impl_->on_editor_operation_applied(operation);
        }
        return result;
    }

    if (!apply_operation_to_storage(operation)) {
        impl_->pending_editor_operations.push_back(operation);
    }
    if (impl_->on_editor_operation_applied) {
        impl_->on_editor_operation_applied(operation);
    }
    return result;
}

EditorSyncOperation SyncEngine::make_local_delete(const std::string& actor_guid) {
    EditorSyncOperation op;
    op.kind = EditorSyncOperationKind::Delete;
    op.actor_guid = actor_guid;
    op.version = impl_->lww_state.next_local_version();
    (void)impl_->lww_state.apply_delete(op.actor_guid, op.version);
    return op;
}

EditorSyncOperation SyncEngine::make_local_upsert(
    const std::string& actor_guid, const std::string& field_name,
    std::vector<uint8_t> value) {
    EditorSyncOperation op;
    op.kind = EditorSyncOperationKind::Upsert;
    op.actor_guid = actor_guid;
    op.field_name = field_name;
    op.value = std::move(value);
    op.version = impl_->lww_state.next_local_version();
    (void)impl_->lww_state.apply_upsert(op.actor_guid, op.field_name,
                                         op.value, op.version);
    return op;
}

bool SyncEngine::apply_operation_to_storage(const EditorSyncOperation& operation) {
    if (operation.kind == EditorSyncOperationKind::Delete) return true;
    if (operation.field_name == "actor.create" ||
        operation.field_name == "actor.state" ||
        operation.field_name == "actor.transform") return true;

    impl_->rebuild_entity_maps();
    auto& hub = impl_->hub;
    bool applied = false;
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
            applied = true;
        }
    };
    if (operation.field_name == "xform") apply_to_transform();
    if (operation.field_name == "optics" && impl_->entity_for_guid) {
        auto seq = impl_->entity_for_guid(StorageID::ST_OPTICS, operation.actor_guid);
        if (seq) {
            auto it = impl_->opt_seq_to_id.find(*seq);
            if (it != impl_->opt_seq_to_id.end()) {
                auto handle = hub.optics_storage().try_acquire_write(it->second);
                if (handle.valid()) {
                    deserialize_opt(*handle, operation.value.data(),
                                    static_cast<uint16_t>(operation.value.size()));
                    impl_->set_snapshot(impl_->make_key(StorageID::ST_OPTICS, *seq, "optics"),
                                        hash_opt(*handle, *seq));
                    applied = true;
                }
            }
        }
    }
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
                applied = true;
            }
        }
    }
    return applied;
}

void SyncEngine::retry_pending_operations() {
    for (auto it = impl_->pending_editor_operations.begin();
         it != impl_->pending_editor_operations.end();) {
        if (apply_operation_to_storage(*it)) {
            it = impl_->pending_editor_operations.erase(it);
        } else {
            ++it;
        }
    }
}

void SyncEngine::maintain_snapshot_sessions(uint64_t now_ms) {
    for (auto it = impl_->incoming_editor_snapshots.begin();
         it != impl_->incoming_editor_snapshots.end();) {
        if (now_ms - it->second.last_activity_ms < kEditorSnapshotTimeoutMs) {
            ++it;
            continue;
        }
        const auto sender_peer_id = it->first;
        auto session = std::move(it->second);
        it = impl_->incoming_editor_snapshots.erase(it);
        impl_->recover_incomplete_snapshot(
            sender_peer_id, std::move(session),
            [this](const EditorSyncOperation& operation) {
                return apply_editor_operation(operation);
            });
    }
}

void SyncEngine::shutdown() {
    impl_->last_synced.clear();
    impl_->pending_editor_operations.clear();
    impl_->incoming_editor_snapshots.clear();
}

// ============================================================================
// Outbound
// ============================================================================

void SyncEngine::poll_and_sync() {
    if (impl_->syncing_from_remote) return;

    maintain_snapshot_sessions(editor_now_ms());
    retry_pending_operations();

    impl_->rebuild_entity_maps();

    auto& hub = impl_->hub;

    // --- ModelTransform ---
    {
        auto& store = hub.model_transform_storage();
        for (auto it = store.cbegin(); it != store.cend(); ++it) {
            const ModelTransform& data = *it;
            auto obj_id = reinterpret_cast<std::uintptr_t>(&data);
            auto ent_seq = store.seq_id(obj_id);

            if (!impl_->guid_for_entity) continue;
            auto actor_guid = impl_->guid_for_entity(
                StorageID::ST_MODEL_TRANSFORM, ent_seq);
            if (actor_guid.empty()) continue;

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
                EditorSyncOperation op;
                op.kind = EditorSyncOperationKind::Upsert;
                op.actor_guid = actor_guid;
                op.field_name = "xform";
                op.value.resize(36);
                float values[9] = {data.position.x, data.position.y, data.position.z,
                                   data.euler_rotation.x, data.euler_rotation.y, data.euler_rotation.z,
                                   data.scale.x, data.scale.y, data.scale.z};
                std::memcpy(op.value.data(), values, sizeof(values));
                op.version = impl_->lww_state.next_local_version();
                (void)impl_->lww_state.apply_upsert(op.actor_guid, op.field_name,
                                                     op.value, op.version);
                auto packet = build_editor_sync_operation(op);
                if (!packet.empty() && impl_->on_outgoing) impl_->on_outgoing(packet);
            }
        }
    }

    // Geometry and model resource handles remain local; optics editor state is
    // serialized below without its process-local geometry handle.
    // Actor creation and file transfer build those local objects on each peer;
    // syncing handle-bearing structures afterward can overwrite valid local
    // links with seq/handle values from another process and make meshes vanish.

    // --- OpticsDevice (logical material/editor state; geometry handle excluded) ---
    {
        auto& store = hub.optics_storage();
        for (auto it = store.cbegin(); it != store.cend(); ++it) {
            const OpticsDevice& data = *it;
            auto obj_id = reinterpret_cast<std::uintptr_t>(&data);
            auto ent_seq = store.seq_id(obj_id);
            if (!impl_->guid_for_entity) continue;
            auto actor_guid = impl_->guid_for_entity(StorageID::ST_OPTICS, ent_seq);
            if (actor_guid.empty()) continue;
            if (impl_->ownership_for_entity) {
                auto locally_owned = impl_->ownership_for_entity(StorageID::ST_OPTICS, ent_seq);
                if (locally_owned && !*locally_owned) continue;
            }
            auto cur_hash = hash_opt(data, ent_seq);
            auto snap_key = impl_->make_key(StorageID::ST_OPTICS, ent_seq, "optics");
            if (!impl_->check_snapshot(snap_key, cur_hash)) continue;
            EditorSyncOperation op;
            op.kind = EditorSyncOperationKind::Upsert;
            op.actor_guid = actor_guid;
            op.field_name = "optics";
            OpticsPacked packed{};
            packed.visible = data.visible;
            packed.bEnableLighting = data.bEnableLighting;
            packed.metallic = data.metallic; packed.roughness = data.roughness;
            packed.subsurface = data.subsurface; packed.specular = data.specular;
            packed.specularTint = data.specularTint; packed.anisotropic = data.anisotropic;
            packed.sheen = data.sheen; packed.sheenTint = data.sheenTint;
            packed.clearcoat = data.clearcoat; packed.clearcoatGloss = data.clearcoatGloss;
            packed.ambient[0] = data.ambient.x; packed.ambient[1] = data.ambient.y; packed.ambient[2] = data.ambient.z;
            packed.diffuse[0] = data.diffuse.x; packed.diffuse[1] = data.diffuse.y; packed.diffuse[2] = data.diffuse.z;
            packed.specular_color[0] = data.specular_color.x; packed.specular_color[1] = data.specular_color.y; packed.specular_color[2] = data.specular_color.z;
            packed.shininess = data.shininess;
            op.value.resize(sizeof(packed));
            std::memcpy(op.value.data(), &packed, sizeof(packed));
            op.version = impl_->lww_state.next_local_version();
            (void)impl_->lww_state.apply_upsert(op.actor_guid, op.field_name, op.value, op.version);
            auto packet = build_editor_sync_operation(op);
            if (!packet.empty() && impl_->on_outgoing) impl_->on_outgoing(packet);
        }
    }

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
                (void)impl_->lww_state.apply_upsert(op.actor_guid, op.field_name,
                                                     op.value, op.version);
                auto packet = build_editor_sync_operation(op);
                if (!packet.empty() && impl_->on_outgoing) impl_->on_outgoing(packet);
            }
        }
    }
}

void SyncEngine::sync_full_to(const std::string& target_peer_id) {
    // A peer may connect before the first 16 ms sync tick. Seed the LWW table
    // from current local state without broadcasting those seed operations.
    auto outgoing = std::move(impl_->on_outgoing);
    impl_->on_outgoing = [](const std::vector<uint8_t>&) {};
    poll_and_sync();
    impl_->on_outgoing = std::move(outgoing);
    const auto operations = impl_->lww_state.snapshot();
    if (!target_peer_id.empty() && impl_->on_targeted_outgoing) {
        const auto total = static_cast<uint16_t>(std::max<size_t>(
            1, (operations.size() + kEditorSnapshotChunkOperations - 1) /
                   kEditorSnapshotChunkOperations));
        const auto snapshot_id = impl_->seq++;
        auto begin_packet = build_editor_snapshot_begin(
            snapshot_id, total, static_cast<uint32_t>(operations.size()));
        if (begin_packet.empty()) return;
        impl_->on_targeted_outgoing(target_peer_id, begin_packet);
        for (uint16_t index = 0; index < total; ++index) {
            std::vector<std::vector<uint8_t>> encoded;
            const auto begin = static_cast<size_t>(index) * kEditorSnapshotChunkOperations;
            const auto end = std::min(operations.size(), begin + kEditorSnapshotChunkOperations);
            for (size_t i = begin; i < end; ++i) {
                auto packet = build_editor_sync_operation(operations[i]);
                if (!packet.empty()) encoded.push_back(std::move(packet));
            }
            auto chunk = build_editor_snapshot_chunk(snapshot_id, index, total, encoded);
            if (chunk.empty()) return;
            impl_->on_targeted_outgoing(target_peer_id, chunk);
        }
        auto end_packet = build_editor_snapshot_end(snapshot_id);
        if (!end_packet.empty()) impl_->on_targeted_outgoing(target_peer_id, end_packet);
        return;
    }
    emit_snapshot();
}

void SyncEngine::emit_snapshot() {
    if (!impl_->on_outgoing) return;
    for (const auto& operation : impl_->lww_state.snapshot()) {
        auto packet = build_editor_sync_operation(operation);
        if (!packet.empty()) impl_->on_outgoing(packet);
    }
}

std::vector<uint8_t> SyncEngine::make_snapshot_request() const {
    return build_editor_snapshot_request();
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
        auto snapshot = impl_->incoming_editor_snapshots.find(sender_peer_id);
        if (snapshot != impl_->incoming_editor_snapshots.end()) {
            if (snapshot->second.queued_incremental.size() >=
                kMaxEditorSyncOperations) return;
            snapshot->second.queued_incremental.push_back(std::move(*operation));
            snapshot->second.last_activity_ms = editor_now_ms();
            return;
        }
        (void)apply_editor_operation(*operation);
        return;
    }
    if (data[0] == static_cast<uint8_t>(MessageType::EDITOR_SNAPSHOT_REQUEST)) {
        if (len != 2 || data[1] != kEditorSnapshotSchemaVersion ||
            !impl_->on_full_sync_request) return;
        impl_->on_full_sync_request(sender_peer_id);
        return;
    }
    if (data[0] == static_cast<uint8_t>(MessageType::EDITOR_SNAPSHOT_BEGIN)) {
        if (len != 12) return;
        BufferReader r(data, len);
        if (r.read_u8() != static_cast<uint8_t>(MessageType::EDITOR_SNAPSHOT_BEGIN) ||
            r.read_u8() != kEditorSnapshotSchemaVersion) return;
        Impl::IncomingEditorSnapshot snapshot;
        snapshot.snapshot_id = r.read_u32();
        snapshot.chunk_total = r.read_u16();
        snapshot.operation_total = r.read_u32();
        snapshot.last_activity_ms = editor_now_ms();
        if (snapshot.chunk_total == 0 ||
            snapshot.chunk_total > kMaxEditorSnapshotChunks ||
            snapshot.operation_total > kMaxEditorSyncOperations) return;
        auto existing = impl_->incoming_editor_snapshots.find(sender_peer_id);
        if (existing != impl_->incoming_editor_snapshots.end()) {
            if (existing->second.snapshot_id == snapshot.snapshot_id &&
                existing->second.chunk_total == snapshot.chunk_total &&
                existing->second.operation_total == snapshot.operation_total) {
                return;
            }
            auto old_session = std::move(existing->second);
            impl_->incoming_editor_snapshots.erase(existing);
            impl_->recover_incomplete_snapshot(
                sender_peer_id, std::move(old_session),
                [this](const EditorSyncOperation& operation) {
                    return apply_editor_operation(operation);
                }, false);
        }
        snapshot.received_chunks.resize(snapshot.chunk_total, false);
        snapshot.raw_chunks.resize(snapshot.chunk_total);
        snapshot.chunk_operations.resize(snapshot.chunk_total);
        impl_->incoming_editor_snapshots[sender_peer_id] = std::move(snapshot);
        return;
    }
    if (data[0] == static_cast<uint8_t>(MessageType::EDITOR_SNAPSHOT_CHUNK)) {
        if (len > kMaxEditorSnapshotChunkBytes) return;
        BufferReader r(data, len);
        if (!r.has_remaining(1 + 1 + 4 + 2 + 2 + 2) ||
            r.read_u8() != static_cast<uint8_t>(MessageType::EDITOR_SNAPSHOT_CHUNK) ||
            r.read_u8() != kEditorSnapshotSchemaVersion) return;
        const auto snapshot_id = r.read_u32();
        const auto index = r.read_u16();
        const auto total = r.read_u16();
        const auto count = r.read_u16();
        if (total == 0 || index >= total || count > kEditorSnapshotChunkOperations) return;
        auto snapshot = impl_->incoming_editor_snapshots.find(sender_peer_id);
        if (snapshot == impl_->incoming_editor_snapshots.end() ||
            snapshot->second.snapshot_id != snapshot_id ||
            snapshot->second.chunk_total != total) return;
        std::vector<EditorSyncOperation> operations;
        operations.reserve(count);
        for (uint16_t i = 0; i < count; ++i) {
            if (!r.has_remaining(4)) return;
            const auto operation_len = r.read_u32();
            if (operation_len == 0 || operation_len > kMaxEditorSyncValueBytes ||
                !r.has_remaining(operation_len)) return;
            const auto* operation_data = r.data + r.pos;
            auto operation = parse_editor_sync_operation(operation_data, operation_len);
            if (!operation) return;
            // A snapshot forwards the winning version it currently stores.
            // Its writer may therefore be a third peer rather than the peer
            // transporting this snapshot chunk.
            operations.push_back(std::move(*operation));
            r.pos += operation_len;
        }
        if (r.pos != len) return;
        auto& session = snapshot->second;
        session.last_activity_ms = editor_now_ms();
        const std::vector<uint8_t> raw(data, data + len);
        if (session.received_chunks[index]) {
            if (session.raw_chunks[index] != raw) {
                auto failed_session = std::move(session);
                impl_->incoming_editor_snapshots.erase(snapshot);
                impl_->recover_incomplete_snapshot(
                    sender_peer_id, std::move(failed_session),
                    [this](const EditorSyncOperation& operation) {
                        return apply_editor_operation(operation);
                    });
            }
            return;
        }
        if (operations.size() >
            session.operation_total - session.received_operation_count) {
            auto failed_session = std::move(session);
            impl_->incoming_editor_snapshots.erase(snapshot);
            impl_->recover_incomplete_snapshot(
                sender_peer_id, std::move(failed_session),
                [this](const EditorSyncOperation& operation) {
                    return apply_editor_operation(operation);
                });
            return;
        }
        session.received_chunks[index] = true;
        session.received_operation_count += static_cast<uint32_t>(operations.size());
        session.raw_chunks[index] = raw;
        session.chunk_operations[index] = std::move(operations);
        return;
    }
    if (data[0] == static_cast<uint8_t>(MessageType::EDITOR_SNAPSHOT_END)) {
        if (len != 6) return;
        BufferReader r(data, len);
        if (r.read_u8() != static_cast<uint8_t>(MessageType::EDITOR_SNAPSHOT_END) ||
            r.read_u8() != kEditorSnapshotSchemaVersion) return;
        const auto snapshot_id = r.read_u32();
        auto snapshot = impl_->incoming_editor_snapshots.find(sender_peer_id);
        if (snapshot == impl_->incoming_editor_snapshots.end() ||
            snapshot->second.snapshot_id != snapshot_id) return;
        auto session = std::move(snapshot->second);
        impl_->incoming_editor_snapshots.erase(snapshot);
        const bool complete =
            std::find(session.received_chunks.begin(), session.received_chunks.end(),
                      false) == session.received_chunks.end();
        if (complete &&
            session.received_operation_count == session.operation_total) {
            for (const auto& operations : session.chunk_operations) {
                for (const auto& operation : operations) {
                    (void)apply_editor_operation(operation);
                }
            }
            for (const auto& operation : session.queued_incremental) {
                (void)apply_editor_operation(operation);
            }
        } else {
            impl_->recover_incomplete_snapshot(
                sender_peer_id, std::move(session),
                [this](const EditorSyncOperation& operation) {
                    return apply_editor_operation(operation);
                });
        }
        return;
    }

    BufferReader r(data, len);
    auto type = static_cast<MessageType>(r.read_u8());

    if (type == MessageType::SYNC_DIRTY) {
        // Legacy dirty packets carry no LWW version and must never mutate
        // collaborative editor state. New peers use EDITOR_SYNC exclusively.
        return;
    }
    else if (type == MessageType::SYNC_FULL) {
        // Legacy SYNC_FULL has no LWW versions and its old parser treated the
        // first entry byte as a message type. Reject it; new peers use the
        // versioned snapshot request/chunk protocol above.
        return;
    }
    else if (type == MessageType::HEARTBEAT) {
        // No action needed
    }
}

// ============================================================================
void SyncEngine::set_on_outgoing(OnSyncOutgoing cb) {
    impl_->on_outgoing = std::move(cb);
}

void SyncEngine::set_on_targeted_outgoing(OnTargetedSyncOutgoing cb) {
    impl_->on_targeted_outgoing = std::move(cb);
}

void SyncEngine::set_on_full_sync_request(OnFullSyncRequest cb) {
    impl_->on_full_sync_request = std::move(cb);
}

void SyncEngine::set_on_editor_operation_applied(OnEditorOperationApplied cb) {
    impl_->on_editor_operation_applied = std::move(cb);
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
