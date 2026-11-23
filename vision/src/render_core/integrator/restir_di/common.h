#pragma once

#include "base/mgr/geometry.h"
#include "base/mgr/renderer.h"
#include "base/mgr/scene.h"
#include "base/sensor/frame_buffer.h"
#include "base/scattering/material.h"
#include "base/illumination/lightsampler.h"
#include "base/integral/restir_shadow.h"
#include "math/warp.h"
#include <cmath>
#include <stdexcept>

namespace vision::restir_di {

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

} // namespace vision::restir_di

OC_STRUCT(vision::restir_di, Reservoir, light_index, packed_uv, count, target, weight) {
    [[nodiscard]] Float2 uv() const {
        return cast<float2>(make_uint2(packed_uv & 65535u, packed_uv >> 16u)) / 65535.f;
    }

    void set_uv(const Float2 &uv) {
        const Uint2 bits = cast<uint2>(saturate(uv) * 65535.f + 0.5f);
        packed_uv = bits.x | (bits.y << 16u);
    }
};

OC_STRUCT(vision::restir_di, LightTriangleGeometry, p0, p1, p2, ng, emission, pdf_pos) {};

namespace vision::restir_di {

inline constexpr float ray_epsilon = 0.001f;
inline constexpr uint tile_size = 1024u;
inline constexpr uint tile_count = 128u;
inline constexpr uint neighbor_count = 8192u;
inline constexpr float environment_ray_distance = 1e16f;

[[nodiscard]] inline vector<float> light_selection_probabilities(const vector<float> &area_weights, bool has_environment) {
    float power = 0.f;
    for (float weight : area_weights) {
        if (!(weight >= 0.f) || !std::isfinite(weight))
            throw std::runtime_error("ReSTIR DI requires finite nonnegative light power");
        power += weight;
    }
    if (!std::isfinite(power) || (!(power > 0.f) && !has_environment))
        throw std::runtime_error("ReSTIR DI found no finite light power");
    vector<float> result(area_weights.size(), 0.f);
    if (power > 0.f) {
        const float mass = has_environment ? .5f : 1.f;
        for (size_t i = 0; i < result.size(); ++i) result[i] = area_weights[i] / power * mass;
    }
    if (has_environment) result.push_back(power > 0.f ? .5f : 1.f);
    return result;
}

[[nodiscard]] inline Float3 environment_direction(const Float2 &uv) {
    const Float z = 1.f - 2.f * uv.x;
    const Float radius = sqrt(max(0.f, 1.f - z * z));
    const Float phi = 2.f * Pi * uv.y;
    return make_float3(radius * cos(phi), radius * sin(phi), z);
}

[[nodiscard]] inline Float2 environment_uv(const Float3 &direction) {
    const Float3 unit = normalize(direction);
    const Float phi = atan2(unit.y, unit.x);
    return make_float2(.5f * (1.f - clamp(unit.z, -1.f, 1.f)),
        select(phi < 0.f, phi + 2.f * Pi, phi) / (2.f * Pi));
}

[[nodiscard]] inline Float2 canonical_environment_uv(const Float2 &uv) {
    // Evaluate F, Le and both proposal PDFs at the direction actually retained
    // by the reservoir. A small UV16 change can cross a sharp HDR boundary.
    return floor(saturate(uv) * 65535.f + 0.5f) / 65535.f;
}

[[nodiscard]] inline Float2 sample_environment_uv(const Float2 &random) {
    // Delta environments do not define the continuous HDR CDF contract. Keep
    // their pre-existing uniform approximation outside this narrowly scoped fix.
    if (Toolkit::renderer().light_sampler()->env_light()->match(LightType::DeltaDirection)) return random;
    Float2 uv = random;
    Toolkit::renderer().light_sampler()->dispatch_environment([&](const Environment *environment) {
        // Warp only the direction here; resolve Le and its conditional density
        // together once, after canonicalizing the stored world-direction UV.
        const auto sample = environment->sample_only(random);
        LightSampleContext reference;
        reference.pos = make_float3(0.f);
        const auto point = environment->compute_light_eval_context(reference, sample);
        uv = environment_uv(point.pos - reference.pos);
    });
    return canonical_environment_uv(uv);
}

struct Context {
    uint2 extent;
    Uint frame;
    Bool has_history;
    uint light_count;
    uint environment_index;
    const Buffer<uint4> &surfaces0;
    const Buffer<uint2> &surface_normals;
    Buffer<Reservoir> &reservoirs;
    const Buffer<float2> &neighbors;
    const Buffer<uint4> &light_primitives;
    const Buffer<LightTriangleGeometry> &light_geometry;
    const Buffer<float> &probabilities;
    const Buffer<uint> &light_bases;
    Buffer<uint2> &ris;
    const Buffer<uint2> &light_alias;
    Buffer<float4> &ris_points;
    bool precompute_light_points;
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
    Float3 target{make_float3(0.f)};
    Bool infinite{false};
    Bool delta{false};
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
    const auto area_point = [&] {
        $if(triangle.pdf_pos < 0.f) {
            // Directional entries retain an exact world direction, not UV16.
            point.pos = triangle.p0.as_vec3();
            point.PDF_pos = -1.f;
        } $else {
            $if(triangle.pdf_pos > 0.f) {
                point.pos = triangle_lerp(bary, triangle.p0.as_vec3(), triangle.p1.as_vec3(), triangle.p2.as_vec3());
                point.ng = triangle.ng.as_vec3();
                point.PDF_pos = triangle.pdf_pos;
            } $else {
                const Uint4 primitive = ctx.light_primitives.read(index, false);
                point = Toolkit::geometry().compute_light_eval_context(primitive.x, primitive.y, bary);
            };
        };
    };
    if (ctx.environment_index != InvalidUI32) {
        $if(index == ctx.environment_index) {
            // An environment sample is a direction, independent of the receiver.
            point.pos = environment_direction(u);
            // light_emission resolves Le and its conditional solid-angle PDF
            // together. Area points continue to carry their area density.
            point.PDF_pos = 0.f;
        } $else { area_point(); };
    } else area_point();
    return point;
}

[[nodiscard]] inline Float3 light_emission(const Context &ctx, const Uint &index,
                                          const LightTriangleGeometryVar &triangle, LightEvalContext &point) {
    Float3 emission = triangle.emission.as_vec3();
    const auto evaluate_area = [&] {
        $if(triangle.pdf_pos == 0.f) {
            const auto swl = Toolkit::spectrum()->sample_wavelength(Toolkit::renderer().sampler());
            const Uint4 primitive = ctx.light_primitives.read(index, false);
            Toolkit::renderer().light_sampler()->dispatch_light(primitive.z, [&](const Light *light) {
                if (!light->match(LightType::Area)) return;
                LightSampleContext front;
                front.pos = point.pos + point.ng;
                emission = light->Le(front, point, swl).vec3();
            });
        };
    };
    if (ctx.environment_index != InvalidUI32) {
        $if(index == ctx.environment_index) {
            const auto swl = Toolkit::spectrum()->sample_wavelength(Toolkit::renderer().sampler());
            LightSampleContext ref;
            ref.pos = make_float3(0.f);
            ref.ng = point.pos;
            Toolkit::renderer().light_sampler()->dispatch_environment([&](const Environment *environment) {
                const bool delta = environment->match(LightType::DeltaDirection);
                const auto eval = environment->evaluate_wi(ref, point, swl, delta ? LightEvalMode::L : LightEvalMode::All);
                emission = eval.L.vec3();
                // The light selector PMF is accounted for separately. The
                // framework LightSampler's selector must not enter this PDF.
                if (delta) point.PDF_pos = uniform_sphere_PDF();
                else point.PDF_pos = eval.pdf;
            });
        } $else { evaluate_area(); };
    } else evaluate_area();
    return emission;
}

[[nodiscard]] inline TriangleSample evaluate_light_point(const Context &ctx, const Uint &index,
                                                        const Float3 &emission, const LightEvalContext &point,
                                                        const Surface &surface) {
    TriangleSample sample;
    const auto area_sample = [&] {
        $if(point.PDF_pos == -1.f) {
            // Delta direction: unit conditional mass, with no solid-angle PDF
            // or distance attenuation. The selector PMF remains separate.
            sample.direction = point.pos;
            sample.distance = environment_ray_distance;
            sample.infinite = true;
            sample.delta = true;
            sample.pdf = 1.f;
            sample.radiance = emission;
        } $else {
            const Float3 to_light = point.pos - surface.position;
            $if(dot(to_light, point.ng) < 0.f) {
                // RIS accounts for selecting the triangle. Keep its conditional PDF.
                const Float pdf = vision::PDF_wi(point.PDF_pos, point.ng, surface.position - point.pos);
                $if(pdf > 0.f && finite(pdf)) {
                    sample.distance = length(to_light);
                    sample.direction = to_light / sample.distance;
                    sample.target = point.pos;
                    sample.radiance = emission / pdf;
                    sample.pdf = pdf;
                };
            };
        };
    };
    if (ctx.environment_index != InvalidUI32) {
        $if(index == ctx.environment_index) {
            $if(point.PDF_pos > 0.f && finite(point.PDF_pos)) {
                sample.direction = point.pos;
                sample.distance = environment_ray_distance;
                sample.infinite = true;
                sample.pdf = point.PDF_pos;
                sample.radiance = emission / sample.pdf;
            };
        } $else { area_sample(); };
    } else area_sample();
    return sample;
}

[[nodiscard]] inline TriangleSample sample_light(const Context &ctx, const Uint &index, const Surface &surface,
                                                const Float2 &uv) {
    const LightTriangleGeometryVar triangle = ctx.light_geometry.read(index, false);
    auto point = load_light_point(ctx, index, triangle, uv);
    const auto emission = light_emission(ctx, index, triangle, point);
    return evaluate_light_point(ctx, index, emission, point, surface);
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
    $if(sample.distance > 0.f) {
        const Float3 origin = Interaction::custom_offset_ray_origin(surface.position, surface.geometric_normal,
                                                                    sample.direction);
        RayVar ray;
        $if(sample.infinite) { ray = restir_shadow::direction_ray(origin, sample.direction); }
        $else { ray = restir_shadow::finite_ray(origin, sample.target); };
        unoccluded = !Toolkit::geometry().trace_occlusion(ray);
    };
    return unoccluded;
}

[[nodiscard]] inline Uint4 trace_light(const Context &ctx, const Float3 &origin, const Float3 &direction,
                                       const Float &t_min, const Float &t_max) {
    const auto hit = Toolkit::geometry().trace_closest(make_light_ray(origin, direction, t_min, t_max));
    Uint light_index = InvalidUI32;
    Float2 uv = make_float2(0.f);
    $if(hit->is_hit()) {
        const Uint base = ctx.light_bases.read(hit.inst_id);
        light_index = select(base == InvalidUI32, base, base + hit.prim_id);
        uv = make_float2(hit.bary.y, 1.f - hit.bary.x - hit.bary.y);
    } $else {
        if (ctx.environment_index != InvalidUI32) {
            light_index = ctx.environment_index;
            uv = environment_uv(direction);
        }
    };
    return make_uint4(light_index, hit.prim_id, as<uint>(uv.x), as<uint>(uv.y));
}

} // namespace vision::restir_di
