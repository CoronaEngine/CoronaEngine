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

struct MergedRestirLobes {
    Float4 diffuse, specular, bypass;
};

// Keep the DI/GI arithmetic together so producer-side FP32 staging and the
// observable raw-output path use the same operation order and hit weighting.
inline MergedRestirLobes merge_restir_lobes(
    const Float3 &direct, const Float3 &indirect, const Float4 &di_specular,
    const Float4 &gi_specular, const Float3 &bypass, const Float &di, const Float &gi,
    const Float4 &old_diffuse, const Float4 &old_specular, const Float4 &old_bypass) {
    Float3 d = direct - bypass;
    Float3 g = indirect;
    Float3 spec = di_specular.xyz() * di + gi_specular.xyz() * gi;
    Float3 diff = (d - di_specular.xyz()) * di + (g - gi_specular.xyz()) * gi;
    Float2 wd = make_float2(max(luminance(d - di_specular.xyz()), 0.f) * di,
                           max(luminance(di_specular.xyz()), 0.f) * di);
    Float2 wg = make_float2(max(luminance(g - gi_specular.xyz()), 0.f) * gi,
                           max(luminance(gi_specular.xyz()), 0.f) * gi);
    Float2 distance = merge_hit_distances(wd, select(di > 0.f, di_specular.w, 0.f),
                                         wg, select(gi > 0.f, gi_specular.w, 0.f));
    return {accumulate_lobe(old_diffuse, diff, distance.x),
            accumulate_lobe(old_specular, spec, distance.y),
            old_bypass + make_float4(bypass * di, 0.f)};
}
}
