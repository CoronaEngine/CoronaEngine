#pragma once

#include "math/basic_types.h"
#include "dsl/dsl.h"
#include "base/mgr/pipeline.h"
#include "base/mgr/scene.h"
#include "base/scattering/interaction.h"
#include "base/scattering/material.h"
#include "base/color/spectrum.h"
#include "svgf_config.h"
#include "base/using.h"

namespace vision::svgf {
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
};

}// namespace vision::svgf

OC_STRUCT(vision::svgf, SVGFDataDual, illumi_direct, illumi_indirect, moments_direct, moments_indirect) {
    [[nodiscard]] vision::RadTypeVar variance_direct() const noexcept { return illumi_direct.w; }
    [[nodiscard]] vision::RadType3Var illumination_direct() const noexcept { return illumi_direct.xyz(); }
    [[nodiscard]] vision::RadTypeVar first_moment_direct() const noexcept { return moments_direct.x; }
    [[nodiscard]] vision::RadTypeVar second_moment_direct() const noexcept { return moments_direct.y; }
    
    [[nodiscard]] vision::RadTypeVar variance_indirect() const noexcept { return illumi_indirect.w; }
    [[nodiscard]] vision::RadType3Var illumination_indirect() const noexcept { return illumi_indirect.xyz(); }
    [[nodiscard]] vision::RadTypeVar first_moment_indirect() const noexcept { return moments_indirect.x; }
    [[nodiscard]] vision::RadTypeVar second_moment_indirect() const noexcept { return moments_indirect.y; }
    
    [[nodiscard]] vision::RadTypeVar history_count() const noexcept { return moments_direct.z; }
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
                    SampledSpectrum albedo_spec = bsdf.albedo(it.wo);
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
                    bsdf.albedo_split(it.wo, diffuse_spec, specular_spec);
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
