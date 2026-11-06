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

inline void offer_initial_candidate(const Context &ctx, Reservoir &reservoir, const Surface &surface,
                                    const Uint &light_index, const Float2 &uv,
                                    const Float &light_probability, const Float &u) {
    $if(light_index != InvalidUI32) {
        const LightSample light = sample_triangle(load_light(ctx, light_index), surface, uv, true);
        $if(light.pdf > 0.f && finite(light.pdf)) {
            const Float target = target_pdf(light, surface);
            $if(target > 0.f && finite(target)) {
                // Convert the BRDF PDF to triangle UV measure before mixing it with the light PMF.
                Float brdf_probability = 0.f;
                $if(light.distance >= ray_epsilon) {
                    brdf_probability = brdf_pdf(surface, light.direction);
                };
                const Float mixture_pdf = (float(initial_light_count) * light_probability +
                    max(0.f, brdf_probability / light.pdf)) / float(initial_candidate_count);
                $if(mixture_pdf > 0.f && finite(mixture_pdf)) {
                    const Float candidate_weight = target / mixture_pdf;
                    $if(finite(candidate_weight)) {
                        stream_reservoir(reservoir, light_index, uv, target, candidate_weight, 1.f, u);
                    };
                };
            };
        };
    };
}

[[nodiscard]] inline Reservoir sample_initial(const Context &ctx, const Uint2 &pixel, const Surface &surface) {
    Reservoir reservoir;
    $if(valid_surface(surface)) {
        Uint rng = init_random(pixel, ctx.frame, 2u);
        Uint tile_rng = init_random(pixel / screen_tile_size, ctx.frame, 3u);
        const Uint tile = cast<uint>(next_random(tile_rng) * float(tile_count));
        $for(i, initial_light_count) {
            const Uint offset = cast<uint>(next_random(rng) * float(tile_size));
            const Uint2 entry = ctx.ris.read(tile * tile_size + offset, false);
            const Float x = next_random(rng);
            const Float y = next_random(rng);
            const Float u = next_random(rng);
            offer_initial_candidate(ctx, reservoir, surface, entry.x, make_float2(x, y), as<float>(entry.y), u);
        };

        const Float3 direction = sample_brdf(surface, rng);
        const Float u = next_random(rng);
        $if(dot(surface.normal, direction) > 0.f) {
            const Uint4 hit = trace_light(ctx, surface.position, direction, ray_epsilon, 3.402823466e38f);
            $if(hit.y != InvalidUI32 && hit.x != InvalidUI32) {
                // The folded-square sampler accepts the native triangle hit weights.
                offer_initial_candidate(ctx, reservoir, surface, hit.x, make_float2(as<float>(hit.z), as<float>(hit.w)),
                                        ctx.probabilities.read(hit.x, false), u);
            };
        };

        finalize_reservoir(reservoir, 1.f, float(initial_candidate_count));
        // Reuse receives one finalized estimate. Rejected draws still occupy their mixture slots.
        reservoir.count = 1.f;
    };
    return reservoir;
}

} // namespace vision::falcor_di
