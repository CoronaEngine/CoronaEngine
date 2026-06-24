#pragma once

#include "math/basic_types.h"
#include "dsl/dsl.h"
#include "base/mgr/global.h"
#include "base/denoiser.h"
#include "base/sensor/frame_buffer.h"
#include "utils.h"

namespace vision::svgf {

struct ModulatorParam {
    BufferDesc<RadType4> radiance_direct;
    BufferDesc<RadType4> radiance_indirect;
    BufferDesc<TriangleHit> visibility_buffer;
    BufferDesc<float2> motion_vectors;
    array_float3 camera_pos{};
    uint channel_kind{};///< RealTimeDenoiseInput::ChannelKind (0=diffuse/specular, 1=direct/indirect)
};

}// namespace vision::svgf

OC_PARAM_STRUCT(vision::svgf, ModulatorParam, radiance_direct, radiance_indirect,
                visibility_buffer, motion_vectors, camera_pos, channel_kind){};

namespace vision::svgf {

class SVGF;

class Modulator : public Toolkit, public RuntimeObject {
private:
    SVGF *svgf_{nullptr};
    Shader<void(ModulatorParam)> modulate_;
    Shader<void(ModulatorParam)> demodulate_;

public:
    explicit Modulator(SVGF *svgf)
        : svgf_(svgf) {}
    VS_HOTFIX_MAKE_RESTORE(RuntimeObject, svgf_, modulate_, demodulate_)
    void prepare() noexcept;
    void compile() noexcept;
    [[nodiscard]] CommandBatch modulate(RealTimeDenoiseInput &input) noexcept;
    [[nodiscard]] CommandBatch demodulate(RealTimeDenoiseInput &input) noexcept;
};

}// namespace vision::svgf
