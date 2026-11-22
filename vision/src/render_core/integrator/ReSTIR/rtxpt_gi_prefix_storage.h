#pragma once

#include "base/using.h"
#include "rtxpt_gi_denoiser.h"

namespace vision {
struct RtxptGICameraParam {
    array<float4, 8> columns{}; // current world-to-camera, camera-to-raster
};

// A queued node overlays hit/virtual/flags with its native image transform.
struct RtxptGIPrefixPlane {
    uint4 hit_words{};
    float4 origin_distance{}, direction_vertex{}, throughput_flags{};
    uint4 interiors_branch{}; // PT path: etaScale bits, incoming ideal-path flag, branch, flags
    float4 virtual_position{};
};
struct alignas(16) RtxptGIPrefixAux {
    float4 prefix_throughput_depth{};
    float4 current_virtual_position{};
    float4 previous_virtual_position{};
    uint4 metadata{}; // planeHash, dominant plane, vertex depth, stable branch
    uint2 packed_motion{}; // binary16 XY pixel motion, Z depth motion, W zero
};
static_assert(sizeof(RtxptGIPrefixPlane) == 96u);
static_assert(sizeof(RtxptGIPrefixAux) == 80u);
}
OC_PARAM_STRUCT(vision, RtxptGICameraParam, columns) {};
OC_STRUCT(vision, RtxptGIPrefixPlane, hit_words, origin_distance, direction_vertex,
          throughput_flags, interiors_branch, virtual_position) {};
OC_STRUCT(vision, RtxptGIPrefixAux, prefix_throughput_depth, current_virtual_position,
          previous_virtual_position, metadata, packed_motion) {};

namespace vision::rtxpt_gi {
constexpr uint PrefixInvalid = 0xffffffffu, PrefixQueued = 0xfffffffeu;
constexpr uint PrefixDominantBit = 1u << 8u;

// PT Build retains the queue representation above until a plane terminates.
// Terminal records reuse dead queue fields for exact FP32 guide data.
constexpr uint PtPrefixDepthMask = 0x3cu;
constexpr uint PtPrefixConsumedBit = 1u << 6u;
constexpr uint PtPrefixIdealPathBit = 1u << 7u;
constexpr uint PtPrefixControlMask = 0xfcu;
constexpr uint PtPrefixGuideMask = 0xffff0003u;

[[nodiscard]] inline Uint pt_prefix_depth(const RtxptGIPrefixPlaneVar &node) noexcept {
    return (node.interiors_branch.w & PtPrefixDepthMask) >> 2u;
}
[[nodiscard]] inline Bool pt_prefix_consumed(const RtxptGIPrefixPlaneVar &node) noexcept {
    return (node.interiors_branch.w & PtPrefixConsumedBit) != 0u;
}
[[nodiscard]] inline Bool pt_prefix_ideal_path(const RtxptGIPrefixPlaneVar &node) noexcept {
    return (node.interiors_branch.w & PtPrefixIdealPathBit) != 0u;
}
[[nodiscard]] inline Bool pt_prefix_guide_valid(const RtxptGIPrefixPlaneVar &node) noexcept {
    return (node.interiors_branch.w & 1u) != 0u;
}
inline void pt_prefix_store_controls(RtxptGIPrefixPlaneVar &node, const Uint &depth,
                                     const Bool &consumed, const Bool &ideal_path) noexcept {
    // Build terminates at at most five vertices; the depth remains an exact
    // integer. Always encode transport controls, including without denoising.
    node.interiors_branch.w = (depth << 2u) |
        ocarina::select(consumed, PtPrefixConsumedBit, 0u) |
        ocarina::select(ideal_path, PtPrefixIdealPathBit, 0u);
}
inline void pt_prefix_store_guide(RtxptGIPrefixPlaneVar &node, const reblur::GuideVar &guide) noexcept {
    node.origin_distance = make_float4(guide.normal_roughness.xyz(), node.origin_distance.w);
    node.direction_vertex = make_float4(node.direction_vertex.xyz(), guide.normal_roughness.w);
    node.throughput_flags = make_float4(guide.diffuse_factor.xyz(), guide.diffuse_factor.w);
    node.interiors_branch.x = ocarina::as<uint>(Float{guide.specular_factor.y});
    node.interiors_branch.y = ocarina::as<uint>(Float{guide.specular_factor.z});
    // Preserve the actual FP32 Guide motion. The producer's nominal half
    // roundtrip can be optimized away by GPU code generation; Y lives in its
    // separate FP32 buffer rather than forcing a new quantization boundary.
    node.interiors_branch.z = ocarina::as<uint>(Float{guide.specular_factor.x});
    node.interiors_branch.w = (node.interiors_branch.w & PtPrefixControlMask) |
        (guide.identity.w & PtPrefixGuideMask);
}
[[nodiscard]] inline reblur::GuideVar pt_prefix_decode_guide(const RtxptGIPrefixPlaneVar &node,
                                                            const Uint &branch, const Uint &material,
                                                            const Float &motion_y) noexcept {
    reblur::GuideVar guide;
    guide.position = make_float4(0.f);
    guide.identity = make_uint4(0u);
    guide.normal_roughness = make_float4(0.f, 0.f, 1.f, 1.f);
    guide.diffuse_factor = make_float4(1.f, 1.f, 1.f, 0.f);
    guide.specular_factor = guide.diffuse_factor;
    $if(pt_prefix_guide_valid(node)) {
        guide.position = node.virtual_position;
        guide.identity = make_uint4(node.hit_words.x, material, branch,
                                   node.interiors_branch.w & PtPrefixGuideMask);
        guide.normal_roughness = make_float4(node.origin_distance.xyz(), node.direction_vertex.w);
        guide.diffuse_factor = node.throughput_flags;
        guide.specular_factor = make_float4(ocarina::as<float>(Uint{node.interiors_branch.z}),
            ocarina::as<float>(Uint{node.interiors_branch.x}),
            ocarina::as<float>(Uint{node.interiors_branch.y}),
            motion_y);
    };
    return guide;
}

struct PrefixStorage {
    mutable ocarina::BindlessArrayBuffer<RtxptGIPrefixPlane> planes;
    mutable ocarina::BindlessArrayBuffer<uint4> headers;
    mutable ocarina::BindlessArrayBuffer<float4> stable_radiance;
    Uint pixel_count;
    NativeReblurStorage denoiser;
    [[nodiscard]] RtxptGIPrefixPlaneVar read(const Uint &plane) const noexcept {
        return planes.read(plane * pixel_count + dispatch_id());
    }
    void write(const Uint &plane, const RtxptGIPrefixPlaneVar &value) const noexcept {
        planes.write(plane * pixel_count + dispatch_id(), value);
    }
};
}
