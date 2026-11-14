#pragma once

#include "rtxpt_gi_material.h"
#include "base/scattering/half_storage.h"

namespace vision {
// RTXPT PackedPathTracerSurfaceData, including its storage quantization.
struct RtxptGIStandardSurface {
    array<uint, 16> words{};
};
static_assert(sizeof(RtxptGIStandardSurface) == 64u);
}// namespace vision
OC_STRUCT(vision, RtxptGIStandardSurface, words) {};

namespace vision::rtxpt_gi {

[[nodiscard]] inline Float2 standard_oct(const Float3 &normal) noexcept {
    Float3 n = normal / (abs(normal.x) + abs(normal.y) + abs(normal.z));
    Float2 wrapped = (1.f - abs(n.yx())) * select(n.xy() >= make_float2(0.f), Float2{make_float2(1.f)}, Float2{make_float2(-1.f)});
    return select(n.z >= 0.f, n.xy(), wrapped) * .5f + .5f;
}
[[nodiscard]] inline Float3 standard_unoct(const Float2 &encoded) noexcept {
    Float2 f = encoded * 2.f - 1.f;
    Float3 n = make_float3(f, 1.f - abs(f.x) - abs(f.y));
    Float t = saturate(-n.z);
    n.x += select(n.x >= 0.f, -t, t);
    n.y += select(n.y >= 0.f, -t, t);
    return normalize(n);
}
[[nodiscard]] inline Uint standard_r11(const Float3 &rgb) noexcept {
    Uint2 half = pack_storage_half4(make_float4(rgb, 0.f));
    Uint r = half.x & 0xffffu, g = half.x >> 16u, b = half.y & 0xffffu;
    return ((r << 17u) & 0xffe00000u) | ((g << 6u) & 0x001ffc00u) | ((b >> 5u) & 0x000003ffu);
}
[[nodiscard]] inline Float3 standard_unr11(const Uint &rgb) noexcept {
    Uint r = (rgb >> 17u) & 0x7ff0u, g = (rgb >> 6u) & 0x7ff0u, b = (rgb << 5u) & 0x7fe0u;
    Float2 rg = unpack_storage_half2(r | (g << 16u));
    return make_float3(rg, unpack_storage_half2(b).x);
}
[[nodiscard]] inline Uint standard_plane_hash(Uint value) noexcept {
    value ^= 2747636419u;
    value *= 2654435769u;
    value ^= value >> 16u;
    value *= 2654435769u;
    value ^= value >> 16u;
    value *= 2654435769u;
    return value;
}

[[nodiscard]] inline RtxptGIStandardSurfaceVar pack_standard_surface(const StandardSurfaceView &surface) noexcept {
    RtxptGIStandardSurfaceVar packed;
    packed.words[10u] = 2u;
    $if(surface.valid) {
        packed.words[4u] = surface.material.header.x;
        packed.words[5u] = surface.material.header.y;
        packed.words[8u] = pack_storage_half2(standard_oct(surface.tangent));
        packed.words[9u] = pack_storage_half2(standard_oct(surface.normal));
        Float winding = -sign(dot(cross(surface.tangent, surface.normal), surface.bitangent));
        Uint2 v = pack_storage_half4(make_float4(surface.view, winding));
        packed.words[6u] = v.x;
        packed.words[7u] = v.y;
        packed.words[0u] = as<uint>(surface.it.pos.x);
        packed.words[1u] = as<uint>(surface.it.pos.y);
        packed.words[2u] = as<uint>(surface.it.pos.z);
        Float3 face = normalize(surface.it.ng) * select(surface.front_facing, 1.f, -1.f);
        packed.words[3u] = pack_storage_half2(standard_oct(face));
        Uint depth = pack_storage_half2(make_float2(surface.depth, 0.f)) & 0xffffu;
        packed.words[10u] = (depth << 16u) | (standard_plane_hash(surface.plane_hash) & 0xfffcu) | cast<uint>(surface.front_facing);
        packed.words[11u] = standard_r11(surface.material.p0.xyz());
        packed.words[12u] = standard_r11(surface.material.p1.xyz());
        packed.words[13u] = standard_r11(make_float3(surface.material.p0.w, surface.material.p1.w, surface.material.p3.y));
        packed.words[14u] = standard_r11(surface.material.p2.xyz());
        packed.words[15u] = pack_storage_half2(make_float2(surface.material.p2.w, surface.material.p3.x));
    };
    return packed;
}

[[nodiscard]] inline StandardSurfaceView unpack_standard_surface(const RtxptGIStandardSurfaceVar &packed) noexcept {
    StandardSurfaceView surface{Interaction{false}};
    surface.material.header = make_uint2(packed.words[4u], packed.words[5u]);
    surface.tangent = standard_unoct(unpack_storage_half2(packed.words[8u]));
    surface.normal = standard_unoct(unpack_storage_half2(packed.words[9u]));
    Float4 v = unpack_storage_half4(make_uint2(packed.words[6u], packed.words[7u]));
    surface.bitangent = normalize(cross(surface.normal, surface.tangent * v.w));
    surface.view = v.xyz();
    surface.it.wo = surface.view;
    surface.it.pos = make_float3(as<float>(packed.words[0u]), as<float>(packed.words[1u]), as<float>(packed.words[2u]));
    surface.it.ng = standard_unoct(unpack_storage_half2(packed.words[3u]));
    surface.it.shading.x = surface.tangent;
    surface.it.shading.y = surface.bitangent;
    surface.it.shading.z = surface.normal;
    surface.valid = ((packed.words[10u] >> 1u) & 1u) == 0u;
    surface.front_facing = (packed.words[10u] & 1u) != 0u;
    surface.depth = unpack_storage_half2(packed.words[10u] >> 16u).x;
    surface.plane_hash = packed.words[10u] & 0xfffcu;
    Float3 rme = standard_unr11(packed.words[13u]);
    Float2 transmission = unpack_storage_half2(packed.words[15u]);
    surface.material.p0 = make_float4(standard_unr11(packed.words[11u]), rme.x);
    surface.material.p1 = make_float4(standard_unr11(packed.words[12u]), rme.y);
    surface.material.p2 = make_float4(standard_unr11(packed.words[14u]), transmission.x);
    surface.material.p3 = make_float4(transmission.y, rme.z, 0.f, 0.f);
    return surface;
}

}// namespace vision::rtxpt_gi
