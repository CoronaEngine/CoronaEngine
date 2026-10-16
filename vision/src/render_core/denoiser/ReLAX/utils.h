#pragma once
#include "dsl/dsl.h"
#include "filter.h"

namespace vision::relax {
using namespace ocarina;

inline Float luminance(const Float3 &c) { return dot(c, make_float3(.2126f, .7152f, .0722f)); }
inline Float3 safe_normalize(const Float3 &v) { return v * rsqrt(max(dot(v, v), 1e-20f)); }
inline Float smooth01(const Float &v) { Float x = saturate(v); return x * x * (3.f - 2.f * x); }
inline Float linear_step(float a, float b, const Float &x) { return saturate((x - a) / (b - a)); }
inline Float acos_approx(const Float &x) { return 1.41421356237f * sqrt(saturate(1.f - x)); }
inline Float nonexp_weight(const Float &x) { return smooth01(1.f - abs(x)); }
inline Float exponential_weight(const Float &x) { Float t = 3.f * abs(x); return 1.f / (t * t + t + 1.f); }
inline Float spec_magic(const Float &r) { return (1.f - exp2(-200.f * r * r)) * pow(saturate(r), .25f); }
// MathLib v1.3 corrected lobe bounding; RELAX's Atrous helper intentionally uses the legacy expression.
inline Float lobe_tan(const Float &r, const Float &volume) {
    Float v = saturate(volume); return saturate(r) * sqrt(v / (1.f - v + 1e-6f));
}
inline Float legacy_lobe_tan(const Float &r, const Float &volume) {
    Float v = saturate(volume); return r * r * v / (1.f - v + 1e-6f);
}
inline Float4 dominant_direction(const Float3 &n, const Float3 &v, const Float &roughness) {
    Float r = saturate(roughness), a = .298475f * log(39.4115f - 39.0029f * r);
    Float f = saturate(pow(saturate(1.f - abs(dot(n, v))), 10.8649f) * (1.f - a) + a);
    Float3 reflection = 2.f * dot(n, v) * n - v;
    return make_float4(safe_normalize(lerp(f, n, reflection)), f);
}
inline Bool valid_guide(const Var<Guide> &g) { return g.identity.w != 0u && g.position.w > 0.f; }
inline Bool spatial_compatible(const Var<Guide> &a, const Var<Guide> &b) {
    return valid_guide(b) && a.identity.y == b.identity.y && a.identity.z == b.identity.z;
}
inline Float plane_weight(const Var<Guide> &a, const Var<Guide> &b, const Float &threshold) {
    Float d = abs(dot(b.position.xyz() - a.position.xyz(), a.normal_roughness.xyz()));
    return select(d < threshold * a.position.w, 1.f, 0.f);
}

inline Float2 project_pixel(const Float4x4 &matrix, const Float3 &position) {
    Float4 clip = matrix * make_float4(position, 1.f);
    Float w = select(abs(clip.w) > 1e-8f, clip.w, 1e-8f);
    return clip.xy() / w;
}
inline Float encoding_normal_weight(const Float3 &a, const Float3 &b, const Float &angle,
                                    const Float &curvature_angle, float threshold, bool remap) {
    Float w = smooth01(1.f - (acos_approx(dot(a, b)) - curvature_angle - threshold) / max(angle, 1e-6f));
    return remap ? smooth01((w - .05f) / .9f) : w;
}
inline Float thin_lens(const Float &distance, const Float &curvature) {
    Float denominator = 2.f * curvature * distance + 1.f;
    denominator = select(abs(denominator) < 1e-6f, select(denominator < 0.f, -1e-6f, 1e-6f), denominator);
    return distance / denominator;
}
inline Float3 to_ycocg(const Float3 &c) {
    return make_float3(dot(c, make_float3(.25f, .5f, .25f)), (c.x - c.z) * .5f,
                       dot(c, make_float3(-.25f, .5f, -.25f)));
}
inline Float3 to_rgb(const Float3 &c) { return make_float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }
inline Float2 atrous_normal_params(const Float &r, const Float &frames, const Float &confidence,
                                   const Float &relaxation, const Float &fraction, const Float &slack) {
    Float relax = saturate(frames / 5.f) * lerp(relaxation, 1.f, confidence);
    Float angle = atan(legacy_lobe_tan(r, fraction)) * (10.f - 9.f * relax) + slack * .01745329252f;
    return make_float2(min(1.57079632679f, angle), .9f + .1f * relax);
}
inline Float atrous_specular_normal(const Float2 &params, const Float3 &n, const Float3 &sample_n,
                                    const Float3 &v, const Float3 &sample_v) {
    Float angle = acos_approx(min(dot(n, sample_n), dot(v, sample_v)));
    return saturate(1.f - smooth01(angle / max(params.x, 1e-6f)) * params.y);
}

inline Float4 pack_signal(const Float4 &signal, const Float3 &factor) {
    Float3 rgb = signal.xyz() / max(factor, make_float3(.01f));
    Bool invalid = any(ocarina::isnan(rgb)) || any(ocarina::isinf(rgb));
    rgb = select(invalid, make_float3(0.f), clamp(rgb, 0.f, 65504.f));
    Float distance = select(ocarina::isnan(signal.w) || ocarina::isinf(signal.w), 0.f, clamp(signal.w, 0.f, 65504.f));
    return make_float4(rgb, distance);
}

inline Float2 motion_to_uv(const Float2 &motion, const Uint2 &size) {
    return -motion / make_float2(size);
}

inline Uint pixel_index(const Int2 &pos, const Uint2 &size) {
    Int2 p = clamp(pos, make_int2(0), make_int2(size) - 1);
    return cast<uint>(p.y) * size.x + cast<uint>(p.x);
}

inline Bool inside(const Int2 &pos, const Uint2 &size) {
    return all(pos >= make_int2(0)) && all(pos < make_int2(size));
}

template<typename B>
inline Float4 sample_linear(const B &buffer, const Float2 &pos, const Uint2 &size) {
    Float2 origin = floor(pos - .5f);
    Float2 f = pos - .5f - origin;
    Int2 p = make_int2(origin);
    Float4 a = lerp(f.x, buffer.read(pixel_index(p, size)), buffer.read(pixel_index(p + make_int2(1, 0), size)));
    Float4 b = lerp(f.x, buffer.read(pixel_index(p + make_int2(0, 1), size)), buffer.read(pixel_index(p + make_int2(1, 1), size)));
    return lerp(f.y, a, b);
}

template<typename B>
inline Float4 sample_bicubic(const B &buffer, const Float2 &pos, const Uint2 &size) {
    Float2 center = floor(pos - .5f) + .5f;
    Float2 f = saturate(pos - center);
    Float2 w0 = f * (f * (-.5f * f + 1.f) - .5f);
    Float2 w1 = f * (f * (1.5f * f - 2.5f)) + 1.f;
    Float2 w2 = f * (f * (-1.5f * f + 2.f) + .5f);
    Float2 w3 = f * (f * (.5f * f - .5f));
    Float2 w12 = w1 + w2, tc = w2 / w12;
    Float a = w12.x * w0.y, b = w0.x * w12.y, c = w12.x * w12.y;
    Float d = w3.x * w12.y, e = w12.x * w3.y;
    Float4 color = sample_linear(buffer, center + make_float2(tc.x, -1.f), size) * a;
    color += sample_linear(buffer, center + make_float2(-1.f, tc.y), size) * b;
    color += sample_linear(buffer, center + tc, size) * c;
    color += sample_linear(buffer, center + make_float2(2.f, tc.y), size) * d;
    color += sample_linear(buffer, center + make_float2(tc.x, 2.f), size) * e;
    return color / max(a + b + c + d + e, .0001f);
}
}
