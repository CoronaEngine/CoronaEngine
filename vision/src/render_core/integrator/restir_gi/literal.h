#pragma once

#include <string_view>

namespace vision::restir_gi {

// The descriptor ABI and typed image helpers come from the Vulkan builtin
// preamble. Matrices use Horizon's row-vector representation in Slang.
inline constexpr std::string_view LiteralSource = R"RESTIRGI(
#ifndef CORONA_RESTIR_GI_LITERAL
#define CORONA_RESTIR_GI_LITERAL

[ForceInline]
float rg_lit_safe(float value) {
    return isinf(value) || isnan(value) ? 0.0 : value;
}

[ForceInline]
float3 rg_lit_safe3(float3 value) {
    return float3(rg_lit_safe(value.x), rg_lit_safe(value.y), rg_lit_safe(value.z));
}

[ForceInline]
float rg_lit_smooth(float a, float b, float value) {
    float t = clamp((value - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

[ForceInline]
uint rg_lit_hash(uint value) {
    value += value << 10; value ^= value >> 6; value += value << 3;
    value ^= value >> 11; value += value << 15;
    return value;
}

[ForceInline]
uint rg_lit_hash_combine(uint x, uint y) {
    uint state = (x * 1664525u + y + 1013904223u) * 1664525u;
    state ^= state >> 11; state ^= (state << 7) & 0x9d2c5680u;
    state ^= (state << 15) & 0xefc60000u; state ^= state >> 18;
    return state;
}

[ForceInline]
uint rg_lit_seed(uint2 pixel, uint frame) {
    return rg_lit_hash_combine(pixel.x, rg_lit_hash_combine(pixel.y, rg_lit_hash(frame)));
}

[ForceInline]
float rg_lit_random(inout uint state) {
    uint old = state;
    state = rg_lit_hash(state);
    return asfloat((old & 0x007fffffu) | 0x3f800000u) - 1.0;
}

[ForceInline]
uint2 rg_lit_subpixel(uint frame, uint scale) {
    if (scale == 1u) return uint2(0u, 0u);
    uint phase = frame & 3u;
    return uint2(phase < 2u ? 1u : 0u, phase == 0u || phase == 3u ? 1u : 0u);
}

[ForceInline]
uint2 rg_lit_full_pixel(uint2 pixel, uint frame, uint scale, uint2 extent) {
    if (scale == 1u) return pixel;
    return min(pixel * scale + rg_lit_subpixel(frame, scale), extent - 1u);
}

[ForceInline]
bool rg_lit_in_cells(int2 pixel, uint2 sampling) {
    return all(pixel >= int2(0, 0)) && all(pixel < int2(sampling));
}

[ForceInline]
float3 rg_lit_point(float4x4 matrix, float3 point) {
    return mul(float4(point, 1.0), matrix).xyz;
}

[ForceInline]
float3 rg_lit_vector(float4x4 matrix, float3 direction) {
    return mul(float4(direction, 0.0), matrix).xyz;
}

[ForceInline]
float3 rg_lit_reconstruct(uint2 pixel, float depth, float4x4 raster_to_camera, float4x4 camera_to_world) {
    float3 ray = rg_lit_point(raster_to_camera, float3(float2(pixel) + 0.5, 0.0));
    return rg_lit_point(camera_to_world, ray * (depth / ray.z));
}

[ForceInline]
float3 rg_lit_direction(float3 delta) {
    return delta * rsqrt(max(dot(delta, delta), 1e-20));
}

[ForceInline]
float3 rg_lit_normal(uint packed) {
    float3 normal = float3(float(packed & 2047u) / 2047.0,
                          float((packed >> 11) & 1023u) / 1023.0,
                          float((packed >> 21) & 2047u) / 2047.0) * 2.0 - 1.0;
    return normalize(normal);
}

[ForceInline]
float rg_lit_depth_difference(float primary, float secondary) {
    return abs(secondary / max(primary, 1e-20) - 1.0);
}

[ForceInline]
float2 rg_lit_reservoir_mw(uint2 raw) {
    return float2(f16tof32(raw.y & 65535u), f16tof32(raw.y >> 16));
}

[ForceInline]
uint2 rg_lit_payload_pixel(uint payload) {
    return uint2(payload & 65535u, payload >> 16);
}

struct RgLiteralSample {
    float depth;
    float3 offset;
    float luminance;
    float3 normal;
};

[ForceInline]
RgLiteralSample rg_lit_sample(uint4 raw) {
    RgLiteralSample sample;
    sample.depth = asfloat(raw.x);
    sample.offset = float3(f16tof32(raw.y & 65535u), f16tof32(raw.y >> 16), f16tof32(raw.z & 65535u));
    sample.luminance = f16tof32(raw.z >> 16);
    sample.normal = rg_lit_normal(raw.w);
    return sample;
}

[ForceInline]
float rg_lit_visibility(float3 start, float3 hit, float3 source, float2 p0,
                        float start_depth, float source_depth, uint frame,
                        uint2 extent, uint2 sampling, uint scale,
                        float4x4 world_to_camera, float4x4 camera_to_raster,
                        float3 eye, OCTextureDesc depth_tex) {
    float3 ray = hit - start;
    float3 source_at_center_depth = eye + (source - eye) * (start_depth / max(source_depth, 1e-6));
    float3 end = start + ray * min(1.0, 3.0 * length(source_at_center_depth - start) / max(length(ray), 1e-6));
    float3 end_camera = rg_lit_point(world_to_camera, end);
    float2 p1 = rg_lit_point(camera_to_raster, end_camera / end_camera.z).xy;
    int count = min(6, int(floor(length(p1 - p0) * 0.25)));
    float inverse_start = 1.0 / max(start_depth, 1e-6);
    float inverse_end = 1.0 / max(end_camera.z, 1e-6);
    float inverse_segment_length = 1.0 / max(length((p1 - p0) / float2(extent)), 1e-6);
    float visibility = 1.0;
    for (int k = 0; k < count; ++k) {
        float t = (float(k) + 0.5) / float(count);
        float2 projected = lerp(p0, p1, t);
        int2 cell = int2(floor((projected - float2(rg_lit_subpixel(frame, scale))) / float(scale)));
        if (!rg_lit_in_cells(cell, sampling)) continue;
        uint2 pixel = rg_lit_full_pixel(uint2(cell), frame, scale, extent);
        float depth = oc_tex2d_read_native_float(depth_tex, int2(pixel));
        float inverse_depth = lerp(inverse_start, inverse_end, t);
        float quantized_t = length((float2(pixel) + 0.5 - p0) / float2(extent)) * inverse_segment_length;
        float biased_inverse = lerp(inverse_start, inverse_end, quantized_t);
        if (depth > 0.0 && 1.0 / depth > biased_inverse) {
            float difference = abs(inverse_depth * depth - 1.0);
            visibility *= 1.0 - rg_lit_smooth(0.05, 0.025, difference);
        }
    }
    return visibility;
}

[ForceInline]
void restir_gi_literal_spatial(uint2 pixel, uint frame, uint random_frame, uint pass,
    uint2 extent, uint2 sampling, uint scale,
    float4x4 camera_to_world, float4x4 world_to_camera,
    float4x4 camera_to_raster, float4x4 raster_to_camera, float3 eye,
    OCTextureDesc depth_tex, OCTextureDesc view_normal_tex, OCTextureDesc sample_tex,
    OCTextureDesc reservoir_input, OCTextureDesc reservoir_output) {
#if defined(CORONA_RESTIR_GI_DIAGNOSTIC) && CORONA_RESTIR_GI_DIAGNOSTIC <= 3
    // Diagnostic fresh/fresh_resolve/temporal: keep ping-pong storage valid
    // without changing the reservoir produced by temporal initialization.
    oc_tex2d_write_native(reservoir_output, int2(pixel), oc_tex2d_read_native_uint2(reservoir_input, int2(pixel)));
#else
    uint2 receiver_pixel = rg_lit_full_pixel(pixel, frame, scale, extent);
    float receiver_depth = oc_tex2d_read_native_float(depth_tex, int2(receiver_pixel));
    float3 receiver = rg_lit_reconstruct(receiver_pixel, receiver_depth, raster_to_camera, camera_to_world);
    float3 view_normal = oc_tex2d_read_native_float4(view_normal_tex, int2(receiver_pixel)).xyz;
    float3 normal = rg_lit_vector(camera_to_world, view_normal);
    float2 center_pixel = float2(receiver_pixel) + 0.5;
    float2 center_mw = rg_lit_reservoir_mw(oc_tex2d_read_native_uint2(reservoir_input, int2(pixel)));
    float sum = 0.0, stream_count = 0.0, selected_target = 0.0;
    uint selected = 0u;
    uint rng = rg_lit_seed(pixel, random_frame + pass * 123u);
    float radius_offset = rg_lit_random(rng);
    float max_count = pass == 0u ? 20.0 : 160.0;
    float tightness = 0.5 * rg_lit_smooth(max_count * 0.5, max_count, center_mw.x);
    float maximum = pass == 0u ? lerp(32.0, 12.0, tightness) : lerp(16.0, 6.0, tightness);
    maximum *= 2.0 / float(scale);
    float2 edge = min(float2(pixel), float2(sampling) - float2(pixel));
    float2 radius = min(float2(maximum), edge * (center_mw.x < 10.0 ? 100.0 : 1.25));
    uint tile_shift = (pass == 0u ? 3u : 2u) + (scale == 1u ? 1u : 0u);
    uint angular_rng = rg_lit_seed(pixel >> tile_shift, random_frame * 2u + pass);
    float angle_offset = rg_lit_random(angular_rng) * 6.28318530718;
    uint count = pass == 0u ? 8u : 5u;
    if (receiver_depth > 0.0) {
        for (uint slot = 0u; slot < count; ++slot) {
            float angle = (float(slot) + angle_offset) * 2.399963229728653;
            float2 distance = slot == 0u ? float2(0.0) : sqrt((float(slot) + radius_offset) / float(count)) * radius;
            int2 other = int2(pixel) + int2(float2(cos(angle), sin(angle)) * distance);
            if (!rg_lit_in_cells(other, sampling)) continue;
            uint2 raw = oc_tex2d_read_native_uint2(reservoir_input, other);
            float2 mw = rg_lit_reservoir_mw(raw);
            if (mw.x <= 0.0) continue;
            uint2 donor_pixel = rg_lit_full_pixel(uint2(other), frame, scale, extent);
            float donor_depth = oc_tex2d_read_native_float(depth_tex, int2(donor_pixel));
            if (donor_depth <= 0.0) continue;
            float normal_dot = dot(oc_tex2d_read_native_float4(view_normal_tex, int2(donor_pixel)).xyz, view_normal);
            float normal_term = max(0.0, normal_dot + 0.5);
            float relevance = normal_term * normal_term / 2.25;
            if (slot != 0u) {
                float difference = max(0.3, -view_normal.z) * rg_lit_depth_difference(receiver_depth, donor_depth);
                relevance *= 1.0 - rg_lit_smooth(0.0, pass == 0u ? 0.15 : 0.1, difference);
            }
            if (!(relevance > 0.0)) continue;
            float3 donor = rg_lit_reconstruct(donor_pixel, donor_depth, raster_to_camera, camera_to_world);
            uint2 payload_pixel = rg_lit_payload_pixel(raw.x);
            RgLiteralSample sample = rg_lit_sample(oc_tex2d_read_native_uint4(sample_tex, int2(payload_pixel)));
            float3 source = rg_lit_reconstruct(rg_lit_full_pixel(payload_pixel, frame, scale, extent), sample.depth, raster_to_camera, camera_to_world);
            float3 hit = source + sample.offset;
            float3 delta = hit - receiver;
            float current_distance = length(delta), donor_distance = length(hit - donor);
            float3 direction = rg_lit_direction(delta), donor_direction = rg_lit_direction(hit - donor);
            float cosine = dot(normal, direction);
            if (!(current_distance > 1e-6) || (slot != 0u && !(cosine >= 1e-5))) continue;
            float target = sample.luminance * max(0.0, cosine);
            float jacobian = 1.0;
            if (slot != 0u) {
                float ratio = rg_lit_safe(donor_distance / current_distance);
                float cosine_ratio = clamp(rg_lit_safe(-dot(sample.normal, direction) / -dot(sample.normal, donor_direction)), 0.0, 1e4);
                jacobian = sqrt(ratio * ratio * cosine_ratio);
            }
            float visibility = 1.0;
            if (pass == 1u) {
                visibility = rg_lit_visibility(receiver, hit, source, center_pixel, receiver_depth, sample.depth,
                    frame, extent, sampling, scale, world_to_camera, camera_to_raster, eye, depth_tex);
            }
            float M = min(mw.x, 500.0) * relevance;
            stream_count += M;
            float weight = max(0.0, rg_lit_safe(target * (visibility * jacobian) * mw.y * M));
            sum += weight;
            // DSL Bool conjunction evaluates the RNG expression even at zero weight.
            float draw = rg_lit_random(rng);
            bool chosen = weight > 0.0 && draw * sum <= weight;
            if (chosen) { selected = raw.x; selected_target = target; }
        }
    }
    float W = clamp(rg_lit_safe(sum / max(1e-8, stream_count * selected_target)), 0.0, 10.0);
    uint packed_mw = f32tof16(stream_count) | (f32tof16(W) << 16);
    oc_tex2d_write_native(reservoir_output, int2(pixel), uint2(selected, packed_mw));
#endif
}

)RESTIRGI"
R"RESTIRGI([ForceInline]
float rg_lit_filter(float3 view_normal, float3 sample_normal, float receiver_depth, float donor_depth) {
    float similarity = clamp(dot(view_normal, sample_normal), 0.0, 1.0);
    float denominator = similarity * similarity * (0.01 - 1.0) + 1.0;
    return 0.01 / (denominator * denominator) *
        exp2(-200.0 * abs(view_normal.z) * rg_lit_depth_difference(receiver_depth, donor_depth));
}

[ForceInline]
float3 restir_gi_literal_resolve(uint2 pixel, uint frame, uint random_frame, uint2 extent, uint2 sampling, uint scale,
    float4x4 camera_to_world, float4x4 world_to_camera, float4x4 raster_to_camera,
    OCTextureDesc depth_tex, OCTextureDesc view_normal_tex, OCTextureDesc world_normal_tex,
    OCTextureDesc sample_tex, OCTextureDesc candidate_hit_tex, OCTextureDesc candidate_radiance_tex,
    OCTextureDesc selected_radiance_tex, OCTextureDesc reservoir_tex) {
    float receiver_depth = oc_tex2d_read_native_float(depth_tex, int2(pixel));
    if (!(receiver_depth > 0.0)) return float3(0.0);
    float3 receiver = rg_lit_reconstruct(pixel, receiver_depth, raster_to_camera, camera_to_world);
    float3 normal = rg_lit_normal(oc_tex2d_read_native_uint(world_normal_tex, int2(pixel)));
#if defined(CORONA_RESTIR_GI_DIAGNOSTIC) && CORONA_RESTIR_GI_DIAGNOSTIC >= 3
    // Full-resolution center-only output measures the selected full path;
    // neither a nearby gather nor the near/far fades enter this estimator.
    uint2 raw = oc_tex2d_read_native_uint2(reservoir_tex, int2(pixel));
    float2 mw = rg_lit_reservoir_mw(raw);
    if (mw.x <= 0.0) return float3(0.0);
    uint2 source_pixel = rg_lit_payload_pixel(raw.x);
    RgLiteralSample sample = rg_lit_sample(oc_tex2d_read_native_uint4(sample_tex, int2(source_pixel)));
    float3 source = rg_lit_reconstruct(source_pixel, sample.depth, raster_to_camera, camera_to_world);
    float3 direction = rg_lit_direction(source + sample.offset - receiver);
    float3 radiance = oc_tex2d_read_native_float4(selected_radiance_tex, int2(source_pixel)).xyz;
    return max(rg_lit_safe3(radiance * (2.0 * max(0.0, dot(normal, direction))) * mw.y), float3(0.0));
#else
    float3 view_normal = rg_lit_vector(world_to_camera, normal);
    float3 near_sum = float3(0.0), far_sum = float3(0.0);
    float near_weight = 0.0, far_weight = 0.0;
    bool sharpen = false;
    uint rng = rg_lit_seed(pixel, random_frame);
    float angle_random = rg_lit_random(rng) * 6.28318530718;
    uint quad = (((pixel.x & 1u) | (pixel.y & 1u) * 2u) + rg_lit_hash(random_frame)) & 3u;
    float end = receiver_depth * (40.0 / float(extent.y)), start = end * 0.5;
    for (uint slot = 0u; slot < 4u; ++slot) {
        float angle = (float(slot) + angle_random) * 2.399963229728653 + float(quad) * 1.57079632679;
        float radius = (pow(float(slot), 0.666) + 0.4) * (2.0 / float(scale));
        int2 cell = int2(floor(float2(pixel) / float(scale) + float2(cos(angle), sin(angle)) * radius));
        if (!rg_lit_in_cells(cell, sampling)) continue;
        uint2 donor_pixel = rg_lit_full_pixel(uint2(cell), frame, scale, extent);
        float donor_depth = oc_tex2d_read_native_float(depth_tex, int2(donor_pixel));
        if (donor_depth <= 0.0) continue;
        float3 hit = rg_lit_reconstruct(donor_pixel, donor_depth, raster_to_camera, camera_to_world) +
            oc_tex2d_read_native_float4(candidate_hit_tex, cell).xyz;
        float3 radiance = oc_tex2d_read_native_float4(candidate_radiance_tex, cell).xyz;
        float3 sample_normal = oc_tex2d_read_native_float4(view_normal_tex, int2(donor_pixel)).xyz;
        float3 delta = hit - receiver;
        float distance = length(delta);
        float3 direction = rg_lit_direction(delta);
        float attenuation = rg_lit_smooth(end, start, distance);
        sharpen = sharpen || attenuation > 0.9;
        float filter = rg_lit_filter(view_normal, sample_normal, receiver_depth, donor_depth);
        float3 contribution = radiance * (2.0 * max(0.0, dot(normal, direction))) * 1.0 * attenuation;
        near_sum += contribution * filter;
        near_weight += filter;
    }
    for (uint slot = 0u; slot < 4u; ++slot) {
        float angle = (float(slot) + angle_random) * 2.399963229728653 + float(quad) * 1.57079632679;
        float radius = (pow(float(slot), 0.666) + 0.4) * (2.0 / float(scale));
        radius *= sharpen ? 0.5 : 1.0;
        int2 cell = int2(floor(float2(pixel) / float(scale) + float2(cos(angle), sin(angle)) * radius));
        if (!rg_lit_in_cells(cell, sampling)) continue;
        uint2 donor_pixel = rg_lit_full_pixel(uint2(cell), frame, scale, extent);
        float donor_depth = oc_tex2d_read_native_float(depth_tex, int2(donor_pixel));
        if (donor_depth <= 0.0) continue;
        uint2 raw = oc_tex2d_read_native_uint2(reservoir_tex, cell);
        float2 mw = rg_lit_reservoir_mw(raw);
        if (mw.x <= 0.0) continue;
        uint2 source_pixel = rg_lit_payload_pixel(raw.x);
        RgLiteralSample sample = rg_lit_sample(oc_tex2d_read_native_uint4(sample_tex, int2(source_pixel)));
        uint2 full_source = rg_lit_full_pixel(source_pixel, frame, scale, extent);
        float3 sample_normal = oc_tex2d_read_native_float4(view_normal_tex, int2(full_source)).xyz;
        float3 hit = rg_lit_reconstruct(full_source, sample.depth, raster_to_camera, camera_to_world) + sample.offset;
        float3 radiance = oc_tex2d_read_native_float4(selected_radiance_tex, int2(source_pixel)).xyz;
        float3 delta = hit - receiver;
        float distance = length(delta);
        float3 direction = rg_lit_direction(delta);
        float attenuation = rg_lit_smooth(start, end, distance);
        float filter = rg_lit_filter(view_normal, sample_normal, receiver_depth, donor_depth);
        float3 contribution = radiance * (2.0 * max(0.0, dot(normal, direction))) * mw.y * attenuation;
        far_sum += contribution * filter;
        far_weight += filter;
    }
    float3 result = near_sum / max(near_weight, 1e-20) + far_sum / max(far_weight, 1e-20);
    return max(rg_lit_safe3(result), float3(0.0));
#endif
}

#endif
)RESTIRGI";

} // namespace vision::restir_gi
