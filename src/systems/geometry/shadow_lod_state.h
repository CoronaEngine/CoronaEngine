#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <limits>

namespace Corona::Systems::GeometryDetail {

constexpr std::uint64_t kShadowLodRequestTtlFrames = 120;

// Upper bound of the linear transform's largest singular value. Includes
// normalization, reflections and shear, without allocating or taking an SVD.
template <typename Matrix>
[[nodiscard]] inline float shadow_transform_scale(const Matrix& matrix) noexcept {
    float one_norm = 0.0f, infinity_norm = 0.0f;
    for (int i = 0; i < 3; ++i) {
        float column = 0.0f, row = 0.0f;
        for (int j = 0; j < 3; ++j) {
            if (!std::isfinite(matrix[i][j])) return std::numeric_limits<float>::quiet_NaN();
            column += std::abs(matrix[i][j]);
            row += std::abs(matrix[j][i]);
        }
        one_norm = std::max(one_norm, column);
        infinity_norm = std::max(infinity_norm, row);
    }
    return std::sqrt(one_norm * infinity_norm);
}

struct ShadowLodState {
    int committed = -1;
    int previous = -1;
    bool swap_in_progress = false;
    std::uint64_t last_request_frame = 0;
};

struct ShadowLodDecision {
    int target = -1;
    bool expired = false;
    bool needs_build = false;
    int main_level = 0;
    int main_previous = -1;
    int shadow_previous = -1;

    [[nodiscard]] bool keep_level(int level) const noexcept {
        return level == 0 || level == main_level || level == main_previous ||
               level == target || level == shadow_previous;
    }
};

struct ShadowLodQueryInput {
    bool enabled = false;
    float world_units_per_texel = 0.0f;
};

struct ShadowLodBatchDecision {
    int aggregated_demand = -1;
};

[[nodiscard]] inline int choose_shadow_target(const std::array<float, 8>& errors,
                                              int level_count,
                                              float max_abs_scale,
                                              float texel,
                                              int fallback) noexcept {
    if (level_count <= 0 || !std::isfinite(texel) || texel <= 0.0f ||
        !std::isfinite(max_abs_scale)) {
        return fallback;
    }
    int target = std::clamp(fallback, 0, level_count - 1);
    const float scale = std::max(std::abs(max_abs_scale), 1.0e-6f);
    for (int i = 0; i < level_count; ++i) {
        const float error = errors[static_cast<std::size_t>(i)] * scale;
        if (std::isfinite(error) && error <= texel) {
            target = i;
        }
    }
    return target;
}

[[nodiscard]] inline ShadowLodBatchDecision choose_shadow_targets(
    const std::array<float, 8>& errors,
    int level_count,
    float max_abs_scale,
    std::span<const ShadowLodQueryInput> queries,
    std::span<int> targets,
    int fallback) noexcept {
    ShadowLodBatchDecision result;
    const std::size_t count = std::min(queries.size(), targets.size());
    const int safe_fallback = level_count > 0
        ? std::clamp(fallback, 0, level_count - 1)
        : fallback;
    for (std::size_t i = 0; i < count; ++i) {
        if (!queries[i].enabled) {
            targets[i] = -1;
            continue;
        }
        const bool valid = level_count > 0 &&
                           std::isfinite(queries[i].world_units_per_texel) &&
                           queries[i].world_units_per_texel > 0.0f &&
                           std::isfinite(max_abs_scale);
        if (!valid) {
            targets[i] = safe_fallback;
            continue;
        }
        targets[i] = choose_shadow_target(errors, level_count, max_abs_scale,
                                          queries[i].world_units_per_texel,
                                          safe_fallback);
        result.aggregated_demand = result.aggregated_demand < 0
            ? targets[i]
            : std::min(result.aggregated_demand, targets[i]);
    }
    return result;
}

[[nodiscard]] inline ShadowLodDecision decide_shadow_lod(
    const ShadowLodState& state,
    std::uint64_t frame,
    int main_level,
    int main_previous,
    int level_count) noexcept {
    ShadowLodDecision decision;
    decision.target = state.committed;
    decision.main_level = main_level;
    decision.main_previous = main_previous;
    decision.shadow_previous = state.previous;
    if (state.committed >= 0 && frame > state.last_request_frame &&
        frame - state.last_request_frame > kShadowLodRequestTtlFrames) {
        decision.target = -1;
        decision.expired = true;
    }
    if (decision.target >= level_count) {
        decision.target = level_count - 1;
    }
    decision.needs_build = decision.target >= 1;
    return decision;
}

}  // namespace Corona::Systems::GeometryDetail
