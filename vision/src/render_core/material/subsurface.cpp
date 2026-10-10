//
// Created by Zero on 09/12/2022.
//

#include "base/import/param_schema.h"
#include "base/import/medium_scatter_data.h"
#include "base/scattering/material.h"
#include "base/shader_graph/shader_node.h"
#include "base/mgr/scene.h"
#include "math/warp.h"

namespace vision {

struct DielectricBoundaryEval {
    Float F{};
    Float cos_theta_t{};
    Bool tir{false};
};

[[nodiscard]] inline DielectricBoundaryEval evaluate_dielectric_boundary(const Float &abs_cos_theta_i,
                                                                         const Float &eta) noexcept {
    DielectricBoundaryEval ret;
    Float sin_theta_i_2 = max(0.f, 1.f - sqr(abs_cos_theta_i));
    Float sin_theta_t_2 = sin_theta_i_2 / sqr(eta);
    ret.tir = sin_theta_t_2 >= 1.f;
    ret.cos_theta_t = safe_sqrt(1.f - sin_theta_t_2);
    Float r_parl = (eta * abs_cos_theta_i - ret.cos_theta_t) / (eta * abs_cos_theta_i + ret.cos_theta_t);
    Float r_perp = (abs_cos_theta_i - eta * ret.cos_theta_t) / (abs_cos_theta_i + eta * ret.cos_theta_t);
    ret.F = select(ret.tir, 1.f, 0.5f * (sqr(r_parl) + sqr(r_perp)));
    return ret;
}

enum class SubsurfaceMethod : uint {
    Burley,
    RandomWalk,
    RandomWalkSkin,
};

[[nodiscard]] inline pair<float3, float3> lookup_subsurface_sigma(string_view name) noexcept {
    for (auto elm : SubsurfaceParameterTable) {
        if (elm.name == name) {
            return {elm.sigma_s, elm.sigma_a};
        }
    }
    auto elm = SubsurfaceParameterTable[0];
    return {elm.sigma_s, elm.sigma_a};
}

[[nodiscard]] inline Float bssrdf_dipole_compute_Rd(const Float &alpha_prime,
                                                    const Float &fourthirdA) noexcept {
    Float s = safe_sqrt(3.f * (1.f - alpha_prime));
    return 0.5f * alpha_prime * (1.f + ocarina::exp(-fourthirdA * s)) * ocarina::exp(-s);
}

[[nodiscard]] inline Float bssrdf_dipole_compute_alpha_prime(const Float &rd,
                                                             const Float &fourthirdA) noexcept {
    $if(rd < 1e-4f) {
        return 0.f;
    };
    $if(rd >= 0.995f) {
        return 0.999999f;
    };
    Float x0 = 0.f;
    Float x1 = 1.f;
    Float xmid = 0.5f;
    constexpr uint max_iteration_count = 12u;
    for (uint i = 0; i < max_iteration_count; ++i) {
        xmid = 0.5f * (x0 + x1);
        Float fmid = bssrdf_dipole_compute_Rd(xmid, fourthirdA);
        $if(fmid < rd) {
            x0 = xmid;
        }
        $else {
            x1 = xmid;
        };
    }
    return xmid;
}

[[nodiscard]] inline Float bssrdf_burley_fitting(const Float &albedo) noexcept {
    return 1.9f - albedo + 3.5f * sqr(albedo - 0.8f);
}

[[nodiscard]] inline SampledSpectrum burley_compatible_mfp(SampledSpectrum radius) noexcept {
    return radius * (0.25f * InvPi);
}

[[nodiscard]] inline SampledSpectrum blender_effective_radius(SampledSpectrum radius,
                                                              SampledSpectrum albedo,
                                                              const Float &ior,
                                                              SubsurfaceMethod method) noexcept {
    switch (method) {
        case SubsurfaceMethod::Burley: {
            SampledSpectrum s = albedo.map([](const Float &value) noexcept {
                return bssrdf_burley_fitting(value);
            });
            return safe_div(burley_compatible_mfp(radius), s);
        }
        case SubsurfaceMethod::RandomWalkSkin: {
            Float inv_eta = rcp(ior);
            Float F_dr = inv_eta * (-1.440f * inv_eta + 0.710f) + 0.668f + 0.0636f * ior;
            Float fourthirdA = (4.f / 3.f) * (1.f + F_dr) / (1.f - F_dr);
            SampledSpectrum alpha_prime = albedo.map([&](const Float &value) noexcept {
                return bssrdf_dipole_compute_alpha_prime(value, fourthirdA);
            });
            SampledSpectrum radius_scale = alpha_prime.map([](const Float &value) noexcept {
                return safe_sqrt(3.f * max(1.f - value, 1e-4f));
            });
            return radius * radius_scale;
        }
        case SubsurfaceMethod::RandomWalk:
        default:
            return burley_compatible_mfp(radius);
    }
}

class SubsurfaceLobe final : public Lobe {
private:
    DCSP<Fresnel> fresnel_;
    DCUP<MicrofacetReflection> specular_;
    SampledSpectrum substrate_albedo_;
    SampledSpectrum scaled_sigma_a_;
    Float avg_transmittance_{};
    Float ior_{};
    Float inv_ior_{};
    Float inv_ior_sq_{};

private:
    [[nodiscard]] uint64_t compute_topology_hash() const noexcept override {
        return hash64(fresnel_->topology_hash(), specular_->topology_hash());
    }

    [[nodiscard]] Float specular_probability(const Float3 &wo) const noexcept {
        Float Fi = fresnel_->evaluate(abs_cos_theta(wo)).average();
        Float substrate_weight = substrate_albedo_.average() * avg_transmittance_ * (1.f - Fi);
        Float specular_weight = Fi;
        Float weight_sum = substrate_weight + specular_weight;
        return select(weight_sum > 0.f, specular_weight / weight_sum, 1.f);
    }

    [[nodiscard]] SampledSpectrum substrate_absorption(const Float &cos_theta_i,
                                                       const Float &cos_theta_o) const noexcept {
        Float optical_depth = rcp(cos_theta_i) + rcp(cos_theta_o);
        return scaled_sigma_a_.map([&](const Float &sigma_a) noexcept {
            return ocarina::exp(-sigma_a * optical_depth);
        });
    }

    [[nodiscard]] Float substrate_pdf(const Float3 &wi) const noexcept {
        return inv_ior_sq_ * cosine_hemisphere_PDF(abs_cos_theta(wi));
    }

public:
    SubsurfaceLobe(const SP<Fresnel> &fresnel, UP<MicrofacetReflection> specular,
                   SampledSpectrum substrate_albedo, SampledSpectrum scaled_sigma_a,
                   Float avg_transmittance, Float ior, ShadingFrame shading_frame = {})
        : Lobe(std::move(shading_frame)),
          fresnel_(fresnel),
          specular_(std::move(specular)),
          substrate_albedo_(std::move(substrate_albedo)),
          scaled_sigma_a_(std::move(scaled_sigma_a)),
          avg_transmittance_(avg_transmittance),
          ior_(ior),
          inv_ior_(rcp(ior)),
          inv_ior_sq_(rcp(sqr(ior))) {}

    [[nodiscard]] Uint flag() const noexcept override { return SurfaceData::Glossy; }

    [[nodiscard]] SampledSpectrum albedo(const Float &cos_theta) const noexcept override {
        return substrate_albedo_;
    }

    [[nodiscard]] const SampledWavelengths *swl() const override {
        return &specular_->swl();
    }

    void reuse_material(SampledSpectrum &diffuse, SampledSpectrum &specular,
                        Float2 &roughness) const noexcept override {
        diffuse = substrate_albedo_ * avg_transmittance_;
        specular = fresnel_->evaluate(1.f);
        roughness = sqrt(make_float2(specular_->alpha_x(), specular_->alpha_y()));
    }

    [[nodiscard]] Float diffuse_factor() const noexcept override {
        return substrate_albedo_.average();
    }

    [[nodiscard]] ScatterEval evaluate_local_impl(const Float3 &wo, const Float3 &wi, MaterialEvalMode mode,
                                                  const Uint &flag, TransportMode tm) const noexcept override {
        ScatterEval ret{*swl()};
        Bool valid = same_hemisphere(wo, wi) && wo.z > 0.f && wi.z > 0.f;
        Float specular_prob = specular_probability(wo);

        if (BxDF::match_F(mode)) {
            SampledSpectrum specular = specular_->f(wo, wi, fresnel_.ptr(), tm);
            DielectricBoundaryEval entry = evaluate_dielectric_boundary(abs_cos_theta(wo), ior_);
            DielectricBoundaryEval exit = evaluate_dielectric_boundary(abs_cos_theta(wi), ior_);

            SampledSpectrum substrate = SampledSpectrum::zero(*swl());
            $if(!entry.tir && !exit.tir) {
                substrate = substrate_albedo_ * InvPi;
                substrate *= (1.f - entry.F) * (1.f - exit.F) * inv_ior_sq_;
                substrate *= substrate_absorption(entry.cos_theta_t, exit.cos_theta_t);
            };
            ret.f = select(valid, specular + substrate, 0.f);
        }

        if (BxDF::match_PDF(mode)) {
            Float specular_pdf = specular_->PDF(wo, wi, fresnel_.ptr());
            Float diffuse_pdf = substrate_pdf(wi);
            ret.pdfs = select(valid, specular_prob * specular_pdf + (1.f - specular_prob) * diffuse_pdf, 0.f);
        }

        ret.flags = this->flag();
        return ret;
    }

    [[nodiscard]] SampledDirection sample_wi_local_impl(const Float3 &wo, const Uint &flag,
                                                        TSampler &sampler) const noexcept override {
        SampledDirection sd;
        Float specular_prob = specular_probability(wo);
        Float uc = sampler->next_1d();

        $if(wo.z <= 0.f) {
            sd.valid = false;
        }
        $else {
            $if(uc < specular_prob) {
                sd = specular_->sample_wi(wo, sampler->next_2d(), fresnel_.ptr());
            }
            $else {
                Float3 wi_sub = square_to_cosine_hemisphere(sampler->next_2d());
                DielectricBoundaryEval exit = evaluate_dielectric_boundary(abs_cos_theta(wi_sub), inv_ior_);
                $if(exit.tir) {
                    sd.valid = false;
                }
                $else {
                    sd.wi = make_float3(wi_sub.x * ior_, wi_sub.y * ior_, exit.cos_theta_t);
                    sd.valid = true;
                };
            };
        };
        return sd;
    }
};

/**
 Example material JSON:
 {
     "type" : "subsurface",
     "name" : "MatSubsurface",
     "param" : {
         "sigma_a" : {
             "channels" : "xyz",
             "node" : {
                 "type" : "number",
                 "param" : { "value" : [0.0011, 0.0024, 0.014] }
             }
         },
         "sigma_s" : {
             "channels" : "xyz",
             "node" : {
                 "type" : "number",
                 "param" : { "value" : [2.55, 3.21, 3.77] }
             }
         },
         "color" : {
             "channels" : "xyz",
             "node" : {
                 "type" : "number",
                 "param" : { "value" : [1.0, 1.0, 1.0] }
             }
         },
         "ior" : {
             "channels" : "x",
             "node" : {
                 "type" : "number",
                 "param" : { "value" : 1.5 }
             }
         },
         "roughness" : {
             "channels" : "xy",
             "node" : {
                 "type" : "number",
                 "param" : { "value" : [0.1, 0.1] }
             }
         },
         "sigma_scale" : 1.0,
         "remapping_roughness" : false
     },
     "node_tab" : {}
 }
 */
class SubsurfaceMaterial final : public Material {
private:
#define SUBSURFACE_SLOTS(X)                                                  \
    X(sigma_a, make_float3(.0011f, .0024f, .014f), Unbound, , 3u, false)    \
    X(sigma_s, make_float3(2.55f, 3.21f, 3.77f), Unbound, , 3u, false)      \
    X(color, make_float3(1.f), Albedo, , 3u, false)                         \
    X(radius, make_float3(1.f, 0.2f, 0.1f), Number, , 3u, false)            \
    X(scale, 0.05f, Number, .set_range(0.f, 1000.f), 1u, false)             \
    X(ior, 1.4f, Number, .set_range(1.003f, 5.f), 1u, false)                \
    X(roughness, make_float2(0.1f), Number, , 2u, false)                    \
    X(anisotropy, 0.f, Number, .set_range(0.f, 0.9f), 1u, false)

    float sigma_scale_{1.f};
    float3 preset_sigma_a_{.0011f, .0024f, .014f};
    float3 preset_sigma_s_{2.55f, 3.21f, 3.77f};
    SubsurfaceMethod method_{SubsurfaceMethod::RandomWalk};
    bool use_blender_inputs_{true};
    bool use_medium_preset_{false};
    bool remapping_roughness_{false};

#define SUBSURFACE_DECLARE_SLOT_(name, val, tag, extra, dim, required) VS_MAKE_SLOT(name)
    SUBSURFACE_SLOTS(SUBSURFACE_DECLARE_SLOT_)
#undef SUBSURFACE_DECLARE_SLOT_

    [[nodiscard]] static const ParamSchema &param_schema() noexcept {
        static const ParamSchema schema = [] {
            ParamSchema ret;
#define SUBSURFACE_REGISTER_PARAM_(name, val, tag, extra, dim, required) ret.add_slot(#name, tag, dim, required);
            SUBSURFACE_SLOTS(SUBSURFACE_REGISTER_PARAM_)
#undef SUBSURFACE_REGISTER_PARAM_
            ret.add_plain("method", ParamType::String);
            ret.add_plain("medium_name", ParamType::String);
            ret.add_plain("sigma_scale", ParamType::Float);
            ret.add_plain("remapping_roughness", ParamType::Bool);
            return ret;
        }();
        return schema;
    }

protected:
    VS_MAKE_MATERIAL_EVALUATOR(SubsurfaceLobe)

public:
    SubsurfaceMaterial() = default;
    explicit SubsurfaceMaterial(const MaterialDesc &desc)
        : Material(desc),
          sigma_scale_{desc["sigma_scale"].as_float(1.f)},
          remapping_roughness_(desc["remapping_roughness"].as_bool(false)) {}

    [[nodiscard]] bool enable_delta() const noexcept override { return false; }
    VS_HOTFIX_MAKE_RESTORE(Material, sigma_scale_, preset_sigma_a_, preset_sigma_s_, method_,
                           use_blender_inputs_, use_medium_preset_, remapping_roughness_)
    VS_MAKE_PLUGIN_NAME_FUNC

    SubsurfaceMaterial(const SubsurfaceMaterial &) = default;

    void initialize_slots(const vision::Material::Desc &desc) noexcept override {
        Material::initialize_slots(desc);
        const ParamSchema &schema = param_schema();
        validate_params(desc, schema);
#define SUBSURFACE_INIT_SLOT_(name, val, tag, extra, dim, required) VS_INIT_SLOT(name, val, tag) extra;
        SUBSURFACE_SLOTS(SUBSURFACE_INIT_SLOT_)
#undef SUBSURFACE_INIT_SLOT_
        roughness_->set_range(0.0001f, 1.f);
        string method_name = desc["method"].as_string("random_walk");
        if (method_name == "burley") {
            method_ = SubsurfaceMethod::Burley;
        } else if (method_name == "random_walk_skin") {
            method_ = SubsurfaceMethod::RandomWalkSkin;
        } else {
            method_ = SubsurfaceMethod::RandomWalk;
        }
        if (desc.contains("medium_name")) {
            auto [sigma_s, sigma_a] = lookup_subsurface_sigma(desc["medium_name"].as_string());
            preset_sigma_s_ = sigma_s;
            preset_sigma_a_ = sigma_a;
            use_medium_preset_ = true;
        }
        bool has_explicit_blender_inputs = desc.contains("radius") || desc.contains("scale") ||
                                           desc.contains("method") || desc.contains("anisotropy");
        use_blender_inputs_ = has_explicit_blender_inputs &&
                              !desc.contains("sigma_a") && !desc.contains("sigma_s") && !use_medium_preset_;
    }

    [[nodiscard]] UP<Lobe> create_lobe_set(const Interaction &it,
                                           const SampledWavelengths &swl) const noexcept override {
        auto shading_frame = compute_shading_frame(it, swl);
        SampledSpectrum color = color_.eval_albedo_spectrum(it, swl).sample;
        SampledSpectrum sigma_a = use_medium_preset_ ? SampledSpectrum{preset_sigma_a_}
                                                     : SampledSpectrum{sigma_a_.evaluate(it, swl).array};
        SampledSpectrum sigma_s = use_medium_preset_ ? SampledSpectrum{preset_sigma_s_}
                                                     : SampledSpectrum{sigma_s_.evaluate(it, swl).array};

        Float ior = max(ior_.evaluate(it, swl)->as_scalar(), 1.003f);
        DynamicArray<float> roughness_values = roughness_.evaluate(it, swl).array;
        Float2 roughness = clamp(make_float2(roughness_values[0], roughness_values[1]),
                                 make_float2(0.0001f), make_float2(1.f));
        Float anisotropy = clamp(anisotropy_.evaluate(it, swl)->as_scalar(), 0.f, 0.9f);

        Float2 alpha = remapping_roughness_ ? roughness_to_alpha(roughness) : roughness;
        alpha = clamp(alpha, make_float2(0.0001f), make_float2(1.f));

        auto microfacet = make_shared<GGXMicrofacet>(alpha.x, alpha.y);
        auto fresnel = make_shared<FresnelDielectric>(SampledSpectrum{swl.dimension(), ior}, swl);

        SampledSpectrum substrate_albedo = color;
        SampledSpectrum scaled_sigma_a = sigma_a * sigma_scale_;
        if (use_blender_inputs_) {
            Float scale = max(scale_.evaluate(it, swl)->as_scalar(), 1e-4f);
            SampledSpectrum radius = SampledSpectrum{radius_.evaluate(it, swl).array}.map([&](const Float &value) noexcept {
                return max(value * scale, 1e-4f);
            });
            SampledSpectrum albedo = color.map([](const Float &value) noexcept {
                return clamp(value, 0.0001f, 0.9999f);
            });
            SampledSpectrum effective_radius = blender_effective_radius(radius, albedo, ior, method_);
            SampledSpectrum transport_sigma_t = effective_radius.map([](const Float &value) noexcept {
                return rcp(max(value, 1e-4f));
            });
            SampledSpectrum reduced_sigma_s = albedo * transport_sigma_t;
            sigma_a = transport_sigma_t * (1.f - albedo);
            sigma_s = reduced_sigma_s * rcp(max(1.f - anisotropy, 0.1f));
            substrate_albedo = color;
            scaled_sigma_a = sigma_a;
        } else {
            SampledSpectrum sigma_t = sigma_a + sigma_s;
            substrate_albedo = color * safe_div(sigma_s, sigma_t);
            scaled_sigma_a = sigma_a * sigma_scale_;
        }
        Float avg_transmittance = ocarina::exp(-2.f * scaled_sigma_a.average());

        UP<MicrofacetReflection> specular = make_unique<MicrofacetReflection>(spectrum()->one(), swl, microfacet);
        return make_unique<SubsurfaceLobe>(fresnel, std::move(specular), substrate_albedo,
                                           scaled_sigma_a, avg_transmittance, ior, shading_frame);
    }
};

}// namespace vision

VS_MAKE_CLASS_CREATOR_HOTFIX(vision, SubsurfaceMaterial)