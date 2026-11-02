#pragma once

#include "params.h"
#include "base/using.h"
#include <array>

namespace vision::reblur {
struct Guide {
    float4 position{}; // world position, positive viewZ; w=0 for sky/invalid
    uint4 identity{};
    float4 normal_roughness{};
    float4 diffuse_factor{1.f, 1.f, 1.f, 0.f}; // RGB demodulation, motion.x in w
    float4 specular_factor{1.f, 1.f, 1.f, 0.f}; // RGB demodulation, motion.y in w
};

// Fused Pack has already consumed geometry, motion and classification. Unpack
// needs only these exact FP32 factors plus the original viewZ and roughness.
struct CompactGuide {
    float4 diffuse_depth{};
    float4 specular_roughness{};
};
static_assert(sizeof(CompactGuide) == 32u);

struct History {
    float4 diffuse{}, specular{};
    float4 fast_count{}; // fast luminance D/S, accumulated frame count D/S
    float4 stabilized{}; // luminance D/S, reprojection confidence D/S
    float4 tracking{}; // virtual raster position xy, curvature, hit distance
};

// Persist only fields consumed next frame. Factors, motion, confidence and
// virtual raster coordinates are current-frame data, shared across layers.
struct GeometryHistory {
    float4 position{}, normal_roughness{};
    uint4 identity{};
};
struct PersistentHistory {
    // FP16 storage only: shaders promote these values before arithmetic.
    half4 diffuse{}, specular{}, fast_count{};
    half2 stabilized{};
    float hit_distance{}; // world units, not limited to the FP16 range
};
struct TemporalData {
    half4 fast_count{}, stabilized{};
    float4 tracking{};
};

struct Settings {
    float max_history{50.f}, fast_history{6.f}, history_fix_frames{3.f};
    float history_fix_stride{14.f};
    float diffuse_prepass{15.f}, specular_prepass{40.f};
    float min_radius{1.f}, max_radius{30.f};
    float lobe_fraction{0.15f}, roughness_fraction{0.15f};
    float plane_sensitivity{0.02f}, min_hit_weight{0.1f};
    float stabilization{63.f / 64.f}, antilag_sigma{4.f}, antilag_sensitivity{3.f};
    uint anti_firefly{1u};
    HitDistanceReconstructionMode reconstruction{HitDistanceReconstructionMode::AREA_5X5};
};

struct FilterParam {
    BufferDesc<> guide; // Guide fields stored contiguously through Ocarina's SOA view
    BufferDesc<> prev_guide;
    BufferDesc<> history;
    BufferDesc<> prev_history;
    BufferDesc<float4> raw_diffuse, raw_specular, output_diffuse, output_specular;
    BufferDesc<half4> src_diffuse, src_specular, dst_diffuse, dst_specular;
    BufferDesc<half2> fast;
    BufferDesc<float> tracking;
    BufferDesc<uint> tiles;
    uint2 size{};
    uint frame{}, history_valid{}, reconstruction{};
    float4x4 world_to_raster{make_float4x4(1.f)}, prev_world_to_raster{make_float4x4(1.f)};
    array_float3 camera{}, prev_camera{};
    float unproject{1.f}, frame_rate_scale{1.f}, max_history{}, fast_history{}, fix_frames{}, fix_stride{};
    float diffuse_prepass{}, specular_prepass{}, min_radius{}, max_radius{};
    float lobe_fraction{}, roughness_fraction{}, plane_sensitivity{}, min_hit_weight{};
    float stabilization{}, antilag_sigma{}, antilag_sensitivity{};
    uint anti_firefly{};
};
}

OC_STRUCT(vision::reblur, Guide, position, identity, normal_roughness, diffuse_factor, specular_factor) {};
OC_STRUCT(vision::reblur, CompactGuide, diffuse_depth, specular_roughness) {};
OC_STRUCT(vision::reblur, History, diffuse, specular, fast_count, stabilized, tracking) {};
OC_STRUCT(vision::reblur, GeometryHistory, position, normal_roughness, identity) {};
OC_STRUCT(vision::reblur, PersistentHistory, diffuse, specular, fast_count, stabilized, hit_distance) {};
OC_STRUCT(vision::reblur, TemporalData, fast_count, stabilized, tracking) {};
OC_PARAM_STRUCT(vision::reblur, FilterParam, guide, prev_guide, history, prev_history,
                raw_diffuse, raw_specular, output_diffuse, output_specular,
                src_diffuse, src_specular, dst_diffuse, dst_specular, fast, tracking, tiles,
                size, frame, history_valid, reconstruction, world_to_raster, prev_world_to_raster,
                camera, prev_camera, unproject, frame_rate_scale, max_history, fast_history, fix_frames, fix_stride,
                diffuse_prepass, specular_prepass, min_radius, max_radius, lobe_fraction,
                roughness_fraction, plane_sensitivity, min_hit_weight,
                stabilization, antilag_sigma, antilag_sensitivity, anti_firefly) {};

namespace vision::reblur {
// The scene-independent shader stages are shared with the GPU integration tests.
// Previous-frame data is immutable through temporal reprojection, then updated
// by pointwise stabilization/output writes. Current data is shared.
class Filter {
    uint2 size_{};
    struct Layer {
        ocarina::ByteBuffer guide;
        ocarina::ByteBuffer history;
        uint frame{InvalidUI32};
    };
    std::array<Layer, 3> layers_;
    ocarina::ByteBuffer guide_;
    ocarina::ByteBuffer history_;
    std::array<Buffer<half4>, 2> diffuse_, specular_;
    Buffer<half2> fast_;
    Buffer<float> tracking_;
    Buffer<uint> tiles_;
    Shader<void(FilterParam)> pack_, reconstruct_, prepass_, temporal_;
    Shader<void(FilterParam)> fix_, blur_, post_blur_, stabilize_;
    void compile_modulation(Device &device);
    void compile_prepass(Device &device);
    void compile_temporal(Device &device);
    void compile_history_fix(Device &device);
    void compile_spatial(Device &device);
    void compile_stabilization(Device &device);
    [[nodiscard]] Shader<void(FilterParam)> compile_spatial_pass(Device &device, uint stage);

public:
    Settings settings;
    void prepare(Device &device, uint2 size, uint layer_count = 1u);
    void prepare_native(Device &device, uint2 size, bool compact_guide = false);
    void compile(Device &device);
    void reset() noexcept;
    [[nodiscard]] uint2 resolution() const noexcept { return size_; }
    [[nodiscard]] ByteBufferView guides() const { return guide_.view(); }
    [[nodiscard]] ByteBufferView history() const { return history_.view(); }
    [[nodiscard]] FilterParam native_parameters(FilterParam param, uint layer);
    [[nodiscard]] CommandBatch dispatch(FilterParam param, uint layer = 0u);
};
}
