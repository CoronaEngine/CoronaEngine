#pragma once

#include "base/using.h"
#include "dsl/data/shared_array.h"

/***************************************************************************
 # Copyright (c) 2023-2024, NVIDIA CORPORATION. All rights reserved.
 # NVIDIA CORPORATION and its licensors retain all intellectual property
 # and proprietary rights in and to this software, related documentation
 # and any modifications thereto. Any use, reproduction, disclosure or
 # distribution of this software and related documentation without an express
 # license agreement from NVIDIA CORPORATION is strictly prohibited.
 **************************************************************************/

namespace vision::rtxpt_gi {

// Matched native RTXDI 8x8 two-level wave reduction, expressed in typed DSL.
// All 64 lanes reach both barriers; invalid padded lanes supply zero weight.
[[nodiscard]] inline Bool native_boiling_filter(const Uint2 &local_index,
                                                const Float &strength,
                                                const Float &weight) noexcept {
    SharedArray<float, 2> weights;
    SharedArray<uint, 2> counts;
    Float multiplier = 10.f / clamp(strength, 1e-6f, 1.f) - 9.f;
    Float wave_weight = subgroup_active_sum(weight);
    Uint wave_count = subgroup_active_count_bits(weight > 0.f);
    Uint linear = local_index.x + local_index.y * 8u;
    Uint lane_count = subgroup_lane_count();
    Uint wave_index = linear / lane_count;
    $if(subgroup_is_first_lane()) {
        weights.write(wave_index, wave_weight);
        counts.write(wave_index, wave_count);
    };
    synchronize_block(true);
    $if(linear < (64u + lane_count - 1u) / lane_count) {
        wave_weight = weights.read(linear);
        wave_count = counts.read(linear);
        wave_weight = subgroup_active_sum(wave_weight);
        wave_count = subgroup_active_sum(wave_count);
        $if(linear == 0u) {
            weights.write(0u, select(wave_count > 0u, wave_weight / cast<float>(wave_count), 0.f));
        };
    };
    synchronize_block(true);
    Float average = weights.read(0u);
    return weight > average * multiplier;
}
}// namespace vision::rtxpt_gi
