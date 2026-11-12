#pragma once

#include "vision_display_sampling.h"

#include <dsl/dsl.h>

namespace Corona::Systems::Vision {

inline auto make_vision_auto_exposure_kernel() {
    using namespace ocarina;
    ocarina::Kernel kernel = [](BufferVar<float4> hdr, BufferVar<uint2> bloom, BufferVar<float> gain,
                                Uint2 extent, Uint2 bloom_size, Float bloom_intensity,
                                Float minimum_gain, Float maximum_gain) {
        constexpr uint threads = 64u;
        configure_block(threads);

        // Meter on the largest power-of-two grid within the source extent.
        Uint2 meter_size = extent;
        for (uint shift = 1u; shift < 32u; shift *= 2u) {
            meter_size |= meter_size >> make_uint2(shift);
        }
        meter_size -= meter_size >> make_uint2(1u);
        const Uint2 strata = min(meter_size, make_uint2(64u));
        const Uint sample_count = strata.x * strata.y;
        const Uint lane = thread_idx().x;

        auto load = [&](const Uint2& pixel) {
            Float3 color = display_detail::load_hdr(hdr, extent, pixel);
            $if(bloom_intensity > 0.f) {
                color = display_detail::composite_bloom(color, pixel, extent, bloom, bloom_size, bloom_intensity);
            };
            return color;
        };

        // Read the center of each stratum so the meter fits in one workgroup.
        Float sum = 0.f;
        $for(i, lane, sample_count, threads) {
            const Uint2 stratum = make_uint2(i % strata.x, i / strata.x);
            const Uint2 meter_pixel = (stratum * meter_size + meter_size / 2u) / strata;
            const Float2 position = (make_float2(meter_pixel) + 0.5f) * make_float2(extent) /
                                    make_float2(meter_size) - 0.5f;
            const Float3 color = display_detail::sample_bilinear(position, extent, load);
            sum += log2(max(0.0001f, dot(color, make_float3(0.299f, 0.587f, 0.114f))));
        };

        SharedArray<float, threads> log_luminance_sums;
        log_luminance_sums.write(lane, sum);
        synchronize_block();
        for (uint step = threads / 2u; step != 0u; step /= 2u) {
            $if(lane < step) {
                log_luminance_sums.write(lane, log_luminance_sums.read(lane) + log_luminance_sums.read(lane + step));
            };
            synchronize_block();
        }

        $if(lane == 0u) {
            const Float luminance = exp2(log_luminance_sums.read(0u) / cast<float>(sample_count));
            gain.write(0u, clamp(0.042f / luminance, minimum_gain, maximum_gain));
        };
    };
    return kernel;
}

} // namespace Corona::Systems::Vision
