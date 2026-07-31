//
// Created by Zero on 2025/01/26.
// Phase 2: Unified Spatio-Angular Integration
//
// Single-pass Gaussian gathering on the 5D light-field manifold.
// Center pixel: compute_surface_interaction for depth/normal.
// Neighbors: read from visibility buffer (TriangleHit.t for depth).
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

class SSAT;

// ============================================================================
// Spatial-Angular Filter Parameter Structure
// ============================================================================

struct SpatialAngularParam {
    BufferDesc<RadType4> radiance_src;
    BufferDesc<RadType4> radiance_dst;
    BufferDesc<TriangleHit> visibility;
    BufferDesc<uint> sampling_mask;
    BufferDesc<float> shear_magnitude;
    array_float3 camera_pos{};
    float sigma_lum{3.0f};
    float sigma_normal{128.0f};
    float sigma_depth_angular{1.0f};
    float sigma_x{2.0f};
    float sigma_u{0.3f};
    float z_ref{20.0f};
    float beta{1.0f};
    float angular_range{0.15f};
    int spatial_radius{2};
    int angular_samples{3};
    int step_size{1};
};

}// namespace vision::ssat

OC_PARAM_STRUCT(vision::ssat, SpatialAngularParam,
                radiance_src, radiance_dst, visibility, sampling_mask, shear_magnitude,
                camera_pos, sigma_lum, sigma_normal, sigma_depth_angular,
                sigma_x, sigma_u, z_ref, beta, angular_range,
                spatial_radius, angular_samples, step_size){};

namespace vision::ssat {

// ============================================================================
// Spatial-Angular Filter Class
// ============================================================================

class SpatialAngularFilter : public Toolkit, public RuntimeObject {
private:
    SSAT *ssat_{nullptr};

    Shader<void(SpatialAngularParam, LenticularParams, LightFieldGeometry, float4x4)> filter_shader_;

    Buffer<RadType4> temp_buffer_;

public:
    explicit SpatialAngularFilter(SSAT *ssat)
        : ssat_(ssat) {}

    VS_HOTFIX_MAKE_RESTORE(RuntimeObject, ssat_)

    [[nodiscard]] Buffer<RadType4>& temp_buffer() noexcept { return temp_buffer_; }
    [[nodiscard]] const Buffer<RadType4>& temp_buffer() const noexcept { return temp_buffer_; }

    void ensure_buffers(uint total_subpixels) noexcept {
        if (temp_buffer_.size() != total_subpixels) {
            init_buffer_zero(device(), temp_buffer_, total_subpixels, "SSAT::spatial_angular_temp");
        }
    }

    void prepare(uint total_subpixels) noexcept {
        ensure_buffers(total_subpixels);
    }

    void compile() noexcept {
        compile_filter();
    }

    void update_resolution(uint total_subpixels) noexcept {
        ensure_buffers(total_subpixels);
    }

    // ========================================================================
    // Phase 2 Filter — single-pass Gaussian gathering on 5D manifold
    // ========================================================================

    void compile_filter() noexcept {
        Pipeline *pipeline_ref = pipeline();
        Kernel kernel = [&, pipeline_ref](Var<SpatialAngularParam> param,
                           Var<LenticularParams> lent,
                           Var<LightFieldGeometry> geom,
                           Float4x4 l2w) {

            Uint2 dispatch_idx_val = dispatch_idx().xy();
            Uint linear_idx = dispatch_id();
            Int2 screen_size = make_int2(dispatch_dim().xy());

            Float3 camera_pos = param.camera_pos.as_vec3();
            Float sigma_lum = param.sigma_lum;
            Float sigma_normal = param.sigma_normal;
            Float sigma_z = param.sigma_depth_angular;
            Float sigma_x = param.sigma_x;
            Float sigma_u = param.sigma_u;
            Float z_ref = param.z_ref;
            Float beta = param.beta;
            Float angular_range = param.angular_range;
            Int spatial_radius = param.spatial_radius;
            Int angular_samples = param.angular_samples;
            Int step_size = param.step_size;

            // Read center radiance
            Float4 center_radiance = param.radiance_src.read(linear_idx);
            Float center_lum = luminance(center_radiance.xyz());

            // Center depth and validity
            TriangleHitVar center_hit = param.visibility.read(linear_idx);
            Bool center_valid = !center_hit->is_miss() && center_hit.inst_id != InvalidUI32;
            Float center_depth = z_ref;
            $if(center_valid) {
                Interaction center_it = pipeline_ref->geometry().compute_surface_interaction(center_hit, false);
                center_depth = length(center_it.pos - camera_pos);
            };

            PhaseSpaceCoordVar center_coord = SparseGather::subpixel_to_phase_space(
                dispatch_idx_val, lent, geom);

            Float3 sum_radiance = make_float3(0.f);
            Float sum_weight = 0.f;

            Float sigma_x_sq = sigma_x * sigma_x;
            Float sigma_u_sq = sigma_u * sigma_u;

            // Joint spatio-angular aperture loop (à-trous: step_size scales offsets)
            $for(dy, -spatial_radius, spatial_radius + 1) {
                $for(dx, -spatial_radius, spatial_radius + 1) {
                    Int actual_dx = dx * step_size;
                    Int actual_dy = dy * step_size;
                    Float dist_sq = cast<float>(actual_dx * actual_dx + actual_dy * actual_dy);
                    Float w_spatial = exp(-dist_sq / sigma_x_sq);

                    $for(ang_idx, 0, angular_samples) {
                        Float du = 0.f;
                        $if(angular_samples > 1) {
                            du = (cast<float>(ang_idx) / (cast<float>(angular_samples) - 1.f) - 0.5f)
                                 * angular_range;
                        };

                        Float w_angular = exp(-du * du / sigma_u_sq);

                        // Manifold transport: C = Ψ_ang(Ψ_spatial(p, Δx), Δu; z)
                        Float2 delta_x = make_float2(
                            cast<float>(actual_dx) / cast<float>(screen_size.x),
                            cast<float>(actual_dy) / cast<float>(screen_size.y));
                        PhaseSpaceCoordVar transported = ManifoldTransport::spatial_transport(
                            center_coord, delta_x);
                        PhaseSpaceCoordVar target_coord = ManifoldTransport::angular_transport(
                            transported, make_float2(du, 0.f),
                            center_depth, z_ref, beta);

                        // Map target back to subpixel index
                        Float2 target_spatial = target_coord->spatial();
                        Int target_sx = clamp(
                            cast<int>(round(target_spatial.x * cast<float>(screen_size.x))),
                            0, screen_size.x - 1);
                        Int target_sy = clamp(
                            cast<int>(round(target_spatial.y * cast<float>(screen_size.y))),
                            0, screen_size.y - 1);
                        Uint target_idx = cast<uint>(target_sy) * cast<uint>(screen_size.x) +
                                         cast<uint>(target_sx);

                        // Sparsity check
                        Uint is_sampled = param.sampling_mask.read(target_idx);
                        $if(is_sampled == 0u) { $continue; };

                        // Read neighbor radiance
                        Float4 n_radiance = param.radiance_src.read(target_idx);
                        Float n_lum = luminance(n_radiance.xyz());

                        // Edge-stopping: luminance
                        Float w_lum = exp(-abs(center_lum - n_lum) / max(sigma_lum, 0.001f));

                        // Geometry edge-stopping for ALL samples (inst_id check)
                        TriangleHitVar n_hit = param.visibility.read(target_idx);
                        Bool n_valid = !n_hit->is_miss() && n_hit.inst_id != InvalidUI32;
                        Float w_geo = 1.f;
                        $if(n_valid && center_valid) {
                            w_geo = select(center_hit.inst_id == n_hit.inst_id, 1.f, 0.05f);
                        };
                        $if(!n_valid) {
                            w_geo = select(center_valid, 0.f, 1.f);
                        };

                        Float w = w_spatial * w_angular * w_lum * w_geo;

                        sum_radiance = sum_radiance + n_radiance.xyz() * w;
                        sum_weight += w;
                    };
                };
            };

            Float3 result = sum_radiance / max(sum_weight, SSATConfig::Gather::kEpsilon);
            // Phase 3 computes depth from visibility. Preserve an explicit
            // support marker in .w so the final-view branch can distinguish a
            // valid black reconstruction from an unsupported hole.
            Float support = select(
                sum_weight > SSATConfig::Gather::kEpsilon, 1.f, 0.f);
            param.radiance_dst.write(linear_idx, make_float4(result, support));
        };

        filter_shader_ = device().compile(kernel, "SSAT-SpatialAngularFilter");
    }

    // ========================================================================
    // Dispatch
    // ========================================================================

    [[nodiscard]] CommandBatch dispatch_filter(
        BufferView<RadType4> radiance_src,
        BufferView<RadType4> radiance_dst,
        BufferView<TriangleHit> visibility,
        BufferView<uint> sampling_mask,
        BufferView<float> shear_magnitude,
        const array_float3 &camera_pos,
        const LenticularParams &lent,
        const LightFieldGeometry &geom,
        const float4x4 &l2w,
        float sigma_lum,
        float sigma_normal,
        float sigma_depth_angular,
        float sigma_x,
        float sigma_u,
        float z_ref,
        float beta,
        float angular_range,
        int spatial_radius,
        int angular_samples,
        int step_size,
        uint2 subpixel_res) noexcept {

        SpatialAngularParam param;
        param.radiance_src = radiance_src.descriptor();
        param.radiance_dst = radiance_dst.descriptor();
        param.visibility = visibility.descriptor();
        param.sampling_mask = sampling_mask.descriptor();
        param.shear_magnitude = shear_magnitude.descriptor();
        param.camera_pos = camera_pos;
        param.sigma_lum = sigma_lum;
        param.sigma_normal = sigma_normal;
        param.sigma_depth_angular = sigma_depth_angular;
        param.sigma_x = sigma_x;
        param.sigma_u = sigma_u;
        param.z_ref = z_ref;
        param.beta = beta;
        param.angular_range = angular_range;
        param.spatial_radius = spatial_radius;
        param.angular_samples = angular_samples;
        param.step_size = step_size;

        CommandBatch ret;
        ret << filter_shader_(param, lent, geom, l2w).dispatch(subpixel_res);
        return ret;
    }
};

}// namespace vision::ssat
