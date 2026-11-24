#pragma once

#include "base/using.h"
#include "base/scattering/interaction.h"
#include "base/integral/restir_shadow.h"
#include "base/mgr/geometry.h"
#include "ast/function.h"

namespace vision::restir_gi {
// Native RtxdiBridgeUserSettings::rayEpsilon is 1e-4f in the measured source;
// the benchmark driver does not override it. This is an absolute distance.
inline constexpr float NativeRayEpsilon = 1e-4f;
inline constexpr float NativeMaxRayTravel = 1e15f; // matched Config.h

[[nodiscard]] inline Float3 native_offset_origin(const Float3 &position,
                                                 const Float3 &face_normal) noexcept {
    Int3 offset = cast<int3>(face_normal * (3.f * 256.f));
    Float3 integer_position = ocarina::as<float3>(
        ocarina::as<int3>(position) + select(position < 0.f, -offset, offset));
    Float3 fixed_offset = face_normal * (3.f / 65536.f);
    return select(abs(position) < (1.f / 16.f), position + fixed_offset, integer_position);
}

// The explicit event flag retains native ShadingData.computeNewRayOrigin's
// reflection/view-side versus transmission/opposite-side distinction, including
// shading-normal events whose direction crosses the geometric normal.
[[nodiscard]] inline RayVar native_spawn_ray(const Interaction &it,
                                             const Float3 &direction,
                                             const Bool &transmission) noexcept {
    Float3 face = it.ng * select(dot(it.ng, it.wo) >= 0.f, 1.f, -1.f);
    face = select(transmission, -face, face);
    return make_ray(native_offset_origin(it.pos, face), direction, NativeMaxRayTravel);
}

// Geometric-side overload for callers that have no BSDF event. Candidate PT
// should use the explicit transmission overload above when its lobe is known.
[[nodiscard]] inline RayVar native_spawn_ray(const Interaction &it,
                                             const Float3 &direction) noexcept {
    const Float3 face = it.ng * select(dot(it.ng, direction) > 0.f, 1.f, -1.f);
    return make_ray(native_offset_origin(it.pos, face), direction, NativeMaxRayTravel);
}

// Native RtxdiApplicationBridge::setupVisibilityRay(surface,samplePosition):
// choose the face side before offsetting, then recompute normalized direction
// and subtract an absolute epsilon from the distance to the target.
[[nodiscard]] inline RayVar native_visibility_ray(const Interaction &it,
                                                  const Float3 &target) noexcept {
    const Float3 delta = target - it.pos;
    const Float3 face = it.ng * select(dot(delta, it.ng) > 0.f, 1.f, -1.f);
    const Float3 origin = native_offset_origin(it.pos, face);
    return restir_shadow::finite_ray(origin, target);
}

[[nodiscard]] inline Float4x4 native_closest_with_distance(const RayVar &ray) noexcept {
    ocarina::Function::current()->require_ray_query_compute_workgroups();
    Float distance = 0.f;
    const auto hit = Toolkit::geometry().accel().trace_closest(ray, distance);
    Float4x4 result = make_float4x4(0.f);
    result[0u] = make_float4(ocarina::as<float>(hit.inst_id), ocarina::as<float>(hit.prim_id),
                            hit.bary.x, hit.bary.y);
    result[1u].x = distance;
    return result;
}

// GI-only alpha traversal on the renderer's closest-hit API. Each rejected
// alpha triangle advances TMin by one representable distance; accepted opaque
// geometry remains in the shared acceleration structure. This adapter restarts
// traversal after a rejected hit.
template<typename MaterialInputs>
[[nodiscard]] inline TriangleHitVar native_trace_closest(const RayVar &input,
                                                        const MaterialInputs &materials,
                                                        const SampledWavelengths &swl,
                                                        Float *accepted_t = nullptr) noexcept {
    const auto &geometry = Toolkit::geometry();
    if (!accepted_t && !materials.has_alpha_tested())
        return geometry.trace_closest(input);
    RayVar ray = input;
    TriangleHitVar hit;
    $loop {
        Float4x4 packed = native_closest_with_distance(ray);
        hit.inst_id = ocarina::as<uint>(packed[0].x);
        hit.prim_id = ocarina::as<uint>(packed[0].y);
        hit.bary = packed[0].zw();
        $if(hit->is_miss()) { $break; };
        Uint material_id = geometry.data()->instances().read(hit.inst_id).mat_id;
        Bool skip = false;
        $if(materials.alpha_tested(material_id)) {
            Interaction it = geometry.compute_surface_interaction(hit, false);
            skip = materials.opacity(it, swl) < materials.alpha_cutoff(material_id);
        };
        Float distance = packed[1].x;
        $if(!skip) {
            if (accepted_t) *accepted_t = distance;
            $break;
        };
        // Use traversal's exact distance. A reconstructed world position can
        // round back to the origin and cannot safely determine the next TMin.
        Float lower = max(ray.org_min.w, max(distance, 0.f));
        ray.org_min.w = ocarina::as<float>(ocarina::as<uint>(lower) + 1u);
        $if(ray.org_min.w > ray.dir_max.w) {
            hit = TriangleHitVar{};
            $break;
        };
    };
    return hit;
}

template<typename MaterialInputs>
[[nodiscard]] inline Bool native_occluded(const Interaction &it,
                                          const Float3 &target,
                                          const MaterialInputs &materials,
                                          const SampledWavelengths &swl) noexcept {
    RayVar ray = native_visibility_ray(it, target);
    if (!materials.has_alpha_tested()) return Toolkit::geometry().trace_occlusion(ray);
    return native_trace_closest(ray, materials, swl)->is_hit();
}

}// namespace vision::restir_gi
