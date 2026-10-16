//
// Created by ling.zhu on 2025/8/6.
//

#pragma once

#include "math/basic_types.h"
#include "dsl/dsl.h"
#include "base/denoiser.h"
#include "filter.h"

namespace vision::relax {

struct GuideParam {
    BufferDesc<TriangleHit> visibility;
    BufferDesc<StablePlaneData> planes;
    BufferDesc<SurfaceData> surfaces;
    BufferDesc<SurfaceExtend> extensions;
    BufferDesc<float2> motion;
    BufferDesc<Guide> output;
    BufferDesc<float4> diffuse, specular;
    uint layered{}, stable{}, frame{};
};
struct PublishParam {
    BufferDesc<float4> diffuse, specular, bypass;
    BufferDesc<RadType4> direct, indirect;
};
}
OC_PARAM_STRUCT(vision::relax, GuideParam, visibility, planes, surfaces, extensions, motion, output, diffuse, specular, layered, stable, frame) {};
OC_PARAM_STRUCT(vision::relax, PublishParam, diffuse, specular, bypass, direct, indirect) {};

namespace vision::relax {

class ReLAX : public Denoiser {
    SP<Filter> filter_{make_shared<Filter>()};
    SP<Denoiser> coverage_;
    bool enabled_{};
    bool compiled_{};
    uint frame_{InvalidUI32};
    float fov_{};
    float4x4 current_raster_{make_float4x4(1.f)}, previous_raster_{make_float4x4(1.f)};
    float4x4 current_view_{make_float4x4(1.f)}, previous_view_{make_float4x4(1.f)};
    Shader<void(struct GuideParam)> prepare_guides_;
    Shader<void(struct PublishParam)> publish_;
public:
    ReLAX() = default;
    explicit ReLAX(const DenoiserDesc &desc);
    void initialize_(const NodeDesc &desc) noexcept override;
    void prepare() noexcept override;
    void compile() noexcept override;
    void update_resolution(uint2 resolution) noexcept override;
    void set_enabled(bool enabled) noexcept override;
    void set_stable_planes_enabled(bool enabled) noexcept override;
    [[nodiscard]] bool enabled() noexcept override { return enabled_; }
    [[nodiscard]] bool has_prepared_resources() const noexcept override { return filter_->resolution().x != 0u; }
    [[nodiscard]] CommandBatch dispatch(RealTimeDenoiseInput &input) noexcept override;
    void render_sub_UI(Widgets *widgets) noexcept override;
    VS_HOTFIX_MAKE_RESTORE(Denoiser, filter_, coverage_, enabled_, compiled_, frame_, fov_, current_raster_, previous_raster_, current_view_, previous_view_, prepare_guides_, publish_)
    [[nodiscard]] string_view impl_type() const noexcept override { return "ReLAX"; }
    [[nodiscard]] string_view category() const noexcept override { return "denoiser"; }
    [[nodiscard]] bool requires_lobe_signals() const noexcept override { return true; }
};

}// namespace vision::relax
