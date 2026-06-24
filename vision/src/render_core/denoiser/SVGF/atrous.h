#pragma once

#include "math/basic_types.h"
#include "dsl/dsl.h"
#include "base/sensor/filter.h"
#include "base/denoiser.h"
#include "base/mgr/global.h"
#include "base/mgr/pipeline.h"
#include "utils.h"
#include "base/using.h"

namespace vision::svgf {

struct CombinedAtrousParam {
    BufferDesc<RadType4> direct_src;
    BufferDesc<RadType4> direct_dst;
    BufferDesc<RadType4> indirect_src;
    BufferDesc<RadType4> indirect_dst;
    BufferDesc<TriangleHit> visibility_buffer;
    BufferDesc<SVGFDataDual> svgf_buffer;
    array_float3 camera_pos{};
    float l_phi{};
    float n_phi{};
    float z_phi{};
    int step_size{};
    uint iteration{};
    uint frame_index{};
    uint write_history{};
};

}// namespace vision::svgf

OC_PARAM_STRUCT(vision::svgf, CombinedAtrousParam, direct_src, direct_dst, indirect_src, indirect_dst,
visibility_buffer, svgf_buffer, camera_pos, l_phi, n_phi, z_phi, step_size, iteration, frame_index, write_history){};

namespace vision::svgf {
class SVGF;

class AtrousFilter : public Toolkit, public RuntimeObject {
private:
    SVGF *svgf_{nullptr};

    using combined_signature = void(CombinedAtrousParam);
    Shader<combined_signature> combined_shader_;

    Buffer<RadType4> temp_buffer_direct_;
    Buffer<RadType4> temp_buffer_indirect_;

public:
    explicit AtrousFilter(SVGF *svgf)
        : svgf_(svgf) {}
    VS_HOTFIX_MAKE_RESTORE(RuntimeObject, svgf_, combined_shader_,
                           temp_buffer_direct_, temp_buffer_indirect_)
    
    [[nodiscard]] Buffer<RadType4>& temp_buffer_direct() noexcept { return temp_buffer_direct_; }
    [[nodiscard]] Buffer<RadType4>& temp_buffer_indirect() noexcept { return temp_buffer_indirect_; }
    
    void prepare() noexcept;
    void compile() noexcept;
    
    void compile_combined() noexcept;
    
    [[nodiscard]] CommandBatch dispatch_combined(vision::RealTimeDenoiseInput &input, 
                                                 uint step_width, 
                                                 uint iteration) noexcept;
    
    void update_resolution(uint2 resolution) noexcept;
};

}// namespace vision::svgf