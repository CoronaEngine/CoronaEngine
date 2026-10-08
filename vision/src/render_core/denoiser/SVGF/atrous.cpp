#include "atrous.h"
#include "svgf.h"
#include "svgf_config.h"
#include "math/util.h"

namespace vision::svgf {

using Cfg = SVGFConfig;




void AtrousFilter::prepare() noexcept {
    uint num = pipeline()->pixel_num();
    init_buffer_zero(device(), guide_position_, num, "AtrousFilter::guide_position");
    init_buffer_zero(device(), guide_normal_, num, "AtrousFilter::guide_normal");
    init_buffer_zero(device(), guide_identity_, num, "AtrousFilter::guide_identity");
    init_buffer_zero(device(), guide_surface_, num, "AtrousFilter::guide_surface");
    init_buffer_zero(device(), temp_buffer_direct_, num, "AtrousFilter::temp_buffer_direct");
    init_buffer_zero(device(), temp_buffer_indirect_, num, "AtrousFilter::temp_buffer_indirect");
}

void AtrousFilter::compile() noexcept {
    compile_combined();
}


void AtrousFilter::compile_combined() noexcept {
Pipeline *pipeline_ref = pipeline();
// Extract the immutable spatial guide once, rather than decoding path records
// and querying scene geometry at every tap in every wavelet iteration.
Kernel prepare_guide = [pipeline_ref](Var<CombinedAtrousParam> param) {
    Uint idx = dispatch_id();
    StableGeometryGuide guide(param, idx, param.visibility_buffer.read(idx));
    Bool sky = PixelStateUtils::is_sky(guide.hit);
    Float3 position = make_float3(0.f), normal = make_float3(0.f);
    Bool emissive = false;
    $if(!sky) {
        Interaction it = pipeline_ref->geometry().compute_surface_interaction(guide.hit, false);
        guide.apply(it);
        position = it.pos; normal = it.ng;
        emissive = PixelStateUtils::is_emissive(pipeline_ref, guide.hit);
    };
    param.guide_position.write(idx, make_float4(position, cast<float>(sky)));
    param.guide_normal.write(idx, make_float4(normal, cast<float>(emissive)));
    param.guide_identity.write(idx, make_uint4(cast<uint>(guide.valid), guide.branch, guide.sequence, guide.identity));
    param.guide_surface.write(idx, make_uint4(guide.depth, guide.material, guide.hit.inst_id, cast<uint>(guide.layered)));
};
guide_shader_ = device().compile(prepare_guide, "SVGF-AtrousGuide");
// Canonical SVGF a-trous wavelet pass (Schied et al. 2017): a deterministic
// 5x5 cross-bilateral B-spline kernel with depth/normal/luminance edge stopping.
//
// P0 rewrite rationale: the previous kernel used a per-frame RANDOMLY ROTATED
// sparse 12-tap Poisson disk. Because the a-trous output is never fed back into
// the temporal history buffer (history is written only by the variance stage),
// that per-frame rotation produced shimmering, blotchy "blocky" noise that
// temporal accumulation could not average out. A fixed, dense, deterministic
// kernel is temporally stable by construction. Anisotropic stretching and the
// variance-driven adaptive radius were removed for the same reason (they made
// every pixel's footprint frame/varying-dependent); they can be re-added later
// on top of a stable base if needed.
// Retain the inexpensive original path when Stable Planes are disabled.
auto compile_filter = [&]<bool cached>() {
Kernel kernel = [&, pipeline_ref](Var<CombinedAtrousParam> param) {
    Int2 screen_size = make_int2(dispatch_dim().xy());
    Int2 cur_pixel = make_int2(dispatch_idx().xy());
    Uint cur_idx = dispatch_id();

    auto center_guide = load_spatial_guide<cached>(param, cur_idx);
    TriangleHitVar center_hit = center_guide.hit;
    RadType4Var direct_center = param.direct_src.read(cur_idx);
    RadType4Var indirect_center = param.indirect_src.read(cur_idx);

    $if(!PixelStateUtils::is_sky(center_hit)) {
        Interaction center_it{false};
        if constexpr (!cached) center_it = pipeline_ref->geometry().compute_surface_interaction(center_hit, false);
        center_guide.apply(center_it);

        Float lum_center_direct = HalfSafeUtils::clamp_luminance(luminance(direct_center.xyz()));
        Float lum_center_indirect = HalfSafeUtils::clamp_luminance(luminance(indirect_center.xyz()));

        // ReSTIR's mixed lighting channels need a noise floor at fine scales,
        // but keeping it constant across wide passes erases converged shadows.
        // Measured variance still widens the filter where noise remains.
        Float floor_scale = ocarina::select(
            param.channel_kind == static_cast<uint>(RealTimeDenoiseInput::ChannelKind::DirectIndirect),
            1.f / cast<float>(param.step_size), 1.f);
        Float floor_variance_scale = floor_scale * floor_scale;
        Float variance_epsilon = Cfg::Epsilon::kVariance * floor_variance_scale;

        // Variance must be 3x3 Gaussian pre-filtered before deriving the luminance
        // edge-stopping width (Schied et al. 2017). Using raw per-pixel variance makes
        // phi_l noisy, and at large a-trous steps that noisy edge-stopping collapses to
        // axis-aligned taps -> visible "grid"/streak lines. Smoothing the variance makes
        // phi_l spatially coherent, removing the streaks; it also lets noisy specular
        // highlights borrow a larger phi_l from neighbours so they actually get filtered.
        constexpr float kGauss3[3] = {0.25f, 0.5f, 0.25f};// separable 1D -> {1,2,1}/{2,4,2}/{1,2,1}
        Float var_sum_direct = 0.f;
        Float var_sum_indirect = 0.f;
        Float var_gw_sum = 0.f;
        for (int gy = -1; gy <= 1; ++gy) {
            for (int gx = -1; gx <= 1; ++gx) {
                float gw = kGauss3[gx + 1] * kGauss3[gy + 1];
                Int2 gp = cur_pixel + make_int2(gx, gy);
                $if(all(gp >= 0) && all(gp < screen_size)) {
                    Uint gidx = cast<uint>(gp.y) * cast<uint>(screen_size.x) + cast<uint>(gp.x);
                    auto variance_guide = load_spatial_guide<cached>(param, gidx);
                    $if(param.layered == 0u || center_guide.compatible(variance_guide)) {
                    var_sum_direct += max(Float(param.direct_src.read(gidx).w), variance_epsilon) * gw;
                    var_sum_indirect += max(Float(param.indirect_src.read(gidx).w), variance_epsilon) * gw;
                    var_gw_sum += gw;
                    };
                };
            }
        }
        Float inv_var_gw = 1.f / max(var_gw_sum, 1e-4f);
        Float var_direct_clamped = max(var_sum_direct * inv_var_gw, variance_epsilon);
        Float var_indirect_clamped = max(var_sum_indirect * inv_var_gw, variance_epsilon);
        // The absolute filter floors are specified in radiance units. Convert
        // them with the current reflectance guide, just like the signal itself.
        // Otherwise a dark guide (e.g. 0.03) makes the effective radiance floor
        // ~33 times smaller and leaves fine noise even after spatial filtering.
        // A scalar guide is a luminance approximation for coloured reflectance;
        // RGB illumination and texture reconstruction remain unchanged.
        auto metadata = param.svgf_buffer.read(cur_idx).moments_indirect;
        // A producer may write a zero signal with w=0 when demodulation is
        // bypassed. Only positive metadata represents a reflectance scale.
        Float scale_direct = ocarina::select(metadata.z > 0.f, Float(metadata.z), 1.f);
        Float scale_indirect = ocarina::select(metadata.w > 0.f, Float(metadata.w), 1.f);
        Float phi_l_direct = LuminanceWeightUtils::compute_phi_l(param.l_phi, var_direct_clamped,
            Cfg::Atrous::kMinVariance * floor_variance_scale / (scale_direct * scale_direct),
            Cfg::Atrous::kMinPhi * floor_scale / scale_direct);
        Float phi_l_indirect = LuminanceWeightUtils::compute_phi_l(param.l_phi, var_indirect_clamped,
            Cfg::Atrous::kMinVariance * floor_variance_scale / (scale_indirect * scale_indirect),
            Cfg::Atrous::kMinPhi * floor_scale / scale_indirect);

        // Center tap: kernel weight h(0)*h(0), geometry/luminance weight == 1.
        constexpr float kW0 = Cfg::Atrous::kBSpline1D[0] * Cfg::Atrous::kBSpline1D[0];
        Float3 sum_direct = direct_center.xyz() * kW0;
        Float3 sum_indirect = indirect_center.xyz() * kW0;
        Float weight_sum_direct = kW0;
        Float weight_sum_indirect = kW0;
        // The Gaussian above controls only phi_l. Propagate variance with the
        // same accepted samples as RGB; otherwise rejected neighbours inject
        // uncertainty through the centre term and widen every later pass.
        Float variance_sum_direct = max(Float(direct_center.w), variance_epsilon) * (kW0 * kW0);
        Float variance_sum_indirect = max(Float(indirect_center.w), variance_epsilon) * (kW0 * kW0);

        auto accumulate_tap = [&](int dx, int dy, float h) {
            // h is the host-side separable B-spline kernel weight for this tap.
            Int2 p = cur_pixel + make_int2(dx, dy) * param.step_size;

            $if(all(p >= 0) && all(p < screen_size)) {
                Uint idx = cast<uint>(p.y) * cast<uint>(screen_size.x) + cast<uint>(p.x);

                RadType4Var direct_neighbor = param.direct_src.read(idx);
                RadType4Var indirect_neighbor = param.indirect_src.read(idx);

                auto neighbor_guide = load_spatial_guide<cached>(param, idx);
                TriangleHitVar neighbor_hit = neighbor_guide.hit;
                Bool neighbor_is_sky = PixelStateUtils::is_sky(neighbor_hit);

                Float boundary_weight;
                if constexpr (cached) {
                    boundary_weight = ocarina::select(neighbor_is_sky || center_guide.emissive != neighbor_guide.emissive, 0.f, 1.f);
                } else {
                    boundary_weight = BoundaryUtils::compute_boundary_weight(pipeline_ref, center_hit, neighbor_hit);
                }

                Float w_geo = 0.f;
                $if(!neighbor_is_sky && boundary_weight > 0.f) {
                    Interaction neighbor_it{false};
                    if constexpr (!cached) neighbor_it = pipeline_ref->geometry().compute_surface_interaction(neighbor_hit, false);
                    neighbor_guide.apply(neighbor_it);
                    w_geo = GeometryWeightUtils::compute_depth_weight(
                        center_it.pos, neighbor_it.pos, center_it.ng, param.z_phi) *
                        GeometryWeightUtils::compute_normal_weight(
                            ocarina::select(param.use_shading_normal != 0u,
                                make_float3(param.svgf_buffer.read(cur_idx).surface_normal.xyz()), center_it.ng),
                            ocarina::select(param.use_shading_normal != 0u,
                                make_float3(param.svgf_buffer.read(idx).surface_normal.xyz()), neighbor_it.ng), param.n_phi);
                };
                w_geo *= boundary_weight * cast<float>(center_guide.compatible(neighbor_guide));

                Float lum_neighbor_direct = HalfSafeUtils::clamp_luminance(luminance(direct_neighbor.xyz()));
                Float lum_neighbor_indirect = HalfSafeUtils::clamp_luminance(luminance(indirect_neighbor.xyz()));

                Float w_direct = h * w_geo *
                    LuminanceWeightUtils::compute_variance_guided(lum_center_direct, lum_neighbor_direct, phi_l_direct);
                // Specular illumination depends on the surface BRDF as well as
                // lighting. Borrowing demodulated values across objects can turn
                // a small reflectance guide into a bright halo after remodulation.
                Float w_specular_geo = ocarina::select(
                    param.channel_kind != 0u || center_hit.inst_id == neighbor_hit.inst_id, w_geo, 0.f);
                Float w_indirect = h * w_specular_geo *
                    LuminanceWeightUtils::compute_variance_guided(lum_center_indirect, lum_neighbor_indirect, phi_l_indirect);

                Float var_neighbor_direct = max(Float(direct_neighbor.w), variance_epsilon);
                Float var_neighbor_indirect = max(Float(indirect_neighbor.w), variance_epsilon);

                sum_direct += direct_neighbor.xyz() * w_direct;
                sum_indirect += indirect_neighbor.xyz() * w_indirect;
                variance_sum_direct += var_neighbor_direct * w_direct * w_direct;
                variance_sum_indirect += var_neighbor_indirect * w_indirect * w_indirect;
                weight_sum_direct += w_direct;
                weight_sum_indirect += w_indirect;
            };
        };

        // Host-unrolled 5x5 separable B-spline {0.375, 0.25, 0.0625}; center handled above.
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                if (dx == 0 && dy == 0) { continue; }
                float h = Cfg::Atrous::kBSpline1D[dx < 0 ? -dx : dx] *
                          Cfg::Atrous::kBSpline1D[dy < 0 ? -dy : dy];
                accumulate_tap(dx, dy, h);
            }
        }

        Float3 filtered_direct = sum_direct / max(weight_sum_direct, Cfg::Epsilon::kWeight);
        Float3 filtered_indirect = sum_indirect / max(weight_sum_indirect, Cfg::Epsilon::kWeight);
        Float out_var_direct = VarianceUtils::propagate_filtered_variance(variance_sum_direct, weight_sum_direct * weight_sum_direct);
        Float out_var_indirect = VarianceUtils::propagate_filtered_variance(variance_sum_indirect, weight_sum_indirect * weight_sum_indirect);

        param.direct_dst.write(cur_idx, make_RadType4(make_float4(filtered_direct, out_var_direct)));
        param.indirect_dst.write(cur_idx, make_RadType4(make_float4(filtered_indirect, out_var_indirect)));

        // === SVGF colour-history feedback (Schied et al. 2017) ===
        // Feed the FIRST a-trous iteration's output back as the colour history so
        // spatial filtering compounds across frames instead of being recomputed from
        // scratch every frame. Without this the temporal history only ever holds the
        // raw 1-spp accumulation, so residual noise can only be reduced by history
        // length -- the root cause of the residual "blocky" noise. Only iteration 0 is
        // fed back (a mild 5x5 blur) to avoid the over-blur/lag of feeding back the
        // full multi-iteration result. Moments (M1/M2/history) are intentionally left
        // untouched: they must keep tracking the RAW signal so variance still drives
        // edge stopping. svgf_buffer is not read by this pass (only input buffers are),
        // so this center-only write is race-free. Feedback is disabled by default
        // to avoid compounding spatial blur in the temporal illumination history.
        //
        // illumi_direct = DIFFUSE channel, illumi_indirect = SPECULAR channel. The
        // specular channel is NOT fed back by default (kFeedbackSpecular=false): feeding
        // back a view-dependent specular signal over-blurs and ghosts highlights.
        if constexpr (Cfg::Atrous::kFeedbackEnabled) {
            $if(param.write_history != 0u) {
                SVGFDataDualVar hist = param.svgf_buffer.read(cur_idx);
                hist.illumi_direct = make_RadType4(make_RadType3(filtered_direct), out_var_direct);
                if constexpr (Cfg::Atrous::kFeedbackSpecular) {
                    hist.illumi_indirect = make_RadType4(make_RadType3(filtered_indirect), out_var_indirect);
                }
                param.svgf_buffer.write(cur_idx, hist);
            };
        }
    }
    $else {
        param.direct_dst.write(cur_idx, direct_center);
        param.indirect_dst.write(cur_idx, indirect_center);
    };
};

    return device().compile(kernel, cached ? "SVGF-AtrousFilter-Cached" : "SVGF-AtrousFilter-BSpline");
    };
    combined_shader_ = compile_filter.operator()<true>();
    legacy_combined_shader_ = compile_filter.operator()<false>();
}




CommandBatch AtrousFilter::dispatch_guide(RealTimeDenoiseInput &input) noexcept {
    CombinedAtrousParam param;
    bind_stable_planes(param, input);
    bind_guides(param);
    param.visibility_buffer = input.visibility.descriptor();
    CommandBatch ret;
    ret << guide_shader_(param).dispatch(input.resolution);
    return ret;
}

CommandBatch AtrousFilter::dispatch_combined(vision::RealTimeDenoiseInput &input,
                                         uint step_width, uint iteration, bool use_shading_normal) noexcept {
    CombinedAtrousParam param;
    bind_stable_planes(param, input);
    param.use_shading_normal = use_shading_normal;
    bind_guides(param);
    
    bool read_from_temp = (iteration % 2 == 1);
    if (!read_from_temp) {
        param.direct_src = input.direct.descriptor();
        param.direct_dst = temp_buffer_direct_.descriptor();
        param.indirect_src = input.indirect.descriptor();
        param.indirect_dst = temp_buffer_indirect_.descriptor();
    } else {
        param.direct_src = temp_buffer_direct_.descriptor();
        param.direct_dst = input.direct.descriptor();
        param.indirect_src = temp_buffer_indirect_.descriptor();
        param.indirect_dst = input.indirect.descriptor();
    }
    
    param.visibility_buffer = input.visibility.descriptor();
    param.svgf_buffer = svgf_->svgf_buffer_cur(input.frame_index, input.layer_index).descriptor();
    param.camera_pos = input.camera_pos;

    float l_phi = svgf_->sigma_rt();
    if (input.channel_kind == RealTimeDenoiseInput::ChannelKind::DirectIndirect) {
        l_phi *= Cfg::Atrous::kReSTIRLPhiMultiplier;
    }
    float n_phi = svgf_->sigma_normal();

    if (input.channel_kind != RealTimeDenoiseInput::ChannelKind::DirectIndirect &&
        step_width >= Cfg::Atrous::kLargeStepThreshold) {
        l_phi *= Cfg::Atrous::kLargeStepLPhiMultiplier;
        n_phi *= Cfg::Atrous::kLargeStepNPhiMultiplier;
    }

    param.l_phi = l_phi;
    param.n_phi = n_phi;
    param.z_phi = svgf_->sigma_depth();
    param.step_size = static_cast<int>(step_width);
    param.iteration = iteration;
    param.frame_index = input.frame_index;
    // Feed back only the first a-trous iteration as colour history (Schied 2017).
    param.write_history = (iteration == 0u) ? 1u : 0u;
    param.channel_kind = static_cast<uint>(input.channel_kind);
    
    CommandBatch ret;
    if (input.use_stable_planes) {
        ret << combined_shader_(param).dispatch(input.resolution);
    } else {
        ret << legacy_combined_shader_(param).dispatch(input.resolution);
    }
    return ret;
}


void AtrousFilter::update_resolution(uint2 resolution) noexcept {
    uint num = resolution.x * resolution.y;
    init_buffer_zero(device(), guide_position_, num, "AtrousFilter::guide_position");
    init_buffer_zero(device(), guide_normal_, num, "AtrousFilter::guide_normal");
    init_buffer_zero(device(), guide_identity_, num, "AtrousFilter::guide_identity");
    init_buffer_zero(device(), guide_surface_, num, "AtrousFilter::guide_surface");
    init_buffer_zero(device(), temp_buffer_direct_, num, "AtrousFilter::temp_buffer_direct");
    init_buffer_zero(device(), temp_buffer_indirect_, num, "AtrousFilter::temp_buffer_indirect");
}

}// namespace vision::svgf
