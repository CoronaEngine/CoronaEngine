#pragma once

#include "base/using.h"

/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/
// GI-only typed DSL translation of the matched native BxDF/Fresnel/Microfacet
// implementation. Provenance and source hashes remain in the offline native
// source manifest. Configuration: Frostbite diffuse, correlated Smith GGX,
// bounded VNDF, delta enabled, recycled selection sample, FP32 parameters.

namespace vision::rtxpt_gi {

struct StandardMaterial {
    // p0=(diffuse,roughness), p1=(specularF0,metallic),
    // p2=(transmissionColor,diffuseTransmission), p3=(specularTransmission,eta,0,0).
    Float4 p0, p1, p2, p3;
    Uint2 header;
};
struct StandardEval { Float3 diffuse, specular; };
struct StandardSample {
    Float3 direction, weight;
    Float pdf, lobe_probability;
    Uint lobe;
    Bool valid;
};
struct StandardDeltaLobe {
    Float3 direction, throughput;
    Float probability;
    Bool transmission;
};
struct StandardDeltaLobes {
    std::array<StandardDeltaLobe, 3> lobes;
    Uint count;
    Float non_delta_part;
};

namespace standard_detail {
inline constexpr float Pi = 3.14159265358979323846f;
inline constexpr float InvPi = .31830988618379067154f;
inline constexpr float MinCos = 1e-6f, MinAlpha = .0064f;
inline constexpr float OneMinusEpsilon = 0x1.fffffep-1f;
inline constexpr uint DiffuseReflection = 0x01u, SpecularReflection = 0x02u, DeltaReflection = 0x04u;
inline constexpr uint DiffuseTransmission = 0x10u, SpecularTransmission = 0x20u, DeltaTransmission = 0x40u;

[[nodiscard]] inline Float3 schlick(const Float3 &f0, const Float &f90, const Float &cosine) noexcept {
    return f0 + (f90 - f0) * pow(max(1.f - cosine, 0.f), 5.f);
}
[[nodiscard]] inline Float native_sign(const Float &value) noexcept {
    // DSL sign deliberately returns +1 at zero; HLSL sign returns zero.
    Float result = 0.f;
    $if(value > 0.f) { result = 1.f; } $elif(value < 0.f) { result = -1.f; };
    return result;
}
[[nodiscard]] inline Float schlick(const Float &f0, const Float &f90, const Float &cosine) noexcept {
    return f0 + (f90 - f0) * pow(max(1.f - cosine, 0.f), 5.f);
}
struct FresnelResult { Float value, cos_transmitted; };
[[nodiscard]] inline FresnelResult dielectric(const Float &relative_eta, const Float &cosine) noexcept {
    Float eta = relative_eta, ci = cosine, ct = 0.f, value = 1.f;
    $if(ci < 0.f) { eta = 1.f / eta; ci = -ci; };
    Float sin2 = eta * eta * (1.f - ci * ci);
    $if(!(sin2 > 1.f)) {
        ct = sqrt(1.f - sin2);
        Float rs = (eta * ci - ct) / (eta * ci + ct);
        Float rp = (eta * ct - ci) / (eta * ct + ci);
        value = .5f * (rs * rs + rp * rp);
    };
    return {value, ct};
}
[[nodiscard]] inline Float ndf(const Float &alpha, const Float &cosine) noexcept {
    Float a2 = alpha * alpha;
    Float d = ((cosine * a2 - cosine) * cosine + 1.f);
    return a2 / (d * d * Pi);
}
[[nodiscard]] inline Float lambda(const Float &alpha2, const Float &cosine) noexcept {
    Float result = 0.f;
    $if(!(cosine <= 0.f)) {
        Float c2 = cosine * cosine;
        Float t2 = max(1.f - c2, 0.f) / c2;
        result = .5f * (-1.f + sqrt(1.f + alpha2 * t2));
    };
    return result;
}
[[nodiscard]] inline Float masking(const Float &alpha, const Float &ci, const Float &co) noexcept {
    Float alpha2 = alpha * alpha;
    Float li = lambda(alpha2, ci), lo = lambda(alpha2, co);
    return 1.f / (1.f + li + lo);
}
[[nodiscard]] inline Float bounded_pdf(const Float &alpha, const Float3 &i, const Float3 &m) noexcept {
    Float density = ndf(alpha, m.z);
    Float2 ai = make_float2(alpha) * i.xy();
    Float len2 = dot(ai, ai);
    Float t = sqrt(len2 + i.z * i.z);
    Float a = saturate(alpha), s = 1.f + length(Float2{i.xy()});
    Float a2 = a * a, s2 = s * s;
    Float k = (1.f - a2) * s2 / (s2 + a2 * i.z * i.z);
    return density / (2.f * (k * i.z + t));
}
[[nodiscard]] inline Float3 bounded_sample(const Float &alpha, const Float3 &i, const Float2 &random) noexcept {
    Float3 i_std = normalize(make_float3(i.xy() * make_float2(alpha), i.z));
    Float phi = 2.f * Pi * random.x;
    Float a = saturate(alpha), s = 1.f + length(Float2{i.xy()});
    Float a2 = a * a, s2 = s * s;
    Float k = (1.f - a2) * s2 / (s2 + a2 * i.z * i.z);
    Float b = select(i.z > 0.f, k * i_std.z, i_std.z);
    Float z = fma(1.f - random.y, 1.f + b, -b);
    Float sin_theta = sqrt(saturate(1.f - z * z));
    Float3 o_std = make_float3(sin_theta * cos(phi), sin_theta * sin(phi), z);
    Float3 m_std = i_std + o_std;
    return normalize(make_float3(m_std.xy() * make_float2(alpha), m_std.z));
}
[[nodiscard]] inline Float3 cosine_sample(const Float2 &random, Float &pdf) noexcept {
    Float2 u = 2.f * random - 1.f, disk = u;
    $if(!(u.x == 0.f && u.y == 0.f)) {
        Float phi = 0.f, r = 0.f;
        $if(abs(u.x) > abs(u.y)) { r = u.x; phi = (u.y / u.x) * (Pi / 4.f); }
        $else { r = u.y; phi = Pi / 2.f - (u.x / u.y) * (Pi / 4.f); };
        disk = r * make_float2(cos(phi), sin(phi));
    };
    Float z = sqrt(max(0.f, 1.f - dot(disk, disk)));
    pdf = z * InvPi;
    return make_float3(disk, z);
}

struct Model {
    Float3 diffuse, specular, transmission;
    Float roughness, alpha, transmission_alpha, eta, diff_trans, spec_trans;
    Float p_dr, p_dt, p_sr, p_srt;
    Uint active;
    Bool psd_exclude;

    Model(const StandardMaterial &m, const Float3 &normal, const Float3 &view) noexcept
        : diffuse(m.p0.xyz()), specular(m.p1.xyz()), transmission(make_float3(0.f)),
          roughness(m.p0.w), alpha(m.p0.w * m.p0.w), transmission_alpha(0.f),
          eta(m.p3.y), diff_trans(m.p2.w), spec_trans(m.p3.x),
          p_dr(0.f), p_dt(0.f), p_sr(0.f), p_srt(0.f),
          active((m.header.x >> 20u) & 0xffu), psd_exclude((m.header.y & (1u << 17u)) != 0u) {
        $if((m.header.x & (1u << 29u)) != 0u) { transmission = m.p2.xyz(); }
        $else { transmission = sqrt(Float3{m.p2.xyz()}); };
        $if(alpha < MinAlpha) { alpha = 0.f; };
        transmission_alpha = select(eta == 1.f, 0.f, alpha);
        Float metallic_brdf = m.p1.w * (1.f - spec_trans);
        Float dielectric_bsdf = (1.f - m.p1.w) * (1.f - spec_trans);
        Float specular_bsdf = spec_trans;
        Float diffuse_weight = dot(diffuse, make_float3(.2126f, .7152f, .0722f));
        // The native ShadingData overload swaps N,V when forwarding to __init;
        // its only use here is the symmetric dot product, kept in native order.
        Float specular_weight = dot(schlick(specular, Float{1.f}, dot(normal, view)), make_float3(.2126f, .7152f, .0722f));
        p_dr = select((active & DiffuseReflection) != 0u, diffuse_weight * dielectric_bsdf * (1.f - diff_trans), 0.f);
        p_dt = select((active & DiffuseTransmission) != 0u, diffuse_weight * dielectric_bsdf * diff_trans, 0.f);
        p_sr = select((active & (SpecularReflection | DeltaReflection)) != 0u, specular_weight * (metallic_brdf + dielectric_bsdf), 0.f);
        p_srt = select((active & (SpecularReflection | DeltaReflection | SpecularTransmission | DeltaTransmission)) != 0u, specular_bsdf, 0.f);
        Float norm = p_dr + p_dt + p_sr + p_srt;
        $if(norm > 0.f) {
            norm = 1.f / norm;
            p_dr *= norm; p_dt *= norm; p_sr *= norm; p_srt *= norm;
        };
    }
    [[nodiscard]] Bool has(uint lobe) const noexcept { return (active & lobe) != 0u; }

    [[nodiscard]] Float3 diffuse_weight(const Float3 &wi, const Float3 &wo) const noexcept {
        Float3 h = normalize(wi + wo);
        Float wo_h = dot(wo, h);
        Float energy_bias = lerp(roughness, 0.f, .5f);
        Float energy_factor = lerp(roughness, 1.f, 1.f / 1.51f);
        Float fd90 = energy_bias + 2.f * wo_h * wo_h * roughness;
        Float fi = schlick(Float{1.f}, fd90, Float{wi.z});
        Float fo = schlick(Float{1.f}, fd90, Float{wo.z});
        return diffuse * fi * fo * energy_factor;
    }
    [[nodiscard]] Float3 diffuse_eval(const Float3 &wi, const Float3 &wo, bool transmitted) const noexcept {
        Float3 result = make_float3(0.f);
        Float co = transmitted ? Float{-wo.z} : Float{wo.z};
        $if(!(min(wi.z, co) < MinCos)) {
            if (transmitted) result = InvPi * transmission * -wo.z;
            else result = diffuse_weight(wi, wo) * InvPi * wo.z;
        };
        return result;
    }
    [[nodiscard]] Float diffuse_pdf(const Float3 &wi, const Float3 &wo, bool transmitted) const noexcept {
        Float co = transmitted ? Float{-wo.z} : Float{wo.z}, result = 0.f;
        $if(!(min(wi.z, co) < MinCos)) { result = InvPi * co; };
        return result;
    }
    [[nodiscard]] StandardSample diffuse_sample(const Float3 &wi, const Float3 &u, bool transmitted) const noexcept {
        StandardSample r{make_float3(0.f), make_float3(0.f), Float{0.f}, Float{0.f}, Uint{transmitted ? DiffuseTransmission : DiffuseReflection}, Bool{false}};
        r.direction = cosine_sample(Float2{u.xy()}, r.pdf);
        if (transmitted) r.direction.z = -r.direction.z;
        Float co = transmitted ? Float{-r.direction.z} : Float{r.direction.z};
        $if(!(min(wi.z, co) < MinCos)) {
            r.weight = transmitted ? Float3{transmission} : diffuse_weight(wi, r.direction);
            r.lobe_probability = 1.f; r.valid = true;
        };
        return r;
    }
    [[nodiscard]] Float3 reflection_eval(const Float3 &wi, const Float3 &wo) const noexcept {
        Float3 result = make_float3(0.f);
        $if(!(min(wi.z, wo.z) < MinCos) && alpha != 0.f && has(SpecularReflection)) {
            Float3 h = normalize(wi + wo);
            Float wi_h = dot(wi, h), d = ndf(alpha, Float{h.z}), g = masking(alpha, Float{wi.z}, Float{wo.z});
            Float3 f = schlick(specular, Float{1.f}, wi_h);
            result = f * d * g * .25f / wi.z;
        };
        return result;
    }
    [[nodiscard]] Float reflection_pdf(const Float3 &wi, const Float3 &wo) const noexcept {
        Float result = 0.f;
        $if(!(min(wi.z, wo.z) < MinCos) && alpha != 0.f && has(SpecularReflection)) {
            Float3 h = normalize(wi + wo);
            result = bounded_pdf(alpha, wi, h);
        };
        return result;
    }
    [[nodiscard]] StandardSample reflection_sample(const Float3 &wi, const Float3 &u) const noexcept {
        StandardSample r{make_float3(0.f), make_float3(0.f), Float{0.f}, Float{1.f}, Uint{SpecularReflection}, Bool{false}};
        $if(!(wi.z < MinCos)) {
            $if(alpha == 0.f) {
                $if(has(DeltaReflection)) {
                    r.direction = make_float3(-wi.x, -wi.y, wi.z);
                    r.weight = schlick(specular, Float{1.f}, Float{wi.z});
                    r.lobe = DeltaReflection; r.valid = true;
                };
            } $else {
                $if(has(SpecularReflection)) {
                    Float3 h = bounded_sample(alpha, wi, Float2{u.xy()});
                    Float wi_h = dot(wi, h);
                    r.direction = 2.f * wi_h * h - wi;
                    $if(!(r.direction.z < MinCos)) {
                        r.pdf = reflection_pdf(wi, r.direction);
                        r.weight = reflection_eval(wi, r.direction) / r.pdf;
                        r.lobe = SpecularReflection; r.valid = true;
                    };
                };
            };
        };
        return r;
    }
    [[nodiscard]] Float3 transmission_eval(const Float3 &wi, const Float3 &wo) const noexcept {
        Float3 result = make_float3(0.f);
        Bool refl = wo.z > 0.f, hr = has(SpecularReflection), ht = has(SpecularTransmission);
        $if(!(min(wi.z, abs(wo.z)) < MinCos) && transmission_alpha != 0.f && ((refl && hr) || (!refl && ht))) {
            Float3 h = normalize(wo + wi * select(refl, 1.f, eta));
            h *= native_sign(Float{h.z});
            Float wi_h = dot(wi, h), wo_h = dot(wo, h);
            Float d = ndf(transmission_alpha, Float{h.z});
            Float g = masking(transmission_alpha, Float{wi.z}, abs(Float{wo.z}));
            Float f = dielectric(eta, wi_h).value;
            $if(refl) { result = make_float3(f * d * g * .25f / wi.z); }
            $else {
                Float denominator = wo_h + eta * wi_h;
                Float t = eta * eta * wi_h * wo_h / (wi.z * denominator * denominator);
                result = transmission * (1.f - f) * d * g * abs(t);
            };
        };
        return result;
    }
    [[nodiscard]] Float transmission_pdf(const Float3 &wi, const Float3 &wo) const noexcept {
        Float result = 0.f;
        Bool refl = wo.z > 0.f, hr = has(SpecularReflection), ht = has(SpecularTransmission);
        $if(!(min(wi.z, abs(wo.z)) < MinCos) && transmission_alpha != 0.f && ((refl && hr) || (!refl && ht))) {
            Float3 h = normalize(wo + wi * select(refl, 1.f, eta));
            h *= native_sign(Float{h.z});
            Float wi_h = dot(wi, h), wo_h = dot(wo, h);
            Float f = dielectric(eta, wi_h).value;
            Float pdf = bounded_pdf(transmission_alpha, wi, h);
            Bool valid = true;
            $if(refl) {
                $if(wo_h <= 0.f) { valid = false; }
                $else { pdf *= wi_h / wo_h; };
            } $else {
                $if(wo_h > 0.f) { valid = false; }
                $else {
                    pdf *= wi_h * 4.f;
                    Float denominator = wo_h + eta * wi_h;
                    Float denom = denominator * denominator;
                    pdf *= abs(wo_h) / denom;
                };
            };
            $if(hr && ht) { pdf *= select(refl, f, 1.f - f); };
            $if(valid) { result = clamp(pdf, 0.f, as<float>(Uint{0x7f7fffffu})); };
        };
        return result;
    }
    [[nodiscard]] StandardSample transmission_sample(const Float3 &wi, const Float3 &u) const noexcept {
        StandardSample r{make_float3(0.f), make_float3(0.f), Float{0.f}, Float{1.f}, Uint{SpecularReflection}, Bool{false}};
        $if(!(wi.z < MinCos)) {
            $if(transmission_alpha == 0.f) {
                Bool hr = has(DeltaReflection), ht = has(DeltaTransmission);
                $if(hr || ht) {
                    const auto f = dielectric(eta, Float{wi.z});
                    Bool refl = hr, accepted = true;
                    $if(hr && ht) {
                        refl = u.z < f.value;
                        r.lobe_probability = select(refl, f.value, 1.f - f.value);
                    } $else { $if(ht && f.value == 1.f) { accepted = false; }; };
                    $if(accepted) {
                        r.weight = select(refl, make_float3(1.f), transmission);
                        $if(!(hr && ht)) { r.weight *= make_float3(select(refl, f.value, 1.f - f.value)); };
                        r.direction = select(refl, make_float3(-wi.x, -wi.y, wi.z), make_float3(-wi.x * eta, -wi.y * eta, -f.cos_transmitted));
                        r.lobe = select(refl, DeltaReflection, DeltaTransmission);
                        r.valid = !(abs(r.direction.z) < MinCos || ((r.direction.z > 0.f) != refl));
                    };
                };
            } $else {
                Bool hr = has(SpecularReflection), ht = has(SpecularTransmission);
                $if(hr || ht) {
                    Float3 h = bounded_sample(transmission_alpha, wi, Float2{u.xy()});
                    Float wi_h = dot(wi, h);
                    const auto f = dielectric(eta, wi_h);
                    Bool refl = hr, accepted = true;
                    $if(hr && ht) { refl = u.z < f.value; }
                    $else { $if(ht && f.value == 1.f) { accepted = false; }; };
                    $if(accepted) {
                        r.direction = select(refl, 2.f * wi_h * h - wi, (eta * wi_h - f.cos_transmitted) * h - eta * wi);
                        $if(!(abs(r.direction.z) < MinCos || ((r.direction.z > 0.f) != refl))) {
                            r.lobe = select(refl, SpecularReflection, SpecularTransmission);
                            r.pdf = transmission_pdf(wi, r.direction);
                            $if(r.pdf > 0.f) { r.weight = transmission_eval(wi, r.direction) / r.pdf; };
                            r.valid = true;
                        };
                    };
                };
            };
        };
        return r;
    }
};
inline void assign_sample(StandardSample &a, const StandardSample &b) noexcept {
    a.direction = b.direction; a.weight = b.weight; a.pdf = b.pdf;
    a.lobe_probability = b.lobe_probability; a.lobe = b.lobe; a.valid = b.valid;
}
}// namespace standard_detail

// Both directions are native local coordinates, view-away and scatter-away.
// The evaluated F includes the scatter cosine.
[[nodiscard]] inline StandardEval standard_eval(const StandardMaterial &material,
    const Float3 &Nworld, const Float3 &Vworld, const Float3 &localView, const Float3 &localScatter) noexcept {
    standard_detail::Model b{material, Nworld, Vworld};
    StandardEval result{make_float3(0.f), make_float3(0.f)};
    $if(b.p_dr > 0.f) { result.diffuse += (1.f - b.spec_trans) * (1.f - b.diff_trans) * b.diffuse_eval(localView, localScatter, false); };
    $if(b.p_dt > 0.f) { result.diffuse += (1.f - b.spec_trans) * b.diff_trans * b.diffuse_eval(localView, localScatter, true); };
    $if(b.p_sr > 0.f) { result.specular += (1.f - b.spec_trans) * b.reflection_eval(localView, localScatter); };
    $if(b.p_srt > 0.f) { result.specular += b.spec_trans * b.transmission_eval(localView, localScatter); };
    return result;
}
[[nodiscard]] inline Float standard_pdf(const StandardMaterial &material,
    const Float3 &Nworld, const Float3 &Vworld, const Float3 &localView, const Float3 &localScatter) noexcept {
    standard_detail::Model b{material, Nworld, Vworld};
    Float pdf = 0.f;
    $if(b.p_dr > 0.f) { pdf += b.p_dr * b.diffuse_pdf(localView, localScatter, false); };
    $if(b.p_dt > 0.f) { pdf += b.p_dt * b.diffuse_pdf(localView, localScatter, true); };
    $if(b.p_sr > 0.f) { pdf += b.p_sr * b.reflection_pdf(localView, localScatter); };
    $if(b.p_srt > 0.f) { pdf += b.p_srt * b.transmission_pdf(localView, localScatter); };
    return pdf;
}
[[nodiscard]] inline StandardSample standard_sample(const StandardMaterial &material,
    const Float3 &Nworld, const Float3 &Vworld, const Float3 &localView, const Float3 &random) noexcept {
    using namespace standard_detail;
    Model b{material, Nworld, Vworld};
    StandardSample r{make_float3(0.f), make_float3(0.f), Float{0.f}, Float{0.f}, Uint{DiffuseReflection}, Bool{false}};
    Float3 u = random;
    Float select_lobe = random.z;
    $if(select_lobe < b.p_dr) {
        u.z = clamp(select_lobe / b.p_dr, 0.f, OneMinusEpsilon);
        assign_sample(r, b.diffuse_sample(localView, u, false));
        r.weight /= b.p_dr; r.weight *= (1.f - b.spec_trans) * (1.f - b.diff_trans);
        r.pdf *= b.p_dr; r.lobe_probability *= b.p_dr;
        $if(b.p_sr > 0.f) { r.pdf += b.p_sr * b.reflection_pdf(localView, r.direction); };
        $if(b.p_srt > 0.f) { r.pdf += b.p_srt * b.transmission_pdf(localView, r.direction); };
    } $elif(select_lobe < b.p_dr + b.p_dt) {
        assign_sample(r, b.diffuse_sample(localView, u, true));
        r.weight /= b.p_dt; r.weight *= (1.f - b.spec_trans) * b.diff_trans;
        r.pdf *= b.p_dt; r.lobe_probability *= b.p_dt;
        $if(b.p_srt > 0.f) { r.pdf += b.p_srt * b.transmission_pdf(localView, r.direction); };
    } $elif(select_lobe < b.p_dr + b.p_dt + b.p_sr) {
        u.z = clamp((select_lobe - (b.p_dr + b.p_dt)) / b.p_sr, 0.f, OneMinusEpsilon);
        assign_sample(r, b.reflection_sample(localView, u));
        r.weight /= b.p_sr; r.weight *= (1.f - b.spec_trans);
        r.pdf *= b.p_sr; r.lobe_probability *= b.p_sr;
        $if(b.p_dr > 0.f) { r.pdf += b.p_dr * b.diffuse_pdf(localView, r.direction, false); };
        $if(b.p_srt > 0.f) { r.pdf += b.p_srt * b.transmission_pdf(localView, r.direction); };
    } $elif(b.p_srt > 0.f) {
        u.z = clamp((select_lobe - (b.p_dr + b.p_dt + b.p_sr)) / b.p_srt, 0.f, OneMinusEpsilon);
        assign_sample(r, b.transmission_sample(localView, u));
        r.weight /= b.p_srt; r.weight *= b.spec_trans;
        r.pdf *= b.p_srt; r.lobe_probability *= b.p_srt;
        $if(b.p_dr > 0.f) { r.pdf += b.p_dr * b.diffuse_pdf(localView, r.direction, false); };
        $if(b.p_dt > 0.f) { r.pdf += b.p_dt * b.diffuse_pdf(localView, r.direction, true); };
        $if(b.p_sr > 0.f) { r.pdf += b.p_sr * b.reflection_pdf(localView, r.direction); };
    };
    $if(!r.valid || (r.lobe & 0x44u) != 0u) { r.pdf = 0.f; };
    return r;
}
[[nodiscard]] inline Uint standard_lobes(const StandardMaterial &material) noexcept {
    using namespace standard_detail;
    Bool delta = material.p0.w * material.p0.w < MinAlpha;
    Uint result = select(delta, DeltaReflection, SpecularReflection);
    $if(any(material.p0.xyz() > 0.f) && material.p3.x < 1.f) {
        $if(material.p2.w < 1.f) { result |= DiffuseReflection; };
        $if(material.p2.w > 0.f) { result |= DiffuseTransmission; };
    };
    $if(material.p3.x > 0.f) { result |= select(delta, DeltaTransmission, SpecularTransmission); };
    return result;
}
[[nodiscard]] inline StandardDeltaLobes standard_delta_lobes(const StandardMaterial &material,
    const Float3 &Nworld, const Float3 &Vworld, const Float3 &localView) noexcept {
    using namespace standard_detail;
    Model b{material, Nworld, Vworld};
    StandardDeltaLobes r{std::array<StandardDeltaLobe, 3>{
        StandardDeltaLobe{make_float3(0.f), make_float3(0.f), Float{0.f}, Bool{false}},
        StandardDeltaLobe{make_float3(0.f), make_float3(0.f), Float{0.f}, Bool{false}},
        StandardDeltaLobe{make_float3(0.f), make_float3(0.f), Float{0.f}, Bool{false}}}, Uint{2u}, Float{b.p_dr + b.p_dt}};
    $if(b.alpha > 0.f) { r.non_delta_part += b.p_sr; };
    $if(b.transmission_alpha > 0.f) { r.non_delta_part += b.p_srt; };
    $if((b.p_sr + b.p_srt) != 0.f && !b.psd_exclude) {
        r.lobes[0].transmission = true;
        r.lobes[1].direction = make_float3(-localView.x, -localView.y, localView.z);
        $if(b.alpha == 0.f && b.has(DeltaReflection)) {
            r.lobes[1].probability = b.p_sr;
            r.lobes[1].throughput = (1.f - b.p_srt) * schlick(b.specular, Float{1.f}, Float{localView.z});
        };
        $if(b.transmission_alpha == 0.f) {
            Bool hr = b.has(DeltaReflection), ht = b.has(DeltaTransmission);
            $if(hr || ht) {
                const auto f = dielectric(b.eta, Float{localView.z});
                $if(hr) {
                    Float p = b.p_srt * f.value;
                    Float3 w = make_float3(1.f) * p;
                    r.lobes[1].throughput += w; r.lobes[1].probability += p;
                };
                $if(ht) {
                    Float p = b.p_srt * (1.f - f.value);
                    Float3 w = b.transmission * p;
                    r.lobes[0].direction = make_float3(-localView.x * b.eta, -localView.y * b.eta, -f.cos_transmitted);
                    r.lobes[0].throughput = w; r.lobes[0].probability = p;
                };
            };
        };
    };
    return r;
}
}// namespace vision::rtxpt_gi
