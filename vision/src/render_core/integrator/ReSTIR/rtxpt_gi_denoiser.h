#pragma once
#include "render_core/integrator/ReSTIR/rtxpt_gi_standard_bsdf.h"
#include "render_core/integrator/ReSTIR/rtxpt_gi_payload.h"
#include "render_core/denoiser/ReBLUR/filter.h"

/*
 * Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto. Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 * Typed DSL translation of the matched StandardBSDF::getProperties,
 * BSDFProperties::estimateSpecDiffBSDF and StablePlanes helper mathematics.
 */
namespace vision::rtxpt_gi {
static_assert(sizeof(reblur::Guide) == 80u);
static_assert(offsetof(reblur::Guide, position) == 0u && offsetof(reblur::Guide, identity) == 16u);
static_assert(offsetof(reblur::Guide, normal_roughness) == 32u && offsetof(reblur::Guide, diffuse_factor) == 48u && offsetof(reblur::Guide, specular_factor) == 64u);

// Host pointers are only dereferenced while building an enabled kernel AST.
// Disabled storage captures no resource and creates no extra GPU expressions.
struct NativeReblurStorage {
    Buffer<float4> *diffuse{}, *specular{}, *bypass{};
    Buffer<reblur::Guide> *guides{};
    uint pixels{};
    [[nodiscard]] bool enabled() const noexcept { return guides != nullptr; }
    [[nodiscard]] Uint address(const Uint &pixel, const Uint &plane) const noexcept { return plane * pixels + pixel; }
    void write(const Uint &pixel, const Uint &plane, const Float4 &d, const Float4 &s) const noexcept {
        const Uint i = address(pixel, plane); diffuse->write(i, d); specular->write(i, s);
    }
    [[nodiscard]] Float4 read_diffuse(const Uint &pixel, const Uint &plane) const noexcept { return diffuse->read(address(pixel, plane)); }
    [[nodiscard]] Float4 read_specular(const Uint &pixel, const Uint &plane) const noexcept { return specular->read(address(pixel, plane)); }
    void write_guide(const Uint &pixel, const Uint &plane, const reblur::GuideVar &guide) const noexcept { guides->write(address(pixel, plane), guide); }
};

[[nodiscard]] inline Float native_reblur_hit_t(const Float &previous, const Float &segment,
                                               const Uint &bounces, const Bool &delta_only) noexcept {
    Float result = previous;
    $if(bounces == 1u) { result = segment; }
    $elif(bounces > 1u && bounces <= 3u && delta_only) { result = previous + segment; };
    return result;
}

[[nodiscard]] inline Float native_reblur_combine_hit(const Float3 &previous, const Float &previous_hit,
                                                    const Float3 &incoming, const Float &new_hit) noexcept {
    Float result = previous_hit;
    Float l_new = dot(incoming, make_float3(.2126f, .7152f, .0722f));
    $if(!(l_new < 1e-5f)) {
        Float l_old = dot(previous, make_float3(.2126f, .7152f, .0722f));
        Float w = l_new / (l_old + l_new);
        result = abs(previous_hit) * (1.f - w) + new_hit * w;
    };
    return result;
}
[[nodiscard]] inline Float4 native_reblur_commit(const Float4 &previous, const Float3 &incoming,
                                                const Float &new_hit) noexcept {
    Float4 result = previous;
    Float l_new = dot(incoming, make_float3(.2126f, .7152f, .0722f));
    $if(!(l_new < 1e-5f)) {
        Float h = native_reblur_combine_hit(Float3{previous.xyz()}, Float{previous.w}, incoming, new_hit);
        result = make_float4(previous.xyz() + incoming, h);
    };
    return result;
}
[[nodiscard]] inline Float native_reblur_half_signed(const Float &value) noexcept {
    return f16tof32(f32tof16(clamp(value, -65504.f, 65504.f)));
}
[[nodiscard]] inline Float4 native_reblur_half(const Float4 &value) noexcept {
    return make_float4(native_reblur_half_signed(Float{value.x}), native_reblur_half_signed(Float{value.y}),
        native_reblur_half_signed(Float{value.z}), native_reblur_half_signed(Float{value.w}));
}
[[nodiscard]] inline Uint native_reblur_motion_flags(const Float &depth_motion) noexcept {
    return 3u | (f32tof16(clamp(depth_motion, -65504.f, 65504.f)) << 16u);
}

// Matched height-correlated GGX approximation, same coefficient matrices,
// row-vector products, rcp and final mad as Microfacet.hlsli.
[[nodiscard]] inline Float3 native_reblur_specular_integral(const Float3 &reflectance,
                                                           const Float &alpha, const Float &cosine) noexcept {
    Float c = abs(cosine), c2 = c * c, c3 = c * c2;
    Float a2 = alpha * alpha, a3 = alpha * a2;
    Float2 x2 = make_float2(1.f, c), y2 = make_float2(1.f, alpha);
    Float3 xyw = make_float3(1.f, c, c3), xzw = make_float3(1.f, c2, c3), yyw = make_float3(1.f, alpha, a3);
    Float2 m1 = make_float2(dot(make_float2(.995367f, -1.38839f), x2), dot(make_float2(-.24751f, 1.97442f), x2));
    Float3 m2 = make_float3(dot(make_float3(1.f, 2.68132f, 52.366f), xyw),
        dot(make_float3(16.0932f, -3.98452f, 59.3013f), xyw), dot(make_float3(-5.18731f, 255.259f, 2544.07f), xyw));
    Float2 m3 = make_float2(dot(make_float2(-.0564526f, 3.82901f), x2), dot(make_float2(16.91f, -11.0303f), x2));
    Float3 m4 = make_float3(dot(make_float3(1.f, 4.11118f, -1.37886f), xzw),
        dot(make_float3(19.3254f, -28.9947f, 16.9514f), xzw), dot(make_float3(.545386f, 96.0994f, -79.4492f), xzw));
    Float bias = dot(m1, y2) * rcp(dot(m2, yyw));
    Float scale = dot(m3, y2) * rcp(dot(m4, yyw));
    Float luma = dot(reflectance, make_float3(1.f / 3.f));
    bias *= saturate(luma * 50.f);
    return fma(reflectance, make_float3(max(0.f, scale)), make_float3(max(0.f, bias)));
}
[[nodiscard]] inline StandardEval native_reblur_estimates(const StandardMaterial &m,
                                                         const Float3 &normal, const Float3 &view) noexcept {
    Float roughness = m.p0.w;
    $if(roughness * roughness < .0064f) { roughness = 0.f; };
    Float3 dr = (1.f - m.p2.w) * (1.f - m.p3.x) * m.p0.xyz();
    Float3 dt = m.p2.w * m.p2.xyz() * (1.f - m.p3.x);
    Float3 sr = (1.f - m.p3.x) * m.p1.xyz();
    Float3 st = m.p3.x * m.p2.xyz();
    Float3 d = dr + dt;
    Float ndv = saturate(dot(normal, view));
    Float alpha = roughness * roughness;
    Float3 s = native_reblur_specular_integral(sr, alpha, ndv);
    s += st;
    // Native StoreStablePlaneCenter clamps and half-packs the estimates.
    // Prefix throughput intentionally does not enter these modulation factors.
    Float3 dc = clamp(d, make_float3(.04f), make_float3(65504.f));
    Float3 sc = clamp(s, make_float3(.04f), make_float3(65504.f));
    return {payload_signed(dc), payload_signed(sc)};
}
}// namespace vision::rtxpt_gi
