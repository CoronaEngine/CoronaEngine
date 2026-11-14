#pragma once
#include "base/using.h"

/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

/* Copyright (c) 2025 NVIDIA CORPORATION. All rights reserved.
 * Typed DSL translation of the matched PathPayload / StablePlanes serialization.
 * Original full notices and source remain in offline independent references.
 */
namespace vision::rtxpt_gi {
[[nodiscard]] inline Float payload_positive(const Float &v) noexcept {
    return ocarina::f16tof32(ocarina::f32tof16(clamp(v, 0.f, 65504.f)));
}
[[nodiscard]] inline Float3 payload_positive(const Float3 &v) noexcept {
    return make_float3(payload_positive(Float{v.x}), payload_positive(Float{v.y}), payload_positive(Float{v.z}));
}
[[nodiscard]] inline Float3 payload_signed(const Float3 &v) noexcept {
    Float3 value = clamp(v, make_float3(-65504.f), make_float3(65504.f));
    return make_float3(ocarina::f16tof32(ocarina::f32tof16(value.x)),
        ocarina::f16tof32(ocarina::f32tof16(value.y)), ocarina::f16tof32(ocarina::f32tof16(value.z)));
}
[[nodiscard]] inline Float3 payload_denoiser(const Float3 &v) noexcept {
    Float3 value = clamp(v, make_float3(0.f), make_float3(65504.f));
    return make_float3(ocarina::f16tof32(ocarina::f32tof16(value.x)),
        ocarina::f16tof32(ocarina::f32tof16(value.y)), ocarina::f16tof32(ocarina::f32tof16(value.z)));
}
[[nodiscard]] inline Float payload_firefly(const Float &v) noexcept {
    return saturate(ocarina::f16tof32(ocarina::f32tof16(v)));
}
[[nodiscard]] inline Float2 payload_cone(const Float &width, const Float &spread) noexcept {
    Uint packed = (ocarina::f32tof16(width) << 16u) | ocarina::f32tof16(spread);
    return make_float2(ocarina::f16tof32(packed >> 16u), ocarina::f16tof32(packed));
}
[[nodiscard]] inline Float4x4 payload_image_roundtrip(const Float4x4 &input) noexcept {
    Float3 a = input[0].xyz(), b = input[1].xyz(), c = input[2].xyz();
    Uint p0 = (ocarina::f32tof16(a.x) << 16u) | ocarina::f32tof16(a.y);
    Uint p1 = (ocarina::f32tof16(a.z) << 16u) | ocarina::f32tof16(b.x);
    Uint p2 = (ocarina::f32tof16(b.y) << 16u) | ocarina::f32tof16(b.z);
    Bool handedness = dot(cross(a, b), c) > 0.f;
    a = make_float3(ocarina::f16tof32(p0 >> 16u), ocarina::f16tof32(p0 & 0xffffu), ocarina::f16tof32(p1 >> 16u));
    b = make_float3(ocarina::f16tof32(p1 & 0xffffu), ocarina::f16tof32(p2 >> 16u), ocarina::f16tof32(p2 & 0xffffu));
    a = normalize(a); b = normalize(b);
    c = select(handedness, cross(a, b), cross(b, a));
    return make_float4x4(make_float4(a, 0.f), make_float4(b, 0.f), make_float4(c, 0.f), make_float4(0.f, 0.f, 0.f, 1.f));
}
[[nodiscard]] inline Float4 payload_stable_ray(const Float3 &direction, const Float &input_length) noexcept {
    Float3 packed = direction * clamp(input_length, 1e-7f, 1e15f);
    Float scene_length = length(packed);
    return make_float4(packed / scene_length, scene_length);
}
}// namespace vision::rtxpt_gi
