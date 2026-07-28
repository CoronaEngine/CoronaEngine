//
// Created by Zero on 2025/01/26.
// Common Light Field Types - shared between LightFieldFrameBuffer, SSAT denoiser, etc.
//

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "math/basic_types.h"
#include "dsl/dsl.h"

namespace vision {
using namespace ocarina;

// ============================================================================
// Light Field Parameter Structures
// ============================================================================

/// Lenticular interlacing parameters (from LightFieldArray.js)
/// Note: Using float for all parameters to ensure compatibility with OC_STRUCT macro
struct LenticularParams {
    float pe{19.1813f};    // Period
    float angle{0.2305f};  // Slant angle in radians
    float offset{14.1171f};// Offset
    float num_views{60.f}; // Number of views
    float res_w{32.f};     // Pixel resolution width
    float res_h{32.f};     // Pixel resolution height
};

/// Light field geometry parameters
struct LightFieldGeometry {
    float d_f{20.f};            // Focal distance (camera array to focal plane) = D_opt = z_ref
    float fov_h_deg{45.f};      // Horizontal FOV (degrees)
    float aspect{1.77f};        // Aspect ratio
    float array_angle_deg{30.f};// Camera array span angle (degrees)
    float W_f{0.f};             // Focal plane width (derived)
    float H_f{0.f};             // Focal plane height (derived)
};

/// The presentation source selected for a light-field framebuffer.
enum class LightFieldViewerMode : std::uint8_t {
    Interlaced,
    FinalView,
};

/// Sentinel used by runtime-only viewer state to request the default center view.
inline constexpr std::uint32_t kLightFieldAutoViewIndex =
    std::numeric_limits<std::uint32_t>::max();

/// Convert the serialized float view count into the integer count used by the
/// renderer. Invalid and sub-unit values still represent one usable view.
[[nodiscard]] inline std::uint32_t lightfield_view_count(float raw) noexcept {
    if (!std::isfinite(raw) || raw < 1.f) {
        return 1u;
    }
    const double floored = std::floor(static_cast<double>(raw));
    if (floored >= static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return std::max(1u, static_cast<std::uint32_t>(floored));
}

[[nodiscard]] inline std::uint32_t lightfield_effective_view_index(
    std::uint32_t requested,
    std::uint32_t count) noexcept {
    count = std::max(1u, count);
    if (requested == kLightFieldAutoViewIndex) {
        return (count - 1u) / 2u;
    }
    return std::min(requested, count - 1u);
}

[[nodiscard]] inline float lightfield_view_u(
    std::uint32_t index,
    std::uint32_t count) noexcept {
    count = std::max(1u, count);
    index = lightfield_effective_view_index(index, count);
    return count > 1u
               ? static_cast<float>(index) / static_cast<float>(count - 1u)
               : 0.5f;
}

[[nodiscard]] constexpr float lightfield_aspect_from_resolution(
    uint2 resolution) noexcept {
    return resolution.y == 0u
               ? 1.0f
               : static_cast<float>(resolution.x) /
                     static_cast<float>(resolution.y);
}

}// namespace vision

// GPU-side struct definitions
// Custom macro expansion that:
// 1. Does NOT use OC_MAKE_PARAM_STRUCT (allows Var<T> copy construction in DSL contexts)
// 2. Does NOT generate OC_MAKE_STRUCT_SOA_VAR/OC_MAKE_STRUCT_SOA_VIEW (avoids invalid TBuffer instantiation)
// This is a hybrid approach to get the best of both OC_STRUCT and OC_PARAM_STRUCT.
// clang-format off
OC_MAKE_STRUCT_REFLECTION(vision::LenticularParams, pe, angle, offset, num_views, res_w, res_h)
OC_MAKE_STRUCT_DESC(vision::LenticularParams, pe, angle, offset, num_views, res_w, res_h)
OC_MAKE_COMPUTABLE_BODY(vision::LenticularParams, pe, angle, offset, num_views, res_w, res_h)
OC_STRUCT_ALIAS(vision, LenticularParams)
OC_MAKE_PROXY(vision::LenticularParams) {};

OC_MAKE_STRUCT_REFLECTION(vision::LightFieldGeometry, d_f, fov_h_deg, aspect, array_angle_deg, W_f, H_f)
OC_MAKE_STRUCT_DESC(vision::LightFieldGeometry, d_f, fov_h_deg, aspect, array_angle_deg, W_f, H_f)
OC_MAKE_COMPUTABLE_BODY(vision::LightFieldGeometry, d_f, fov_h_deg, aspect, array_angle_deg, W_f, H_f)
OC_STRUCT_ALIAS(vision, LightFieldGeometry)
OC_MAKE_PROXY(vision::LightFieldGeometry) {};
// clang-format on

namespace vision {

// ============================================================================
// Light Field FrameBuffer Interface
// ============================================================================

/// Interface for accessing light field parameters from FrameBuffer
/// Implemented by LightFieldFrameBuffer, used by denoisers and integrators
class ILightFieldFrameBuffer {
public:
    virtual ~ILightFieldFrameBuffer() = default;
    
    /// Get lenticular interlacing parameters
    [[nodiscard]] virtual const LenticularParams& lenticular_params() const noexcept = 0;
    
    /// Get light field geometry parameters
    [[nodiscard]] virtual const LightFieldGeometry& geometry_params() const noexcept = 0;
    
    /// Get current local-to-world transform (includes camera transform)
    [[nodiscard]] virtual float4x4 get_current_l2w() const noexcept = 0;
    
    /// Get previous frame's local-to-world transform (for temporal reprojection)
    [[nodiscard]] virtual float4x4 get_prev_l2w() const noexcept = 0;

    /// Select the presentation source for this framebuffer's camera context.
    virtual void set_viewer_state(LightFieldViewerMode mode,
                                  std::uint32_t view_index) noexcept = 0;

    /// Get the currently selected presentation mode.
    [[nodiscard]] virtual LightFieldViewerMode viewer_mode() const noexcept = 0;

    /// Get the currently effective, clamped view index.
    [[nodiscard]] virtual std::uint32_t viewer_view_index() const noexcept = 0;

    /// Get the pixel-sized SSAT single-view output buffer.
    [[nodiscard]] virtual BufferView<float4> viewer_output_buffer() const noexcept = 0;

    /// Whether the pixel-sized viewer output has been allocated for this resolution.
    [[nodiscard]] virtual bool viewer_output_ready() const noexcept = 0;
};

}// namespace vision
