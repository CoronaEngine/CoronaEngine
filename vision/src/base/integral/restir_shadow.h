#pragma once

#include "base/illumination/lightsampler.h"
#include "base/sampler.h"

namespace vision::restir_shadow {
// RTXPT uses an absolute endpoint exclusion and fixed directional travel.
inline constexpr float finite_epsilon = 1e-4f;
inline constexpr float directional_distance = 100000.f;

[[nodiscard]] inline RayVar finite_ray(const Float3 &origin, const Float3 &target) noexcept {
    const Float3 segment = target - origin;
    const Float distance = length(segment);
    const Float safe_distance = select(distance > 0.f, distance, 1.f);
    const Float3 direction = select(distance > 0.f, segment / safe_distance, make_float3(0.f, 0.f, 1.f));
    return make_ray(origin, direction, max(0.f, distance - finite_epsilon));
}

// unit_wi is the original sampled direction, independent of the origin offset.
[[nodiscard]] inline RayVar direction_ray(const Float3 &origin, const Float3 &unit_wi) noexcept {
    return make_ray(origin, unit_wi, directional_distance);
}

[[nodiscard]] inline RayVar visibility_ray(const Interaction &it, const Float3 &target,
                                          const Bool &infinite) noexcept {
    const Float3 wi = normalize(target - it.pos);
    const Float3 origin = Interaction::custom_offset_ray_origin(it.pos, it.ng, wi);
    RayVar ray;
    $if(infinite) { ray = direction_ray(origin, wi); }
    $else { ray = finite_ray(origin, target); };
    return ray;
}

[[nodiscard]] inline Bool is_infinite_light(const LightSampler &light_sampler, const Uint &index) noexcept {
    Bool infinite = false;
    const auto [type_id, inst_id] = light_sampler.extract_light_id(index);
    light_sampler.dispatch_light(type_id, inst_id, [&](const Light *light) {
        infinite = light->match(LightType::Infinite);
    });
    return infinite;
}

[[nodiscard]] inline LightSample sample_light(const LightSampler &light_sampler, const LightSampleContext &lsc,
                                             TSampler &sampler, const SampledWavelengths &swl,
                                             Bool &infinite) noexcept {
    // Identical draw/call order to LightSampler::sample_wi(lsc, sampler, swl).
    const Float selector = sampler->next_1d();
    const Float2 uv = sampler->next_2d();
    const SampledLight selected = light_sampler.select_light(lsc, selector);
    const LightSample sample = light_sampler.sample_wi(selected, lsc, uv, swl);
    infinite = is_infinite_light(light_sampler, selected.light_index);
    return sample;
}
}// namespace vision::restir_shadow
