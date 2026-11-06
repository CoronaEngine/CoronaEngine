#pragma once

#include "brdf.h"
#include "reservoir.h"

namespace vision::falcor_di {

// Bitterli et al. 2020, doi:10.1145/3386569.3392481.
// Lin et al. 2022 GRIS supplemental, section S.1.2 (generalized pairwise MIS).
struct HistoryInput {
    Surface surface;
    Reservoir reservoir;
};

[[nodiscard]] inline Float2 reuse_random2(Uint &rng) {
    const Float x = next_random(rng);
    const Float y = next_random(rng);
    return make_float2(x, y);
}

[[nodiscard]] inline Bool reuse_similar(const Surface &receiver, const Surface &history) {
    return valid_surface(history) &&
           dot(receiver.normal, history.normal) >= 0.8191520442889918f &&
           abs(receiver.depth - history.depth) <= 0.1f * max(receiver.depth, 1e-6f);
}

[[nodiscard]] inline Reservoir read_history(const Context &ctx, const Int2 &pixel, const Uint &slice,
                                            const Float &history_limit) {
    Reservoir reservoir = load_reservoir(ctx, cast<uint2>(pixel), slice);
    reservoir.count = min(reservoir.count, history_limit);
    return reservoir;
}

[[nodiscard]] inline Float reuse_target(const Light &light, const Float2 &uv,
                                       const Surface &surface, bool trace) {
    const LightSample sample = sample_triangle(light, surface, uv, true);
    const Float sample_target = target_pdf(sample, surface);
    Float target = 0.f;
    $if(finite(sample_target) && sample_target > 0.f) {
        target = sample_target;
        if (trace) {
            $if(!visible(surface, sample)) {
                target = 0.f;
            };
        }
    };
    return target;
}

[[nodiscard]] inline Float target_at(const Context &ctx, const Reservoir &candidate, const Surface &surface) {
    Float target = 0.f;
    $if(valid_reservoir(candidate)) {
        target = reuse_target(load_light(ctx, candidate.light_index), candidate.uv, surface, false);
    };
    return target;
}

[[nodiscard]] inline Bool stream_history(const Context &ctx, Reservoir &output, const Reservoir &candidate,
                                         const Surface &receiver, const Float &u) {
    Bool selected = false;
    $if(!(candidate.count <= 0.f)) {
        const Float target = target_at(ctx, candidate, receiver);
        const Float weight = select(target > 0.f, target * candidate.weight * candidate.count, 0.f);
        selected = stream_reservoir(output, candidate.light_index, candidate.uv, target, weight, candidate.count, u);
    };
    return selected;
}

[[nodiscard]] inline Reservoir reuse_talbot(const Context &ctx, const Reservoir &current, const Surface &receiver,
                                            const HistoryInput &temporal, const HistoryInput &spatial,
                                            bool trace, Uint &rng) {
    Reservoir output;
    const Float current_weight = select(valid_reservoir(current), current.target * current.weight * current.count, 0.f);
    stream_reservoir(output, current.light_index, current.uv, current.target, current_weight, current.count, 0.f);

    Uint selected_source = 0u;
    const Float temporal_random = next_random(rng);
    $if(stream_history(ctx, output, temporal.reservoir, receiver, temporal_random)) {
        selected_source = 1u;
    };
    const Float spatial_random = next_random(rng);
    $if(stream_history(ctx, output, spatial.reservoir, receiver, spatial_random)) {
        selected_source = 2u;
    };

    $if(valid_reservoir(output)) {
        // Re-evaluate the winner on each admitted history receiver to normalize against its support.
        const Light winner = load_light(ctx, output.light_index);
        Float numerator = output.target;
        Float denominator = current.count * output.target;
        $if(temporal.reservoir.count > 0.f) {
            const Float target = reuse_target(winner, output.uv, temporal.surface, trace);
            numerator = select(selected_source == 1u, target, numerator);
            denominator += temporal.reservoir.count * target;
        };
        $if(spatial.reservoir.count > 0.f) {
            const Float target = reuse_target(winner, output.uv, spatial.surface, trace);
            numerator = select(selected_source == 2u, target, numerator);
            denominator += spatial.reservoir.count * target;
        };
        finalize_reservoir(output, numerator, denominator);
    };
    return output;
}

[[nodiscard]] inline Float pair_balance(const Float &own, const Float &other) {
    const Float total = own + other;
    return select(total > 0.f, own / total, 0.f);
}

inline void stream_pair(const Context &ctx, Reservoir &output, Float &canonical_mis, const Reservoir &canonical,
                        const Surface &receiver, const HistoryInput &history, const Float &pair_count, const Float &u) {
    const Reservoir &neighbor = history.reservoir;
    $if(!(neighbor.count <= 0.f)) {
        const Float target_neighbor_at_current = target_at(ctx, neighbor, receiver);
        const Float target_current_at_neighbor = target_at(ctx, canonical, history.surface);
        const Float neighbor_confidence = pair_count * neighbor.count;
        const Float neighbor_mis = pair_balance(neighbor_confidence * neighbor.target,
                                                canonical.count * target_neighbor_at_current) / pair_count;
        canonical_mis += pair_balance(canonical.count * canonical.target,
                                      neighbor_confidence * target_current_at_neighbor) / pair_count;

        const Float weight = select(target_neighbor_at_current > 0.f,
                                    target_neighbor_at_current * neighbor.weight * neighbor_mis, 0.f);
        stream_reservoir(output, neighbor.light_index, neighbor.uv, target_neighbor_at_current, weight, neighbor.count, u);
    };
}

[[nodiscard]] inline Reservoir reuse_pairwise(const Context &ctx, const Reservoir &current, const Surface &receiver,
                                              const HistoryInput &temporal, const HistoryInput &spatial, Uint &rng) {
    const Float pair_count = cast<float>(temporal.reservoir.count > 0.f) + cast<float>(spatial.reservoir.count > 0.f);
    Reservoir output;
    $if(pair_count == 0.f) {
        output = current;
    } $else {
        Float canonical_mis = 0.f;
        const Float temporal_random = next_random(rng);
        stream_pair(ctx, output, canonical_mis, current, receiver, temporal, pair_count, temporal_random);
        const Float spatial_random = next_random(rng);
        stream_pair(ctx, output, canonical_mis, current, receiver, spatial, pair_count, spatial_random);
        const Float current_random = next_random(rng);
        const Float weight = select(valid_reservoir(current), current.target * current.weight * canonical_mis, 0.f);
        stream_reservoir(output, current.light_index, current.uv, current.target, weight, current.count, current_random);
        // The balance shares already partition unity across the pairs.
        finalize_reservoir(output, 1.f, 1.f);
    };
    return output;
}

[[nodiscard]] inline Reservoir reuse_direct(const Context &ctx, const Uint2 &pixel, const Surface &receiver,
                                            const Reservoir &current, const Uint &history_slice, uint bias_mode) {
    Reservoir output = current;
    $if(ctx.has_history) {
        Uint rng = init_random(pixel, ctx.frame, 4u);
        const Float2 motion_vector = motion(pixel_index(pixel, ctx.extent));
        const Float2 jitter = reuse_random2(rng);
        const Float2 projected = cast<float2>(pixel) + motion_vector + jitter - 0.5f;
        // Match the original reprojection: round half-integers to even.
        const Int2 reprojected = cast<int2>(ocarina::call<float2>("round", projected));
        const Float history_limit = 20.f * current.count;
        HistoryInput temporal, spatial;
        Int2 temporal_pixel = make_int2(-1);

        // Search the reprojected pixel first, then try a small local neighborhood.
        $for(attempt, 9u) {
            Int2 offset = make_int2(0);
            $if(attempt != 0u) {
                offset = cast<int2>(4.f * (reuse_random2(rng) - 0.5f));
            };
            const Int2 previous_pixel = reprojected + offset;
            const Surface previous = load_surface(ctx, previous_pixel, true);
            $if(reuse_similar(receiver, previous)) {
                temporal.surface = previous;
                temporal.reservoir = read_history(ctx, previous_pixel, history_slice, history_limit);
                temporal_pixel = previous_pixel;
                $break;
            };
        };

        // One spatial neighbor from the previous frame keeps reads and writes in separate slices.
        const Uint neighbor_index = cast<uint>(next_random(rng) * float(neighbor_count));
        const Int2 spatial_pixel = reprojected + cast<int2>(30.f * ctx.neighbors.read(neighbor_index, false));
        $if(any(spatial_pixel != temporal_pixel)) {
            const Surface neighbor = load_surface(ctx, spatial_pixel, true);
            $if(reuse_similar(receiver, neighbor)) {
                spatial.surface = neighbor;
                spatial.reservoir = read_history(ctx, spatial_pixel, history_slice, history_limit);
            };
        };

        if (bias_mode == 2u) {
            output = reuse_pairwise(ctx, current, receiver, temporal, spatial, rng);
        } else {
            output = reuse_talbot(ctx, current, receiver, temporal, spatial, bias_mode == 3u, rng);
        }
    };
    return output;
}

} // namespace vision::falcor_di
