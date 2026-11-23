#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vision::restir_gi {

// Scalar equations shared by the device adapter and executable CPU oracle.
// Reference: RestirGI shaders/reservoir.hlsl, restirTemporalCS.hlsl,
// restirSpatialCS.hlsl, RtdgiValidateRgen.hlsl (cache-disabled diffuse route).
struct HostOps {
    using Scalar = float;
    static float min(float a, float b) { return std::min(a, b); }
    static float max(float a, float b) { return std::max(a, b); }
    static float clamp(float x, float a, float b) { return std::clamp(x, a, b); }
    static float sqrt(float x) { return std::sqrt(x); }
    static float abs(float x) { return std::abs(x); }
    static float pow(float x, float y) { return std::pow(x, y); }
    static float safe(float x) { return std::isfinite(x) ? x : 0.f; }
};

template<typename O> struct Math {
    using T = typename O::Scalar;
    template<typename V> static auto interpolate(const V &start, const V &end, T fraction) {
        return start + (end - start) * fraction;
    }
    static T spatial_radius(unsigned pass, T count, unsigned scale) {
        T max_count = T{pass == 0u ? 20.f : 160.f};
        T t = O::clamp((count - max_count * T{.5f}) / (max_count * T{.5f}), T{0.f}, T{1.f});
        T tightness = T{.5f} * (t * t * (T{3.f} - T{2.f} * t));
        T start = T{pass == 0u ? 32.f : 16.f};
        T end = T{pass == 0u ? 12.f : 6.f};
        return interpolate(start, end, tightness) * T{2.f / float(scale)};
    }
    static T finish_weight(T sum, T count, T target) {
        return O::clamp(O::safe(sum / O::max(T{1e-8f}, count * target)), T{0.f}, T{10.f});
    }
    static T temporal_count(T count, T raw_invalidity, T relevance) {
        T invalidity = O::sqrt(O::clamp(raw_invalidity, T{0.f}, T{1.f}));
        return O::max(T{0.f}, O::min(count, O::pow(T{20.f}, T{1.f} - invalidity))) * relevance;
    }
    static T spatial_jacobian(T source_distance, T target_distance, T source_cos, T target_cos) {
        T ratio = O::safe(source_distance / target_distance);
        T cosine = O::clamp(O::safe(target_cos / source_cos), T{0.f}, T{1e4f});
        return O::sqrt(ratio * ratio * cosine);
    }
    static T temporal_jacobian(T source_distance, T target_distance, T source_cos, T target_cos) {
        T ratio = O::clamp(O::safe(source_distance / target_distance), T{1e-4f}, T{1e4f});
        return ratio * ratio * O::clamp(O::safe(target_cos / source_cos), T{0.f}, T{1e4f});
    }
    static T validation_m_scale(T old_luminance, T new_luminance) {
        return O::clamp(old_luminance / O::max(T{1e-8f}, new_luminance), T{.03f}, T{1.f});
    }
    static T validation_w_scale(T old_luminance, T new_luminance) {
        return O::clamp(T{10.f} * old_luminance / O::max(T{1e-8f}, new_luminance), T{.01f}, T{1.f});
    }
    static T hemisphere_factor(T cosine) { return T{2.f} * O::max(T{0.f}, cosine); }
    static T inverse_depth_relative_difference(T primary_linear, T secondary_linear) {
        return O::abs(secondary_linear / O::max(primary_linear,T{1e-20f}) - T{1.f});
    }
};

inline unsigned sampling_extent(unsigned extent, unsigned scale) noexcept { return (extent + scale - 1u) / scale; }
inline bool consecutive_history(unsigned previous, unsigned frame) noexcept {
    return previous != 0xffffffffu && frame > 0u && previous + 1u == frame;
}
} // namespace vision::restir_gi
