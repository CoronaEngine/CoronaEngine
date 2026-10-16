#pragma once
#include "config.h"
#include "math/geometry.h"
#include "rhi/context.h"
#include <array>
#include <functional>

namespace vision::relax {
struct Guide {
    float4 position{}; // Virtual world position, positive anchor axial viewZ; w=0 for invalid.
    float4 depth_position{}; // Actual depth anchor in world space; distinct after refraction.
    float4 normal_roughness{};
    float4 diffuse_factor{1.f, 1.f, 1.f, 0.f}, specular_factor{1.f, 1.f, 1.f, 0.f};
    float2 motion{}; // Current minus previous raster coordinates, in pixels.
    uint4 identity{}; // Receiver/prefix hash, material, branch sequence, valid.
};
struct GeometryHistory {
    float4 position{}, normal_roughness{};
    uint4 identity{};
};
struct PersistentHistory {
    float4 diffuse{}, specular{}; // Slow RGB and luminance second moment.
    float4 diffuse_fast{}, specular_fast{};
    float2 length_hit{}; // Shared history length and reflection tracking distance.
};
struct TemporalData {
    float4 diffuse_fast{}, specular_fast{};
    float4 meta{}; // History length, specular confidence, reflection hitT, curvature.
};
struct FilterParam {
    Settings settings;
    BufferDesc<Guide> guide;
    BufferDesc<GeometryHistory> prev_guide;
    BufferDesc<PersistentHistory> prev_history;
    BufferDesc<TemporalData> history;
    BufferDesc<float4> raw_diffuse, raw_specular, output_diffuse, output_specular;
    BufferDesc<float4> prepass_diffuse, prepass_specular;
    BufferDesc<float4> src_diffuse, src_specular, dst_diffuse, dst_specular;
    BufferDesc<uint> tiles;
    uint2 size{};
    uint frame{}, history_valid{}, step{1u}, first{}, last{};
    float4x4 world_to_raster{make_float4x4(1.f)}, prev_world_to_raster{make_float4x4(1.f)};
    float4x4 prev_world_to_view{make_float4x4(1.f)};
    array_float3 camera{}, prev_camera{};
    float unproject{1.f}, framerate_scale{1.f}, disocclusion_threshold{.01f}, jitter_delta{};
    float4 rotator{1.f, 0.f, 0.f, 1.f};
};
}

OC_STRUCT(vision::relax, Guide, position, depth_position, normal_roughness, diffuse_factor, specular_factor, motion, identity) {};
OC_STRUCT(vision::relax, GeometryHistory, position, normal_roughness, identity) {};
OC_STRUCT(vision::relax, PersistentHistory, diffuse, specular, diffuse_fast, specular_fast, length_hit) {};
OC_STRUCT(vision::relax, TemporalData, diffuse_fast, specular_fast, meta) {};
OC_PARAM_STRUCT(vision::relax, FilterParam, settings, guide, prev_guide, prev_history, history,
                raw_diffuse, raw_specular, output_diffuse, output_specular, prepass_diffuse, prepass_specular,
                src_diffuse, src_specular, dst_diffuse, dst_specular, tiles, size, frame, history_valid, step, first, last,
                world_to_raster, prev_world_to_raster, prev_world_to_view, camera, prev_camera, unproject, framerate_scale,
                disocclusion_threshold, jitter_delta, rotator) {};

namespace vision::relax {
// The scene adapter and GPU tests execute the same kernels and resource graph.
class Filter {
    uint2 size_{};
    struct Layer {
        Buffer<GeometryHistory> geometry;
        Buffer<PersistentHistory> history;
        uint frame{~0u};
    };
    std::array<Layer, 3> layers_;
    Buffer<Guide> guide_;
    Buffer<TemporalData> temporal_data_;
    std::array<Buffer<float4>, 2> diffuse_, specular_;
    Buffer<float4> prepass_diffuse_, prepass_specular_;
    Buffer<uint> tiles_;
    Shader<void(FilterParam)> pack_, unpack_, classify_, reconstruct_, prepass_;
    Shader<void(FilterParam)> temporal_, publish_;
    Shader<void(FilterParam)> fix_, clamp_, firefly_;
    Shader<void(FilterParam)> atrous_first_, atrous_;
    void compile_modulation(Device &device);
    void compile_prepass(Device &device);
    void compile_temporal(Device &device);
    void compile_history_fix(Device &device);
    void compile_history_clamping(Device &device);
    void compile_atrous(Device &device);

public:
    Settings settings;
    void prepare(Device &device, uint2 size, uint layer_count = 1u);
    void compile(Device &device);
    void reset() noexcept;
    [[nodiscard]] uint2 resolution() const noexcept { return size_; }
    [[nodiscard]] BufferView<Guide> guides() const { return guide_.view(); }
    [[nodiscard]] BufferView<TemporalData> history() const { return temporal_data_.view(); }
    [[nodiscard]] BufferView<PersistentHistory> persistent_history(uint layer = 0u) const { return layers_[layer].history.view(); }
    [[nodiscard]] BufferView<float4> prepass_diffuse() const { return prepass_diffuse_.view(); }
    [[nodiscard]] BufferView<float4> prepass_specular() const { return prepass_specular_.view(); }
    [[nodiscard]] size_t allocated_bytes() const noexcept;
    // Evaluation may execute/timestamp each batch in order; normal rendering queues them together.
    using PassSubmit = std::function<void(CommandBatch, string_view)>;
    [[nodiscard]] CommandBatch dispatch(FilterParam param, uint layer = 0u, PassSubmit submit = {});
};
}
