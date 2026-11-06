#pragma once

#include "common.h"

namespace vision::falcor_di {

[[nodiscard]] inline Bool valid_reservoir(const ReservoirVar &reservoir) {
    return reservoir.light_index != InvalidUI32 && reservoir.weight > 0.f && reservoir.target > 0.f;
}

[[nodiscard]] inline Uint reservoir_index(const uint2 &extent, const Uint2 &pixel, const Uint &slice) {
    const uint blocks_per_row = (extent.x + 15u) / 16u;
    const uint block_rows = (extent.y + 15u) / 16u;
    const Uint2 block = pixel / 16u;
    const Uint2 local = pixel % 16u;
    return (slice * blocks_per_row * block_rows + block.y * blocks_per_row + block.x) * 256u
           + local.y * 16u + local.x;
}

[[nodiscard]] inline ReservoirVar load_reservoir(const Context &ctx, const Uint2 &pixel, const Uint &slice) {
    ReservoirVar reservoir;
    $if(!any(pixel >= ctx.extent) && slice < 3u) {
        const Uint index = reservoir_index(ctx.extent, pixel, slice);
        reservoir = ctx.reservoirs.read(index, false);
    };
    return reservoir;
}

inline void store_reservoir(const Context &ctx, const Uint2 &pixel, const Uint &slice,
                            const ReservoirVar &reservoir) {
    const Uint index = reservoir_index(ctx.extent, pixel, slice);
    ctx.reservoirs.write(index, reservoir, false);
}

// Weighted reservoir sampling and RIS normalization, Bitterli et al. (2020).
template<typename SelectUV>
inline Bool stream_candidate(ReservoirVar &reservoir, const Uint &light_index, const Float &target,
                             const Float &candidate_weight, const Float &count, const Float &u,
                             SelectUV &&select_uv) {
    reservoir.count += count;
    Bool selected = false;
    $if(candidate_weight > 0.f && finite(candidate_weight) && target > 0.f && finite(target) &&
        light_index != InvalidUI32) {
        reservoir.weight += candidate_weight;
        selected = !(u * reservoir.weight >= candidate_weight);
        $if(selected) {
            reservoir.light_index = light_index;
            select_uv();
            reservoir.target = target;
        };
    };
    return selected;
}

inline Bool stream_reservoir(ReservoirVar &reservoir, const Uint &light_index, const Uint &packed_uv,
                             const Float &target, const Float &candidate_weight, const Float &count,
                             const Float &u) {
    return stream_candidate(reservoir, light_index, target, candidate_weight, count, u, [&] {
        reservoir.packed_uv = packed_uv;
    });
}

inline void finalize_reservoir(ReservoirVar &reservoir, const Float &numerator, const Float &denominator) {
    const Float divisor = reservoir.target * denominator;
    Float weight = 0.f;
    $if(divisor > 0.f) {
        weight = reservoir.weight * numerator / divisor;
    };
    reservoir.weight = select(finite(weight) && weight > 0.f, weight, 0.f);
}

} // namespace vision::falcor_di
