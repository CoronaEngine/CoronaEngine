//
// Created by Zero on 2025/01/26.
// Phase 3: 5D Temporal Accumulation
//
// Stabilizes the spatially reconstructed signal by exploiting temporal coherence
// in the 5D phase space, preventing angular aliasing on view-dependent surfaces.
//

#pragma once

#include "math/basic_types.h"
#include "dsl/dsl.h"
#include "base/mgr/global.h"
#include "base/mgr/pipeline.h"
#include "phase_space.h"
#include "manifold_transport.h"
#include "sparse_gather.h"
#include "utils.h"
#include "ssat_config.h"

namespace vision::ssat {
using namespace ocarina;

class SSAT;  // Forward declaration

// ============================================================================
// Temporal Accumulator Parameter Structure
// ============================================================================

struct TemporalAccumParam {
    BufferDesc<RadType4> spatial_result;      // Input: spatially filtered radiance
    BufferDesc<RadType4> history;             // Read-only: previous frame's history (ping-pong)
    BufferDesc<RadType4> output;              // Output: temporally accumulated result
    BufferDesc<SSATData> ssat_data;           // Per-subpixel SSAT data (moments, history count)
    BufferDesc<TriangleHit> visibility;       // Current visibility
    BufferDesc<TriangleHit> prev_visibility;  // Previous visibility
    BufferDesc<float2> motion_vectors;        // Motion vectors in subpixel space (dx, dy)
    float3 camera_pos{};                      // Current camera position (from l2w * origin)
    float3 prev_camera_pos{};                 // Previous camera position
    float alpha_base{0.1f};                   // Base blending factor
    float angular_bandwidth{0.1f};            // ω_angular for strict history rejection
    float sigma_x{2.0f};                      // Spatial bandwidth for history gathering
    float sigma_u{0.5f};                      // Angular bandwidth for history gathering
    uint frame_index{0};
};

}// namespace vision::ssat

OC_PARAM_STRUCT(vision::ssat, TemporalAccumParam,
                spatial_result, history, output, ssat_data,
                visibility, prev_visibility, motion_vectors,
                camera_pos, prev_camera_pos,
                alpha_base, angular_bandwidth, sigma_x, sigma_u, frame_index){};

namespace vision::ssat {

// ============================================================================
// Temporal Accumulator Class
// ============================================================================

/// Phase 3: 5D Temporal Accumulation
/// 
/// For each sub-pixel p:
/// 1. Compute historical phase-space coordinate: C_prev = Ψ_temp(p, Δt; v)
/// 2. Gather from history: L_fetch = Υ(L_hist, C_prev) with strict angular bandwidth
/// 3. Blend: L_out = lerp(I_spatial, L_fetch, α · w_valid)
class TemporalAccumulator : public Toolkit, public RuntimeObject {
private:
    SSAT *ssat_{nullptr};
    
    // Shader for temporal accumulation
    Shader<void(TemporalAccumParam, LenticularParams, LightFieldGeometry)> accumulate_shader_;
    Shader<void(Buffer<RadType4>, Buffer<SSATData>)> publish_support_shader_;

    // Buffers
    RegistrableBuffer<SSATData> ssat_data_;     // Per-subpixel temporal data
    RegistrableManaged<RadType4> history_buffer_; // Accumulated history
    
public:
    explicit TemporalAccumulator(SSAT *ssat)
        : ssat_(ssat),
          ssat_data_(pipeline()->bindless_array()),
          history_buffer_(pipeline()->bindless_array()) {}
    
    VS_HOTFIX_MAKE_RESTORE(RuntimeObject, ssat_, accumulate_shader_,
                           publish_support_shader_,
                           ssat_data_, history_buffer_)
    
    // ========================================================================
    // Accessors
    // ========================================================================
    
    [[nodiscard]] BufferView<SSATData> ssat_data() const noexcept {
        return ssat_data_.view();
    }
    
    [[nodiscard]] BufferView<RadType4> history_buffer() const noexcept {
        return history_buffer_.view();
    }
    
    // ========================================================================
    // Lifecycle
    // ========================================================================

    void ensure_buffers(uint total_subpixels) noexcept {
        if (ssat_data_.size() != total_subpixels) {
            ssat_data_.super() = device().create_buffer<SSATData>(total_subpixels, "SSAT::ssat_data");
            vector<SSATData> ssat_init(total_subpixels, SSATData{});
            ssat_data_.upload_immediately(ssat_init.data());
            ssat_data_.set_bindless_array(pipeline()->bindless_array());
            ssat_data_.register_self();
        }
        if (history_buffer_.device_buffer().size() != total_subpixels) {
            history_buffer_.reset_all(device(), total_subpixels, "SSAT::history_buffer");
            history_buffer_.host_buffer().assign(total_subpixels, RadType4{});
            history_buffer_.upload_immediately();
            history_buffer_.set_bindless_array(pipeline()->bindless_array());
            history_buffer_.register_self();
        }
    }
    
    void prepare(uint total_subpixels) noexcept {
        ensure_buffers(total_subpixels);
    }
    
    void compile() noexcept {
        compile_accumulate();
        compile_publish_support();
    }
    
    void update_resolution(uint total_subpixels) noexcept {
        ensure_buffers(total_subpixels);
    }
    
    // ========================================================================
    // Shader Compilation
    // ========================================================================
    
    void compile_accumulate() noexcept {
        Pipeline *pipeline_ref = pipeline();
        Kernel kernel = [&, pipeline_ref](Var<TemporalAccumParam> param,
                           Var<LenticularParams> lent,
                           Var<LightFieldGeometry> geom) {
            
            Uint2 dispatch_idx_val = dispatch_idx().xy();
            Uint linear_idx = dispatch_id();
            Int2 screen_size = make_int2(dispatch_dim().xy());
            
            // Camera positions passed directly from host (saves 128 bytes vs passing float4x4)
            Float3 camera_pos = param.camera_pos;
            Float3 prev_camera_pos = param.prev_camera_pos;
            Float alpha_base = param.alpha_base;
            Float angular_bandwidth = param.angular_bandwidth;
            Float sigma_x = param.sigma_x;
            Float sigma_u = param.sigma_u;
            // Use geom.d_f as z_ref (focal distance = ZPP depth)
            Float z_ref = geom.d_f;
            Uint frame_index = param.frame_index;
            
            // Decode subpixel
            Uint pixel_x, pixel_y, channel_k;
            decode_subpixel(dispatch_idx_val, pixel_x, pixel_y, channel_k);
            
            // Read current spatial result
            Float4 spatial_result = param.spatial_result.read(linear_idx);
            Float3 cur_radiance = spatial_result.xyz();
            Float current_support = saturate(spatial_result.w);
            Float cur_lum = luminance(cur_radiance);
            
            // Read current geometry
            TriangleHitVar cur_hit = param.visibility.read(linear_idx);
            Bool cur_valid = !cur_hit->is_miss() && cur_hit.inst_id != InvalidUI32;
            
            Float cur_depth = z_ref;
            Float3 cur_normal = make_float3(0.f, 1.f, 0.f);
            Float3 cur_pos = make_float3(0.f);
            Uint cur_inst = InvalidUI32;
            Bool cur_emissive = false;
            
            $if(cur_valid) {
                Interaction cur_it = pipeline_ref->geometry().compute_surface_interaction(cur_hit, false);
                cur_pos = cur_it.pos;
                cur_depth = length(cur_it.pos - camera_pos);
                cur_normal = cur_it.ng;
                cur_inst = cur_hit.inst_id;
                cur_emissive = cur_it.has_emission();
            };
            
            // Get current phase-space coordinate
            PhaseSpaceCoordVar cur_coord = SparseGather::subpixel_to_phase_space(
                dispatch_idx_val, lent, geom);
            
            // ================================================================
            // Temporal Reprojection using Ψ_temp
            // ================================================================

            Float2 motion_vec = param.motion_vectors.read(linear_idx);
            Float motion_length = length(motion_vec);
            Float res_w = lent.res_w;
            Float res_h = lent.res_h;

            // Temporal transport in phase space (motion-vector approximation)
            PhaseSpaceCoordVar prev_coord = ManifoldTransport::temporal_transport_motion_vec(
                cur_coord, motion_vec, res_w * 3.f, res_h);

            // ================================================================
            // Bilinear Reprojection for SSAT Metadata (SVGF pattern)
            // ================================================================

            Float2 prev_pos_float = make_float2(dispatch_idx_val) + 0.5f - motion_vec;
            Float2 prev_texel = prev_pos_float - 0.5f;
            Float2 floor_pos = floor(prev_texel);
            Float2 frac_pos = prev_texel - floor_pos;
            Int2 base_pixel = make_int2(floor_pos);

            // Bilinear weights
            Float w00 = (1.f - frac_pos.x) * (1.f - frac_pos.y);
            Float w10 = frac_pos.x * (1.f - frac_pos.y);
            Float w01 = (1.f - frac_pos.x) * frac_pos.y;
            Float w11 = frac_pos.x * frac_pos.y;

            // Consistency-gated bilinear gather for ssat_data
            Float acc_history = 0.f;
            Float acc_m1 = 0.f;
            Float acc_m2 = 0.f;
            Float acc_support = 0.f;
            Float total_tap_weight = 0.f;

            auto check_tap = [&](Int2 tap_pixel, Float bilinear_w) {
                $if(bilinear_w > 0.001f &&
                    all(tap_pixel >= 0) && all(tap_pixel < screen_size)) {

                    Uint tap_idx = cast<uint>(tap_pixel.y) * cast<uint>(screen_size.x) + cast<uint>(tap_pixel.x);
                    TriangleHitVar tap_hit = param.prev_visibility.read(tap_idx);
                    Bool tap_valid = !tap_hit->is_miss() && tap_hit.inst_id != InvalidUI32;

                    $if(tap_valid) {
                        Interaction tap_it = pipeline_ref->geometry().compute_surface_interaction(tap_hit, false);
                        Float tap_depth = length(tap_it.pos - prev_camera_pos);

                        Float tap_depth_diff = abs(cur_depth - tap_depth) / max(cur_depth, 0.1f);
                        Float tap_normal_dot = max(dot(cur_normal, tap_it.ng), 0.f);
                        Bool tap_consistent = cur_valid && (cur_inst == tap_hit.inst_id) &&
                            (tap_depth_diff < SSATConfig::Temporal::kDepthThreshold) &&
                            (tap_normal_dot > SSATConfig::Temporal::kNormalThreshold);

                        $if(tap_consistent) {
                            SSATDataVar tap_ssat = param.ssat_data.read(tap_idx);
                            acc_history += tap_ssat->history_count() * bilinear_w;
                            acc_m1 += tap_ssat->first_moment() * bilinear_w;
                            acc_m2 += tap_ssat->second_moment() * bilinear_w;
                            acc_support += tap_ssat->support_validity() * bilinear_w;
                            total_tap_weight += bilinear_w;
                        };
                    };
                };
            };

            check_tap(base_pixel + make_int2(0, 0), w00);
            check_tap(base_pixel + make_int2(1, 0), w10);
            check_tap(base_pixel + make_int2(0, 1), w01);
            check_tap(base_pixel + make_int2(1, 1), w11);

            Bool valid_history = total_tap_weight > 0.01f;
            Float inv_tap_weight = 1.f / max(total_tap_weight, 0.001f);
            Float prev_history_count = acc_history * inv_tap_weight;
            Float prev_m1 = acc_m1 * inv_tap_weight;
            Float prev_m2 = acc_m2 * inv_tap_weight;
            Float history_support = acc_support * inv_tap_weight;

            // ================================================================
            // Read from History Buffer at reprojected position
            // ================================================================

            // Use bilinear center for history read (consistent with ssat_data bilinear)
            Int2 hist_pixel = clamp(make_int2(floor_pos) + make_int2(0, 0),
                                    make_int2(0), screen_size - 1);
            Uint hist_idx = cast<uint>(hist_pixel.y) * cast<uint>(screen_size.x) + cast<uint>(hist_pixel.x);
            Float4 history_direct = param.history.read(hist_idx);
            Float3 prev_radiance = history_direct.xyz();
            Float gather_weight = select(luminance(prev_radiance) > 0.f || history_direct.w > 0.f, 1.f, 0.f);
            Float prev_lum = luminance(prev_radiance);

            // ================================================================
            // Smooth Ghosting Rejection (SVGF pattern)
            // ================================================================

            Float3 prev_color = prev_radiance;
            Float color_diff = length(cur_radiance - prev_color) /
                              max(length(cur_radiance) + length(prev_color), 0.001f);
            Float lum_ratio = prev_lum / max(cur_lum, 0.01f);

            Float ghosting_factor = 0.f;

            $if(cur_emissive) {
                Float t = saturate((color_diff - SSATConfig::Ghosting::kColorDiffThreshold * 0.7f) /
                    (SSATConfig::Ghosting::kColorDiffThreshold * 0.6f));
                ghosting_factor = max(ghosting_factor, t * t * (3.f - 2.f * t));
            };

            $if(prev_lum > SSATConfig::Ghosting::kBrightHistoryMinLum) {
                Float t = saturate((lum_ratio - SSATConfig::Ghosting::kBrightHistoryRatio * 0.6f) /
                    (SSATConfig::Ghosting::kBrightHistoryRatio * 0.6f));
                ghosting_factor = max(ghosting_factor, t * t * (3.f - 2.f * t));
            };

            $if(prev_lum > 2.f) {
                Float t_motion = saturate((motion_length - SSATConfig::Ghosting::kFastMotionThreshold * 0.6f) /
                    (SSATConfig::Ghosting::kFastMotionThreshold * 0.8f));
                Float t_lum = saturate((lum_ratio - 15.f) / 15.f);
                Float motion_ghosting = t_motion * t_motion * (3.f - 2.f * t_motion) *
                                        t_lum * t_lum * (3.f - 2.f * t_lum);
                ghosting_factor = max(ghosting_factor, motion_ghosting);
            };

            // ================================================================
            // Adaptive History Length
            // ================================================================

            Float max_history_for_motion = max(
                SSATConfig::Temporal::kMaxHistory -
                tanh(motion_length / SSATConfig::Temporal::kMotionScaleDivisor) *
                (SSATConfig::Temporal::kMaxHistory - SSATConfig::Temporal::kFastHistory),
                SSATConfig::Temporal::kFastHistory);
            Float base_history = min(prev_history_count + 1.f, max_history_for_motion);

            Float history_scale = 1.f - ghosting_factor * 0.85f;
            Float effective_history = max(base_history * history_scale, 1.f);
            Float new_history_count = select(valid_history, effective_history, 1.f);

            // ================================================================
            // Temporal Blending with Gather Confidence
            // ================================================================

            Float alpha = 1.f / new_history_count;
            Float gather_confidence = min(gather_weight / (1.f + SSATConfig::Gather::kEpsilon), 1.f);

            Float motion_boost = tanh(motion_length / 8.f) * 0.15f;
            Float effective_alpha = max(max(alpha, SSATConfig::Temporal::kMinAlpha), motion_boost);
            effective_alpha = min(effective_alpha, 0.95f);

            Bool use_history = valid_history && (gather_confidence > 0.01f);
            Float final_support = ssat_final_support_validity(
                current_support, history_support, use_history);

            Float3 blended_radiance = select(use_history,
                prev_radiance + effective_alpha * (cur_radiance - prev_radiance),
                cur_radiance);
            
            // ================================================================
            // Firefly Suppression
            // ================================================================
            
            Float3 clamped_radiance = blended_radiance;
            Bool skip_firefly = true;

            $if(!skip_firefly && valid_history && prev_history_count > 4.f &&
                prev_m2 - prev_m1 * prev_m1 > SSATConfig::SpatialAngular::kMinVariance) {

                Float prev_variance = max(prev_m2 - prev_m1 * prev_m1, 0.f);
                Float sigma = sqrt(prev_variance);
                Float mean_lum = max(prev_m1, 0.001f);

                Float k = lerp(SSATConfig::Firefly::kSigmaMultiplierMax,
                              SSATConfig::Firefly::kSigmaMultiplierMin,
                              saturate(prev_history_count / max_history_for_motion));

                Float max_lum = mean_lum + k * max(sigma, SSATConfig::Firefly::kMinSigma);
                max_lum = max(max_lum, mean_lum * SSATConfig::Firefly::kMeanMultiplier);

                Float result_lum = luminance(blended_radiance);
                $if(result_lum > max_lum) {
                    Float excess = result_lum - max_lum;
                    Float compressed_lum = max_lum + excess * SSATConfig::Firefly::kClampRatio;
                    Float scale = compressed_lum / max(result_lum, 0.001f);
                    clamped_radiance = blended_radiance * scale;
                };
            };

            // ================================================================
            // Update Moments for Variance Tracking
            // ================================================================

            Float result_lum = luminance(clamped_radiance);

            Float new_m1 = select(use_history,
                                 prev_m1 + effective_alpha * (result_lum - prev_m1),
                                 result_lum);
            Float new_m2 = select(use_history,
                                 prev_m2 + effective_alpha * (result_lum * result_lum - prev_m2),
                                 result_lum * result_lum);

            Float new_variance = max(new_m2 - new_m1 * new_m1, 0.f);

            new_variance = select(!use_history,
                                  new_variance * 8.f + 0.5f,
                                  new_variance);
            
            // ================================================================
            // Write Outputs
            // ================================================================

            // Write accumulated radiance
            param.output.write(linear_idx, make_float4(clamped_radiance, new_variance));

            // Update history buffer for next frame
            param.history.write(linear_idx, make_float4(clamped_radiance, 1.f));

            // Update SSAT data
            SSATDataVar new_ssat;
            new_ssat.radiance_accum = make_float4(clamped_radiance, new_variance);
            new_ssat.moments_history = make_float4(
                new_m1, new_m2, new_history_count, final_support);
            param.ssat_data.write(linear_idx, new_ssat);
        };
        
        accumulate_shader_ = device().compile(kernel, "SSAT-TemporalAccumulator");
    }

    void compile_publish_support() noexcept {
        Kernel kernel = [](BufferVar<RadType4> spatial_result,
                           BufferVar<SSATData> ssat_data) {
            const Uint linear_idx = dispatch_id();
            const Float support = saturate(spatial_result.read(linear_idx).w);
            SSATDataVar previous = ssat_data.read(linear_idx);
            SSATDataVar updated;
            updated.radiance_accum = make_float4(
                previous->radiance(), previous->variance());
            updated.moments_history = make_float4(
                previous->first_moment(),
                previous->second_moment(),
                previous->history_count(),
                support);
            ssat_data.write(linear_idx, updated);
        };
        publish_support_shader_ = device().compile(
            kernel, "SSAT-PublishPhase2Support");
    }
    
    // ========================================================================
    // Dispatch
    // ========================================================================
    
    /// Dispatch temporal accumulation
    /// 
    /// @param spatial_result Spatially filtered radiance from Phase 2
    /// @param output Output buffer for accumulated result
    /// @param visibility Current visibility buffer
    /// @param prev_visibility Previous visibility buffer
    /// @param motion_vectors Motion vectors in subpixel space (dx, dy)
    /// @param lent Lenticular parameters
    /// @param geom Light field geometry (z_ref is derived from geom.d_f)
    /// @param l2w Current local to world (used to compute camera_pos)
    /// @param prev_l2w Previous local to world (used to compute prev_camera_pos)
    /// @param alpha_base Base blending factor
    /// @param angular_bandwidth Angular bandwidth for history rejection
    /// @param sigma_x Spatial bandwidth
    /// @param sigma_u Angular bandwidth
    /// @param frame_index Current frame index
    /// @param subpixel_res Subpixel resolution
    /// @return Command list for execution
    [[nodiscard]] CommandBatch dispatch_accumulate(
        BufferView<RadType4> spatial_result,
        BufferView<RadType4> output,
        BufferView<TriangleHit> visibility,
        BufferView<TriangleHit> prev_visibility,
        BufferView<float2> motion_vectors,
        const LenticularParams &lent,
        const LightFieldGeometry &geom,
        const float4x4 &l2w,
        const float4x4 &prev_l2w,
        float alpha_base,
        float angular_bandwidth,
        float sigma_x,
        float sigma_u,
        uint frame_index,
        uint2 subpixel_res) noexcept {
        
        // Compute camera positions on host (saves 128 bytes vs passing float4x4 to shader)
        // Camera array center is at local origin (0, 0, 0)
        float3 camera_pos = make_float3(
            l2w[3][0], l2w[3][1], l2w[3][2]);  // Translation column of l2w
        float3 prev_camera_pos = make_float3(
            prev_l2w[3][0], prev_l2w[3][1], prev_l2w[3][2]);
        
        TemporalAccumParam param;
        param.spatial_result = spatial_result.descriptor();
        param.history = history_buffer_.view().descriptor();
        param.output = output.descriptor();
        param.ssat_data = ssat_data_.view().descriptor();
        param.visibility = visibility.descriptor();
        param.prev_visibility = prev_visibility.descriptor();
        param.motion_vectors = motion_vectors.descriptor();
        param.camera_pos = camera_pos;
        param.prev_camera_pos = prev_camera_pos;
        param.alpha_base = alpha_base;
        param.angular_bandwidth = angular_bandwidth;
        param.sigma_x = sigma_x;
        param.sigma_u = sigma_u;
        param.frame_index = frame_index;

        CommandBatch ret;
        ret << accumulate_shader_(param, lent, geom).dispatch(subpixel_res);
        return ret;
    }

    [[nodiscard]] CommandBatch publish_phase2_support(
        BufferView<RadType4> spatial_result,
        uint2 subpixel_res) noexcept {
        CommandBatch ret;
        ret << publish_support_shader_(spatial_result, ssat_data_.view())
                   .dispatch(subpixel_res);
        return ret;
    }
};

}// namespace vision::ssat
