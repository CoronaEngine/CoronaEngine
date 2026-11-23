#pragma once

#include "core.h"
#include "base/scattering/half_storage.h"
#include "base/mgr/geometry.h"
#include "base/mgr/renderer.h"
#include "base/mgr/scene.h"
#include "base/sensor/frame_buffer.h"
#include "math/warp.h"

namespace vision::restir_gi {
struct Sample {
    float4 hit_distance{};
    float4 origin_cosine{};
    float4 radiance_count{};
    float4 normal_weight{};
    uint light_seed{};
};
static_assert(sizeof(Sample)==80u);
} // namespace vision::restir_gi
OC_STRUCT(vision::restir_gi, Sample, hit_distance, origin_cosine, radiance_count, normal_weight, light_seed) {};

namespace vision::restir_gi {
struct DeviceOps {
    using Scalar = Float;
    static Float min(Float a, Float b) { return ocarina::min(a,b); }
    static Float max(Float a, Float b) { return ocarina::max(a,b); }
    static Float clamp(Float x, Float a, Float b) { return ocarina::clamp(x,a,b); }
    static Float sqrt(Float x) { return ocarina::sqrt(x); }
    static Float abs(Float x) { return ocarina::abs(x); }
    static Float pow(Float x, Float y) { return ocarina::pow(x,y); }
    static Float safe(Float x) { return ocarina::zero_if_nan_inf(x); }
};
using DeviceMath = Math<DeviceOps>;

inline Uint hash(Uint x) {
    x += x << 10u; x ^= x >> 6u; x += x << 3u; x ^= x >> 11u; x += x << 15u;
    return x;
}
inline Uint hash_combine(Uint x, Uint y) {
    Uint s = (x * 1664525u + y + 1013904223u) * 1664525u;
    s ^= s >> 11u; s ^= (s << 7u) & 0x9d2c5680u; s ^= (s << 15u) & 0xefc60000u; s ^= s >> 18u;
    return s;
}
inline Uint seed(const Uint2 &p, const Uint &frame) { return hash_combine(p.x, hash_combine(p.y, hash(frame))); }
inline Float random(Uint &state) {
    Uint old = state; state = hash(state);
    return ocarina::as<float>((old & 0x007fffffu) | 0x3f800000u) - 1.f;
}
inline Float3 direction(const Float3 &delta) { return delta * ocarina::rsqrt(ocarina::max(dot(delta,delta),1e-20f)); }
inline Float smooth(const Float &a, const Float &b, const Float &x) {
    Float t = ocarina::clamp((x-a)/(b-a),0.f,1.f);
    return t*t*(3.f-2.f*t);
}
inline Float luminance(const Float3 &v) { return ocarina::max(0.f,ocarina::luminance(v)); }
inline Float normal_relevance(const Float &dot_value) {
    Float x = ocarina::max(0.f,dot_value+.5f);
    return x*x/2.25f;
}
// Native TemporalReservoirOutput word layout. The depth word deliberately holds
// Corona positive linear depth, rather than the reference's reverse-Z depth.
// Offset/normal use the caller's world axes; encoding does not change handedness.
struct TemporalOutput {
    Float depth;
    Float3 hit_offset;
    Float luminance;
    Float3 hit_normal;
};
inline Uint pack_normal_11_10_11(const Float3 &normal) {
    Uint x = ocarina::cast<uint>(ocarina::clamp(normal.x*.5f+.5f,0.f,1.f)*2047.f);
    Uint y = ocarina::cast<uint>(ocarina::clamp(normal.y*.5f+.5f,0.f,1.f)*1023.f);
    Uint z = ocarina::cast<uint>(ocarina::clamp(normal.z*.5f+.5f,0.f,1.f)*2047.f);
    return x | (y << 11u) | (z << 21u);
}
inline Float3 unpack_normal_11_10_11(const Uint &packed) {
    Float3 normal = make_float3(ocarina::cast<float>(packed & 2047u)/2047.f,
                               ocarina::cast<float>((packed >> 11u) & 1023u)/1023.f,
                               ocarina::cast<float>((packed >> 21u) & 2047u)/2047.f)*2.f-1.f;
    return ocarina::normalize(normal);
}
// Match the reference integer textures. Only these mixed records need explicit
// binary16 conversion, using shader intrinsics instead of the emulated half type.
inline Uint pack_image_half2(const Float2 &value) {
    return ocarina::f32tof16(value.x) | (ocarina::f32tof16(value.y) << 16u);
}
inline Float2 unpack_image_half2(const Uint &value) {
    return make_float2(ocarina::f16tof32(value & 65535u),ocarina::f16tof32(value >> 16u));
}
inline Uint pack_pixel(const Uint2 &pixel) {
    return pixel.x | (pixel.y << 16u);
}
inline Uint2 unpack_pixel(const Uint &payload) {
    return make_uint2(payload & 65535u,payload >> 16u);
}
inline Uint4 pack_temporal_output(const Float &linear_depth, const Float3 &hit_offset,
                                  const Float &sample_luminance, const Float3 &hit_normal) {
    return make_uint4(ocarina::as<uint>(linear_depth),
                      pack_image_half2(make_float2(hit_offset.x,hit_offset.y)),
                      pack_image_half2(make_float2(hit_offset.z,sample_luminance)),
                      pack_normal_11_10_11(hit_normal));
}
inline TemporalOutput unpack_temporal_output(const Uint4 &raw) {
    Float2 xy = unpack_image_half2(raw.y);
    Float2 zl = unpack_image_half2(raw.z);
    return {ocarina::as<float>(raw.x),make_float3(xy,zl.x),zl.y,unpack_normal_11_10_11(raw.w)};
}
struct Reservoir {
    Uint payload{0u};
    Float M{0.f}, W{0.f};
};
inline Reservoir unpack(const Uint2 &raw) {
    Float2 mw = unpack_image_half2(raw.y);
    return {raw.x,mw.x,mw.y};
}
inline Uint2 pack(const Reservoir &r) {
    return make_uint2(r.payload,pack_image_half2(make_float2(r.M,r.W)));
}
struct Stream {
    Float sum{0.f}, count{0.f}, selected_target{0.f};
    void initialize(const Float &target) { sum=target; count=1.f; selected_target=target; }
    Bool merge(const Reservoir &r,const Float &target,const Float &extra,Uint &rng) {
        count += r.M;
        Float weight = ocarina::max(0.f,ocarina::zero_if_nan_inf(target*extra*r.W*r.M));
        sum += weight;
        Bool chosen = weight > 0.f && random(rng)*sum <= weight;
        selected_target = ocarina::select(chosen,target,selected_target);
        return chosen;
    }
    Float finish() const { return DeviceMath::finish_weight(sum,count,selected_target); }
};
} // namespace vision::restir_gi
