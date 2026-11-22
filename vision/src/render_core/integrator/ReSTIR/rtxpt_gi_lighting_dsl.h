#pragma once

#include "base/using.h"
#include "dsl/dsl.h"

/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

namespace vision::rtxpt_gi::lighting_dsl {
using namespace ocarina;

// Typed AST implementations of the RTXPT light sampling and feedback math.
struct PackedLight {
    Uint4 lo{make_uint4(0u)}, hi{make_uint4(0u)};
};
struct Triangle {
    Float3 base{make_float3(0.f)}, edge1{make_float3(0.f)}, edge2{make_float3(0.f)};
    Float3 radiance{make_float3(0.f)}, normal{make_float3(0.f)};
    Float area{0.f};
};
struct TriangleSample {
    Float3 position{make_float3(0.f)}, normal{make_float3(0.f)}, radiance{make_float3(0.f)};
    Float pdf{0.f};
    Bool sampleable{false};
};
struct Feedback {
    Uint index{0xffffffffu};
    Float candidate{0.f}, total{0.f};
};

// Native HLSL sign maps both signed zeros to zero. The project's generic
// sign primitive deliberately maps zero to +1, so it cannot be used here.
[[nodiscard]] inline Float native_sign(const Float &value) noexcept {
    return select(value > 0.f,1.f,select(value < 0.f,-1.f,0.f));
}
[[nodiscard]] inline Float2 native_sign(const Float2 &value) noexcept {
    return make_float2(native_sign(Float{value.x}),native_sign(Float{value.y}));
}
[[nodiscard]] inline Float3 oct_decode(const Float2 &uv) noexcept {
    Float2 p = uv * 2.f - 1.f;
    Float d = 1.f - (abs(p.x) + abs(p.y));
    Float r = 1.f - abs(d);
    Float phi = select(r > 0.f, ((abs(p.y) - abs(p.x)) / r + 1.f) * .78539816339744830962f, 0.f);
    Float f = r * sqrt(2.f - r * r);
    return make_float3(f * native_sign(Float{p.x}) * cos(phi), f * native_sign(Float{p.y}) * sin(phi), native_sign(d) * (1.f - r * r));
}
[[nodiscard]] inline Float2 oct_encode(const Float3 &n) noexcept {
    Float r = sqrt(1.f - abs(n.z));
    Float phi = atan2(abs(n.y), abs(n.x));
    Float2 p;
    p.y = r * phi * .63661977236758134308f;
    p.x = r - p.y;
    $if(n.z < 0.f) { p = 1.f - p.yx(); };
    p *= native_sign(Float2{n.xy()});
    return saturate(p * .5f + .5f);
}
[[nodiscard]] inline Float3 cube_direction(const Uint &face, const Float2 &uv) noexcept {
    Float cx = uv.x * 2.f - 1.f, cy = 1.f - uv.y * 2.f;
    Float l = sqrt(cx * cx + cy * cy + 1.f);
    Float3 d = make_float3(0.f);
    $if(face == 0u) { d = make_float3(1.f, cy, -cx); }
    $elif(face == 1u) { d = make_float3(-1.f, cy, cx); }
    $elif(face == 2u) { d = make_float3(cx, 1.f, -cy); }
    $elif(face == 3u) { d = make_float3(cx, -1.f, cy); }
    $elif(face == 4u) { d = make_float3(cx, cy, 1.f); }
    $elif(face == 5u) { d = make_float3(-cx, cy, -1.f); };
    return d * (1.f / l);
}
[[nodiscard]] inline Float3 cube_coordinate(const Float3 &d) noexcept {
    Float3 a = abs(d);
    Uint face = 0u;
    Float2 q = make_float2(0.f);
    $if(a.x >= a.y && a.x >= a.z) {
        face = select(d.x >= 0.f, 0u, 1u);
        q = make_float2(select(d.x >= 0.f, -d.z, d.z), d.y) / a.x;
    } $elif(a.y >= a.z) {
        face = select(d.y >= 0.f, 2u, 3u);
        q = make_float2(d.x, select(d.y >= 0.f, -d.z, d.z)) / a.y;
    } $else {
        face = select(d.z >= 0.f, 4u, 5u);
        q = make_float2(select(d.z >= 0.f, d.x, -d.x), d.y) / a.z;
    };
    return make_float3(q.x * .5f + .5f, .5f - q.y * .5f, cast<float>(face));
}
[[nodiscard]] inline Float sphere_quadrant_area(const Float &x, const Float &y) noexcept {
    return atan2(x * y, sqrt(x * x + y * y + 1.f));
}
[[nodiscard]] inline Float cube_area(const Float &dim, const Uint2 &coord) noexcept {
    Float inv_dim = 1.f / dim;
    Float s = (cast<float>(coord.x) + .5f) * 2.f * inv_dim - 1.f;
    Float t = (cast<float>(coord.y) + .5f) * 2.f * inv_dim - 1.f;
    Float x0 = s - inv_dim, y0 = t - inv_dim, x1 = s + inv_dim, y1 = t + inv_dim;
    Float area = sphere_quadrant_area(x0,y0) - sphere_quadrant_area(x0,y1) -
                 sphere_quadrant_area(x1,y0) + sphere_quadrant_area(x1,y1);
    return max(1e-6f, abs(area));
}
[[nodiscard]] inline Float4 decode_half(const Uint2 &v) noexcept {
    return make_float4(f16tof32(v.x & 65535u), f16tof32(v.x >> 16u),
                       f16tof32(v.y & 65535u), f16tof32(v.y >> 16u));
}
[[nodiscard]] inline Uint2 encode_half(const Float4 &v) noexcept {
    return make_uint2(f32tof16(v.x) | (f32tof16(v.y) << 16u),
                      f32tof16(v.z) | (f32tof16(v.w) << 16u));
}
[[nodiscard]] inline Float4 half_round(const Float4 &v) noexcept { return decode_half(encode_half(v)); }

[[nodiscard]] inline Float unpack_radiance(const Uint &packed) noexcept {
    return select(packed == 0u, 0.f, exp2((cast<float>(packed - 1u) / 65534.f) * (40.f - -8.f) + -8.f));
}
[[nodiscard]] inline Float3 radiance(const Uint4 &lo, const Uint4 &hi) noexcept {
    Float3 color = make_float3(cast<float>(lo.w & 255u) / 255.f,
                              cast<float>((lo.w >> 8u) & 255u) / 255.f,
                              cast<float>((lo.w >> 16u) & 255u) / 255.f);
    return color * unpack_radiance(hi.w & 65535u);
}
inline void pack_color(const Float3 &radiance, PackedLight &p) noexcept {
    Float intensity = max(radiance.x, max(radiance.y, radiance.z));
    $if(intensity > 0.f) {
        Float log_radiance = saturate((log2(intensity) - -8.f) / (40.f - -8.f));
        Uint encoded = min(cast<uint>(ceil(log_radiance * 65534.f)) + 1u, 65535u);
        Float unpacked = unpack_radiance(encoded);
        Float3 normalized = saturate(radiance / unpacked);
        Uint r = cast<uint>(floor(normalized.x * 255u + .5f)) & 255u;
        Uint g = (cast<uint>(floor(normalized.y * 255u + .5f)) & 255u) << 8u;
        Uint b = (cast<uint>(floor(normalized.z * 255u + .5f)) & 255u) << 16u;
        p.hi.w |= encoded;
        p.lo.w |= r | g | b;
    };
}
[[nodiscard]] inline PackedLight pack_triangle(const Float3 &a, const Float3 &b,
                                               const Float3 &c, const Float3 &radiance) noexcept {
    PackedLight p;
    Float3 e1 = b - a, e2 = c - a;
    pack_color(radiance, p);
    p.lo = make_uint4(as<uint3>(a + ((e1 + e2) / 3.f)), p.lo.w | (1u << 24u));
    Uint3 encoded = make_uint3((f32tof16(e1.x) & 65535u) | (f32tof16(e2.x) << 16u),
                              (f32tof16(e1.y) & 65535u) | (f32tof16(e2.y) << 16u),
                              (f32tof16(e1.z) & 65535u) | (f32tof16(e2.z) << 16u));
    // Matched TriangleLight::Store uses numeric uint3->float3->uint, not asuint.
    Float3 edges = cast<float3>(encoded);
    p.hi.x = cast<uint>(edges.x); p.hi.y = cast<uint>(edges.y); p.hi.z = cast<uint>(edges.z);
    return p;
}
[[nodiscard]] inline PackedLight pack_environment(const Uint3 &node, const Float3 &radiance,
                                                  const Float &weight) noexcept {
    PackedLight p;
    pack_color(radiance, p);
    p.lo.w |= 5u << 24u;
    p.hi.x = (node.x << 16u) | node.y;
    p.hi.y = node.z << 16u;
    p.hi.z = as<uint>(weight);
    return p;
}
[[nodiscard]] inline Triangle unpack_triangle(const Uint4 &lo, const Uint4 &hi) noexcept {
    Triangle t;
    t.edge1 = make_float3(f16tof32(hi.x & 65535u), f16tof32(hi.y & 65535u), f16tof32(hi.z & 65535u));
    t.edge2 = make_float3(f16tof32(hi.x >> 16u), f16tof32(hi.y >> 16u), f16tof32(hi.z >> 16u));
    t.base = as<float3>(Uint3{lo.xyz()}) - ((t.edge1 + t.edge2) / 3.f);
    t.radiance = radiance(lo,hi);
    Float3 n = cross(t.edge1,t.edge2);
    Float len = length(n);
    $if(len > 0.f) { t.area = .5f * len; t.normal = n / len; };
    return t;
}
[[nodiscard]] inline Float3 offset_origin(const Float3 &p,const Float3 &n) noexcept {
    Int3 offset = cast<int3>(n * (3.f * 256.f));
    Float3 integer = as<float3>(as<int3>(p) + select(p < 0.f, -offset, offset));
    Float3 fixed = n * (3.f / 65536.f);
    return select(abs(p) < 1.f / 16.f, p + fixed, integer);
}
[[nodiscard]] inline Float triangle_pdf(const Triangle &t,const Float3 &viewer,const Float3 &position) noexcept {
    Float3 delta = position - viewer;
    Float square_distance = max(2e-9f,dot(delta,delta));
    Float distance = sqrt(square_distance);
    Float3 direction = delta / distance;
    Float cosine = dot(t.normal,-direction);
    Float area_pdf = max(2e-9f,1.f / t.area);
    return min(1e10f,area_pdf * (distance * distance) / max(cosine,2e-9f));
}
[[nodiscard]] inline TriangleSample sample_triangle(const Uint4 &lo,const Uint4 &hi,
                                                    const Float2 &u,const Float3 &viewer) noexcept {
    Triangle t = unpack_triangle(lo,hi);
    TriangleSample s;
    Float sqrt_x = sqrt(u.x);
    Float3 bary = make_float3(1.f - sqrt_x,sqrt_x * (1.f - u.y),sqrt_x * u.y);
    s.position = t.base + t.edge1 * bary.y + t.edge2 * bary.z;
    s.position = offset_origin(s.position,t.normal);
    s.normal = t.normal;
    Float3 delta = s.position - viewer;
    Float square_distance = max(2e-9f,dot(delta,delta));
    Float distance = sqrt(square_distance);
    Float3 direction = delta / distance;
    Float cosine = dot(t.normal,-direction);
    $if(!(cosine <= 0.f)) {
        Float area_pdf = max(2e-9f,1.f / t.area);
        s.pdf = min(1e10f,area_pdf * (distance * distance) / max(cosine,2e-9f));
        s.radiance = t.radiance;
        s.sampleable = true;
    };
    return s;
}
[[nodiscard]] inline Float triangle_pdf(const Uint4 &lo,const Uint4 &hi,
                                       const Float3 &viewer,const Float3 &position) noexcept {
    return triangle_pdf(unpack_triangle(lo,hi),viewer,position);
}
[[nodiscard]] inline Float power(const Uint4 &lo,const Uint4 &hi) noexcept {
    Float result = 0.f;
    $if(((lo.w >> 24u) & 15u) == 5u) { result = as<float>(hi.z); }
    $else { Triangle t = unpack_triangle(lo,hi); result = t.area * Pi * dot(t.radiance,make_float3(.2126f,.7152f,.0722f)); };
    return result;
}
[[nodiscard]] inline Feedback unpack_feedback(const Uint2 &v) noexcept {
    Feedback r; r.index=v.x;r.candidate=f16tof32(v.y & 65535u);r.total=f16tof32(v.y >> 16u);return r;
}
[[nodiscard]] inline Uint2 pack_feedback(const Feedback &r) noexcept {
    return make_uint2(r.index,f32tof16(r.candidate) | (f32tof16(r.total) << 16u));
}
inline void feedback_add(Feedback &r,const Uint &index,const Float &weight,const Float &random) noexcept {
    r.total += weight;
    Float threshold = saturate(weight / r.total);
    $if(random < threshold) { r.index=index;r.candidate=weight; };
}
[[nodiscard]] inline Uint2 feedback_add(const Uint2 &packed,const Uint &index,const Float &weight,const Float &random) noexcept {
    Feedback r = unpack_feedback(packed);feedback_add(r,index,weight,random);return pack_feedback(r);
}
[[nodiscard]] inline Float feedback_blend(const Float &power,const Float &sum,const Uint &feedback_count,
                                          const Uint &valid_count,const Float &ratio) noexcept {
    Float feedback_weight = cast<float>(feedback_count) * sum / max(1.f,cast<float>(valid_count));
    return lerp(ratio,power,feedback_weight);
}
[[nodiscard]] inline Uint proxy_count(const Float &weight,const Float &sum,const Uint &budget,
                                     const Uint &light_count) noexcept {
    Uint count = 0u;
    $if(weight > 0.f) { count=min(cast<uint>(ceil((cast<float>(budget-light_count) * weight) / sum)),32768u); };
    return count;
}
[[nodiscard]] inline Uint hash32(Uint x) noexcept {
    x ^= x >> 16u;x *= 0x7feb352du;x ^= x >> 15u;x *= 0x846ca68bu;x ^= x >> 16u;return x;
}
[[nodiscard]] inline Uint hash_combine(const Uint &seed,const Uint &value) noexcept {
    return seed ^ (hash32(value) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u));
}
[[nodiscard]] inline Uint feedback_flood(const Uint2 &center,const Uint2 &left,const Uint2 &right,
                                        const Uint2 &up,const Uint2 &down,const Uint2 &pixel,const Uint &update) noexcept {
    Feedback r = unpack_feedback(center);
    Uint base = hash_combine(hash32(Uint{0x035f9f29u}),(pixel.x << 16u) | pixel.y);
    Uint state = hash_combine(hash_combine(base,Uint{0u}),update);
    $if(r.index == 0xffffffffu) {
        // Static host unroll preserves the native left/right/up/down RNG order.
        for (const auto *value : {&left,&right,&up,&down}) {
            Feedback other = unpack_feedback(*value);
            $if(other.index != 0xffffffffu) {
                other.candidate *= .05f;other.total *= .05f;
                state = hash32(state);
                Float random = cast<float>(state >> 8u) / 16777216.f;
                $if(other.total > 0.f) { feedback_add(r,other.index,other.total,random); };
            };
        }
    };
    return r.index;
}
}// namespace vision::rtxpt_gi::lighting_dsl
