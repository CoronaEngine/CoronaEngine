#include "variance_estimator.h"
#include "svgf.h"
#include "svgf_config.h"
#include "base/sensor/sensor.h"

namespace vision::svgf {

using Cfg = SVGFConfig;

void VarianceEstimator::prepare() noexcept {}

void VarianceEstimator::compile() noexcept {
Pipeline *pipeline_ref = pipeline();
Kernel variance_kernel = [&, pipeline_ref](Var<VarianceEstimatorParam> param) {
    Float4 film_offsets = frame_filter_offsets(pipeline_ref, param.frame_index);
    Int2 screen_size = make_int2(dispatch_dim().xy());
    Uint index = dispatch_id();
        
    TriangleHitVar cur_hit = param.visibility_buffer.read(index);
        
    $if(!PixelStateUtils::is_sky(cur_hit)) {
        RadType4Var cur_direct = param.radiance_direct.read(index);
        RadType4Var cur_indirect = param.radiance_indirect.read(index);

        // Accumulate the original Monte Carlo samples. A bright indirect sample
        // surrounded by eight zero samples still carries valid illumination;
        // clipping it to the current 1-spp neighbourhood biases the mean dark.

        Float lum_direct = luminance(cur_direct.xyz());
        Float lum_indirect = luminance(cur_indirect.xyz());
            
        Interaction cur_it = pipeline_ref->geometry().compute_surface_interaction(cur_hit, false);
        Float3 shading_normal = PixelStateUtils::query_shading_normal(
            pipeline_ref, cur_hit, param.camera_pos.as_vec3());
        // Reprojection tests the current surface against the previous view.
        // Both distances must use that same eye position: comparing current-eye
        // and previous-eye distances rejects a stationary surface on dolly moves.
        Float expected_prev_depth = length(cur_it.pos - param.prev_camera_pos.as_vec3());
            
        Float2 motion_vec = param.motion_vectors.read(index);
        Float motion_length = length(motion_vec);
        
        Float2 cur_pos_float = make_float2(dispatch_idx().xy()) + 0.5f;
        Float2 prev_pos_float = cur_pos_float - motion_vec;
        
        Float2 prev_texel = prev_pos_float - 0.5f + film_offsets.xy() - film_offsets.zw();
        Float2 floor_pos = floor(prev_texel);
        Float2 frac_pos = prev_texel - floor_pos;
        
        Float w00 = (1.f - frac_pos.x) * (1.f - frac_pos.y);
        Float w10 = frac_pos.x * (1.f - frac_pos.y);
        Float w01 = (1.f - frac_pos.x) * frac_pos.y;
        Float w11 = frac_pos.x * frac_pos.y;
        
        // Use Float3 accumulators for precision (avoid half precision accumulation errors)
        Float3 acc_direct = make_float3(0.f);
        Float3 acc_indirect = make_float3(0.f);
        Float acc_m1_direct = 0.f;
        Float acc_m2_direct = 0.f;
        Float acc_m1_indirect = 0.f;
        Float acc_m2_indirect = 0.f;
        Float acc_history = 0.f;
        Float acc_history_indirect = 0.f;
        Float total_weight = 0.f;
        
        auto check_tap_consistency = [&](Int2 tap_pixel, Float bilinear_w, bool fallback = false) {
            $if(bilinear_w > 0.001f && 
                all(tap_pixel >= 0) && all(tap_pixel < screen_size)) {
                
                Uint tap_idx = cast<uint>(tap_pixel.y) * cast<uint>(screen_size.x) + cast<uint>(tap_pixel.x);
                TriangleHitVar tap_hit = param.visibility_buffer_prev.read(tap_idx);
                Bool tap_is_sky = PixelStateUtils::is_sky(tap_hit);
                
                $if(!tap_is_sky) {
                    Interaction tap_it = pipeline_ref->geometry().compute_surface_interaction(tap_hit, false);
                    Float tap_depth = length(tap_it.pos - param.prev_camera_pos.as_vec3());
                    Bool tap_is_emissive = PixelStateUtils::is_emissive(pipeline_ref, tap_hit);
                    
                    Float depth_diff = abs(expected_prev_depth - tap_depth) / max(expected_prev_depth, 0.1f);
                    // A subpixel curve may land on a different facet next frame.
                    // ReSTIR has sparse mixed-light samples: rejecting normals
                    // just six degrees apart repeatedly resets them to black.
                    Float normal_exponent = ocarina::select(param.channel_kind ==
                        uint(RealTimeDenoiseInput::ChannelKind::DirectIndirect),
                        Cfg::Temporal::kReSTIRNormalExp, Cfg::Temporal::kNormalExp);
                    Float normal_sim = pow(max(dot(cur_it.ng, tap_it.ng), 0.f), normal_exponent);
                    Bool same_instance = cur_hit.inst_id == tap_hit.inst_id;
                    Bool emission_match = (cur_it.has_emission() == tap_is_emissive) &&
                        (!cur_it.has_emission() || cur_it.light_id() == tap_it.light_id());
                    
                    Bool tap_consistent = same_instance &&
                        (depth_diff < Cfg::Temporal::kDepthThreshold) &&
                        (normal_sim > Cfg::Temporal::kNormalThreshold) &&
                        emission_match;
                    SVGFDataDualVar tap_svgf = param.svgf_buffer_prev.read(tap_idx);
                    if (fallback) {
                        // Nearby history may lie on another parallel surface of
                        // this instance. Keep the search on the current plane.
                        tap_consistent = tap_consistent &&
                            abs(dot(tap_it.pos - cur_it.pos, cur_it.ng)) <
                                Cfg::Temporal::kFallbackPlaneThreshold * max(expected_prev_depth, 0.1f);
                    }
                    
                    Float effective_weight = 0.f;
                    
                    $if(tap_consistent) {
                        effective_weight = bilinear_w;
                    };
                    
                    $if(effective_weight > 0.001f) {
                        acc_direct += tap_svgf->illumination_direct() * effective_weight;
                        acc_indirect += tap_svgf->illumination_indirect() * effective_weight;
                        acc_m1_direct += tap_svgf->first_moment_direct() * effective_weight;
                        acc_m2_direct += tap_svgf->second_moment_direct() * effective_weight;
                        acc_m1_indirect += tap_svgf->first_moment_indirect() * effective_weight;
                        acc_m2_indirect += tap_svgf->second_moment_indirect() * effective_weight;
                        Float history_scale = ocarina::select(tap_consistent, 1.f, 0.5f);
                        acc_history += tap_svgf->history_count() * effective_weight * history_scale;
                        acc_history_indirect += tap_svgf->history_count_indirect() * effective_weight * history_scale;
                        total_weight += effective_weight;
                    };
                };
            };
        };
        
        Int2 base_pixel = make_int2(floor_pos);
        check_tap_consistency(base_pixel + make_int2(0, 0), w00);
        check_tap_consistency(base_pixel + make_int2(1, 0), w10);
        check_tap_consistency(base_pixel + make_int2(0, 1), w01);
        check_tap_consistency(base_pixel + make_int2(1, 1), w11);

        // Jitter and camera motion can move a thin surface outside the bilinear
        // footprint. Recover nearby history on the same instance and plane
        // instead of cold-starting every frame. Limit the borrowed history's age
        // below, and reject large jumps or disocclusions with no matching surface.
        Bool fallback_history = false;
        $if(param.frame_index > 0u && total_weight <= 0.01f &&
            motion_length < Cfg::Temporal::kFallbackMotionThreshold) {
            fallback_history = true;
            acc_direct = make_float3(0.f);
            acc_indirect = make_float3(0.f);
            acc_m1_direct = 0.f; acc_m2_direct = 0.f;
            acc_m1_indirect = 0.f; acc_m2_indirect = 0.f;
            acc_history = 0.f; acc_history_indirect = 0.f;
            total_weight = 0.f;
            Int2 nearest_pixel = make_int2(floor(prev_texel + 0.5f));
            for (int y = -1; y <= 1; ++y) {
                for (int x = -1; x <= 1; ++x) {
                    Int2 tap_pixel = nearest_pixel + make_int2(x, y);
                    Float2 delta = make_float2(tap_pixel) - prev_texel;
                    check_tap_consistency(tap_pixel, 1.f / (1.f + dot(delta, delta)), true);
                }
            }
        };
        
        // Explicit integrator invalidation (including lighting edits) starts a
        // new history even when the camera and surface geometry are unchanged.
        Bool valid_history = param.frame_index > 0u && total_weight > 0.01f;
        Float inv_weight = 1.f / max(total_weight, 0.001f);
        
        // Keep as Float3 for precision during blending
        Float3 prev_direct = acc_direct * inv_weight;
        Float3 prev_indirect = acc_indirect * inv_weight;
        Float prev_m1_direct = acc_m1_direct * inv_weight;
        Float prev_m2_direct = acc_m2_direct * inv_weight;
        Float prev_m1_indirect = acc_m1_indirect * inv_weight;
        Float prev_m2_indirect = acc_m2_indirect * inv_weight;
        Float prev_history = acc_history * inv_weight;
        Float prev_history_indirect = acc_history_indirect * inv_weight;

        // Do not clip accumulated illumination to a 1-spp colour box, even under
        // motion: sparse indirect samples make that box dark and leave a biased
        // history after the camera stops. Reject geometry-inconsistent history
        // and use the motion-dependent alpha below for responsiveness instead.

        Float3 cur_color = make_float3(cur_direct.xyz() + cur_indirect.xyz());
        Float3 prev_color = prev_direct + prev_indirect;
        
        Float ghosting_factor = 0.f;
        
        Float color_diff = length(cur_color - prev_color) / 
            max(length(cur_color) + length(prev_color), 0.001f);
        $if(cur_it.has_emission()) {
            Float t = saturate((color_diff - Cfg::Ghosting::kColorDiffThreshold * 0.7f) / 
                (Cfg::Ghosting::kColorDiffThreshold * 0.6f));
            ghosting_factor = max(ghosting_factor, t * t * (3.f - 2.f * t));
        };
        
        Bool diffuse_specular = param.channel_kind ==
            uint(RealTimeDenoiseInput::ChannelKind::DiffuseSpecular);
        // Surface-reprojected specular history depends on the outgoing world
        // direction, as in RELAX's surface-motion confidence. Pure camera
        // rotation moves pixels but does not change that direction. Convert
        // angular parallax to pixel units so translation retains the existing
        // response scale, including when rotation cancels its screen motion.
        Float3 current_to_eye = param.camera_pos.as_vec3() - cur_it.pos;
        Float3 previous_to_eye = param.prev_camera_pos.as_vec3() - cur_it.pos;
        Float3 current_view = current_to_eye / max(length(current_to_eye), 1e-6f);
        Float3 previous_view = previous_to_eye / max(length(previous_to_eye), 1e-6f);
        Float view_motion = length(current_view - previous_view) * param.pixels_per_radian;
        // ReSTIR's mixed direct/indirect channels are also invariant to pure
        // rotation at a fixed eye. Only parallax changes their outgoing view.
        Float response_motion = view_motion;

        Float max_history_for_motion = max(
            Cfg::Temporal::kMaxHistoryStatic - 
            tanh(response_motion / Cfg::Temporal::kMotionScaleDivisor) *
            (Cfg::Temporal::kMaxHistoryStatic - Cfg::Temporal::kMaxHistoryFast),
            Cfg::Temporal::kMaxHistoryFast);
        // PT's first channel contains view-independent diffuse lighting. A
        // matching surface can retain it during camera motion; specular and
        // legacy direct/indirect producers retain the responsive motion policy.
        Float max_history_direct = ocarina::select(diffuse_specular,
            Cfg::Temporal::kMaxHistoryStatic, max_history_for_motion);
        Float base_history = min(prev_history + 1.f, max_history_direct);
        Float base_history_indirect = min(prev_history_indirect + 1.f, max_history_for_motion);
        // Spatially borrowed colour must not inherit a neighbour's full age:
        // that would freeze a shifted shadow or highlight behind alpha=1/128.
        base_history = ocarina::select(fallback_history,
            min(base_history, Cfg::Temporal::kFallbackMaxHistory), base_history);
        base_history_indirect = ocarina::select(fallback_history,
            min(base_history_indirect, Cfg::Temporal::kFallbackMaxHistory), base_history_indirect);
        
        Float history_scale = 1.f - ghosting_factor * 0.85f;
        Float effective_history = max(base_history * history_scale, 1.f);
        
        Float new_history = ocarina::select(valid_history, effective_history, 1.f);
        Float new_history_indirect = ocarina::select(valid_history,
            max(base_history_indirect * history_scale, 1.f), 1.f);
        
        Float base_alpha = 1.f / new_history;
        
        Float motion_boost = tanh(response_motion / Cfg::Temporal::kMotionAlphaDivisor) *
                             Cfg::Temporal::kMotionAlphaScale;
        
        Float alpha = max(base_alpha, ocarina::select(diffuse_specular, 0.f, motion_boost));
        Float alpha_indirect = max(1.f / new_history_indirect, motion_boost);
        
        Float max_alpha = 0.95f;
        alpha = min(alpha, max_alpha);
        alpha_indirect = min(alpha_indirect, max_alpha);

        // Motion can raise alpha above 1/history. Store the corresponding
        // effective window, not the camera's age, so stopping does not freeze
        // a noisy moving estimate behind an artificially long history.
        new_history = min(new_history, 1.f / max(alpha, 1e-6f));
        new_history_indirect = min(new_history_indirect, 1.f / max(alpha_indirect, 1e-6f));
        
        Bool use_history = valid_history;
        
        // Clamp luminance before squaring to prevent half overflow
        Float lum_direct_clamped = HalfSafeUtils::clamp_luminance(lum_direct);
        Float lum_indirect_clamped = HalfSafeUtils::clamp_luminance(lum_indirect);
        
        // Compute blended values in Float precision, then convert to RadType
        Float3 new_direct_f = ocarina::select(use_history,
            prev_direct + alpha * (make_float3(cur_direct.xyz()) - prev_direct),
            make_float3(cur_direct.xyz()));
        Float3 new_indirect_f = ocarina::select(use_history,
            prev_indirect + alpha_indirect * (make_float3(cur_indirect.xyz()) - prev_indirect),
            make_float3(cur_indirect.xyz()));
        
        
        // Clamp output radiance to half-safe range
        new_direct_f = HalfSafeUtils::clamp_radiance(new_direct_f);
        new_indirect_f = HalfSafeUtils::clamp_radiance(new_indirect_f);
        
        RadType3Var new_direct = make_RadType3(new_direct_f);
        RadType3Var new_indirect = make_RadType3(new_indirect_f);
            
        Float new_m1_direct = ocarina::select(use_history,
            prev_m1_direct + alpha * (lum_direct_clamped - prev_m1_direct),
            lum_direct_clamped);
        Float new_m2_direct = ocarina::select(use_history,
            prev_m2_direct + alpha * (lum_direct_clamped * lum_direct_clamped - prev_m2_direct),
            lum_direct_clamped * lum_direct_clamped);
            
        Float new_m1_indirect = ocarina::select(use_history,
            prev_m1_indirect + alpha_indirect * (lum_indirect_clamped - prev_m1_indirect),
            lum_indirect_clamped);
        Float new_m2_indirect = ocarina::select(use_history,
            prev_m2_indirect + alpha_indirect * (lum_indirect_clamped * lum_indirect_clamped - prev_m2_indirect),
            lum_indirect_clamped * lum_indirect_clamped);
            
        Float temporal_var_direct = VarianceUtils::compute_variance(new_m1_direct, new_m2_direct);
        Float temporal_var_indirect = VarianceUtils::compute_variance(new_m1_indirect, new_m2_indirect);
            
        SVGFDataDualVar output;
        output.surface_normal = make_RadType4(shading_normal, 1.f);
        output.illumi_direct = make_RadType4(new_direct, temporal_var_direct);
        output.illumi_indirect = make_RadType4(new_indirect, temporal_var_indirect);
        // Independent effective windows, without increasing history-buffer size.
        output.moments_direct = make_RadType4(new_m1_direct, new_m2_direct, new_history, new_history_indirect);
        // Current reflectance scales, not temporal moments. The raw PT path
        // supplies 1; demodulation supplies the luminance of its safe RGB guide.
        output.moments_indirect = make_RadType4(new_m1_indirect, new_m2_indirect,
                                               cur_direct.w, cur_indirect.w);
        param.svgf_buffer_cur.write(index, output);
    };
};

    variance_shader_ = device().compile(variance_kernel, "SVGF-VarianceEstimator");
}

CommandBatch VarianceEstimator::dispatch_variance(RealTimeDenoiseInput &input) noexcept {
    VarianceEstimatorParam param;
    param.radiance_direct = input.direct.descriptor();
    param.radiance_indirect = input.indirect.descriptor();
    param.svgf_buffer_prev = svgf_->svgf_buffer_prev(input.frame_index).descriptor();
    param.svgf_buffer_cur = svgf_->svgf_buffer_cur(input.frame_index).descriptor();
    param.visibility_buffer = input.visibility.descriptor();
    param.visibility_buffer_prev = input.prev_visibility.descriptor();
    param.motion_vectors = input.motion_vec.descriptor();
    param.camera_pos = input.camera_pos;
    param.prev_camera_pos = input.prev_camera_pos;
    // Sensor projects the configured FOV across the shorter image dimension.
    param.pixels_per_radian = 0.5f * compute_screen_short_edge(input.resolution) /
        tan(radians(pipeline()->scene().sensor()->fov_y()) * 0.5f);
    param.frame_index = input.frame_index;
    param.channel_kind = static_cast<uint>(input.channel_kind);
    CommandBatch ret;
    ret << variance_shader_(param).dispatch(input.resolution);
    return ret;
}

void VarianceEstimator::update_resolution(uint2 resolution) noexcept {}

}// namespace vision::svgf
