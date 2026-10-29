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

struct VisionSceneResource {
    std::shared_ptr<::vision::SceneData> logical_scene;
    std::shared_ptr<::vision::GeometryGpuResource> scene_gpu_resource;
    std::uint64_t logical_transform_version{0};
    std::uint64_t scene_gpu_transform_version{0};
    std::unordered_map<VisionLogicalInstanceKey,
                       VisionLogicalInstanceRecord,
                       VisionLogicalInstanceKeyHash>
        logical_instances;
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
        logical_instances.clear();
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
        logical_instances.clear();
        for (auto& record : records) {
            logical_instances.emplace(record.key, std::move(record));
        }
    }
};

}  // namespace Corona::Systems::Vision
