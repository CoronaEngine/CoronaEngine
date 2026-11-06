#pragma once

#include "common.h"

namespace vision::falcor_di {

// Guide BRDF adapted from the Falcor bridge credited in common.h.

[[nodiscard]] inline Float ggx_ndf(const Float &alpha, const Float &cos_theta) {
    const Float alpha_squared = alpha * alpha;
    const Float d = (cos_theta * alpha_squared - cos_theta) * cos_theta + 1.f;
    return alpha_squared / (d * d * Pi);
}

[[nodiscard]] inline Float ggx_lambda(const Float &alpha_squared, const Float &cos_theta) {
    Float value = 0.f;
    $if(!(cos_theta <= 0.f)) {
        const Float cos_squared = cos_theta * cos_theta;
        value = 0.5f * (-1.f + sqrt(1.f + alpha_squared * (max(1.f - cos_squared, 0.f) / cos_squared)));
    };
    return value;
}

[[nodiscard]] inline Float target_pdf(const LightSample &sample, const Surface &surface) {
    Float target = 0.f;
    $if(!(sample.pdf <= 0.f)) {
        const Float3 half_vector = normalize(surface.view_dir + sample.direction);
        const Float n_dot_v = saturate(dot(surface.normal, surface.view_dir));
        const Float n_dot_l = saturate(dot(surface.normal, sample.direction));
        const Float n_dot_h = saturate(dot(surface.normal, half_vector));
        const Float l_dot_h = saturate(dot(sample.direction, half_vector));
        const Float alpha = surface.roughness * surface.roughness;
        const Float d = ggx_ndf(alpha, n_dot_h);
        const Float g = 1.f / ((1.f + ggx_lambda(alpha, n_dot_l)) * (1.f + ggx_lambda(alpha, n_dot_v)));
        Float f = 0.f;
        $if(!(surface.specular < 1e-8f)) {
            f = (surface.specular + (1.f - surface.specular) * pow(max(1.f - l_dot_h, 0.f), 5.f))
                / surface.specular;
        };
        const Float diffuse = n_dot_l * (1.f / Pi);
        const Float specular = d * g * f / (4.f * n_dot_v);
        target = max(0.f, ocarina::lerp(surface.diffuse_prob, specular, diffuse)) * sample.radiance.x;
    };
    return target;
}

} // namespace vision::falcor_di
