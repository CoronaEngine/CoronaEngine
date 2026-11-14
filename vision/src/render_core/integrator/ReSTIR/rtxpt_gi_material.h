#pragma once

#include "rtxpt_gi_standard_bsdf.h"
#include "base/scattering/material.h"
#include "base/mgr/scene.h"
#include "base/mgr/registries.h"

namespace vision::rtxpt_gi {

namespace detail {
// Typed DSL translation of PathTracerBridgeDonut::ApplyNormalMapRTXPT.
/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/
[[nodiscard]] inline Float3 apply_native_normal_map(const Float3 &normal, const Float3 &tangent,
                                                    const Float &winding, const Float3 &encoded,
                                                    const Float &scale) noexcept {
    Float3 result = normal;
    Float square_tangent_length = dot(tangent, tangent);
    $if(square_tangent_length != 0.f && winding != 0.f) {
        Float3 map = encoded;
        map.x = map.x * 2.f - 1.f;
        map.y = map.y * 2.f - 1.f;
        map.x *= scale; map.y *= scale;
        $if(map.z <= 0.f) { map.z = sqrt(saturate(1.f - map.x * map.x - map.y * map.y)); }
        $else { map.z = abs(map.z * 2.f - 1.f); };
        Float square_map_length = dot(map, map);
        $if(square_map_length != 0.f) {
            Float map_length = sqrt(square_map_length);
            Float3 local_normal = map / map_length;
            Float3 t = tangent;
            t *= rsqrt(square_tangent_length);
            Float3 bitangent = cross(normal, t) * winding;
            result = normalize(t * local_normal.x + bitangent * local_normal.y + normal * local_normal.z);
        };
    };
    return result;
}
}// namespace detail

// Uncompressed vertex input. Only GI owns the corresponding packed history.
struct StandardSurfaceView {
    Interaction it;
    StandardMaterial material;
    Float3 tangent, bitangent, normal, view;
    Bool valid{false};
    Bool front_facing{true};
    Float depth{0.f};
    Float interior_ior{1.5f};
    Uint plane_hash{1u};

    explicit StandardSurfaceView(const Interaction &interaction)
        : it(interaction), tangent(interaction.shading.x), bitangent(interaction.shading.y),
          normal(interaction.shading.z), view(interaction.wo) {}
    [[nodiscard]] Float3 to_local(const Float3 &direction) const noexcept {
        return make_float3(dot(direction, tangent), dot(direction, bitangent), dot(direction, normal));
    }
    [[nodiscard]] Float3 to_world(const Float3 &direction) const noexcept {
        return tangent * direction.x + bitangent * direction.y + normal * direction.z;
    }
    [[nodiscard]] StandardEval evaluate(const Float3 &direction, bool rough = false) const noexcept {
        StandardMaterial data = material;
        if (rough) data.p0.w = max(data.p0.w, .2f);
        return standard_eval(data, normal, view, to_local(view), to_local(direction));
    }
    [[nodiscard]] Float pdf(const Float3 &direction) const noexcept {
        return standard_pdf(material, normal, view, to_local(view), to_local(direction));
    }
    [[nodiscard]] StandardSample sample(const Float3 &random) const noexcept {
        auto result = standard_sample(material, normal, view, to_local(view), random);
        result.direction = to_world(result.direction);
        return result;
    }
};

[[nodiscard]] inline Float3 standard_perpendicular(const Float3 &u) noexcept {
    Float3 a = abs(u);
    Uint uyx = select((a.x - a.y) < 0.f, 1u, 0u);
    Uint uzx = select((a.x - a.z) < 0.f, 1u, 0u);
    Uint uzy = select((a.y - a.z) < 0.f, 1u, 0u);
    Uint xm = uyx & uzx, ym = (1u ^ xm) & uzy, zm = 1u ^ (xm | ym);
    return normalize(cross(u, make_float3(cast<float>(xm), cast<float>(ym), cast<float>(zm))));
}

inline void standard_tangent_frame(const Float3 &n, const Float3 &raw_tangent,
                                    const Float &winding, Float3 &t, Float3 &b) noexcept {
    Float cosine = dot(raw_tangent, n);
    $if(winding != 0.f && dot(raw_tangent, raw_tangent) > 0.f && abs(cosine) < .9999f) {
        t = normalize(raw_tangent - n * cosine);
        b = cross(n, t) * winding;
    } $else {
        t = standard_perpendicular(n);
        b = cross(n, t);
    };
}

[[nodiscard]] inline Float standard_number(const Material &material, string_view name,
                                           const Interaction &it, const SampledWavelengths &swl,
                                           float fallback) noexcept {
    const auto *slot = material.input_slot(name);
    return slot && slot->valid() ? slot->evaluate(it, swl)->as_scalar() : Float{fallback};
}

// The bridge follows PathTracerBridgeDonut's metallic/roughness conversion.
// It reads shader inputs directly and never constructs a Corona BSDF/lobe.
[[nodiscard]] inline StandardSurfaceView load_standard_surface(
    const Interaction &it, const SampledWavelengths &swl,
    const Float &outside_ior = Float{1.f}, const Float *ior_override = nullptr) noexcept {
    StandardSurfaceView surface{it};
    Toolkit::scene().materials().dispatch(it.material_id(), [&](const Material *source) {
        Float3 color = make_float3(1.f);
        if (const auto *slot = source->input_slot("color"); slot && slot->valid()) {
            color = slot->eval_albedo_spectrum(it, swl).sample.vec3();
        }
        Float metallic = saturate(standard_number(*source, "metallic", it, swl, 0.f));
        Float roughness = saturate(standard_number(*source, "roughness", it, swl, .5f));
        Float ior = ior_override ? *ior_override : standard_number(*source, "ior", it, swl, 1.5f);
        Float specular_transmission = saturate(standard_number(*source, "transmission_weight", it, swl, 0.f)) * (1.f - metallic);
        Float diffuse_transmission = saturate(standard_number(*source, "diffuse_transmission", it, swl, 0.f)) * (1.f - metallic);
        Bool front = dot(it.ng, it.wo) >= 0.f;
        surface.front_facing = front;
        surface.interior_ior = ior;
        Float eta = select(front, outside_ior / ior, ior / outside_ior);
        Float f = (ior - 1.f) / (ior + 1.f), f0 = f * f;
        surface.material.p0 = make_float4(lerp(metallic, color, make_float3(0.f)), roughness);
        surface.material.p1 = make_float4(lerp(metallic, make_float3(f0), color), metallic);
        surface.material.p2 = make_float4(color, diffuse_transmission);
        surface.material.p3 = make_float4(specular_transmission, eta, 0.f, 0.f);
        // Native Donut defaults: all active lobes, nested priority15, optional thin flag.
        const bool thin = source->source_desc()["thin_surface"].as_bool(false);
        surface.material.header = make_uint2(0x000f0000u | (0xffu << 20u) | (thin ? (1u << 29u) : 0u), 0u);
        Float3 raw_tangent = it.shading.x;
        Float3 base_normal = normalize(it.shading.z);
        Float winding = sign(dot(cross(base_normal, raw_tangent), it.shading.y));
        Float3 base_tangent = make_float3(0.f), base_bitangent = make_float3(0.f);
        standard_tangent_frame(base_normal, raw_tangent, winding, base_tangent, base_bitangent);
        Float3 normal = base_normal;
        if (const auto *slot = source->input_slot("normal"); slot && slot->valid()) {
            const auto *slots = dynamic_cast<const ShaderNodeSlotSet *>(slot->node());
            const auto *color = slots ? slots->input_slot("color") : nullptr;
            if (slot->node()->impl_type() == "normal_map" && color && color->valid()) {
                Float scale = 1.f;
                if (const auto *strength = slots->input_slot("strength"); strength && strength->valid())
                    scale = strength->evaluate(it, swl)->as_scalar();
                // Read encoded RGB before Corona NormalMap normalization or
                // clamp_ns. Legacy scaled EXR inputs already contain XY scale.
                normal = detail::apply_native_normal_map(base_normal, raw_tangent, winding,
                                                         color->evaluate(it, swl)->as_vec3(), scale);
            } else {
                Float3 mapped = slot->evaluate(it, swl)->as_vec3();
                normal = normalize(base_tangent * mapped.x + base_bitangent * mapped.y + base_normal * mapped.z);
            }
        }
        surface.normal = normal * select(front, 1.f, -1.f);
        Float3 face = normalize(it.ng) * select(front, 1.f, -1.f);
        Float sign_n = select(dot(surface.normal, face) >= 0.f, 1.f, -1.f);
        Float3 ns = sign_n * surface.normal;
        Float cosine = dot(it.wo, ns);
        $if(cosine <= .1f) {
            surface.normal = sign_n * normalize(lerp(saturate(cosine * 10.f), face, ns));
        };
        standard_tangent_frame(surface.normal, raw_tangent, winding, surface.tangent, surface.bitangent);
        surface.view = it.wo;
        surface.depth = Toolkit::scene().sensor()->linear_depth(it.pos);
        surface.valid = true;
    });
    return surface;
}

}// namespace vision::rtxpt_gi
