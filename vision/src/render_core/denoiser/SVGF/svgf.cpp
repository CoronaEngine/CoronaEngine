#include "svgf.h"
#include "svgf_config.h"
#include "variance_estimator.h"
#include "prefilter.h"
#include <cstdlib>

namespace vision::svgf {

using Cfg = SVGFConfig;
namespace {
[[nodiscard]] bool env_flag(const char *name) noexcept {
    const char *value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}
}

void SVGF::prepare_buffers() {
    uint pixel_num = pipeline()->pixel_num();
    auto rt_res = pipeline()->frame_buffer()->raytracing_resolution();
    OC_INFO_FORMAT("SVGF::prepare_buffers pipeline_pixel_num={}, framebuffer=({}, {}), raytracing=({}, {})",
                   pixel_num,
                   pipeline()->frame_buffer()->resolution().x, pipeline()->frame_buffer()->resolution().y,
                   rt_res.x, rt_res.y);
    init_buffer_zero(device(), svgf_data, pixel_num, "SVGF::svgf_data");
    svgf_data.register_self(0, pixel_num);
    init_buffer_zero(device(), svgf_data2, pixel_num, "SVGF::svgf_data2");
    svgf_data2.register_self(0, pixel_num);
    prepare_resolve(pixel_num);
}

void SVGF::prepare_resolve(uint pixel_num) {
    for (uint layer = 0u; layer < StablePlaneCount; ++layer) {
        if (layer == 0u) {
            init_buffer_zero(device(), resolve_direct_[layer], pixel_num, "SVGF::resolve_direct");
            init_buffer_zero(device(), resolve_indirect_[layer], pixel_num, "SVGF::resolve_indirect");
            init_buffer_zero(device(), resolve_direct2_[layer], pixel_num, "SVGF::resolve_direct2");
            init_buffer_zero(device(), resolve_indirect2_[layer], pixel_num, "SVGF::resolve_indirect2");
        }
        if (layer > 0u) {
            init_buffer_zero(device(), layer_data_[layer - 1u], pixel_num, "SVGF::layer_data");
            init_buffer_zero(device(), layer_data2_[layer - 1u], pixel_num, "SVGF::layer_data2");
        }
    }
    layer_state_ = {};
}

void SVGF::compile_resolve() {
    Pipeline *pipeline_ref = pipeline();
    Kernel clear_invalid = [](Var<VarianceEstimatorParam> param) {
        Uint idx = dispatch_id();
        auto plane = param.stable_planes.read(idx);
        $if(plane.valid == 0u || plane.surface.hit->is_miss() || plane.surface.hit.inst_id == InvalidUI32) {
            SVGFDataDualVar empty;
            param.svgf_buffer_cur.write(idx, empty);
            param.radiance_direct.write(idx, make_RadType4(0.f));
            param.radiance_indirect.write(idx, make_RadType4(0.f));
        };
    };
    clear_invalid_shader_ = device().compile(clear_invalid, "SVGF-ClearInvalidLayer");
    Kernel kernel = [pipeline_ref](Var<ResolveParam> param) {
        pipeline_ref->scene().sensor()->load_data();
        pipeline_ref->renderer().sampler()->load_data();
        Float2 film_offset = pixel_filter_offset(pipeline_ref, dispatch_idx().xy(), param.frame_index);
        Int2 filter_radius = make_int2(ceil(pipeline_ref->scene().sensor()->filter()->radius()));
        Uint idx = dispatch_id();
        Int2 pixel = make_int2(dispatch_idx().xy());
        Int2 size = make_int2(dispatch_dim().xy());
        CoverageGeometryGuide center_guide(param, idx, param.visibility.read(idx));
        $if(!center_guide.valid) {
            param.output_direct.write(idx, make_float4(0.f));
            param.output_indirect.write(idx, make_float4(0.f));
        } $else {
        TriangleHitVar hit = center_guide.hit;
        Bool sky = PixelStateUtils::is_sky(hit);
        Float3 center_pos = make_float3(0.f);
        Float3 center_normal = make_float3(0.f);
        Float depth = 0.1f;
        $if(!sky) {
            Interaction center = pipeline_ref->geometry().compute_surface_interaction(hit, false);
            center_guide.apply(center);
            center_pos = center.pos;
            center_normal = center.ng;
            depth = max(length(center_guide.depth_point(center_pos) - param.camera_pos.as_vec3()), 0.1f);
        };
        Float edge_history = 0.f;
        $if(param.alpha < 1.f) {
            // The footprint itself jitters. Retain coverage classification until
            // invalidation so it cannot toggle the temporal weight every frame.
            edge_history = param.history_direct.read(idx).w;
        };
        Bool edge = edge_history > 0.f;
        // Moving views also need coverage reconstruction at subpixel edges.
        $if(!edge) {
            for (int y = -1; y <= 1; ++y) {
                for (int x = -1; x <= 1; ++x) {
                    if (x == 0 && y == 0) { continue; }
                    Int2 tap_pixel = pixel + make_int2(x, y);
                    $if(!edge && all(tap_pixel >= 0) && all(tap_pixel < size)) {
                        Uint tap_idx = cast<uint>(tap_pixel.y * size.x + tap_pixel.x);
                        CoverageGeometryGuide tap_guide(param, tap_idx, param.visibility.read(tap_idx));
                        TriangleHitVar tap = tap_guide.hit;
                        Bool tap_sky = PixelStateUtils::is_sky(tap);
                        edge = (center_guide.branch != tap_guide.branch) || (hit.inst_id != tap.inst_id) || (sky != tap_sky) ||
                               coverage_layer_edge(param, idx, tap_idx);
                        $if(!edge && !sky && !tap_sky) {
                            Interaction neighbor = pipeline_ref->geometry().compute_surface_interaction(tap, false);
                            tap_guide.apply(neighbor);
                            edge = dot(center_normal, neighbor.ng) < Cfg::Resolve::kNormalThreshold ||
                                   abs(dot(neighbor.pos - center_pos, center_normal)) >
                                       Cfg::Resolve::kPlaneThreshold * depth;
                        };
                    };
                }
            }
        };
        // A boundary first seen late must not inherit the camera's entire age:
        // its previous estimate was only an EMA. Seed it with that weight, then
        // count real local observations. Zero metadata means surface interior.
        Float edge_count = ocarina::select(edge_history > 0.f,
            min(edge_history + 1.f, float(Cfg::Resolve::kHistoryPrecisionLimit)),
            1.f / param.interior_alpha);
        Float alpha = ocarina::select(edge, 1.f / edge_count, param.interior_alpha);
        Float3 reprojected_direct = make_float3(0.f);
        Float3 reprojected_indirect = make_float3(0.f);
        Float reprojection_weight = 0.f;
        Float supported_weight = 0.f;
        Float moving_alpha = 1.f;
        Bool moving_edge = edge && !sky && !center_guide.ambiguous && param.alpha == 1.f && param.history_valid != 0u &&
                           param.channel_kind == uint(RealTimeDenoiseInput::ChannelKind::DirectIndirect);
        $if(moving_edge) {
            Float2 motion = stable_motion(param, idx);
            moving_alpha = max(Cfg::Resolve::kMovingAlpha,
                               saturate(length(motion) / Cfg::Resolve::kMotionRejectPixels));
            Float2 previous_pixel = make_float2(pixel) - motion;
            Int2 base = make_int2(floor(previous_pixel));
            Float2 fraction = previous_pixel - floor(previous_pixel);
            // Resolved colour stays on the pixel-centre grid. Each raw guide
            // has its own jitter; validate the actual previous sample positions.
            Float2 guide_pixel = previous_pixel + film_offset;
            Int2 guide_base = make_int2(floor(guide_pixel));
            Float guide_weight_sum = 0.f;
            Float expected_depth = max(length(center_guide.depth_point(center_pos) - param.prev_camera_pos.as_vec3()), 0.1f);
            $for(y, -filter_radius.y, filter_radius.y + 2) {
                $for(x, -filter_radius.x, filter_radius.x + 2) {
                    Int2 guide_p = guide_base + make_int2(x, y);
                    $if(all(guide_p >= 0) && all(guide_p < size)) {
                        Float2 guide_offset = pixel_filter_offset(pipeline_ref, make_uint2(guide_p),
                                                                  max(param.frame_index, 1u) - 1u);
                        Float guide_weight = film_tent_weight(make_float2(guide_p) + guide_offset - guide_pixel);
                        guide_weight_sum += guide_weight;
                        $if(guide_weight > 0.f) {
                            Uint tap_idx = cast<uint>(guide_p.y * size.x + guide_p.x);
                            CoverageGeometryGuide tap_guide(param, tap_idx, param.prev_visibility.read(tap_idx), true);
                            TriangleHitVar tap = tap_guide.hit;
                            $if(!PixelStateUtils::is_sky(tap)) {
                                Interaction previous = pipeline_ref->geometry().compute_surface_interaction(tap, false);
                                tap_guide.apply(previous);
                                Float previous_depth = length(tap_guide.depth_point(previous.pos) - param.prev_camera_pos.as_vec3());
                                // Coverage may cross adjacent facets of the same
                                // object (e.g. a cabinet bevel). Requiring matching
                                // normals here would preserve its aliased black edge.
                                // Reject opposite-facing sides of a thin shell.
                                // Depth/plane support and the current colour box
                                // still bound reuse; illumination uses its own normals.
                                Bool consistent = !tap_guide.ambiguous && (center_guide.branch == tap_guide.branch) && tap.inst_id == hit.inst_id &&
                                    dot(center_normal, previous.ng) >= -0.1f &&
                                    abs(previous_depth - expected_depth) < Cfg::Temporal::kDepthThreshold * expected_depth &&
                                    abs(dot(previous.pos - center_pos, center_normal)) < Cfg::Resolve::kPlaneThreshold * expected_depth;
                                // Resolved history represents a pixel footprint, not
                                // just its current jittered hit. A subpixel grille
                                // alternates between shell and holes; either surface
                                // can support coverage when it is still present in
                                // the current reconstruction neighbourhood. Validate
                                // that surface's geometry instead of accepting an
                                // unrelated previous hit or opposite-facing shell.
                                $if(!consistent) {
                                    $for(cy, -1, 2) {
                                        $for(cx, -1, 2) {
                                            Int2 q = pixel + make_int2(cx, cy);
                                            $if(!consistent && all(q >= 0) && all(q < size)) {
                                                Uint neighbor_idx = cast<uint>(q.y * size.x + q.x);
                                                CoverageGeometryGuide neighbor_guide(param, neighbor_idx, param.visibility.read(neighbor_idx));
                                                TriangleHitVar neighbor_hit = neighbor_guide.hit;
                                                $if(neighbor_hit.inst_id == tap.inst_id) {
                                                    Interaction neighbor = pipeline_ref->geometry().compute_surface_interaction(neighbor_hit, false);
                                                    neighbor_guide.apply(neighbor);
                                                    Float neighbor_depth = max(length(neighbor_guide.depth_point(neighbor.pos) - param.prev_camera_pos.as_vec3()), 0.1f);
                                                    consistent = !tap_guide.ambiguous && !neighbor_guide.ambiguous &&
                                                        (center_guide.branch == neighbor_guide.branch) && (neighbor_guide.branch == tap_guide.branch) && dot(neighbor.ng, previous.ng) >= -0.1f &&
                                                        abs(previous_depth - neighbor_depth) < Cfg::Temporal::kDepthThreshold * neighbor_depth &&
                                                        abs(dot(previous.pos - neighbor.pos, neighbor.ng)) < Cfg::Resolve::kPlaneThreshold * neighbor_depth;
                                                };
                                            };
                                        };
                                    };
                                };
                                supported_weight += ocarina::select(consistent, guide_weight, 0.f);
                            };
                        };
                    };
                };
            };
            // Irregular samples do not have a partition-of-unity weight sum.
            supported_weight /= max(guide_weight_sum, 1e-6f);
            for (int y = 0; y < 2; ++y) {
                for (int x = 0; x < 2; ++x) {
                    Float weight = (x ? fraction.x : 1.f - fraction.x) *
                                   (y ? fraction.y : 1.f - fraction.y);
                    Int2 p = base + make_int2(x, y);
                    $if(weight > 0.f && all(p >= 0) && all(p < size)) {
                        Uint tap_idx = cast<uint>(p.y * size.x + p.x);
                        // Reconstruct coverage with the full bilinear footprint,
                        // only when it includes this surface. Renormalizing just
                        // same-surface taps would preserve the aliased silhouette.
                        CoverageGeometryGuide history_guide(param, tap_idx, param.prev_visibility.read(tap_idx), true);
                        $if(!history_guide.ambiguous) {
                        reprojected_direct += param.history_direct.read(tap_idx).xyz() * weight;
                        reprojected_indirect += param.history_indirect.read(tap_idx).xyz() * weight;
                        reprojection_weight += weight;
                        };
                    };
                }
            }
        };
        Bool reuse_coverage = moving_edge && moving_alpha < 1.f && reprojection_weight > 0.99f &&
                              supported_weight > Cfg::Resolve::kMinReprojectionSupport;
        auto resolve_channel = [&](auto &radiance, auto &history, auto &output, Float3 reprojected) {
            RadType4Var current = radiance.read(idx);
            // Never persist non-finite samples. The presentation/debug checks
            // are too late for history and may be disabled in release builds.
            Float3 color = ocarina::zero_if_nan_inf(make_float3(current.xyz()));
            $if(edge && param.alpha == 1.f &&
                param.channel_kind == uint(RealTimeDenoiseInput::ChannelKind::DirectIndirect)) {
                // Reconstruct the jittered radiance samples at the pixel centre.
                // Coverage includes BOTH sides of a silhouette. Requiring equal
                // instance/normal here preserves black subpixel holes forever.
                // A positive one-pixel tent footprint preserves complementary
                // dark/bright edges; it does not feed illumination history.
                Float3 sum = make_float3(0.f);
                Float weight_sum = 0.f;
                $for(y, -filter_radius.y, filter_radius.y + 1) {
                    $for(x, -filter_radius.x, filter_radius.x + 1) {
                        Int2 p = pixel + make_int2(x, y);
                        $if(all(p >= 0) && all(p < size)) {
                            Float2 offset = pixel_filter_offset(pipeline_ref, make_uint2(p), param.frame_index);
                            Float weight = film_tent_weight(make_float2(x, y) + offset);
                            Uint tap_idx = cast<uint>(p.y * size.x + p.x);
                            sum += ocarina::zero_if_nan_inf(make_float3(radiance.read(tap_idx).xyz())) * weight;
                            weight_sum += weight;
                        };
                    };
                };
                color = ocarina::select(weight_sum > 0.f, sum / max(weight_sum, 1e-6f), color);
            };
            $if(alpha < 1.f) {
                Float3 previous = history.read(idx).xyz();
                color = previous + alpha * (color - previous);
            };
            $if(reuse_coverage) {
                // Clamp the already-denoised, remodulated history, never the
                // sparse Monte Carlo samples used for illumination accumulation.
                Float3 lower = color;
                Float3 upper = color;
                for (int y = -1; y <= 1; ++y) {
                    for (int x = -1; x <= 1; ++x) {
                        Int2 p = clamp(pixel + make_int2(x, y), make_int2(0), size - 1);
                        Float3 neighbor = ocarina::zero_if_nan_inf(make_float3(radiance.read(cast<uint>(p.y * size.x + p.x)).xyz()));
                        lower = min(lower, neighbor); upper = max(upper, neighbor);
                    }
                }
                Float3 previous = clamp(ocarina::zero_if_nan_inf(reprojected), lower, upper);
                color = previous + moving_alpha * (color - previous);
            };
            // FP32 history avoids stagnation at small alpha on dark FP16 colours.
            Float count = ocarina::select(edge, edge_count, 0.f);
            $if(!center_guide.valid) { color = make_float3(0.f); count = 0.f; };
            output.write(idx, make_float4(color, count));
        };
        resolve_channel(param.direct, param.history_direct, param.output_direct, reprojected_direct);
        resolve_channel(param.indirect, param.history_indirect, param.output_indirect, reprojected_indirect);
        };
    };
    resolve_shader_ = device().compile(kernel, "SVGF-CoverageResolve");
    // Neighbourhood reads above must see one immutable current image. Publish
    // in a separate dispatch, avoiding an in-place read/write race at edges.
    Kernel publish = [](Var<ResolveParam> param) {
        Uint idx = dispatch_id();
        param.direct.write(idx, make_RadType4(param.output_direct.read(idx).xyz(), param.direct.read(idx).w));
        param.indirect.write(idx, make_RadType4(param.output_indirect.read(idx).xyz(), param.indirect.read(idx).w));
    };
    publish_resolve_shader_ = device().compile(publish, "SVGF-PublishResolve");
    layer_state_ = {};
}

CommandBatch SVGF::resolve(RealTimeDenoiseInput &input) {
    auto &state = layer_state_[input.layer_index];
    const auto &camera = scene().sensor();
    const float4x4 transform = camera->host_c2w();
    bool history_valid = state.history > 0u && input.frame_index > 0u &&
                      state.frame != InvalidUI32 && state.frame + 1u == input.frame_index &&
                      camera->fov_y() == state.fov;
    bool stationary = history_valid;
    for (uint i = 0u; i < 4u; ++i) {
        stationary = stationary && all(transform[i] == state.camera[i]);
    }
    state.history = stationary
                           ? std::min(state.history + 1u, Cfg::Resolve::kHistoryPrecisionLimit)
                           : 1u;
    state.camera = transform;
    state.fov = camera->fov_y();
    state.frame = input.frame_index;

    ResolveParam param;
    bind_stable_planes(param, input);
    param.composed_coverage = input.composed_coverage;
    if (input.composed_coverage) {
        param.coverage_planes = input.coverage_planes.descriptor();
        param.prev_coverage_planes = input.prev_coverage_planes.descriptor();
    }
    param.direct = input.direct.descriptor();
    param.indirect = input.indirect.descriptor();
    const bool even = (input.frame_index & 1u) == 0u;
    param.history_direct = (even ? resolve_direct_[input.layer_index] : resolve_direct2_[input.layer_index]).view().descriptor();
    param.history_indirect = (even ? resolve_indirect_[input.layer_index] : resolve_indirect2_[input.layer_index]).view().descriptor();
    param.output_direct = (even ? resolve_direct2_[input.layer_index] : resolve_direct_[input.layer_index]).view().descriptor();
    param.output_indirect = (even ? resolve_indirect2_[input.layer_index] : resolve_indirect_[input.layer_index]).view().descriptor();
    param.visibility = input.visibility.descriptor();
    param.prev_visibility = input.prev_visibility.descriptor();
    param.motion_vectors = input.motion_vec.descriptor();
    param.camera_pos = input.camera_pos;
    param.prev_camera_pos = input.prev_camera_pos;
    param.history_valid = history_valid;
    param.channel_kind = static_cast<uint>(input.channel_kind);
    param.frame_index = input.frame_index;
    param.alpha = 1.f / static_cast<float>(state.history);
    param.interior_alpha = 1.f / static_cast<float>(std::min(state.history, Cfg::Resolve::kInteriorHistory));
    CommandBatch ret;
    ret << resolve_shader_(param).dispatch(input.resolution);
    ret << publish_resolve_shader_(param).dispatch(input.resolution);
    return ret;
}

void SVGF::compute_GBuffer(const vision::RayState &rs, const vision::Interaction &it) noexcept {
}

void SVGF::initialize_(const vision::NodeDesc &node_desc) noexcept {
    atrous_ = make_shared<AtrousFilter>(this);
    modulator_ = make_shared<Modulator>(this);
    variance_estimator_ = make_shared<VarianceEstimator>(this);
    prefilter_ = make_shared<Prefilter>(this);
}

void SVGF::render_sub_UI(Widgets *widgets) noexcept {
    bool enabled = params_.switch_;
    if (widgets->check_box("turn on", &enabled)) {
        changed_ = true;
        set_enabled(enabled);
    }
    changed_ |= widgets->input_float_limit("sigma_rt", &params_.sigma_rt_,
                                           0.01, 1e10, 1, 3);
    changed_ |= widgets->input_float_limit("sigma_normal", &params_.sigma_normal_,
                                           0.01, 1e10, 1, 3);
    changed_ |= widgets->input_float_limit("sigma_depth", &params_.sigma_depth_,
                                           0.01, 10.0, 0.1, 0.5);
}

bool SVGF::history_valid(const RealTimeDenoiseInput &input) const noexcept {
    return input.frame_index > 0u && (input.layer_count == 1u ||
        (layer_state_[input.layer_index].temporal_frame != InvalidUI32 &&
         layer_state_[input.layer_index].temporal_frame + 1u == input.frame_index));
}

void SVGF::prepare() noexcept {
    prepare_buffers();
    frame_buffer().register_callback(shared_from_this());
    atrous_->prepare();
    modulator_->prepare();
    variance_estimator_->prepare();
    prefilter_->prepare();
}

void SVGF::compile() noexcept {
    atrous_->compile();
    modulator_->compile();
    variance_estimator_->compile();
    prefilter_->compile();
    compile_resolve();
}

CommandBatch SVGF::dispatch(vision::RealTimeDenoiseInput &input) noexcept {
    CommandBatch ret;
    if (params_.switch_) {
        if (input.composed_coverage) {
            if (!env_flag("VISION_SVGF_SKIP_RESOLVE")) ret << resolve(input);
            else layer_state_[0].history = 0u;
            return ret;
        }
        if (input.use_stable_planes && input.layer_count > 1u) {
            VarianceEstimatorParam clear;
            bind_stable_planes(clear, input);
            clear.radiance_direct = input.direct.descriptor();
            clear.radiance_indirect = input.indirect.descriptor();
            clear.svgf_buffer_cur = svgf_buffer_cur(input.frame_index, input.layer_index).descriptor();
            ret << clear_invalid_shader_(clear).dispatch(input.resolution);
        }
        const bool radiance_domain = env_flag("VISION_SVGF_RADIANCE_DOMAIN");
        const bool skip_variance = env_flag("VISION_SVGF_SKIP_VARIANCE");
        const bool skip_prefilter = env_flag("VISION_SVGF_SKIP_PREFILTER") || !params_.spatial_filter_;
        const bool skip_atrous = env_flag("VISION_SVGF_SKIP_ATROUS") || !params_.spatial_filter_;

        if (!radiance_domain) {
            ret << modulator_->demodulate(input);
        }
        if (!skip_variance) {
            ret << variance_estimator_->dispatch_variance(input);
        }
        if (!skip_prefilter) {
            // The temporal bypass does not produce current shading guides.
            // Preserve geometry-guided spatial filtering for this ablation.
            ret << prefilter_->dispatch(input, !skip_variance);
        }
        
        if (!skip_atrous) {
            for (uint i = 0; i < Cfg::Atrous::kIterationCount; ++i) {
                ret << atrous_->dispatch_combined(input, Cfg::Atrous::kStepSizes[i], i, !skip_variance);
            }
        }
        if (!radiance_domain) {
            ret << modulator_->modulate(input);
        }
        // Illumination history rejects cross-surface taps correctly, but cannot
        // integrate jittered edge coverage or the albedo restored above. Average
        // stationary boundary radiance with decaying 1/N weights,
        // separately from (and without feeding back into) illumination history.
        // Motion uses bounded, validated coverage history at geometric edges;
        // explicit scene invalidation discards both kinds of history.
        if (input.layer_count == 1u) {
            if (!env_flag("VISION_SVGF_SKIP_RESOLVE")) ret << resolve(input);
            else layer_state_[0].history = 0u;
        }
    }
    layer_state_[input.layer_index].temporal_frame = params_.switch_ ? input.frame_index : InvalidUI32;
    return ret;
}

void SVGF::set_enabled(bool enabled) noexcept {
    if (params_.switch_ != enabled) {
        layer_state_ = {};
    }
    params_.switch_ = enabled;
}

bool SVGF::enabled() noexcept {
    return params_.switch_;
}

void SVGF::update_resolution(uint2 resolution) noexcept {
    uint pixel_num = resolution.x * resolution.y;
    OC_INFO_FORMAT("SVGF::update_resolution input=({}, {}), pixel_num={}, framebuffer=({}, {}), pipeline_pixel_num={}, raytracing=({}, {})",
                   resolution.x, resolution.y, pixel_num,
                   frame_buffer().resolution().x, frame_buffer().resolution().y,
                   pipeline()->pixel_num(),
                   frame_buffer().raytracing_resolution().x, frame_buffer().raytracing_resolution().y);
    init_buffer_zero(device(), svgf_data.super(), pixel_num, "SVGF::svgf_data");
    svgf_data.register_self(0, pixel_num);
    init_buffer_zero(device(), svgf_data2.super(), pixel_num, "SVGF::svgf_data2");
    svgf_data2.register_self(0, pixel_num);
    atrous_->update_resolution(resolution);
    variance_estimator_->update_resolution(resolution);
    prepare_resolve(pixel_num);
}

}// namespace vision::svgf

VS_MAKE_CLASS_CREATOR_HOTFIX_DIRECTORY(vision::svgf, SVGF, 1)
