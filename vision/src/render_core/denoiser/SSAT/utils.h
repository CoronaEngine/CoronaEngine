//
// Created by Zero on 2025/01/26.
// SSAT Utility Functions and Data Structures
//

#pragma once

#include <algorithm>

#include "math/basic_types.h"
#include "dsl/dsl.h"
#include "base/mgr/global.h"
#include "ssat_config.h"

namespace vision::ssat {
using namespace ocarina;

// ============================================================================
// Buffer Initialization Utilities
// ============================================================================

template<typename T>
inline void init_buffer_zero(Device &dev, Buffer<T> &buffer, uint num, const string &desc = "") {
    buffer = dev.create_buffer<T>(num, desc);
    vector<T> vec(num, T{});
    buffer.upload_immediately(vec.data());
}

// ============================================================================
// SSAT Per-Pixel Data Structure
// ============================================================================

/// Per-subpixel data for temporal accumulation, variance, and viewer support.
struct SSATData {
    float4 radiance_accum{};   // RGB accumulated radiance + variance in .w
    float4 moments_history{};  // M1, M2, history_count, support_validity
};

[[nodiscard]] inline float ssat_final_support_validity(
    float current_support,
    float history_support,
    bool use_history) noexcept {
    return std::clamp(
        std::max(current_support, use_history ? history_support : 0.f),
        0.f,
        1.f);
}

}// namespace vision::ssat

// GPU-side structure definition
OC_STRUCT(vision::ssat, SSATData, radiance_accum, moments_history) {
    /// Get accumulated radiance RGB
    [[nodiscard]] Float3 radiance() const noexcept { return radiance_accum.xyz(); }
    /// Get variance estimate
    [[nodiscard]] Float variance() const noexcept { return radiance_accum.w; }
    /// Get first moment (mean luminance)
    [[nodiscard]] Float first_moment() const noexcept { return moments_history.x; }
    /// Get second moment
    [[nodiscard]] Float second_moment() const noexcept { return moments_history.y; }
    /// Get history count for EMA
    [[nodiscard]] Float history_count() const noexcept { return moments_history.z; }
    /// Get SSAT reconstruction support for the final-view diagnostic.
    [[nodiscard]] Float support_validity() const noexcept { return moments_history.w; }
};

namespace vision::ssat {
using SSATDataVar = Var<SSATData>;

[[nodiscard]] inline Float ssat_final_support_validity(
    const Float &current_support,
    const Float &history_support,
    const Bool &use_history) noexcept {
    return saturate(max(
        current_support,
        select(use_history, history_support, 0.f)));
}

// ============================================================================
// Pixel Index Utilities
// ============================================================================

/// Safe pixel index computation with boundary clamping
[[nodiscard]] inline Uint safe_pixel_index(const Int2 &pixel, const Int2 &screen_size) noexcept {
    Int2 clamped = clamp(pixel, make_int2(0), screen_size - 1);
    return cast<uint>(clamped.y) * cast<uint>(screen_size.x) + cast<uint>(clamped.x);
}

/// Compute subpixel linear index from dispatch coordinates
/// For light field: dx = x*3 + k, dy = y, so linear_idx = dy * (res_w * 3) + dx
[[nodiscard]] inline Uint subpixel_linear_index(const Uint2 &dispatch_idx, const Float &res_w) noexcept {
    Uint res_w_3 = cast<uint>(res_w) * 3u;
    return dispatch_idx.y * res_w_3 + dispatch_idx.x;
}

/// Decode subpixel index to (pixel_x, pixel_y, channel_k)
/// dx = x*3 + k, so x = dx/3, k = dx%3
[[nodiscard]] inline void decode_subpixel(const Uint2 &dispatch_idx, 
                                          Uint &pixel_x, Uint &pixel_y, Uint &channel_k) noexcept {
    pixel_x = dispatch_idx.x / 3u;
    channel_k = dispatch_idx.x % 3u;
    pixel_y = dispatch_idx.y;
}

/// Encode pixel coordinates and channel to dispatch index
[[nodiscard]] inline Uint2 encode_subpixel(const Uint &pixel_x, const Uint &pixel_y, const Uint &channel_k) noexcept {
    return make_uint2(pixel_x * 3u + channel_k, pixel_y);
}

// ============================================================================
// Weight Computation Utilities
// ============================================================================

/// Compute luminance from RGB
[[nodiscard]] inline Float compute_luminance(const Float3 &color) noexcept {
    return luminance(color);
}

/// Luminance-based edge-stopping weight
[[nodiscard]] inline Float weight_luminance(const Float &lum_center, const Float &lum_neighbor, 
                                            const Float &sigma) noexcept {
    return exp(-abs(lum_center - lum_neighbor) / max(sigma, SSATConfig::Gather::kEpsilon));
}

/// Normal-based edge-stopping weight
[[nodiscard]] inline Float weight_normal(const Float3 &n1, const Float3 &n2, const Float &sigma) noexcept {
    Float cos_angle = max(dot(n1, n2), 0.f);
    return pow(cos_angle, sigma);
}

/// Depth-based edge-stopping weight (standard bilateral)
[[nodiscard]] inline Float weight_depth(const Float &z1, const Float &z2, const Float &sigma) noexcept {
    Float diff = abs(z1 - z2) / max(z1, 0.1f);
    return exp(-diff / max(sigma, SSATConfig::Gather::kEpsilon));
}

/// Spatio-Angular Geometric Weight w_geo (paper Eq. for manifold consistency)
/// w_geo = exp(-|z(p) - z_q| * (1 + β||Δu||) / σ_z)
[[nodiscard]] inline Float weight_spatio_angular_geo(const Float &z_center, const Float &z_neighbor,
                                                      const Float &angular_baseline,
                                                      const Float &sigma_z,
                                                      const Float &beta = SSATConfig::SpatialAngular::kBetaAngular) noexcept {
    Float depth_diff = abs(z_center - z_neighbor);
    Float angular_penalty = 1.f + beta * angular_baseline;
    return exp(-depth_diff * angular_penalty / max(sigma_z, SSATConfig::Gather::kEpsilon));
}

/// B-spline à-trous kernel weight
[[nodiscard]] inline Float bspline_weight(const Int &offset) noexcept {
    Int abs_off = abs(offset);
    return ocarina::select(abs_off == 0, SSATConfig::SpatialAngular::kBSpline1D[0],
           ocarina::select(abs_off == 1, SSATConfig::SpatialAngular::kBSpline1D[1],
                                         SSATConfig::SpatialAngular::kBSpline1D[2]));
}

// ============================================================================
// Disparity and Shear Computation
// ============================================================================

/// Compute signed disparity δ(z) = 1/z - 1/z_ref (paper Eq. for EPI shear)
[[nodiscard]] inline Float compute_disparity(const Float &depth, const Float &z_ref) noexcept {
    Float safe_depth = max(depth, 0.001f);
    Float safe_z_ref = max(z_ref, 0.001f);
    return 1.f / safe_depth - 1.f / safe_z_ref;
}

/// Compute shear magnitude λ(p) = |δ(z)|
[[nodiscard]] inline Float compute_shear_magnitude(const Float &depth, const Float &z_ref) noexcept {
    return abs(compute_disparity(depth, z_ref));
}

/// Transfer function W mapping shear to probability gain
/// Piecewise linear ramp: W(λ) = clamp(λ * scale, 0, 1)
[[nodiscard]] inline Float shear_transfer_function(const Float &shear_magnitude) noexcept {
    return saturate(shear_magnitude * SSATConfig::Sampling::kShearTransferScale);
}

/// Compute adaptive sampling probability P_t(p) = clamp(ρ_base + α*W(λ), 0, 1)
[[nodiscard]] inline Float compute_sampling_probability(const Float &shear_magnitude,
                                                         const Float &rho_base,
                                                         const Float &alpha) noexcept {
    Float w = shear_transfer_function(shear_magnitude);
    return saturate(rho_base + alpha * w);
}

// ============================================================================
// Blue Noise / Stochastic Dithering
// ============================================================================

/// Simple hash-based pseudo-random for stochastic dithering
/// Approximates spatio-temporal blue noise B(p, t)
[[nodiscard]] inline Float stochastic_dither(const Uint2 &pixel, const Uint &frame_index) noexcept {
    // Simple hash combining spatial and temporal components
    Uint h = pixel.x * 73856093u ^ pixel.y * 19349663u ^ frame_index * 83492791u;
    h = h ^ (h >> 16u);
    h = h * 0x85ebca6bu;
    h = h ^ (h >> 13u);
    h = h * 0xc2b2ae35u;
    h = h ^ (h >> 16u);
    return cast<float>(h) / cast<float>(0xFFFFFFFFu);
}

}// namespace vision::ssat
