//
// Created by ling.zhu on 2025/2/22.
//

#include "base/import/param_schema.h"
#include "base/scattering/material.h"
#include "base/shader_graph/shader_node.h"
#include "base/mgr/scene.h"
#include "math/warp.h"

namespace vision {

[[nodiscard]] inline Float plastic_diffuse_fresnel(const Float &ior) noexcept {
    Float eta = max(ior, 1.003f);
    Float eta2 = sqr(eta);
    return saturate(-1.4399f / eta2 + 0.7099f / eta + 0.6681f + 0.0636f * eta);
}

class PlasticLobe final : public Lobe {
private:
    DCSP<Fresnel> fresnel_;
    DCUP<MicrofacetReflection> specular_;
    DCUP<LambertReflection> diffuse_;
    SampledSpectrum diffuse_albedo_;
    SampledSpectrum scaled_sigma_a_;
    Float diffuse_fresnel_{};
    Float avg_transmittance_{};
    Float eta_inv_sq_{};

protected:
    [[nodiscard]] uint64_t compute_topology_hash() const noexcept override {
        return hash64(fresnel_->topology_hash(),
                      specular_->topology_hash(),
                      diffuse_->topology_hash());
    }

    [[nodiscard]] Float specular_probability(const Float3 &wo) const noexcept {
        Float Fi = fresnel_->evaluate(abs_cos_theta(wo)).average();
        Float substrate_weight = diffuse_albedo_.average() * avg_transmittance_ * (1.f - Fi);
        Float specular_weight = Fi;
        Float weight_sum = substrate_weight + specular_weight;
        return select(weight_sum > 0.f, specular_weight / weight_sum, 1.f);
    }

    [[nodiscard]] SampledSpectrum substrate_absorption(const Float3 &wo, const Float3 &wi) const noexcept {
        Float optical_depth = rcp(abs_cos_theta(wo)) + rcp(abs_cos_theta(wi));
        return scaled_sigma_a_.map([&](const Float &sigma_a) noexcept {
            return ocarina::exp(-sigma_a * optical_depth);
        });
    }

public:
    PlasticLobe(const SP<Fresnel> &fresnel, UP<MicrofacetReflection> specular, UP<LambertReflection> diffuse,
                SampledSpectrum diffuse_albedo, SampledSpectrum scaled_sigma_a, Float diffuse_fresnel,
                Float avg_transmittance, Float eta_inv_sq, ShadingFrame shading_frame = {})
        : Lobe(std::move(shading_frame)),
          fresnel_(fresnel),
          specular_(std::move(specular)),
          diffuse_(std::move(diffuse)),
          diffuse_albedo_(std::move(diffuse_albedo)),
          scaled_sigma_a_(std::move(scaled_sigma_a)),
          diffuse_fresnel_(diffuse_fresnel),
          avg_transmittance_(avg_transmittance),
          eta_inv_sq_(eta_inv_sq) {}

    [[nodiscard]] Uint flag() const noexcept override { return SurfaceData::Glossy; }

    [[nodiscard]] SampledSpectrum albedo(const Float &cos_theta) const noexcept override {
        return diffuse_albedo_;
    }

    [[nodiscard]] const SampledWavelengths *swl() const override {
        return &specular_->swl();
    }

    void reuse_material(SampledSpectrum &diffuse, SampledSpectrum &specular,
                        Float2 &roughness) const noexcept override {
        diffuse = diffuse_albedo_ * avg_transmittance_;
        specular = fresnel_->evaluate(1.f);
        roughness = sqrt(make_float2(specular_->alpha_x(), specular_->alpha_y()));
    }

    [[nodiscard]] Float diffuse_factor() const noexcept override {
        return 1.f - fresnel_->evaluate(1.f).average();
    }

    [[nodiscard]] ScatterEval evaluate_local_impl(const Float3 &wo, const Float3 &wi, MaterialEvalMode mode,
                                                  const Uint &flag, TransportMode tm) const noexcept override {
        ScatterEval ret{*swl()};
        Bool valid = same_hemisphere(wo, wi) && wo.z > 0.f && wi.z > 0.f;
        Float specular_prob = specular_probability(wo);
        if (BxDF::match_F(mode)) {
            SampledSpectrum Fi = fresnel_->evaluate(abs_cos_theta(wo));
            SampledSpectrum Fo = fresnel_->evaluate(abs_cos_theta(wi));
            SampledSpectrum substrate_albedo = safe_div(diffuse_albedo_, 1.f - diffuse_albedo_ * diffuse_fresnel_);
            SampledSpectrum diffuse = diffuse_->f(wo, wi, nullptr, tm);
            diffuse *= (1.f - Fi) * (1.f - Fo) * eta_inv_sq_;
            diffuse *= substrate_albedo;
            diffuse *= substrate_absorption(wo, wi);

            SampledSpectrum specular = specular_->f(wo, wi, fresnel_.ptr(), tm);
            ret.f = select(valid, specular + diffuse, 0.f);
        }
        if (BxDF::match_PDF(mode)) {
            Float diffuse_pdf = diffuse_->PDF(wo, wi, nullptr);
            Float specular_pdf = specular_->PDF(wo, wi, fresnel_.ptr());
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
                sd = diffuse_->sample_wi(wo, sampler->next_2d(), nullptr);
            };
        };
        return sd;
    }
};

/**
 Example material JSON:
 {
     "type" : "plastic",
     "name" : "MatPlastic",
     "param" : {
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
             "channels" : "x",
             "node" : {
                 "type" : "number",
                 "param" : { "value" : 0.02 }
             }
         },
         "anisotropic" : {
             "channels" : "x",
             "node" : {
                 "type" : "number",
                 "param" : { "value" : 0.0 }
             }
         },
         "sigma_a" : {
             "channels" : "xyz",
             "node" : {
                 "type" : "number",
                 "param" : { "value" : [0.0, 0.0, 0.0] }
             }
         },
         "thickness" : {
             "channels" : "x",
             "node" : {
                 "type" : "number",
                 "param" : { "value" : 1.0 }
             }
         },
         "remapping_roughness" : true
     },
     "node_tab" : {}
 }
 */
class PlasticMaterial final : public Material {
private:
#define PLASTIC_SLOTS(X)                                             \
    X(color, make_float3(1.f), Albedo, , 3u, false)                  \
    X(ior, 1.5f, Number, .set_range(1.003, 5), 1u, false)            \
    X(roughness, 0.02f, Number, .set_range(0.0001f, 1.f), 1u, false) \
    X(anisotropic, 0.f, Number, .set_range(-1, 1), 1u, false)        \
    X(sigma_a, make_float3(0.f), Unbound, , 3u, false)               \
    X(thickness, 1.f, Number, .set_range(0.f, 1000.f), 1u, false)

#define PLASTIC_DECLARE_SLOT_(name, val, tag, extra, dim, required) VS_MAKE_SLOT(name)
    PLASTIC_SLOTS(PLASTIC_DECLARE_SLOT_)
#undef PLASTIC_DECLARE_SLOT_

    [[nodiscard]] static const ParamSchema &param_schema() noexcept {
        static const ParamSchema schema = [] {
            ParamSchema ret;
#define PLASTIC_REGISTER_PARAM_(name, val, tag, extra, dim, required) ret.add_slot(#name, tag, dim, required);
            PLASTIC_SLOTS(PLASTIC_REGISTER_PARAM_)
#undef PLASTIC_REGISTER_PARAM_
            ret.add_plain("remapping_roughness", ParamType::Bool);
            return ret;
        }();
        return schema;
    }
    bool remapping_roughness_{true};

protected:
    VS_MAKE_MATERIAL_EVALUATOR(PlasticLobe)
public:
    PlasticMaterial() = default;
    explicit PlasticMaterial(const MaterialDesc &desc)
        : Material(desc),
          remapping_roughness_(desc["remapping_roughness"].as_bool(true)) {}
    [[nodiscard]] bool enable_delta() const noexcept override { return false; }
    VS_HOTFIX_MAKE_RESTORE(Material, remapping_roughness_)

    void initialize_slots(const vision::Material::Desc &desc) noexcept override {
        Material::initialize_slots(desc);
        const ParamSchema &schema = param_schema();
        validate_params(desc, schema);
#define PLASTIC_INIT_SLOT_(name, val, tag, extra, dim, required) VS_INIT_SLOT(name, val, tag) extra;
        PLASTIC_SLOTS(PLASTIC_INIT_SLOT_)
#undef PLASTIC_INIT_SLOT_
    }

    void prepare() noexcept override {
        Material::prepare();
    }

    VS_MAKE_PLUGIN_NAME_FUNC
    [[nodiscard]] UP<Lobe> create_lobe_set(const Interaction &it, const SampledWavelengths &swl) const noexcept override {
        auto shading_frame = compute_shading_frame(it, swl);
        SampledSpectrum Rd = color_.eval_albedo_spectrum(it, swl).sample;
        SampledSpectrum sigma_a = SampledSpectrum{sigma_a_.evaluate(it, swl).array};

        Float ior = ior_.evaluate(it, swl)->as_scalar();
        Float roughness = ocarina::clamp(roughness_.evaluate(it, swl)->as_scalar(), 0.0001f, 1.f);
        Float anisotropic = ocarina::clamp(anisotropic_.evaluate(it, swl)->as_scalar(), -0.9f, 0.9f);
        Float thickness = max(thickness_.evaluate(it, swl)->as_scalar(), 0.f);

        Float alpha_value = remapping_roughness_ ? roughness_to_alpha(roughness) : roughness;
        Float2 alpha = calculate_alpha<D>(alpha_value, anisotropic);
        alpha = clamp(alpha, make_float2(0.0001f), make_float2(1.f));

        SampledSpectrum scaled_sigma_a = sigma_a * thickness;
        Float avg_transmittance = ocarina::exp(-2.f * scaled_sigma_a.average());
        Float diffuse_fresnel = plastic_diffuse_fresnel(ior);

        SP<Fresnel> fresnel = make_shared<FresnelDielectric>(SampledSpectrum{swl.dimension(), ior}, swl);
        auto microfacet = make_shared<GGXMicrofacet>(alpha.x, alpha.y);
        UP<MicrofacetReflection> refl = make_unique<MicrofacetReflection>(spectrum()->one(), swl, microfacet);
        UP<LambertReflection> diffuse = make_unique<LambertReflection>(Rd, swl);

        return make_unique<PlasticLobe>(fresnel, std::move(refl), std::move(diffuse), Rd, scaled_sigma_a,
                                        diffuse_fresnel, avg_transmittance, 1.f / sqr(ior), shading_frame);
    }
};

}// namespace vision

VS_MAKE_CLASS_CREATOR_HOTFIX(vision, PlasticMaterial)