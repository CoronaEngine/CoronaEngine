//
// Created by Zero on 2025/01/26.
// Sparse Gathering Operator Υ for Light Field
//
// Implements the sparse manifold gathering operator that bridges
// continuous phase-space coordinates to discrete buffer storage.
// Uses a separable anisotropic kernel for weighted aggregation.
//

#pragma once

#include "math/basic_types.h"
#include "dsl/dsl.h"
#include "base/mgr/geometry.h"
#include "phase_space.h"
#include "utils.h"
#include "ssat_config.h"

namespace vision::ssat {
using namespace ocarina;

// ============================================================================
// Sparse Gathering Operator Υ
// ============================================================================

/// Static DSL functions implementing the Sparse Gathering Operator
/// Υ(F, C; κ) = Σ_q [I_S(q) · κ(C, Φ(q)) · F(q)] / (Σ_q [I_S(q) · κ(C, Φ(q))] + ε)
class SparseGather {
public:
    
    // ========================================================================
    // Subpixel to Phase-Space Coordinate Conversion
    // ========================================================================
    
    /// Convert a subpixel index to its phase-space coordinate
    /// This implements the Ray Transfer Operator Φ mapping p → (x, u)
    /// 
    /// For light field displays:
    /// - x is the subpixel location on the display plane Ω
    /// - u is the angular coordinate on the viewing plane Π
    /// 
    /// @param subpixel_idx Dispatch index (dx, dy) where dx = x*3 + k
    /// @param lent Lenticular parameters
    /// @param geom Light field geometry
    /// @return Phase-space coordinate (x, u)
    [[nodiscard]] static PhaseSpaceCoordVar subpixel_to_phase_space(
        const Uint2 &subpixel_idx,
        Var<LenticularParams> lent,
        Var<LightFieldGeometry> geom) noexcept {
        
        // Decode subpixel index
        Uint dx = subpixel_idx.x;
        Uint dy = subpixel_idx.y;
        Uint pixel_x = dx / 3u;
        Uint channel_k = dx % 3u;
        Uint pixel_y = dy;
        
        // Use the same discrete view mapping as light-field ray generation.
        Uint view_count = lightfield_dsl_view_count(lent);
        Uint view_id = lightfield_subpixel_view_id(
            pixel_x, pixel_y, channel_k, lent, view_count);
        Float num_views_f = cast<float>(view_count);
        
        // Compute spatial coordinate x (subpixel position on focal plane, normalized)
        Float res_w = lent.res_w;
        Float res_h = lent.res_h;
        Float W_f = geom.W_f;
        Float H_f = geom.H_f;
        
        Float pw = W_f / res_w;  // Pixel width
        Float ph = H_f / res_h;  // Pixel height
        
        // Subpixel offset within pixel
        Float subpixel_offset = (cast<float>(channel_k) + 0.5f) * (pw / 3.f);
        
        // Focal plane coordinates (local)
        Float focal_x = -W_f / 2.f + cast<float>(pixel_x) * pw + subpixel_offset;
        Float focal_y = H_f / 2.f - (cast<float>(pixel_y) + 0.5f) * ph;
        
        // Normalize to [0, 1] range for spatial coordinate
        Float x_norm = (focal_x + W_f / 2.f) / W_f;
        Float y_norm = (H_f / 2.f - focal_y) / H_f;
        
        // Compute angular coordinate u (based on view ID)
        // Camera position determines viewing angle
        Float array_angle_rad = radians(geom.array_angle_deg);
        Float u_normalized = select(num_views_f > 1.f,
                                    cast<float>(view_id) / (num_views_f - 1.f),
                                    0.5f);
        Float angle_offset = (u_normalized - 0.5f) * array_angle_rad;
        
        // Angular coordinate in normalized space
        Float u_x = u_normalized;
        Float u_y = 0.5f;  // Assuming horizontal-only parallax for simplicity
        
        PhaseSpaceCoordVar result;
        result.x = make_float2(x_norm, y_norm);
        result.u = make_float2(u_x, u_y);
        return result;
    }
    
    /// Convert phase-space coordinate back to subpixel buffer index
    /// This is the inverse of subpixel_to_phase_space (approximate)
    /// 
    /// @param coord Phase-space coordinate
    /// @param lent Lenticular parameters
    /// @param geom Light field geometry
    /// @return Linear buffer index, or InvalidUI32 if out of bounds
    [[nodiscard]] static Uint phase_space_to_buffer_index(
        const PhaseSpaceCoordVar &coord,
        Var<LenticularParams> lent,
        Var<LightFieldGeometry> geom) noexcept {
        
        Float res_w = lent.res_w;
        Float res_h = lent.res_h;
        
        // Convert normalized spatial coordinate to pixel coordinates
        Float pixel_x_f = coord.x.x * res_w;
        Float pixel_y_f = coord.x.y * res_h;
        
        // Clamp to valid range
        Int pixel_x = clamp(cast<int>(floor(pixel_x_f)), 0, cast<int>(res_w) - 1);
        Int pixel_y = clamp(cast<int>(floor(pixel_y_f)), 0, cast<int>(res_h) - 1);
        
        // Determine channel from angular coordinate
        // u.x encodes the view, which maps to channel via lenticular formula
        // For simplicity, we find the closest channel (0=R, 1=G, 2=B)
        Float u_x = coord.u.x;
        Uint channel_k = clamp(cast<uint>(u_x * 3.f), 0u, 2u);
        
        // Compute linear index: idx = dy * (res_w * 3) + dx, where dx = x*3 + k
        Uint dx = cast<uint>(pixel_x) * 3u + channel_k;
        Uint dy = cast<uint>(pixel_y);
        Uint res_w_3 = cast<uint>(res_w) * 3u;
        
        return dy * res_w_3 + dx;
    }
    
    // ========================================================================
    // Core Gathering Operations
    // ========================================================================
    
    /// Gather radiance from a sparse buffer at a target phase-space coordinate
    /// Implements the Υ operator with separable anisotropic kernel
    /// 
    /// Υ(F, C; κ) = Σ_q [I_S(q) · κ(C, Φ(q)) · F(q)] / (Σ_q [I_S(q) · κ(C, Φ(q))] + ε)
    /// 
    /// κ(C, Q) = exp(-||x'-x_q||²/σ_x²) · exp(-||u'-u_q||²/σ_u²)
    /// 
    /// @param buffer Radiance buffer (subpixel-sized)
    /// @param target Target phase-space coordinate C
    /// @param lent Lenticular parameters
    /// @param geom Light field geometry
    /// @param sigma_x Spatial bandwidth
    /// @param sigma_u Angular bandwidth
    /// @param neighbor_radius Search radius in pixels
    /// @return Gathered radiance value (RGB + weight in .w)
    [[nodiscard]] static Float4 gather(
        const BufferVar<RadType4> &buffer,
        const PhaseSpaceCoordVar &target,
        Var<LenticularParams> lent,
        Var<LightFieldGeometry> geom,
        const Float &sigma_x,
        const Float &sigma_u,
        const Int &neighbor_radius) noexcept {
        
        Float res_w = lent.res_w;
        Float res_h = lent.res_h;
        Uint res_w_u = cast<uint>(res_w);
        Uint res_h_u = cast<uint>(res_h);
        Uint res_w_3 = res_w_u * 3u;
        
        // Convert target spatial coordinate to pixel space
        Float target_pixel_x = target.x.x * res_w;
        Float target_pixel_y = target.x.y * res_h;
        Int center_x = cast<int>(floor(target_pixel_x));
        Int center_y = cast<int>(floor(target_pixel_y));
        
        Float3 sum_radiance = make_float3(0.f);
        Float sum_weight = 0.f;
        
        // Iterate over neighborhood N(C)
        $for(dy, -neighbor_radius, neighbor_radius + 1) {
            $for(dx, -neighbor_radius, neighbor_radius + 1) {
                Int neighbor_x = center_x + dx;
                Int neighbor_y = center_y + dy;
                
                // Bounds check
                Bool in_bounds = neighbor_x >= 0 && neighbor_x < cast<int>(res_w) &&
                                 neighbor_y >= 0 && neighbor_y < cast<int>(res_h);
                
                $if(in_bounds) {
                    // For each pixel, we have 3 channels (subpixels)
                    $for(k, 0, 3) {
                        // Compute buffer index
                        Uint buf_dx = cast<uint>(neighbor_x) * 3u + cast<uint>(k);
                        Uint buf_dy = cast<uint>(neighbor_y);
                        Uint buf_idx = buf_dy * res_w_3 + buf_dx;
                        
                        // Get neighbor's phase-space coordinate
                        Uint2 neighbor_subpixel = make_uint2(buf_dx, buf_dy);
                        PhaseSpaceCoordVar neighbor_coord = subpixel_to_phase_space(
                            neighbor_subpixel, lent, geom);
                        
                        // Compute separable kernel weight κ(C, Φ(q))
                        Float w_kernel = target->kernel_weight(neighbor_coord, sigma_x, sigma_u);
                        
                        // Read radiance from buffer
                        Float4 neighbor_radiance = buffer.read(buf_idx);
                        
                        // Sparsity indicator I_S(q) - check if sample is valid
                        // We use luminance > 0 or explicit validity flag
                        Float neighbor_lum = luminance(neighbor_radiance.xyz());
                        Float sparsity_indicator = select(neighbor_lum > 0.f || neighbor_radiance.w > 0.f, 
                                                         1.f, 0.f);
                        
                        // Accumulate weighted contribution
                        Float effective_weight = sparsity_indicator * w_kernel;
                        sum_radiance = sum_radiance + neighbor_radiance.xyz() * effective_weight;
                        sum_weight = sum_weight + effective_weight;
                    };
                };
            };
        };
        
        // Normalize
        Float inv_weight = 1.f / (sum_weight + SSATConfig::Gather::kEpsilon);
        Float3 result_radiance = sum_radiance * inv_weight;
        
        return make_float4(result_radiance, sum_weight);
    }
    
    /// Channel-specific gather for monochromatic subpixels
    /// Extracts the relevant channel based on the subpixel's spectral basis
    /// I_sample = e_c^T · Υ(S_curr, C_target)
    /// 
    /// @param buffer Radiance buffer
    /// @param target Target phase-space coordinate
    /// @param channel_k Color channel (0=R, 1=G, 2=B)
    /// @param lent Lenticular parameters
    /// @param geom Light field geometry
    /// @param sigma_x Spatial bandwidth
    /// @param sigma_u Angular bandwidth
    /// @param neighbor_radius Search radius
    /// @return Channel intensity and total weight
    [[nodiscard]] static Float2 gather_channel(
        const BufferVar<RadType4> &buffer,
        const PhaseSpaceCoordVar &target,
        const Uint &channel_k,
        Var<LenticularParams> lent,
        Var<LightFieldGeometry> geom,
        const Float &sigma_x,
        const Float &sigma_u,
        const Int &neighbor_radius) noexcept {
        
        // Get full RGB gather
        Float4 gathered = gather(buffer, target, lent, geom, sigma_x, sigma_u, neighbor_radius);
        
        // Extract the relevant channel based on spectral basis e_c
        Float channel_value = select(channel_k == 0u, gathered.x,
                             select(channel_k == 1u, gathered.y,
                                                     gathered.z));
        
        return make_float2(channel_value, gathered.w);
    }
    
    // ========================================================================
    // Visibility-Aware Gathering
    // ========================================================================
    
    /// Gather with geometry-based rejection using visibility buffer
    /// Extends basic gathering with depth and normal consistency checks
    /// 
    /// @param buffer Radiance buffer
    /// @param visibility Visibility buffer (TriangleHit)
    /// @param target Target phase-space coordinate
    /// @param center_depth Depth at center pixel
    /// @param center_normal Normal at center pixel
    /// @param camera_pos Camera position for depth computation
    /// @param lent Lenticular parameters
    /// @param geom Light field geometry
    /// @param sigma_x Spatial bandwidth
    /// @param sigma_u Angular bandwidth
    /// @param sigma_z Depth sigma for geometry weight
    /// @param neighbor_radius Search radius
    /// @return Gathered radiance with geometry-aware weighting
    [[nodiscard]] static Float4 gather_with_geometry(
        const Geometry &geometry,
        const BufferVar<float4> &buffer,
        const BufferVar<TriangleHit> &visibility,
        const PhaseSpaceCoordVar &target,
        const Float &center_depth,
        const Float3 &center_normal,
        const Float3 &camera_pos,
        Var<LenticularParams> lent,
        Var<LightFieldGeometry> geom,
        const Float &sigma_x,
        const Float &sigma_u,
        const Float &sigma_z,
        const Int &neighbor_radius) noexcept {
        
        Float res_w = lent.res_w;
        Float res_h = lent.res_h;
        Uint res_w_u = cast<uint>(res_w);
        Uint res_w_3 = res_w_u * 3u;
        
        // Convert target to pixel space
        Float target_pixel_x = target.x.x * res_w;
        Float target_pixel_y = target.x.y * res_h;
        Int center_x = cast<int>(floor(target_pixel_x));
        Int center_y = cast<int>(floor(target_pixel_y));
        
        Float3 sum_radiance = make_float3(0.f);
        Float sum_weight = 0.f;
        
        $for(dy, -neighbor_radius, neighbor_radius + 1) {
            $for(dx, -neighbor_radius, neighbor_radius + 1) {
                Int neighbor_x = center_x + dx;
                Int neighbor_y = center_y + dy;
                
                Bool in_bounds = neighbor_x >= 0 && neighbor_x < cast<int>(res_w) &&
                                 neighbor_y >= 0 && neighbor_y < cast<int>(res_h);
                
                $if(in_bounds) {
                    $for(k, 0, 3) {
                        Uint buf_dx = cast<uint>(neighbor_x) * 3u + cast<uint>(k);
                        Uint buf_dy = cast<uint>(neighbor_y);
                        Uint buf_idx = buf_dy * res_w_3 + buf_dx;
                        
                        // Get neighbor phase-space coordinate
                        Uint2 neighbor_subpixel = make_uint2(buf_dx, buf_dy);
                        PhaseSpaceCoordVar neighbor_coord = subpixel_to_phase_space(
                            neighbor_subpixel, lent, geom);
                        
                        // Phase-space kernel weight
                        Float w_kernel = target->kernel_weight(neighbor_coord, sigma_x, sigma_u);
                        
                        // Read visibility for geometry checks
                        TriangleHitVar neighbor_hit = visibility.read(buf_idx);
                        Bool neighbor_valid = !neighbor_hit->is_miss() && 
                                             neighbor_hit.inst_id != InvalidUI32;
                        
                        Float neighbor_depth = center_depth;  // Default if invalid
                        Float3 neighbor_normal = center_normal;
                        
                        $if(neighbor_valid) {
                            Interaction neighbor_it = geometry.compute_surface_interaction(neighbor_hit, false);
                            neighbor_depth = length(neighbor_it.pos - camera_pos);
                            neighbor_normal = neighbor_it.ng;
                        };
                        
                        // Compute angular baseline for w_geo
                        Float2 delta_u = target.u - neighbor_coord.u;
                        Float angular_baseline = length(delta_u);
                        
                        // Spatio-angular geometric weight w_geo
                        Float w_geo = weight_spatio_angular_geo(
                            center_depth, neighbor_depth, angular_baseline, sigma_z);
                        
                        // Normal consistency weight
                        Float w_normal = weight_normal(center_normal, neighbor_normal, 
                                                       SSATConfig::SpatialAngular::kDefaultSigmaNormal);
                        
                        // Read radiance
                        Float4 neighbor_radiance = buffer.read(buf_idx);
                        Float neighbor_lum = luminance(neighbor_radiance.xyz());
                        Float sparsity_indicator = select(neighbor_lum > 0.f, 1.f, 0.f);
                        
                        // Combined weight
                        Float total_weight = sparsity_indicator * w_kernel * w_geo * w_normal;
                        
                        sum_radiance = sum_radiance + neighbor_radiance.xyz() * total_weight;
                        sum_weight = sum_weight + total_weight;
                    };
                };
            };
        };
        
        Float inv_weight = 1.f / (sum_weight + SSATConfig::Gather::kEpsilon);
        Float3 result_radiance = sum_radiance * inv_weight;
        
        return make_float4(result_radiance, sum_weight);
    }
};

}// namespace vision::ssat
