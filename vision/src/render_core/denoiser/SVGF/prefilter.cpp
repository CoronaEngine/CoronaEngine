#include "prefilter.h"
#include "svgf.h"
#include "svgf_config.h"

namespace vision::svgf {

using Cfg = SVGFConfig;

void Prefilter::prepare() noexcept {}

void Prefilter::compile() noexcept {
Pipeline *pipeline_ref = pipeline();
    
auto soft_clamp_asinh = [](Float x, Float threshold, Float softness, Float retain) -> Float {
    Float safe_threshold = max(threshold, 0.1f);
    Float safe_x = max(x, 0.f);
    Float ratio = safe_x / safe_threshold;
        
    Float compressed = ocarina::select(ratio < 1.f,
        ratio,
        1.f + asinh((ratio - 1.f) * softness) / max(softness, 0.1f) * retain);
        
    return safe_threshold * max(compressed, 0.f);
};

auto compute_spatial_weight = [](Float history) -> Float {
    Float t = saturate((history - 1.f) / (Cfg::VarianceBlend::kHistoryThreshold - 1.f));
    return 1.f - t * t * (3.f - 2.f * t);
};
    
    Kernel kernel = [&, pipeline_ref, soft_clamp_asinh, compute_spatial_weight](Var<PrefilterParam> param) {
        Int2 screen_size = make_int2(dispatch_dim().xy());
        Int2 pixel = make_int2(dispatch_idx().xy());
        Uint idx = dispatch_id();

        TriangleHitVar center_hit = param.visibility_buffer.read(idx);

        $if(!PixelStateUtils::is_sky(center_hit)) {
            SVGFDataDualVar svgf_data = param.svgf_buffer.read(idx);
            RadType3Var center_direct = svgf_data->illumination_direct();
            RadType3Var center_indirect = svgf_data->illumination_indirect();
            RadTypeVar temporal_var_direct = svgf_data->variance_direct();
            RadTypeVar temporal_var_indirect = svgf_data->variance_indirect();
            Float history = svgf_data->history_count();

            RadType3Var output_direct = center_direct;
            RadType3Var output_indirect = center_indirect;
            Float output_variance_direct = temporal_var_direct;
            Float output_variance_indirect = temporal_var_indirect;

            $if(!PixelStateUtils::is_emissive(pipeline_ref, center_hit)) {
                Interaction it = pipeline_ref->geometry().compute_surface_interaction(center_hit, false);
                // Clamp center luminance for half precision safety
                Float center_lum_direct = HalfSafeUtils::clamp_luminance(luminance(center_direct));
                Float center_lum_indirect = HalfSafeUtils::clamp_luminance(luminance(center_indirect));

                Float3 spatial_sum_direct = make_float3(0.f);
                Float3 spatial_sum_indirect = make_float3(0.f);
                Float spatial_weight_sum = 0.f;
                
                Float3 sum_direct = make_float3(0.f);
                Float3 sum_indirect = make_float3(0.f);
                Float sum_variance_direct = 0.f;
                Float sum_variance_indirect = 0.f;
                Float sum_m1_direct = 0.f;
                Float sum_m2_direct = 0.f;
                Float sum_m1_indirect = 0.f;
                Float sum_m2_indirect = 0.f;
                Float weight_sum_direct = 0.f;
                Float weight_sum_indirect = 0.f;
                Float weight_sum_geo = 0.f;

                $for(dy, -1, 2) {
                    $for(dx, -1, 2) {
                        Int2 p = pixel + make_int2(dx, dy);

                        $if(all(p >= 0) && all(p < screen_size)) {
                            Uint p_idx = safe_pixel_index(p, screen_size);
                            
                            SVGFDataDualVar n_svgf = param.svgf_buffer.read(p_idx);
                            RadType3Var n_direct = n_svgf->illumination_direct();
                            RadType3Var n_indirect = n_svgf->illumination_indirect();

                            TriangleHitVar n_hit = param.visibility_buffer.read(p_idx);
                            Bool n_is_sky = PixelStateUtils::is_sky(n_hit);
                            
                            Float boundary_weight = BoundaryUtils::compute_boundary_weight(
                                pipeline_ref, center_hit, n_hit);

                            Float kernel_w = ocarina::select(dx == 0 && dy == 0, 0.25f,
                                             ocarina::select(dx == 0 || dy == 0, 0.125f, 0.0625f));

                            Float w_geo = 1.f;
                            $if(!n_is_sky) {
                                Interaction n_it = pipeline_ref->geometry().compute_surface_interaction(n_hit, false);
                                w_geo = GeometryWeightUtils::compute_geometry_weight(
                                    it.pos, it.ng, n_it.pos, n_it.ng,
                                    Cfg::GeometryWeight::kPrefilterNormalPower,
                                    Cfg::GeometryWeight::kPrefilterDepthScale,
                                    Cfg::GeometryWeight::kEpsilon);
                            };
                            w_geo = GeometryWeightUtils::handle_sky_weight(false, n_is_sky, w_geo);
                            
                            w_geo *= boundary_weight;
                            
                            $if((dx != 0 || dy != 0) && w_geo > 0.1f) {
                                spatial_sum_direct += n_direct * w_geo;
                                spatial_sum_indirect += n_indirect * w_geo;
                                spatial_weight_sum += w_geo;
                            };

                            Float w = kernel_w * w_geo;
                            // Clamp luminance to prevent M2 overflow in half precision
                            Float n_lum_direct = HalfSafeUtils::clamp_luminance(luminance(n_direct));
                            Float n_lum_indirect = HalfSafeUtils::clamp_luminance(luminance(n_indirect));

                            Float w_lum_direct = LuminanceWeightUtils::compute_normalized(
                                center_lum_direct, n_lum_direct, Cfg::Prefilter::kLuminanceSigma);
                            Float w_lum_indirect = LuminanceWeightUtils::compute_normalized(
                                center_lum_indirect, n_lum_indirect, Cfg::Prefilter::kLuminanceSigma);
                            
                            Float w_total_direct = w * w_lum_direct;
                            Float w_total_indirect = w * w_lum_indirect;
                            
                            sum_direct += n_direct * w_total_direct;
                            sum_indirect += n_indirect * w_total_indirect;
                            
                            weight_sum_direct += w_total_direct;
                            weight_sum_indirect += w_total_indirect;
                            
                            sum_variance_direct += n_svgf->variance_direct() * w;
                            sum_variance_indirect += n_svgf->variance_indirect() * w;

                            sum_m1_direct += n_lum_direct * w;
                            sum_m2_direct += n_lum_direct * n_lum_direct * w;
                            sum_m1_indirect += n_lum_indirect * w;
                            sum_m2_indirect += n_lum_indirect * n_lum_indirect * w;
                            weight_sum_geo += w;
                        };
                    };
                };

                RadType3Var firefly_clamped_direct = center_direct;
                RadType3Var firefly_clamped_indirect = center_indirect;
                
                $if(spatial_weight_sum > 0.5f) {
                    Float inv_spatial_w = 1.f / max(spatial_weight_sum, 1e-4f);
                    Float safe_spatial_mean_direct = max(luminance(spatial_sum_direct * inv_spatial_w), 0.01f);
                    Float safe_spatial_mean_indirect = max(luminance(spatial_sum_indirect * inv_spatial_w), 0.01f);
                    
                    Float isolation_ratio_direct = center_lum_direct / safe_spatial_mean_direct;
                    Float isolation_ratio_indirect = center_lum_indirect / safe_spatial_mean_indirect;
                    
                    Float spatial_thresh_direct = safe_spatial_mean_direct * Cfg::Firefly::kSpatialIsolationThreshold;
                    Float spatial_thresh_indirect = safe_spatial_mean_indirect * Cfg::Firefly::kSpatialIsolationThreshold;
                    
                    Float temporal_thresh_direct = spatial_thresh_direct;
                    Float temporal_thresh_indirect = spatial_thresh_indirect;
                    
                    $if(history > Cfg::Prefilter::kFireflyHistoryThreshold) {
                        Float k = lerp(Cfg::Firefly::kSigmaMultiplierMax, 
                                      Cfg::Firefly::kSigmaMultiplierMin, 
                                      saturate(history / Cfg::Temporal::kMaxHistoryStatic));
                        
                        $if(temporal_var_direct > Cfg::Variance::kMinVarianceConsistent) {
                            temporal_thresh_direct = max(svgf_data->first_moment_direct(), RadTypeVar (0.01f)) +
                                k * max(sqrt(temporal_var_direct), Cfg::Firefly::kMinSigma);
                        };
                        
                        $if(temporal_var_indirect > Cfg::Variance::kMinVarianceConsistent) {
                            temporal_thresh_indirect = max(svgf_data->first_moment_indirect(),  RadTypeVar (0.01f)) +
                                k * 0.8f * max(sqrt(temporal_var_indirect), Cfg::Firefly::kMinSigma);
                        };
                    };
                    
                    Float combined_thresh_direct = max(lerp(temporal_thresh_direct, spatial_thresh_direct,
                        ocarina::select(isolation_ratio_direct > Cfg::Firefly::kSpatialIsolationThreshold,
                            Cfg::Firefly::kSpatialWeightIsolated, Cfg::Firefly::kSpatialWeightNormal)), 0.1f);
                    Float combined_thresh_indirect = max(lerp(temporal_thresh_indirect, spatial_thresh_indirect,
                        ocarina::select(isolation_ratio_indirect > Cfg::Firefly::kSpatialIsolationThreshold,
                            Cfg::Firefly::kSpatialWeightIsolated, Cfg::Firefly::kSpatialWeightNormal)), 0.1f);
                    
                    $if(isolation_ratio_direct > Cfg::Firefly::kSpatialIsolationThreshold) {
                        Float scale = soft_clamp_asinh(center_lum_direct, combined_thresh_direct, 
                            Cfg::Firefly::kSoftnessDefault, Cfg::Firefly::kRetainRatio) / max(center_lum_direct, 0.001f);
                        firefly_clamped_direct = center_direct *  RadTypeVar(min(scale, 1.f));
                    };
                    
                    $if(isolation_ratio_indirect > Cfg::Firefly::kSpatialIsolationThreshold) {
                        Float scale = soft_clamp_asinh(center_lum_indirect, combined_thresh_indirect, 
                            Cfg::Firefly::kSoftnessIndirect, Cfg::Firefly::kRetainRatio) / max(center_lum_indirect, 0.001f);
                        firefly_clamped_indirect = center_indirect *  RadTypeVar (min(scale, 1.f));
                    };
                };

                Float inv_w_direct = 1.f / max(weight_sum_direct, 1e-4f);
                Float inv_w_indirect = 1.f / max(weight_sum_indirect, 1e-4f);
                Float inv_w_geo = 1.f / max(weight_sum_geo, 1e-4f);
                
                Float3 filtered_direct = sum_direct * inv_w_direct;
                Float3 filtered_indirect = sum_indirect * inv_w_indirect;

                Float spatial_m1_direct = sum_m1_direct * inv_w_geo;
                Float spatial_m1_indirect = sum_m1_indirect * inv_w_geo;
                Float spatial_variance_direct = max(sum_m2_direct * inv_w_geo - spatial_m1_direct * spatial_m1_direct, 0.f);
                Float spatial_variance_indirect = max(sum_m2_indirect * inv_w_geo - spatial_m1_indirect * spatial_m1_indirect, 0.f);

                Float spatial_weight = compute_spatial_weight(history);
                Float history_factor = saturate((history - Cfg::VarianceBlend::kSoftTransitionStart) / 
                    (Cfg::VarianceBlend::kSoftTransitionEnd - Cfg::VarianceBlend::kSoftTransitionStart));
                history_factor = history_factor * history_factor * (3.f - 2.f * history_factor);
                Float max_allowed = lerp(Cfg::VarianceBlend::kMinSpatialWeight, 
                                         Cfg::VarianceBlend::kMaxSpatialWeight, 
                                         history_factor);
                spatial_weight = min(spatial_weight, max_allowed);
                
                Float lum_floor_direct = center_lum_direct * Cfg::VarianceBlend::kLumFloorScale;
                Float lum_floor_indirect = center_lum_indirect * Cfg::VarianceBlend::kLumFloorScale;
                
                Float enhanced_spatial_var_direct = max(spatial_variance_direct, lum_floor_direct);
                Float enhanced_spatial_var_indirect = max(spatial_variance_indirect, lum_floor_indirect);
                
                Float blended_var_direct = lerp(temporal_var_direct, enhanced_spatial_var_direct, spatial_weight);
                Float blended_var_indirect = lerp(temporal_var_indirect, enhanced_spatial_var_indirect, spatial_weight);
                
                output_variance_direct = max(blended_var_direct, Cfg::Variance::kMinVarianceConsistent);
                output_variance_indirect = max(blended_var_indirect, Cfg::Variance::kMinVarianceConsistent);

                // Disocclusion variance boost (canonical SVGF). Freshly disoccluded / low-history
                // pixels have an unreliable, too-small temporal variance, so the variance-guided
                // a-trous weight (phi_l = l_phi*sqrt(var)+min_phi) refuses to blend them and noise
                // / fireflies survive. For low history, raise the variance floor (and boost it) so
                // the spatial filter is allowed to smooth aggressively until temporal history is
                // rebuilt. apply_min_variance degrades to max(var, kMinVarianceConsistent) when the
                // pixel is NOT low-history, matching the line above (no effect on stable pixels).
                Bool low_history = history < Cfg::Variance::kHistoryThreshold;
                output_variance_direct = VarianceUtils::apply_min_variance(output_variance_direct, low_history);
                output_variance_indirect = VarianceUtils::apply_min_variance(output_variance_indirect, low_history);

                Float radiance_blend = spatial_weight * Cfg::Prefilter::kMaxRadianceBlend;
                
                output_direct = firefly_clamped_direct + radiance_blend * (filtered_direct - firefly_clamped_direct);
                output_indirect = firefly_clamped_indirect + radiance_blend * (filtered_indirect - firefly_clamped_indirect);
            };

            param.radiance_direct.write(idx, make_RadType4(make_float4(output_direct, output_variance_direct)));
            param.radiance_indirect.write(idx, make_RadType4(make_float4(output_indirect, output_variance_indirect)));
        };
    };

    prefilter_shader_ = device().compile(kernel, "SVGF-Prefilter-SpatioTemporalFirefly");
}

CommandBatch Prefilter::dispatch(RealTimeDenoiseInput &input) noexcept {
    PrefilterParam param;
    param.radiance_direct = input.direct.descriptor();
    param.radiance_indirect = input.indirect.descriptor();
    param.svgf_buffer = svgf_->svgf_buffer_cur(input.frame_index).descriptor();
    param.visibility_buffer = input.visibility.descriptor();
    param.camera_pos = input.camera_pos;

    CommandBatch ret;
    ret << prefilter_shader_(param).dispatch(input.resolution);
    return ret;
}

}// namespace vision::svgf
