#include "poisson.h"
#include "svgf.h"
#include "svgf_config.h"
#include "math/util.h"

namespace vision::svgf {

using Cfg = SVGFConfig;




void PoissonFilter::prepare() noexcept {
    uint num = pipeline()->pixel_num();
    init_buffer_zero(device(), temp_buffer_direct_, num, "PoissonFilter::temp_buffer_direct");
    init_buffer_zero(device(), temp_buffer_indirect_, num, "PoissonFilter::temp_buffer_indirect");
}

void PoissonFilter::prepare_guides(uint num, bool coverage_only) noexcept {
    if (guides_ready(num, coverage_only)) { return; }
    if (num == 0u) {
        guide_position_ = {}; guide_normal_ = {}; guide_shading_normal_ = {}; guide_albedo_ = {};
        guide_identity_ = {}; guide_surface_ = {};
        return;
    }
    init_buffer_zero(device(), guide_position_, num, "PoissonFilter::guide_position");
    init_buffer_zero(device(), guide_normal_, num, "PoissonFilter::guide_normal");
    if (coverage_only) {
        guide_shading_normal_ = {}; guide_albedo_ = {};
    } else {
        init_buffer_zero(device(), guide_shading_normal_, num, "PoissonFilter::guide_shading_normal");
        init_buffer_zero(device(), guide_albedo_, num, "PoissonFilter::guide_albedo");
    }
    init_buffer_zero(device(), guide_identity_, num, "PoissonFilter::guide_identity");
    init_buffer_zero(device(), guide_surface_, num, "PoissonFilter::guide_surface");
}

void PoissonFilter::compile() noexcept {
    compile_combined();
}


void PoissonFilter::compile_combined() noexcept {
Pipeline *pipeline_ref = pipeline();
// Extract the immutable spatial guide once, rather than decoding path records
// and querying scene geometry at every tap in every spatial iteration.
auto write_guide = [pipeline_ref](Var<CombinedPoissonParam> &param, Uint idx) {
    StableGeometryGuide guide(param, idx, param.visibility_buffer.read(idx));
    Bool sky = PixelStateUtils::is_sky(guide.hit);
    Float3 position = make_float3(0.f), normal = make_float3(0.f);
    Bool emissive = false;
    $if(!sky) {
        $if(param.layered != 0u) {
            position = guide.position; normal = guide.normal;
        } $else {
            Interaction it = pipeline_ref->geometry().compute_surface_interaction(guide.hit, false);
            guide.apply(it);
            position = it.pos; normal = it.ng;
        };
        emissive = PixelStateUtils::is_emissive(pipeline_ref, guide.hit);
    };
    param.guide_shading_normal.write(idx, make_float4(param.svgf_buffer.read(idx).surface_normal));
    param.guide_position.write(idx, make_float4(position, cast<float>(sky)));
    param.guide_normal.write(idx, make_float4(normal, cast<float>(emissive)));
    param.guide_identity.write(idx, make_uint4(cast<uint>(guide.valid), guide.branch, guide.sequence, guide.identity));
    param.guide_surface.write(idx, make_uint4(guide.depth, guide.material, guide.hit.inst_id, cast<uint>(guide.layered)));
};
Kernel prepare_guide = [write_guide](Var<CombinedPoissonParam> param) {
    skip_empty_tile(param);
    Uint idx = dispatch_id();
    $if(param.layered != 0u) {
        $if(param.stable_planes.read(idx).valid == 0u) { $return(); };
    };
    write_guide(param, idx);
};
guide_shader_ = device().compile(prepare_guide, "SVGF-PoissonGuide");
// One block prepares one 16x16 tile, one pixel per thread. All 256 lanes
// participate in the barrier/vote, including pixels outside a partial tile.
Kernel prepare_layer_guide = [pipeline_ref](Var<CombinedPoissonParam> param) {
    configure_block(256u);
    Uint tile = dispatch_id() / 256u, local = dispatch_id() % 256u;
    Uint tiles_x = (param.width + 15u) / 16u;
    Uint x = (tile % tiles_x) * 16u + local % 16u;
    Uint y = (tile / tiles_x) * 16u + local / 16u;
    Bool active = false;
    $if(local == 0u) { param.active_tiles.write(tile, 0u); };
    // Only this block owns the tile. Its reset must precede every warp's vote.
    synchronize_block();
    $if(x < param.width && y < param.height) {
        Uint idx = y * param.width + x;
        // Read validity before geometry: sparse layers must not fetch the
        // full stable-plane payload for pixels without an endpoint.
        Bool has_plane = param.stable_planes.read(idx).valid != 0u;
        Bool valid = false;
        $if(has_plane) {
            auto plane = param.stable_planes.read(idx);
            valid = plane.surface.hit->is_hit() && plane.surface.hit.inst_id != InvalidUI32;
            $if(valid) {
                // This entry point only handles full stable-plane records.
                // Decode once before any radiance/scratch writes can alias them.
                Float3 position = plane.surface.virtual_position.as_vec3();
                Float3 normal = plane.surface.virtual_geometric_normal.as_vec3();
                Float3 shading_normal = plane.surface.virtual_normal.as_vec3();
                Float view_depth = length(plane.depth_position.as_vec3() - param.camera_pos.as_vec3());
                Float3 albedo = modulation_albedo(plane.surface.denoiser_albedo.as_vec3(), Cfg::Modulator::kSoftEpsilon);
                Uint branch = ocarina::select(plane.surface.stable_branch == InvalidUI32, 0u, plane.surface.stable_branch);
                Uint4 identity = make_uint4(1u, branch, plane.branch_sequence, plane.instance_hash);
                Uint4 surface = make_uint4(plane.depth, plane.material_id, plane.surface.hit.inst_id, 1u);
                Bool emissive = PixelStateUtils::is_emissive(pipeline_ref, plane.surface.hit);
                $if(!emissive) {
                    auto direct = param.direct_src.read(idx), indirect = param.indirect_src.read(idx);
                    param.direct_src.write(idx, make_RadType4(make_RadType3(direct.xyz() / albedo), luminance(albedo)));
                    param.indirect_src.write(idx, make_RadType4(make_RadType3(indirect.xyz() / albedo), luminance(albedo)));
                    param.guide_albedo.write(idx, make_float4(albedo, 0.f));
                };
                param.guide_position.write(idx, make_float4(position, view_depth));
                param.guide_normal.write(idx, make_float4(normal, cast<float>(emissive)));
                param.guide_shading_normal.write(idx, make_float4(shading_normal, 1.f));
                param.guide_identity.write(idx, identity);
                param.guide_surface.write(idx, surface);
            } $else {
                // Sky endpoints retain coverage identity without a filterable signal.
                param.guide_identity.write(idx, make_uint4(2u, 0u, plane.branch_sequence, plane.instance_hash));
                param.guide_surface.write(idx, make_uint4(plane.depth, plane.material_id, plane.surface.hit.inst_id, 1u));
                param.guide_position.write(idx, make_float4(plane.surface.virtual_position.as_vec3(),
                    length(plane.depth_position.as_vec3() - param.camera_pos.as_vec3())));
                param.guide_normal.write(idx, make_float4(plane.surface.virtual_geometric_normal.as_vec3(), 0.f));
            };
        };
        active |= valid;
        $if(!valid) {
            // History starts at zero; every valid temporal write has age >= 1.
            // Clear a used slot once, including both frame parities as they
            // become current, instead of rewriting empty histories every frame.
            $if(param.svgf_buffer.read(idx).moments_direct.z != 0.f) {
                SVGFDataDualVar empty;
                param.svgf_buffer.write(idx, empty);
            };
            param.direct_src.write(idx, make_RadType4(0.f));
            param.indirect_src.write(idx, make_RadType4(0.f));
            // Spatial consumers mask invalid taps. Empty-tile scratch may stay
            // untouched; iteration zero overwrites all pixels of active tiles.
            // Invalid guide geometry is ignored by spatial and coverage
            // consumers. Keep only the validity marker; preserve sky metadata.
            $if(!has_plane) { param.guide_identity.write(idx, make_uint4(0u)); };
        };
    };
    Uint valid_lanes = warp_active_count_bits(active);
    $if(local % 32u == 0u && valid_lanes != 0u) {
        param.active_tiles.atomic(tile).fetch_add(valid_lanes);
    };
};
layer_guide_shader_ = device().compile(prepare_layer_guide, "SVGF-PrepareLayerGuide");
// Multiscale Poisson disk pass with depth/normal/luminance edge stopping.
// Keep the disk fixed across frames: spatial output is not fed back into
// temporal history, so frame-varying rotations would introduce unaveraged flicker.
// Retain the inexpensive original path when Stable Planes are disabled.
auto compile_filter = [&]<bool cached>() {
Kernel kernel = [&, pipeline_ref](Var<CombinedPoissonParam> param) {
    skip_empty_tile(param);
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
        // phi_l noisy. Smoothing the variance makes phi_l spatially coherent
        // across the sparse footprint; it also lets noisy specular
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
            Cfg::Poisson::kMinVariance * floor_variance_scale / (scale_direct * scale_direct),
            Cfg::Poisson::kMinPhi * floor_scale / scale_direct);
        Float phi_l_indirect = LuminanceWeightUtils::compute_phi_l(param.l_phi, var_indirect_clamped,
            Cfg::Poisson::kMinVariance * floor_variance_scale / (scale_indirect * scale_indirect),
            Cfg::Poisson::kMinPhi * floor_scale / scale_indirect);

        // Center tap: radial and geometry/luminance weights are all 1.
        constexpr float kW0 = 1.f;
        Float3 sum_direct = direct_center.xyz() * kW0;
        Float3 sum_indirect = indirect_center.xyz() * kW0;
        Float weight_sum_direct = kW0;
        Float weight_sum_indirect = kW0;
        // The Gaussian above controls only phi_l. Propagate variance with the
        // same accepted samples as RGB; otherwise rejected neighbours inject
        // uncertainty through the centre term and widen every later pass.
        Float variance_sum_direct = max(Float(direct_center.w), variance_epsilon) * (kW0 * kW0);
        Float variance_sum_indirect = max(Float(indirect_center.w), variance_epsilon) * (kW0 * kW0);

        auto accumulate_tap = [&](float dx, float dy, float h) {
            // Scale before rounding so wide passes retain the irregular disk.
            Int2 p = cur_pixel + make_int2(round(
                make_float2(dx, dy) * (Cfg::PoissonDisk::kRadius * cast<float>(param.step_size))));

            $if(all(p >= 0) && all(p < screen_size)) {
                Uint idx = cast<uint>(p.y) * cast<uint>(screen_size.x) + cast<uint>(p.x);

                RadType4Var direct_neighbor = param.direct_src.read(idx);
                RadType4Var indirect_neighbor = param.indirect_src.read(idx);

                auto neighbor_guide = load_spatial_guide<cached>(param, idx);
                TriangleHitVar neighbor_hit = neighbor_guide.hit;
                Bool neighbor_is_sky = PixelStateUtils::is_sky(neighbor_hit);
                if constexpr (cached) {
                    // Sparse fused preparation leaves empty-tile scratch stale.
                    // Zero the complete value before luminance/variance math:
                    // multiplying a stale NaN by a zero weight is insufficient.
                    Bool unused = param.fused_modulation != 0u && neighbor_is_sky;
                    direct_neighbor = ocarina::select(unused, make_RadType4(0.f), direct_neighbor);
                    indirect_neighbor = ocarina::select(unused, make_RadType4(0.f), indirect_neighbor);
                }

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
                    Float3 center_normal, neighbor_normal;
                    if constexpr (cached) {
                        center_normal = center_guide.shading_normal;
                        neighbor_normal = neighbor_guide.shading_normal;
                    } else {
                        center_normal = make_float3(param.svgf_buffer.read(cur_idx).surface_normal.xyz());
                        neighbor_normal = make_float3(param.svgf_buffer.read(idx).surface_normal.xyz());
                    }
                    w_geo = GeometryWeightUtils::compute_depth_weight(
                        center_it.pos, neighbor_it.pos, center_it.ng, param.z_phi) *
                        GeometryWeightUtils::compute_normal_weight(
                            ocarina::select(param.use_shading_normal != 0u, center_normal, center_it.ng),
                            ocarina::select(param.use_shading_normal != 0u, neighbor_normal, neighbor_it.ng), param.n_phi);
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

        // Host-unrolled fixed disk; center handled above.
        for (uint i = 0; i < Cfg::PoissonDisk::kSampleCount; ++i) {
            accumulate_tap(Cfg::PoissonDisk::kSamplesX[i], Cfg::PoissonDisk::kSamplesY[i],
                           Cfg::PoissonDisk::kWeights[i]);
        }

        Float3 filtered_direct = sum_direct / max(weight_sum_direct, Cfg::Epsilon::kWeight);
        Float3 filtered_indirect = sum_indirect / max(weight_sum_indirect, Cfg::Epsilon::kWeight);
        Float out_var_direct = VarianceUtils::propagate_filtered_variance(variance_sum_direct, weight_sum_direct * weight_sum_direct);
        Float out_var_indirect = VarianceUtils::propagate_filtered_variance(variance_sum_indirect, weight_sum_indirect * weight_sum_indirect);

        Float3 output_direct = filtered_direct, output_indirect = filtered_indirect;
        if constexpr (cached) {
            $if(param.fused_modulation != 0u && param.iteration == Cfg::Poisson::kIterationCount - 1u && !center_guide.emissive) {
                Float3 albedo = param.guide_albedo.read(cur_idx).xyz();
                // Preserve the intermediate storage rounding in half builds.
                output_direct = make_float3(make_RadType3(filtered_direct)) * albedo;
                output_indirect = make_float3(make_RadType3(filtered_indirect)) * albedo;
            };
        }
        param.direct_dst.write(cur_idx, make_RadType4(make_float4(output_direct, out_var_direct)));
        param.indirect_dst.write(cur_idx, make_RadType4(make_float4(output_indirect, out_var_indirect)));

        // === SVGF colour-history feedback (Schied et al. 2017) ===
        // Feed the FIRST spatial iteration's output back as the colour history so
        // spatial filtering compounds across frames instead of being recomputed from
        // scratch every frame. Without this the temporal history only ever holds the
        // raw 1-spp accumulation, so residual noise can only be reduced by history
        // length -- the root cause of the residual "blocky" noise. Only iteration 0 is
        // fed back (the smallest footprint) to avoid the over-blur/lag of feeding back the
        // full multi-iteration result. Moments (M1/M2/history) are intentionally left
        // untouched: they must keep tracking the RAW signal so variance still drives
        // edge stopping. svgf_buffer is not read by this pass (only input buffers are),
        // so this center-only write is race-free. Feedback is disabled by default
        // to avoid compounding spatial blur in the temporal illumination history.
        //
        // illumi_direct = DIFFUSE channel, illumi_indirect = SPECULAR channel. The
        // specular channel is NOT fed back by default (kFeedbackSpecular=false): feeding
        // back a view-dependent specular signal over-blurs and ghosts highlights.
        if constexpr (Cfg::Poisson::kFeedbackEnabled) {
            $if(param.write_history != 0u) {
                SVGFDataDualVar hist = param.svgf_buffer.read(cur_idx);
                hist.illumi_direct = make_RadType4(make_RadType3(filtered_direct), out_var_direct);
                if constexpr (Cfg::Poisson::kFeedbackSpecular) {
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

    return device().compile(kernel, cached ? "SVGF-PoissonFilter-Cached" : "SVGF-PoissonFilter");
    };
    combined_shader_ = compile_filter.operator()<true>();
    legacy_combined_shader_ = compile_filter.operator()<false>();
}




CommandBatch PoissonFilter::dispatch_guide(RealTimeDenoiseInput &input, bool fused_modulation) noexcept {
    CombinedPoissonParam param;
    bind_stable_planes(param, input);
    svgf_->bind_tiles(param);
    bind_guides(param);
    param.visibility_buffer = input.visibility.descriptor();
    param.svgf_buffer = svgf_->svgf_buffer_cur(input.frame_index, input.layer_index).descriptor();
    param.direct_src = input.direct.descriptor();
    param.indirect_src = input.indirect.descriptor();
    param.fused_modulation = fused_modulation;
    param.camera_pos = input.camera_pos;
    param.width = input.resolution.x; param.height = input.resolution.y;
    CommandBatch ret;
    if (fused_modulation) {
        const uint tiles = ((param.width + 15u) / 16u) * ((param.height + 15u) / 16u);
        ret << layer_guide_shader_(param).dispatch(tiles * 256u);
    } else {
        ret << guide_shader_(param).dispatch(input.resolution);
    }
    return ret;
}

CommandBatch PoissonFilter::dispatch_combined(vision::RealTimeDenoiseInput &input,
                                         uint step_width, uint iteration, bool use_shading_normal, bool fused_modulation) noexcept {
    CombinedPoissonParam param;
    bind_stable_planes(param, input);
    svgf_->bind_tiles(param);
    param.use_shading_normal = use_shading_normal;
    param.fused_modulation = fused_modulation;
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
        l_phi *= Cfg::Poisson::kReSTIRLPhiMultiplier;
    }
    float n_phi = svgf_->sigma_normal();

    if (input.channel_kind != RealTimeDenoiseInput::ChannelKind::DirectIndirect &&
        step_width >= Cfg::Poisson::kLargeStepThreshold) {
        l_phi *= Cfg::Poisson::kLargeStepLPhiMultiplier;
        n_phi *= Cfg::Poisson::kLargeStepNPhiMultiplier;
    }

    param.l_phi = l_phi;
    param.n_phi = n_phi;
    param.z_phi = svgf_->sigma_depth();
    param.step_size = static_cast<int>(step_width);
    param.iteration = iteration;
    param.frame_index = input.frame_index;
    // Feed back only the first Poisson iteration as colour history (Schied 2017).
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


void PoissonFilter::update_resolution(uint2 resolution) noexcept {
    uint num = resolution.x * resolution.y;
    init_buffer_zero(device(), temp_buffer_direct_, num, "PoissonFilter::temp_buffer_direct");
    init_buffer_zero(device(), temp_buffer_indirect_, num, "PoissonFilter::temp_buffer_indirect");
}

}// namespace vision::svgf
