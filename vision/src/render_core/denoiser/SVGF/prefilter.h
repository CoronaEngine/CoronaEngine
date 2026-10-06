#pragma once

#include "math/basic_types.h"
#include "dsl/dsl.h"
#include "base/denoiser.h"
#include "base/mgr/global.h"
#include "base/mgr/pipeline.h"
#include "utils.h"
#include "base/using.h"

namespace vision::svgf {

struct PrefilterParam {
    BufferDesc<RadType4> radiance_direct;
    BufferDesc<RadType4> radiance_indirect;
    BufferDesc<SVGFDataDual> svgf_buffer;
    BufferDesc<TriangleHit> visibility_buffer;
    array_float3 camera_pos{};
    uint channel_kind{};
    uint use_shading_normal{};
};

}// namespace vision::svgf

OC_PARAM_STRUCT(vision::svgf, PrefilterParam,
    radiance_direct, radiance_indirect, svgf_buffer,
    visibility_buffer, camera_pos, channel_kind, use_shading_normal){};

namespace vision::svgf {
class SVGF;

class Prefilter : public Toolkit, public RuntimeObject {
private:
    SVGF *svgf_{nullptr};
    Shader<void(PrefilterParam)> prefilter_shader_;

public:
    explicit Prefilter(SVGF *svgf)
        : svgf_(svgf) {}

    VS_HOTFIX_MAKE_RESTORE(RuntimeObject, svgf_, prefilter_shader_)

    void prepare() noexcept;
    void compile() noexcept;
    [[nodiscard]] CommandBatch dispatch(RealTimeDenoiseInput &input, bool use_shading_normal = true) noexcept;
};

}// namespace vision::svgf
