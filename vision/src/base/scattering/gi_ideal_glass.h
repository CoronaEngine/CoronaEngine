#pragma once

#include "sample.h"
#include "math/optics.h"

namespace vision {

[[nodiscard]] inline Bool gi_ideal_glass_enabled(bool gi_scope, const Bool &complete_glass,
                                               const Float &roughness) noexcept {
    return gi_scope && complete_glass && roughness * roughness < .0064f;
}

// Corona stores a discrete branch mass in pdfs so valid()/throughput() retain
// their contracts. Specular flags distinguish this mass from a solid-angle
// density; emission MIS must give such a sampled delta ray weight one.
[[nodiscard]] inline BSDFSample gi_ideal_glass_branch(const SampledWavelengths &swl,
                                                     const Float3 &wo, const Float &eta,
                                                     const SampledSpectrum &color,
                                                     bool transmission) noexcept {
    BSDFSample ret{swl};
    Float F = fresnel_dielectric(abs_cos_theta(wo), eta);
    Float probability = transmission ? 1.f - F : F;
    $if(probability > 0.f) {
        Bool valid = true;
        ret.wi = make_float3(-wo.x, -wo.y, wo.z);
        SampledSpectrum weight = SampledSpectrum::one(swl);
        if (transmission) {
            valid = refract(wo, face_forward(make_float3(0.f, 0.f, 1.f), wo), eta, &ret.wi);
            // Native thick glass applies half of the authored tint at each
            // interface; entry plus exit therefore recovers the full color.
            weight = color.map([](const Float &v) { return ocarina::sqrt(max(v, 0.f)); });
            ret.eta = eta;
        }
        $if(valid && abs_cos_theta(ret.wi) >= 1e-6f) {
            ret.eval.pdfs = probability;
            ret.eval.f = weight * probability / abs_cos_theta(ret.wi);
            ret.eval.flags = transmission ? BxDFFlag::SpecTrans : BxDFFlag::SpecRefl;
            ret.stable_lobe_index = transmission ? StableLobes::transmission_index : StableLobes::reflection_index;
        };
    };
    return ret;
}

[[nodiscard]] inline Bool gi_prefix_endpoint_matches(const Uint &branch, const Uint &depth,
                                                     const Float3 &direction, const Uint &expected_branch,
                                                     const Uint &expected_depth, const Float3 &expected_direction) noexcept {
    return branch != 0u && branch == expected_branch && depth == expected_depth &&
           length_squared(direction - expected_direction) <= 1e-8f;
}

[[nodiscard]] inline Uint gi_prefix_next_branch(const Uint &branch, const Uint &stable_lobe,
                                               const Bool &valid) noexcept {
    return ocarina::select(branch != 0u && stable_lobe < 2u && valid,
                           (branch << 2u) | (stable_lobe + 1u), Uint{0u});
}

[[nodiscard]] inline Uint gi_prefix_reuse_lobe(const Bool &transmission, const Bool &reflection) noexcept {
    return ocarina::select(transmission, Uint{StableLobes::transmission_index},
                           ocarina::select(reflection, Uint{StableLobes::reflection_index}, Uint{InvalidUI32}));
}

[[nodiscard]] inline Bool gi_camera_emission_owned_by_di(const Bool &first_transmission,
                                                        const Bool &first_reflection_covered_by_di,
                                                        const Uint &bounce) noexcept {
    return bounce == 0u || (bounce == 1u && first_reflection_covered_by_di && !first_transmission);
}

[[nodiscard]] inline Float gi_camera_emission_bsdf_weight(const Float &original,
                                                         const Bool &first_transmission,
                                                         const Bool &first_reflection_covered_by_di,
                                                         const Uint &bounce) noexcept {
    return ocarina::select(bounce == 1u && !gi_camera_emission_owned_by_di(
        first_transmission, first_reflection_covered_by_di, bounce), Float{1.f}, original);
}

[[nodiscard]] inline Bool gi_prefix_stable_emission_allowed(const Bool &ideal_path, const Uint &depth,
                                                           const Uint &branch = Uint(0u),
                                                           const Bool &first_reflection_covered_by_di = Bool(true)) noexcept {
    // Branch 5 is the first camera transmission: (root 1 << 2) | (lobe 0 + 1).
    return ideal_path && (depth > 2u || (depth == 2u && (branch == 5u || !first_reflection_covered_by_di)));
}

[[nodiscard]] inline Bool gi_prefix_guide_eligible(const Bool &hit, const Bool &consumed, const Uint &plane) noexcept {
    return hit && (!consumed || plane == 0u);
}

[[nodiscard]] inline Bool gi_prefix_receiver_eligible(const Bool &hit, const Bool &consumed,
                                                     const Bool &stochastic_remainder) noexcept {
    return hit && !consumed && stochastic_remainder;
}

[[nodiscard]] inline Bool gi_prefix_emission_represented(const Bool &ideal_path, const Uint &branch,
                                                         const Uint &depth, const Float3 &direction,
                                                         const Uint &terminal_branch, const Uint &terminal_depth,
                                                         const Float3 &terminal_direction,
                                                         const Bool &terminal_identity,
                                                         const Bool &first_reflection_covered_by_di = Bool(true)) noexcept {
    // Prefix construction is bounded to five vertices. Intermediate ancestry
    // is established by identical camera rays and true ideal samples; only an
    // exact terminal depth additionally needs its stored hit and direction.
    Uint relative = ocarina::select(depth <= terminal_depth, terminal_depth - depth, Uint{0u});
    Bool ancestor = branch != 0u && depth <= terminal_depth &&
                    branch == (terminal_branch >> (relative * 2u));
    return gi_prefix_stable_emission_allowed(ideal_path, depth, branch, first_reflection_covered_by_di) && ancestor &&
           (depth < terminal_depth || (terminal_identity && length_squared(direction - terminal_direction) <= 1e-8f));
}

}// namespace vision
