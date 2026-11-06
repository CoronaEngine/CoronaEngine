/***************************************************************************
 # Copyright (c) 2015-24, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/

// Adapted from Falcor 9 RTXDIApplicationBridge.slangh and SurfaceData.slang
// at 759aad033ff610fb0d82c74f7e0a508d0096d5f2 for CoronaEngine's DSL.

#pragma once

#include "base/mgr/geometry.h"
#include "base/mgr/renderer.h"
#include "base/mgr/scene.h"
#include "base/sensor/frame_buffer.h"
#include "math/warp.h"

namespace vision::falcor_di {

struct Reservoir {
    uint light_index{InvalidUI32};
    uint packed_uv{};
    float count{};
    float target{};
    float weight{};
};

static_assert(sizeof(Reservoir) == 20u);

struct LightTriangleGeometry {
    array_float3 p0, p1, p2, ng, emission;
    float pdf_pos;
};

static_assert(sizeof(LightTriangleGeometry) == 64u);

} // namespace vision::falcor_di

OC_STRUCT(vision::falcor_di, Reservoir, light_index, packed_uv, count, target, weight) {
    [[nodiscard]] Float2 uv() const {
        return cast<float2>(make_uint2(packed_uv & 65535u, packed_uv >> 16u)) / 65535.f;
    }

    void set_uv(const Float2 &uv) {
        const Uint2 bits = cast<uint2>(saturate(uv) * 65535.f + 0.5f);
        packed_uv = bits.x | (bits.y << 16u);
    }
};

OC_STRUCT(vision::falcor_di, LightTriangleGeometry, p0, p1, p2, ng, emission, pdf_pos) {};

namespace vision::falcor_di {

inline constexpr float ray_epsilon = 0.001f;
inline constexpr uint tile_size = 1024u;
inline constexpr uint tile_count = 128u;
inline constexpr uint neighbor_count = 8192u;

struct Context {
    uint2 extent;
    Uint frame;
    Bool has_history;
    uint light_count;
    const Buffer<uint4> &surfaces0;
    const Buffer<uint2> &surface_normals;
    Buffer<Reservoir> &reservoirs;
    const Buffer<float2> &neighbors;
    const Buffer<uint4> &light_primitives;
    const Buffer<LightTriangleGeometry> &light_geometry;
    const Buffer<float> &probabilities;
    const Buffer<uint> &light_bases;
    Buffer<uint2> &ris;
};

struct Surface {
    Float3 position{make_float3(0.f)};
    Float3 normal{make_float3(0.f)};
    Float3 geometric_normal{make_float3(0.f)};
    Float depth{-1.f};
};

struct TriangleSample {
    Float3 direction{make_float3(0.f)};
    Float distance{0.f};
    Float3 radiance{make_float3(0.f)};
    Float pdf{0.f};
};

[[nodiscard]] inline Bool finite(const Float &value) {
    return !ocarina::isnan(value) && !ocarina::isinf(value);
}

[[nodiscard]] inline Uint pixel_index(const Uint2 &pixel, const uint2 &extent) {
    return pixel.x + pixel.y * extent.x;
}

[[nodiscard]] inline Float3 unpack_normal(const Uint &packed) {
    const Int2 signed_oct = make_int2(as<int>(packed << 16u) >> 16u, as<int>(packed) >> 16u);
    const Float2 oct = max(make_float2(signed_oct) * (1.f / 32767.f), make_float2(-1.f));
    const Float z = 1.f - abs(oct.x) - abs(oct.y);
    const Float2 signs = make_float2(select(oct.x >= 0.f, 1.f, -1.f), select(oct.y >= 0.f, 1.f, -1.f));
    const Float2 xy = select(z < 0.f, (1.f - abs(oct.yx())) * signs, oct);
    return normalize(make_float3(xy, z));
}

[[nodiscard]] inline Bool valid_surface(const Surface &surface) {
    return surface.depth >= 0.f;
}

[[nodiscard]] inline Surface load_surface(const Context &ctx, const Int2 &pixel, bool previous) {
    Surface surface;
    const Bool in_bounds = all(pixel >= make_int2(0)) && all(pixel < make_int2(ctx.extent));
    $if(in_bounds && (previous ? ctx.has_history : Bool{true})) {
        const Uint slot = (ctx.frame & 1u) ^ uint(previous);
        const Uint index = slot * (ctx.extent.x * ctx.extent.y) + pixel_index(make_uint2(pixel), ctx.extent);
        const Float4 position_depth = as<float4>(ctx.surfaces0.read(index, false));
        $if(position_depth.w >= 0.f) {
            surface.position = position_depth.xyz();
            const Uint2 normals = ctx.surface_normals.read(index, false);
            surface.normal = unpack_normal(normals.x);
            surface.geometric_normal = unpack_normal(normals.y);
            surface.depth = position_depth.w;
        };
    };
    return surface;
}

[[nodiscard]] inline Float2 motion(const Uint &index) {
    return -Toolkit::frame_buffer().motion_vectors().read(index);
}

[[nodiscard]] inline LightEvalContext load_light_point(const Context &ctx, const Uint &index,
                                                       const LightTriangleGeometryVar &triangle, const Float2 &u) {
    const Float2 uv = select(u.x + u.y <= 1.f, u, 1.f - u);
    // The reservoir stores weights for vertices 1 and 2; geometry takes 0 and 1.
    const Float2 bary = make_float2(1.f - uv.x - uv.y, uv.x);
    LightEvalContext point;
    $if(triangle.pdf_pos > 0.f) {
        point.pos = triangle_lerp(bary, triangle.p0.as_vec3(), triangle.p1.as_vec3(), triangle.p2.as_vec3());
        point.ng = triangle.ng.as_vec3();
        point.PDF_pos = triangle.pdf_pos;
    } $else {
        const Uint4 primitive = ctx.light_primitives.read(index, false);
        point = Toolkit::geometry().compute_light_eval_context(primitive.x, primitive.y, bary);
    };
    return point;
}

[[nodiscard]] inline TriangleSample evaluate_light_point(const Float3 &emission, const LightEvalContext &point,
                                                        const Surface &surface) {
    TriangleSample sample;
    const Float3 to_light = point.pos - surface.position;
    $if(dot(to_light, point.ng) < 0.f) {
        // RIS accounts for selecting the triangle. Use the framework's conditional area-light PDF.
        const Float pdf = vision::PDF_wi(point.PDF_pos, point.ng, surface.position - point.pos);
        $if(pdf > 0.f && finite(pdf)) {
            sample.distance = length(to_light);
            sample.direction = to_light / sample.distance;
            sample.radiance = emission / pdf;
            sample.pdf = pdf;
        };
    };
    return sample;
}

[[nodiscard]] inline TriangleSample sample_light(const Context &ctx, const Uint &index, const Surface &surface,
                                                const Float2 &uv) {
    const LightTriangleGeometryVar triangle = ctx.light_geometry.read(index, false);
    return evaluate_light_point(triangle.emission.as_vec3(), load_light_point(ctx, index, triangle, uv), surface);
}

[[nodiscard]] inline RayVar make_light_ray(const Float3 &origin, const Float3 &direction,
                                          const Float &t_min, const Float &t_max) {
    RayVar ray;
    ray.org_min = make_float4(origin, t_min);
    ray.dir_max = make_float4(direction, t_max);
    return ray;
}

[[nodiscard]] inline Bool visible(const Surface &surface, const TriangleSample &sample) {
    Bool unoccluded = false;
    $if(sample.distance > ray_epsilon) {
        const Float3 origin = Interaction::custom_offset_ray_origin(surface.position, surface.geometric_normal,
                                                                    sample.direction);
        const Float3 segment = sample.direction * sample.distance + (surface.position - origin);
        const Float distance = length(segment);
        $if(distance > ray_epsilon) {
            unoccluded = !Toolkit::geometry().trace_occlusion(
                make_light_ray(origin, segment / distance, Float{0.f}, distance - ray_epsilon));
        };
    };
    return unoccluded;
}

[[nodiscard]] inline Uint4 trace_light(const Context &ctx, const Float3 &origin, const Float3 &direction,
                                       const Float &t_min, const Float &t_max) {
    const auto hit = Toolkit::geometry().trace_closest(make_light_ray(origin, direction, t_min, t_max));
    Uint light_index = InvalidUI32;
    $if(hit->is_hit()) {
        const Uint base = ctx.light_bases.read(hit.inst_id);
        light_index = select(base == InvalidUI32, base, base + hit.prim_id);
    };
    return make_uint4(light_index, hit.prim_id, as<uint>(hit.bary.y), as<uint>(1.f - hit.bary.x - hit.bary.y));
}

} // namespace vision::falcor_di
