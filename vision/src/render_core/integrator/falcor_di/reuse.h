#pragma once

#include "brdf.h"
#include "reservoir.h"

namespace vision::falcor_di {

// Bitterli et al. 2020, doi:10.1145/3386569.3392481.
// Lin et al. 2022 GRIS supplemental, section S.1.2 (generalized pairwise MIS).
struct HistoryInput {
    Surface surface;
    ReservoirVar reservoir;
    Uint pixel_index{0u};
};

struct ReceiverTargets {
    Float temporal{0.f};
    Float spatial{0.f};
};

[[nodiscard]] inline Bool reuse_similar(const Surface &receiver, const Surface &history) {
    return valid_surface(history) &&
           dot(receiver.normal, history.normal) >= 0.8191520442889918f &&
           abs(receiver.depth - history.depth) <= 0.1f * max(receiver.depth, 1e-6f);
}

[[nodiscard]] inline Bool same_shadow_receiver(const Surface &receiver, const Surface &history) {
    return all(as<uint3>(receiver.position) == as<uint3>(history.position)) &&
           all(as<uint3>(receiver.geometric_normal) == as<uint3>(history.geometric_normal));
}

[[nodiscard]] inline ReservoirVar read_history(const Context &ctx, const Int2 &pixel, const Uint &slice,
                                            const Float &history_limit) {
    ReservoirVar reservoir = load_reservoir(ctx, cast<uint2>(pixel), slice);
    reservoir.count = min(reservoir.count, history_limit);
    return reservoir;
}

[[nodiscard]] inline TriangleSample sample_history(const Context &ctx, const HistoryInput &history,
                                                   const Surface &receiver) {
    TriangleSample sample;
    $if(valid_reservoir(history.reservoir)) {
        sample = sample_light(ctx, history.reservoir.light_index, receiver, history.reservoir->uv());
    };
    return sample;
}

[[nodiscard]] inline Float target_at(const TriangleSample &sample, const MaterialEvaluator &bsdf, const Float3 &wo) {
    Float target = 0.f;
    $if(sample.pdf > 0.f) {
        target = target_weight(bsdf.evaluate(wo, sample.direction, MaterialEvalMode::F), sample);
    };
    return target;
}

[[nodiscard]] inline Float history_target(const Context &ctx, const Float3 &emission, const LightEvalContext &point,
                                          const Uint &light_index, const HistoryInput &history,
                                          const SampledWavelengths &swl, const Bool &trace) {
    return outline("Falcor DI history target", [&] {
        const TriangleSample sample = evaluate_light_point(ctx, light_index, emission, point, history.surface);
        Float target = 0.f;
        $if(sample.pdf > 0.f) {
            const auto hit = Toolkit::frame_buffer().prev_visibility_buffer_var(ctx.frame).read(history.pixel_index);
            $if(hit->is_hit()) {
                auto &camera = Toolkit::scene().sensor();
                camera->load_data();
                // This route uses a pinhole camera and static scene geometry.
                auto it = Toolkit::geometry().compute_surface_interaction(hit, camera->prev_device_position());
                Toolkit::scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                    const auto bsdf = material->create_evaluator(it, swl);
                    target = target_weight(bsdf.evaluate(it.wo, sample.direction, MaterialEvalMode::F), sample);
                });
            };
        };
        $if(trace && target > 0.f) { target = select(visible(history.surface, sample), target, 0.f); };
        return target;
    });
}

[[nodiscard]] inline Bool stream_history(ReservoirVar &output, const ReservoirVar &candidate,
                                         const Float &target, const Float &u) {
    Bool selected = false;
    $if(!(candidate.count <= 0.f)) {
        const Float weight = select(target > 0.f, target * candidate.weight * candidate.count, 0.f);
        selected = stream_reservoir(output, candidate.light_index, candidate.packed_uv, target, weight, candidate.count, u);
    };
    return selected;
}

[[nodiscard]] inline ReservoirVar reuse_talbot(const Context &ctx, const ReservoirVar &current,
                                            const HistoryInput &temporal, const HistoryInput &spatial,
                                            const ReceiverTargets &targets, const SampledWavelengths &swl,
                                            bool trace, TSampler &sampler) {
    ReservoirVar output;
    const Float current_weight = select(valid_reservoir(current), current.target * current.weight * current.count, 0.f);
    stream_reservoir(output, current.light_index, current.packed_uv, current.target, current_weight, current.count, 0.f);

    Uint selected_source = 0u;
    const Float temporal_random = sampler->next_1d();
    $if(stream_history(output, temporal.reservoir, targets.temporal, temporal_random)) {
        selected_source = 1u;
    };
    const Float spatial_random = sampler->next_1d();
    $if(stream_history(output, spatial.reservoir, targets.spatial, spatial_random)) {
        selected_source = 2u;
    };

    $if(valid_reservoir(output)) {
        // Re-evaluate the winner on each admitted history receiver to normalize against its support.
        const LightTriangleGeometryVar triangle = ctx.light_geometry.read(output.light_index, false);
        const LightEvalContext point = load_light_point(ctx, output.light_index, triangle, output->uv());
        const Float3 emission = light_emission(ctx, output.light_index, triangle, point);
        Float numerator = output.target;
        Float denominator = current.count * output.target;
        $if(temporal.reservoir.count > 0.f) {
            const Float target = history_target(ctx, emission, point, output.light_index, temporal, swl, Bool{trace});
            numerator = select(selected_source == 1u, target, numerator);
            denominator += temporal.reservoir.count * target;
        };
        $if(spatial.reservoir.count > 0.f) {
            const Float target = history_target(ctx, emission, point, output.light_index, spatial, swl, Bool{trace});
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

inline void stream_pair(const Context &ctx, ReservoirVar &output, Float &canonical_mis, const ReservoirVar &canonical,
                        const Surface &receiver, const Float2 &canonical_uv, const HistoryInput &history,
                        const Float &target_neighbor_at_current,
                        const Float &pair_count, const Float &u, const SampledWavelengths &swl) {
    const ReservoirVar &neighbor = history.reservoir;
    $if(!(neighbor.count <= 0.f)) {
        Float target_current_at_neighbor = 0.f;
        $if(valid_reservoir(canonical)) {
            const LightTriangleGeometryVar triangle = ctx.light_geometry.read(canonical.light_index, false);
            const auto point = load_light_point(ctx, canonical.light_index, triangle, canonical_uv);
            // The canonical sample was checked against this frame's acceleration structure.
            target_current_at_neighbor = history_target(
                ctx, light_emission(ctx, canonical.light_index, triangle, point), point, canonical.light_index,
                history, swl, !same_shadow_receiver(receiver, history.surface));
        };
        const Float neighbor_confidence = pair_count * neighbor.count;
        const Float neighbor_mis = pair_balance(neighbor_confidence * neighbor.target,
                                                canonical.count * target_neighbor_at_current) / pair_count;
        canonical_mis += pair_balance(canonical.count * canonical.target,
                                      neighbor_confidence * target_current_at_neighbor) / pair_count;

        const Float weight = select(target_neighbor_at_current > 0.f,
                                    target_neighbor_at_current * neighbor.weight * neighbor_mis, 0.f);
        stream_reservoir(output, neighbor.light_index, neighbor.packed_uv, target_neighbor_at_current, weight, neighbor.count, u);
    };
}

[[nodiscard]] inline ReservoirVar reuse_pairwise(const Context &ctx, const ReservoirVar &current,
                                              const Surface &receiver, const HistoryInput &temporal, const HistoryInput &spatial,
                                              const ReceiverTargets &targets, const SampledWavelengths &swl,
                                              TSampler &sampler) {
    const Float pair_count = cast<float>(temporal.reservoir.count > 0.f) + cast<float>(spatial.reservoir.count > 0.f);
    ReservoirVar output;
    $if(pair_count == 0.f) {
        output = current;
    } $else {
        Float canonical_mis = 0.f;
        const Float2 canonical_uv = current->uv();
        const Float temporal_random = sampler->next_1d();
        stream_pair(ctx, output, canonical_mis, current, receiver, canonical_uv, temporal, targets.temporal, pair_count, temporal_random, swl);
        const Float spatial_random = sampler->next_1d();
        stream_pair(ctx, output, canonical_mis, current, receiver, canonical_uv, spatial, targets.spatial, pair_count, spatial_random, swl);
        const Float current_random = sampler->next_1d();
        const Float weight = select(valid_reservoir(current), current.target * current.weight * canonical_mis, 0.f);
        stream_reservoir(output, current.light_index, current.packed_uv, current.target, weight, current.count, current_random);
        // The balance shares already partition unity across the pairs.
        finalize_reservoir(output, 1.f, 1.f);
    };
    return output;
}

[[nodiscard]] inline ReservoirVar reuse_direct(const Context &ctx, const Uint2 &pixel, const Surface &receiver,
                                            const ReservoirVar &current, const Uint &history_slice,
                                            const SampledWavelengths &swl, uint bias_mode, TSampler &sampler,
                                            bool use_temporal, bool use_spatial, const Bool &same_geometry) {
    ReservoirVar output = current;
    if (!use_temporal && !use_spatial) return output;
    $if(ctx.has_history) {
        sampler->set_seed(pixel, ctx.frame, Dimension::FalcorDI_reuse);
        const Float2 motion_vector = motion(pixel_index(pixel, ctx.extent));
        const Float2 jitter = sampler->next_2d();
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
                offset = cast<int2>(4.f * (sampler->next_2d() - 0.5f));
            };
            const Int2 previous_pixel = reprojected + offset;
            const Surface previous = load_surface(ctx, previous_pixel, true);
            $if(reuse_similar(receiver, previous)) {
                if (use_temporal) {
                    temporal.surface = previous;
                    temporal.pixel_index = pixel_index(make_uint2(previous_pixel), ctx.extent);
                    temporal.reservoir = read_history(ctx, previous_pixel, history_slice, history_limit);
                }
                temporal_pixel = previous_pixel;
                $break;
            };
        };

        // One spatial neighbor from the previous frame keeps reads and writes in separate slices.
        if (use_spatial) {
            const Uint neighbor_index = cast<uint>(sampler->next_1d() * float(neighbor_count));
            const Int2 spatial_pixel = reprojected + cast<int2>(30.f * ctx.neighbors.read(neighbor_index, false));
            $if(any(spatial_pixel != temporal_pixel)) {
                const Surface neighbor = load_surface(ctx, spatial_pixel, true);
                $if(reuse_similar(receiver, neighbor)) {
                    spatial.surface = neighbor;
                    spatial.pixel_index = pixel_index(make_uint2(spatial_pixel), ctx.extent);
                    spatial.reservoir = read_history(ctx, spatial_pixel, history_slice, history_limit);
                };
            };
        }

        ReceiverTargets targets;
        const auto temporal_sample = sample_history(ctx, temporal, receiver);
        const auto spatial_sample = sample_history(ctx, spatial, receiver);
        $if(temporal_sample.pdf > 0.f || spatial_sample.pdf > 0.f) {
            auto &fb = Toolkit::frame_buffer();
            const Uint index = pixel_index(pixel, ctx.extent);
            const auto hit = fb.cur_visibility_buffer_var(ctx.frame).read(index);
            auto ray = fb.rays().read(index)->to_ray_state().ray;
            auto it = Toolkit::geometry().compute_surface_interaction(hit, ray);
            Toolkit::scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                const auto bsdf = material->create_evaluator(it, swl);
                targets.temporal = target_at(temporal_sample, bsdf, it.wo);
                targets.spatial = target_at(spatial_sample, bsdf, it.wo);
            });
        };

        if (bias_mode == 2u) {
            // Pairwise cached self targets already exclude invisible samples. Cross targets must use the same support.
            const auto test_visibility = [&](const HistoryInput &history, const TriangleSample &sample, Float &target) {
                const Bool current_sample = valid_reservoir(current) && current.light_index == history.reservoir.light_index &&
                    current.packed_uv == history.reservoir.packed_uv;
                // A valid Pairwise winner was visible at its own receiver when stored.
                const Bool unchanged_ray = same_geometry && same_shadow_receiver(receiver, history.surface);
                $if(target > 0.f && !(current_sample || unchanged_ray)) {
                    target = select(visible(receiver, sample), target, 0.f);
                };
            };
            test_visibility(temporal, temporal_sample, targets.temporal);
            test_visibility(spatial, spatial_sample, targets.spatial);
            output = reuse_pairwise(ctx, current, receiver, temporal, spatial, targets, swl, sampler);
        } else {
            output = reuse_talbot(ctx, current, temporal, spatial, targets, swl, bias_mode == 3u, sampler);
        }
    };
    return output;
}

} // namespace vision::falcor_di
