#include "rtxpt_gi.h"
#include "indirect.h"
#include "denoiser_signals.h"
#include "base/integral/integrator.h"
#include "dsl/data/shared_array.h"

namespace vision::rtxpt_gi {
using namespace ocarina;

Uint encode_radiance(const Float3 &rgb) noexcept {
    Float3 xyz = make_float3(
        dot(rgb, make_float3(.4123907992659595f, .3575843393838780f, .1804807884018343f)),
        dot(rgb, make_float3(.2126390058715104f, .7151686787677559f, .0721923153607337f)),
        dot(rgb, make_float3(.0193308187155918f, .1191947797946259f, .9505321522496608f)));
    Uint packed = 0u;
    $if(xyz.y > 0.f) {
        Uint le = cast<uint>(clamp(409.6f * (log2(xyz.y) + 20.f), 0.f, 16383.f));
        $if(le != 0u) {
            Float inv = 1.f / (-2.f * xyz.x + 12.f * xyz.y + 3.f * (xyz.x + xyz.y + xyz.z));
            Uint u = cast<uint>(clamp(820.f * 4.f * xyz.x * inv, 0.f, 511.f));
            Uint v = cast<uint>(clamp(820.f * 9.f * xyz.y * inv, 0.f, 511.f));
            packed = (le << 18u) | (u << 9u) | v;
        };
    };
    return packed;
}

Float3 decode_radiance(const Uint &packed) noexcept {
    Float3 rgb = make_float3(0.f);
    Uint le = packed >> 18u;
    $if(le != 0u) {
        Float Y = exp2((cast<float>(le) + .5f) / 409.6f - 20.f);
        Float u = (cast<float>((packed >> 9u) & 511u) + .5f) / 820.f;
        Float v = (cast<float>(packed & 511u) + .5f) / 820.f;
        Float inv = 1.f / (6.f * u - 16.f * v + 12.f);
        Float x = 9.f * u * inv, y = 4.f * v * inv;
        Float s = Y / y;
        Float3 xyz = make_float3(s * x, Y, s * (1.f - x - y));
        rgb = max(make_float3(
            dot(xyz, make_float3(3.240969941904522f, -1.537383177570094f, -.4986107602930032f)),
            dot(xyz, make_float3(-.9692436362808803f, 1.875967501507721f, .04155505740717569f)),
            dot(xyz, make_float3(.05563007969699373f, -.2039769588889765f, 1.056971514242878f))), make_float3(0.f));
    };
    return rgb;
}

Uint encode_normal(const Float3 &normal) noexcept {
    Float denominator = abs(normal.x) + abs(normal.y) + abs(normal.z);
    Float x = 0.f, y = 0.f;
    $if(denominator > 0.f) {
        x = normal.x / denominator;
        y = normal.y / denominator;
        $if(normal.z < 0.f) {
            Float old_x = x;
            x = (1.f - abs(y)) * select(old_x >= 0.f, 1.f, -1.f);
            y = (1.f - abs(old_x)) * select(y >= 0.f, 1.f, -1.f);
        };
    };
    Int ix = cast<int>(round(clamp(x, -1.f, 1.f) * 32767.f));
    Int iy = cast<int>(round(clamp(y, -1.f, 1.f) * 32767.f));
    return (cast<uint>(ix) & 65535u) | (cast<uint>(iy) << 16u);
}

Float3 decode_normal(const Uint &packed) noexcept {
    Int ix = cast<int>(packed & 65535u), iy = cast<int>(packed >> 16u);
    ix = select(ix >= 32768, ix - 65536, ix);
    iy = select(iy >= 32768, iy - 65536, iy);
    Float x = max(cast<float>(ix) / 32767.f, -1.f);
    Float y = max(cast<float>(iy) / 32767.f, -1.f);
    Float z = 1.f - abs(x) - abs(y);
    $if(z < 0.f) {
        Float old_x = x;
        x = (1.f - abs(y)) * select(old_x >= 0.f, 1.f, -1.f);
        y = (1.f - abs(old_x)) * select(y >= 0.f, 1.f, -1.f);
    };
    return normalize(make_float3(x, y, z));
}

RtxptGIStorageVar pack(const RtxptGIReservoirVar &r) noexcept {
    RtxptGIStorageVar result;
    for (uint i = 0; i < 3u; ++i) result.words[i] = as<uint>(r.sample.sp.pos[i]);
    result.words[3] = min(r.M, 255u) | (min(r.age, 255u) << 8u);
    result.words[4] = encode_radiance(r.sample.Lo.as_vec3());
    result.words[5] = as<uint>(r.weight);
    result.words[6] = encode_normal(r.sample.sp->normal());
    result.words[7] = 0u;
    return result;
}

RtxptGIReservoirVar unpack(const RtxptGIStorageVar &stored) noexcept {
    RtxptGIReservoirVar r;
    for (uint i = 0; i < 3u; ++i) r.sample.sp.pos[i] = as<float>(stored.words[i]);
    r.M = stored.words[3] & 255u;
    r.age = (stored.words[3] >> 8u) & 255u;
    r.sample.Lo.set(decode_radiance(stored.words[4]));
    r.weight = as<float>(stored.words[5]);
    r.sample.sp->set_normal(decode_normal(stored.words[6]));
    return r;
}

Float jacobian(const Float3 &receiver, const Float3 &source,
               const Float3 &position, const Float3 &normal) noexcept {
    Float3 n = receiver - position, o = source - position;
    Float dn = length(n), od = length(o);
    Float cn = saturate(dot(normal, n / dn)), co = saturate(dot(normal, o / od));
    return zero_if_nan_inf((cn * od * od) / (co * dn * dn));
}

Bool validate_jacobian(Float &value) noexcept {
    Bool valid = value >= .1f && value <= 10.f;
    value = clamp(value, 1.f / 3.f, 3.f);
    return valid;
}

Bool combine(RtxptGIReservoirVar &out, const RtxptGIReservoirVar &source,
             const Float &random, const Float &target) noexcept {
    Float w = target * source.weight * cast<float>(source.M);
    out.M += source.M;
    out.weight += w;
    Bool selected = random * out.weight <= w;
    $if(selected) {
        out.sample = source.sample;
        out.age = source.age;
    };
    return selected;
}

void finalize(RtxptGIReservoirVar &r, const Float &numerator, const Float &denominator) noexcept {
    r.weight = select(denominator == 0.f, 0.f, r.weight * numerator / denominator);
}

Float3 depth_guide_position(const Float3 &physical, const Float3 &virtual_position,
                           const Float3 &depth_position, const Bool &use_virtual,
                           const Bool &approximate) noexcept {
    // Match SurfaceData::normal_depth and the existing DI guide selection.
    Float3 position = select(use_virtual, virtual_position, physical);
    return select(approximate, depth_position, position);
}

Float luminance_709(const Float3 &rgb) noexcept {
    return dot(rgb, make_float3(.2126f, .7152f, .0722f));
}

// RTXPT retains this historical projection for material similarity and final MIS.
Float luminance_legacy(const Float3 &rgb) noexcept {
    return dot(rgb, make_float3(.299f, .587f, .114f));
}

Float mis_initial_weight(const Float3 &true_f, const Float3 &rough_f) noexcept {
    Float3 rough = clamp(rough_f, 1e-4f, 1e4f), exact = clamp(true_f, 0.f, 1e4f);
    Float w = saturate(luminance_legacy(exact) / luminance_legacy(exact + rough));
    return w * w * w;
}
}// namespace vision::rtxpt_gi

namespace vision {
using namespace ocarina;

namespace {
Uint native_jenkins(Uint a) {
    a = (a + 0x7ed55d16u) + (a << 12u);
    a = (a ^ 0xc761c23cu) ^ (a >> 19u);
    a = (a + 0x165667b1u) + (a << 5u);
    a = (a + 0xd3a2646cu) ^ (a << 9u);
    a = (a + 0xfd7046c5u) + (a << 3u);
    return (a ^ 0xb55a4f09u) ^ (a >> 16u);
}
Int2 permute(Int2 p, const Uint &random) {
    Int2 offset = make_int2(cast<int>(random & 3u), cast<int>((random >> 2u) & 3u));
    p += offset;
    p.x = p.x ^ 3;
    p.y = p.y ^ 3;
    return p - offset;
}
Int2 temporal_offset(const Int &sample) {
    Int s = sample & 7, m2 = (s >> 1) & 1, m4 = 1 - ((s >> 2) & 1);
    Int x = -1 + 2 * (s & 1), y = 1 - 2 * m2;
    return make_int2(x * (m4 | m2), x * y * (m4 | (1 - m2)));
}
// Keep the material evaluator and its true lobes alive. Only alpha is changed
// while evaluating RTXPT's roughness-clamped final-MIS counterpart.
Float3 rough_f(MaterialEvaluator &bsdf, const Interaction &it, const Float3 &wi) {
    vector<Microfacet<D> *> microfacets;
    vector<Float2> original;
    auto visit = [&](Lobe *lobe) {
        Microfacet<D> *microfacet = nullptr;
        if (auto *reflection = dynamic_cast<MicrofacetLobe *>(lobe)) microfacet = reflection->microfacet();
        else if (auto *dielectric = dynamic_cast<DielectricLobe *>(lobe)) microfacet = dielectric->microfacet();
        if (microfacet && std::find(microfacets.begin(), microfacets.end(), microfacet) == microfacets.end()) {
            microfacets.push_back(microfacet);
            original.push_back(make_float2(microfacet->alpha_x(), microfacet->alpha_y()));
            microfacet->set_alpha_x(max(microfacet->alpha_x(), .04f));
            microfacet->set_alpha_y(max(microfacet->alpha_y(), .04f));
        }
    };
    bsdf.dispatch([&](Lobe *root) {
        // Saved alpha must outlive the selected lobe case for the restore below.
        outline("RTXPT GI save and clamp roughness", [&] {
            if (root->is_multi()) static_cast<LobeSet *>(root)->for_each([&](WeightedLobe &lobe) { visit(lobe.get()); });
            else visit(root);
        });
    });
    Float3 result = bsdf.evaluate(it.wo, wi, MaterialEvalMode::F).f.vec3();
    for (size_t i = 0; i < microfacets.size(); ++i) {
        microfacets[i]->set_alpha_x(original[i].x);
        microfacets[i]->set_alpha_y(original[i].y);
    }
    return result;
}
}// namespace

RtxptGIReservoirVar RtxptGI::read(const Uint &index, uint slice) const noexcept {
    auto buffer = pipeline()->bindless_array().buffer_var<RtxptGIStorage>(reservoirs_.index_var() + slice);
    return rtxpt_gi::unpack(buffer.read(index));
}
void RtxptGI::write(const Uint &index, uint slice, const RtxptGIReservoirVar &r) const noexcept {
    auto buffer = pipeline()->bindless_array().buffer_var<RtxptGIStorage>(reservoirs_.index_var() + slice);
    buffer.write(index, rtxpt_gi::pack(r));
}
void RtxptGI::initialize(const Uint &frame, const Uint2 &pixel) const noexcept {
    auto &sampler = renderer().sampler();
    sampler->load_data();
    scene().sensor()->load_data();
    owner_->emplace_frame_index(frame);
    SampledWavelengths swl{spectrum()->dimension()};
    sampler->temporary([&](Sampler *) {
        sampler->set_seed(pixel, frame, -1);
        swl = spectrum()->sample_wavelength(sampler);
    });
    owner_->emplace_sampled_wavelengths(swl);
}
Interaction RtxptGI::interaction(const SurfaceDataVar &s, const Uint &index, bool previous) const noexcept {
    Float3 view;
    if (previous) {
        view = scene().sensor()->prev_device_position();
        $if(s.is_replaced) { view = owner_->prev_surface_extends().read(index).view_pos.as_vec3(); };
    } else view = owner_->cur_view_pos(s.is_replaced, index);
    return geometry().compute_surface_interaction(s.hit, view);
}
RtxptGIReservoirVar RtxptGI::candidate(const Uint &index, const Interaction &receiver) const noexcept {
    RtxptGIReservoirVar r;
    HitBSDFVar hit = frame_buffer().hit_bsdfs().read(index);
    $if(hit.pdf > 0.f && !ocarina::isinf(hit.pdf) && !ocarina::isnan(hit.pdf)) {
        r.sample = owner_->sample_buffer().read(index);
        $if(!r.sample.sp->valid()) {
            r.sample.sp->set_position(receiver.pos + hit.wi.as_vec3() * 50000.f);
            r.sample.sp->set_normal(-hit.wi.as_vec3());
        };
        Float3 normal = r.sample.sp->normal();
        Float3 to_receiver = receiver.pos - r.sample.sp->position();
        r.sample.sp->set_normal(select(dot(normal, to_receiver) < 0.f, -normal, normal));
        r.sample.Lo.set(r.sample.Lo.as_vec3() / hit.pdf);
        r.weight = 1.f;
        r.M = 1u;
        r.age = 0u;
    };
    return r;
}
Float RtxptGI::target(const Interaction &receiver, const RtxptGIReservoirVar &r,
                      MaterialEvaluator *bsdf) const noexcept {
    Float value = 0.f;
    $if(r.M != 0u && length_squared(r.sample.sp->position() - receiver.pos) > 0.f) {
        Float3 f = owner_->eval_bsdf(receiver, r.sample, MaterialEvalMode::F, bsdf).f.vec3();
        value = max(0.f, zero_if_nan_inf(rtxpt_gi::luminance_709(f * r.sample.Lo.as_vec3())));
    };
    return value;
}
Bool RtxptGI::similar(const SurfaceDataVar &a, const SurfaceDataVar &b,
                      const Float &normal_threshold, const Float &depth_threshold,
                      const Float &expected_depth, const Bool &fallback) const noexcept {
    Bool geometry_match = dot(a->normal(), b->normal()) >= normal_threshold &&
        (depth_threshold <= 0.f || abs(expected_depth - b->depth()) <= depth_threshold * max(expected_depth, b->depth()));
    Float ar = max(a.diffuse_roughness.w, a.specular_roughness.w);
    Float br = max(b.diffuse_roughness.w, b.specular_roughness.w);
    Bool materials = abs(ar - br) <= .5f * max(ar, br) &&
        abs(rtxpt_gi::luminance_legacy(a.diffuse_roughness.xyz()) - rtxpt_gi::luminance_legacy(b.diffuse_roughness.xyz())) <= .25f &&
        abs(rtxpt_gi::luminance_legacy(a.specular_roughness.xyz()) - rtxpt_gi::luminance_legacy(b.specular_roughness.xyz())) <= .25f;
    // Corona already identifies the receiver prefix; no triangle/material-ID gate.
    Bool group = a.stable_branch == b.stable_branch && a.is_replaced == b.is_replaced;
    return a.hit->is_hit() && b.hit->is_hit() && (fallback || geometry_match) && materials && group;
}

void RtxptGI::compile_temporal() noexcept {
    Kernel kernel = [&](Var<GIParam> param, Uint frame, Uint2 resolution) {
        Function::current()->require_ray_query_compute_workgroups();
        Uint2 pixel = dispatch_idx().xy();
        Uint index = pixel.x + pixel.y * resolution.x;
        Bool inside = all(pixel < resolution);
        initialize(frame, pixel);
        auto &rng = renderer().sampler();
        rng->set_seed(pixel, frame, 5);
        RtxptGIReservoirVar result;
        $if(inside) {
            auto surface = owner_->cur_surfaces().read(index);
            $if(surface.hit->is_hit()) {
                Interaction it = interaction(surface, index);
                auto input = candidate(index, it);
                auto resample = [&](MaterialEvaluator &bsdf) {
                    Float selected_target = target(it, input, addressof(bsdf));
                    $if(input.M != 0u) { (void)rtxpt_gi::combine(result, input, .5f, selected_target); };
                    RtxptGIReservoirVar history;
                    SurfaceDataVar history_surface;
                    Interaction old_it{geometry().process_mediums()};
                    Uint history_index = 0u;
                    Bool found = false, selected_history = false;
                    Uint max_age = param.max_age;
                    $if(param.temporal != 0u && param.rtxpt_vary_age != 0u) {
                        max_age = cast<uint>(cast<float>(max_age) * (.5f + .5f * rng->next_1d()));
                    };
                    $if(param.temporal != 0u) {
                        // Corona stores current-minus-previous pixel motion.
                        SensorSample ss;
                        rng->temporary([&](Sampler *s) {
                            s->set_seed(pixel, frame, Dimension::Camera);
                            ss = s->sensor_sample(pixel, scene().sensor()->filter(), param.camera_jitter != 0u);
                        });
                        Int2 projected = make_int2(round(previous_reservoir_coord(ss.p_film, frame_buffer().motion_vectors().read(index)) - .5f));
                        Int start = cast<int>(rng->next_1d() * 8.f);
                        Uint uniform = native_jenkins(frame);
                        // No discarded lane may leave before the group boiling barriers.
                        $for(i, 6u) {
                            Bool fallback = i == 5u;
                            $if(fallback && param.rtxpt_fallback == 0u) { $continue; };
                            Int2 p = projected;
                            $if(i > 0u && !fallback) { p += temporal_offset(start + cast<int>(i)); };
                            $if(fallback) { p = make_int2(pixel); };
                            $if((param.rtxpt_permutation != 0u && i == 0u) || fallback) { p = permute(p, uniform); };
                            $if(any(p < 0) || any(p >= make_int2(resolution))) { $continue; };
                            Uint n = cast<uint>(p.x) + cast<uint>(p.y) * resolution.x;
                            auto previous = owner_->prev_surfaces().read(n);
                            Float3 guide_position = rtxpt_gi::depth_guide_position(surface->position(),
                                surface.virtual_position.as_vec3(), surface.depth_position.as_vec3(),
                                surface.is_replaced != 0u && surface.stable_branch != InvalidUI32,
                                surface.approximate != 0u);
                            Float expected = scene().sensor()->prev_linear_depth(guide_position);
                            $if(!similar(surface, previous, param.t_dot, param.t_depth, expected, fallback)) { $continue; };
                            auto old = read(n, 1u);
                            $if(old.M == 0u) { $continue; };
                            history = old;
                            history_surface = previous;
                            history_index = n;
                            found = true;
                            $break;
                        };
                    };
                    $if(found) {
                        old_it = interaction(history_surface, history_index, true);
                        Float J = rtxpt_gi::jacobian(it.pos, old_it.pos, history.sample.sp->position(), history.sample.sp->normal());
                        found = rtxpt_gi::validate_jacobian(J);
                        history.weight *= J;
                        history.M = min(history.M, param.history_limit);
                        history.age += 1u;
                        found &= history.age <= max_age;
                        $if(found) {
                            Float p = target(it, history, addressof(bsdf));
                            selected_history = rtxpt_gi::combine(result, history, rng->next_1d(), p);
                            selected_target = select(selected_history, p, selected_target);
                        };
                    };
                    $if(param.rtxpt_bias_correction >= 1u) {
                        Float pi = selected_target, sum = selected_target * cast<float>(input.M);
                        $if(found && result.M != 0u) {
                            Float p = target(old_it, result);
                            $if(param.rtxpt_bias_correction == 3u && p > 0.f) {
                                // Native RTXPT's temporal conservative adapter uses current BVH.
                                p *= cast<float>(!geometry().occluded(it, result.sample.sp->position()));
                            };
                            pi = select(selected_history, p, pi);
                            sum += p * cast<float>(history.M);
                        };
                        rtxpt_gi::finalize(result, pi, sum * selected_target);
                    } $else {
                        rtxpt_gi::finalize(result, 1.f, selected_target * cast<float>(result.M));
                    };
                };
                if (owner_->integrator()->separate()) {
                    const auto &swl = owner_->sampled_wavelengths();
                    MaterialEvaluator bsdf(it, swl);
                    scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                        outline("RTXPT GI temporal build evaluator", [&] {
                            material->build_evaluator(bsdf, it, swl);
                            bsdf.update_frame(it.shading);
                        });
                    });
                    resample(bsdf);
                } else {
                    scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                        auto bsdf = material->create_evaluator(it, owner_->sampled_wavelengths());
                        resample(bsdf);
                    });
                }
            };
        };
        SharedArray<float, 64> weights;
        Uint lane = (pixel.x & 7u) + (pixel.y & 7u) * 8u;
        // The flag is dispatch-uniform: every padded lane takes both barriers,
        // or every lane skips the complete native boiling filter.
        $if(param.rtxpt_boiling != 0u) {
            Float q = rtxpt_gi::luminance_709(result.sample.Lo.as_vec3()) * result.weight;
            weights.write(lane, select(inside, q, 0.f));
            synchronize_block();
            $if(lane == 0u) {
                Float sum = 0.f, count = 0.f;
                $for(i, 64u) {
                    Float v = weights.read(i);
                    sum += v;
                    count += select(v > 0.f, 1.f, 0.f);
                };
                weights.write(0u, select(count > 0.f, sum / count, 0.f));
            };
            synchronize_block();
            Float multiplier = 10.f / clamp(param.rtxpt_boiling_strength, 1e-6f, 1.f) - 9.f;
            $if(q > weights.read(0u) * multiplier) { result = RtxptGIReservoirVar{}; };
        };
        $if(inside) { write(index, 0u, result); };
    };
    kernel.function()->set_block_dim(8u, 8u, 1u);
    temporal_ = device().compile(kernel, "RTXPT GI temporal resampling");
}

void RtxptGI::compile_spatial() noexcept {
    Kernel kernel = [&](Var<GIParam> param, Uint frame, Uint2 resolution) {
        Function::current()->require_ray_query_compute_workgroups();
        Uint2 pixel = dispatch_idx().xy();
        $if(any(pixel >= resolution)) { $return(); };
        Uint index = pixel.x + pixel.y * resolution.x;
        initialize(frame, pixel);
        auto &rng = renderer().sampler();
        rng->set_seed(pixel, frame, 6);
        auto surface = owner_->cur_surfaces().read(index);
        auto input = read(index, 0u);
        RtxptGIReservoirVar result;
        $if(surface.hit->is_hit()) {
            Interaction it = interaction(surface, index);
            auto resample = [&](MaterialEvaluator &bsdf) {
                Float selected_target = target(it, input, addressof(bsdf));
                $if(input.M != 0u) { (void)rtxpt_gi::combine(result, input, .5f, selected_target); };
                Uint mask = 0u;
                Int selected = -1;
                Uint start = cast<uint>(rng->next_1d() * 8191.f);
                Uint count = select(param.spatial != 0u, min(param.N, 32u), 0u);
                auto neighbor_pixel = [&](const Uint &i) {
                    Float2 offset = neighbors_.read((start + i) & 8191u) * param.s_radius;
                    return clamp(make_int2(pixel) + make_int2(offset), make_int2(0), make_int2(resolution) - 1);
                };
                $for(i, count) {
                    Int2 p = neighbor_pixel(i);
                    Uint n = cast<uint>(p.x) + cast<uint>(p.y) * resolution.x;
                    auto neighbor_surface = owner_->cur_surfaces().read(n);
                    $if(!similar(surface, neighbor_surface, param.s_dot, param.s_depth, surface->depth())) { $continue; };
                    auto neighbor = read(n, 0u);
                    $if(neighbor.M == 0u) { $continue; };
                    Float J = rtxpt_gi::jacobian(it.pos, neighbor_surface->position(), neighbor.sample.sp->position(), neighbor.sample.sp->normal());
                    Float p_hat = target(it, neighbor, addressof(bsdf));
                    $if(!rtxpt_gi::validate_jacobian(J)) { $continue; };
                    mask |= 1u << i;
                    Bool picked = rtxpt_gi::combine(result, neighbor, rng->next_1d(), p_hat * J);
                    selected = select(picked, cast<int>(i), selected);
                    selected_target = select(picked, p_hat, selected_target);
                };
                $if(param.rtxpt_bias_correction >= 1u) {
                    Float pi = selected_target, sum = selected_target * cast<float>(input.M);
                    $for(i, count) {
                        $if((mask & (1u << i)) == 0u) { $continue; };
                        Int2 p = neighbor_pixel(i);
                        Uint n = cast<uint>(p.x) + cast<uint>(p.y) * resolution.x;
                        auto neighbor_surface = owner_->cur_surfaces().read(n);
                        auto neighbor = read(n, 0u);
                        Interaction n_it = interaction(neighbor_surface, n);
                        Float p_hat = target(n_it, result);
                        $if(param.rtxpt_bias_correction == 3u && p_hat > 0.f) {
                            p_hat *= cast<float>(!geometry().occluded(n_it, result.sample.sp->position()));
                        };
                        pi = select(selected == cast<int>(i), p_hat, pi);
                        sum += p_hat * cast<float>(neighbor.M);
                    };
                    rtxpt_gi::finalize(result, pi, sum * selected_target);
                } $else {
                    rtxpt_gi::finalize(result, 1.f, selected_target * cast<float>(result.M));
                };
            };
            if (owner_->integrator()->separate()) {
                const auto &swl = owner_->sampled_wavelengths();
                MaterialEvaluator bsdf(it, swl);
                scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                    outline("RTXPT GI spatial build evaluator", [&] {
                        material->build_evaluator(bsdf, it, swl);
                        bsdf.update_frame(it.shading);
                    });
                });
                resample(bsdf);
            } else {
                scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                    auto bsdf = material->create_evaluator(it, owner_->sampled_wavelengths());
                    resample(bsdf);
                });
            }
        };
        write(index, 1u, result);
    };
    kernel.function()->set_block_dim(8u, 8u, 1u);
    spatial_ = device().compile(kernel, "RTXPT GI spatial resampling");
}

void RtxptGI::compile_final() noexcept {
    const bool lobes = owner_->integrator()->denoiser() && owner_->integrator()->denoiser()->requires_lobe_signals();
    Kernel kernel = [&](Var<GIParam> param, Uint frame, Uint2 resolution) {
        Function::current()->require_ray_query_compute_workgroups();
        Uint2 pixel = dispatch_idx().xy();
        $if(any(pixel >= resolution)) { $return(); };
        Uint index = pixel.x + pixel.y * resolution.x;
        initialize(frame, pixel);
        auto surface = owner_->cur_surfaces().read(index);
        Float3 radiance = make_float3(0.f), specular = make_float3(0.f);
        Float hit_distance = 0.f;
        $if(surface.hit->is_hit()) {
            Interaction it = interaction(surface, index);
            auto selected = read(index, 1u);
            auto fresh = candidate(index, it);
            Float3 prefix = make_float3(1.f);
            $if(surface.is_replaced) { prefix = owner_->cur_surface_extends().read(index).throughput.as_vec3(); };
            $if(selected.M != 0u && length_squared(selected.sample.sp->position() - it.pos) > 0.f) {
                auto shade = [&](MaterialEvaluator &bsdf) {
                    Float3 direction = normalize(selected.sample.sp->position() - it.pos);
                    hit_distance = length(selected.sample.sp->position() - it.pos);
                    Float3 f = owner_->eval_bsdf(it, selected.sample, MaterialEvalMode::F, addressof(bsdf)).f.vec3();
                    Float3 li = selected.sample.Lo.as_vec3() * selected.weight;
                    $if(param.rtxpt_final_visibility != 0u) {
                        li *= cast<float>(!geometry().occluded(it, selected.sample.sp->position()));
                    };
                    Float selected_weight = 1.f, initial_weight = 0.f;
                    Float3 fresh_f = make_float3(0.f), fresh_direction = direction;
                    Float initial_distance = 0.f;
                    $if(param.rtxpt_final_mis != 0u) {
                        selected_weight = 1.f - rtxpt_gi::mis_initial_weight(f, rough_f(bsdf, it, direction));
                        $if(fresh.M != 0u && length_squared(fresh.sample.sp->position() - it.pos) > 0.f) {
                            fresh_direction = normalize(fresh.sample.sp->position() - it.pos);
                            initial_distance = length(fresh.sample.sp->position() - it.pos);
                            fresh_f = owner_->eval_bsdf(it, fresh.sample, MaterialEvalMode::F, addressof(bsdf)).f.vec3();
                            initial_weight = rtxpt_gi::mis_initial_weight(fresh_f, rough_f(bsdf, it, fresh_direction));
                        };
                    };
                    Float3 selected_value = f * li * selected_weight;
                    Float3 fresh_value = fresh_f * fresh.sample.Lo.as_vec3() * fresh.weight * initial_weight;
                    radiance = zero_if_nan_inf(prefix * (selected_value + fresh_value));
                    if (lobes) {
                        specular = zero_if_nan_inf(prefix * (
                            selected_value * bsdf.specular_fraction_rgb(it.wo, direction) +
                            fresh_value * bsdf.specular_fraction_rgb(it.wo, fresh_direction)));
                    }
                    hit_distance = hit_distance * selected_weight + initial_distance * initial_weight;
                };
                if (owner_->integrator()->separate()) {
                    const auto &swl = owner_->sampled_wavelengths();
                    MaterialEvaluator bsdf(it, swl);
                    scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                        outline("RTXPT GI final build evaluator", [&] {
                            material->build_evaluator(bsdf, it, swl);
                            bsdf.update_frame(it.shading);
                        });
                    });
                    shade(bsdf);
                } else {
                    scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                        auto bsdf = material->create_evaluator(it, owner_->sampled_wavelengths());
                        shade(bsdf);
                    });
                }
            };
        };
        owner_->write_radiance(index, make_float4(radiance, 1.f));
        if (lobes) {
            $if(param.precise_lobes != 0u) {
                Float4 direct = make_float4(0.f), di_specular = make_float4(0.f);
                Float3 bypass = make_float3(0.f);
                $if(param.denoiser_di_active != 0u) {
                    direct = frame_buffer().denoiser_di_specular().read(index);
                    di_specular = make_float4(frame_buffer().denoiser_gi_specular().read(index).xyz(), direct.w);
                    bypass = frame_buffer().denoiser_di_bypass().read(index).xyz();
                };
                Uint layer = 0u;
                $if(param.denoiser_layered != 0u) { layer = frame_buffer().stable_dominant().read(index); };
                $if(layer < StablePlaneCount) {
                    Uint address = layer * (resolution.x * resolution.y) + index;
                    Float4 old_d = make_float4(0.f), old_s = make_float4(0.f), old_b = make_float4(0.f);
                    $if(param.denoiser_layered != 0u) {
                        old_d = frame_buffer().denoiser_diffuse().read(address);
                        old_s = frame_buffer().denoiser_specular().read(address);
                        old_b = frame_buffer().denoiser_bypass().read(address);
                    };
                    auto merged = denoiser_signals::merge_restir_lobes(direct.xyz(), radiance, di_specular,
                        make_float4(specular, hit_distance), bypass, param.denoiser_di_factor,
                        param.denoiser_gi_factor, old_d, old_s, old_b);
                    frame_buffer().denoiser_diffuse().write(address, merged.diffuse);
                    frame_buffer().denoiser_specular().write(address, merged.specular);
                    frame_buffer().denoiser_bypass().write(address, merged.bypass);
                };
                frame_buffer().denoiser_di_specular().write(index, di_specular);
            };
            frame_buffer().denoiser_gi_specular().write(index, make_float4(specular, hit_distance));
        }
    };
    kernel.function()->set_block_dim(8u, 8u, 1u);
    final_ = device().compile(kernel, "RTXPT GI final shading");
}

void RtxptGI::reset_history() noexcept {
    previous_frame_ = InvalidUI32;
    previous_jitter_ = InvalidUI32;
}

void RtxptGI::update_resolution(uint2 resolution) noexcept {
    extent_ = resolution;
    const uint count = resolution.x * resolution.y;
    const bool registered = reservoirs_.has_registered();
    reservoirs_.super() = device().create_buffer<RtxptGIStorage>(count * 2u, "RTXPT GI SDK32 reservoirs x2");
    reservoirs_.register_self(0u, count);
    if (registered) reservoirs_.register_view_index(1u, count, count);
    else reservoirs_.register_view(count, count);
    vector<RtxptGIStorage> empty(count * 2u);
    reservoirs_.upload_immediately(empty.data());
    reset_history();
}

void RtxptGI::prepare() noexcept {
    const uint2 resolution = pipeline()->resolution();
    if (!reservoirs_.has_registered() || extent_.x != resolution.x || extent_.y != resolution.y)
        update_resolution(resolution);
    if (!neighbors_.has_registered()) {
        vector<float2> values;
        values.reserve(8192u);
        float u = .5f, v = .5f;
        constexpr float phi = 1.f / 1.3247179572447f;
        while (values.size() < 8192u) {
            u += phi; v += phi * phi;
            if (u >= 1.f) u -= 1.f;
            if (v >= 1.f) v -= 1.f;
            float x = u - .5f, y = v - .5f;
            if (x * x + y * y > .25f) continue;
            values.push_back(make_float2(float(int(x * 250.f)), float(int(y * 250.f))) / 127.f);
        }
        neighbors_.super() = device().create_buffer<float2>(8192u, "RTXPT GI SDK neighbor offsets");
        neighbors_.register_self();
        neighbors_.upload_immediately(values.data());
    }
}

void RtxptGI::compile() noexcept {
    compile_temporal();
    compile_spatial();
    compile_final();
}

CommandBatch RtxptGI::dispatch(uint frame, GIParam param) const noexcept {
    const bool continuous = previous_frame_ != InvalidUI32 && previous_frame_ + 1u == frame;
    const bool same_lattice = previous_jitter_ == param.camera_jitter;
    param.temporal = param.temporal && frame != 0u && continuous && same_lattice;
    previous_frame_ = frame;
    previous_jitter_ = param.camera_jitter;
    uint2 padded = make_uint2((extent_.x + 7u) & ~7u, (extent_.y + 7u) & ~7u);
    CommandBatch batch;
    batch << temporal_(param, frame, extent_).dispatch(padded);
    batch << spatial_(param, frame, extent_).dispatch(padded);
    batch << final_(param, frame, extent_).dispatch(padded);
    return batch;
}

void RtxptGI::download_storage(vector<GISample> &samples, vector<GIReservoir> &reservoirs) const noexcept {
    const uint count = extent_.x * extent_.y;
    samples.resize(count);
    vector<RtxptGIStorage> packed(count * 2u);
    pipeline()->stream() << owner_->sample_buffer().view().download(samples.data())
                         << reservoirs_.view().download(packed.data()) << synchronize() << commit();
    reservoirs.resize(count * 2u);
    for (uint i = 0; i < count * 2u; ++i) {
        const auto &words = packed[i].words;
        auto &value = reservoirs[i];
        value.C = float(words[3] & 255u);
        value.W = std::bit_cast<float>(words[5]);
        value.weight_sum = 0.f; // Native stores normalized W, not streaming S.
        value.sample.age = float((words[3] >> 8u) & 255u);
        for (uint c = 0; c < 3u; ++c) value.sample.sp.pos[c] = std::bit_cast<float>(words[c]);
        value.sample.sp.set_normal(unpack_gi_normal_host(words[6]));
        uint le = words[4] >> 18u;
        value.sample.Lo = {0.f, 0.f, 0.f};
        if (le != 0u) {
            float Y = std::exp2((float(le) + .5f) / 409.6f - 20.f);
            float u = (float((words[4] >> 9u) & 511u) + .5f) / 820.f;
            float v = (float(words[4] & 511u) + .5f) / 820.f;
            float inv = 1.f / (6.f * u - 16.f * v + 12.f);
            float x = 9.f * u * inv, y = 4.f * v * inv, s = Y / y;
            float X = s * x, Z = s * (1.f - x - y);
            value.sample.Lo = {std::max(3.240969941904522f * X - 1.537383177570094f * Y - .4986107602930032f * Z, 0.f),
                               std::max(-.9692436362808803f * X + 1.875967501507721f * Y + .04155505740717569f * Z, 0.f),
                               std::max(.05563007969699373f * X - .2039769588889765f * Y + 1.056971514242878f * Z, 0.f)};
        }
    }
}
}// namespace vision
