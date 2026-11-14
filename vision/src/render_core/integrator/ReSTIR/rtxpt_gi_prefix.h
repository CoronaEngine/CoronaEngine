#pragma once

/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

// Native three-plane producer; shared PT/Falcor inputs remain untouched.
#include "rtxpt_gi_material_inputs.h"
#include "base/integral/path_work_diagnostic.h"
#include "rtxpt_gi_interiors.h"
#include "rtxpt_gi_rays.h"
#include "rtxpt_gi_payload.h"
#include "render_core/integrator/ReSTIR/rtxpt_gi_lights.h"
#include "render_core/integrator/ReSTIR/indirect.h"
#include "base/integral/integrator.h"

#include "rtxpt_gi_prefix_storage.h"
#include "rtxpt_gi_candidate.h"

namespace vision::rtxpt_gi {
namespace prefix_detail {
[[nodiscard]] inline Float4x4 identity() noexcept {
    return make_float4x4(make_float4(1.f, 0.f, 0.f, 0.f), make_float4(0.f, 1.f, 0.f, 0.f),
        make_float4(0.f, 0.f, 1.f, 0.f), make_float4(0.f, 0.f, 0.f, 1.f));
}
// The native matrices below are indexed as rows. Component expressions keep
// that convention independent of host/DSL matrix multiplication conventions.
[[nodiscard]] inline Float4x4 multiply3(const Float4x4 &a, const Float4x4 &b) noexcept {
    Float4x4 result = identity();
    for (uint i = 0u; i < 3u; ++i) {
        Float4 row = make_float4(0.f);
        for (uint j = 0u; j < 3u; ++j)
            row[j] = (a[i].x * b[0][j] + a[i].y * b[1][j]) + a[i].z * b[2][j];
        result[i] = row;
    }
    return result;
}
[[nodiscard]] inline Float4x4 rotate_from_to(const Float3 &from, const Float3 &to) noexcept {
    Float e = dot(from, to), f = abs(e);
    Float4x4 result = identity();
    $if(!(f > float(1.0f - 1e-10f))) {
        Float3 v = cross(from, to);
        Float h = 1.f / (1.f + e);
        Float hvx = h * v.x, hvz = h * v.z;
        Float hvxy = hvx * v.y, hvxz = hvx * v.z, hvyz = hvz * v.y;
        result[0] = make_float4(e + hvx * v.x, hvxy - v.z, hvxz + v.y, 0.f);
        result[1] = make_float4(hvxy + v.z, e + h * v.y * v.y, hvyz - v.x, 0.f);
        result[2] = make_float4(hvxz - v.y, hvyz + v.x, e + hvz * v.z, 0.f);
    };
    return result;
}
[[nodiscard]] inline Float4x4 transform_frame(const Float4x4 &old,
                                        const Float3 &tangent_axis, const Float3 &bitangent_axis, const Float3 &normal_axis,
                                        const Float3 &direction, const Float3 &old_direction,
                                        const Bool &transmission) noexcept {
    Float4x4 local = identity();
    $if(transmission) { local = rotate_from_to(direction, old_direction); }
    $else {
        Float4x4 tangent = make_float4x4(make_float4(tangent_axis, 0.f),
            make_float4(bitangent_axis, 0.f), make_float4(normal_axis, 0.f), make_float4(0.f, 0.f, 0.f, 1.f));
        Float4x4 mirror = make_float4x4(make_float4(1.f, 0.f, 0.f, 0.f), make_float4(0.f, 1.f, 0.f, 0.f),
            make_float4(0.f, 0.f, -1.f, 0.f), make_float4(0.f, 0.f, 0.f, 1.f));
        Float4x4 transposed = identity();
        for (uint i = 0u; i < 3u; ++i) {
            Float4 row = make_float4(0.f);
            for (uint j = 0u; j < 3u; ++j) row[j] = tangent[j][i];
            transposed[i] = row;
        }
        local = multiply3(mirror, tangent);
        local = multiply3(transposed, local);
    };
    return multiply3(old, local);
}
[[nodiscard]] inline Float4x4 transform(const Float4x4 &old, const StandardSurfaceView &surface,
                                        const Float3 &direction, const Float3 &old_direction,
                                        const Bool &transmission) noexcept {
    return transform_frame(old, surface.tangent, surface.bitangent, surface.normal,
                           direction, old_direction, transmission);
}
[[nodiscard]] inline Float3 virtual_position(const Float4x4 &matrix, const Float3 &position,
                                             const Float3 &pivot) noexcept {
    Float3 delta = position - pivot;
    return make_float3(dot(matrix[0].xyz(), delta), dot(matrix[1].xyz(), delta),
        dot(matrix[2].xyz(), delta)) + pivot;
}
[[nodiscard]] inline Float average(const Float3 &v) noexcept { return (v.x + v.y + v.z) / 3.f; }
[[nodiscard]] inline Float3 half_round(const Float3 &v) noexcept {
    Float3 packed = clamp(v, make_float3(-65504.f), make_float3(65504.f));
    return make_float3(ocarina::f16tof32(ocarina::f32tof16(packed.x)),
                       ocarina::f16tof32(ocarina::f32tof16(packed.y)),
                       ocarina::f16tof32(ocarina::f32tof16(packed.z)));
}
[[nodiscard]] inline Uint4 pack_hit(const TriangleHitVar &hit) noexcept {
    return make_uint4(hit.inst_id, hit.prim_id, ocarina::as<uint2>(Float2{hit.bary}));
}
[[nodiscard]] inline TriangleHitVar unpack_hit(const RtxptGIPrefixPlaneVar &node) noexcept {
    TriangleHitVar hit;
    hit.inst_id = node.hit_words.x; hit.prim_id = node.hit_words.y;
    hit.bary = ocarina::as<float2>(Uint2{node.hit_words.zw()});
    return hit;
}
[[nodiscard]] inline Float4x4 unpack_transform(const RtxptGIPrefixPlaneVar &node) noexcept {
    Float4 a = ocarina::as<float4>(Uint4{node.hit_words});
    Float4 b = node.virtual_position;
    Float4x4 result = identity();
    result[0] = make_float4(a.xyz(), 0.f);
    result[1] = make_float4(a.w, b.xy(), 0.f);
    result[2] = make_float4(b.zw(), node.throughput_flags.w, 0.f);
    return result;
}
inline void queue_node(const PrefixStorage &storage, const Uint &plane, const RayVar &ray,
                       const Float3 &throughput, const Uint &depth, const Float &scene_length,
                       const Uint &branch, const Bool &dominant, const InteriorState &interiors,
                       const Float4x4 &matrix) noexcept {
    RtxptGIPrefixPlaneVar node;
    node.hit_words = ocarina::as<uint4>(make_float4(matrix[0].xyz(), matrix[1].x));
    node.origin_distance = make_float4(ray->origin(), scene_length);
    node.direction_vertex = make_float4(ray->direction(), cast<float>(depth));
    node.throughput_flags = make_float4(throughput, matrix[2].z);
    node.interiors_branch = make_uint4(interiors.slots, branch,
        interiors.rejected_hits | select(dominant, PrefixDominantBit, 0u));
    node.virtual_position = make_float4(matrix[1].yz(), matrix[2].xy());
    storage.write(plane, node);
}
}// namespace prefix_detail

// Matched default benchmark: three planes, stable-depth5, primary replacement
// enabled. This performs deterministic native delta enumeration, not a sampled
// delta chain. Its separate producer cost must be included in candidate+GI.
inline void build_standard_prefix(const PrefixStorage &storage, ReSTIRGI &env) noexcept {
    using namespace prefix_detail;
    const auto &materials = env.rtxpt_materials();
    const auto &lights = env.rtxpt_lights();
    const auto &geometry = Toolkit::geometry();
    const auto &swl = env.sampled_wavelengths();
    if (storage.denoiser.enabled()) {
        for (uint p = 0u; p < 3u; ++p) {
            reblur::GuideVar empty;
            empty.position = make_float4(0.f); empty.identity = make_uint4(0u);
            empty.normal_roughness = make_float4(0.f, 0.f, 1.f, 1.f);
            storage.denoiser.write_guide(dispatch_id(), Uint{p}, empty);
        }
    }
    RayVar camera = Toolkit::frame_buffer().rays().read(dispatch_id())->to_ray();
    const Float near_z = camera->t_min() * dot(camera->direction(), Toolkit::scene().sensor()->device_forward());
    camera->update_origin(camera->origin() + camera->direction() * camera->t_min());
    camera.org_min.w = 0.f;
    Uint4 header = make_uint4(PrefixQueued, PrefixInvalid, PrefixInvalid, 0u);
    Float3 stable = make_float3(0.f);
    InteriorState empty;
    queue_node(storage, Uint{0u}, camera, Float3{make_float3(1.f)}, Uint{0u}, Float{0.f},
               Uint{1u}, Bool{true}, empty, identity());
    const Uint max_depth = min(env.integrator()->runtime_suffix_depth() + 1u, 5u);

    $for(plane, 3u) {
        $if(header[plane] != PrefixQueued) { $continue; };
        RtxptGIPrefixPlaneVar queued = storage.read(plane);
        header[plane] = 0u; // native "exploration just started", not an empty slot
        Float4x4 matrix = unpack_transform(queued);
        // A split path was packed when enqueued, then unpacked in RayGen.
        // The subsequent TraceRay boundary below is another distinct roundtrip.
        $if(plane != 0u) { matrix = payload_image_roundtrip(matrix); };
        Float3 beta = queued.throughput_flags.xyz();
        Uint depth = cast<uint>(queued.direction_vertex.w);
        Float travelled = queued.origin_distance.w;
        Uint branch = queued.interiors_branch.z;
        Bool dominant = (queued.interiors_branch.w & PrefixDominantBit) != 0u;
        InteriorState interiors;
        interiors.slots = queued.interiors_branch.xy();
        interiors.rejected_hits = queued.interiors_branch.w & 0xffu;
        Float3 queued_origin = queued.origin_distance.xyz(), queued_direction = queued.direction_vertex.xyz();
        RayVar ray = make_ray(queued_origin, queued_direction, NativeMaxRayTravel);
        $if(plane == 0u) { ray = camera; };

        auto payload_roundtrip = [&] {
            beta = payload_positive(beta);
            stable = payload_positive(stable);
            matrix = payload_image_roundtrip(matrix);
        };
        $loop {
            // RayGen pack -> closest-hit/miss unpack, before the handler.
            payload_roundtrip();
            Float segment = 0.f;
            TriangleHitVar hit = native_trace_closest(ray, materials, swl, false, &segment);
            depth += 1u;
            Float3 point = ray->origin() + ray->direction() * NativeMaxRayTravel;
            StandardSurfaceView surface{Interaction{false}};
            $if(hit->is_hit()) {
                Interaction it = geometry.compute_surface_interaction(hit, true);
                it.wo = -ray->direction();
                ray.dir_max.w = segment;
                point = it.pos;
                travelled = min(travelled + segment, NativeMaxRayTravel);
                $if(interiors.slots.x != 0u) {
                    Uint top = interior_outside_id(interiors.slots, Uint{0u}, Bool{true});
                    beta *= exp(-materials.sigma_a(top, swl) * segment);
                };
                Bool entering = dot(it.ng, it.wo) >= 0.f;
                Uint outside = interior_outside_id(interiors.slots, it.material_id(), entering);
                Float outside_ior = 1.f;
                $if(outside != NativeNoInteriorMaterial) { outside_ior = materials.ior(outside, swl); };
                surface = materials.load(it, swl, outside_ior);
                Uint priority = (surface.material.header.x >> 16u) & 0xfu;
                $if(!interior_true_hit(interiors.slots, priority)) {
                    $if(interiors.rejected_hits >= NativeMaxRejectedIntersections) {
                        header[plane] = PrefixInvalid; payload_roundtrip(); $break;
                    };
                    interiors.rejected_hits += 1u;
                    interiors.slots = interior_update(interiors.slots, it.material_id(), priority, entering);
                    ray = native_spawn_ray(it, ray->direction(), Bool{true});
                    depth -= 1u;
                    payload_roundtrip(); // rejected hit still returns through payload
                    $continue;
                };
                // Falcor DI keeps the physical primary and its one-segment
                // continuous reflection direct term. Native private transmission
                // and deeper delta-tree emission are not covered by that path.
                Bool direct_owned = depth == 1u || (depth == 2u && (branch & 3u) == 1u);
                $if(it.has_emission() && !direct_owned) {
                    stable += max(make_float3(0.f), beta * lights.emissive(it, ray->origin()).radiance);
                };
            } $else {
                travelled = min(travelled + NativeMaxRayTravel, NativeMaxRayTravel);
                Bool direct_owned = depth == 1u || (depth == 2u && (branch & 3u) == 1u);
                $if(!direct_owned) {
                    stable += max(make_float3(0.f), beta * lights.environment(ray->direction(), Uint{0u}).radiance);
                };
            };
            $if(depth == 1u) { header.w = ocarina::as<uint>(travelled) & 0xfffffffcu; };

            Bool continue_current = false;
            $if(hit->is_hit() && surface.valid && depth < max_depth) {
                StandardDeltaLobes delta = standard_delta_lobes(surface.material, surface.normal,
                    surface.view, surface.to_local(surface.view));
                Container<uint> nonzero{3u}, available{2u};
                Bool possible_transmission = false;
                for (uint k = 0u; k < 3u; ++k) {
                    $if(k < delta.count && average(delta.lobes[k].throughput) > .001f) {
                        nonzero.push_back(Uint{k});
                        possible_transmission |= delta.lobes[k].transmission;
                    };
                }
                for (uint p = 1u; p < 3u; ++p) {
                    $if(header[p] == PrefixInvalid) { available.push_back(Uint{p}); };
                }
                Bool allow_psr = plane == 0u && nonzero.count() == 1u && !possible_transmission;
                Uint preferred_p1 = (surface.material.header.y >> 18u) & 0xfu;
                Bool reuse = ((plane != 0u && nonzero.count() > 0u) || allow_psr) &&
                             delta.non_delta_part <= 1e-5f && (plane == 0u || preferred_p1 > 0u);
                Uint count = min(nonzero.count(), available.count() + cast<uint>(reuse));
                Int reuse_lobe = -1;
                $if(reuse && count > 0u) { reuse_lobe = cast<int>(nonzero[count - 1u]); count -= 1u; };

                for (uint k = 0u; k < 3u; ++k) {
                    Uint fork_plane = PrefixInvalid;
                    $for(i, count) { $if(nonzero[i] == k) { fork_plane = available[i]; }; };
                    Bool reused = reuse_lobe == int(k);
                    $if(fork_plane != PrefixInvalid || reused) {
                        auto &lobe = delta.lobes[k];
                        Float3 direction = surface.to_world(lobe.direction);
                        Float3 next_beta = beta * lobe.throughput;
                        InteriorState next_interior;
                        next_interior.slots = interiors.slots;
                        next_interior.rejected_hits = interiors.rejected_hits;
                        $if(lobe.transmission && (surface.material.header.x & (1u << 29u)) == 0u) {
                            next_interior.slots = interior_update(next_interior.slots, surface.it.material_id(),
                                (surface.material.header.x >> 16u) & 0xfu, surface.front_facing);
                        };
                        Float4x4 next_matrix = transform(matrix, surface, direction, ray->direction(), lobe.transmission);
                        RayVar next_ray = native_spawn_ray(surface.it, direction, lobe.transmission);
                        Bool next_dominant = dominant;
                        $if(dominant && (!reused || count > 0u)) { next_dominant = preferred_p1 == k + 1u; };
                        Uint next_branch = (branch << 2u) | k;
                        $if(!reused) {
                            queue_node(storage, fork_plane, next_ray, next_beta, depth, travelled,
                                       next_branch, next_dominant, next_interior, next_matrix);
                            header[fork_plane] = PrefixQueued;
                        } $else {
                            beta = next_beta; matrix = next_matrix; ray = next_ray;
                            branch = next_branch; dominant = next_dominant;
                            interiors.slots = next_interior.slots;
                            continue_current = true;
                        };
                    };
                }
            };
            $if(continue_current) { payload_roundtrip(); $continue; };

            RtxptGIPrefixPlaneVar node;
            node.hit_words = pack_hit(hit);
            Float4 stable_ray = payload_stable_ray(ray->direction(), travelled);
            node.origin_distance = make_float4(ray->origin(), stable_ray.w);
            node.direction_vertex = make_float4(stable_ray.xyz(), cast<float>(depth));
            node.throughput_flags = make_float4(half_round(beta), 0.f);
            node.interiors_branch = make_uint4(interiors.slots, branch,
                interiors.rejected_hits | select(dominant, PrefixDominantBit, 0u));
            Float first = ocarina::as<float>(header.w & 0xfffffffcu);
            Float3 pivot = camera->origin() + camera->direction() * first;
            Float3 virtual_point = virtual_position(matrix, point, pivot);
            Float view_depth = near_z + dot(camera->direction() * stable_ray.w, Toolkit::scene().sensor()->device_forward());
            node.virtual_position = make_float4(virtual_point, view_depth);
            if (storage.denoiser.enabled()) {
                $if(hit->is_hit() && surface.valid) {
                    reblur::GuideVar guide;
                    auto &sensor = Toolkit::scene().sensor();
                    Float3 normal = normalize(make_float3(dot(matrix[0].xyz(), surface.normal),
                        dot(matrix[1].xyz(), surface.normal), dot(matrix[2].xyz(), surface.normal)));
                    const auto estimates = native_reblur_estimates(surface.material, surface.normal, surface.view);
                    Float current_depth = sensor->linear_depth(virtual_point);
                    Float previous_depth = sensor->prev_linear_depth(virtual_point);
                    Float3 motion = make_float3(0.f);
                    $if(current_depth > 0.f && previous_depth > 0.f) {
                        Float2 current_raster = sensor->raster_coord(virtual_point).xy();
                        Float2 previous_raster = sensor->prev_raster_coord(virtual_point).xy();
                        motion = make_float3(current_raster - previous_raster, previous_depth - current_depth);
                    };
                    guide.position = make_float4(virtual_point, view_depth);
                    // bit0 valid, bit1 explicit native Z, upper16 binary16 Z;
                    // bits8..15 remain available for disocclusion relaxation.
                    guide.identity = make_uint4(hit.inst_id, surface.it.material_id(), branch,
                        native_reblur_motion_flags(Float{motion.z}));
                    guide.normal_roughness = make_float4(normal, saturate(surface.material.p0.w));
                    guide.diffuse_factor = make_float4(estimates.diffuse, native_reblur_half_signed(Float{motion.x}));
                    guide.specular_factor = make_float4(estimates.specular, native_reblur_half_signed(Float{motion.y}));
                    storage.denoiser.write_guide(dispatch_id(), plane, guide);
                };
            }
            storage.write(plane, node);
            header[plane] = branch;
            $if(dominant && plane != 0u) { header.w = (header.w & 0xfffffffcu) | plane; };
            // StoreStablePlane observed the handler values above. Only now does
            // closest-hit/miss pack -> RayGen unpack quantize the returned path.
            payload_roundtrip();
            $break;
        };
    };
    storage.headers.write(dispatch_id(), header);
    storage.stable_radiance.write(dispatch_id(), make_float4(half_round(clamp(stable, make_float3(0.f), make_float3(65504.f))), 0.f));
    if (storage.denoiser.enabled()) {
        storage.denoiser.bypass->write(dispatch_id(), make_float4(half_round(clamp(stable, make_float3(0.f), make_float3(65504.f))), 0.f));
    }
}
struct PrefixTraceResult {
    CandidateTraceResult transport;
    StandardSurfaceView receiver{Interaction{false}};
    Float3 prefix_throughput{make_float3(0.f)};
    Float3 current_virtual_position{make_float3(0.f)}, previous_virtual_position{make_float3(0.f)};
    Uint dominant_plane{0u}, vertex_depth{0u};
};

[[nodiscard]] PrefixTraceResult trace_standard_prefix_candidate(const PrefixStorage &storage, ReSTIRGI &env,
                                                                 path_cost::PathWorkCountersVar *work_counters = nullptr) noexcept;
}// namespace vision::rtxpt_gi
