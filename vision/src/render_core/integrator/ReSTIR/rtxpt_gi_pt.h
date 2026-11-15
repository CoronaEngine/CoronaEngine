#pragma once

#include "rtxpt_gi_prefix_storage.h"
#include "rtxpt_gi_candidate.h"
#include "rtxpt_gi_surface.h"
#include "base/scattering/material.h"
#include "base/scattering/gi_principled_parameters.h"
#include "base/mgr/scene.h"

namespace vision {
// GI-only packed RGB Principled receiver. Numeric lobe parameters36B,
// geometry/identity/configuration48B, material-reuse guides12B. No Interaction.
struct RtxptGIPTSurface {
    float4 position_depth{};
    uint4 frame{}, encoded_view{};
    array<uint, 9> material{};
    array<uint, 3> guides{};
};
static_assert(sizeof(RtxptGIPTSurface) == 96u);
// Explicit validation only, never allocated by normal rendering.
struct RtxptGIPTReference {
    array<float4, 13> fields{};
    uint4 identity{}, metadata{}, hit_words{};
};
static_assert(sizeof(RtxptGIPTReference) == 256u);
}
OC_STRUCT(vision, RtxptGIPTSurface, position_depth, frame, encoded_view, material, guides) {};
OC_STRUCT(vision, RtxptGIPTReference, fields, identity, metadata, hit_words) {};

namespace vision::rtxpt_gi {
struct PTEval { Float3 diffuse{make_float3(0.f)}, specular{make_float3(0.f)}; };
struct PTSurfaceView {
    Interaction it{false};
    TriangleHitVar hit;
    Float3 normal{make_float3(0.f, 0.f, 1.f)};
    Float3 diffuse_guide{make_float3(0.f)}, specular_guide{make_float3(0.f)};
    Float2 roughness{make_float2(1.f)};
    Float depth{0.f};
    Uint plane_hash{0u};
    Bool valid{false};
    std::array<Float4, 6> parameters{make_float4(0.f), make_float4(0.f), make_float4(0.f),
                                   make_float4(0.f), make_float4(0.f), make_float4(0.f)};

    void load_parameters() const noexcept {
        for (uint i = 0u; i < 6u; ++i) gi_principled::row(i) = parameters[i];
    }
    [[nodiscard]] Float3 f(const Float3 &wi, const SampledWavelengths &swl) const noexcept {
        gi_principled::CompilationScope decode{true};
        load_parameters();
        Float3 result = make_float3(0.f);
        Toolkit::scene().materials().dispatch(it.material_id(), [&](const Material *material) {
            auto bsdf = material->create_evaluator(it, swl);
            result = Toolkit::spectrum()->linear_srgb(bsdf.evaluate(it.wo, wi, MaterialEvalMode::F).f, swl);
        });
        return result;
    }
    // Final MIS evaluates a roughened copy of the complete receiver. The
    // original parameters and true transport evaluation remain unchanged.
    [[nodiscard]] Float3 f_roughness_clamp(const Float &minimum_roughness, const Float3 &wi,
                                          const SampledWavelengths &swl) const noexcept {
        PTSurfaceView rough = *this;
        rough.parameters[0].w = max(rough.parameters[0].w, minimum_roughness);
        rough.parameters[5].w = as<float>(as<uint>(rough.parameters[5].w) &
            ~(gi_principled::BaseNearBit | gi_principled::IdealGlassBit));
        return rough.f(wi, swl);
    }
    [[nodiscard]] PTEval evaluate(const Float3 &wi, const SampledWavelengths &swl) const noexcept {
        gi_principled::CompilationScope decode{true};
        load_parameters();
        PTEval result;
        Toolkit::scene().materials().dispatch(it.material_id(), [&](const Material *material) {
            auto bsdf = material->create_evaluator(it, swl);
            Float3 total = Toolkit::spectrum()->linear_srgb(bsdf.evaluate(it.wo, wi, MaterialEvalMode::F).f, swl);
            Float3 fraction = bsdf.specular_fraction_rgb(it.wo, wi);
            result.specular = total * fraction;
            result.diffuse = total - result.specular;
        });
        return result;
    }
};

[[nodiscard]] inline RtxptGIPTSurfaceVar pack_pt_surface(const PTSurfaceView &s) noexcept {
    RtxptGIPTSurfaceVar p;
    $if(s.valid) {
        p.position_depth = make_float4(s.it.pos, s.depth);
        p.frame = make_uint4(pack_storage_half2(standard_oct(normalize(s.it.ng))),
            pack_storage_half2(standard_oct(s.it.shading.z)),
            pack_storage_half2(standard_oct(s.it.shading.x)), s.it.material_id());
        Float handedness = sign(dot(cross(s.it.shading.z, s.it.shading.x), s.it.shading.y));
        Uint2 view = pack_storage_half4(make_float4(s.it.wo, handedness));
        p.encoded_view = make_uint4(view, s.plane_hash, as<uint>(s.parameters[5].w) | gi_principled::ValidBit);
        for (uint i = 0u; i < 4u; ++i) p.material[i] = standard_r11(s.parameters[i].xyz());
        p.material[4u] = pack_storage_half2(make_float2(s.parameters[0].w, s.parameters[1].w));
        p.material[5u] = pack_storage_half2(s.parameters[4].xy());
        p.material[6u] = pack_storage_half2(s.parameters[4].zw());
        p.material[7u] = pack_storage_half2(make_float2(s.parameters[2].w, s.parameters[5].x));
        p.material[8u] = pack_storage_half2(make_float2(s.parameters[3].w, s.parameters[5].y));
        p.guides[0u] = standard_r11(s.diffuse_guide);
        p.guides[1u] = standard_r11(s.specular_guide);
        p.guides[2u] = pack_storage_half2(s.roughness);
    };
    return p;
}
[[nodiscard]] inline PTSurfaceView unpack_pt_surface(const RtxptGIPTSurfaceVar &p) noexcept {
    PTSurfaceView s;
    s.it.pos = p.position_depth.xyz(); s.depth = p.position_depth.w;
    s.it.ng = standard_unoct(unpack_storage_half2(p.frame.x));
    s.it.shading.z = standard_unoct(unpack_storage_half2(p.frame.y));
    s.it.shading.x = standard_unoct(unpack_storage_half2(p.frame.z));
    Float4 view = unpack_storage_half4(p.encoded_view.xy());
    s.it.wo = view.xyz();
    s.it.shading.y = normalize(cross(s.it.shading.z, s.it.shading.x)) * view.w;
    s.it.set_material(p.frame.w);
    s.normal = s.it.shading.z;
    s.plane_hash = p.encoded_view.z;
    s.valid = (p.encoded_view.w & gi_principled::ValidBit) != 0u;
    Float2 rm = unpack_storage_half2(p.material[4u]);
    Float2 sw = unpack_storage_half2(p.material[7u]), cw = unpack_storage_half2(p.material[8u]);
    s.parameters[0] = make_float4(standard_unr11(p.material[0u]), rm.x);
    s.parameters[1] = make_float4(standard_unr11(p.material[1u]), rm.y);
    s.parameters[2] = make_float4(standard_unr11(p.material[2u]), sw.x);
    s.parameters[3] = make_float4(standard_unr11(p.material[3u]), cw.x);
    s.parameters[4] = make_float4(unpack_storage_half2(p.material[5u]), unpack_storage_half2(p.material[6u]));
    s.parameters[5] = make_float4(sw.y, cw.y, 0.f, as<float>(p.encoded_view.w));
    s.diffuse_guide = standard_unr11(p.guides[0u]);
    s.specular_guide = standard_unr11(p.guides[1u]);
    s.roughness = unpack_storage_half2(p.guides[2u]);
    return s;
}

[[nodiscard]] inline RtxptGIPTReferenceVar pack_pt_reference(const PTSurfaceView &s) noexcept {
    RtxptGIPTReferenceVar p;
    p.fields[0] = make_float4(s.it.pos, s.it.prim_area);
    p.fields[1] = make_float4(s.it.wo, s.depth);
    p.fields[2] = make_float4(s.it.time, s.roughness.x);
    p.fields[3] = make_float4(s.it.ng, s.roughness.y);
    p.fields[4] = make_float4(s.it.ng_local, 0.f);
    p.fields[5] = make_float4(s.it.shading.x, s.it.uv.x);
    p.fields[6] = make_float4(s.it.shading.y, s.it.uv.y);
    p.fields[7] = make_float4(s.it.shading.z, s.it.du_dx);
    p.fields[8] = make_float4(s.it.shading.dn_du, s.it.du_dy);
    p.fields[9] = make_float4(s.it.shading.dn_dv, s.it.dv_dx);
    p.fields[10] = make_float4(s.diffuse_guide, s.it.dv_dy);
    p.fields[11] = make_float4(s.specular_guide, 0.f);
    p.fields[12] = make_float4(s.normal, 0.f);
    p.identity = make_uint4(s.it.material_id(), s.it.light_id(), s.it.inst_id, s.it.prim_id);
    p.metadata = make_uint4(cast<uint>(s.valid), s.plane_hash, 0u, 0u);
    p.hit_words = make_uint4(s.hit.inst_id, s.hit.prim_id, as<uint2>(Float2{s.hit.bary}));
    return p;
}

[[nodiscard]] inline PTSurfaceView unpack_pt_reference(const RtxptGIPTReferenceVar &p) noexcept {
    PTSurfaceView s;
    s.it.pos = p.fields[0].xyz(); s.it.prim_area = p.fields[0].w;
    s.it.wo = p.fields[1].xyz(); s.depth = p.fields[1].w;
    s.it.time = p.fields[2].xyz(); s.roughness.x = p.fields[2].w;
    s.it.ng = p.fields[3].xyz(); s.roughness.y = p.fields[3].w;
    s.it.ng_local = p.fields[4].xyz();
    s.it.shading.x = p.fields[5].xyz(); s.it.uv.x = p.fields[5].w;
    s.it.shading.y = p.fields[6].xyz(); s.it.uv.y = p.fields[6].w;
    s.it.shading.z = p.fields[7].xyz(); s.it.du_dx = p.fields[7].w;
    s.it.shading.dn_du = p.fields[8].xyz(); s.it.du_dy = p.fields[8].w;
    s.it.shading.dn_dv = p.fields[9].xyz(); s.it.dv_dx = p.fields[9].w;
    s.diffuse_guide = p.fields[10].xyz(); s.it.dv_dy = p.fields[10].w;
    s.specular_guide = p.fields[11].xyz(); s.normal = p.fields[12].xyz();
    s.it.set_material(p.identity.x); s.it.set_light(p.identity.y);
    s.it.inst_id = p.identity.z; s.it.prim_id = p.identity.w;
    s.valid = p.metadata.x != 0u; s.plane_hash = p.metadata.y;
    s.hit.inst_id = p.hit_words.x; s.hit.prim_id = p.hit_words.y;
    s.hit.bary = as<float2>(Uint2{p.hit_words.zw()});
    return s;
}

void build_pt_prefix(const PrefixStorage &storage, ReSTIRGI &env) noexcept;
}
