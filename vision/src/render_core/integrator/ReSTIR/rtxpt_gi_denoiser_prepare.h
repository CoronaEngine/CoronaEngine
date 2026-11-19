#pragma once

#include "rtxpt_gi_prefix_storage.h"

namespace vision::rtxpt_gi {
// RTXPT PostProcess.hlsl ComputeDisocclusionRelaxation. Only the register Guide
// is changed. Prefix/header records remain immutable for all neighbor readers.
template<typename Planes, typename Headers>
inline void condition_native_reblur_guide(const Planes &planes, const Headers &headers,
                                          reblur::GuideVar &guide, const Float3 &terminal_direction,
                                          const Uint &vertex_depth, const Uint &branch,
                                          const Uint2 &pixel, const Uint2 &resolution,
                                          const Uint &plane) noexcept {
    $if((guide.identity.w & 1u) != 0u && guide.position.w > 0.f && branch != PrefixInvalid) {
        Float mix = 0.f;
        $if(vertex_depth > 1u) {
            Float3 direction = normalize(terminal_direction);
            Float divergence = 0.f;
            for (const int2 offset : {make_int2(-1, 0), make_int2(1, 0),
                                     make_int2(0, -1), make_int2(0, 1)}) {
                Int2 neighbor_pixel = clamp(make_int2(pixel) + offset, make_int2(0), make_int2(resolution) - 1);
                Uint neighbor_index = cast<uint>(neighbor_pixel.y) * resolution.x + cast<uint>(neighbor_pixel.x);
                $if(headers.read(neighbor_index)[plane] == PrefixInvalid) {
                    divergence += .02f;
                } $else {
                    auto neighbor = planes.read(plane * resolution.x * resolution.y + neighbor_index);
                    divergence += 1.f - dot(direction, normalize(neighbor.direction_vertex.xyz()));
                };
            }
            mix = saturate((divergence - .00002f) * 25.f);
        };
        Uint encoded = cast<uint>(mix * 255.f + .5f);
        guide.identity.w = (guide.identity.w & ~0xff00u) | (encoded << 8u);
    };
}

// Test this frame's header before reading any possibly stale empty-plane bytes.
// Build encoded guide eligibility from hit && (!consumed || plane == 0), which
// deliberately differs from GI receiver eligibility. Miss neighbors above still
// supply their terminal direction exactly as in the original conditioning pass.
template<typename Planes, typename Headers, typename MotionY, typename MaterialId>
[[nodiscard]] inline reblur::GuideVar load_pt_reblur_guide(const Planes &planes, const Headers &headers, const MotionY &motion_y,
                                                          const Uint2 &pixel, const Uint2 &resolution,
                                                          const Uint &plane, MaterialId &&material_id) noexcept {
    reblur::GuideVar guide;
    guide.position = make_float4(0.f); guide.identity = make_uint4(0u);
    guide.normal_roughness = make_float4(0.f, 0.f, 1.f, 1.f);
    guide.diffuse_factor = make_float4(1.f, 1.f, 1.f, 0.f);
    guide.specular_factor = guide.diffuse_factor;
    Uint index = pixel.y * resolution.x + pixel.x;
    Uint branch = headers.read(index)[plane];
    $if(branch != PrefixInvalid && branch != PrefixQueued) {
        auto terminal = planes.read(plane * resolution.x * resolution.y + index);
        $if(pt_prefix_guide_valid(terminal)) {
            guide = pt_prefix_decode_guide(terminal, branch, material_id(Uint{terminal.hit_words.x}),
                motion_y.read(plane * resolution.x * resolution.y + index));
            condition_native_reblur_guide(planes, headers, guide, Float3{terminal.direction_vertex.xyz()},
                pt_prefix_depth(terminal), branch, pixel, resolution, plane);
        };
    };
    return guide;
}
}// namespace vision::rtxpt_gi
