#include "stable_planes.h"
#include "base/integral/integrator.h"

namespace vision {
using namespace ocarina;
namespace {
// Experimental lossless full-record transpose. Type reflection follows the
// storage emitter's offsets and traverses defined leaves only, never padding.
constexpr uint CooperativeSPWidth = 64u;
constexpr uint CooperativeSPStride = CooperativeSPWidth + 1u;
// The compact record has sixty defined words and no padding.
constexpr uint CooperativeSPRecordWords = 60u;
using CooperativeSPWords = SharedArray<uint, CooperativeSPStride * CooperativeSPRecordWords>;

// This Vulkan compute kernel shares only its LDS tile and owner flags.
void synchronize_stable_plane_shared() noexcept {
    auto *function = Function::current();
    const Expression *shared_only = function->literal(Type::of<bool>(), true);
    function->expr_statement(function->call_builtin(nullptr, CallOp::SYNCHRONIZE_BLOCK, {shared_only}));
}

void stage_stable_plane_words(const Expression *value, const Type *type,
                              size_t offset, CooperativeSPWords &words,
                              const Uint &lane, std::array<bool, CooperativeSPRecordWords> &occupied) {
    if (type->is_structure()) {
        size_t member_offset = 0u;
        for (uint member = 0u; member < type->members().size(); ++member) {
            const Type *member_type = type->members()[member];
            member_offset = mem_offset(member_offset, member_type->alignment());
            stage_stable_plane_words(Function::current()->member(member_type, value, int(member)),
                                     member_type, offset + member_offset, words, lane, occupied);
            member_offset += member_type->size();
        }
    } else if (type->is_array() || type->is_vector()) {
        for (uint component = 0u; component < type->dimension(); ++component) {
            Uint index = component;
            stage_stable_plane_words(Function::current()->subscript(type->element(), value, index.expression()),
                                     type->element(), offset + component * type->element()->size(), words, lane, occupied);
        }
    } else {
        OC_ERROR_IF(type->size() != sizeof(uint) || offset % sizeof(uint) != 0u ||
                        offset >= sizeof(StablePlaneData),
                    "SP cooperative store requires aligned 32-bit fields");
        uint word = uint(offset / sizeof(uint));
        OC_ERROR_IF(occupied[word], "SP cooperative store reflected overlapping fields");
        occupied[word] = true;
        Uint index = word * CooperativeSPStride + lane;
        if (type->tag() == Type::Tag::FLOAT) {
            words.write(index, as<uint>(eval<float>(value)));
        } else {
            OC_ERROR_IF(type->tag() != Type::Tag::UINT,
                        "SP cooperative store only accepts FP32/uint fields");
            words.write(index, eval<uint>(value));
        }
    }
}

static_assert(sizeof(StablePlaneData) == CooperativeSPRecordWords * sizeof(uint));
static_assert((CooperativeSPStride * CooperativeSPRecordWords + CooperativeSPWidth) * sizeof(uint) <= 16u * 1024u);
static_assert(offsetof(StablePlaneData, extension) + sizeof(SurfaceExtend) == 204u);
static_assert(offsetof(StablePlaneData, motion_storage) == 204u);
static_assert(offsetof(StablePlaneData, flags) + sizeof(uint) == 240u);

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
    return plane->valid() != 0u && depth <= plane.depth && sequence == (plane.branch_sequence >> shift);
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

void StablePlanes::restore(RuntimeObject *old_obj) noexcept {
    // ReSTIR can reload without replacing the FrameBuffer object.
    frame_buffer().require_stable_plane_storage_abi();
    ReSTIR::restore(old_obj);
    VS_HOTFIX_MOVE_ATTRS(max_recursion_, clear_children_, build_, fill_, save_history_)
}

void StablePlanes::prepare(bool retain_history) noexcept {
    auto &fb = frame_buffer();
    fb.manage_stable_plane_buffers(integrator()->stable_planes_enabled(), retain_history);
}

void StablePlanes::compile() noexcept {
    Kernel save_history = [](BufferVar<StablePlaneData> current, BufferVar<StablePlaneHistory> previous,
                             BufferVar<uint> history_words) {
        Uint idx = dispatch_id();
        $if(current.read(idx)->valid() != 0u) {
            previous.write(idx, current.read(idx)->history());
        } $else {
            // Invalid records are never consumed. Clearing the validity word
            // avoids gathering/writing all attributes of mostly empty layers.
            history_words.write(idx * uint(sizeof(StablePlaneHistory) / sizeof(uint)) +
                uint(offsetof(StablePlaneHistory, valid) / sizeof(uint)), 0u);
        };
    };
    save_history_ = device().compile(save_history, "StablePlanes-SaveHistory");
    auto &fb = frame_buffer();
    auto &camera = scene().sensor();
    auto &sampler = renderer().sampler();
    auto &spectrum = renderer().spectrum();
    auto &lights = renderer().light_sampler();
    const auto &geometry = pipeline()->geometry();
    const bool lobes = integrator()->denoiser() && integrator()->denoiser()->requires_lobe_signals();
    const uint recursion_limit = std::min(15u, max_recursion_ > 0u ? max_recursion_ - 1u : 0u);
    // FrameBuffer initializes every allocation to complete default records.
    // Only a previously active child can contain data after initialization.
    // Keep complete default records for invalid children without rewriting the
    // mostly empty layers every frame. History has already been saved here.
    Kernel clear_children = [](BufferVar<StablePlaneData> buffer) {
        $if(buffer.read(dispatch_id())->valid() != 0u) {
            buffer.write(dispatch_id(), StablePlaneDataVar{});
        };
    };
    clear_children_ = device().compile(clear_children, "Stable planes clear children");
    Kernel build_kernel = [&](Uint frame, Uint jitter) {
        // Logical tracing and native pipeline classification stay separate.
        // Vulkan executes this explicitly opted-in kernel as compute ray queries.
        Function::current()->require_ray_query_compute_workgroups();
        configure_block(CooperativeSPWidth, 1u, 1u);
        CooperativeSPWords cooperative_words;
        SharedArray<uint, CooperativeSPWidth> cooperative_active;
        Uint cooperative_lane = thread_idx().x;
        Bool cooperative_store = dispatch_dim().x % CooperativeSPWidth == 0u;
        camera->load_data(); sampler->load_data(); integrator()->load_data();
        Uint exploration_limit = min(recursion_limit, integrator()->runtime_suffix_depth());
        initial(sampler, frame, spectrum);
        sampler->set_seed(dispatch_idx().xy(), frame, Dimension::Camera);
        SensorSample ss = sampler->sensor_sample(dispatch_idx().xy(), camera->filter(), jitter != 0u);
        sampler->set_seed(dispatch_idx().xy(), frame, Dimension::ReSTIR_RIS);
        auto planes = fb.cur_stable_planes_var(frame);
        auto plane_words = fb.bindless_array().buffer_var<uint>(fb.stable_planes().index_var());
        RayState camera_ray = fb.rays().read(dispatch_id())->to_ray_state();
        std::array<BuildPath, StablePlaneCount> paths;
        paths[0].ray = camera_ray; paths[0].active = true;
        Float3 stable_radiance = make_float3(0.f);
        Uint dominant = InvalidUI32;
        SurfaceDataVar exported;
        SurfaceExtendVar extension;
        Float2 exported_motion = make_float2(0.f);
        for (uint layer = 0; layer < StablePlaneCount; ++layer) {
            BuildPath &path = paths[layer];
            StablePlaneDataVar plane;
            Float2 plane_motion = make_float2(0.f);
            fb.stable_direct().write(layer * (dispatch_dim().x * dispatch_dim().y) + dispatch_id(), make_float4(0.f));
            fb.stable_indirect().write(layer * (dispatch_dim().x * dispatch_dim().y) + dispatch_id(), make_float4(0.f));
            if (lobes) {
                Uint address = layer * (dispatch_dim().x * dispatch_dim().y) + dispatch_id();
                fb.denoiser_diffuse().write(address, make_float4(0.f));
                fb.denoiser_specular().write(address, make_float4(0.f));
                fb.denoiser_bypass().write(address, make_float4(0.f));
            }
            $if(path.active) {
                $loop {
                    TriangleHitVar hit;
                    if (layer == 0u) {
                        // GBuffer traced this exact owned camera ray (including
                        // lens and film samples). Only extended paths retrace.
                        $if(path.depth == 0u) {
                            hit = fb.cur_visibility_buffer_var(frame).read(dispatch_id());
                        } $else { hit = geometry.trace_closest(path.ray.ray); };
                    } else { hit = geometry.trace_closest(path.ray.ray); }
                    plane.surface.hit = hit;
                    plane->set_valid(1u);
                    plane.depth = path.depth; plane.branch_sequence = path.sequence; plane.instance_hash = path.identity;
                    plane.extension.view_pos.set(path.ray.origin());
                    plane.extension.final_direction.set(path.ray.direction());
                    plane.extension.throughput.set(path.weight);
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
                    plane.depth_position.set(camera_ray.ray->at(path.distance));
                    plane.surface.depth_position.set(plane.depth_position.as_vec3());
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
                        Bool exact_planar = bsdf.supports_stable_reflection(it.wo) &&
                            dot(bsdf.shading_frame().normal(), it.ng) > 0.99999f &&
                            dot(material->shading_normal(it, sampled_wavelengths()), it.ng) > 0.99999f;
                        plane.surface.flag = bsdf.flag();
                        plane.surface->set_position(it.pos);
                        plane.surface->set_normal(bsdf.shading_frame().normal());
                        plane.surface->set_diffuse_factor(bsdf.diffuse_factor());
                        plane.surface.virtual_position.set(select(path.planar, path.vector(it.pos) + path.offset,
                            path.vector(it.pos - path.anchor) + path.anchor));
                        plane.surface.virtual_normal.set(path.vector(bsdf.shading_frame().normal()));
                        plane.surface.virtual_geometric_normal.set(path.vector(it.ng));
                        plane.surface->set_depth(camera->linear_depth(select(path.planar,
                            plane.surface.virtual_position.as_vec3(), plane.depth_position.as_vec3())));
                        plane_motion = fb.compute_motion_vec(camera, ss.p_film, plane.surface.virtual_position.as_vec3(), true);
                        $if(!path.planar) {
                            plane_motion = camera->raster_coord(plane.surface.virtual_position.as_vec3()).xy() -
                                camera->prev_raster_coord(plane.surface.virtual_position.as_vec3()).xy();
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
                        plane.surface.denoiser_albedo.set(spectrum->linear_srgb(bsdf.albedo(albedo_wo) * SampledSpectrum(path.weight), sampled_wavelengths()));
                        plane->set_reservoir_eligible(cast<uint>(!plane.surface->near_specular() && !it.has_emission()));
                        StableLobes lobes = bsdf.eval_stable_lobes(it.wo);
                        Bool supported = Bool(material->enable_delta()) && !lobes.has_stochastic_remainder &&
                            (!bsdf.supports_stable_reflection(it.wo) || exact_planar);
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
                // Quantize the physical SP copy once; retain FP32 for the global export.
                plane->set_motion(plane_motion);
                $if(!cooperative_store) {
                    planes.write(layer * (dispatch_dim().x * dispatch_dim().y) + dispatch_id(), plane);
                };
            };
            // This branch is uniform for the whole dispatch. With a 64x1 block,
            // width divisible by64 means no invocation can take the generated
            // entry-point OOB return. Other widths execute no barriers.
            $if(cooperative_store) {
                cooperative_active.write(cooperative_lane, cast<uint>(path.active));
                $if(path.active) {
                    std::array<bool, CooperativeSPRecordWords> occupied{};
                    stage_stable_plane_words(plane.expression(), Type::of<StablePlaneData>(),
                                             0u, cooperative_words, cooperative_lane, occupied);
                    for (uint word = 0u; word < occupied.size(); ++word) {
                        OC_ERROR_IF(!occupied[word],
                                    "SP cooperative store must cover every compact record word");
                    }
                };
                synchronize_stable_plane_shared();
                // Query only after all64 owner flags are visible. A complete
                // logical warp32 samples all64 flags in pairs, independent of
                // native subgroup size or local-invocation mapping. Partial
                // logical warps use the exact same group-wide OR serially.
                Bool tile_active = true;
                if (layer != 0u) {
                    tile_active = false;
                    Bool complete_warp = warp_active_bit_mask(Bool{true}).x == InvalidUI32;
                    $if(complete_warp) {
                        Uint pair = warp_lane_id();
                        Uint pair_active = cooperative_active.read(pair) | cooperative_active.read(pair + 32u);
                        tile_active = warp_active_bit_mask(pair_active != 0u).x != 0u;
                    } $else {
                        $for(active_owner, CooperativeSPWidth) {
                            tile_active |= cooperative_active.read(active_owner) != 0u;
                        };
                    };
                }
                $if(tile_active) {
                    // Each wave writes consecutive words of one owner's record.
                    // Lanes60-63 are outside the compact record.
                    $if(cooperative_lane < CooperativeSPRecordWords) {
                        Uint group_first = dispatch_id() - cooperative_lane;
                        Uint layer_first = layer * (dispatch_dim().x * dispatch_dim().y);
                        $for(owner, CooperativeSPWidth) {
                            $if(cooperative_active.read(owner) != 0u) {
                                Uint address = (layer_first + group_first + owner) * CooperativeSPRecordWords + cooperative_lane;
                                Uint word = cooperative_words.read(cooperative_lane * CooperativeSPStride + owner);
                                plane_words.write(address, word);
                            };
                        };
                    };
                };
                // Inactive owners participate too. No next-layer staging may
                // overwrite the tile while another invocation still reads it.
                synchronize_stable_plane_shared();
            };
            // The transmission child wins, followed by reflection and base.
            $if(plane->valid() != 0u && plane->reservoir_eligible() != 0u && (dominant == InvalidUI32 || Uint{layer} == 1u)) {
                dominant = layer;
                exported = plane.surface;
                extension = plane.extension;
                exported_motion = plane_motion;
            };
        }
        fb.stable_radiance().write(dispatch_id(), make_float4(stable_radiance, 1.f));
        fb.stable_dominant().write(dispatch_id(), dominant);
        $if(dominant != InvalidUI32) {
            fb.motion_vectors().write(dispatch_id(), exported_motion);
        };
        fb.cur_surfaces_var(frame).write(dispatch_id(), exported);
        fb.cur_surface_exts_var(frame).write(dispatch_id(), extension);
    };
    build_ = device().compile(build_kernel, "Stable planes build");

    Kernel fill_kernel = [&](Uint frame, Float di, Float gi) {
        camera->load_data(); sampler->load_data(); integrator()->load_data();
        initial(sampler, frame, spectrum);
        sampler->set_seed(dispatch_idx().xy(), frame, Dimension::PathTracing);
        auto planes = fb.cur_stable_planes_var(frame);
        Uint dominant = fb.stable_dominant().read(dispatch_id());
        // A dominant primary receiver consumes the entire path.
        $if(dominant == 0u) {
            auto base = planes.read(dispatch_id());
            $if(base.depth == 0u) { $return(); };
        };
        RayState ray = fb.rays().read(dispatch_id())->to_ray_state();
        SampledSpectrum throughput = spectrum->one();
        Uint sequence = 1u, depth = 0u, owner = 0u;
        Uint total_depth = integrator()->runtime_suffix_depth() + 1u;
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
                if (lobes) fb.denoiser_bypass().write(address, fb.denoiser_bypass().read(address) + make_float4(emission * di, 0.f));
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
                if (lobes) fb.denoiser_bypass().write(address, fb.denoiser_bypass().read(address) + make_float4(emission * di, 0.f));
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
                if (lobes) fb.denoiser_bypass().write(address, fb.denoiser_bypass().read(address) + make_float4(emission * di, 0.f));
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
                Float3 specular = make_float3(0.f), direct_specular = make_float3(0.f), bypass = make_float3(0.f);
                Float2 distance = make_float2(0.f);
                if (lobes) {
                    context.Ld_specular = &specular; context.direct_specular = &direct_specular;
                    context.bypass = &bypass; context.lobe_hitdist = &distance;
                }
                context.restir_direct_split = true;
                context.suppress_initial_emission_if = &covered;
                context.complete_terminal_direct = true;
                // compute_surface_interaction clips t_max to the hit distance.
                // Li owns its traversal and must see that receiver again.
                ray.ray.dir_max.w = ray_t_max;
                Float3 total = integrator()->Li(ray, Float{1e16f}, total_depth - depth, throughput,
                    false, context, *this);
                Uint address = owner * (dispatch_dim().x * dispatch_dim().y) + dispatch_id();
                fb.stable_direct().write(address, fb.stable_direct().read(address) + make_float4(direct, 0.f));
                fb.stable_indirect().write(address, make_float4(total - direct, 1.f));
                if (lobes) {
                    Float3 weighted_specular = direct_specular * di + (specular - direct_specular) * gi;
                    Float3 weighted_total = direct * di + (total - direct) * gi;
                    fb.denoiser_diffuse().write(address, make_float4(weighted_total - weighted_specular - bypass * di, distance.x));
                    fb.denoiser_specular().write(address, make_float4(weighted_specular, distance.y));
                    fb.denoiser_bypass().write(address, fb.denoiser_bypass().read(address) + make_float4(bypass * di, 0.f));
                }
                $break;
            };
        };
    };
    fill_ = device().compile(fill_kernel, "Stable planes residual fill");
}

CommandBatch StablePlanes::build(uint frame, bool jitter, bool retain_history) const noexcept {
    auto planes = frame_buffer().cur_stable_planes_view(frame);
    uint2 resolution = pipeline()->resolution();
    uint pixels = resolution.x * resolution.y;
    BufferView<StablePlaneData> children{planes.handle() + pixels * sizeof(StablePlaneData),
        (StablePlaneCount - 1u) * pixels};
    CommandBatch ret;
    if (retain_history && frame > 0u) {
        auto history = frame_buffer().prev_stable_planes_view(frame);
        ret << save_history_(planes, history, history.view_as<uint>()).dispatch(planes.size());
    }
    ret << clear_children_(children).dispatch(children.size());
    ret << build_(frame, uint(jitter)).dispatch(resolution);
    return ret;
}
CommandBatch StablePlanes::fill(uint frame, float direct_factor, float indirect_factor) const noexcept {
    CommandBatch ret; ret << fill_(frame, direct_factor, indirect_factor).dispatch(pipeline()->resolution()); return ret;
}
}// namespace vision

VS_REGISTER_HOTFIX(vision, StablePlanes)
