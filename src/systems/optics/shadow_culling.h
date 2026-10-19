#pragma once

#include <corona/math/frustum.h>
#include <corona/spatial/aabb.h>
#include <ktm/ktm.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace Corona::Systems::OpticsDetail {

struct ShadowCascadeView {
    ktm::fmat4x4 light_view_proj{ktm::fmat4x4::from_eye()};
    Math::Frustum frustum{};
    float orthographic_width = 0.0f;
    float orthographic_height = 0.0f;
    float world_units_per_texel = 0.0f;
};

struct ShadowCasterBoundsSnapshot {
    Spatial::AABB world_bounds{};
    bool valid = false;
};

struct ShadowSceneBounds {
    bool valid = false;
    ktm::fvec3 min_world{0.0f, 0.0f, 0.0f};
    ktm::fvec3 max_world{0.0f, 0.0f, 0.0f};
};

[[nodiscard]] inline ShadowCascadeView make_shadow_cascade_view(
    const ktm::fmat4x4& light_view_proj,
    float orthographic_width,
    float orthographic_height,
    float shadow_map_size) noexcept {
    ShadowCascadeView result;
    result.light_view_proj = light_view_proj;
    result.frustum = Math::Frustum::from_view_proj(light_view_proj);
    result.orthographic_width = orthographic_width;
    result.orthographic_height = orthographic_height;
    const float extent = std::max(orthographic_width, orthographic_height);
    result.world_units_per_texel = (extent > 0.0f && shadow_map_size > 0.0f)
        ? extent / shadow_map_size : 0.0f;
    return result;
}

[[nodiscard]] inline bool shadow_caster_visible(
    const Math::Frustum& frustum, const Spatial::AABB& world_bounds,
    bool bounds_valid = true) noexcept {
    if (!bounds_valid || !std::isfinite(world_bounds.min.x) ||
        !std::isfinite(world_bounds.min.y) || !std::isfinite(world_bounds.min.z) ||
        !std::isfinite(world_bounds.max.x) || !std::isfinite(world_bounds.max.y) ||
        !std::isfinite(world_bounds.max.z) ||
        world_bounds.min.x > world_bounds.max.x ||
        world_bounds.min.y > world_bounds.max.y ||
        world_bounds.min.z > world_bounds.max.z) {
        return true;
    }
    return frustum.intersects(world_bounds);
}

[[nodiscard]] inline std::uint32_t shadow_cascade_visibility_mask(
    const ShadowCasterBoundsSnapshot& caster,
    std::span<const ShadowCascadeView> cascades,
    std::uint32_t enabled_mask) noexcept {
    std::uint32_t visible_mask = 0;
    const std::size_t count = std::min<std::size_t>(cascades.size(), 32);
    for (std::size_t cascade = 0; cascade < count; ++cascade) {
        const std::uint32_t bit = 1u << cascade;
        if ((enabled_mask & bit) == 0u) continue;
        if (shadow_caster_visible(cascades[cascade].frustum,
                                  caster.world_bounds,
                                  caster.valid)) {
            visible_mask |= bit;
        }
    }
    return visible_mask;
}

inline void include_shadow_scene_bounds(
    ShadowSceneBounds& scene,
    const ShadowCasterBoundsSnapshot& caster) noexcept {
    if (!caster.valid || !caster.world_bounds.valid()) return;
    if (!scene.valid) {
        scene.min_world = caster.world_bounds.min;
        scene.max_world = caster.world_bounds.max;
        scene.valid = true;
        return;
    }
    scene.min_world.x = std::min(scene.min_world.x, caster.world_bounds.min.x);
    scene.min_world.y = std::min(scene.min_world.y, caster.world_bounds.min.y);
    scene.min_world.z = std::min(scene.min_world.z, caster.world_bounds.min.z);
    scene.max_world.x = std::max(scene.max_world.x, caster.world_bounds.max.x);
    scene.max_world.y = std::max(scene.max_world.y, caster.world_bounds.max.y);
    scene.max_world.z = std::max(scene.max_world.z, caster.world_bounds.max.z);
}

template <typename Callback>
inline void for_each_enabled_shadow_cascade(std::uint32_t enabled_mask,
                                            std::size_t cascade_count,
                                            Callback&& callback) {
    const std::size_t count = std::min<std::size_t>(cascade_count, 32);
    for (std::size_t cascade = 0; cascade < count; ++cascade) {
        if ((enabled_mask & (1u << cascade)) != 0u) {
            callback(static_cast<std::uint32_t>(cascade));
        }
    }
}

[[nodiscard]] inline int select_shadow_lod_level(
    const std::array<float, 8>& geometric_errors,
    int level_count, float max_abs_scale, float world_units_per_texel,
    int fallback_level) noexcept {
    if (level_count <= 0 || world_units_per_texel <= 0.0f ||
        !std::isfinite(world_units_per_texel)) return fallback_level;
    const float scale = std::max(std::abs(max_abs_scale), 1.0e-6f);
    int result = std::max(0, std::min(fallback_level, level_count - 1));
    for (int level = 0; level < level_count; ++level) {
        const float error = geometric_errors[static_cast<size_t>(level)] * scale;
        if (std::isfinite(error) && error <= world_units_per_texel) result = level;
    }
    return result;
}

}  // namespace Corona::Systems::OpticsDetail
