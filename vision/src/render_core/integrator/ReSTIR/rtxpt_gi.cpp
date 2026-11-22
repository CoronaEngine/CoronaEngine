#include "rtxpt_gi.h"
#include "denoiser_signals.h"
#include "rtxpt_gi_boiling.h"
#include "rtxpt_gi_candidate.h"
#include "rtxpt_gi_lights.h"
#include "rtxpt_gi_material_inputs.h"
#include "rtxpt_gi_prefix_storage.h"
#include "rtxpt_gi_pt.h"
#include "rtxpt_gi_rays.h"
#include "rtxpt_gi_rng.h"
#include "base/integral/integrator.h"
#include "base/integral/path_work_capture.h"
#include "base/scattering/gi_ideal_glass.h"
#include "math/base.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>

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
float3 unpack_gi_normal_host(uint packed) noexcept {
    if (packed == 0x80008000u) return make_float3(0.f);
    int x = int(packed & 0xffffu), y = int(packed >> 16u);
    x -= x >= 32768 ? 65536 : 0;
    y -= y >= 32768 ? 65536 : 0;
    return ocarina::octahedral_decode(make_float2(float(x) / 32767.f, float(y) / 32767.f));
}

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
    emplace_frame_index(frame);
    SampledWavelengths swl{spectrum()->dimension()};
    sampler->temporary([&](Sampler *) {
        sampler->set_seed(pixel, frame, -1);
        swl = spectrum()->sample_wavelength(sampler);
    });
    emplace_sampled_wavelengths(swl);
}
rtxpt_gi::PTSurfaceView RtxptGI::read_pt_surface(const Uint &index, const Uint &frame, bool previous) const noexcept {
    Uint plane = select((frame & 1u) == 0u, 1u, 0u);
    if (previous) plane = 1u - plane;
    auto buffer = pipeline()->bindless_array().buffer_var<RtxptGIPTSurface>(surfaces_.index_var() + plane);
    return rtxpt_gi::unpack_pt_surface(buffer.read(index));
}
void RtxptGI::write_pt_surface(const Uint &index, const rtxpt_gi::PTSurfaceView &surface) const noexcept {
    auto packed = rtxpt_gi::pack_pt_surface(surface);
    Uint plane = select((frame_index() & 1u) == 0u, 1u, 0u);
    auto buffer = pipeline()->bindless_array().buffer_var<RtxptGIPTSurface>(surfaces_.index_var() + plane);
    buffer.write(index, packed);
    if (references_.has_registered()) {
        auto reference = pipeline()->bindless_array().buffer_var<RtxptGIPTReference>(references_.index_var());
        reference.write(index, rtxpt_gi::pack_pt_reference(surface));
    }
}
RtxptGIReservoirVar RtxptGI::candidate(const Uint &index, const Interaction &receiver) const noexcept {
    RtxptGIReservoirVar r;
    r.M = 1u;
    r.weight = 0.f;
    HitBSDFVar hit = rtxpt_proposals_.read(index);
    $if(hit.pdf > 0.f && !ocarina::isinf(hit.pdf) && !ocarina::isnan(hit.pdf)) {
        r.sample = samples_.read(index);
        $if(!r.sample.sp->valid()) {
            r.sample.sp->set_position(receiver.pos + hit.wi.as_vec3() * 50000.f);
            r.sample.sp->set_normal(-hit.wi.as_vec3());
        };
        // Native secondary normal texture is UNORM16 octahedral, followed by
        // the SDK reservoir's separate SNORM16 packing at the pass boundary.
        Uint2 encoded = cast<uint2>(saturate(rtxpt_gi::standard_oct(r.sample.sp->normal())) * 65534.f);
        r.sample.sp->set_normal(rtxpt_gi::standard_unoct(saturate(make_float2(encoded) / 65534.f)));
        // Shared PT exports receiver-independent incident radiance. Keep the
        // proposal density in reservoir W and the current prefix outside Lo.
        Float3 incident = r.sample.Lo.as_vec3();
        // RTXPT stores this normalized signal in RGBA16_FLOAT before reuse.
        r.sample.Lo.set(make_float3(f16tof32(f32tof16(incident.x)),
                                   f16tof32(f32tof16(incident.y)),
                                   f16tof32(f32tof16(incident.z))));
        r.weight = 1.f / hit.pdf;
        r.M = 1u;
        r.age = 0u;
    };
    return r;
}
Float RtxptGI::target(const rtxpt_gi::PTSurfaceView &receiver, const RtxptGIReservoirVar &r) const noexcept {
    Float value = 0.f;
    $if(r.M != 0u && any(r.sample.Lo.as_vec3() != make_float3(0.f)) && length_squared(r.sample.sp->position() - receiver.it.pos) > 0.f) {
        auto f = receiver.f(normalize(r.sample.sp->position() - receiver.it.pos), sampled_wavelengths());
        value = max(0.f, zero_if_nan_inf(rtxpt_gi::luminance_709(f * r.sample.Lo.as_vec3())));
    };
    return value;
}
Bool RtxptGI::similar(const rtxpt_gi::PTSurfaceView &a, const rtxpt_gi::PTSurfaceView &b,
                      const Float &normal_threshold, const Float &depth_threshold,
                      const Float &expected_depth, const Bool &fallback) const noexcept {
    Bool geometry_match = dot(a.normal, b.normal) >= normal_threshold &&
        (depth_threshold <= 0.f || abs(expected_depth - b.depth) <= depth_threshold * max(expected_depth, b.depth));
    Float ar = a.roughness.y, br = b.roughness.y;
    Bool materials = abs(ar - br) <= .5f * max(ar, br) &&
        abs(rtxpt_gi::luminance_legacy(a.diffuse_guide) - rtxpt_gi::luminance_legacy(b.diffuse_guide)) <= .25f &&
        abs(rtxpt_gi::luminance_legacy(a.specular_guide) - rtxpt_gi::luminance_legacy(b.specular_guide)) <= .25f;
    return a.valid && b.valid && (fallback || geometry_match) && materials && a.plane_hash == b.plane_hash;
}

void RtxptGI::compile_temporal() noexcept {
    // This policy is host-owned; generate only the selected mode.
    const uint bias_correction = construct_param().rtxpt_bias_correction;
    OC_INFO_FORMAT("RTXPT GI compiled bias correction: temporal mode={}", bias_correction);
    Kernel kernel = [&](Var<GIParam> param, Uint frame, Uint2 resolution) {
        Function::current()->require_ray_query_compute_workgroups();
        Uint2 pixel = dispatch_idx().xy();
        Uint index = pixel.x + pixel.y * resolution.x;
        Bool inside = all(pixel < resolution);
        initialize(frame, pixel);
        Uint rng_state = rtxpt_gi::resampling_seed(pixel, resolution, frame, 5u);
        RtxptGIReservoirVar result;
        $if(inside) {
            auto surface = read_pt_surface(index, frame);
            $if(surface.valid) {
                Interaction it = surface.it;
                auto input = candidate(index, it);
                auto resample = [&]() {
                    Float selected_target = target(surface, input);
                    $if(input.M != 0u) { (void)rtxpt_gi::combine(result, input, .5f, selected_target); };
                    RtxptGIReservoirVar history;
                    rtxpt_gi::PTSurfaceView history_surface;
                    Interaction old_it{geometry().process_mediums()};
                    Uint history_index = 0u;
                    Bool found = false, selected_history = false;
                    Uint max_age = param.max_age;
                    $if(param.temporal != 0u && param.rtxpt_vary_age != 0u) {
                        max_age = cast<uint>(cast<float>(max_age) * (.5f + .5f * rtxpt_gi::resampling_next(rng_state)));
                    };
                    $if(param.temporal != 0u) {
                        // Corona stores current-minus-previous pixel motion.
                        SensorSample ss;
                        renderer().sampler()->temporary([&](Sampler *s) {
                            s->set_seed(pixel, frame, Dimension::Camera);
                            ss = s->sensor_sample(pixel, scene().sensor()->filter(), param.camera_jitter != 0u);
                        });
                        const Uint2 packed_motion = rtxpt_prefix_aux_.read(index).packed_motion;
                        Float2 motion = make_float2(f16tof32(packed_motion.x & 0xffffu),
                                                   f16tof32(packed_motion.x >> 16u));
                        Int2 projected = make_int2(round(previous_reservoir_coord(ss.p_film, motion) - .5f));
                        Float depth_motion = f16tof32(packed_motion.y & 0xffffu);
                        Float expected = surface.depth + depth_motion;
                        Int start = cast<int>(rtxpt_gi::resampling_next(rng_state) * 8.f);
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
                            auto previous = read_pt_surface(n, frame, true);
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
                        old_it = history_surface.it;
                        Float J = rtxpt_gi::jacobian(rtxpt_gi::native_offset_origin(it.pos, it.ng), rtxpt_gi::native_offset_origin(old_it.pos, old_it.ng), history.sample.sp->position(), history.sample.sp->normal());
                        found = rtxpt_gi::validate_jacobian(J);
                        history.weight *= J;
                        history.M = min(history.M, param.history_limit);
                        history.age += 1u;
                        found &= history.age <= max_age;
                        $if(found) {
                            Float p = target(surface, history);
                            selected_history = rtxpt_gi::combine(result, history, rtxpt_gi::resampling_next(rng_state), p);
                            selected_target = select(selected_history, p, selected_target);
                        };
                    };
                    if (bias_correction >= 1u) {
                        Float pi = selected_target, sum = selected_target * cast<float>(input.M);
                        $if(found && result.M != 0u) {
                            Float p = target(history_surface, result);
                            if (bias_correction == 3u) {
                                $if(p > 0.f) {
                                    // Native RTXPT's temporal conservative adapter uses current BVH.
                                    p *= cast<float>(!rtxpt_gi::native_occluded(it, result.sample.sp->position(), rtxpt_materials(), sampled_wavelengths()));
                                };
                            }
                            pi = select(selected_history, p, pi);
                            sum += p * cast<float>(history.M);
                        };
                        rtxpt_gi::finalize(result, pi, sum * selected_target);
                    } else {
                        rtxpt_gi::finalize(result, 1.f, selected_target * cast<float>(result.M));
                    }
                };
                resample();
            };
        };
        // The flag is dispatch-uniform: every padded lane reaches both native
        // group barriers, or every lane skips the complete boiling filter.
        $if(param.rtxpt_boiling != 0u) {
            Float q = rtxpt_gi::luminance_709(result.sample.Lo.as_vec3()) * result.weight;
            Uint2 local_index = pixel & make_uint2(7u);
            $if(rtxpt_gi::native_boiling_filter(local_index, param.rtxpt_boiling_strength,
                                               select(inside, q, 0.f))) {
                result = RtxptGIReservoirVar{};
            };
        };
        $if(inside) { write(index, 0u, result); };
    };
    kernel.function()->set_block_dim(8u, 8u, 1u);
    temporal_shader_ = device().compile(kernel, "RTXPT GI temporal resampling");
}

void RtxptGI::compile_spatial() noexcept {
    // This policy is host-owned; generate only the selected mode.
    const uint bias_correction = construct_param().rtxpt_bias_correction;
    OC_INFO_FORMAT("RTXPT GI compiled bias correction: spatial mode={}", bias_correction);
    Kernel kernel = [&](Var<GIParam> param, Uint frame, Uint2 resolution) {
        Function::current()->require_ray_query_compute_workgroups();
        Uint2 pixel = dispatch_idx().xy();
        $if(any(pixel >= resolution)) { $return(); };
        Uint index = pixel.x + pixel.y * resolution.x;
        initialize(frame, pixel);
        Uint rng_state = rtxpt_gi::resampling_seed(pixel, resolution, frame, 6u);
        auto surface = read_pt_surface(index, frame);
        auto input = read(index, 0u);
        RtxptGIReservoirVar result;
        RtxptGIStorageVar packed_result;
        $if(surface.valid) {
            Interaction it = surface.it;
            auto resample = [&]() {
                Float selected_target = target(surface, input);
                $if(input.M != 0u) { (void)rtxpt_gi::combine(result, input, .5f, selected_target); };
                Uint mask = 0u;
                Int selected = -1;
                Uint start = cast<uint>(rtxpt_gi::resampling_next(rng_state) * 8191.f);
                Uint count = select(param.spatial != 0u, min(param.N, 32u), 0u);
                auto neighbor_pixel = [&](const Uint &i) {
                    Float2 offset = neighbors_.read((start + i) & 8191u) * param.s_radius;
                    return clamp(make_int2(pixel) + make_int2(offset), make_int2(0), make_int2(resolution) - 1);
                };
                $for(i, count) {
                    Int2 p = neighbor_pixel(i);
                    Uint n = cast<uint>(p.x) + cast<uint>(p.y) * resolution.x;
                    auto neighbor_surface = read_pt_surface(n, frame);
                    $if(!similar(surface, neighbor_surface, param.s_dot, param.s_depth, surface.depth)) { $continue; };
                    auto neighbor = read(n, 0u);
                    $if(neighbor.M == 0u) { $continue; };
                    Float J = rtxpt_gi::jacobian(rtxpt_gi::native_offset_origin(it.pos, it.ng), rtxpt_gi::native_offset_origin(neighbor_surface.it.pos, neighbor_surface.it.ng), neighbor.sample.sp->position(), neighbor.sample.sp->normal());
                    Float p_hat = target(surface, neighbor);
                    $if(!rtxpt_gi::validate_jacobian(J)) { $continue; };
                    mask |= 1u << i;
                    Bool picked = rtxpt_gi::combine(result, neighbor, rtxpt_gi::resampling_next(rng_state), p_hat * J);
                    selected = select(picked, cast<int>(i), selected);
                    selected_target = select(picked, p_hat, selected_target);
                };
                if (bias_correction >= 1u) {
                    Float pi = selected_target, sum = selected_target * cast<float>(input.M);
                    $for(i, count) {
                        $if((mask & (1u << i)) == 0u) { $continue; };
                        Int2 p = neighbor_pixel(i);
                        Uint n = cast<uint>(p.x) + cast<uint>(p.y) * resolution.x;
                        auto neighbor_surface = read_pt_surface(n, frame);
                        auto neighbor = read(n, 0u);
                        Interaction n_it = neighbor_surface.it;
                        Float p_hat = target(neighbor_surface, result);
                        if (bias_correction == 3u) {
                            $if(p_hat > 0.f) {
                                p_hat *= cast<float>(!rtxpt_gi::native_occluded(n_it, result.sample.sp->position(), rtxpt_materials(), sampled_wavelengths()));
                            };
                        }
                        pi = select(selected == cast<int>(i), p_hat, pi);
                        sum += p_hat * cast<float>(neighbor.M);
                    };
                    rtxpt_gi::finalize(result, pi, sum * selected_target);
                } else {
                    rtxpt_gi::finalize(result, 1.f, selected_target * cast<float>(result.M));
                }
            };
            resample();
        };
        auto history = pipeline()->bindless_array().buffer_var<RtxptGIStorage>(reservoirs_.index_var() + 1u);
        history.write(index, rtxpt_gi::pack(result));
    };
    kernel.function()->set_block_dim(8u, 8u, 1u);
    spatial_shader_ = device().compile(kernel, "RTXPT GI spatial resampling");
}

void RtxptGI::compile_final() noexcept {
    const char *signal_override = std::getenv("VISION_GI_SIGNAL_DIAGNOSTIC");
    const string_view signal_mode = signal_override && *signal_override ? string_view{signal_override} : string_view{"full"};
    OC_ERROR_IF(signal_mode != "full" && signal_mode != "half-mis" && signal_mode != "world-miss" &&
                signal_mode != "half-mis-world-miss" && signal_mode != "mis-distance-only", "Unknown GI signal diagnostic.");
    const bool half_mis = signal_mode == "half-mis" || signal_mode == "half-mis-world-miss";
    const bool mis_distance_only = signal_mode == "mis-distance-only";
    OC_INFO_FORMAT("GI signal final diagnostic: mode={}, MIS={}, fresh_guard={}, distance={}", signal_mode,
                   half_mis ? "legacy half" : "native directional cubic", half_mis ? "M nonzero" : "positive weight",
                   half_mis || mis_distance_only ? "legacy half with M guard" : "native directional cubic");
    const bool native_reblur = native_reblur_enabled();
    const bool lobes = !native_reblur && integrator()->denoiser() && integrator()->denoiser()->requires_lobe_signals();
    Kernel kernel = [&](Var<GIParam> param, Uint frame, Uint2 resolution) {
        Function::current()->require_ray_query_compute_workgroups();
        Uint2 pixel = dispatch_idx().xy();
        $if(any(pixel >= resolution)) { $return(); };
        Uint index = pixel.x + pixel.y * resolution.x;
        initialize(frame, pixel);
        auto surface = read_pt_surface(index, frame);
        Float3 radiance = rtxpt_bypass_.read(index).xyz(), specular = radiance;
        Float hit_distance = 0.f;
        $if(surface.valid) {
            const auto &it = surface.it;
            auto selected = read(index, 1u);
            auto fresh = candidate(index, it);
            $if(selected.M != 0u && length_squared(selected.sample.sp->position() - it.pos) > 0.f) {
                Float3 direction = normalize(selected.sample.sp->position() - it.pos);
                hit_distance = length(selected.sample.sp->position() - it.pos);
                auto evaluated = surface.evaluate(direction, sampled_wavelengths());
                Float3 f = evaluated.diffuse + evaluated.specular;
                Float3 li = selected.sample.Lo.as_vec3() * selected.weight;
                $if(param.rtxpt_final_visibility != 0u) {
                    li *= cast<float>(!rtxpt_gi::native_occluded(it, selected.sample.sp->position(), rtxpt_materials(), sampled_wavelengths()));
                };
                Float selected_weight = 1.f, initial_weight = 0.f;
                Float3 fresh_diffuse = make_float3(0.f), fresh_specular = make_float3(0.f);
                Float initial_distance = 0.f;
                $if(param.rtxpt_final_mis != 0u) {
                    // RTXPT GIFinalShading partitions each sampled direction
                    // using true/roughened BRDF response, never proposal PDFs.
                    if (half_mis) {
                        selected_weight = .5f;
                    } else {
                        Float3 rough_f = surface.f_roughness_clamp(.2f, direction, sampled_wavelengths());
                        selected_weight = 1.f - rtxpt_gi::mis_initial_weight(f, rough_f);
                    }
                    Bool fresh_valid = half_mis ? fresh.M != 0u : fresh.weight > 0.f;
                    $if(fresh_valid && length_squared(fresh.sample.sp->position() - it.pos) > 0.f) {
                        Float3 fresh_direction = normalize(fresh.sample.sp->position() - it.pos);
                        initial_distance = length(fresh.sample.sp->position() - it.pos);
                        auto fresh_evaluated = surface.evaluate(fresh_direction, sampled_wavelengths());
                        Float3 fresh_f = fresh_evaluated.diffuse + fresh_evaluated.specular;
                        fresh_diffuse = fresh_evaluated.diffuse;
                        fresh_specular = fresh_evaluated.specular;
                        if (half_mis) {
                            initial_weight = .5f;
                        } else {
                            Float3 fresh_rough_f = surface.f_roughness_clamp(.2f, fresh_direction, sampled_wavelengths());
                            initial_weight = rtxpt_gi::mis_initial_weight(fresh_f, fresh_rough_f);
                        }
                    };
                };
                Float3 fresh_li = fresh.sample.Lo.as_vec3() * fresh.weight * initial_weight;
                Float3 diffuse_result = evaluated.diffuse * li * selected_weight + fresh_diffuse * fresh_li;
                Float3 specular_result = evaluated.specular * li * selected_weight + fresh_specular * fresh_li;
                Float3 prefix = rtxpt_prefix_aux_.read(index).prefix_throughput_depth.xyz();
                diffuse_result *= prefix;
                specular_result *= prefix;
                Bool invalid = any(ocarina::isnan(diffuse_result)) || any(ocarina::isinf(diffuse_result)) ||
                               any(ocarina::isnan(specular_result)) || any(ocarina::isinf(specular_result));
                diffuse_result = select(invalid, make_float3(0.f), diffuse_result);
                specular_result = select(invalid, make_float3(0.f), specular_result);
                // Receiver-independent Lo receives the current prefix once.
                radiance += diffuse_result + specular_result;
                if (lobes) specular += specular_result;
                if (mis_distance_only) {
                    // GI diagnostic: retain native RGB weights and replace only
                    // the final distance by the exact previous half-MIS rule.
                    $if(param.rtxpt_final_mis != 0u) {
                        Float legacy_initial_distance = 0.f;
                        $if(fresh.M != 0u && length_squared(fresh.sample.sp->position() - it.pos) > 0.f) {
                            legacy_initial_distance = length(fresh.sample.sp->position() - it.pos);
                        };
                        hit_distance = .5f * hit_distance + .5f * legacy_initial_distance;
                    };
                } else {
                    hit_distance = hit_distance * selected_weight + initial_distance * initial_weight;
                }
                if (native_reblur) {
                    const auto output = native_reblur_storage();
                    Uint plane = rtxpt_prefix_aux_.read(index).metadata.y;
                    $if(plane < 3u) {
                        Float4 d = rtxpt_gi::native_reblur_commit(output.read_diffuse(index, plane), diffuse_result, hit_distance);
                        Float4 s = rtxpt_gi::native_reblur_commit(output.read_specular(index, plane), specular_result, hit_distance);
                        output.write(index, plane, rtxpt_gi::native_reblur_half(d), rtxpt_gi::native_reblur_half(s));
                    };
                }
            };
        };
        write_radiance(index, make_float4(radiance, 1.f));
        if (lobes) {
            $if(param.precise_lobes != 0u) {
                Float4 direct = make_float4(0.f), di_specular = make_float4(0.f);
                Float3 bypass = make_float3(0.f);
                $if(param.denoiser_di_active != 0u) {
                    direct = frame_buffer().denoiser_di_specular().read(index);
                    di_specular = make_float4(frame_buffer().denoiser_gi_specular().read(index).xyz(), direct.w);
                    bypass = frame_buffer().denoiser_di_bypass().read(index).xyz();
                };
                auto merged = denoiser_signals::merge_restir_lobes(direct.xyz(), radiance, di_specular,
                    make_float4(specular, hit_distance), bypass, param.denoiser_di_factor,
                    param.denoiser_gi_factor, make_float4(0.f), make_float4(0.f), make_float4(0.f));
                frame_buffer().denoiser_diffuse().write(index, merged.diffuse);
                frame_buffer().denoiser_specular().write(index, merged.specular);
                frame_buffer().denoiser_bypass().write(index, merged.bypass);
                frame_buffer().denoiser_di_specular().write(index, di_specular);
            };
            frame_buffer().denoiser_gi_specular().write(index, make_float4(specular, hit_distance));
        }
    };
    kernel.function()->set_block_dim(8u, 8u, 1u);
    final_shader_ = device().compile(kernel, "RTXPT GI final shading");
}

void RtxptGI::reset_history() noexcept {
    previous_frame_ = InvalidUI32;
    previous_jitter_ = InvalidUI32;
}

void RtxptGI::resize_reuse_buffers(uint2 resolution) noexcept {
    extent_ = resolution;
    const uint count = resolution.x * resolution.y;
    const bool registered = reservoirs_.has_registered();
    reservoirs_.super() = device().create_buffer<RtxptGIStorage>(count * 2u, "RTXPT GI SDK32 reservoirs x2");
    reservoirs_.register_self(0u, count);
    if (registered) reservoirs_.register_view_index(1u, count, count);
    else reservoirs_.register_view(count, count);
    vector<RtxptGIStorage> empty(count * 2u);
    reservoirs_.upload_immediately(empty.data());
    const bool surface_registered = surfaces_.has_registered();
    surfaces_.super() = device().create_buffer<RtxptGIPTSurface>(count * 2u, "RTXPT GI Principled receiver96 x2");
    surfaces_.register_self(0u, count);
    if (surface_registered) surfaces_.register_view_index(1u, count, count);
    else surfaces_.register_view(count, count);
    vector<RtxptGIPTSurface> empty_surfaces(count * 2u);
    surfaces_.upload_immediately(empty_surfaces.data());
    const char *validation = std::getenv("VISION_GI_PT_VALIDATE_OUTPUT");
    if (validation && *validation) {
        references_.super() = device().create_buffer<RtxptGIPTReference>(count, "GI validation original Interaction only");
        references_.register_self();
    }
    reset_history();
}

void RtxptGI::prepare_reuse_buffers() noexcept {
    const uint2 resolution = pipeline()->resolution();
    if (!reservoirs_.has_registered() || extent_.x != resolution.x || extent_.y != resolution.y)
        resize_reuse_buffers(resolution);
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

void RtxptGI::compile_pt_validation() noexcept {
    pt_validation_pending_ = false;
    const char *output = std::getenv("VISION_GI_PT_VALIDATE_OUTPUT");
    if (!output || !*output) return;
    constexpr uint samples = 2048u;
    MaterialDesc fixture_desc;
    fixture_desc.init(ParameterSet(DataWrap::parse(R"({"type":"principled_bsdf","param":{
        "color":[0.61,0.37,0.19],"roughness":0.43,"metallic":0.23,"ior":1.53,
        "anisotropic":0.41,"spec_tint":[0.81,0.93,0.67],
        "sheen_weight":0.17,"sheen_roughness":0.48,"sheen_tint":[0.73,0.31,0.59],
        "coat_weight":0.21,"coat_roughness":0.37,"coat_ior":1.47,"coat_tint":[0.83,0.71,0.63],
        "transmission_weight":0.29}})")));
    auto fixture = Material::create_root(fixture_desc);
    fixture->prepare();
    pt_validation_output_ = device().create_buffer<float4>(samples, "GI packed Principled numerical validation only");
    Kernel kernel = [&](Uint frame) {
        Uint index = (dispatch_id() * 15485863u) % (extent_.x * extent_.y);
        Uint2 pixel = make_uint2(index % extent_.x, index / extent_.x);
        initialize(frame, pixel);
        auto surface = read_pt_surface(index, frame);
        auto reference = pipeline()->bindless_array().buffer_var<RtxptGIPTReference>(references_.index_var());
        auto reference_surface = rtxpt_gi::unpack_pt_reference(reference.read(index));
        Float4 result = make_float4(0.f);
        $if(surface.valid) {
            auto original = geometry().compute_surface_interaction(reference_surface.hit, true);
            original.wo = reference_surface.it.wo;
            Float3 f0 = make_float3(0.f), f1 = make_float3(0.f);
            Float p0 = 0.f, p1 = 0.f, f_error = 0.f, p_error = 0.f;
            Bool finite = true;
            Float3 sampled = rtxpt_proposals_.read(index).wi.as_vec3();
            std::array<Float3, 3> directions{normalize(surface.normal + make_float3(.17f, .23f, .31f)),
                -surface.normal, select(length_squared(sampled) > 0.f, sampled, surface.normal)};
            for (uint i = 0u; i < directions.size(); ++i) {
                scene().materials().dispatch(original.material_id(), [&](const Material *material) {
                    auto bsdf = material->create_evaluator(original, sampled_wavelengths());
                    auto e = bsdf.evaluate(original.wo, directions[i], MaterialEvalMode::All);
                    f0 = spectrum()->linear_srgb(e.f, sampled_wavelengths()); p0 = e.pdf();
                });
                {
                    gi_principled::CompilationScope decode{true};
                    surface.load_parameters();
                    scene().materials().dispatch(surface.it.material_id(), [&](const Material *material) {
                        auto bsdf = material->create_evaluator(surface.it, sampled_wavelengths());
                        auto e = bsdf.evaluate(surface.it.wo, directions[i], MaterialEvalMode::All);
                        f1 = spectrum()->linear_srgb(e.f, sampled_wavelengths()); p1 = e.pdf();
                    });
                }
                Float3 error = abs(f0 - f1) / (make_float3(1.f) + abs(f0));
                f_error = max(f_error, max(error.x, max(error.y, error.z)));
                p_error = max(p_error, abs(p0 - p1) / (1.f + abs(p0)));
                finite &= !any(ocarina::isnan(f0)) && !any(ocarina::isinf(f0)) &&
                          !any(ocarina::isnan(f1)) && !any(ocarina::isinf(f1)) &&
                          !ocarina::isnan(p0) && !ocarina::isinf(p0) && !ocarina::isnan(p1) && !ocarina::isinf(p1);
            }
            result = make_float4(f_error, p_error, select(finite, 1.f, 0.f), 1.f);
        };
        // Last32 rows exercise every child independently, including sheen,
        // coat, anisotropy, F82 tint and transmission absent in many scenes.
        $if(dispatch_id() >= 2016u) {
            rtxpt_gi::PTSurfaceView test;
            test.it.pos = make_float3(0.f); test.depth = 1.f; test.plane_hash = 1u;
            test.it.ng = make_float3(0.f, 0.f, 1.f);
            test.it.shading.x = make_float3(1.f, 0.f, 0.f);
            test.it.shading.y = make_float3(0.f, 1.f, 0.f);
            test.it.shading.z = test.it.ng;
            Float x = (cast<float>(dispatch_id() - 2016u) - 15.5f) / 32.f;
            test.it.wo = normalize(make_float3(x, .27f, .8f));
            Float3 wi = normalize(make_float3(-.21f, x, select((dispatch_id() & 1u) != 0u, -.75f, .75f)));
            std::array<Float4, 6> expected{make_float4(0.f),make_float4(0.f),make_float4(0.f),
                                         make_float4(0.f),make_float4(0.f),make_float4(0.f)};
            {
                gi_principled::CompilationScope capture{false};
                auto bsdf = fixture->create_evaluator(test.it, sampled_wavelengths());
                for (uint i = 0u; i < 6u; ++i) test.parameters[i] = gi_principled::row(i);
                test.valid = gi_principled::captured(test.it.material_id());
                bsdf.dispatch([&](const Lobe *root) {
                    const auto *set = root->prepared_lobe_set();
                    OC_ASSERT(set && set->lobe_num() == 6u);
                    set->for_each([&](const WeightedLobe &lobe, uint i) {
                        auto e = lobe->evaluate(test.it.wo, wi, MaterialEvalMode::All, BxDFFlag::All, Radiance);
                        expected[i] = make_float4(e.f.vec3() * lobe.weight(), e.pdf() * lobe.sample_weight());
                    });
                });
            }
            auto decoded = rtxpt_gi::unpack_pt_surface(rtxpt_gi::pack_pt_surface(test));
            Float fe = 0.f, pe = 0.f; Bool finite = decoded.valid;
            Uint mask = 0u;
            {
                gi_principled::CompilationScope decode{true};
                decoded.load_parameters();
                auto bsdf = fixture->create_evaluator(decoded.it, sampled_wavelengths());
                bsdf.dispatch([&](const Lobe *root) {
                    root->prepared_lobe_set()->for_each([&](const WeightedLobe &lobe, uint i) {
                        auto e = lobe->evaluate(decoded.it.wo, wi, MaterialEvalMode::All, BxDFFlag::All, Radiance);
                        Float4 actual = make_float4(e.f.vec3() * lobe.weight(), e.pdf() * lobe.sample_weight());
                        Float4 error = abs(actual - expected[i]) / (1.f + abs(expected[i]));
                        fe = max(fe, max(error.x, max(error.y, error.z))); pe = max(pe, error.w);
                        finite &= !any(ocarina::isnan(actual)) && !any(ocarina::isinf(actual));
                        mask |= 1u << i;
                    });
                });
            }
            result = make_float4(fe, pe, select(finite, 1.f, 0.f), -cast<float>(mask));
        };
        pt_validation_output_.write(dispatch_id(), result);
    };
    pt_validation_ = device().compile(kernel, "GI compressed96B Principled six-lobe numerical validation only");
    pt_validation_pending_ = true;
}

CommandBatch RtxptGI::dispatch_reuse(uint frame, GIParam param) const noexcept {
    const bool continuous = previous_frame_ != InvalidUI32 && previous_frame_ + 1u == frame;
    const bool same_lattice = previous_jitter_ == param.camera_jitter;
    param.temporal = param.temporal && frame != 0u && continuous && same_lattice;
    previous_frame_ = frame;
    previous_jitter_ = param.camera_jitter;
    uint2 padded = make_uint2((extent_.x + 7u) & ~7u, (extent_.y + 7u) & ~7u);
    CommandBatch batch;
    const char *stage_profile = std::getenv("VISION_STAGE_PROFILE");
    const bool stage_sync = stage_profile && string_view(stage_profile) == "1";
    batch << temporal_shader_(param, frame, extent_).dispatch(padded);
    if (stage_sync) batch << synchronize();
    batch << spatial_shader_(param, frame, extent_).dispatch(padded);
    if (stage_sync) batch << synchronize();
    batch << final_shader_(param, frame, extent_).dispatch(padded);
    if (pt_validation_pending_ && frame >= 2u) {
        pt_validation_pending_ = false;
        auto host = std::make_shared<vector<float4>>(2048u);
        const std::filesystem::path output{std::getenv("VISION_GI_PT_VALIDATE_OUTPUT")};
        batch << pt_validation_(frame).dispatch(2048u)
              << pt_validation_output_.download(host->data()) << synchronize();
        batch << [host, output, frame] {
            float f_error = 0.f, p_error = 0.f, fixture_f = 0.f, fixture_p = 0.f;
            uint valid = 0u, nonfinite = 0u, fixtures = 0u, lobe_mask = 0u;
            for (auto value : *host) {
                if (value.w == 0.f) continue;
                if (value.z == 0.f || !std::isfinite(value.x) || !std::isfinite(value.y)) ++nonfinite;
                if (value.w < 0.f) {
                    ++fixtures; lobe_mask |= uint(-value.w);
                    fixture_f = std::max(fixture_f, value.x); fixture_p = std::max(fixture_p, value.y);
                } else {
                    ++valid;
                    f_error = std::max(f_error, value.x); p_error = std::max(p_error, value.y);
                }
            }
            bool passed = valid > 0u && nonfinite == 0u && fixtures == 32u && lobe_mask == 63u &&
                          fixture_f <= .05f && fixture_p <= .05f;
            std::ofstream file{output};
            file << std::setprecision(9) << "{\n\"passed\":" << (passed ? "true" : "false")
                 << ",\n\"sampled_pixels\":2048,\n\"valid_receivers\":" << valid
                 << ",\n\"directions_per_receiver\":3,\n\"frame\":" << frame
                 << ",\n\"nonfinite_receivers\":" << nonfinite
                 << ",\n\"maximum_relative_F_error\":" << f_error
                 << ",\n\"maximum_relative_PDF_error\":" << p_error
                 << ",\n\"receiver_bytes\":" << sizeof(RtxptGIPTSurface)
                 << ",\n\"material_parameter_bytes\":36,\n\"fixture_cases\":" << fixtures
                 << ",\n\"fixture_lobe_mask\":" << lobe_mask
                 << ",\n\"fixture_maximum_relative_F_error\":" << fixture_f
                 << ",\n\"fixture_maximum_relative_PDF_error\":" << fixture_p
                 << ",\n\"lobe_coverage\":[\"Sheen\",\"Coat\",\"Metallic\",\"Trans\",\"Spec\",\"Diffuse\"]"
                 << ",\n\"scope\":\"Actual GPU compressed Principled cache versus original complete BSDF; scene quantization error recorded,32 moderate-direction fixtures independently check each lobe within5%; diagnostic timings excluded\"\n}\n";
        };
    }
    return batch;
}

void RtxptGI::download_storage(vector<GISample> &samples, vector<GIReservoir> &reservoirs) const noexcept {
    const uint count = extent_.x * extent_.y;
    samples.resize(count);
    vector<RtxptGIStorage> packed(count * 2u);
    pipeline()->stream() << samples_.view().download(samples.data())
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

RtxptGI::RtxptGI(IntegratorPtr integrator,
                   const vision::ParameterSet &desc)
    : ReSTIR(integrator, desc) {
    const char *override_backend = std::getenv("VISION_RESTIR_GI_BACKEND");
    const string backend = override_backend && *override_backend ? override_backend : desc["backend"].as_string("rtxpt");
    if (backend == "legacy") {
        OC_WARNING("The legacy ReSTIR GI backend has been removed; using RTXPT GI.");
    } else if (backend != "rtxpt") {
        unsupported_backend_ = "Unsupported ReSTIR GI backend '" + backend + "'. Expected rtxpt.";
    }
    // Two GI neighbors balance reuse quality and the cost of support queries.
    spatial_.sample_num = spatial_.open ? desc["spatial"]["sample_num"].as_uint(2u) : 1u;
    spatial_.sampling_radius = desc["spatial"]["radius"].as_float(12.f);
}

void RtxptGI::compile(bool refresh_lighting) noexcept {
    switch_profile::Scope profile{"ReSTIR.GI.compile", "compile"};
    backend_error_.clear();
    if (!unsupported_backend_.empty()) {
        backend_error_ = unsupported_backend_;
        return;
    }
    if (scene().process_mediums() || spectrum()->dimension() != 3u) {
        backend_error_ = scene().process_mediums()
            ? "RTXPT GI does not support participating media" : "RTXPT GI requires an RGB spectrum";
        return;
    }
    if (native_reblur_enabled_) prepare_native_reblur_buffers();
    if (!rtxpt_materials_) rtxpt_materials_ = make_shared<rtxpt_gi::StandardMaterialInputs>();
    rtxpt_materials_->prepare();
    // A whole-pipeline compile can follow light or topology replacement.
    // Rebuild the native emitter mapping after the scene is rebound.
    if (refresh_lighting || !rtxpt_lights_) {
        if (!rtxpt_lights_) rtxpt_lights_ = make_shared<rtxpt_gi::NativeLightSampler>(*this);
        try {
            rtxpt_lights_->prepare();
        } catch (const std::runtime_error &error) {
            backend_error_ = error.what();
            rtxpt_lights_.reset();
            return;
        }
        prepare_reuse_buffers();
        // These buffers register bindless views after the pipeline's prepare-time upload.
        pipeline()->upload_bindless_array();
    }
    compile_initial_samples();
    OC_INFO("GI receiver evaluation: full Principled six-lobe MaterialEvaluator, packed96B receiver,36B sampled numeric parameters");
    compile_temporal();
    compile_spatial();
    compile_final();
    compile_pt_validation();
    OC_INFO("RTXPT ReSTIR GI active: shared ordinary PT, complete BSDF, private proposals, packed32 reservoirs, temporal/spatial reuse");
}

namespace {
void gi_signal_switch_plane(PTPathCapture &capture, Uint &current_plane,
                            const Uint &plane, const Uint &bounce) {
    current_plane = plane;
    capture.signal_bounce = bounce;
    capture.signal_hit_distance = 0.f;
    // The global delta-only flag survives plane switches, as in RTXPT.
}

void gi_signal_commit(Float4 &diffuse, Float4 &specular, const Float3 &secondary,
                       const Float &hit_distance, const Bool &base_diffuse) {
    // Native CommitDenoiserRadiance merges the whole secondaryL once, using
    // the final sampleHitT even when early delta suffix vertices emitted.
    $if(base_diffuse) {
        diffuse = rtxpt_gi::native_reblur_commit(diffuse, secondary, hit_distance);
    } $else {
        specular = rtxpt_gi::native_reblur_commit(specular, secondary, hit_distance);
    };
}

void gi_signal_commit_visit(Float4 &committed_diffuse, Float4 &committed_specular,
                             Float4 &base_diffuse, Float4 &base_specular, Float3 &secondary,
                             const Float &hit_distance, const Bool &scatter_diffuse) {
    gi_signal_commit(base_diffuse, base_specular, secondary, hit_distance, scatter_diffuse);
    committed_diffuse = rtxpt_gi::native_reblur_commit(committed_diffuse, Float3{base_diffuse.xyz()}, base_diffuse.w);
    committed_specular = rtxpt_gi::native_reblur_commit(committed_specular, Float3{base_specular.xyz()}, base_specular.w);
    base_diffuse = make_float4(0.f); base_specular = make_float4(0.f); secondary = make_float3(0.f);
}

void gi_signal_commit_buffer_visit(const rtxpt_gi::NativeReblurStorage &storage,
                                   const Uint &pixel, const Uint &plane,
                                   Float4 &base_diffuse, Float4 &base_specular, Float3 &secondary,
                                   const Float &hit_distance, const Bool &scatter_diffuse) {
    Float4 diffuse = storage.read_diffuse(pixel, plane);
    Float4 specular = storage.read_specular(pixel, plane);
    gi_signal_commit_visit(diffuse, specular, base_diffuse, base_specular, secondary, hit_distance, scatter_diffuse);
    // Keep the existing float32 accumulation. Native packs FP16 here, but that
    // would change the signal values and confound this retained-state test.
    storage.write(pixel, plane, diffuse, specular);
}

}// namespace

bool RtxptGI::render_UI(Widgets *widgets) noexcept {
    return widgets->use_tree("ReSTIR GI", [&] {
        changed_ |= widgets->check_box("switch", &open_);
        if (open_) {
            render_sub_UI(widgets);
        }
    });
}

void RtxptGI::render_sub_UI(Widgets *widgets) noexcept {
    changed_ |= widgets->check_box("temporal", &temporal_.open);
    changed_ |= widgets->drag_uint("max age", &max_age_, 1, 0, 100);
    changed_ |= widgets->drag_float("diffuse factor threshold", &diff_factor_, 0, 1);
    if (temporal_.open) {
        changed_ |= widgets->input_uint_limit("history", &temporal_.limit, 0, 50, 1, 3);
        changed_ |= widgets->input_float_limit("temporal theta",
                                               &temporal_.theta, 0, 90, 1, 1);
        changed_ |= widgets->input_float_limit("temporal depth", &temporal_.depth_threshold,
                                               0, 1, 0.02, 0.1);
        changed_ |= widgets->input_float_limit("temporal radius", &temporal_.sampling_radius,
                                               0, 50, 1, 5);
    }
    changed_ |= widgets->check_box("spatial", &spatial_.open);
    if (spatial_.open) {
        changed_ |= widgets->input_float_limit("spatial theta",
                                               &spatial_.theta, 0, 90, 1, 1);
        changed_ |= widgets->input_float_limit("spatial depth", &spatial_.depth_threshold,
                                               0, 1, 0.02, 0.1);
        changed_ |= widgets->input_float_limit("spatial radius", &spatial_.sampling_radius,
                                               0, 50, 1, 5);
    }
}

void RtxptGI::compile_initial_samples() noexcept {
    switch_profile::Scope profile{"GI.initial.compile", "compile"};
    const char *nee_diagnostic = std::getenv("VISION_GI_NEE_DIAGNOSTIC");
    OC_ERROR_IF(nee_diagnostic && string_view(nee_diagnostic) != "1x1",
                "Standard RTXPT GI currently supports NEE 1x1 only.");
    OC_ERROR_IF(scene().process_mediums(), "Standard RTXPT GI does not support participating media.");
    OC_ERROR_IF(spectrum()->dimension() != 3u, "Standard RTXPT GI requires RGB rendering.");
    OC_INFO("GI NEE diagnostic: 1x1, candidates=1, full_samples=1, visibility=true");
    OC_INFO("GI candidate transport: shared ordinary PT, full MaterialEvaluator, camera_path=true, single_path=true, exports_only=true, NEE=1/1");
    OC_INFO_FORMAT("ReSTIR GI candidate transport: backend=rtxpt, native_transport=false, candidates_per_pixel=1, NEE_candidates_per_shadow=1, NEE_shadow_samples=1, RR=ordinary immediate, max_surface_depth={}, suffix_depth={}, terminal_endpoint=ordinary PT, BSDF=complete Corona",
                   integrator()->suffix_depth() + 1u, integrator()->suffix_depth());
    OC_INFO("RTXPT GI private stable planes: count=3, max_vertex_depth=5, primary_replacement=true, incident_Lo=unweighted, shared_Falcor_primary=true");
    OC_INFO("RTXPT GI glass policy: complete pure RGB dielectric, roughness^2<0.0064 ideal delta, exact Fresnel, thick sqrt tint per interface, occupied-slot transmission continuation; ordinary PT/Falcor DI unchanged");
    OC_INFO("RTXPT GI stable emission: continuous first reflection owned by DI; first transmission and uncovered delta reflection retained by PT/Build exactly once");
    // Both private passes consume the same primary flag from the active DI producer.
    Kernel build = [&](Uint frame_index, BufferVar<uint> flags) {
        gi_principled::CompilationScope parameters{false};
        initial(sampler(), frame_index, spectrum());
        scene().sensor()->load_data();
        sampler()->load_data();
        integrator()->load_data();
        rtxpt_gi::build_pt_prefix(prefix_storage(), *this, flags.read(dispatch_id()));
    };
    build.function()->set_block_dim(8u, 8u, 1u);
    rtxpt_prefix_build_ = device().compile(build, "RTXPT GI stable plane build");
    const bool count_work = path_cost::enabled();
    const char *cost_override = std::getenv("VISION_GI_EXPORT_COST");
    const string_view cost_mode = cost_override && *cost_override ? string_view{cost_override} : string_view{"full"};
    const array<string_view, 9> cost_modes{"full", "no-distance", "no-aux", "no-guides", "no-surface",
                                         "no-endpoint", "no-radiance", "no-observer", "plain"};
    uint cost_rank = 0u;
    if (cost_mode == "zero-surface") cost_rank = 3u;
    else if (cost_mode == "zero-material") cost_rank = 0u;
    else if (cost_mode == "plain-output48" || cost_mode == "plain-no-bridge" ||
             cost_mode == "plain-output48-no-bridge") cost_rank = 8u;
    else if (cost_mode != "no-distance-math") {
        while (cost_rank < cost_modes.size() && cost_modes[cost_rank] != cost_mode) ++cost_rank;
        OC_ERROR_IF(cost_rank == cost_modes.size(), "Unknown GI export cost diagnostic.");
    }
    const bool skip_distance = cost_rank >= 1u, skip_aux = cost_rank >= 2u, skip_guides = cost_rank >= 3u;
    const bool skip_surface = cost_rank >= 4u, skip_endpoint = cost_rank >= 5u;
    const bool skip_radiance = cost_rank >= 6u, skip_observer = cost_rank >= 7u;
    const bool no_parameter_bridge = cost_mode == "plain-no-bridge" || cost_mode == "plain-output48-no-bridge";
    OC_INFO_FORMAT("GI export cost mode={}, production={}", cost_mode, cost_mode == "full");
    const char *signal_cost_override = std::getenv("VISION_GI_SIGNAL_COST");
    const string_view signal_cost_mode = signal_cost_override && *signal_cost_override
        ? string_view{signal_cost_override} : string_view{"optimized-buffer"};
    const bool reuse_signal_distance = signal_cost_mode == "reuse-distance" || signal_cost_mode == "optimized" ||
                                       signal_cost_mode == "optimized-buffer" || signal_cost_mode == "optimized-native";
    const bool stop_leaf_match = signal_cost_mode == "stop-leaf-match" || signal_cost_mode == "optimized" ||
                                 signal_cost_mode == "optimized-buffer" || signal_cost_mode == "optimized-native";
    const bool skip_signal_direct_fraction = signal_cost_mode == "skip-direct-fraction";
    const bool skip_signal_commit = signal_cost_mode == "no-commit";
    const bool buffer_signal_commit = signal_cost_mode == "buffer-commit" || signal_cost_mode == "optimized-buffer" ||
                                      signal_cost_mode == "optimized-native";
    const bool skip_dominant_signal_suffix = signal_cost_mode == "skip-dominant-suffix" ||
                                             signal_cost_mode == "optimized-native";
    const bool stop_zero_signal_branch = signal_cost_mode == "optimized-buffer" || signal_cost_mode == "optimized-native";
    OC_ERROR_IF(signal_cost_mode != "full" && !reuse_signal_distance && !stop_leaf_match &&
                !skip_signal_direct_fraction && !skip_signal_commit && !buffer_signal_commit && !skip_dominant_signal_suffix,
                "Unknown GI signal cost diagnostic.");
    const char *signal_override = std::getenv("VISION_GI_SIGNAL_DIAGNOSTIC");
    const string_view signal_mode = signal_override && *signal_override ? string_view{signal_override} : string_view{"full"};
    const bool native_signal_miss = signal_mode != "world-miss" && signal_mode != "half-mis-world-miss";
    OC_ERROR_IF(signal_mode != "full" && signal_mode != "half-mis" && signal_mode != "mis-distance-only" && native_signal_miss,
                "Unknown GI signal diagnostic.");
    OC_INFO_FORMAT("GI signal cost mode={}, reuse_distance={}, stop_leaf_match={}, direct_fraction={}, visit_commit={}, buffer_commit={}, dominant_suffix={}, stop_zero_branch={}",
                   signal_cost_mode, reuse_signal_distance, stop_leaf_match,
                   !skip_signal_direct_fraction, !skip_signal_commit, buffer_signal_commit,
                   !skip_dominant_signal_suffix, stop_zero_signal_branch);
    OC_INFO_FORMAT("GI signal diagnostic mode={}, native_miss={}", signal_mode, native_signal_miss);
    const char *endpoint_normal_override = std::getenv("VISION_GI_ENDPOINT_NORMAL");
    const string_view endpoint_normal_mode = endpoint_normal_override && *endpoint_normal_override
        ? string_view{endpoint_normal_override} : string_view{"shading-facing"};
    OC_ERROR_IF(endpoint_normal_mode != "geometric" && endpoint_normal_mode != "geometric-facing" &&
                endpoint_normal_mode != "shading-facing", "Unknown GI endpoint normal diagnostic.");
    OC_INFO_FORMAT("GI endpoint normal diagnostic: mode={}, material_normal={}, native_grazing_adjust={}",
                   endpoint_normal_mode, endpoint_normal_mode == "shading-facing", endpoint_normal_mode == "shading-facing");
    OC_INFO_FORMAT("GI Fill transport: ordinary camera PT with observers only, counters={}", count_work);
    Kernel kernel = [&](Uint frame_index, Var<RtxptGICameraParam> camera, BufferVar<uint> flags) {
        gi_principled::CompilationScope parameters{false, !no_parameter_bridge};
        initial(sampler(), frame_index, spectrum());
        scene().sensor()->load_data(); sampler()->load_data(); integrator()->load_data();
        sampler()->set_seed(dispatch_idx().xy(), frame_index, PTParam{}.dimension_index);
        optional<path_cost::PathWorkCountersVar> work_counters;
        if (count_work) work_counters.emplace();
        if (cost_mode == "plain" || cost_mode == "plain-output48" ||
            cost_mode == "plain-no-bridge" || cost_mode == "plain-output48-no-bridge") {
            RadType3Var direct, specular;
            HitContext context{direct, specular};
            context.restir_shadow_connections = true;
            context.work_counters = work_counters ? &*work_counters : nullptr;
            RayState ray = frame_buffer().rays().read(dispatch_id())->to_ray_state();
            Float3 total = integrator()->Li(ray, Float{1e16f}, integrator()->runtime_suffix_depth() + 1u,
                spectrum()->one(), false, context, *this);
            if (cost_mode == "plain-output48" || cost_mode == "plain-output48-no-bridge") {
                rtxpt_bypass_.write(dispatch_id(), make_float4(total, 1.f));
                auto denoiser = prefix_storage().denoiser;
                OC_ASSERT(denoiser.enabled());
                denoiser.write(dispatch_id(), Uint{0u},
                    make_float4(total - make_float3(specular), 1.f), make_float4(specular, 1.f));
                if (work_counters) path_work_counters_.write(dispatch_id(), *work_counters);
                return;
            }
            rtxpt_bypass_.write(dispatch_id(), make_float4(total, 1.f));
            rtxpt_gi::CandidateTraceResult empty;
            empty.sample.Lo.set(make_float3(specular));
            samples_.write(dispatch_id(), empty.sample);
            rtxpt_proposals_.write(dispatch_id(), empty.proposal);
            if (work_counters) path_work_counters_.write(dispatch_id(), *work_counters);
            return;
        }
        PTPathCapture capture;
        capture.reflection_only_direct = true;
        capture.first_reflection_covered_by_di = !BxDFFlag::is_specular(flags.read(dispatch_id()));
        capture.record_radiance = !skip_radiance;
        capture.record_hit_distance = cost_mode != "no-distance-math";
        capture.reuse_signal_distance = reuse_signal_distance;
        capture.evaluate_signal_direct_fraction = !skip_signal_direct_fraction;
        capture.native_signal_miss = native_signal_miss;
        Bool terminal_seen = false, receiver_written = false;
        Uint signal_plane = 0u;
        Bool sampled_stable_event = false;
        optional<array<Float4, 3>> signal_diffuse, signal_specular;
        if (!buffer_signal_commit) { signal_diffuse.emplace(); signal_specular.emplace(); }
        Float4 signal_base_diffuse = make_float4(0.f), signal_base_specular = make_float4(0.f);
        Float3 signal_secondary = make_float3(0.f);
        Bool signal_scatter_diffuse = false;
        if (!buffer_signal_commit) {
            for (uint p = 0u; p < 3u; ++p) {
                (*signal_diffuse)[p] = make_float4(0.f); (*signal_specular)[p] = make_float4(0.f);
            }
        }
        Uint sampled_branch = 1u;
        Bool ideal_prefix = true;
        Float3 sampled_stable = make_float3(0.f);
        const auto storage = prefix_storage();
        if (buffer_signal_commit && storage.denoiser.enabled()) {
            // This invocation owns all three plane addresses for its
            // pixel. Initialize them before any visit reads them.
            for (uint p = 0u; p < 3u; ++p) {
                storage.denoiser.write(dispatch_id(), Uint{p}, make_float4(0.f), make_float4(0.f));
            }
        }
        Uint4 header = storage.headers.read(dispatch_id());
        Uint dominant = header.w & 3u;
        capture.on_sample = [&](const Interaction &it, const TriangleHitVar &hit,
                                const MaterialEvaluator &bsdf, const BSDFSample &sample,
                                const Uint &bounce, const SampledSpectrum &throughput) {
            $if(bounce == 0u) {
                const Float3 n = bsdf.shading_frame().normal();
                capture.first_scatter_transmission = dot(n, it.wo) * dot(n, sample.wi) < 0.f;
            };
            auto match_planes = [&] {
                $for(plane, 3u) {
                    $if(header[plane] == rtxpt_gi::PrefixInvalid || header[plane] == rtxpt_gi::PrefixQueued) { $continue; };
                    auto node = storage.read(plane);
                    $if(!gi_prefix_endpoint_matches(sampled_branch, bounce + 1u, -it.wo,
                        header[plane], rtxpt_gi::pt_prefix_depth(node), node.direction_vertex.xyz())) { $continue; };
                    Float2 bary = as<float2>(Uint2{node.hit_words.zw()});
                    $if(hit.inst_id != node.hit_words.x || hit.prim_id != node.hit_words.y ||
                        length_squared(Float2{hit.bary} - bary) > 1e-6f) { $continue; };
                    // Signals follow every visited plane, including a
                    // consumed primary and an ineligible GI receiver.
                    if (storage.denoiser.enabled() && !skip_signal_commit) {
                        for (uint p = 0u; p < 3u; ++p) {
                            $if(signal_plane == p) {
                                if (buffer_signal_commit) {
                                    gi_signal_commit_buffer_visit(storage.denoiser, dispatch_id(), Uint{p},
                                        signal_base_diffuse, signal_base_specular, signal_secondary,
                                        skip_distance ? Float{0.f} : capture.signal_hit_distance, signal_scatter_diffuse);
                                } else {
                                    gi_signal_commit_visit((*signal_diffuse)[p], (*signal_specular)[p], signal_base_diffuse,
                                        signal_base_specular, signal_secondary,
                                        skip_distance ? Float{0.f} : capture.signal_hit_distance, signal_scatter_diffuse);
                                }
                            };
                        }
                    }
                    gi_signal_switch_plane(capture, signal_plane, plane, bounce);
                    $if(!terminal_seen && !rtxpt_gi::pt_prefix_consumed(node)) {
                        terminal_seen = true;
                        $if(plane == dominant) {
                            rtxpt_gi::PTSurfaceView receiver;
                            receiver.it = it; receiver.hit = hit;
                            receiver.depth = node.virtual_position.w;
                            receiver.plane_hash = (plane << 24u) | header[plane];
                            receiver.valid = gi_principled::captured(it.material_id());
                            for (uint i = 0u; i < 6u; ++i) receiver.parameters[i] = gi_principled::row(i);
                            if (cost_mode == "zero-material") {
                                // Cost diagnostic only: preserve receiver eligibility,
                                // configuration, geometry, guides and all96B stores.
                                for (uint i = 0u; i < 5u; ++i) receiver.parameters[i] = make_float4(0.f);
                                receiver.parameters[5].xyz() = make_float3(0.f);
                            }
                            if (!skip_guides) {
                                receiver.normal = bsdf.shading_frame().normal();
                                SampledSpectrum d{spectrum()->dimension()}, sp{spectrum()->dimension()}; Float2 roughness;
                                bsdf.reuse_material(d, sp, roughness);
                                receiver.diffuse_guide = spectrum()->linear_srgb(d, sampled_wavelengths());
                                receiver.specular_guide = spectrum()->linear_srgb(sp, sampled_wavelengths());
                                receiver.roughness = roughness;
                            }
                            if (cost_mode == "zero-surface") {
                                // Keep the same receiver stores; let unused
                                // interaction payload reconstruction disappear.
                                rtxpt_gi::PTSurfaceView empty;
                                write_pt_surface(dispatch_id(), empty);
                            } else if (!skip_surface) write_pt_surface(dispatch_id(), receiver);
                            receiver_written = receiver.valid;
                            RtxptGIPrefixAuxVar aux;
                            aux.prefix_throughput_depth = make_float4(spectrum()->linear_srgb(throughput, sampled_wavelengths()), receiver.depth);
                            if (!skip_aux) {
                            aux.current_virtual_position = make_float4(node.virtual_position.xyz(), node.origin_distance.w);
                            aux.previous_virtual_position = make_float4(node.virtual_position.xyz(), 0.f);
                            aux.metadata = make_uint4(receiver.plane_hash, plane, bounce + 1u, header[plane]);
                            Float4x4 world_to_camera = make_float4x4(camera.columns[0u], camera.columns[1u], camera.columns[2u], camera.columns[3u]);
                            Float4x4 camera_to_raster = make_float4x4(camera.columns[4u], camera.columns[5u], camera.columns[6u], camera.columns[7u]);
                            Float3 current_view = transform_point(world_to_camera, aux.current_virtual_position.xyz());
                            Float3 current_raster = transform_point(camera_to_raster, current_view / current_view.z);
                            Float2 motion = current_raster.xy() - scene().sensor()->prev_raster_coord(aux.previous_virtual_position.xyz()).xy();
                            Float previous_depth = scene().sensor()->prev_linear_depth(aux.previous_virtual_position.xyz());
                            aux.packed_motion = make_uint2(f32tof16(motion.x) | (f32tof16(motion.y) << 16u), f32tof16(previous_depth - current_view.z));
                            }
                            rtxpt_prefix_aux_.write(dispatch_id(), aux);
                            Bool delta = BxDFFlag::is_specular(sample.eval.flags) || ocarina::isinf(sample.eval.pdf());
                            Float3 physical = spectrum()->linear_srgb(throughput * sample.eval.throughput(), sampled_wavelengths());
                            capture.active = receiver.valid && sample.valid() && !delta && any(physical != make_float3(0.f));
                            capture.receiver_bounce = bounce;
                            $if(capture.active) {
                                HitBSDFVar proposal;
                                proposal.wi.set(sample.wi); proposal.bsdf.set(sample.eval.f.vec3()); proposal.pdf = sample.eval.pdf();
                                rtxpt_proposals_.write(dispatch_id(), proposal);
                                GISampleVar empty; empty.sp->set_position(make_float3(0.f)); empty.sp->set_normal(make_float3(0.f)); empty.Lo.set(Float3{make_float3(0.f)});
                                samples_.write(dispatch_id(), empty);
                            };
                        };
                    };
                    $break;
                };
            };
            if (stop_leaf_match) {
                // The prefix producer stores no descendant of an unconsumed
                // endpoint. Consumed-parent visits still run before the leaf.
                if (stop_zero_signal_branch) {
                    // A zero sampled branch is absorbing and cannot match
                    // any stored endpoint at this or a later bounce.
                    $if(!terminal_seen && sampled_branch != 0u) { match_planes(); };
                } else {
                    $if(!terminal_seen) { match_planes(); };
                }
            } else {
                match_planes();
            }
            sampled_branch = gi_prefix_next_branch(sampled_branch, sample.stable_lobe_index, sample.valid());
            ideal_prefix = ideal_prefix && BxDFFlag::is_specular(sample.eval.flags) && sample.valid();
        };
        capture.on_emission = [&](const TriangleHitVar &hit, const Float3 &direction, const Uint &bounce,
                                  const Float3 &radiance, const Float3 &) {
            Bool represented = false;
            $if(gi_prefix_stable_emission_allowed(ideal_prefix, bounce + 1u, sampled_branch,
                capture.first_reflection_covered_by_di)) {
                $for(plane, 3u) {
                    $if(header[plane] == rtxpt_gi::PrefixInvalid || header[plane] == rtxpt_gi::PrefixQueued) { $continue; };
                    auto node = storage.read(plane);
                    $if(!rtxpt_gi::pt_prefix_ideal_path(node)) { $continue; };
                    Bool identity = hit->is_miss() && node.hit_words.x == InvalidUI32 && node.hit_words.y == InvalidUI32;
                    $if(hit->is_hit()) {
                        Float2 bary = as<float2>(Uint2{node.hit_words.zw()});
                        identity = hit.inst_id == node.hit_words.x && hit.prim_id == node.hit_words.y &&
                            length_squared(Float2{hit.bary} - bary) <= 1e-6f;
                    };
                    represented |= gi_prefix_emission_represented(ideal_prefix, sampled_branch, bounce + 1u,
                        direction, header[plane], rtxpt_gi::pt_prefix_depth(node), node.direction_vertex.xyz(), identity,
                        capture.first_reflection_covered_by_di);
                    $if(represented) { $break; };
                };
            };
            $if(represented) {
                sampled_stable += radiance;
            };
            sampled_stable_event = represented;
        };
        if (storage.denoiser.enabled()) {
            capture.on_signal = [&](const Float3 &radiance, const Float3 &specular,
                                     const Float &hit_distance, const Float &,
                                     const Uint &bounce, bool emission) {
                signal_scatter_diffuse = capture.signal_base_diffuse;
                // First retained light stays in the camera-plane residual,
                // not receiver Lo. Build replaces represented ideal events.
                const Bool direct_owned = gi_camera_emission_owned_by_di(capture.first_scatter_transmission,
                    capture.first_reflection_covered_by_di, bounce);
                $if(bounce > 0u && !(Bool{emission} && (direct_owned || sampled_stable_event))) {
                    $if(Bool{!emission} && bounce == capture.signal_bounce) {
                        Float distance = skip_distance ? Float{0.f} : hit_distance;
                        signal_base_diffuse = make_float4(radiance - specular, distance);
                        signal_base_specular = make_float4(specular, distance);
                    } $else {
                        if (skip_dominant_signal_suffix) {
                            // Native dominant GI writes this suffix to the
                            // candidate, rather than adding then cancelling it.
                            // Diagnostic: an invalidated candidate cannot
                            // recover suffix events already omitted here.
                            $if(!capture.active) { signal_secondary += radiance; };
                        } else {
                            signal_secondary += radiance;
                        }
                    };
                };
            };
        }
        capture.on_endpoint = [&](const Interaction &it) {
            GISampleVar sample;
            sample.sp->set(it); sample.Lo.set(Float3{make_float3(0.f)}); sample.age = 0.f;
            if (endpoint_normal_mode != "geometric") {
                Float side = select(dot(it.ng, it.wo) >= 0.f, 1.f, -1.f);
                Float3 normal = it.ng * side;
                if (endpoint_normal_mode == "shading-facing") {
                    // The endpoint callback precedes ordinary material
                    // evaluation. Read the deterministic unoriented normal;
                    // no BSDF sample, throughput, PDF or RNG is changed.
                    scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                        normal = material->gi_unoriented_shading_normal(it, sampled_wavelengths()) * side;
                    });
                    // Native Bridge::loadSurface first orients Ns by the
                    // geometric front-face test, then adjustShadingNormal
                    // blends it towards the oriented Ng below NdotV=.1.
                    Float3 geometric = it.ng * side;
                    Float sign_n = select(dot(normal, geometric) >= 0.f, 1.f, -1.f);
                    Float3 oriented = normal * sign_n;
                    Float cosine = dot(it.wo, oriented);
                    $if(cosine <= .1f) {
                        normal = sign_n * normalize(lerp(geometric, oriented, saturate(cosine * 10.f)));
                    };
                }
                sample.sp->set_normal(normal);
            }
            samples_.write(dispatch_id(), sample);
        };
        if (skip_endpoint) capture.on_endpoint = {};
        RadType3Var direct = make_RadType3(make_float3(0.f));
        HitContext context{direct};
        context.restir_shadow_connections = true;
        if (!skip_observer) context.path_capture = &capture;
        context.work_counters = work_counters ? &*work_counters : nullptr;
        context.restir_direct_split = true;
        // This is the ordinary camera PT: one Li call, original camera ray,
        // complete depth, original NEE, BSDF samples and immediate roulette.
        RayState ray = frame_buffer().rays().read(dispatch_id())->to_ray_state();
        Float3 total = integrator()->Li(ray, Float{1e16f}, integrator()->runtime_suffix_depth() + 1u,
            spectrum()->one(), false, context, *this);
        Float3 indirect = total - make_float3(direct);
        Float3 stable = make_float3(0.f);
        if (!skip_observer) {
            stable = storage.stable_radiance.read(dispatch_id()).xyz();
            indirect -= sampled_stable;
        }
        $if(capture.active) {
            auto sample = samples_.read(dispatch_id());
            sample.Lo.set(ocarina::zero_if_nan_inf(capture.radiance));
            samples_.write(dispatch_id(), sample);
            auto proposal = rtxpt_proposals_.read(dispatch_id());
            Float3 prefix = rtxpt_prefix_aux_.read(dispatch_id()).prefix_throughput_depth.xyz();
            Float3 fresh = prefix * proposal->safe_throughput() * sample.Lo.as_vec3();
            indirect -= fresh;
            if (storage.denoiser.enabled() && !skip_dominant_signal_suffix) {
                // An eligible terminal receiver's candidate is the final
                // visit; earlier visits have already been committed.
                signal_secondary -= fresh;
            }
        } $else {
            rtxpt_gi::CandidateTraceResult empty;
            rtxpt_proposals_.write(dispatch_id(), empty.proposal);
            samples_.write(dispatch_id(), empty.sample);
        };
        $if(!receiver_written || capture.receiver_bounce == InvalidUI32) {
            rtxpt_gi::PTSurfaceView empty;
            if (!skip_surface) write_pt_surface(dispatch_id(), empty);
            RtxptGIPrefixAuxVar aux; rtxpt_prefix_aux_.write(dispatch_id(), aux);
        };
        indirect = ocarina::zero_if_nan_inf(indirect);
        rtxpt_bypass_.write(dispatch_id(), make_float4(indirect + stable, 0.f));
        if (work_counters) path_work_counters_.write(dispatch_id(), *work_counters);
        if (storage.denoiser.enabled()) {
            if (buffer_signal_commit) {
                for (uint p = 0u; p < 3u; ++p) {
                    $if(signal_plane == p) {
                        gi_signal_commit_buffer_visit(storage.denoiser, dispatch_id(), Uint{p},
                            signal_base_diffuse, signal_base_specular, signal_secondary,
                            skip_distance ? Float{0.f} : capture.signal_hit_distance, signal_scatter_diffuse);
                    };
                    Float4 diffuse = storage.denoiser.read_diffuse(dispatch_id(), Uint{p});
                    Float4 specular = storage.denoiser.read_specular(dispatch_id(), Uint{p});
                    storage.denoiser.write(dispatch_id(), Uint{p},
                        make_float4(ocarina::zero_if_nan_inf(Float3{diffuse.xyz()}), diffuse.w),
                        make_float4(ocarina::zero_if_nan_inf(Float3{specular.xyz()}), specular.w));
                }
            } else {
                for (uint p = 0u; p < 3u; ++p) {
                    if (!skip_signal_commit) {
                        $if(signal_plane == p) {
                            gi_signal_commit_visit((*signal_diffuse)[p], (*signal_specular)[p], signal_base_diffuse,
                                signal_base_specular, signal_secondary,
                                skip_distance ? Float{0.f} : capture.signal_hit_distance, signal_scatter_diffuse);
                        };
                    }
                    Float4 diffuse = (*signal_diffuse)[p], specular = (*signal_specular)[p];
                    storage.denoiser.write(dispatch_id(), Uint{p},
                        make_float4(ocarina::zero_if_nan_inf(Float3{diffuse.xyz()}), diffuse.w),
                        make_float4(ocarina::zero_if_nan_inf(Float3{specular.xyz()}), specular.w));
                }
            }
            // Physical stable-prefix radiance is outside every noisy plane
            // and receives no second multiplication by prefix throughput.
            storage.denoiser.bypass->write(dispatch_id(), make_float4(stable, 0.f));
        }
    };
    kernel.function()->set_block_dim(64u, 1u, 1u);
    rtxpt_initial_samples_ = device().compile(kernel, "RTXPT GI shared PT initial candidates");
}

GIParam RtxptGI::construct_param() const noexcept {
    GIParam param;
    param.camera_jitter = integrator()->jitter_primary_samples();
    param.max_age = max_age_;

    param.spatial = static_cast<uint>(spatial_.open);
    param.N = spatial_.sample_num;
    param.s_dot = spatial_.dot_threshold();
    param.s_depth = spatial_.depth_threshold;
    param.s_radius = spatial_.sampling_radius;

    param.temporal = static_cast<uint>(temporal_.open);
    param.history_limit = temporal_.limit;
    param.t_dot = temporal_.dot_threshold();
    param.t_depth = temporal_.depth_threshold;
    return param;
}

CommandBatch RtxptGI::dispatch(uint frame_index, bool precise_lobes, BufferView<uint> primary_flags,
                                float di_factor, float gi_factor, bool di_active) const noexcept {
    CommandBatch ret;
    const Pipeline *rp = pipeline();
    GIParam param = construct_param();
    param.precise_lobes = uint(precise_lobes);
    param.denoiser_di_active = uint(di_active);
    param.denoiser_di_factor = di_factor;
    param.denoiser_gi_factor = gi_factor;
    // The first frame after a mode/scene reset must not reuse old reservoirs.
    bool history_valid = history_.begin_frame(frame_index, param.camera_jitter);
    param.temporal = param.temporal && history_valid;
    const char *stage_profile = std::getenv("VISION_STAGE_PROFILE");
    const bool synchronize_native_stages = stage_profile && string_view(stage_profile) == "1";
    if (synchronize_native_stages) ret << synchronize();
    ret << rtxpt_prefix_build_(frame_index, primary_flags).dispatch(rp->resolution());
    if (synchronize_native_stages) ret << synchronize();
    RtxptGICameraParam camera;
    const float4x4 world_to_camera = inverse(scene().sensor()->host_c2w());
    const float4x4 camera_to_raster = inverse(scene().sensor()->host_raster_to_sensor());
    for (uint column = 0u; column < 4u; ++column) {
        camera.columns[column] = world_to_camera[column];
        camera.columns[4u + column] = camera_to_raster[column];
    }
    ret << rtxpt_initial_samples_(frame_index, camera, primary_flags).dispatch(rp->resolution());
    if (path_cost::enabled()) ret << path_cost::capture(path_work_counters_.view(), frame_index, rp->resolution(), "gi");
    if (synchronize_native_stages) ret << synchronize();
    ret << dispatch_reuse(frame_index, param);
    return ret;
}

void RtxptGI::update_resolution(ocarina::uint2 res) noexcept {
    Pipeline *rp = pipeline();
    if (compact_radiance_) {
        raw_radiance_.super() = device().create_buffer<uint2>(rp->pixel_num(), radiance_->name() + " rgb21");
        raw_radiance_.register_self();
        vector<uint2> zero(rp->pixel_num(), make_uint2(0u));
        raw_radiance_.upload_immediately(zero.data());
    }
    if (reservoirs_.has_registered()) resize_reuse_buffers(res);
    if (rtxpt_lights_) rtxpt_lights_->update_resolution(res);
    prepare_rtxpt_candidate_buffers();
    if (native_reblur_enabled_) {
        prepare_native_reblur_buffers();
        if (reservoirs_.has_registered() && backend_error_.empty()) {
            compile_denoiser_passes();
        }
    }

    samples_.super() = device().create_buffer<GISample>(rp->pixel_num(),
                                                        "RtxptGI::samples_");
    samples_.register_self();
    vector<GISample> vec{rp->pixel_num(), GISample{}};
    samples_.upload_immediately(vec.data());
}

void RtxptGI::restore(RuntimeObject *old_obj) noexcept {
    const auto *previous = hotfix::hotfix_detail::restore_source(*this, old_obj);
    if (!previous) return;
    OC_ERROR_IF(!previous->radiance_->name().ends_with(" [gi-radiance-v3-rtxpt]"),
                "GI radiance storage ABI changed; restart the engine before loading the new shaders.");
    ReSTIR::restore(old_obj);
    VS_HOTFIX_MOVE_ATTRS(radiance_, samples_, raw_radiance_, compact_radiance_, radiance_prepared_,
                       unsupported_backend_, backend_error_, path_work_counters_, rtxpt_proposals_, rtxpt_bypass_, rtxpt_lights_, rtxpt_materials_,
                       rtxpt_prefix_planes_, rtxpt_prefix_headers_, rtxpt_prefix_stable_, rtxpt_prefix_aux_, rtxpt_prefix_build_, rtxpt_initial_samples_,
                       native_reblur_enabled_, native_reblur_diffuse_, native_reblur_specular_, native_reblur_bypass_, native_reblur_motion_y_,
                       reservoirs_, neighbors_, surfaces_, references_, extent_, temporal_shader_, spatial_shader_, final_shader_,
                       pt_validation_, pt_validation_output_, pt_validation_pending_, previous_frame_, previous_jitter_)
    if (rtxpt_lights_) rtxpt_lights_->rebind_owner(*this);
}

void RtxptGI::set_compact_radiance(bool compact) noexcept {
    if (radiance_prepared_ && compact == compact_radiance_) return;
    const string suffix = " [gi-radiance-v3-rtxpt]";
    if (!radiance_->name().ends_with(suffix)) radiance_->set_name(radiance_->name() + suffix);
    ByteBufferView empty{handle_ty{0u}, 0u};
    if (radiance_prepared_) {
        // remove_buffer renumbers later slots; preserve their captured indices.
        if (compact_radiance_) {
            pipeline()->bindless_array().set_buffer(raw_radiance_.index().hv(), empty);
            raw_radiance_.super().destroy();
        } else {
            frame_buffer().unregister(radiance_);
            pipeline()->bindless_array().set_buffer(radiance_->index().hv(), empty);
            radiance_->clear_all();
        }
    }
    compact_radiance_ = compact;
    if (compact) {
        raw_radiance_.super() = device().create_buffer<uint2>(pipeline()->pixel_num(), radiance_->name() + " rgb21");
        raw_radiance_.register_self();
        vector<uint2> zero(pipeline()->pixel_num(), make_uint2(0u));
        raw_radiance_.upload_immediately(zero.data());
    } else {
        frame_buffer().prepare_screen_buffer(radiance_);
        vector<float4> zero(pipeline()->pixel_num(), make_float4(0.f));
        radiance_->device_buffer().upload_immediately(zero.data());
    }
    radiance_prepared_ = true;
}

vector<float4> RtxptGI::download_radiance() const noexcept {
    vector<float4> result(pipeline()->pixel_num());
    if (compact_radiance_) {
        vector<uint2> stored(result.size());
        pipeline()->stream() << raw_radiance_.view().download(stored.data()) << synchronize() << commit();
        for (size_t i = 0u; i < result.size(); ++i) result[i] = unpack_restir_radiance_host(stored[i]);
    } else {
        pipeline()->stream() << radiance_->view().download(result.data()) << synchronize() << commit();
    }
    return result;
}

void RtxptGI::prepare(bool compact_radiance) noexcept {
    switch_profile::Scope profile{"RtxptGI::prepare", "buffers"};
    Pipeline *rp = pipeline();

    set_compact_radiance(compact_radiance);

    samples_.super() = device().create_buffer<GISample>(rp->pixel_num(),
                                                        "RtxptGI::samples_");
    samples_.register_self();
    vector<GISample> vec{rp->pixel_num(), GISample{}};
    samples_.upload_immediately(vec.data());
    prepare_rtxpt_candidate_buffers();
}

void RtxptGI::prepare_rtxpt_candidate_buffers() noexcept {
    const uint pixels = pipeline()->pixel_num();
    if (path_cost::enabled()) path_work_counters_ = device().create_buffer<path_cost::PathWorkCounters>(pixels, "GI Fill work counters");
    rtxpt_proposals_.super() = device().create_buffer<HitBSDF>(pixels,
        "RtxptGI::rtxpt_proposals_ [native-standard-v1]");
    rtxpt_proposals_.register_self();
    vector<HitBSDF> proposals(pixels);
    rtxpt_proposals_.upload_immediately(proposals.data());
    rtxpt_bypass_.super() = device().create_buffer<float4>(pixels,
        "RtxptGI::rtxpt_bypass_ [native-standard-v1]");
    rtxpt_bypass_.register_self();
    vector<float4> bypass(pixels, make_float4(0.f));
    rtxpt_bypass_.upload_immediately(bypass.data());
    rtxpt_prefix_planes_.super() = device().create_buffer<RtxptGIPrefixPlane>(pixels * 3u,
        "RTXPT GI private native three stable planes");
    rtxpt_prefix_planes_.register_self();
    rtxpt_prefix_headers_.super() = device().create_buffer<uint4>(pixels,
        "RTXPT GI private stable plane headers");
    rtxpt_prefix_headers_.register_self();
    rtxpt_prefix_stable_.super() = device().create_buffer<float4>(pixels,
        "RTXPT GI private stable emission without shared direct");
    rtxpt_prefix_stable_.register_self();
    rtxpt_prefix_aux_.super() = device().create_buffer<RtxptGIPrefixAux>(pixels,
        "RTXPT GI private prefix aux and half motion [80B-v2]");
    rtxpt_prefix_aux_.register_self();
}

void RtxptGI::prepare_native_reblur_buffers() noexcept {
    const uint pixels = pipeline()->pixel_num();
    if (native_reblur_diffuse_.size() == size_t(pixels) * 3u &&
        native_reblur_motion_y_.size() == size_t(pixels) * 3u) return;
    native_reblur_diffuse_ = device().create_buffer<float4>(size_t(pixels) * 3u, "RTXPT GI native REBLUR diffuse x3");
    native_reblur_specular_ = device().create_buffer<float4>(size_t(pixels) * 3u, "RTXPT GI native REBLUR specular x3");
    native_reblur_bypass_ = device().create_buffer<float4>(pixels, "RTXPT GI native deterministic stable radiance");
    native_reblur_motion_y_ = device().create_buffer<float>(size_t(pixels) * 3u, "RTXPT GI native REBLUR FP32 motion Y x3");
}

void RtxptGI::compile_denoiser_passes() noexcept {
    compile_initial_samples();
    compile_final();
    reset_history();
}

bool RtxptGI::set_native_reblur_enabled(bool enabled) noexcept {
    if (enabled == native_reblur_enabled_) return false;
    native_reblur_enabled_ = enabled;
    if (reservoirs_.has_registered() && backend_error_.empty()) {
        if (enabled) prepare_native_reblur_buffers();
        compile_denoiser_passes();
    }
    invalidate_history();
    if (!enabled && native_reblur_diffuse_.size() != 0u) {
        native_reblur_diffuse_.destroy(); native_reblur_specular_.destroy();
        native_reblur_bypass_.destroy();
        native_reblur_motion_y_.destroy();
    }
    return true;
}
}// namespace vision

VS_REGISTER_HOTFIX(vision, RtxptGI)
