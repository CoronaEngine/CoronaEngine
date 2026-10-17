#pragma once
#include "dsl/dsl.h"

namespace vision::denoiser_signals {
using namespace ocarina;

inline Float2 merge_hit_distances(const Float2 &a, const Float &hit_a, const Float2 &b, const Float &hit_b) {
    Float2 wa = select(hit_a > 0.f, a, make_float2(0.f));
    Float2 wb = select(hit_b > 0.f, b, make_float2(0.f));
    Float2 sum = wa + wb;
    Float2 alpha = wa / select(sum > 0.f, sum, make_float2(1.f));
    // Zero energy does not invalidate a sampled endpoint. No positive-sum epsilon:
    // common brightness or presentation scaling must not alter geometric distance.
    return select(sum > 0.f, lerp(alpha, make_float2(hit_b), make_float2(hit_a)), make_float2(max(hit_a, hit_b)));
}

inline Float4 accumulate_lobe(const Float4 &old, const Float3 &value, const Float &hit) {
    Float a = max(luminance(old.xyz()), 0.f), b = max(luminance(value), 0.f);
    Float h = merge_hit_distances(make_float2(a), old.w, make_float2(b), hit).x;
    return make_float4(old.xyz() + value, h);
}
}
