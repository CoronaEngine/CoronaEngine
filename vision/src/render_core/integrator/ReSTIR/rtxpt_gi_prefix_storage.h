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
    uint4 interiors_branch{};
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
