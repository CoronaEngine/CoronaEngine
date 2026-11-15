#pragma once

#include "rtxpt_gi_prefix_storage.h"

namespace vision::rtxpt_gi {
// RTXPT PostProcess.hlsl ComputeDisocclusionRelaxation. The caller dispatches
// after Build/Fill/Final and bounds-checks pixel. Neighbor reads use immutable
// prefix records; each invocation owns its three current-pixel guide writes.
inline void prepare_native_reblur_guides(const Buffer<RtxptGIPrefixPlane> &planes,
                                         const Buffer<uint4> &headers,
                                         Buffer<reblur::Guide> &guides,
                                         const Uint2 &pixel, const Uint2 &resolution) noexcept {
    Uint pixels = resolution.x * resolution.y;
    Uint index = pixel.y * resolution.x + pixel.x;
    Uint4 header = headers.read(index);
    for (uint plane = 0u; plane < 3u; ++plane) {
        Uint address = plane * pixels + index;
        reblur::GuideVar guide = guides.read(address);
        $if((guide.identity.w & 1u) != 0u && guide.position.w > 0.f && header[plane] != PrefixInvalid) {
            Float mix = 0.f;
            auto terminal = planes.read(address);
            $if(terminal.direction_vertex.w > 1.f) {
                Float3 direction = normalize(terminal.direction_vertex.xyz());
                Float divergence = 0.f;
                for (const int2 offset : {make_int2(-1, 0), make_int2(1, 0),
                                         make_int2(0, -1), make_int2(0, 1)}) {
                    Int2 neighbor_pixel = clamp(make_int2(pixel) + offset, make_int2(0), make_int2(resolution) - 1);
                    Uint neighbor_index = cast<uint>(neighbor_pixel.y) * resolution.x + cast<uint>(neighbor_pixel.x);
                    $if(headers.read(neighbor_index)[plane] == PrefixInvalid) {
                        divergence += .02f;
                    } $else {
                        auto neighbor = planes.read(plane * pixels + neighbor_index);
                        divergence += 1.f - dot(direction, normalize(neighbor.direction_vertex.xyz()));
                    };
                }
                mix = saturate((divergence - .00002f) * 25.f);
            };
            Uint encoded = cast<uint>(mix * 255.f + .5f);
            guide.identity.w = (guide.identity.w & ~0xff00u) | (encoded << 8u);
            guides.write(address, guide);
        };
    }
}
}// namespace vision::rtxpt_gi
