#pragma once

#include "brdf.h"
#include "reservoir.h"

namespace vision::falcor_di {

// Initial RIS uses the balance heuristic and the unnormalized receiver target.
inline constexpr uint initial_light_count = 10u;
inline constexpr uint initial_candidate_count = initial_light_count + 1u;
inline constexpr uint screen_tile_size = 16u;

inline void presample_light(const Context &ctx, const Uint2 &position, TSampler &sampler) {
    sampler->set_seed(position, ctx.frame, Dimension::FalcorDI_presample);
    const Float u = sampler->next_1d();
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

[[nodiscard]] inline ReservoirVar sample_initial(const Context &ctx, const Uint2 &pixel, const Surface &surface,
                                                const MaterialEvaluator &bsdf, const Float3 &wo,
                                                const SampledWavelengths &swl, TSampler &sampler) {
    ReservoirVar reservoir;
    Float2 winner_uv = make_float2(0.f);
    // The reservoir represents a continuous area-light sample, so a material
    // with a discrete lobe uses only the light proposal.
    const Bool use_bsdf = !BxDFFlag::is_specular(bsdf.flag());
    const auto offer_candidate = [&](const Uint &light_index, const Float2 &uv, const Float &light_probability,
                                     const Float &u, const Float *sampled_pdf = nullptr) {
        $if(light_index != InvalidUI32) {
            const TriangleSample light = sample_light(ctx, light_index, surface, uv, swl);
            $if(light.pdf > 0.f) {
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
                            stream_candidate(reservoir, light_index, target, candidate_weight, 1.f, u, [&] {
                                winner_uv = uv;
                            });
                        };
                    };
                };
            };
        };
    };

    Float bsdf_replacement = 0.f;
    // Use a separate stream for light proposals, then restore the BSDF stream.
    sampler->temporary([&](Sampler *light_sampler) {
        light_sampler->set_seed(pixel / screen_tile_size, ctx.frame, Dimension::FalcorDI_tile);
        const Uint tile = cast<uint>(light_sampler->next_1d() * float(tile_count));
        light_sampler->set_seed(pixel, ctx.frame, Dimension::FalcorDI_candidates);
        $for(i, initial_light_count) {
            const Uint offset = cast<uint>(light_sampler->next_1d() * float(tile_size));
            const Uint2 entry = ctx.ris.read(tile * tile_size + offset, false);
            const Float2 uv = light_sampler->next_2d();
            const Float u = light_sampler->next_1d();
            offer_candidate(entry.x, uv, as<float>(entry.y), u);
        };
        bsdf_replacement = light_sampler->next_1d();
    });

    $if(use_bsdf) {
        const auto sample = bsdf.sample(wo, sampler);
        const Float pdf = sample.eval.pdf();
        $if(pdf > 0.f && finite(pdf) && dot(surface.normal, sample.wi) > 0.f) {
            const Uint4 hit = trace_light(ctx, surface.position, sample.wi, ray_epsilon, 3.402823466e38f);
            $if(hit.y != InvalidUI32 && hit.x != InvalidUI32) {
                // The folded-square sampler accepts the native triangle hit weights.
                offer_candidate(hit.x, make_float2(as<float>(hit.z), as<float>(hit.w)),
                                ctx.probabilities.read(hit.x, false), bsdf_replacement, &pdf);
            };
        };
    };

    reservoir->set_uv(winner_uv);
    finalize_reservoir(reservoir, 1.f, float(initial_candidate_count));
    // Reuse receives one finalized estimate. Null draws still occupy their mixture slots.
    reservoir.count = 1.f;
    return reservoir;
}

} // namespace vision::falcor_di
