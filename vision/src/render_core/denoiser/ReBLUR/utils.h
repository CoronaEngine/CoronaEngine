#pragma once
#include "guide_layout.h"

namespace vision::reblur {
// Match NRD's FP16 signal/history storage, keeping all filtering in Float.
// Signed YCoCg chroma must survive conversion; reprojection can overshoot
// the packed input range, so saturate only at the storage boundary.
inline Half4 store_half(Float4 value) {
    return make_half4(clamp(value, make_float4(-65504.f), make_float4(65504.f)));
}
inline Half2 store_half(Float2 value) {
    return make_half2(clamp(value, make_float2(-65504.f), make_float2(65504.f)));
}
inline Uint guide_instance(Uint receiver, Uint prefix) {
    // A stable prefix identifies the path to the plane, not its final receiver.
    return (prefix * 1664525u) ^ (receiver + 1013904223u);
}
inline Float smooth01(Float x) { x = saturate(x); return x * x * (3.f - 2.f * x); }
inline Float linear_step(Float a, Float b, Float x) { return saturate((x - a) / (b - a)); }
inline Float3 to_ycocg(Float3 c) {
    return make_float3(dot(c, make_float3(.25f, .5f, .25f)),
                       .5f * (c.x - c.z), -.25f * c.x + .5f * c.y - .25f * c.z);
}
inline Float3 to_rgb(Float3 c) {
    Float t = c.x - c.z;
    return max(make_float3(t + c.y, c.x + c.z, t - c.y), make_float3(0.f));
}
inline Float hit_scale(Float z, Float roughness) {
    return (3.f + abs(z) * .1f) * lerp(saturate(exp2(-25.f * roughness * roughness)), 1.f, 20.f);
}
inline Float spec_magic(Float r) { return (1.f - exp2(-200.f * r * r)) * pow(saturate(r), .25f); }
inline Float lobe_tan(Float r, Float volume) { return r * r * volume / max(1.f - volume, 1e-6f); }
inline Float direction_angle(Float3 a, Float3 b) {
    // Unit directions retain FP32 normalization error. acos(dot(a, a)) can
    // report motion for a stationary view and reject narrow glossy lobes.
    // The half-angle form preserves zero and resolves small real rotations.
    return 2.f * atan2(length(a - b), length(a + b));
}
inline Float parallax_confidence(Float displacement, Float lobe_pixels) {
    // Reflection tracking has a pixel footprint even for a vanishing GGX
    // lobe. Combine its half-pixel support with the projected lobe rather
    // than interpreting stochastic subpixel hit changes as disocclusions.
    Float footprint = sqrt(lobe_pixels * lobe_pixels + .25f);
    return footprint / (footprint + displacement);
}
inline Float dominant_factor(Float NoV, Float r) {
    Float a = .298475f * log(39.4115f - 39.0029f * r);
    return saturate(pow(saturate(1.f - NoV), 10.8649f) * (1.f - a) + a);
}
inline Float exponential_weight(Float delta) {
    Float x = 3.f * abs(delta);
    return 1.f / (x * x + x + 1.f);
}
inline Float normal_param(Float alpha, Float fraction, Float r) {
    Float volume = .75f * lerp(alpha, fraction, 1.f);
    return 1.f / max(atan(lobe_tan(r, volume)), .5f / 255.f);
}
inline Float4 change_luma(Float4 c, Float y) {
    return make_float4(c.xyz() * ((max(y, 0.f) + 1e-6f) / (c.x + 1e-6f)), c.w);
}
inline Float4 mix_history(Float4 h, Float4 c, Float alpha, Float r, Float max_frames) {
    Float hit_alpha = max(alpha, 1.f / (1.f + .5f * spec_magic(r) * max_frames));
    return make_float4(lerp(alpha, h.xyz(), c.xyz()), lerp(hit_alpha, h.w, c.w));
}
inline Float3 safe_rgb(Float3 rgb) {
    return select(all(rgb == rgb) && all(abs(rgb) < 3.402823466e38f),
                  clamp(rgb, make_float3(0.f), make_float3(65504.f)), make_float3(0.f));
}
inline Float4 pack_signal(Float4 raw, Float3 factor, Float z, Float r) {
    Float h = select(raw.w == raw.w && abs(raw.w) < 3.402823466e38f, saturate(raw.w / hit_scale(z, r)), 0.f);
    return make_float4(to_ycocg(safe_rgb(raw.xyz() / max(factor, make_float3(1e-3f)))), h);
}
inline Float2 project_raster(Float4x4 m, Float3 x) {
    Float4 p = m * make_float4(x, 1.f);
    return p.xy() / select(abs(p.w) > 1e-6f, p.w, 1e-6f);
}
inline Uint pixel_index(Int2 p, Uint2 size) {
    Uint2 q = make_uint2(clamp(p, make_int2(0), make_int2(size) - 1));
    return q.y * size.x + q.x;
}
inline Bool in_screen(Int2 p, Uint2 size) { return all(p >= 0) && all(p < make_int2(size)); }
inline Bool active_tile(const Var<FilterParam> &p) {
    Uint2 tile = dispatch_idx().xy() / 16u;
    return p.tiles.read(tile.y * ((p.size.x + 15u) / 16u) + tile.x) != 0u;
}
template<typename T>
inline Bool valid_guide(const Var<T> &g) { return g.identity.w != 0u && g.position.w > 0.f; }
template<typename T>
inline Bool same_surface(const Var<Guide> &a, const Var<T> &b) {
    return valid_guide(b) && all(a.identity.xyz() == b.identity.xyz());
}
inline Float frustum_size(const Var<FilterParam> &p, const Var<Guide> &g) {
    return max(min(p.size.x, p.size.y).cast<float>() * p.unproject * g.position.w, 1e-5f);
}
inline Float geometry_weight(const Var<FilterParam> &p, const Var<Guide> &a, const Var<Guide> &b) {
    Float delta = abs(dot(a.normal_roughness.xyz(), b.position.xyz() - a.position.xyz()));
    Float w = 1.f - smooth01(delta / max(p.plane_sensitivity * frustum_size(p, a), 1e-6f));
    return select(same_surface(a, b), w, 0.f);
}
inline Float spatial_weight(const Var<FilterParam> &p, const Var<Guide> &a, const Var<Guide> &b,
                            Float alpha, Float fraction_scale, bool specular, Float normal_footprint = 0.f) {
    Float r = specular ? a.normal_roughness.w : Float{1.f};
    Float angle = direction_angle(a.normal_roughness.xyz(), b.normal_roughness.xyz());
    Float w = geometry_weight(p, a, b);
    Float normal_width = max(fraction_scale / normal_param(alpha, p.lobe_fraction, r), normal_footprint);
    w *= 1.f - smooth01(angle / normal_width);
    if (specular) {
        Float norm = lerp(saturate(r * p.roughness_fraction * fraction_scale), .01f, 1.f);
        w *= 1.f - smooth01(abs(b.normal_roughness.w - r) / norm);
    }
    return w;
}
inline Float antilag(Float h, Float avg, Float sigma, Float count, const Var<FilterParam> &p) {
    Float hc = clamp(h, avg - sigma * p.antilag_sigma, avg + sigma * p.antilag_sigma);
    Float d = abs(h - hc) / (max(h, hc) + 1e-6f);
    return 1.f / (1.f + d * count / max(p.antilag_sensitivity * p.frame_rate_scale * p.frame_rate_scale, 1e-6f));
}
}
