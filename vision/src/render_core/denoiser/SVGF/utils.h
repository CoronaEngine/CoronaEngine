#pragma once

#include "math/basic_types.h"
#include "dsl/dsl.h"
#include "base/mgr/pipeline.h"
#include "base/mgr/scene.h"
#include "base/scattering/interaction.h"
#include "base/scattering/material.h"
#include "base/sampler.h"
#include "base/denoiser.h"
#include "base/color/spectrum.h"
#include "svgf_config.h"
#include "base/using.h"

namespace vision::svgf {
// Keep the legacy single-surface contract available for existing producers.
template<typename Param>
inline void bind_stable_planes(Param &param, const RealTimeDenoiseInput &input) {
    param.use_stable_planes = input.use_stable_planes;
    param.layered = input.use_stable_planes && input.layer_count > 1u;
    if (param.layered) {
        param.stable_planes = input.stable_planes.descriptor();
        param.prev_stable_planes = input.prev_stable_planes.descriptor();
    } else if (input.use_stable_planes) {
        param.stable_surfaces = input.stable_surfaces.descriptor();
        param.prev_stable_surfaces = input.prev_stable_surfaces.descriptor();
    }
}

template<typename Param>
[[nodiscard]] inline SurfaceDataVar load_stable_surface(const Param &param, Uint index, bool previous = false) {
    SurfaceDataVar surface;
    $if(param.layered != 0u) {
        surface = previous ? param.prev_stable_planes.read(index).surface : param.stable_planes.read(index).surface;
    } $else {
        surface = previous ? param.prev_stable_surfaces.read(index) : param.stable_surfaces.read(index);
    };
    return surface;
}

template<typename Param>
[[nodiscard]] inline Float2 stable_motion(const Param &param, Uint index) {
    Float2 motion;
    $if(param.layered != 0u) { motion = param.stable_planes.read(index).motion; }
    $else { motion = param.motion_vectors.read(index); };
    return motion;
}

// Physical hits evaluate material data; virtual geometry guides image filtering.
struct StableGeometryGuide {
    TriangleHitVar hit;
    Float3 position{make_float3(0.f)}, normal{make_float3(0.f)}, depth_position{make_float3(0.f)};
    Uint branch{0u}, sequence{0u}, identity{0u}, depth{0u}, material{InvalidUI32};
    Bool replaced{false}, valid{true}, layered{false}, approximate{false};

    template<typename Param>
    StableGeometryGuide(const Param &param, Uint index, TriangleHitVar primary, bool previous = false) : hit(primary) {
        $if(param.use_stable_planes != 0u) {
            auto surface = load_stable_surface(param, index, previous);
            branch = ocarina::select(surface.stable_branch == InvalidUI32, 0u, surface.stable_branch);
            replaced = surface.is_replaced && surface.stable_branch != InvalidUI32;
            approximate = surface.approximate != 0u;
            depth_position = surface.depth_position;
            $if(param.layered != 0u) {
                auto plane = previous ? param.prev_stable_planes.read(index) : param.stable_planes.read(index);
                layered = true;
                valid = plane.valid != 0u && plane.surface.hit->is_hit() && plane.surface.hit.inst_id != InvalidUI32;
                sequence = plane.branch_sequence; identity = plane.instance_hash;
                depth = plane.depth; material = plane.material_id;
                depth_position = plane.depth_position;
                replaced = true;
            };
            $if(replaced) {
                hit = surface.hit;
                position = surface.virtual_position;
                normal = surface.virtual_geometric_normal;
            };
            $if(!valid) { hit.inst_id = InvalidUI32; };
        };
    }

    [[nodiscard]] Bool compatible(const StableGeometryGuide &other) const {
        return valid && other.valid && branch == other.branch &&
            (!layered || (sequence == other.sequence && identity == other.identity &&
             depth == other.depth && material == other.material && hit.inst_id == other.hit.inst_id));
    }
    [[nodiscard]] Float3 depth_point(const Float3 &fallback) const {
        // Preserve the established Euclidean virtual depth of exact mirrors.
        return ocarina::select(approximate, depth_position, fallback);
    }
    void apply(Interaction &it) const {
        $if(replaced) { it.pos = position; it.ng = normal; };
    }
};

template<typename Param>
[[nodiscard]] inline TriangleHitVar stable_hit(const Param &param, Uint index, TriangleHitVar hit, bool previous = false) {
    return StableGeometryGuide(param, index, hit, previous).hit;
}

template<typename Param>
[[nodiscard]] inline Float3 stable_normal(const Param &param, Uint index, Float3 normal) {
    $if(param.use_stable_planes != 0u) {
        auto surface = load_stable_surface(param, index);
        $if(param.layered != 0u || (surface.is_replaced && surface.stable_branch != InvalidUI32)) { normal = surface.virtual_normal; };
    };
    return normal;
}

template<typename Param>
[[nodiscard]] inline Float3 stable_albedo(const Param &param, Uint index, Float3 albedo) {
    $if(param.use_stable_planes != 0u) {
        auto surface = load_stable_surface(param, index);
        $if(param.layered != 0u || (surface.is_replaced && surface.stable_branch != InvalidUI32)) { albedo = surface.denoiser_albedo; };
    };
    return albedo;
}

// Coverage estimates a complete pixel footprint, unlike path-space illumination.
// A single exact mirror may keep its virtual guide. Multiple branches or an
// approximate refraction cannot share one motion vector: use primary silhouettes
// for reconstruction and reject moving composite temporal reuse conservatively.
struct CoverageGeometryGuide : StableGeometryGuide {
    Bool ambiguous{false};
    template<typename Param>
    CoverageGeometryGuide(const Param &param, Uint index, TriangleHitVar primary, bool previous = false)
        : StableGeometryGuide(param, index, primary, previous) {
        $if(param.composed_coverage != 0u) {
            Uint count = dispatch_dim().x * dispatch_dim().y;
            for (uint layer = 0u; layer < StablePlaneCount; ++layer) {
                auto plane = previous ? param.prev_coverage_planes.read(layer * count + index)
                                      : param.coverage_planes.read(layer * count + index);
                ambiguous |= plane.valid != 0u && (Bool(layer > 0u) || plane.surface.approximate != 0u);
            }
            $if(ambiguous) {
                hit = primary; branch = 0u; replaced = false; approximate = false; valid = true;
            };
        };
    }
};

template<typename Param>
[[nodiscard]] inline Bool coverage_layer_edge(const Param &param, Uint center, Uint neighbor) {
    Bool edge = false;
    $if(param.composed_coverage != 0u) {
        Uint count = dispatch_dim().x * dispatch_dim().y;
        for (uint layer = 0u; layer < StablePlaneCount; ++layer) {
            auto a = param.coverage_planes.read(layer * count + center);
            auto b = param.coverage_planes.read(layer * count + neighbor);
            edge |= a.valid != b.valid;
            $if(a.valid != 0u && b.valid != 0u) {
                edge |= a.branch_sequence != b.branch_sequence || a.instance_hash != b.instance_hash ||
                    a.depth != b.depth || a.material_id != b.material_id || a.surface.hit.inst_id != b.surface.hit.inst_id;
                $if(a.surface.hit->is_hit() && b.surface.hit->is_hit()) {
                    Float depth = max(length(a.depth_position - param.camera_pos.as_vec3()), 0.1f);
                    edge |= dot(a.surface.virtual_geometric_normal, b.surface.virtual_geometric_normal) < SVGFConfig::Resolve::kNormalThreshold ||
                        abs(dot(b.surface.virtual_position - a.surface.virtual_position, a.surface.virtual_geometric_normal)) > SVGFConfig::Resolve::kPlaneThreshold * depth ||
                        abs(length(b.depth_position - param.camera_pos.as_vec3()) - depth) > SVGFConfig::Temporal::kDepthThreshold * depth;
                };
            };
        }
    };
    return edge;
}

// Raw guides/illumination live at independent per-pixel film samples; resolved
// colour lives at pixel centres. Call after loading the camera and sampler.
// Preserve the caller's RNG when querying a neighbour's sample position.
[[nodiscard]] inline Float2 pixel_filter_offset(Pipeline *pipeline, Uint2 pixel, Uint frame) {
    auto &camera = pipeline->scene().sensor();
    auto &sampler = pipeline->renderer().sampler();
    Float2 offset = make_float2(0.f);
    sampler->temporary([&](Sampler *local_sampler) {
        local_sampler->set_seed(pixel, frame, Dimension::Camera);
        offset = camera->filter()->sample(local_sampler->next_2d()).p;
    });
    return offset;
}

[[nodiscard]] inline Float film_tent_weight(Float2 delta) {
    Float2 axes = max(make_float2(1.f) - abs(delta), make_float2(0.f));
    return axes.x * axes.y;
}

template<typename T>
inline void init_buffer_zero(Device &dev, Buffer<T> &buffer, uint num, const string &desc = "") {
    buffer = dev.create_buffer<T>(num, desc);
    vector<T> vec(num, T{});
    buffer.upload_immediately(vec.data());
}

struct SVGFDataDual {
    RadType4 illumi_direct{};
    RadType4 illumi_indirect{};
    RadType4 moments_direct{};
    RadType4 moments_indirect{};
    // World-space shading normal, including smooth vertex normals and normal maps.
    RadType4 surface_normal{};
};

}// namespace vision::svgf

OC_STRUCT(vision::svgf, SVGFDataDual, illumi_direct, illumi_indirect, moments_direct, moments_indirect, surface_normal) {
    [[nodiscard]] vision::RadTypeVar variance_direct() const noexcept { return illumi_direct.w; }
    [[nodiscard]] vision::RadType3Var illumination_direct() const noexcept { return illumi_direct.xyz(); }
    [[nodiscard]] vision::RadTypeVar first_moment_direct() const noexcept { return moments_direct.x; }
    [[nodiscard]] vision::RadTypeVar second_moment_direct() const noexcept { return moments_direct.y; }
    
    [[nodiscard]] vision::RadTypeVar variance_indirect() const noexcept { return illumi_indirect.w; }
    [[nodiscard]] vision::RadType3Var illumination_indirect() const noexcept { return illumi_indirect.xyz(); }
    [[nodiscard]] vision::RadTypeVar first_moment_indirect() const noexcept { return moments_indirect.x; }
    [[nodiscard]] vision::RadTypeVar second_moment_indirect() const noexcept { return moments_indirect.y; }
    
    [[nodiscard]] vision::RadTypeVar history_count() const noexcept { return moments_direct.z; }
    [[nodiscard]] vision::RadTypeVar history_count_indirect() const noexcept { return moments_direct.w; }
};


namespace vision::svgf {
using SVGFDataDualVar = Var<SVGFDataDual>;

// Half-precision safe clamping utilities.
// In the float radiance path (VS_HALF_RADIANCE == 0) there is no overflow risk, and
// clamping would clip legitimate HDR highlights/emitters and lose energy, so the clamps
// degrade to identity. They are only active when radiance is stored as half.
struct HalfSafeUtils {
    using Cfg = SVGFConfig::HalfSafety;

    // Clamp luminance to prevent overflow when squaring (for M2 calculation)
    [[nodiscard]] static Float clamp_luminance(Float lum) noexcept {
#if VS_HALF_RADIANCE
        return ocarina::select(lum > Cfg::kMaxLuminance, Float(Cfg::kMaxLuminance), lum);
#else
        return lum;
#endif
    }

    // Clamp radiance to prevent overflow during accumulation
    [[nodiscard]] static Float3 clamp_radiance(Float3 rad) noexcept {
#if VS_HALF_RADIANCE
        return make_float3(
            ocarina::select(rad.x > Cfg::kMaxRadiance, Float(Cfg::kMaxRadiance), rad.x),
            ocarina::select(rad.y > Cfg::kMaxRadiance, Float(Cfg::kMaxRadiance), rad.y),
            ocarina::select(rad.z > Cfg::kMaxRadiance, Float(Cfg::kMaxRadiance), rad.z));
#else
        return rad;
#endif
    }
};

[[nodiscard]] inline Uint safe_pixel_index(const Int2 &pixel, const Int2 &screen_size) noexcept {
    Int2 clamped = clamp(pixel, make_int2(0), screen_size - 1);
    return cast<uint>(clamped.y) * cast<uint>(screen_size.x) + cast<uint>(clamped.x);
}

[[nodiscard]] inline Float compute_screen_short_edge(const Int2 &screen_size) noexcept {
    return cast<float>(min(screen_size.x, screen_size.y));
}

[[nodiscard]] inline float compute_screen_short_edge(uint2 resolution) noexcept {
    return static_cast<float>(std::min(resolution.x, resolution.y));
}

struct GeometryWeightUtils {
    using Cfg = SVGFConfig::GeometryWeight;


    [[nodiscard]] static Float compute_normal_weight(
        const Float3 &center_normal,
        const Float3 &neighbor_normal,
        Float power) noexcept {
        Float normal_dot = max(dot(center_normal, neighbor_normal), 0.f);
        return pow(normal_dot, power);
    }

    [[nodiscard]] static Float compute_depth_weight(
        const Float3 &center_pos,
        const Float3 &neighbor_pos,
        const Float3 &center_normal,
        Float scale,
        Float epsilon = Cfg::kEpsilon) noexcept {
        Float3 diff_vec = neighbor_pos - center_pos;
        Float dist_to_plane = abs(dot(diff_vec, center_normal));
        Float dist_to_center = length(diff_vec);
        Float denom = max(dist_to_center * scale, epsilon);
        return exp(-dist_to_plane / denom);
    }

    [[nodiscard]] static Float compute_geometry_weight(
        const Float3 &center_pos,
        const Float3 &center_normal,
        const Float3 &neighbor_pos,
        const Float3 &neighbor_normal,
        Float normal_power,
        Float depth_scale,
        Float epsilon = Cfg::kEpsilon) noexcept {
        Float w_n = compute_normal_weight(center_normal, neighbor_normal, normal_power);
        Float w_z = compute_depth_weight(center_pos, neighbor_pos, center_normal, depth_scale, epsilon);
        return w_n * w_z;
    }

    [[nodiscard]] static Float handle_sky_weight(
        Bool center_is_sky,
        Bool neighbor_is_sky,
        Float geo_weight) noexcept {
        return ocarina::select(
            center_is_sky || neighbor_is_sky,
            ocarina::select(center_is_sky == neighbor_is_sky, 1.f, 0.f),
            geo_weight);
    }

    [[nodiscard]] static Float compute_full_geometry_weight(
        const Float3 &center_pos,
        const Float3 &center_normal,
        const Float3 &neighbor_pos,
        const Float3 &neighbor_normal,
        Bool center_is_sky,
        Bool neighbor_is_sky,
        Float normal_power,
        Float depth_scale,
        Float epsilon = Cfg::kEpsilon) noexcept {
        Float w_geo = compute_geometry_weight(
            center_pos, center_normal,
            neighbor_pos, neighbor_normal,
            normal_power, depth_scale, epsilon);
        return handle_sky_weight(center_is_sky, neighbor_is_sky, w_geo);
    }
};

struct LuminanceWeightUtils {
    using Cfg = SVGFConfig;

    [[nodiscard]] static Float compute_variance_guided(
        Float center_lum,
        Float neighbor_lum,
        Float phi_l) noexcept {
        return exp(-abs(center_lum - neighbor_lum) / phi_l);
    }

    [[nodiscard]] static Float compute_normalized(
        Float center_lum,
        Float neighbor_lum,
        Float sigma,
        Float epsilon = Cfg::Epsilon::kLuminance) noexcept {
        Float lum_diff = abs(center_lum - neighbor_lum) / (center_lum + neighbor_lum + epsilon);
        return exp(-lum_diff * sigma);
    }

    [[nodiscard]] static Float compute_phi_l(
        Float l_phi,
        Float variance,
        Float min_variance = Cfg::Atrous::kMinVariance,
        Float min_phi = Cfg::Atrous::kMinPhi) noexcept {
        return l_phi * sqrt(max(variance, min_variance)) + min_phi;
    }
};

struct PixelStateUtils {
    [[nodiscard]] static Float3 query_shading_normal(Pipeline *pipeline,
        const TriangleHitVar &hit, const Float3 &camera_pos) noexcept {
        Interaction it = pipeline->geometry().compute_surface_interaction(hit, camera_pos);
        Float3 normal = it.shading.normal();
        if (MaterialRegistry::instance().individual_ns()) {
            $if(it.has_material()) {
                SampledWavelengths swl{pipeline->renderer().spectrum()->dimension()};
                pipeline->scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                    normal = material->shading_normal(it, swl);
                });
            };
        }
        normal = ocarina::zero_if_nan_inf(normal);
        Float norm2 = dot(normal, normal);
        return ocarina::select(norm2 > 1e-10f, normal / sqrt(max(norm2, 1e-10f)), it.ng);
    }

[[nodiscard]] static Bool is_sky(const TriangleHitVar &hit) noexcept {
    return hit->is_miss() || hit.inst_id == InvalidUI32;
}

[[nodiscard]] static Bool is_emissive(const Pipeline *pipeline,
                                      const TriangleHitVar &hit) noexcept {
    Bool result = false;
    $if(!is_sky(hit)) {
        result = pipeline->geometry().is_emissive(hit.inst_id);
    };
    return result;
}

[[nodiscard]] static Bool should_skip(Bool is_sky, Bool has_emission) noexcept {
    return is_sky || has_emission;
}

    [[nodiscard]] static Float3 query_albedo(
        Pipeline *pipeline,
        const TriangleHitVar &hit,
        const Float3 &camera_pos) noexcept {

        Float3 albedo = make_float3(0.5f);

        Bool is_valid = !hit->is_miss() && hit.inst_id != InvalidUI32;
        $if(is_valid) {
            Scene &scene = pipeline->scene();
            Geometry &geometry = pipeline->geometry();
            TSpectrum &sp = pipeline->renderer().spectrum();
            Interaction it = geometry.compute_surface_interaction(hit, camera_pos);
            $if(it.has_material()) {
                SampledWavelengths swl{sp->dimension()};
                scene.materials().dispatch(it.material_id(), [&](const Material *material) {
                    MaterialEvaluator bsdf = material->create_evaluator(it, swl);
                    // Match the specular guide's reflectance convention. A
                    // negative incidence cosine can create Fresnel poles in
                    // substrate's total albedo and amplify filtered lighting.
                    Float3 guide_wo = ocarina::select(
                        dot(bsdf.shading_frame().normal(), it.wo) < 0.f, -it.wo, it.wo);
                    SampledSpectrum albedo_spec = bsdf.albedo(guide_wo);
                    albedo = sp->linear_srgb(albedo_spec, swl);
                });
            };
        };

        return albedo;
    }

    // Diffuse-only albedo, the demodulation guide for the diffuse denoiser channel.
    // Mirrors query_albedo but buckets lobes by type via MaterialEvaluator::albedo_split.
    [[nodiscard]] static Float3 query_diffuse_albedo(
        Pipeline *pipeline,
        const TriangleHitVar &hit,
        const Float3 &camera_pos) noexcept {

        Float3 albedo = make_float3(0.5f);

        Bool is_valid = !hit->is_miss() && hit.inst_id != InvalidUI32;
        $if(is_valid) {
            Scene &scene = pipeline->scene();
            Geometry &geometry = pipeline->geometry();
            TSpectrum &sp = pipeline->renderer().spectrum();
            Interaction it = geometry.compute_surface_interaction(hit, camera_pos);
            $if(it.has_material()) {
                SampledWavelengths swl{sp->dimension()};
                scene.materials().dispatch(it.material_id(), [&](const Material *material) {
                    MaterialEvaluator bsdf = material->create_evaluator(it, swl);
                    SampledSpectrum diffuse_spec{swl.dimension()};
                    SampledSpectrum specular_spec{swl.dimension()};
                    bsdf.albedo_split(it.wo, diffuse_spec, specular_spec);
                    albedo = sp->linear_srgb(diffuse_spec, swl);
                });
            };
        };

        return albedo;
    }

    // Specular-only reflectance, the optional demodulation guide for the specular channel.
    [[nodiscard]] static Float3 query_specular_albedo(
        Pipeline *pipeline,
        const TriangleHitVar &hit,
        const Float3 &camera_pos) noexcept {

        Float3 albedo = make_float3(0.04f);

        Bool is_valid = !hit->is_miss() && hit.inst_id != InvalidUI32;
        $if(is_valid) {
            Scene &scene = pipeline->scene();
            Geometry &geometry = pipeline->geometry();
            TSpectrum &sp = pipeline->renderer().spectrum();
            Interaction it = geometry.compute_surface_interaction(hit, camera_pos);
            $if(it.has_material()) {
                SampledWavelengths swl{sp->dimension()};
                scene.materials().dispatch(it.material_id(), [&](const Material *material) {
                    MaterialEvaluator bsdf = material->create_evaluator(it, swl);
                    SampledSpectrum diffuse_spec{swl.dimension()};
                    SampledSpectrum specular_spec{swl.dimension()};
                    // Reflectance guides use an absolute incidence cosine. A
                    // back-facing substrate otherwise feeds a negative cosine
                    // into Fresnel, creating poles that filtering amplifies into
                    // bright bands when the guide is multiplied back in.
                    Float3 guide_wo = ocarina::select(
                        dot(bsdf.shading_frame().normal(), it.wo) < 0.f, -it.wo, it.wo);
                    bsdf.albedo_split(guide_wo, diffuse_spec, specular_spec);
                    albedo = sp->linear_srgb(specular_spec, swl);
                });
            };
        };

        return albedo;
    }
};

struct BoundaryUtils {
    [[nodiscard]] static Bool is_instance_boundary(
        const TriangleHitVar &center_hit,
        const TriangleHitVar &neighbor_hit) noexcept {
        return center_hit.inst_id != neighbor_hit.inst_id;
    }
    
    [[nodiscard]] static Bool is_emissive_boundary(
        const Pipeline *pipeline,
        const TriangleHitVar &center_hit,
        const TriangleHitVar &neighbor_hit) noexcept {
        Bool center_emissive = PixelStateUtils::is_emissive(pipeline, center_hit);
        Bool neighbor_emissive = PixelStateUtils::is_emissive(pipeline, neighbor_hit);
        return center_emissive != neighbor_emissive;
    }
    
    [[nodiscard]] static Bool is_any_boundary(
        const Pipeline *pipeline,
        const TriangleHitVar &center_hit,
        const TriangleHitVar &neighbor_hit) noexcept {
        Bool center_sky = PixelStateUtils::is_sky(center_hit);
        Bool neighbor_sky = PixelStateUtils::is_sky(neighbor_hit);
        
        Bool sky_boundary = center_sky != neighbor_sky;

        Bool emissive_boundary = !center_sky && !neighbor_sky &&
            is_emissive_boundary(pipeline, center_hit, neighbor_hit);
        
        return sky_boundary || emissive_boundary;
    }
    
    [[nodiscard]] static Float compute_boundary_weight(
        const Pipeline *pipeline,
        const TriangleHitVar &center_hit,
        const TriangleHitVar &neighbor_hit) noexcept {
        return ocarina::select(is_any_boundary(pipeline, center_hit, neighbor_hit), 0.f, 1.f);
    }
};

struct VarianceUtils {
    using Cfg = SVGFConfig::Variance;

    [[nodiscard]] static Float compute_variance(Float m1, Float m2) noexcept {
        return max(m2 - m1 * m1, 0.f);
    }

    [[nodiscard]] static Float apply_min_variance(
        Float variance,
        Bool needs_boost) noexcept {
        return ocarina::select(needs_boost,
            max(variance * Cfg::kDisocclusionBoost, Cfg::kMinVarianceDisocclusion),
            max(variance, Cfg::kMinVarianceConsistent));
    }

    [[nodiscard]] static Float propagate_filtered_variance(
        Float variance_sum,
        Float weight_sum_sq,
        Float epsilon = SVGFConfig::Epsilon::kVariance) noexcept {
        return variance_sum / max(weight_sum_sq, epsilon);
    }
};

struct AnisotropicUtils {
using Cfg = SVGFConfig::Anisotropic;
    
[[nodiscard]] static Float compute_grazing_anisotropy(
        const Float3 &normal,
        const Float3 &view_dir) noexcept {
        Float NdotV = abs(dot(normal, view_dir));
        Float grazing_factor = saturate(1.f - NdotV / Cfg::kGrazingAngleThreshold);
        return 1.f + grazing_factor * (Cfg::kMaxAnisotropy - 1.f) * Cfg::kAnisotropyStrength;
    }
    
    struct EdgeAnisotropyInfo {
        Float2 stretch_dir;
        Float ratio;
    };
    
    [[nodiscard]] static EdgeAnisotropyInfo compute_edge_anisotropy(
        Float w_right, Float w_up, Float w_left, Float w_down) noexcept {
        
        EdgeAnisotropyInfo info;
        
        Float h_weight = w_right + w_left;
        Float v_weight = w_up + w_down;
        Float total = h_weight + v_weight + 0.001f;
        
        Float h_pref = h_weight / total;
        Float v_pref = v_weight / total;
        
        Float2 h_dir = make_float2(1.f, 0.f);
        Float2 v_dir = make_float2(0.f, 1.f);
        
        Float2 blend_dir = h_dir * h_pref + v_dir * v_pref;
        Float blend_len = length(blend_dir);
        info.stretch_dir = ocarina::select(blend_len > 0.001f, 
            blend_dir / blend_len, 
            make_float2(1.f, 0.f));
        
        Float weight_diff = abs(h_weight - v_weight) / total;
        info.ratio = 1.f + weight_diff * (Cfg::kMaxAnisotropy - 1.f);
        
        Float total_confidence = saturate(total * 0.25f);
        info.ratio = lerp(1.f, info.ratio, total_confidence);
        
        return info;
    }
    
    [[nodiscard]] static EdgeAnisotropyInfo compute_combined_anisotropy(
        const Float3 &normal,
        const Float3 &view_dir,
        Float w_right, Float w_up, Float w_left, Float w_down) noexcept {
        
        EdgeAnisotropyInfo edge_info = compute_edge_anisotropy(w_right, w_up, w_left, w_down);
        
        Float grazing_ratio = compute_grazing_anisotropy(normal, view_dir);
        
        Float h_weight = w_right + w_left;
        Float v_weight = w_up + w_down;
        Float edge_confidence = abs(h_weight - v_weight) / (h_weight + v_weight + 0.001f);
        
        Float blend = edge_confidence * Cfg::kEdgeAnisotropyBlend;
        edge_info.ratio = lerp(grazing_ratio, edge_info.ratio, blend);
        edge_info.ratio = clamp(edge_info.ratio, 1.f, Cfg::kMaxAnisotropy);
        
        return edge_info;
    }
    
    [[nodiscard]] static Float2 apply_edge_anisotropic_transform(
        Float2 sample_offset,
        const EdgeAnisotropyInfo &info) noexcept {
        Float proj = dot(sample_offset, info.stretch_dir);
        Float2 parallel = info.stretch_dir * proj;
        Float2 perp = sample_offset - parallel;
        
        return parallel * info.ratio + perp;
    }
};

}// namespace vision::svgf
