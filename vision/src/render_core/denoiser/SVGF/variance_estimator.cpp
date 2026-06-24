#include "variance_estimator.h"
#include "svgf.h"
#include "svgf_config.h"

namespace vision::svgf {

using Cfg = SVGFConfig;

void VarianceEstimator::prepare() noexcept {}

void VarianceEstimator::compile() noexcept {
Pipeline *pipeline_ref = pipeline();
Kernel variance_kernel = [&, pipeline_ref](Var<VarianceEstimatorParam> param) {
    Int2 screen_size = make_int2(dispatch_dim().xy());
    Uint index = dispatch_id();
        
    TriangleHitVar cur_hit = param.visibility_buffer.read(index);
        
    $if(!PixelStateUtils::is_sky(cur_hit)) {
        RadType4Var cur_direct = param.radiance_direct.read(index);
        RadType4Var cur_indirect = param.radiance_indirect.read(index);

        // === Input anti-firefly clamp (NRD-style) ===
        // Specular highlights leave isolated single-pixel luminance spikes that SVGF
        // cannot remove (a spike is not proportional to any surface quantity, and its
        // view-dependent position breaks surface-motion reprojection, so it neither
        // blurs nor accumulates cleanly). Clamp the current pixel luminance to the
        // neighbour distribution's upper bound BEFORE it pollutes moments/history.
        // The neighbourMax floor protects genuine multi-pixel highlights; only lone
        // outliers above the local max are reined in.
        {
            Int2 ff_pixel = make_int2(dispatch_idx().xy());
            Float ff_m1_d = 0.f, ff_m2_d = 0.f, ff_max_d = 0.f;
            Float ff_m1_i = 0.f, ff_m2_i = 0.f, ff_max_i = 0.f;
            Float ff_cnt = 0.f;
            for (int fy = -Cfg::InputFirefly::kRadius; fy <= Cfg::InputFirefly::kRadius; ++fy) {
                for (int fx = -Cfg::InputFirefly::kRadius; fx <= Cfg::InputFirefly::kRadius; ++fx) {
                    if (fx == 0 && fy == 0) { continue; }
                    Int2 fp = ff_pixel + make_int2(fx, fy);
                    $if(all(fp >= 0) && all(fp < screen_size)) {
                        Uint fidx = cast<uint>(fp.y) * cast<uint>(screen_size.x) + cast<uint>(fp.x);
                        Float ld = luminance(param.radiance_direct.read(fidx).xyz());
                        Float li = luminance(param.radiance_indirect.read(fidx).xyz());
                        ff_m1_d += ld;
                        ff_m2_d += ld * ld;
                        ff_max_d = max(ff_max_d, ld);
                        ff_m1_i += li;
                        ff_m2_i += li * li;
                        ff_max_i = max(ff_max_i, li);
                        ff_cnt += 1.f;
                    };
                }
            }
            Float inv_ff = 1.f / max(ff_cnt, 1.f);
            auto clamp_firefly = [&](RadType4Var c, Float m1, Float m2, Float nmax) -> RadType4Var {
                Float mean = m1 * inv_ff;
                Float variance = max(m2 * inv_ff - mean * mean, 0.f);
                Float hi = max(mean + Cfg::InputFirefly::kSigmaScale * sqrt(variance), nmax);
                Float lum = luminance(c.xyz());
                Float scale = ocarina::select(lum > hi, hi / max(lum, 1e-4f), 1.f);
                return make_RadType4(c.xyz() * RadTypeVar(scale), Float(c.w));
            };
            cur_direct = clamp_firefly(cur_direct, ff_m1_d, ff_m2_d, ff_max_d);
            cur_indirect = clamp_firefly(cur_indirect, ff_m1_i, ff_m2_i, ff_max_i);
        }

        Float lum_direct = luminance(cur_direct.xyz());
        Float lum_indirect = luminance(cur_indirect.xyz());
            
        Interaction cur_it = pipeline_ref->geometry().compute_surface_interaction(cur_hit, false);
        Float cur_depth = length(cur_it.pos - param.camera_pos.as_vec3());
            
        Float2 motion_vec = param.motion_vectors.read(index);
        Float motion_length = length(motion_vec);
        
        Float2 cur_pos_float = make_float2(dispatch_idx().xy()) + 0.5f;
        Float2 prev_pos_float = cur_pos_float - motion_vec;
        
        Float2 prev_texel = prev_pos_float - 0.5f;
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
        Float total_weight = 0.f;
        
        auto check_tap_consistency = [&](Int2 tap_pixel, Float bilinear_w) {
            $if(bilinear_w > 0.001f && 
                all(tap_pixel >= 0) && all(tap_pixel < screen_size)) {
                
                Uint tap_idx = cast<uint>(tap_pixel.y) * cast<uint>(screen_size.x) + cast<uint>(tap_pixel.x);
                TriangleHitVar tap_hit = param.visibility_buffer_prev.read(tap_idx);
                Bool tap_is_sky = PixelStateUtils::is_sky(tap_hit);
                
                $if(!tap_is_sky) {
                    Interaction tap_it = pipeline_ref->geometry().compute_surface_interaction(tap_hit, false);
                    Float tap_depth = length(tap_it.pos - param.prev_camera_pos.as_vec3());
                    Bool tap_is_emissive = PixelStateUtils::is_emissive(pipeline_ref, tap_hit);
                    
                    Float depth_diff = abs(cur_depth - tap_depth) / max(cur_depth, 0.1f);
                    Float normal_sim = pow(max(dot(cur_it.ng, tap_it.ng), 0.f), Cfg::Temporal::kNormalExp);
                    Bool same_instance = cur_hit.inst_id == tap_hit.inst_id;
                    Bool emission_match = (cur_it.has_emission() == tap_is_emissive) &&
                        (!cur_it.has_emission() || cur_it.light_id() == tap_it.light_id());
                    
                    Bool tap_consistent = same_instance &&
                        (depth_diff < Cfg::Temporal::kDepthThreshold) &&
                        (normal_sim > Cfg::Temporal::kNormalThreshold) &&
                        emission_match;
                    
                    Float effective_weight = 0.f;
                    
                    $if(tap_consistent) {
                        effective_weight = bilinear_w;
                    };
                    
                    $if(effective_weight > 0.001f) {
                        SVGFDataDualVar tap_svgf = param.svgf_buffer_prev.read(tap_idx);
                        acc_direct += tap_svgf->illumination_direct() * effective_weight;
                        acc_indirect += tap_svgf->illumination_indirect() * effective_weight;
                        acc_m1_direct += tap_svgf->first_moment_direct() * effective_weight;
                        acc_m2_direct += tap_svgf->second_moment_direct() * effective_weight;
                        acc_m1_indirect += tap_svgf->first_moment_indirect() * effective_weight;
                        acc_m2_indirect += tap_svgf->second_moment_indirect() * effective_weight;
                        Float history_scale = ocarina::select(tap_consistent, 1.f, 0.5f);
                        acc_history += tap_svgf->history_count() * effective_weight * history_scale;
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
        
        Bool valid_history = total_weight > 0.01f;
        Float inv_weight = 1.f / max(total_weight, 0.001f);
        
        // Keep as Float3 for precision during blending
        Float3 prev_direct = acc_direct * inv_weight;
        Float3 prev_indirect = acc_indirect * inv_weight;
        Float prev_m1_direct = acc_m1_direct * inv_weight;
        Float prev_m2_direct = acc_m2_direct * inv_weight;
        Float prev_m1_indirect = acc_m1_indirect * inv_weight;
        Float prev_m2_indirect = acc_m2_indirect * inv_weight;
        Float prev_history = acc_history * inv_weight;

        // === NRD/ReLAX-style history color clamping (anti-ghosting) ===
        // Reprojected history that has drifted outside the current frame's local
        // colour distribution is the direct cause of trailing/smearing. Clamp the
        // history luminance into the current 3x3 neighbourhood box
        // [mean - k*sigma, mean + k*sigma] (chroma preserved by scaling). In smooth
        // regions (small sigma) the box is tight and kills ghosting where it is most
        // visible; in noisy regions (large sigma) it widens automatically and leaves
        // detail untouched. This lets us keep a long, clean history (see
        // kMaxHistoryStatic) for noise reduction without re-introducing trails.
        $if(valid_history) {
            Float box_m1_direct = 0.f, box_m2_direct = 0.f;
            Float box_m1_indirect = 0.f, box_m2_indirect = 0.f;
            Float box_count = 0.f;
            $for(by, -Cfg::HistoryClamp::kRadius, Cfg::HistoryClamp::kRadius + 1) {
                $for(bx, -Cfg::HistoryClamp::kRadius, Cfg::HistoryClamp::kRadius + 1) {
                    Int2 bp = make_int2(dispatch_idx().xy()) + make_int2(bx, by);
                    $if(all(bp >= 0) && all(bp < screen_size)) {
                        Uint bidx = cast<uint>(bp.y) * cast<uint>(screen_size.x) + cast<uint>(bp.x);
                        Float ld = HalfSafeUtils::clamp_luminance(luminance(param.radiance_direct.read(bidx).xyz()));
                        Float li = HalfSafeUtils::clamp_luminance(luminance(param.radiance_indirect.read(bidx).xyz()));
                        box_m1_direct += ld;
                        box_m2_direct += ld * ld;
                        box_m1_indirect += li;
                        box_m2_indirect += li * li;
                        box_count += 1.f;
                    };
                };
            };
            Float inv_box = 1.f / max(box_count, 1.f);
            auto clamp_history = [&](Float3 hist, Float m1, Float m2) -> Float3 {
                Float mean = m1 * inv_box;
                Float variance = max(m2 * inv_box - mean * mean, 0.f);
                Float sigma = sqrt(variance);
                Float lo = max(mean - Cfg::HistoryClamp::kSigmaScale * sigma, 0.f);
                Float hi = mean + Cfg::HistoryClamp::kSigmaScale * sigma;
                Float lum = luminance(hist);
                Float clamped = clamp(lum, lo, hi);
                return hist * (clamped / max(lum, 1e-4f));
            };
            prev_direct = clamp_history(prev_direct, box_m1_direct, box_m2_direct);
            prev_indirect = clamp_history(prev_indirect, box_m1_indirect, box_m2_indirect);
        };

        Float3 cur_color = make_float3(cur_direct.xyz() + cur_indirect.xyz());
        Float3 prev_color = prev_direct + prev_indirect;
        Float prev_lum = luminance(prev_color);
        Float cur_lum = luminance(cur_color);
        Float lum_ratio = prev_lum / max(cur_lum, 0.01f);
        
        Float ghosting_factor = 0.f;
        
        Float color_diff = length(cur_color - prev_color) / 
            max(length(cur_color) + length(prev_color), 0.001f);
        $if(cur_it.has_emission()) {
            Float t = saturate((color_diff - Cfg::Ghosting::kColorDiffThreshold * 0.7f) / 
                (Cfg::Ghosting::kColorDiffThreshold * 0.6f));
            ghosting_factor = max(ghosting_factor, t * t * (3.f - 2.f * t));
        };
        
        $if(prev_lum > Cfg::Ghosting::kBrightHistoryMinLum) {
            Float t = saturate((lum_ratio - Cfg::Ghosting::kBrightHistoryRatio * 0.6f) / 
                (Cfg::Ghosting::kBrightHistoryRatio * 0.6f));
            ghosting_factor = max(ghosting_factor, t * t * (3.f - 2.f * t));
        };
        
        $if(prev_lum > Cfg::Ghosting::kMotionLumMinPrev) {
            Float t_motion = saturate((motion_length - Cfg::Ghosting::kFastMotionThreshold * 0.6f) / 
                (Cfg::Ghosting::kFastMotionThreshold * 0.8f));
            Float t_lum = saturate((lum_ratio - Cfg::Ghosting::kMotionLumRatio * 0.6f) / 
                (Cfg::Ghosting::kMotionLumRatio * 0.6f));
            Float motion_ghosting = t_motion * t_motion * (3.f - 2.f * t_motion) * 
                                    t_lum * t_lum * (3.f - 2.f * t_lum);
            ghosting_factor = max(ghosting_factor, motion_ghosting);
        };
        
        Float max_history_for_motion = max(
            Cfg::Temporal::kMaxHistoryStatic - 
            tanh(motion_length / Cfg::Temporal::kMotionScaleDivisor) * 
            (Cfg::Temporal::kMaxHistoryStatic - Cfg::Temporal::kMaxHistoryFast),
            Cfg::Temporal::kMaxHistoryFast);
        Float base_history = min(prev_history + 1.f, max_history_for_motion);
        
        Float history_scale = 1.f - ghosting_factor * 0.85f;
        Float effective_history = max(base_history * history_scale, 1.f);
        
        Float new_history = ocarina::select(valid_history, effective_history, 1.f);
        
        Float base_alpha = 1.f / new_history;
        
        Float motion_boost = tanh(motion_length / Cfg::Temporal::kMotionAlphaDivisor) * 
                             Cfg::Temporal::kMotionAlphaScale;
        
        Float alpha = max(base_alpha, motion_boost);
        
        Float max_alpha = 0.95f;
        alpha = min(alpha, max_alpha);
        
        Bool use_history = valid_history;
        
        // Clamp luminance before squaring to prevent half overflow
        Float lum_direct_clamped = HalfSafeUtils::clamp_luminance(lum_direct);
        Float lum_indirect_clamped = HalfSafeUtils::clamp_luminance(lum_indirect);
        
        // Compute blended values in Float precision, then convert to RadType
        Float3 new_direct_f = ocarina::select(use_history,
            prev_direct + alpha * (make_float3(cur_direct.xyz()) - prev_direct),
            make_float3(cur_direct.xyz()));
        Float3 new_indirect_f = ocarina::select(use_history,
            prev_indirect + alpha * (make_float3(cur_indirect.xyz()) - prev_indirect),
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
            prev_m1_indirect + alpha * (lum_indirect_clamped - prev_m1_indirect),
            lum_indirect_clamped);
        Float new_m2_indirect = ocarina::select(use_history,
            prev_m2_indirect + alpha * (lum_indirect_clamped * lum_indirect_clamped - prev_m2_indirect),
            lum_indirect_clamped * lum_indirect_clamped);
            
        Float temporal_var_direct = VarianceUtils::compute_variance(new_m1_direct, new_m2_direct);
        Float temporal_var_indirect = VarianceUtils::compute_variance(new_m1_indirect, new_m2_indirect);
            
        SVGFDataDualVar output;
        output.illumi_direct = make_RadType4(new_direct, temporal_var_direct);
        output.illumi_indirect = make_RadType4(new_indirect, temporal_var_indirect);
        output.moments_direct = make_RadType4(new_m1_direct, new_m2_direct, new_history, 0.f);
        output.moments_indirect = make_RadType4(new_m1_indirect, new_m2_indirect, 0.f, 0.f);
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
    param.screen_short_edge = compute_screen_short_edge(input.resolution);
    CommandBatch ret;
    ret << variance_shader_(param).dispatch(input.resolution);
    return ret;
}

void VarianceEstimator::update_resolution(uint2 resolution) noexcept {}

}// namespace vision::svgf
