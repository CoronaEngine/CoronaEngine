#include "restir_gi_pt.h"
#include "restir_gi_rays.h"
#include "restir_gi.h"
#include "base/integral/integrator.h"
#include "base/scattering/gi_ideal_glass.h"

namespace vision::restir_gi {
namespace {
using namespace ocarina;

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

[[nodiscard]] inline Float3 virtual_position(const Float4x4 &matrix, const Float3 &position,
                                             const Float3 &pivot) noexcept {
    Float3 delta = position - pivot;
    return make_float3(dot(matrix[0].xyz(), delta), dot(matrix[1].xyz(), delta),
        dot(matrix[2].xyz(), delta)) + pivot;
}

[[nodiscard]] inline Uint4 pack_hit(const TriangleHitVar &hit) noexcept {
    return make_uint4(hit.inst_id, hit.prim_id, ocarina::as<uint2>(Float2{hit.bary}));
}

[[nodiscard]] inline Float4x4 unpack_transform(const ReSTIRGIPrefixPlaneVar &node) noexcept {
    Float4 a = ocarina::as<float4>(Uint4{node.hit_words});
    Float4 b = node.virtual_position;
    Float4x4 result = identity();
    result[0] = make_float4(a.xyz(), 0.f);
    result[1] = make_float4(a.w, b.xy(), 0.f);
    result[2] = make_float4(b.zw(), node.throughput_flags.w, 0.f);
    return result;
}

void queue_pt_plane(const PrefixStorage &storage, const Uint &plane, const RayVar &ray,
                    const Float3 &beta, const Uint &depth, const Float &distance,
                    const Uint &branch, const Bool &dominant, const Bool &ideal_path, const Float &eta_scale,
                    const Float4x4 &matrix) {
    ReSTIRGIPrefixPlaneVar node;
    node.hit_words = as<uint4>(make_float4(matrix[0].xyz(), matrix[1].x));
    node.virtual_position = make_float4(matrix[1].yz(), matrix[2].xy());
    node.origin_distance = make_float4(ray->origin(), distance);
    node.direction_vertex = make_float4(ray->direction(), cast<float>(depth));
    node.throughput_flags = make_float4(beta, matrix[2].z);
    node.interiors_branch = make_uint4(as<uint>(eta_scale), cast<uint>(ideal_path), branch,
                                      select(dominant, PrefixDominantBit, 0u));
    storage.write(plane, node);
}

}// namespace

void build_pt_prefix(const PrefixStorage &storage, ReSTIRGI &env, const Uint &primary_flag) noexcept {
    const auto &geometry = Toolkit::geometry();
    const auto &swl = env.sampled_wavelengths();
    auto &lights = Toolkit::renderer().light_sampler();
    const Bool first_reflection_covered_by_di = !BxDFFlag::is_specular(primary_flag);
    RayVar camera = Toolkit::frame_buffer().rays().read(dispatch_id())->to_ray();
    Float near_depth = camera->t_min() * dot(camera->direction(), Toolkit::scene().sensor()->device_forward());
    camera->update_origin(camera->origin() + camera->direction() * camera->t_min());
    camera.org_min.w = 0.f;
    Uint4 header = make_uint4(PrefixQueued, PrefixInvalid, PrefixInvalid, 3u);
    Uint dominant_plane = 3u;
    Float3 stable = make_float3(0.f);
    queue_pt_plane(storage, Uint{0u}, camera, Float3{make_float3(1.f)}, Uint{0u}, Float{0.f},
                   Uint{1u}, Bool{true}, Bool{true}, Float{1.f}, identity());
    Uint limit = min(env.integrator()->runtime_suffix_depth() + 1u, 5u);
    $for(plane, 3u) {
        $if(header[plane] != PrefixQueued) { $continue; };
        auto queued = storage.read(plane);
        Float4x4 matrix = unpack_transform(queued);
        Float3 beta = queued.throughput_flags.xyz();
        Float eta_scale = as<float>(queued.interiors_branch.x);
        Uint depth = cast<uint>(queued.direction_vertex.w), branch = queued.interiors_branch.z;
        Float travelled = queued.origin_distance.w;
        Bool dominant = (queued.interiors_branch.w & PrefixDominantBit) != 0u;
        Bool ideal_path = queued.interiors_branch.y != 0u;
        Float3 origin = queued.origin_distance.xyz(), direction = queued.direction_vertex.xyz();
        RayVar ray = make_ray(origin, direction, NativeMaxRayTravel);
        $if(plane == 0u) { ray = camera; };
        $loop {
            auto hit = geometry.trace_closest(ray);
            depth += 1u;
            Float3 point = ray->origin() + ray->direction() * Toolkit::scene().world_diameter();
            Interaction it{false};
            Bool consumed = false, continued = false, has_remainder = false;
            Float3 shading_normal = make_float3(0.f, 0.f, 1.f);
            Float3 guide_diffuse = make_float3(0.f), guide_specular = make_float3(0.f);
            Float roughness = 1.f;
            $if(hit->is_miss()) {
                $if(gi_prefix_stable_emission_allowed(ideal_path, depth, branch, first_reflection_covered_by_di)) {
                    LightSampleContext ref; ref.pos = ray->origin(); ref.ng = ray->direction();
                    stable += beta * Toolkit::spectrum()->linear_srgb(lights->evaluate_miss_wi(ref, ray->direction(), swl, LightEvalMode::L).L, swl) *
                        env.integrator()->correct_bsdf_weight(Float{1.f}, depth - 1u);
                };
                travelled += Toolkit::scene().world_diameter();
            } $else {
                it = geometry.compute_surface_interaction(hit, ray);
                point = it.pos;
                travelled += length(it.pos - ray->origin());
                $if(it.has_emission() && it.has_material() &&
                    gi_prefix_stable_emission_allowed(ideal_path, depth, branch, first_reflection_covered_by_di)) {
                    LightSampleContext ref; ref.pos = ray->origin(); ref.ng = ray->direction();
                    stable += beta * Toolkit::spectrum()->linear_srgb(lights->evaluate_hit_wi(ref, it, swl).L, swl) *
                        env.integrator()->correct_bsdf_weight(Float{1.f}, depth - 1u);
                };
                $if(!it.has_material()) {
                    ray = it.spawn_ray(ray->direction()); depth -= 1u; $continue;
                };
                Toolkit::scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                    auto bsdf = material->create_evaluator(it, swl);
                    auto lobes = bsdf.eval_stable_lobes(it.wo);
                    has_remainder = lobes.has_stochastic_remainder;
                    shading_normal = bsdf.shading_frame().normal();
                    if (storage.denoiser.enabled()) {
                        SampledSpectrum d{swl.dimension()}, s{swl.dimension()};
                        bsdf.denoiser_factors(it.wo, d, s);
                        guide_diffuse = max(Toolkit::spectrum()->linear_srgb(d, swl), make_float3(.04f));
                        guide_specular = max(Toolkit::spectrum()->linear_srgb(s, swl), make_float3(.04f));
                        Float2 r;
                        bsdf.reuse_material(d, s, r); roughness = r.y;
                    }
                    Bool t = lobes.lobes[0].valid, r = lobes.lobes[1].valid;
                    Bool next_ideal_path = ideal_path && BxDFFlag::is_specular(bsdf.flag());
                    // Only complete delta distributions are enumerated. Mixed
                    // lobes remain whole and are sampled by ordinary PT.
                    $if(Bool{material->enable_delta()} && !has_remainder && depth < limit) {
                        $if(t && r && plane == 0u && header.y == PrefixInvalid && header.z == PrefixInvalid) {
                            for (uint k = 0u; k < 2u; ++k) {
                                auto &lobe = lobes.lobes[k];
                                auto next_matrix = transform_frame(matrix, bsdf.shading_frame().x,
                                    bsdf.shading_frame().y, bsdf.shading_frame().z,
                                    lobe.wi, ray->direction(), lobe.transmission);
                                queue_pt_plane(storage, Uint{k + 1u}, it.spawn_ray(lobe.wi),
                                    beta * lobe.weight.vec3(), depth, travelled, (branch << 2u) | (k + 1u),
                                    Bool{k == 0u}, next_ideal_path, eta_scale * sqr(lobe.eta), next_matrix);
                                header[k + 1u] = PrefixQueued;
                            }
                            consumed = true;
                        } $else {
                            // Native stable-plane pruning keeps the first live
                            // delta entry when reusing an occupied slot: transmit
                            // through the glass exit, or reflect at TIR. This is
                            // guide discovery; Fill still samples the full BSDF.
                            $if(t || r) {
                                Uint selected = gi_prefix_reuse_lobe(t, r);
                                for (uint k = 0u; k < 2u; ++k) {
                                    auto &lobe = lobes.lobes[k];
                                    $if(selected == k) {
                                        matrix = transform_frame(matrix, bsdf.shading_frame().x,
                                            bsdf.shading_frame().y, bsdf.shading_frame().z,
                                            lobe.wi, ray->direction(), lobe.transmission);
                                        beta *= lobe.weight.vec3(); eta_scale *= sqr(lobe.eta);
                                        ideal_path = next_ideal_path;
                                        ray = it.spawn_ray(lobe.wi); branch = (branch << 2u) | (k + 1u);
                                        continued = true;
                                    };
                                }
                            };
                        };
                    };
                });
            };
            $if(depth == 1u) { header.w = (as<uint>(travelled) & 0xfffffffcu) | (header.w & 3u); };
            $if(continued) { $continue; };
            ReSTIRGIPrefixPlaneVar node;
            node.hit_words = pack_hit(hit);
            node.origin_distance = make_float4(ray->origin(), travelled);
            node.direction_vertex = make_float4(ray->direction(), cast<float>(depth));
            node.throughput_flags = make_float4(beta, select(consumed, 1.f, 0.f));
            node.interiors_branch = make_uint4(as<uint>(eta_scale), cast<uint>(ideal_path), branch, 0u);
            Float first = as<float>(header.w & 0xfffffffcu);
            Float3 pivot = camera->origin() + camera->direction() * first;
            Float3 virtual_point = virtual_position(matrix, point, pivot);
            Float view_depth = near_depth + dot(camera->direction() * travelled, Toolkit::scene().sensor()->device_forward());
            node.virtual_position = make_float4(virtual_point, view_depth);
            pt_prefix_store_controls(node, depth, consumed, ideal_path);
            header[plane] = branch;
            $if(gi_prefix_receiver_eligible(hit->is_hit(), consumed, has_remainder) && (dominant_plane == 3u || dominant)) {
                dominant_plane = plane;
            };
            if (storage.denoiser.enabled()) {
                // Plane0 still filters residual camera-path radiance when its
                // ideal split was consumed. Consumption excludes a GI receiver,
                // while the physical-primary denoiser guide remains valid.
                $if(gi_prefix_guide_eligible(hit->is_hit(), consumed, plane)) {
                    reblur::GuideVar guide;
                    auto &sensor = Toolkit::scene().sensor();
                    Float3 n = normalize(make_float3(dot(matrix[0].xyz(), shading_normal),
                        dot(matrix[1].xyz(), shading_normal), dot(matrix[2].xyz(), shading_normal)));
                    Float current_z = sensor->linear_depth(virtual_point), previous_z = sensor->prev_linear_depth(virtual_point);
                    Float2 motion = sensor->raster_coord(virtual_point).xy() - sensor->prev_raster_coord(virtual_point).xy();
                    guide.position = make_float4(virtual_point, view_depth);
                    guide.identity = make_uint4(hit.inst_id, it.material_id(), branch, native_reblur_motion_flags(Float{previous_z - current_z}));
                    guide.normal_roughness = make_float4(n, roughness);
                    guide.diffuse_factor = make_float4(guide_diffuse, native_reblur_half_signed(Float{motion.x}));
                    guide.specular_factor = make_float4(guide_specular, native_reblur_half_signed(Float{motion.y}));
                    pt_prefix_store_guide(node, guide);
                    storage.denoiser.write_motion_y(dispatch_id(), plane, Float{guide.specular_factor.w});
                };
            }
            storage.write(plane, node);
            $break;
        };
    };
    header.w = (header.w & 0xfffffffcu) | dominant_plane;
    storage.headers.write(dispatch_id(), header);
    storage.stable_radiance.write(dispatch_id(), make_float4(zero_if_nan_inf(stable), 0.f));
    if (storage.denoiser.enabled()) storage.denoiser.bypass->write(dispatch_id(), make_float4(zero_if_nan_inf(stable), 0.f));
}

}
