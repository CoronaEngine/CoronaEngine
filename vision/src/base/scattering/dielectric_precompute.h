#pragma once

#include "base/scattering/lobe.h"
#include "base/sampler.h"

namespace vision {

class DielectricPrecompute : public DielectricLobe {
public:
    using DielectricLobe::DielectricLobe;
    [[nodiscard]] bool compensate() const noexcept override { return false; }
    [[nodiscard]] SampledSpectrum integral_albedo(const Float3 &wo, TSampler &sampler,
                                                  const Uint &sample_num) const noexcept override {
        SampledSpectrum ret = SampledSpectrum::zero(*swl());

        $for(i, sample_num) {
            BSDFSample bs = sample_local(wo, BxDFFlag::All, sampler, Importance);
            ScatterEval se = bs.eval;
            $if(se.pdf() > 0) {
                SampledSpectrum r = se.throughput() * abs_cos_theta(bs.wi);
                ret[0] += r[0];
                $if(same_hemisphere(bs.wi, wo)) {
                    ret[1] += r[0];
                };
            };
        };
        return ret / sample_num;
    }

    void from_ratio_x(const ocarina::Float &x) noexcept override {
        Float a = ocarina::clamp(sqr(x), alpha_lower, alpha_upper);
        microfacet_->set_alpha_x(a);
        microfacet_->set_alpha_y(a);
    }
};

class DielectricLobePrecompute : public DielectricPrecompute {
public:
    using DielectricPrecompute::DielectricPrecompute;

    /// for precompute begin
    static UP<DielectricLobePrecompute> create_for_precompute(const SampledWavelengths &swl) noexcept {
        SP<Fresnel> fresnel = make_shared<FresnelDielectric>(SampledSpectrum(swl, 1.5f), swl);
        SP<GGXMicrofacet> microfacet = make_shared<GGXMicrofacet>(make_float2(0.001f), true);
        return make_unique<DielectricLobePrecompute>(fresnel, microfacet, SampledSpectrum::one(3), false, BxDFFlag::Glossy);
    }
    static constexpr const char *name = "DielectricLobe";
    void from_ratio_z(ocarina::Float z) noexcept override {
        Float ior = lerp(z, ior_lower, ior_upper);
        fresnel_->set_eta(SampledSpectrum(*swl(), ior));
    }
    /// for precompute end
};

class DielectricLobeInvPrecompute : public DielectricPrecompute {
public:
    using DielectricPrecompute::DielectricPrecompute;
    /// for precompute begin
    static UP<DielectricLobeInvPrecompute> create_for_precompute(const SampledWavelengths &swl) noexcept {
        SP<Fresnel> fresnel = make_shared<FresnelDielectric>(SampledSpectrum(swl, 1.5f), swl);
        SP<GGXMicrofacet> microfacet = make_shared<GGXMicrofacet>(make_float2(0.001f), true);
        return make_unique<DielectricLobeInvPrecompute>(fresnel, microfacet, SampledSpectrum::one(3), false, BxDFFlag::Glossy);
    }
    static constexpr const char *name = "DielectricInvLobe";
    void from_ratio_z(ocarina::Float z) noexcept override {
        Float ior = lerp(z, ior_lower, ior_upper);
        fresnel_->set_eta(SampledSpectrum(*swl(), rcp(ior)));
    }
    /// for precompute end
};

}// namespace vision
