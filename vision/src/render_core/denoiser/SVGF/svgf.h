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

OC_PARAM_STRUCT(vision::svgf, ResolveParam, direct, indirect,
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
    // Reprojected coverage reads require separate previous/current storage.
    RegistrableBuffer<float4> resolve_direct_;
    RegistrableBuffer<float4> resolve_indirect_;
    RegistrableBuffer<float4> resolve_direct2_;
    RegistrableBuffer<float4> resolve_indirect2_;
    Shader<void(ResolveParam)> resolve_shader_;
    Shader<void(ResolveParam)> publish_resolve_shader_;
    float4x4 resolve_camera_{};
    float resolve_fov_{};
    uint resolve_frame_{InvalidUI32};
    uint resolve_history_{0u};

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
          resolve_direct_(pipeline()->bindless_array()),
          resolve_indirect_(pipeline()->bindless_array()),
          resolve_direct2_(pipeline()->bindless_array()),
          resolve_indirect2_(pipeline()->bindless_array()),
          params_(desc) {}

    void initialize_(const vision::NodeDesc &node_desc) noexcept override;
    void compute_GBuffer(const vision::RayState &rs, const vision::Interaction &it) noexcept override;

    VS_HOTFIX_MAKE_RESTORE(Denoiser, svgf_data, svgf_data2,
                           atrous_, modulator_, variance_estimator_, prefilter_, params_,
                           resolve_direct_, resolve_indirect_, resolve_direct2_, resolve_indirect2_,
                           resolve_shader_, publish_resolve_shader_, resolve_camera_,
                           resolve_fov_, resolve_frame_, resolve_history_)
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
    [[nodiscard]] BufferView<SVGFDataDual> svgf_buffer_cur(uint frame_index) const noexcept;
    /// Previous-frame history half (read-only this frame).
    [[nodiscard]] BufferView<SVGFDataDual> svgf_buffer_prev(uint frame_index) const noexcept;
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
