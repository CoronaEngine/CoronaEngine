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

// Test mesh-local bounds against the light clip planes, including the full
// actor/node transform. Unknown bounds are retained; a camera frustum must not
// be used here because offscreen geometry can still cast visible shadows.
class ShadowMeshFrustum {
public:
    explicit ShadowMeshFrustum(const ktm::fmat4x4& light_clip) noexcept {
        std::array<std::array<float, 4>, 4> rows;
        for (std::size_t row = 0; row < 4; ++row) {
            for (std::size_t column = 0; column < 4; ++column) {
                const float value = light_clip[column][row];
                rows[row][column] = value;
                absolute_rows_[row][column] = std::abs(value);
                valid_ = valid_ && std::isfinite(value);
            }
        }
        for (std::size_t column = 0; column < 4; ++column) {
            planes_[0][column] = rows[3][column] + rows[0][column];
            planes_[1][column] = rows[3][column] - rows[0][column];
            planes_[2][column] = rows[3][column] + rows[1][column];
            planes_[3][column] = rows[3][column] - rows[1][column];
            planes_[4][column] = rows[2][column];
            planes_[5][column] = rows[3][column] - rows[2][column];
            for (const auto& plane : planes_) valid_ = valid_ && std::isfinite(plane[column]);
        }
    }

    [[nodiscard]] bool visible(const std::array<float, 3>& minimum,
                               const std::array<float, 3>& maximum, bool bounds_valid) const noexcept {
        if (!valid_ || !bounds_valid) return true;
        std::array<float, 3> magnitude;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) ||
                minimum[axis] > maximum[axis]) return true;
            magnitude[axis] = std::max(std::abs(minimum[axis]), std::abs(maximum[axis]));
        }
        // Bound every clip coordinate, including cancellation in large world
        // transforms. The tolerance can retain extra triangles, never remove a
        // caster because of rounding at a clip boundary.
        float scale = 1.0f;
        for (const auto& row : absolute_rows_) {
            const float upper = row[0] * magnitude[0] + row[1] * magnitude[1] +
                                row[2] * magnitude[2] + row[3];
            if (!std::isfinite(upper)) return true;
            scale = std::max(scale, upper);
        }
        const float epsilon = 1.0e-5f * scale;
        for (const auto& plane : planes_) {
            float upper = plane[3];
            for (std::size_t axis = 0; axis < 3; ++axis)
                upper += plane[axis] * (plane[axis] >= 0.0f ? maximum[axis] : minimum[axis]);
            if (!std::isfinite(upper)) return true;
            if (upper < -epsilon) return false;
        }
        return true;
    }

private:
    std::array<std::array<float, 4>, 6> planes_{};
    std::array<std::array<float, 4>, 4> absolute_rows_{};
    bool valid_{true};
};

[[nodiscard]] inline bool shadow_mesh_visible(
    const ktm::fmat4x4& light_clip, const std::array<float, 3>& minimum,
    const std::array<float, 3>& maximum, bool bounds_valid) noexcept {
    return ShadowMeshFrustum(light_clip).visible(minimum, maximum, bounds_valid);
}

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
