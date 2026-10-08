#pragma once

#include "math/basic_types.h"
#include "dsl/dsl.h"
#include "base/denoiser.h"
#include "atrous.h"
#include "modulator.h"
#include "variance_estimator.h"
#include "prefilter.h"
#include "utils.h"
#include "base/using.h"

namespace vision::svgf {

struct ResolveParam {
    // Current guides borrow the spatial-filter scratch after edge classification.
    // Previous guides have separate storage for reprojection reads.
    BufferDesc<float4> guide_position, guide_normal, guide_depth;
    BufferDesc<uint4> guide_meta;
    BufferDesc<float4> prev_guide_position, prev_guide_normal, prev_guide_depth;
    BufferDesc<uint4> prev_guide_meta;
    BufferDesc<float4> edge_position, edge_normal;
    BufferDesc<uint4> edge_identity, edge_surface;
    BufferDesc<uint> edge_flags;
    uint edge_layer{0u};

    uint composed_coverage{0u};
    BufferDesc<StablePlaneData> coverage_planes;
    BufferDesc<StablePlaneData> prev_coverage_planes;
    uint use_stable_planes{0u};
    uint layered{0u};
    BufferDesc<StablePlaneData> stable_planes;
    BufferDesc<StablePlaneData> prev_stable_planes;
    BufferDesc<SurfaceData> stable_surfaces;
    BufferDesc<SurfaceData> prev_stable_surfaces;
    BufferDesc<RadType4> direct;
    BufferDesc<RadType4> indirect;
    BufferDesc<float4> history_direct;
    BufferDesc<float4> history_indirect;
    BufferDesc<float4> output_direct;
    BufferDesc<float4> output_indirect;
    BufferDesc<TriangleHit> visibility;
    BufferDesc<TriangleHit> prev_visibility;
    BufferDesc<float2> motion_vectors;
    array_float3 camera_pos;
    array_float3 prev_camera_pos;
    uint history_valid{0u};
    uint channel_kind{0u};
    uint frame_index{0u};
    float alpha{1.f};
    float interior_alpha{1.f};
};

}// namespace vision::svgf

OC_PARAM_STRUCT(vision::svgf, ResolveParam, guide_position, guide_normal, guide_depth, guide_meta, prev_guide_position, prev_guide_normal, prev_guide_depth, prev_guide_meta, edge_position, edge_normal, edge_identity, edge_surface, edge_flags, edge_layer, composed_coverage, coverage_planes, prev_coverage_planes, use_stable_planes, layered, stable_planes, prev_stable_planes, stable_surfaces, prev_stable_surfaces, direct, indirect,
                history_direct, history_indirect, output_direct, output_indirect,
                visibility, prev_visibility, motion_vectors, camera_pos, prev_camera_pos,
                history_valid, channel_kind, frame_index, alpha, interior_alpha){};

namespace vision::svgf {
class SVGF : public Denoiser, public GBufferCallback, public enable_shared_from_this<SVGF> {
public:
    // Temporal history is double-buffered to remove the read/write hazard in the
    // variance estimator: it reprojects (scatter-reads) the PREVIOUS frame's history
    // while writing the CURRENT frame's history. With a single buffer those races under
    // motion (a tap may read a neighbour slot already overwritten this frame). cur/prev
    // are selected by frame parity; each is a full buffer (offset 0), so no descriptor
    // offset semantics are relied upon.
    RegistrableBuffer<SVGFDataDual> svgf_data;
    RegistrableBuffer<SVGFDataDual> svgf_data2;

private:
    HotfixSlot<SP<AtrousFilter>> atrous_{};
    HotfixSlot<SP<Modulator>> modulator_{};
    HotfixSlot<SP<VarianceEstimator>> variance_estimator_{};
    HotfixSlot<SP<Prefilter>> prefilter_{};
    // Layer zero retains the public N-sized buffers used by diagnostics.
    std::array<Buffer<SVGFDataDual>, StablePlaneCount - 1u> layer_data_, layer_data2_;
    std::array<Buffer<float4>, StablePlaneCount> resolve_direct_, resolve_indirect_;
    std::array<Buffer<float4>, StablePlaneCount> resolve_direct2_, resolve_indirect2_;
    struct LayerState {
        float4x4 camera{};
        float fov{};
        uint frame{InvalidUI32};
        uint history{};
        uint temporal_frame{InvalidUI32};
    };
    std::array<LayerState, StablePlaneCount> layer_state_{};
    Shader<void(ResolveParam)> resolve_shader_;
    Shader<void(ResolveParam)> coverage_guide_shader_, legacy_resolve_shader_;
    Buffer<float4> coverage_position_, coverage_normal_, coverage_depth_;
    Buffer<uint4> coverage_meta_;
    Buffer<uint> edge_flags_;
    Shader<void(ResolveParam)> edge_guide_shader_, edge_classify_shader_;
    Shader<void(ResolveParam)> publish_resolve_shader_;
    Shader<void(VarianceEstimatorParam)> clear_invalid_shader_;

    void prepare_resolve(uint pixel_num);
    void compile_resolve();
    [[nodiscard]] CommandBatch resolve(RealTimeDenoiseInput &input);

private:
    struct Params {
        float sigma_rt_{4.0f};
        float sigma_normal_{128.f};
        float sigma_depth_{SVGFConfig::GeometryWeight::kAtrousDepthScaleDefault};
        bool spatial_filter_{true};
        bool switch_{true};

        Params() = default;
        explicit Params(const DenoiserDesc &desc)
            : sigma_rt_(desc["sigma_rt"].as_float(4.0f)),
              sigma_normal_(desc["sigma_normal"].as_float(128.f)),
              sigma_depth_(desc["sigma_depth"].as_float(SVGFConfig::GeometryWeight::kAtrousDepthScaleDefault)),
              spatial_filter_(desc["spatial_filter"].as_bool(true)),
        //
        switch_(false) {}
    };
    Params params_;

public:
    SVGF() = default;
    explicit SVGF(const DenoiserDesc &desc)
        : Denoiser(desc),
          svgf_data(pipeline()->bindless_array()),
          svgf_data2(pipeline()->bindless_array()),
          params_(desc) {}

    void initialize_(const vision::NodeDesc &node_desc) noexcept override;
    void compute_GBuffer(const vision::RayState &rs, const vision::Interaction &it) noexcept override;

    VS_HOTFIX_MAKE_RESTORE(Denoiser, svgf_data, svgf_data2,
                           atrous_, modulator_, variance_estimator_, prefilter_, params_,
                           resolve_direct_, resolve_indirect_, resolve_direct2_, resolve_indirect2_,
                           resolve_shader_, publish_resolve_shader_, clear_invalid_shader_, coverage_guide_shader_, legacy_resolve_shader_,
                           coverage_position_, coverage_normal_, coverage_depth_, coverage_meta_,
                           edge_flags_, edge_guide_shader_, edge_classify_shader_,
                           layer_data_, layer_data2_, layer_state_)
    VS_MAKE_PLUGIN_NAME_FUNC

#define VS_MAKE_MEMBER_GETTER(member, modifier)                                             \
    [[nodiscard]] const auto modifier member() const noexcept { return params_.member##_; } \
    [[nodiscard]] auto modifier member() noexcept { return params_.member##_; }

    VS_MAKE_MEMBER_GETTER(sigma_rt, )
    VS_MAKE_MEMBER_GETTER(sigma_normal, )
    VS_MAKE_MEMBER_GETTER(sigma_depth, )
    VS_MAKE_MEMBER_GETTER(spatial_filter, )

#undef VS_MAKE_MEMBER_GETTER

    [[nodiscard]] AtrousFilter *atrous() noexcept { return atrous_.get(); }
    [[nodiscard]] const AtrousFilter *atrous() const noexcept { return atrous_.get(); }

    void prepare_buffers();
    void render_sub_UI(Widgets *widgets) noexcept override;
    /// Current-frame history half (written this frame). Selected by frame parity.
    [[nodiscard]] BufferView<SVGFDataDual> svgf_buffer_cur(uint frame_index, uint layer = 0u) const noexcept {
        if (layer > 0u) return ((frame_index & 1u) == 0u) ? layer_data_[layer - 1u].view() : layer_data2_[layer - 1u].view();
        return ((frame_index & 1u) == 0u) ? svgf_data.view() : svgf_data2.view();
    }
    /// Previous-frame history half (read-only this frame).
    [[nodiscard]] BufferView<SVGFDataDual> svgf_buffer_prev(uint frame_index, uint layer = 0u) const noexcept {
        if (layer > 0u) return ((frame_index & 1u) == 0u) ? layer_data2_[layer - 1u].view() : layer_data_[layer - 1u].view();
        return ((frame_index & 1u) == 0u) ? svgf_data2.view() : svgf_data.view();
    }
    [[nodiscard]] bool history_valid(const RealTimeDenoiseInput &input) const noexcept;
    void prepare() noexcept override;
    void compile() noexcept override;
    [[nodiscard]] bool has_prepared_resources() const noexcept override {
        return svgf_data.has_registered();
    }
    void update_resolution(uint2 resolution) noexcept override;
    [[nodiscard]] CommandBatch dispatch(vision::RealTimeDenoiseInput &input) noexcept override;
    void set_enabled(bool enabled) noexcept override;
    [[nodiscard]] bool enabled() noexcept override;
};

}// namespace vision::svgf
