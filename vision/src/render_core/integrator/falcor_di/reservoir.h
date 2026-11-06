#pragma once

#include "common.h"

namespace vision::falcor_di {

struct Reservoir {
    Uint light_index{InvalidUI32};
    Float2 uv{make_float2(0.f)};
    Float weight{0.f};
    Float target{0.f};
    Float count{0.f};
};

[[nodiscard]] inline Bool valid_reservoir(const Reservoir &reservoir) {
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

[[nodiscard]] inline Reservoir load_reservoir(const Context &ctx, const Uint2 &pixel, const Uint &slice) {
    Reservoir reservoir;
    $if(!any(pixel >= ctx.extent) && slice < 3u) {
        const Uint index = reservoir_index(ctx.extent, pixel, slice);
        const PackedReservoirVar packed = ctx.reservoirs.read(index, false);
        reservoir.light_index = packed.light_index;
        reservoir.uv = cast<float2>(make_uint2(packed.packed_uv & 65535u, packed.packed_uv >> 16u)) / 65535.f;
        reservoir.count = as<float>(packed.count_bits);
        reservoir.target = packed.target;
        reservoir.weight = packed.weight;
    };
    return reservoir;
}

inline void store_reservoir(const Context &ctx, const Uint2 &pixel, const Uint &slice,
                            const Reservoir &reservoir) {
    const Uint2 uv = cast<uint2>(saturate(reservoir.uv) * 65535.f + 0.5f);
    const Uint packed_uv = uv.x | (uv.y << 16u);
    const Uint index = reservoir_index(ctx.extent, pixel, slice);
    PackedReservoirVar packed;
    packed.light_index = reservoir.light_index;
    packed.packed_uv = packed_uv;
    packed.count_bits = as<uint>(reservoir.count);
    packed.target = reservoir.target;
    packed.weight = reservoir.weight;
    ctx.reservoirs.write(index, packed, false);
}

// Weighted reservoir sampling and RIS normalization, Bitterli et al. (2020).
inline Bool stream_reservoir(Reservoir &reservoir, const Uint &light_index, const Float2 &uv,
                             const Float &target, const Float &candidate_weight, const Float &count,
                             const Float &u) {
    reservoir.count += count;
    Bool selected = false;
    $if(candidate_weight > 0.f && finite(candidate_weight) && target > 0.f && finite(target) &&
        light_index != InvalidUI32) {
        reservoir.weight += candidate_weight;
        selected = !(u * reservoir.weight >= candidate_weight);
        $if(selected) {
            reservoir.light_index = light_index;
            reservoir.uv = uv;
            reservoir.target = target;
        };
    };
    return selected;
}

inline void finalize_reservoir(Reservoir &reservoir, const Float &numerator, const Float &denominator) {
    const Float divisor = reservoir.target * denominator;
    Float weight = 0.f;
    $if(divisor > 0.f) {
        weight = reservoir.weight * numerator / divisor;
    };
    reservoir.weight = select(finite(weight) && weight > 0.f, weight, 0.f);
}

} // namespace vision::falcor_di
