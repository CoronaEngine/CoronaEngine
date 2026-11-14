#include "rtxpt_gi_pt.h"
#include "rtxpt_gi_prefix.h"
#include "indirect.h"
#include "base/integral/integrator.h"

namespace vision::rtxpt_gi {
namespace {
using namespace ocarina;

void queue_pt_plane(const PrefixStorage &storage, const Uint &plane, const RayVar &ray,
                    const Float3 &beta, const Uint &depth, const Float &distance,
                    const Uint &branch, const Bool &dominant, const Float &eta_scale,
                    const Float4x4 &matrix) {
    RtxptGIPrefixPlaneVar node;
    node.hit_words = as<uint4>(make_float4(matrix[0].xyz(), matrix[1].x));
    node.virtual_position = make_float4(matrix[1].yz(), matrix[2].xy());
    node.origin_distance = make_float4(ray->origin(), distance);
    node.direction_vertex = make_float4(ray->direction(), cast<float>(depth));
    node.throughput_flags = make_float4(beta, matrix[2].z);
    node.interiors_branch = make_uint4(as<uint>(eta_scale), 0u, branch,
                                      select(dominant, PrefixDominantBit, 0u));
    storage.write(plane, node);
}


}

void build_pt_prefix(const PrefixStorage &storage, ReSTIRGI &env) noexcept {
    using namespace prefix_detail;
    const auto &geometry = Toolkit::geometry();
    const auto &swl = env.sampled_wavelengths();
    auto &lights = Toolkit::renderer().light_sampler();
    if (storage.denoiser.enabled()) {
        for (uint p = 0u; p < 3u; ++p) {
            reblur::GuideVar empty;
            empty.position = make_float4(0.f); empty.identity = make_uint4(0u);
            empty.normal_roughness = make_float4(0.f, 0.f, 1.f, 1.f);
            storage.denoiser.write_guide(dispatch_id(), Uint{p}, empty);
        }
    }
    RayVar camera = Toolkit::frame_buffer().rays().read(dispatch_id())->to_ray();
    Float near_depth = camera->t_min() * dot(camera->direction(), Toolkit::scene().sensor()->device_forward());
    camera->update_origin(camera->origin() + camera->direction() * camera->t_min());
    camera.org_min.w = 0.f;
    Uint4 header = make_uint4(PrefixQueued, PrefixInvalid, PrefixInvalid, 3u);
    Uint dominant_plane = 3u;
    Float3 stable = make_float3(0.f);
    queue_pt_plane(storage, Uint{0u}, camera, Float3{make_float3(1.f)}, Uint{0u}, Float{0.f},
                   Uint{1u}, Bool{true}, Float{1.f}, identity());
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
            // Falcor owns physical-primary emission/NEE and the emitter reached
            // by the first physical BSDF ray. Deeper delta branches belong to GI.
            Bool direct_owned = depth == 1u || (depth == 2u && (branch & 3u) == 2u);
            $if(hit->is_miss()) {
                $if(!direct_owned) {
                    LightSampleContext ref; ref.pos = ray->origin(); ref.ng = ray->direction();
                    stable += beta * Toolkit::spectrum()->linear_srgb(lights->evaluate_miss_wi(ref, ray->direction(), swl, LightEvalMode::L).L, swl);
                };
                travelled += Toolkit::scene().world_diameter();
            } $else {
                it = geometry.compute_surface_interaction(hit, ray);
                point = it.pos;
                travelled += length(it.pos - ray->origin());
                $if(it.has_emission() && !direct_owned) {
                    LightSampleContext ref; ref.pos = ray->origin(); ref.ng = ray->direction();
                    stable += beta * Toolkit::spectrum()->linear_srgb(lights->evaluate_hit_wi(ref, it, swl).L, swl);
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
                                    Bool{k == 0u}, eta_scale * sqr(lobe.eta), next_matrix);
                                header[k + 1u] = PrefixQueued;
                            }
                            consumed = true;
                        } $else {
                            // Once all three slots are occupied a split remains
                            // stochastic; never discard its other Fresnel branch.
                            $if(t != r) {
                                for (uint k = 0u; k < 2u; ++k) {
                                    auto &lobe = lobes.lobes[k];
                                    $if(lobe.valid) {
                                        matrix = transform_frame(matrix, bsdf.shading_frame().x,
                                            bsdf.shading_frame().y, bsdf.shading_frame().z,
                                            lobe.wi, ray->direction(), lobe.transmission);
                                        beta *= lobe.weight.vec3(); eta_scale *= sqr(lobe.eta);
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
            RtxptGIPrefixPlaneVar node;
            node.hit_words = pack_hit(hit);
            node.origin_distance = make_float4(ray->origin(), travelled);
            node.direction_vertex = make_float4(ray->direction(), cast<float>(depth));
            node.throughput_flags = make_float4(beta, select(consumed, 1.f, 0.f));
            node.interiors_branch = make_uint4(as<uint>(eta_scale), 0u, branch, 0u);
            Float first = as<float>(header.w & 0xfffffffcu);
            Float3 pivot = camera->origin() + camera->direction() * first;
            Float3 virtual_point = virtual_position(matrix, point, pivot);
            Float view_depth = near_depth + dot(camera->direction() * travelled, Toolkit::scene().sensor()->device_forward());
            node.virtual_position = make_float4(virtual_point, view_depth);
            storage.write(plane, node); header[plane] = branch;
            $if(!consumed && hit->is_hit() && has_remainder && (dominant_plane == 3u || dominant)) {
                dominant_plane = plane;
            };
            if (storage.denoiser.enabled()) {
                $if(hit->is_hit() && !consumed) {
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
                    storage.denoiser.write_guide(dispatch_id(), plane, guide);
                };
            }
            $break;
        };
    };
    header.w = (header.w & 0xfffffffcu) | dominant_plane;
    storage.headers.write(dispatch_id(), header);
    storage.stable_radiance.write(dispatch_id(), make_float4(zero_if_nan_inf(stable), 0.f));
    if (storage.denoiser.enabled()) storage.denoiser.bypass->write(dispatch_id(), make_float4(zero_if_nan_inf(stable), 0.f));
}

}
