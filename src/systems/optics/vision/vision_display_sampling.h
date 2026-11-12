#pragma once

#include "base/scattering/half_storage.h"

namespace Corona::Systems::Vision::display_detail {

inline ocarina::Float3 load_hdr(const ocarina::BufferVar<ocarina::float4>& source,
                                const ocarina::Uint2& size, const ocarina::Uint2& pixel) {
    return source.read(pixel.y * size.x + pixel.x, false).xyz();
}

inline ocarina::Float3 load_bloom(const ocarina::BufferVar<ocarina::uint2>& source,
                                  const ocarina::Uint2& size, const ocarina::Uint2& pixel) {
    return ::vision::unpack_storage_half4(source.read(pixel.y * size.x + pixel.x, false)).xyz();
}

inline ocarina::Float3 sample_bilinear(const ocarina::Float2& pos, const ocarina::Uint2& size,
                                       const auto& load) {
    using namespace ocarina;
    const Uint2 lo = cast<uint2>(floor(pos));
    const Uint2 hi = min(lo + 1u, size - 1u);
    const Float2 t = fract(pos);
    const Float3 top = lerp(t.x, load(lo), load(make_uint2(hi.x, lo.y)));
    const Float3 bottom = lerp(t.x, load(make_uint2(lo.x, hi.y)), load(hi));
    return lerp(t.y, top, bottom);
}

inline ocarina::Float3 composite_bloom(const ocarina::Float3& hdr, const ocarina::Uint2& pixel,
                                       const ocarina::Uint2& size,
                                       const ocarina::BufferVar<ocarina::uint2>& bloom,
                                       const ocarina::Uint2& bloom_size, const ocarina::Float& intensity) {
    using namespace ocarina;
    const Float2 uv = (make_float2(pixel) + .5f) / make_float2(size);
    const Float2 extent = make_float2(bloom_size);
    const Float2 pos = clamp(uv * extent - .5f, make_float2(0.f), extent - 1.f);
    const Float3 color = sample_bilinear(pos, bloom_size, [&](const Uint2& tap) {
        return load_bloom(bloom, bloom_size, tap);
    });
    const Float3 blended = lerp(intensity, hdr, color);
    // RTXPT stores the blend as RGBA16F before exposure and tone mapping.
    return ::vision::unpack_storage_half4(::vision::pack_storage_half4(make_float4(blended, 1.f))).xyz();
}

inline ocarina::Float3 blur_bloom(const ocarina::BufferVar<ocarina::uint2>& source,
                                  const ocarina::Uint2& size, const ocarina::Uint2& pixel,
                                  const ocarina::Bool& horizontal) {
    using namespace ocarina;
    // RTXPT's radius of eight full-resolution pixels gives sigma two at quarter size.
    // The paired bilinear taps expand to these weights at integer offsets.
    constexpr float weights[] = {
        .1994711402f, .1760326634f, .1209853623f, .0647587978f, .0269954833f,
        .0087641502f, .0022159242f, .0004363413f, .0000669151f,
    };
    const Int x_step = select(horizontal, Int(1), Int(0));
    const Int2 direction = make_int2(x_step, 1 - x_step);
    const Int2 center = make_int2(pixel);
    const Int2 last = make_int2(size) - 1;
    Float3 sum = load_bloom(source, size, pixel) * weights[0];
    for (int tap = 1; tap <= 8; ++tap) {
        const Int2 offset = direction * tap;
        const Uint2 before = cast<uint2>(max(center - offset, make_int2(0)));
        const Uint2 after = cast<uint2>(min(center + offset, last));
        sum += (load_bloom(source, size, before) + load_bloom(source, size, after)) * weights[tap];
    }
    return sum;
}

inline auto make_vision_bloom_kernel() {
    using namespace ocarina;
    return ocarina::Kernel([](BufferVar<float4> hdr, BufferVar<uint2> source, BufferVar<uint2> destination,
                              Uint2 input_size, Uint2 output_size, Uint operation) {
        configure_block(8u, 8u, 1u);
        const Uint2 pixel = dispatch_idx().xy();
        $if(all(pixel < output_size)) {
            Float3 color;
            $if(operation < 2u) {
                const Float2 pos =
                    (make_float2(pixel) + .5f) * make_float2(input_size) / make_float2(output_size) - .5f;
                $if(operation == 0u) {
                    color = sample_bilinear(pos, input_size, [&](const Uint2& tap) {
                        return load_hdr(hdr, input_size, tap);
                    });
                }
                $else {
                    color = sample_bilinear(pos, input_size, [&](const Uint2& tap) {
                        return load_bloom(source, input_size, tap);
                    });
                };
            }
            $else {
                color = blur_bloom(source, input_size, pixel, operation == 2u);
            };
            destination.write(pixel.y * output_size.x + pixel.x,
                              ::vision::pack_storage_half4(make_float4(color, 1.f)), false);
        };
    });
}

} // namespace Corona::Systems::Vision::display_detail
