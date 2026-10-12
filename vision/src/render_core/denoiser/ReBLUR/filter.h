#pragma once

#include "params.h"
#include "base/using.h"
#include <array>

namespace vision::reblur {
struct Guide {
    float4 position{}; // world position, positive viewZ; w=0 for sky/invalid
    float4 normal_roughness{};
    float4 diffuse_factor{1.f, 1.f, 1.f, 0.f};
    float4 specular_factor{1.f, 1.f, 1.f, 0.f};
    float2 motion{}; // current minus previous raster coordinates
    uint4 identity{}; // instance, material, stable branch, valid
};

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
    float4 diffuse{}, specular{}, fast_count{};
    float2 stabilized{};
    float hit_distance{};
};
struct TemporalData {
    float4 fast_count{}, stabilized{}, tracking{};
};

struct Settings {
    float max_history{30.f}, fast_history{6.f}, history_fix_frames{3.f};
    float history_fix_stride{14.f};
    float diffuse_prepass{30.f}, specular_prepass{50.f};
    float min_radius{1.f}, max_radius{30.f};
    float lobe_fraction{0.15f}, roughness_fraction{0.15f};
    float plane_sensitivity{0.02f}, min_hit_weight{0.1f};
    float stabilization{1.f}, antilag_sigma{4.f}, antilag_sensitivity{3.f};
    uint anti_firefly{1u};
    HitDistanceReconstructionMode reconstruction{HitDistanceReconstructionMode::OFF};
};

struct FilterParam {
    BufferDesc<Guide> guide;
    BufferDesc<GeometryHistory> prev_guide;
    BufferDesc<TemporalData> history;
    BufferDesc<PersistentHistory> prev_history;
    BufferDesc<float4> raw_diffuse, raw_specular, output_diffuse, output_specular;
    BufferDesc<float4> src_diffuse, src_specular, dst_diffuse, dst_specular;
    BufferDesc<float2> fast;
    BufferDesc<float> tracking;
    BufferDesc<uint> tiles;
    uint2 size{};
    uint frame{}, history_valid{}, reconstruction{};
    float4x4 world_to_raster{make_float4x4(1.f)}, prev_world_to_raster{make_float4x4(1.f)};
    array_float3 camera{}, prev_camera{};
    float unproject{1.f}, max_history{}, fast_history{}, fix_frames{}, fix_stride{};
    float diffuse_prepass{}, specular_prepass{}, min_radius{}, max_radius{};
    float lobe_fraction{}, roughness_fraction{}, plane_sensitivity{}, min_hit_weight{};
    float stabilization{}, antilag_sigma{}, antilag_sensitivity{};
    uint anti_firefly{};
};
}

OC_STRUCT(vision::reblur, Guide, position, normal_roughness, diffuse_factor, specular_factor, motion, identity) {};
OC_STRUCT(vision::reblur, History, diffuse, specular, fast_count, stabilized, tracking) {};
OC_STRUCT(vision::reblur, GeometryHistory, position, normal_roughness, identity) {};
OC_STRUCT(vision::reblur, PersistentHistory, diffuse, specular, fast_count, stabilized, hit_distance) {};
OC_STRUCT(vision::reblur, TemporalData, fast_count, stabilized, tracking) {};
OC_PARAM_STRUCT(vision::reblur, FilterParam, guide, prev_guide, history, prev_history,
                raw_diffuse, raw_specular, output_diffuse, output_specular,
                src_diffuse, src_specular, dst_diffuse, dst_specular, fast, tracking, tiles,
                size, frame, history_valid, reconstruction, world_to_raster, prev_world_to_raster,
                camera, prev_camera, unproject, max_history, fast_history, fix_frames, fix_stride,
                diffuse_prepass, specular_prepass, min_radius, max_radius, lobe_fraction,
                roughness_fraction, plane_sensitivity, min_hit_weight,
                stabilization, antilag_sigma, antilag_sensitivity, anti_firefly) {};

namespace vision::reblur {
// The scene-independent shader stages are shared with the GPU integration tests.
// Previous-frame data is immutable through temporal reprojection, then updated
// by pointwise post-blur/stabilization/unpack writes. Current data is shared.
class Filter {
    uint2 size_{};
    struct Layer {
        Buffer<GeometryHistory> guide;
        Buffer<PersistentHistory> history;
        uint frame{InvalidUI32};
    };
    std::array<Layer, 3> layers_;
    Buffer<Guide> guide_;
    Buffer<TemporalData> history_;
    std::array<Buffer<float4>, 2> diffuse_, specular_;
    Buffer<float2> fast_;
    Buffer<float> tracking_;
    Buffer<uint> tiles_;
    Shader<void(FilterParam)> pack_, classify_, reconstruct_, prepass_, temporal_;
    Shader<void(FilterParam)> fix_, blur_, post_blur_, stabilize_, unpack_;
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
    void compile(Device &device);
    void reset() noexcept;
    [[nodiscard]] uint2 resolution() const noexcept { return size_; }
    [[nodiscard]] BufferView<Guide> guides() const { return guide_.view(); }
    [[nodiscard]] BufferView<TemporalData> history() const { return history_.view(); }
    [[nodiscard]] CommandBatch dispatch(FilterParam param, uint layer = 0u);
};
}
