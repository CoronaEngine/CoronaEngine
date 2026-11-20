//
// Created by Zero on 2025/01/26.
// Phase 1: Disparity-Guided Adaptive Sampling
//
// Implements the adaptive sampling strategy that modulates per-frame
// sampling density based on transport geometry (shear magnitude).
//

#pragma once

#include "math/basic_types.h"
#include "dsl/dsl.h"
#include "base/mgr/global.h"
#include "base/mgr/pipeline.h"
#include "phase_space.h"
#include "utils.h"
#include "ssat_config.h"

namespace vision::ssat {
using namespace ocarina;

class SSAT;  // Forward declaration

// ============================================================================
// Adaptive Sampler Parameter Structure
// ============================================================================

struct AdaptiveSamplerParam {
    BufferDesc<TriangleHit> prev_visibility;  // Previous frame visibility for depth
    BufferDesc<uint> sampling_mask;           // Output: binary sampling mask M_t
    BufferDesc<float> shear_magnitude;        // Output: shear magnitude λ(p)
    array_float3 camera_pos{};                // Camera position for depth
    float rho_base{0.5f};                     // Baseline fill rate ρ_base
    float alpha{1.0f};                        // Sensitivity coefficient α
    float z_ref{20.0f};                       // ZPP depth (focal distance)
    uint frame_index{0};                      // For blue noise temporal offset
};

}// namespace vision::ssat

OC_PARAM_STRUCT(vision::ssat, AdaptiveSamplerParam, 
                prev_visibility, sampling_mask, shear_magnitude,
                camera_pos, rho_base, alpha, z_ref, frame_index){};

namespace vision::ssat {

// ============================================================================
// Adaptive Sampler Class
// ============================================================================

/// Phase 1: Disparity-Guided Adaptive Sampling
/// 
/// Modulates per-frame sampling density via shear magnitude λ(p) = |δ(z_prev)|
/// P_t(p) = clamp(ρ_base + α·W(λ(p)), 0, 1)
/// M_t(p) = I(B(p,t) < P_t(p))
class AdaptiveSampler : public Toolkit, public RuntimeObject {
private:
    SSAT *ssat_{nullptr};
    
    // Shader for computing adaptive sampling mask
    Shader<void(AdaptiveSamplerParam, LenticularParams)> compute_mask_shader_;
    
    // Buffers
    Buffer<uint> sampling_mask_;      // Binary mask M_t (1 = sample, 0 = skip)
    Buffer<float> shear_magnitude_;   // Shear magnitude λ(p) per subpixel
    
public:
    void set_ssat(SSAT *ssat) noexcept { ssat_ = ssat; }
    explicit AdaptiveSampler(SSAT *ssat)
        : ssat_(ssat) {}
    
    VS_HOTFIX_MAKE_RESTORE(RuntimeObject, ssat_, compute_mask_shader_,
                           sampling_mask_, shear_magnitude_)
    
    // ========================================================================
    // Accessors
    // ========================================================================
    
    [[nodiscard]] BufferView<uint> sampling_mask() const noexcept {
        return sampling_mask_.view();
    }
    
    [[nodiscard]] BufferView<float> shear_magnitude() const noexcept {
        return shear_magnitude_.view();
    }
    
    // ========================================================================
    // Lifecycle
    // ========================================================================

    void ensure_buffers(uint total_subpixels) noexcept {
        if (sampling_mask_.size() != total_subpixels) {
            OC_INFO_FORMAT("AdaptiveSampler allocate sampling buffers for {} subpixels",
                           total_subpixels);
            init_buffer_zero(device(), sampling_mask_, total_subpixels, "SSAT::sampling_mask");
            init_buffer_zero(device(), shear_magnitude_, total_subpixels, "SSAT::shear_magnitude");
            return;
        }
        if (shear_magnitude_.size() != total_subpixels) {
            OC_INFO_FORMAT("AdaptiveSampler resize shear buffer to {} entries",
                           total_subpixels);
            init_buffer_zero(device(), sampling_mask_, total_subpixels, "SSAT::sampling_mask");
            init_buffer_zero(device(), shear_magnitude_, total_subpixels, "SSAT::shear_magnitude");
        }
    }
    
    void prepare(uint total_subpixels) noexcept {
        ensure_buffers(total_subpixels);
    }
    
    void compile() noexcept {
        compile_compute_mask();
    }
    
    void update_resolution(uint total_subpixels) noexcept {
        ensure_buffers(total_subpixels);
    }
    
    // ========================================================================
    // Shader Compilation
    // ========================================================================
    
    void compile_compute_mask() noexcept {
        Pipeline *pipeline_ref = pipeline();
        Kernel kernel = [&, pipeline_ref](Var<AdaptiveSamplerParam> param, Var<LenticularParams> lent) {
            Uint2 dispatch_idx_val = dispatch_idx().xy();
            Uint linear_idx = dispatch_id();
            
            Float3 camera_pos = param.camera_pos.as_vec3();
            Float rho_base = param.rho_base;
            Float alpha = param.alpha;
            Float z_ref = param.z_ref;
            Uint frame_index = param.frame_index;
            
            // Get depth from previous frame's visibility buffer
            TriangleHitVar prev_hit = param.prev_visibility.read(linear_idx);
            Bool is_valid = !prev_hit->is_miss() && prev_hit.inst_id != InvalidUI32;
            
            Float depth = z_ref;  // Default to focal plane
            $if(is_valid) {
                Interaction prev_it = pipeline_ref->geometry().compute_surface_interaction(prev_hit, false);
                depth = length(prev_it.pos - camera_pos);
            };
            
            // Compute shear magnitude λ(p) = |δ(z_prev)|
            Float shear = compute_shear_magnitude(depth, z_ref);
            shear = clamp(shear, SSATConfig::Sampling::kMinShearMagnitude,
                                 SSATConfig::Sampling::kMaxShearMagnitude);
            
            // Store shear magnitude
            param.shear_magnitude.write(linear_idx, shear);
            
            // Compute sampling probability P_t(p) = clamp(ρ_base + α·W(λ), 0, 1)
            Float probability = compute_sampling_probability(shear, rho_base, alpha);
            
            // Stochastic dithering using blue noise B(p, t)
            // Decode pixel from subpixel index for spatial coherence
            Uint pixel_x = dispatch_idx_val.x / 3u;
            Uint pixel_y = dispatch_idx_val.y;
            Uint channel_k = dispatch_idx_val.x % 3u;
            
            // Add channel offset to break correlation between RGB subpixels
            Float noise = stochastic_dither(make_uint2(pixel_x, pixel_y), frame_index + channel_k);
            
            // Generate binary mask M_t(p) = I(B(p,t) < P_t(p))
            Uint mask_value = select(noise < probability, 1u, 0u);
            param.sampling_mask.write(linear_idx, mask_value);
        };
        
        compute_mask_shader_ = device().compile(kernel, "SSAT-AdaptiveSampler-ComputeMask");
    }
    
    // ========================================================================
    // Dispatch
    // ========================================================================
    
    /// Compute adaptive sampling mask for the current frame
    /// 
    /// @param prev_visibility Previous frame visibility buffer
    /// @param prev_camera_pos Previous camera position
    /// @param lent Lenticular parameters
    /// @param z_ref ZPP depth
    /// @param rho_base Baseline fill rate
    /// @param alpha Shear sensitivity
    /// @param frame_index Current frame index
    /// @param subpixel_res Subpixel resolution
    /// @return Command list for execution
    [[nodiscard]] CommandBatch compute_sampling_mask(
        BufferView<TriangleHit> prev_visibility,
        const array_float3 &prev_camera_pos,
        const LenticularParams &lent,
        float z_ref,
        float rho_base,
        float alpha,
        uint frame_index,
        uint2 subpixel_res) noexcept {
        
        AdaptiveSamplerParam param;
        param.prev_visibility = prev_visibility.descriptor();
        param.sampling_mask = sampling_mask_.descriptor();
        param.shear_magnitude = shear_magnitude_.descriptor();
        param.camera_pos = prev_camera_pos;
        param.rho_base = rho_base;
        param.alpha = alpha;
        param.z_ref = z_ref;
        param.frame_index = frame_index;
        
        CommandBatch ret;
        ret << compute_mask_shader_(param, lent).dispatch(subpixel_res);
        return ret;
    }
};

}// namespace vision::ssat
