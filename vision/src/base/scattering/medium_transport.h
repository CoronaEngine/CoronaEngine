#pragma once

#include "base/using.h"
#include <cmath>
#include <stdexcept>

namespace vision {

using ocarina::RayTMax;

inline void validate_homogeneous_parameters(const float3 &sigma_a, const float3 &sigma_s, float scale, float g) {
    if (!std::isfinite(scale) || scale < 0.f || !std::isfinite(g) || std::abs(g) >= 1.f) {
        throw std::invalid_argument("Homogeneous medium requires finite scale >= 0 and abs(g) < 1");
    }
    for (uint axis = 0; axis < 3u; ++axis) {
        if (!std::isfinite(sigma_a[axis]) || sigma_a[axis] < 0.f ||
            !std::isfinite(sigma_s[axis]) || sigma_s[axis] < 0.f ||
            !std::isfinite((sigma_a[axis] + sigma_s[axis]) * scale)) {
            throw std::invalid_argument("Homogeneous medium requires finite nonnegative extinction coefficients");
        }
    }
}

// Ray parameters are invariant under an affine boundary transform. Extinction
// and sampled free flights, however, always use distance in world units.
[[nodiscard]] inline Float medium_world_distance(const Float &t, const Float &direction_length) {
    return max(t, 0.f) * direction_length;
}

[[nodiscard]] inline Float homogeneous_transmittance(const Float &sigma_t, const Float &distance) {
    return exp(-sigma_t * min(distance, RayTMax));
}

[[nodiscard]] inline Float homogeneous_sample_distance(const Float &sigma_t, const Float &u) {
    Float distance = RayTMax;
    $if(sigma_t > 0.f) {
        distance = -log(max(1.f - u, 1e-7f)) / sigma_t;
    };
    return distance;
}

[[nodiscard]] inline Float medium_phase_hg(const Float &cos_theta, const Float &g) {
    Float denom = 1.f + sqr(g) + 2.f * g * cos_theta;
    return Inv4Pi * (1.f - sqr(g)) / (denom * sqrt(denom));
}

// Slab intersection of the source cube [-1,1]^3. Handle parallel directions
// explicitly: 0 * infinity on a face must never turn an interval into NaN.
[[nodiscard]] inline Float2 medium_cube_interval(const Float3 &origin, const Float3 &direction) {
    Float entry = -RayTMax, exit = RayTMax;
    for (uint axis = 0; axis < 3u; ++axis) {
        $if(direction[axis] == 0.f) {
            $if(abs(origin[axis]) > 1.f) {
                entry = RayTMax;
                exit = -RayTMax;
            };
        }
        $else {
            Float a = (-1.f - origin[axis]) / direction[axis];
            Float b = (1.f - origin[axis]) / direction[axis];
            entry = max(entry, min(a, b));
            exit = min(exit, max(a, b));
        };
    }
    return make_float2(entry, exit);
}

[[nodiscard]] inline Uint medium_after_surface(const Uint &incident, const Uint &inside, const Uint &outside,
                                              const Bool &declared, const Float &cosine) {
    return ocarina::select(declared, ocarina::select(cosine > 0.f, outside, inside), incident);
}

}// namespace vision
