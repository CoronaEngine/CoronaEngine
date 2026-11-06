#pragma once

#include "common.h"
#include "math/optics.h"

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

[[nodiscard]] inline Float3 bitangent(const Float3 &normal) {
    const Float3 abs_normal = abs(normal);
    const Uint x = select(abs_normal.x < abs_normal.y, 1u, 0u) &
                   select(abs_normal.x < abs_normal.z, 1u, 0u);
    const Uint y = (1u ^ x) & select(abs_normal.y < abs_normal.z, 1u, 0u);
    const Uint z = 1u ^ (x | y);
    return normalize(cross(normal, make_float3(x.cast<float>(), y.cast<float>(), z.cast<float>())));
}

[[nodiscard]] inline Float3 world_to_tangent(const Surface &surface, const Float3 &direction) {
    const Float3 b = bitangent(surface.normal);
    const Float3 t = cross(b, surface.normal);
    return make_float3(dot(b, direction), dot(t, direction), dot(surface.normal, direction));
}

[[nodiscard]] inline Float3 tangent_to_world(const Surface &surface, const Float3 &direction) {
    const Float3 b = bitangent(surface.normal);
    const Float3 t = cross(b, surface.normal);
    return b * direction.x + t * direction.y + surface.normal * direction.z;
}

[[nodiscard]] inline Float3 cosine_hemisphere(const Float2 &sample) {
    const Float2 square = 2.f * sample - 1.f;
    Float3 direction = make_float3(0.f, 0.f, 1.f);
    $if(any(square != 0.f)) {
        Float radius;
        Float phi;
        $if(abs(square.x) > abs(square.y)) {
            radius = square.x;
            phi = (square.y / square.x) * (Pi / 4.f);
        } $else {
            radius = square.y;
            phi = (Pi / 2.f) - (square.x / square.y) * (Pi / 4.f);
        };
        const Float2 disk = radius * make_float2(cos(phi), sin(phi));
        direction = make_float3(disk, sqrt(max(0.f, 1.f - dot(disk, disk))));
    };
    return direction;
}

[[nodiscard]] inline Float3 sample_ggx_ndf(const Float &alpha, const Float2 &sample) {
    const Float phi = sample.y * (2.f * Pi);
    const Float tan_squared = alpha * alpha * sample.x / (1.f - sample.x);
    const Float cos_theta = 1.f / sqrt(1.f + tan_squared);
    const Float radius = sqrt(max(1.f - cos_theta * cos_theta, 0.f));
    return make_float3(cos(phi) * radius, sin(phi) * radius, cos_theta);
}

[[nodiscard]] inline Float3 sample_brdf(const Surface &surface, Uint &rng) {
    const Float lobe = next_random(rng);
    const Float u = next_random(rng);
    const Float v = next_random(rng);
    const Float2 sample = make_float2(u, v);
    Float3 direction;
    $if(lobe < surface.diffuse_prob) {
        direction = tangent_to_world(surface, cosine_hemisphere(sample));
    } $else {
        const Float alpha = surface.roughness * surface.roughness;
        const Float3 half_vector = tangent_to_world(surface, sample_ggx_ndf(alpha, sample));
        direction = vision::reflect(surface.view_dir, half_vector);
    };
    return direction;
}

[[nodiscard]] inline Float brdf_pdf(const Surface &surface, const Float3 &direction) {
    const Float cos_theta = saturate(dot(surface.normal, direction));
    Float pdf = 0.f;
    $if(cos_theta > 0.f) {
        const Float diffuse_pdf = cos_theta / Pi;
        const Float3 h = world_to_tangent(surface, normalize(direction + surface.view_dir));
        const Float alpha = surface.roughness * surface.roughness;
        const Float specular_pdf = ggx_ndf(alpha, h.z) * h.z /
            (4.f * saturate(dot(h, world_to_tangent(surface, surface.view_dir))));
        pdf = ocarina::lerp(surface.diffuse_prob, specular_pdf, diffuse_pdf);
    };
    return pdf;
}

} // namespace vision::falcor_di
