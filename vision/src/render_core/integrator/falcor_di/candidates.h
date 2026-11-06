#pragma once

#include "brdf.h"
#include "reservoir.h"

namespace vision::falcor_di {

// Initial RIS uses the balance heuristic and the unnormalized receiver target.
inline constexpr uint initial_light_count = 10u;
inline constexpr uint initial_candidate_count = initial_light_count + 1u;
inline constexpr uint screen_tile_size = 16u;

inline void presample_light(const Context &ctx, const Uint2 &position) {
    Uint rng = init_random(position, ctx.frame, 0u);
    const Float u = next_random(rng);
    Float cumulative = 0.f;
    Uint selected = InvalidUI32;
    Float selected_probability = 0.f;
    $for(light, ctx.light_count) {
        const Float probability = ctx.probabilities.read(light, false);
        $if(!(probability > 0.f)) { $continue; };

        // Keep the last positive entry if the FP32 CDF ends below one.
        selected = light;
        selected_probability = probability;
        cumulative += probability;
        $if(u < cumulative) { $break; };
    };
    ctx.ris.write(position.y * tile_size + position.x, make_uint2(selected, as<uint>(selected_probability)), false);
}

[[nodiscard]] inline Reservoir sample_initial(const Context &ctx, const Uint2 &pixel, const Surface &surface,
                                              const MaterialEvaluator &bsdf, const Float3 &wo, TSampler &sampler) {
    Reservoir reservoir;
    // The reservoir represents a continuous area-light sample, so a material
    // with a discrete lobe uses only the light proposal.
    const Bool use_bsdf = !BxDFFlag::is_specular(bsdf.flag());
    const auto offer_candidate = [&](const Uint &light_index, const Float2 &uv, const Float &light_probability,
                                     const Float &u, const Float *sampled_pdf = nullptr) {
        $if(light_index != InvalidUI32) {
            const LightSample light = sample_triangle(load_light(ctx, light_index), surface, uv);
            $if(light.pdf > 0.f && finite(light.pdf)) {
                const auto eval = bsdf.evaluate(
                    wo, light.direction, sampled_pdf ? MaterialEvalMode::F : MaterialEvalMode::All);
                const Float target = target_weight(eval, light);
                $if(target > 0.f) {
                    Float bsdf_probability = 0.f;
                    if (sampled_pdf) {
                        bsdf_probability = *sampled_pdf;
                    } else {
                        $if(use_bsdf && light.distance >= ray_epsilon && dot(surface.normal, light.direction) > 0.f) {
                            bsdf_probability = eval.pdf();
                        };
                    }
                    // Convert the solid-angle density to the same triangle measure as the light proposal.
                    const Float mixture_pdf = (float(initial_light_count) * light_probability +
                        max(0.f, bsdf_probability / light.pdf)) / float(initial_candidate_count);
                    $if(mixture_pdf > 0.f && finite(mixture_pdf)) {
                        const Float candidate_weight = target / mixture_pdf;
                        $if(finite(candidate_weight)) {
                            stream_reservoir(reservoir, light_index, uv, target, candidate_weight, 1.f, u);
                        };
                    };
                };
            };
        };
    };

    Uint rng = init_random(pixel, ctx.frame, 2u);
    Uint tile_rng = init_random(pixel / screen_tile_size, ctx.frame, 3u);
    const Uint tile = cast<uint>(next_random(tile_rng) * float(tile_count));
    $for(i, initial_light_count) {
        const Uint offset = cast<uint>(next_random(rng) * float(tile_size));
        const Uint2 entry = ctx.ris.read(tile * tile_size + offset, false);
        const Float x = next_random(rng);
        const Float y = next_random(rng);
        const Float u = next_random(rng);
        offer_candidate(entry.x, make_float2(x, y), as<float>(entry.y), u);
    };

    $if(use_bsdf) {
        const auto sample = bsdf.sample(wo, sampler);
        const Float pdf = sample.eval.pdf();
        const Float u = next_random(rng);
        $if(pdf > 0.f && finite(pdf) && dot(surface.normal, sample.wi) > 0.f) {
            const Uint4 hit = trace_light(ctx, surface.position, sample.wi, ray_epsilon, 3.402823466e38f);
            $if(hit.y != InvalidUI32 && hit.x != InvalidUI32) {
                // The folded-square sampler accepts the native triangle hit weights.
                offer_candidate(hit.x, make_float2(as<float>(hit.z), as<float>(hit.w)),
                                ctx.probabilities.read(hit.x, false), u, &pdf);
            };
        };
    };

    finalize_reservoir(reservoir, 1.f, float(initial_candidate_count));
    // Reuse receives one finalized estimate. Null draws still occupy their mixture slots.
    reservoir.count = 1.f;
    return reservoir;
}

} // namespace vision::falcor_di
