#pragma once

#include <corona/systems/optics/vision_pipeline_key.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vision {
class GeometryGpuResource;
class SceneData;
}

namespace Corona::Systems::Vision {

struct VisionGeometrySnapshot;
struct VisionSceneImportCache;

enum class VisionResourceOwnership {
    SharedLogicalScene,
    SharedSceneGpu,
    PerPipelineRenderState,
    LegacyPipelineOwned,
};

struct VisionOwnershipAuditEntry {
    std::string_view name;
    VisionResourceOwnership target_ownership;
    std::string_view current_owner;
    std::string_view phase4_action;
};

inline constexpr VisionOwnershipAuditEntry kVisionPhase4OwnershipAudit[] = {
    {"Pipeline::scene_",
     VisionResourceOwnership::SharedLogicalScene,
     "vision::Pipeline",
     "Replaced by per-pipeline Scene view bound to shared VisionSceneResource SceneData in Phase 4D."},
    {"SceneData logical objects",
     VisionResourceOwnership::SharedLogicalScene,
     "VisionSceneResource",
     "Own parsed logical scene identity once per external source."},
    {"Scene::geometry_",
     VisionResourceOwnership::SharedSceneGpu,
     "vision::Scene",
     "Move scene-correlated GPU geometry state into shared scene GPU resource in Phase 4C."},
    {"Geometry::gpu_resource_",
     VisionResourceOwnership::SharedSceneGpu,
     "vision::Geometry",
     "Bind Geometry to the VisionSceneResource scene GPU resource instead of storing Pipeline*."},
    {"GeometryData mesh buffers",
     VisionResourceOwnership::SharedSceneGpu,
     "vision::GeometryData via GeometryGpuResource BindlessArray",
     "Share mesh buffers and mesh handle buffers per logical scene, not per render mode."},
    {"Geometry instance buffers",
     VisionResourceOwnership::SharedSceneGpu,
     "vision::GeometryData via GeometryGpuResource BindlessArray",
     "Update instance buffers and TLAS once per shared logical transform version."},
    {"Accel / BLAS / TLAS",
     VisionResourceOwnership::SharedSceneGpu,
     "vision::GeometryGpuResource with caller-supplied Stream",
     "Move acceleration structures under shared scene GPU resource."},
    {"Material and medium registries",
     VisionResourceOwnership::SharedSceneGpu,
     "vision::SceneData prepared through VisionSceneResource scene GPU bindless",
     "Prepare scene material/medium tables through explicit scene GPU bindless when available."},
    {"Light tables",
     VisionResourceOwnership::SharedSceneGpu,
     "vision::LightManager prepared through VisionSceneResource scene GPU bindless",
     "Prepare scene light tables through explicit scene GPU bindless when available."},
    {"ImagePool textures",
     VisionResourceOwnership::SharedSceneGpu,
     "vision::SceneData ImagePool with VisionSceneResource scene GPU bindless",
     "Keep image textures in the shared logical scene's image pool and bind them through shared scene GPU resources."},
    {"FrameBuffer and denoiser state",
     VisionResourceOwnership::PerPipelineRenderState,
     "vision::Pipeline renderer/framebuffer",
     "Keep per PT/SVGF/SSAT runtime."},
    {"Global::pipeline() and Global::bindless_array() users",
     VisionResourceOwnership::LegacyPipelineOwned,
     "vision::Global / Toolkit helpers",
     "Replace usages that bind scene data to one active pipeline before Phase 5."},
};

struct VisionSceneResourceKey {
    std::string source_path_key;
    VisionPipelineSource source{VisionPipelineSource::EngineBuilt};

    friend bool operator==(const VisionSceneResourceKey& lhs,
                           const VisionSceneResourceKey& rhs) noexcept {
        return lhs.source_path_key == rhs.source_path_key && lhs.source == rhs.source;
    }
};

struct VisionSceneResourceKeyHash {
    [[nodiscard]] std::size_t operator()(
        const VisionSceneResourceKey& key) const noexcept {
        std::size_t seed = std::hash<std::string>{}(key.source_path_key);
        seed ^= std::hash<int>{}(static_cast<int>(key.source)) +
                0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
        return seed;
    }
};

struct VisionLogicalInstanceKey {
    int shape_index{-1};
    int instance_index{-1};

    friend bool operator==(const VisionLogicalInstanceKey& lhs,
                           const VisionLogicalInstanceKey& rhs) noexcept {
        return lhs.shape_index == rhs.shape_index &&
               lhs.instance_index == rhs.instance_index;
    }
};

struct VisionLogicalInstanceKeyHash {
    [[nodiscard]] std::size_t operator()(
        const VisionLogicalInstanceKey& key) const noexcept {
        std::size_t seed = std::hash<int>{}(key.shape_index);
        seed ^= std::hash<int>{}(key.instance_index) +
                0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
        return seed;
    }
};

struct VisionLogicalInstanceRecord {
    VisionLogicalInstanceKey key;
    std::uintptr_t actor_handle{0};
    std::size_t transform_signature{0};
    std::array<float, 16> object_to_world{};
};

struct ExternalLiveShapeRecord {
    std::uintptr_t actor_handle{0};
    int shape_index{-1};
    std::string shape_guid;
    std::string shape_identity_key;
    bool dynamically_added{false};
};

enum class ExternalLiveShapeRemovalAction {
    ForgetTracking,
    HideOriginal,
    RemoveTopology,
};

[[nodiscard]] constexpr ExternalLiveShapeRemovalAction external_live_shape_removal_action(
    const ExternalLiveShapeRecord& record,
    bool embedded_runtime) noexcept {
    if (record.dynamically_added) {
        return ExternalLiveShapeRemovalAction::RemoveTopology;
    }
    return embedded_runtime ? ExternalLiveShapeRemovalAction::HideOriginal
                            : ExternalLiveShapeRemovalAction::ForgetTracking;
}

[[nodiscard]] constexpr std::string_view external_live_shape_removal_action_name(
    ExternalLiveShapeRemovalAction action) noexcept {
    switch (action) {
        case ExternalLiveShapeRemovalAction::ForgetTracking:
            return "forget_tracking";
        case ExternalLiveShapeRemovalAction::HideOriginal:
            return "hide_original";
        case ExternalLiveShapeRemovalAction::RemoveTopology:
            return "remove_topology";
    }
    return "forget_tracking";
}

// Tracks an engine-native actor (one WITHOUT an external_vision_binding) that has
// been mixed into an ExternalLive Vision scene as an appended shape. Kept in a map
// separate from external_live_shapes_by_actor because the bound-proxy reaper would
// otherwise delete these every frame (they never appear in active_bound_actors).
// Both maps index the same vision::Scene::groups_ vector, so removals must remap
// indices in BOTH (see remap_external_live_shape_indices_after_remove).
struct EngineMixedShapeRecord {
    std::uintptr_t actor_handle{0};
    int shape_index{-1};
    std::size_t transform_signature{0};
};

enum class VisionSceneSourceKind { File, Embedded };

// Scene identity is not a filename. Retain the import source independently of
// render-mode runtimes; paths are captured once when a load request is accepted.
struct VisionSceneSourceDesc {
    VisionSceneSourceKind kind{VisionSceneSourceKind::File};
    std::string file_path;
    std::string scene_json;
    std::string base_dir;

    friend bool operator==(const VisionSceneSourceDesc&, const VisionSceneSourceDesc&) = default;
};

struct VisionSceneResource {
    VisionSceneResourceKey key;
    std::string display_source_path;
    std::optional<VisionSceneSourceDesc> source_desc;
    std::uint64_t source_revision{0};
    // Parsed geometry and decoded images only; survives retirement of GPU runtimes.
    std::shared_ptr<VisionSceneImportCache> import_cache;
    // CPU-only publication survives idle runtime eviction; a new source replaces
    // the whole resource. No SceneData or GPU object is owned by this snapshot.
    std::shared_ptr<const VisionGeometrySnapshot> geometry_snapshot;
    std::uint64_t geometry_version{0};
    std::uint64_t next_geometry_identity{1};
    std::string overlay_path;
    std::string overlay_guid;
    std::shared_ptr<::vision::SceneData> logical_scene;
    std::shared_ptr<::vision::GeometryGpuResource> scene_gpu_resource;
    std::uint64_t logical_transform_version{0};
    std::uint64_t scene_gpu_transform_version{0};
    std::unordered_map<std::uintptr_t, std::size_t> external_live_transform_signatures;
    std::uint64_t external_live_cache_generation{0};
    std::unordered_map<std::uintptr_t, std::size_t>
        external_live_original_transform_signatures;
    std::unordered_map<VisionLogicalInstanceKey,
                       VisionLogicalInstanceRecord,
                       VisionLogicalInstanceKeyHash>
        logical_instances;
    std::unordered_map<VisionLogicalInstanceKey,
                       VisionLogicalInstanceRecord,
                       VisionLogicalInstanceKeyHash>
        external_live_original_instances;
    std::unordered_map<std::uintptr_t, ExternalLiveShapeRecord>
        external_live_shapes_by_actor;
    // Engine-native actors mixed into this ExternalLive scene (no binding).
    std::unordered_map<std::uintptr_t, EngineMixedShapeRecord>
        engine_mixed_shapes_by_actor;

    [[nodiscard]] bool is_embedded() const noexcept {
        return source_desc && source_desc->kind == VisionSceneSourceKind::Embedded;
    }

    [[nodiscard]] bool is_external_live() const noexcept {
        return key.source == VisionPipelineSource::ExternalLive;
    }

    [[nodiscard]] bool has_scene_gpu_resource() const noexcept {
        return scene_gpu_resource != nullptr;
    }

    [[nodiscard]] bool has_logical_scene() const noexcept {
        return logical_scene != nullptr;
    }

    [[nodiscard]] std::uintptr_t logical_scene_identity() const noexcept {
        return reinterpret_cast<std::uintptr_t>(logical_scene.get());
    }

    [[nodiscard]] std::uintptr_t scene_gpu_resource_identity() const noexcept {
        return reinterpret_cast<std::uintptr_t>(scene_gpu_resource.get());
    }

    void set_logical_scene(std::shared_ptr<::vision::SceneData> scene) noexcept {
        logical_scene = std::move(scene);
    }

    std::shared_ptr<::vision::SceneData> ensure_logical_scene(
        const std::function<std::shared_ptr<::vision::SceneData>()>& factory) {
        if (!logical_scene) {
            logical_scene = factory();
        }
        return logical_scene;
    }

    void set_scene_gpu_resource(
        std::shared_ptr<::vision::GeometryGpuResource> resource) noexcept {
        scene_gpu_resource = std::move(resource);
    }

    void reset_loaded_scene() noexcept {
        logical_scene.reset();
        scene_gpu_resource.reset();
        logical_transform_version = 0;
        scene_gpu_transform_version = 0;
        external_live_transform_signatures.clear();
        ++external_live_cache_generation;
        external_live_original_transform_signatures.clear();
        logical_instances.clear();
        external_live_original_instances.clear();
        external_live_shapes_by_actor.clear();
        engine_mixed_shapes_by_actor.clear();
    }

    std::shared_ptr<::vision::GeometryGpuResource> ensure_scene_gpu_resource(
        const std::function<std::shared_ptr<::vision::GeometryGpuResource>()>& factory) {
        if (!scene_gpu_resource) {
            scene_gpu_resource = factory();
        }
        return scene_gpu_resource;
    }

    void mark_transforms_changed() noexcept {
        ++logical_transform_version;
    }

    [[nodiscard]] bool scene_gpu_needs_transform_upload() const noexcept {
        return scene_gpu_transform_version != logical_transform_version;
    }

    void mark_scene_gpu_transforms_uploaded() noexcept {
        scene_gpu_transform_version = logical_transform_version;
    }

    [[nodiscard]] std::size_t logical_instance_count() const noexcept {
        return logical_instances.size();
    }

    [[nodiscard]] const VisionLogicalInstanceRecord* find_logical_instance(
        const VisionLogicalInstanceKey& key) const noexcept {
        const auto iter = logical_instances.find(key);
        return iter == logical_instances.end() ? nullptr : &iter->second;
    }

    bool upsert_logical_instance(VisionLogicalInstanceRecord record) {
        const auto key = record.key;
        auto [iter, inserted] = logical_instances.try_emplace(key, std::move(record));
        if (inserted) {
            return true;
        }
        if (iter->second.actor_handle == record.actor_handle &&
            iter->second.transform_signature == record.transform_signature &&
            iter->second.object_to_world == record.object_to_world) {
            return false;
        }
        iter->second = std::move(record);
        return true;
    }

    void replace_logical_instances(std::vector<VisionLogicalInstanceRecord> records) {
        ++external_live_cache_generation;
        logical_instances.clear();
        for (auto& record : records) {
            logical_instances.emplace(record.key, std::move(record));
        }
    }

    bool cache_external_live_original_instance(VisionLogicalInstanceRecord record) {
        const auto key = record.key;
        auto [iter, inserted] =
            external_live_original_instances.try_emplace(key, std::move(record));
        (void)iter;
        return inserted;
    }

    [[nodiscard]] std::vector<VisionLogicalInstanceRecord>
    restore_external_live_original_instances(int shape_index) const {
        std::vector<VisionLogicalInstanceRecord> records;
        if (shape_index < 0) {
            return records;
        }
        for (const auto& [key, record] : external_live_original_instances) {
            if (key.shape_index == shape_index) {
                records.push_back(record);
            }
        }
        std::sort(records.begin(), records.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.key.instance_index < rhs.key.instance_index;
        });
        return records;
    }

    [[nodiscard]] const ExternalLiveShapeRecord* find_external_live_shape(
        std::uintptr_t actor_handle) const noexcept {
        const auto iter = external_live_shapes_by_actor.find(actor_handle);
        return iter == external_live_shapes_by_actor.end() ? nullptr : &iter->second;
    }

    bool upsert_external_live_shape(ExternalLiveShapeRecord record) {
        if (record.actor_handle == 0 || record.shape_index < 0) {
            return false;
        }
        const auto actor_handle = record.actor_handle;
        auto iter = external_live_shapes_by_actor.find(actor_handle);
        if (iter == external_live_shapes_by_actor.end()) {
            external_live_shapes_by_actor.emplace(actor_handle, std::move(record));
            return true;
        }
        if (iter->second.shape_index == record.shape_index &&
            iter->second.shape_guid == record.shape_guid &&
            iter->second.shape_identity_key == record.shape_identity_key &&
            iter->second.dynamically_added == record.dynamically_added) {
            return false;
        }
        iter->second = std::move(record);
        return true;
    }

    void erase_external_live_shape(std::uintptr_t actor_handle) noexcept {
        const auto iter = external_live_shapes_by_actor.find(actor_handle);
        if (iter != external_live_shapes_by_actor.end()) {
            const auto shape_index = iter->second.shape_index;
            for (auto instance_iter = external_live_original_instances.begin();
                 instance_iter != external_live_original_instances.end();) {
                if (instance_iter->second.actor_handle == actor_handle ||
                    instance_iter->first.shape_index == shape_index) {
                    instance_iter = external_live_original_instances.erase(instance_iter);
                } else {
                    ++instance_iter;
                }
            }
        }
        external_live_shapes_by_actor.erase(actor_handle);
        ++external_live_cache_generation;
        external_live_transform_signatures.erase(actor_handle);
        external_live_original_transform_signatures.erase(actor_handle);
    }

    [[nodiscard]] const EngineMixedShapeRecord* find_engine_mixed_shape(
        std::uintptr_t actor_handle) const noexcept {
        const auto iter = engine_mixed_shapes_by_actor.find(actor_handle);
        return iter == engine_mixed_shapes_by_actor.end() ? nullptr : &iter->second;
    }

    void erase_engine_mixed_shape(std::uintptr_t actor_handle) noexcept {
        engine_mixed_shapes_by_actor.erase(actor_handle);
    }

    [[nodiscard]] std::vector<std::uintptr_t> remap_external_live_shape_indices_after_remove(
        int removed_shape_index) {
        std::vector<std::uintptr_t> actors_to_rewrite;
        if (removed_shape_index < 0) {
            return actors_to_rewrite;
        }
        ++external_live_cache_generation;

        std::vector<VisionLogicalInstanceRecord> remapped_logical_instances;
        remapped_logical_instances.reserve(logical_instances.size());
        for (auto& [key, existing_record] : logical_instances) {
            (void)key;
            auto record = std::move(existing_record);
            if (record.key.shape_index == removed_shape_index) {
                continue;
            }
            if (record.key.shape_index > removed_shape_index) {
                --record.key.shape_index;
            }
            remapped_logical_instances.push_back(std::move(record));
        }
        logical_instances.clear();
        for (auto& record : remapped_logical_instances) {
            logical_instances.emplace(record.key, std::move(record));
        }

        std::vector<VisionLogicalInstanceRecord> remapped_external_live_original_instances;
        remapped_external_live_original_instances.reserve(external_live_original_instances.size());
        for (auto& [key, existing_record] : external_live_original_instances) {
            (void)key;
            auto record = std::move(existing_record);
            if (record.key.shape_index == removed_shape_index) {
                continue;
            }
            if (record.key.shape_index > removed_shape_index) {
                --record.key.shape_index;
            }
            remapped_external_live_original_instances.push_back(std::move(record));
        }
        external_live_original_instances.clear();
        for (auto& record : remapped_external_live_original_instances) {
            external_live_original_instances.emplace(record.key, std::move(record));
        }

        for (auto iter = external_live_shapes_by_actor.begin();
             iter != external_live_shapes_by_actor.end();) {
            auto& record = iter->second;
            if (record.shape_index == removed_shape_index) {
                external_live_transform_signatures.erase(iter->first);
                external_live_original_transform_signatures.erase(iter->first);
                iter = external_live_shapes_by_actor.erase(iter);
                continue;
            }
            if (record.shape_index > removed_shape_index) {
                --record.shape_index;
                external_live_transform_signatures.erase(iter->first);
                external_live_original_transform_signatures.erase(iter->first);
                actors_to_rewrite.push_back(iter->first);
            }
            ++iter;
        }

        // Engine-native mixed shapes index into the same groups_ vector, so they
        // must be remapped too. They have no binding to write back, so they are
        // NOT added to actors_to_rewrite; their index is fixed in place and the
        // transform signature is zeroed to force a re-apply at the new index.
        // (Their per-instance entries in logical_instances were already remapped
        // by the loop above.)
        for (auto iter = engine_mixed_shapes_by_actor.begin();
             iter != engine_mixed_shapes_by_actor.end();) {
            auto& record = iter->second;
            if (record.shape_index == removed_shape_index) {
                iter = engine_mixed_shapes_by_actor.erase(iter);
                continue;
            }
            if (record.shape_index > removed_shape_index) {
                --record.shape_index;
                record.transform_signature = 0;
            }
            ++iter;
        }

        return actors_to_rewrite;
    }
};

[[nodiscard]] inline std::string_view vision_resource_ownership_name(
    VisionResourceOwnership ownership) noexcept {
    switch (ownership) {
        case VisionResourceOwnership::SharedLogicalScene:
            return "shared_logical_scene";
        case VisionResourceOwnership::SharedSceneGpu:
            return "shared_scene_gpu";
        case VisionResourceOwnership::PerPipelineRenderState:
            return "per_pipeline_render_state";
        case VisionResourceOwnership::LegacyPipelineOwned:
            return "legacy_pipeline_owned";
    }
    return "legacy_pipeline_owned";
}

}  // namespace Corona::Systems::Vision
