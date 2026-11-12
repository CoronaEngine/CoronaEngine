#pragma once

#include "brdf.h"
#include "reservoir.h"

namespace vision::falcor_di {

// Initial RIS uses the balance heuristic and the unnormalized receiver target.
inline constexpr uint initial_light_count = 10u;
inline constexpr uint initial_candidate_count = initial_light_count + 1u;
inline constexpr uint screen_tile_size = 16u;
inline constexpr uint ris_point_words = 3u;

[[nodiscard]] inline Float initial_mixture_pdf(const Float &light_probability, const Float &bsdf_probability,
                                               const Float &conditional_pdf) {
    return (float(initial_light_count) * light_probability + max(0.f, bsdf_probability / conditional_pdf)) /
        float(initial_candidate_count);
}

inline void presample_light(const Context &ctx, const Uint2 &position, TSampler &sampler) {
    sampler->set_seed(position, ctx.frame, Dimension::FalcorDI_presample);
    const Float u = sampler->next_1d();
    Float cumulative = 0.f;
    Uint selected = InvalidUI32;
    Float selected_probability = 0.f;
    if (ctx.environment_index != InvalidUI32) {
        const Float scaled = u * float(ctx.light_count);
        const Uint slot = min(cast<uint>(scaled), ctx.light_count - 1u);
        const Uint2 entry = ctx.light_alias.read(slot, false);
        selected = select(scaled - cast<float>(slot) < as<float>(entry.x), slot, entry.y);
        selected_probability = ctx.probabilities.read(selected, false);
    } else {
        $for(light, ctx.light_count) {
            const Float probability = ctx.probabilities.read(light, false);
            $if(!(probability > 0.f)) { $continue; };
            // Preserve the original area-only CDF and random-number mapping.
            selected = light;
            selected_probability = probability;
            cumulative += probability;
            $if(u < cumulative) { $break; };
        };
    }
    const Uint index = position.y * tile_size + position.x;
    ctx.ris.write(index, make_uint2(selected, as<uint>(selected_probability)), false);
    if (ctx.precompute_light_points) {
        $if(selected != InvalidUI32) {
            // Share the sampled point across pixels that draw this RIS entry.
            const Float2 uv = sampler->next_2d();
            const auto triangle = ctx.light_geometry.read(selected, false);
            const auto point = load_light_point(ctx, selected, triangle, uv);
            const auto emission = light_emission(ctx, selected, triangle, point);
            const Uint base = index * ris_point_words;
            ctx.ris_points.write(base, make_float4(point.pos, point.PDF_pos), false);
            ctx.ris_points.write(base + 1u, make_float4(point.ng, uv.x), false);
            ctx.ris_points.write(base + 2u, make_float4(emission, uv.y), false);
        };
    }
}

[[nodiscard]] inline ReservoirVar sample_initial(const Context &ctx, const Uint2 &pixel, const Surface &surface,
                                                const MaterialEvaluator &bsdf, const Float3 &wo,
                                                TSampler &sampler,
                                                HitBSDFVar *gi_proposal) {
    ReservoirVar reservoir;
    Float2 winner_uv = make_float2(0.f);
    TriangleSample winner_light;
    const bool use_proxy = ctx.precompute_light_points;
    // The reservoir represents a continuous area-light sample, so a material
    // with a discrete lobe uses only the light proposal.
    const Uint flags = bsdf.flag();
    const Bool use_bsdf = !BxDFFlag::is_specular(flags);
    const auto offer_candidate = [&](const Uint &light_index, const Float2 &uv, const Float &light_probability,
                                     const TriangleSample &light, const Float &u, const Float *sampled_pdf = nullptr) {
        $if(light.pdf > 0.f) {
            const Float cosine = dot(surface.normal, light.direction);
            const Bool bsdf_supported = use_bsdf && light.distance >= ray_epsilon && cosine > 0.f;
            Float target = 0.f;
            Float bsdf_probability = 0.f;
            if (use_proxy) {
                // The nonzero floor keeps support for scattering that differs from this guide.
                const Float angular_weight = select(BxDFFlag::is_transmission(flags), abs(cosine), max(0.f, cosine));
                target = luminance(abs(light.radiance)) * (angular_weight + 0.015625f);
                if (sampled_pdf) {
                    bsdf_probability = *sampled_pdf;
                } else {
                    $if(bsdf_supported && target > 0.f && finite(target)) {
                        bsdf_probability = bsdf.evaluate(wo, light.direction, MaterialEvalMode::PDF).pdf();
                    };
                }
            } else {
                const auto eval = bsdf.evaluate(
                    wo, light.direction, sampled_pdf ? MaterialEvalMode::F : MaterialEvalMode::All);
                target = target_weight(eval, light);
                if (sampled_pdf) {
                    bsdf_probability = *sampled_pdf;
                } else {
                    bsdf_probability = select(bsdf_supported, eval.pdf(), 0.f);
                }
            }
            $if(target > 0.f && finite(target)) {
                const Float mixture_pdf = initial_mixture_pdf(light_probability, bsdf_probability, light.pdf);
                $if(mixture_pdf > 0.f && finite(mixture_pdf)) {
                    const Float candidate_weight = target / mixture_pdf;
                    $if(finite(candidate_weight)) {
                        stream_candidate(reservoir, light_index, target, candidate_weight, 1.f, u, [&] {
                            winner_uv = uv;
                            if (use_proxy) {
                                winner_light.direction = light.direction;
                                winner_light.radiance = light.radiance;
                            }
                        });
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
            const Uint ris_index = tile * tile_size + offset;
            const Uint2 entry = ctx.ris.read(ris_index, false);
            // Keep replacement draws on the same pixel RNG stream when the RIS entry supplies UV.
            const Float2 uv = light_sampler->next_2d();
            const Float u = light_sampler->next_1d();
            $if(entry.x != InvalidUI32) {
                if (ctx.precompute_light_points) {
                    const Uint base = ris_index * ris_point_words;
                    const Float4 position_pdf = ctx.ris_points.read(base, false);
                    const Float4 normal_u = ctx.ris_points.read(base + 1u, false);
                    const Float4 emission_v = ctx.ris_points.read(base + 2u, false);
                    LightEvalContext point;
                    point.pos = position_pdf.xyz();
                    point.PDF_pos = position_pdf.w;
                    point.ng = normal_u.xyz();
                    const auto light = evaluate_light_point(ctx, entry.x, emission_v.xyz(), point, surface);
                    offer_candidate(entry.x, make_float2(normal_u.w, emission_v.w), as<float>(entry.y), light, u);
                } else {
                    const auto light = sample_light(ctx, entry.x, surface, uv);
                    offer_candidate(entry.x, uv, as<float>(entry.y), light, u);
                }
            };
        };
        bsdf_replacement = light_sampler->next_1d();
    });

    $if(gi_proposal ? Bool{true} : use_bsdf) {
        const auto sample = bsdf.sample(wo, sampler);
        const Float pdf = sample.eval.pdf();
        if (gi_proposal) {
            // GI consumes the proposal before the DI light-hit and hemisphere tests.
            gi_proposal->wi.set(sample.wi);
            gi_proposal->bsdf.set(sample.eval.f.vec3());
            gi_proposal->pdf = pdf;
        }
        const Bool eligible_for_di = gi_proposal ? use_bsdf : Bool{true};
        $if(eligible_for_di && pdf > 0.f && finite(pdf) && dot(surface.normal, sample.wi) > 0.f) {
            const Float3 origin = Interaction::custom_offset_ray_origin(surface.position, surface.geometric_normal,
                                                                        sample.wi);
            const Uint4 hit = trace_light(ctx, origin, sample.wi, ray_epsilon, environment_ray_distance);
            $if(hit.x != InvalidUI32) {
                // The folded-square sampler accepts the native triangle hit weights.
                const Float2 uv = make_float2(as<float>(hit.z), as<float>(hit.w));
                const auto light = sample_light(ctx, hit.x, surface, uv);
                offer_candidate(hit.x, uv, ctx.probabilities.read(hit.x, false), light, bsdf_replacement, &pdf);
            };
        };
    };

    reservoir->set_uv(winner_uv);
    finalize_reservoir(reservoir, 1.f, float(initial_candidate_count));
    if (use_proxy) {
        // Keep the RIS weight normalized by the target that selected this sample.
        // Reuse consumes the exact target with that weight and its ordinary MIS.
        $if(valid_reservoir(reservoir)) {
            const auto eval = bsdf.evaluate(wo, winner_light.direction, MaterialEvalMode::F);
            reservoir.target = target_weight(eval, winner_light);
        };
    }
    // Reuse receives one finalized estimate. Null draws still occupy their mixture slots.
    reservoir.count = 1.f;
    return reservoir;
}

} // namespace vision::falcor_di
