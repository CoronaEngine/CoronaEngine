//
// Created by Zero on 29/11/2022.
//

#include "base/scattering/medium.h"
#include "base/sampler.h"
#include "base/mgr/pipeline.h"

namespace vision {
using namespace ocarina;

class HomogeneousMedium : public Medium {
private:
    EncodedData<float3> sigma_a_;
    EncodedData<float3> sigma_s_;
    EncodedData<float> g_;
    static constexpr float g_clamp = 0.99f;

public:
    HomogeneousMedium() = default;
    explicit HomogeneousMedium(const MediumDesc &desc)
        : Medium(desc),
          sigma_a_(desc.sigma_a["value"].as_float3()),
          sigma_s_(desc.sigma_s["value"].as_float3()),
          g_(desc.g["value"].as_float()) {
        validate_homogeneous_parameters(sigma_a_.hv(), sigma_s_.hv(), scale_.hv(), g_.hv());
    }

    VS_MAKE_PLUGIN_NAME_FUNC
    OC_ENCODABLE_FUNC(Medium, sigma_a_, sigma_s_, g_)
    VS_HOTFIX_MAKE_RESTORE(Medium, sigma_a_, sigma_s_, g_)

    [[nodiscard]] Float3 sigma_t() const noexcept { return (*sigma_a_ + *sigma_s_) * *scale_; }
    [[nodiscard]] Float3 sigma_s() const noexcept { return *sigma_s_ * *scale_; }

    [[nodiscard]] SampledSpectrum Tr(const Float &t, const SampledWavelengths &swl) const noexcept {
        SampledSpectrum sigma_t_sp = spectrum()->decode_to_unbound_spectrum(sigma_t(), swl).sample;
        return sigma_t_sp.map([&](Float sigma) { return homogeneous_transmittance(sigma, t); });
    }

    void render_sub_UI(Widgets *widgets) noexcept override {
        Medium::render_sub_UI(widgets);
        changed_ |= widgets->drag_float3("sigma_a", addressof(sigma_a_.hv()), 0.1, 0);
        changed_ |= widgets->drag_float3("sigma_s", addressof(sigma_s_.hv()), 0.1, 0);
        changed_ |= widgets->drag_float("g", addressof(g_.hv()), 0.01, -g_clamp, g_clamp);
    }

    [[nodiscard]] SampledSpectrum Tr(const RayVar &ray, const SampledWavelengths &swl,
                                     TSampler &sampler) const noexcept override {
        return Tr(medium_world_distance(ray->t_max(), length(ray->direction())), swl);
    }

    [[nodiscard]] SampledSpectrum sample(const RayState &rs, Interaction &it, TSampler &sampler,
                                         const SampledWavelengths &swl) const noexcept override {
        RayVar ray = rs.ray;
        Uint medium_id = rs.medium;
        SampledSpectrum sigma_t_sp = spectrum()->decode_to_unbound_spectrum(sigma_t(), swl).sample;
        SampledSpectrum sigma_s_sp = spectrum()->decode_to_unbound_spectrum(sigma_s(), swl).sample;
        Float direction_length = length(ray->direction());
        SampledSpectrum ret{swl.dimension(), 1.f};
        $if(sigma_s_sp.is_zero()) {
            // Pure absorption has no scattering proposal. Its exact Beer law
            // also handles a zero-extinction medium without divisions by zero.
            ret = Tr(medium_world_distance(ray->t_max(), direction_length), swl);
        }
        $else {
            Uint channel = min(cast<uint>(sampler->next_1d() * swl.dimension()), swl.dimension() - 1);
            Float sigma = sigma_t_sp[channel];
            Float dist = homogeneous_sample_distance(sigma, sampler->next_1d());
            Float t = min(dist / direction_length, ray->t_max());
            Bool sampled_medium = sigma > 0.f && t < ray->t_max();
            $if(sampled_medium) {
                it = Interaction(ray->at(t), normalize(-ray->direction()), true);
                it.ng = make_float3(0.f);
                it.init_phase(*g_, swl);
                it.set_medium(medium_id, medium_id);
            };
            SampledSpectrum tr = Tr(medium_world_distance(t, direction_length), swl);
            SampledSpectrum density = select(sampled_medium, sigma_t_sp * tr, tr);
            Float pdf = density.average();
            Float safe_pdf = select(pdf > 0.f, pdf, 1.f);
            ret = select(sampled_medium, tr * sigma_s_sp, tr) / safe_pdf;
        };
        return ret;
    }
};

}// namespace vision

VS_MAKE_CLASS_CREATOR_HOTFIX(vision, HomogeneousMedium)
