#pragma once

#include "math/basic_types.h"
#include "dsl/dsl.h"
#include "base/denoiser.h"
#include "base/mgr/global.h"
#include "base/mgr/pipeline.h"
#include "utils.h"
#include "base/using.h"

namespace vision::svgf {
struct VarianceEstimatorParam {
    BufferDesc<RadType4> radiance_direct;
    BufferDesc<RadType4> radiance_indirect;
    BufferDesc<SVGFDataDual> svgf_buffer_prev;
    BufferDesc<SVGFDataDual> svgf_buffer_cur;
    BufferDesc<TriangleHit> visibility_buffer;
    BufferDesc<TriangleHit> visibility_buffer_prev;
    BufferDesc<float2> motion_vectors;
    array_float3 camera_pos{};
    array_float3 prev_camera_pos{};
    float screen_short_edge{};
};

}// namespace vision::svgf

OC_PARAM_STRUCT(vision::svgf, VarianceEstimatorParam,
radiance_direct, radiance_indirect, svgf_buffer_prev, svgf_buffer_cur,
visibility_buffer, visibility_buffer_prev, motion_vectors, camera_pos, prev_camera_pos, screen_short_edge){};

namespace vision::svgf {
class SVGF;

class VarianceEstimator : public Toolkit, public RuntimeObject {
private:
    SVGF *svgf_{nullptr};

    using variance_signature = void(VarianceEstimatorParam);
    Shader<variance_signature> variance_shader_;

public:
    explicit VarianceEstimator(SVGF *svgf)
        : svgf_(svgf) {}

    VS_HOTFIX_MAKE_RESTORE(RuntimeObject, svgf_, variance_shader_)

    void prepare() noexcept;
    void compile() noexcept;

    [[nodiscard]] CommandBatch dispatch_variance(RealTimeDenoiseInput &input) noexcept;

    void update_resolution(uint2 resolution) noexcept;
};

}// namespace vision::svgf
