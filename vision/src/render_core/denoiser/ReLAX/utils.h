#pragma once
#include "dsl/dsl.h"

namespace vision::relax {
using namespace ocarina;

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
