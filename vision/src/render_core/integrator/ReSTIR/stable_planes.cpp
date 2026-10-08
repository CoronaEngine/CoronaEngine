#include "stable_planes.h"
#include "base/integral/integrator.h"

namespace vision {
using namespace ocarina;
namespace {
struct BuildPath {
    RayState ray;
    Float3 weight{make_float3(1.f)};
    Float3 x{make_float3(1, 0, 0)}, y{make_float3(0, 1, 0)}, z{make_float3(0, 0, 1)};
    Float3 offset{make_float3(0.f)}, anchor{make_float3(0.f)};
    Float distance{0.f};
    Uint sequence{1u}, identity{0u}, depth{0u};
    Bool active{false}, planar{true};
    [[nodiscard]] Float3 vector(const Float3 &v) const { return x * v.x + y * v.y + z * v.z; }
};

[[nodiscard]] Uint advance_identity(const Uint &identity, const Uint &instance, const Uint &branch) {
    return ((identity ^ (instance + 1u)) * 16777619u) ^ (branch + 1u);
}

[[nodiscard]] Bool on_prefix(const Uint &sequence, const Uint &depth, const StablePlaneDataVar &plane) {
    Uint shift = min(plane.depth - min(depth, plane.depth), 15u) * 2u;
    return plane.valid != 0u && depth <= plane.depth && sequence == (plane.branch_sequence >> shift);
}

[[nodiscard]] Float3 rotate_from_to(const Float3 &v, const Float3 &from, const Float3 &to) {
    Float3 axis = cross(from, to);
    Float cosine = clamp(dot(from, to), -1.f, 1.f);
    // Refraction has a non-opposite propagation direction. The branch is also
    // well-defined at normal incidence, where axis is exactly zero.
    return v + cross(axis, v) + cross(axis, cross(axis, v)) / max(1.f + cosine, 1e-6f);
}

void scatter_path(BuildPath &path, const Interaction &it, const StableLobe &lobe,
                  uint branch, const Bool &exact_planar) {
    Float3 old_direction = path.ray.direction();
    Float3 n = normalize(lobe.wi - old_direction);
    Float3 local_x = make_float3(1, 0, 0), local_y = make_float3(0, 1, 0), local_z = make_float3(0, 0, 1);
    $if(lobe.transmission) {
        local_x = rotate_from_to(local_x, lobe.wi, old_direction);
        local_y = rotate_from_to(local_y, lobe.wi, old_direction);
        local_z = rotate_from_to(local_z, lobe.wi, old_direction);
    }
    $else {
        local_x -= 2.f * n.x * n;
        local_y -= 2.f * n.y * n;
        local_z -= 2.f * n.z * n;
    };
    $if(path.planar && exact_planar && !lobe.transmission) {
        path.offset += 2.f * dot(n, it.pos) * path.vector(n);
    }
    $else { path.planar = false; };
    Float3 x = path.vector(local_x), y = path.vector(local_y), z = path.vector(local_z);
    path.x = x; path.y = y; path.z = z;
    path.weight *= lobe.weight.vec3();
    path.sequence = (path.sequence << 2u) | (branch + 1u);
    path.identity = advance_identity(path.identity, it.inst_id, Uint{branch});
    $if(path.planar) {
        Int3 plane_n = make_int3(round(it.ng * 4096.f));
        Int plane_d = cast<int>(round(dot(it.ng, it.pos) * 4096.f));
        path.identity = (path.identity * 16777619u) ^ cast<uint>(plane_n.x);
        path.identity = (path.identity * 16777619u) ^ cast<uint>(plane_n.y);
        path.identity = (path.identity * 16777619u) ^ cast<uint>(plane_n.z);
        path.identity = (path.identity * 16777619u) ^ cast<uint>(plane_d);
    };
    path.depth += 1u;
    path.ray = it.spawn_ray_state(lobe.wi);
}
}// namespace

void StablePlanes::prepare() noexcept {
    auto &fb = frame_buffer();
    fb.prepare_stable_planes(); fb.prepare_stable_direct(); fb.prepare_stable_indirect();
    fb.prepare_stable_radiance(); fb.prepare_stable_dominant();
}

void StablePlanes::compile() noexcept {
    auto &fb = frame_buffer();
    auto &camera = scene().sensor();
    auto &sampler = renderer().sampler();
    auto &spectrum = renderer().spectrum();
    auto &lights = renderer().light_sampler();
    const auto &geometry = pipeline()->geometry();
    const uint exploration_limit = std::min({15u, max_recursion_ > 0u ? max_recursion_ - 1u : 0u, integrator()->suffix_depth()});
    Kernel build_kernel = [&](Uint frame, Uint jitter) {
        camera->load_data(); sampler->load_data();
        initial(sampler, frame, spectrum);
        sampler->set_seed(dispatch_idx().xy(), frame, Dimension::Camera);
        SensorSample ss = sampler->sensor_sample(dispatch_idx().xy(), camera->filter(), jitter != 0u);
        sampler->set_seed(dispatch_idx().xy(), frame, Dimension::ReSTIR_RIS);
        auto planes = fb.cur_stable_planes_var(frame);
        RayState camera_ray = fb.rays().read(dispatch_id())->to_ray_state();
        std::array<BuildPath, StablePlaneCount> paths;
        paths[0].ray = camera_ray; paths[0].active = true;
        Float3 stable_radiance = make_float3(0.f);
        Uint dominant = InvalidUI32;
        for (uint layer = 0; layer < StablePlaneCount; ++layer) {
            BuildPath &path = paths[layer];
            StablePlaneDataVar plane;
            fb.stable_direct().write(layer * (dispatch_dim().x * dispatch_dim().y) + dispatch_id(), make_float4(0.f));
            fb.stable_indirect().write(layer * (dispatch_dim().x * dispatch_dim().y) + dispatch_id(), make_float4(0.f));
            $if(path.active) {
                $loop {
                    TriangleHitVar hit = geometry.trace_closest(path.ray.ray);
                    plane.surface.hit = hit;
                    plane.valid = 1u;
                    plane.depth = path.depth; plane.branch_sequence = path.sequence; plane.instance_hash = path.identity;
                    plane.extension.view_pos = path.ray.origin();
                    plane.extension.final_direction = path.ray.direction();
                    plane.extension.throughput = path.weight;
                    plane.extension.prefix_depth = path.depth;
                    plane.surface.is_replaced = path.depth > 0u;
                    plane.surface.stable_branch = select(path.depth == 0u, 0u, (path.identity % (InvalidUI32 - 1u)) + 1u);
                    $if(hit->is_miss()) {
                        if (lights->env_light()) {
                            LightSampleContext ref;
                            ref.pos = path.ray.origin(); ref.ng = path.ray.direction();
                            auto eval = lights->evaluate_miss_wi(ref, path.ray.direction(), sampled_wavelengths(), LightEvalMode::L);
                            stable_radiance += spectrum->linear_srgb(eval.L * SampledSpectrum(path.weight), sampled_wavelengths());
                        }
                        $break;
                    };
                    Interaction it = geometry.compute_surface_interaction(hit, path.ray.ray, true);
                    path.distance += path.ray.ray->t_max();
                    $if(path.depth == 0u) { path.anchor = it.pos; };
                    plane.extension.t_max = path.distance;
                    plane.depth_position = camera_ray.ray->at(path.distance);
                    plane.surface.depth_position = plane.depth_position;
                    plane.surface.approximate = !path.planar;
                    plane.material_id = it.material_id();
                    $if(it.has_emission()) {
                        LightSampleContext ref;
                        ref.pos = path.ray.origin(); ref.ng = path.ray.direction();
                        auto eval = lights->evaluate_hit_wi(ref, it, sampled_wavelengths());
                        stable_radiance += spectrum->linear_srgb(eval.L * SampledSpectrum(path.weight), sampled_wavelengths());
                    };
                    Bool continued = false;
                    scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                        Interaction guide_it = it;
                        auto bsdf = material->create_evaluator(it, sampled_wavelengths());
                        StableLobes lobes = bsdf.eval_stable_lobes(it.wo);
                        Bool exact_planar = bsdf.supports_stable_reflection(it.wo) &&
                            dot(bsdf.shading_frame().normal(), it.ng) > 0.99999f &&
                            dot(material->shading_normal(it, sampled_wavelengths()), it.ng) > 0.99999f;
                        Bool supported = Bool(material->enable_delta()) && !lobes.has_stochastic_remainder &&
                            (!bsdf.supports_stable_reflection(it.wo) || exact_planar);
                        plane.surface.flag = bsdf.flag();
                        plane.surface->set_position(it.pos);
                        plane.surface->set_normal(bsdf.shading_frame().normal());
                        plane.surface->set_diffuse_factor(bsdf.diffuse_factor());
                        plane.surface.virtual_position = select(path.planar, path.vector(it.pos) + path.offset,
                            path.vector(it.pos - path.anchor) + path.anchor);
                        plane.surface.virtual_normal = path.vector(bsdf.shading_frame().normal());
                        plane.surface.virtual_geometric_normal = path.vector(it.ng);
                        plane.surface->set_depth(camera->linear_depth(select(path.planar,
                            plane.surface.virtual_position, plane.depth_position)));
                        plane.motion = fb.compute_motion_vec(camera, ss.p_film, plane.surface.virtual_position, true);
                        $if(!path.planar) {
                            plane.motion = camera->raster_coord(plane.surface.virtual_position).xy() -
                                camera->prev_raster_coord(plane.surface.virtual_position).xy();
                        };
                        guide_it.wo = guide_it.ng;
                        SampledWavelengths guide_swl{spectrum->dimension()};
                        sampler->temporary([&](Sampler *guide_sampler) {
                            guide_sampler->set_seed(make_uint2(0u), 0u, Dimension::Camera);
                            guide_swl = spectrum->sample_wavelength(sampler);
                        });
                        auto guide = material->create_evaluator(guide_it, guide_swl);
                        SampledSpectrum diffuse{guide_swl.dimension()}, specular{guide_swl.dimension()};
                        Float2 roughness;
                        guide.reuse_material(diffuse, specular, roughness);
                        plane.surface.diffuse_roughness = make_float4(spectrum->linear_srgb(diffuse, guide_swl), roughness.x);
                        plane.surface.specular_roughness = make_float4(spectrum->linear_srgb(specular, guide_swl), roughness.y);
                        Float3 albedo_wo = select(dot(bsdf.shading_frame().normal(), it.wo) < 0.f, -it.wo, it.wo);
                        plane.surface.denoiser_albedo = spectrum->linear_srgb(bsdf.albedo(albedo_wo) * SampledSpectrum(path.weight), sampled_wavelengths());
                        plane.reservoir_eligible = !plane.surface->near_specular() && !it.has_emission();
                        $if(supported && path.depth < exploration_limit) {
                            if (layer == 0u) {
                                $if(lobes.lobes[0].valid && lobes.lobes[1].valid) {
                                    for (uint branch = 0; branch < 2; ++branch) {
                                        paths[branch + 1] = path;
                                        scatter_path(paths[branch + 1], it, lobes.lobes[branch], branch, exact_planar);
                                        paths[branch + 1].active = true;
                                    }
                                }
                                $else {
                                    for (uint branch = 0; branch < 2; ++branch) {
                                        $if(lobes.lobes[branch].valid) {
                                            scatter_path(path, it, lobes.lobes[branch], branch, exact_planar);
                                            continued = true;
                                        };
                                    }
                                };
                            } else {
                                $if(lobes.lobes[0].valid) {
                                    scatter_path(path, it, lobes.lobes[0], 0u, exact_planar); continued = true;
                                }
                                $else {
                                    $if(lobes.lobes[1].valid) {
                                        scatter_path(path, it, lobes.lobes[1], 1u, exact_planar); continued = true;
                                    };
                                };
                            }
                        };
                    });
                    $if(!continued) { $break; };
                };
            };
            planes.write(layer * (dispatch_dim().x * dispatch_dim().y) + dispatch_id(), plane);
            // The transmission child wins, followed by reflection and base.
            $if(plane.valid != 0u && plane.reservoir_eligible != 0u && (dominant == InvalidUI32 || Uint{layer} == 1u)) {
                dominant = layer;
            };
        }
        fb.stable_radiance().write(dispatch_id(), make_float4(stable_radiance, 1.f));
        fb.stable_dominant().write(dispatch_id(), dominant);
        SurfaceDataVar exported;
        SurfaceExtendVar extension;
        $if(dominant != InvalidUI32) {
            auto plane = planes.read(dominant * (dispatch_dim().x * dispatch_dim().y) + dispatch_id());
            exported = plane.surface; extension = plane.extension;
            fb.motion_vectors().write(dispatch_id(), plane.motion);
        };
        fb.cur_surfaces_var(frame).write(dispatch_id(), exported);
        fb.cur_surface_exts_var(frame).write(dispatch_id(), extension);
    };
    build_ = device().compile(build_kernel, "Stable planes build");

    Kernel fill_kernel = [&](Uint frame) {
        camera->load_data(); sampler->load_data(); integrator()->load_data();
        initial(sampler, frame, spectrum);
        sampler->set_seed(dispatch_idx().xy(), frame, Dimension::PathTracing);
        auto planes = fb.cur_stable_planes_var(frame);
        Uint dominant = fb.stable_dominant().read(dispatch_id());
        RayState ray = fb.rays().read(dispatch_id())->to_ray_state();
        SampledSpectrum throughput = spectrum->one();
        Uint sequence = 1u, depth = 0u, owner = 0u;
        const uint total_depth = integrator()->suffix_depth() + 1u;
        $loop {
            Bool covered = false, claimed = false;
            for (uint layer = 0; layer < StablePlaneCount; ++layer) {
                auto plane = planes.read(layer * (dispatch_dim().x * dispatch_dim().y) + dispatch_id());
                Bool prefix = on_prefix(sequence, depth, plane);
                covered |= prefix;
                $if(prefix && depth == plane.depth) {
                    owner = layer;
                    claimed = dominant == layer;
                };
            }
            $if(claimed) { $break; };
            TriangleHitVar hit = geometry.trace_closest(ray.ray);
            Float3 emission = make_float3(0.f);
            $if(hit->is_miss()) {
                $if(!covered) {
                    if (lights->env_light()) {
                        LightSampleContext ref; ref.pos = ray.origin(); ref.ng = ray.direction();
                        auto eval = lights->evaluate_miss_wi(ref, ray.direction(), sampled_wavelengths(), LightEvalMode::L);
                        emission = spectrum->linear_srgb(eval.L * throughput, sampled_wavelengths());
                    }
                };
                Uint address = owner * (dispatch_dim().x * dispatch_dim().y) + dispatch_id();
                auto old = fb.stable_direct().read(address);
                fb.stable_direct().write(address, old + make_float4(emission, 0.f));
                $break;
            };
            Interaction it = geometry.compute_surface_interaction(hit, ray.ray);
            $if(depth >= total_depth) {
                $if(it.has_emission() && !covered) {
                    LightSampleContext ref; ref.pos = ray.origin(); ref.ng = ray.direction();
                    auto eval = lights->evaluate_hit_wi(ref, it, sampled_wavelengths());
                    emission = spectrum->linear_srgb(eval.L * throughput, sampled_wavelengths());
                };
                Uint address = owner * (dispatch_dim().x * dispatch_dim().y) + dispatch_id();
                fb.stable_direct().write(address, fb.stable_direct().read(address) + make_float4(emission, 0.f));
                $break;
            };
            Bool near_specular = false;
            BSDFSample sample{sampled_wavelengths()};
            scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                auto bsdf = material->create_evaluator(it, sampled_wavelengths());
                near_specular = Bool(material->enable_delta()) && bsdf.flag() == SurfaceData::NearSpec && depth < std::max(1u, max_recursion_) - 1u;
                $if(near_specular) { sample = bsdf.sample_delta(it.wo, sampler); };
            });
            $if(near_specular) {
                $if(it.has_emission() && !covered) {
                    LightSampleContext ref; ref.pos = ray.origin(); ref.ng = ray.direction();
                    auto eval = lights->evaluate_hit_wi(ref, it, sampled_wavelengths());
                    emission = spectrum->linear_srgb(eval.L * throughput, sampled_wavelengths());
                };
                Uint address = owner * (dispatch_dim().x * dispatch_dim().y) + dispatch_id();
                fb.stable_direct().write(address, fb.stable_direct().read(address) + make_float4(emission, 0.f));
                $if(!sample.valid()) { $break; };
                throughput *= sample.eval.throughput();
                sequence = select(sample.stable_lobe_index < 2u && depth < 15u,
                    (sequence << 2u) | (sample.stable_lobe_index + 1u), InvalidUI32);
                depth += 1u;
                ray = it.spawn_ray_state(sample.wi);
            }
            $else {
                Float3 direct = make_float3(0.f);
                HitContext context{direct};
                context.restir_direct_split = true;
                context.suppress_initial_emission_if = &covered;
                context.complete_terminal_direct = true;
                // compute_surface_interaction clips t_max to the hit distance.
                // Li owns its traversal and must see that receiver again.
                ray.ray.dir_max.w = ray_t_max;
                Float3 total = integrator()->Li(ray, Float{1e16f}, total_depth - depth, throughput,
                    total_depth - 1u < 2u, context, *this);
                Uint address = owner * (dispatch_dim().x * dispatch_dim().y) + dispatch_id();
                fb.stable_direct().write(address, fb.stable_direct().read(address) + make_float4(direct, 0.f));
                fb.stable_indirect().write(address, make_float4(total - direct, 1.f));
                $break;
            };
        };
    };
    fill_ = device().compile(fill_kernel, "Stable planes residual fill");
}

CommandBatch StablePlanes::build(uint frame, bool jitter) const noexcept {
    CommandBatch ret; ret << build_(frame, uint(jitter)).dispatch(pipeline()->resolution()); return ret;
}
CommandBatch StablePlanes::fill(uint frame) const noexcept {
    CommandBatch ret; ret << fill_(frame).dispatch(pipeline()->resolution()); return ret;
}
}// namespace vision

VS_REGISTER_HOTFIX(vision, StablePlanes)
