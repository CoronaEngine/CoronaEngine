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

struct CombinedPoissonParam {
    BufferDesc<uint> active_tiles;
    BufferDesc<float4> guide_position;
    BufferDesc<float4> guide_normal;
    BufferDesc<float4> guide_shading_normal;
    BufferDesc<float4> guide_albedo;
    BufferDesc<uint4> guide_identity;
    BufferDesc<uint4> guide_surface;
    uint use_stable_planes{0u};
    uint layered{0u};
    BufferDesc<StablePlaneData> stable_planes;
    BufferDesc<StablePlaneHistory> prev_stable_planes;
    BufferDesc<SurfaceData> stable_surfaces;
    BufferDesc<SurfaceData> prev_stable_surfaces;
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
    uint channel_kind{};
    uint use_shading_normal{};
    uint fused_modulation{};
    uint width{}, height{};
};

}// namespace vision::svgf

OC_PARAM_STRUCT(vision::svgf, CombinedPoissonParam, active_tiles, guide_position, guide_normal, guide_shading_normal, guide_albedo, guide_identity, guide_surface, use_stable_planes, layered, stable_planes, prev_stable_planes, stable_surfaces, prev_stable_surfaces, direct_src, direct_dst, indirect_src, indirect_dst,
visibility_buffer, svgf_buffer, camera_pos, l_phi, n_phi, z_phi, step_size, iteration, frame_index, write_history, channel_kind, use_shading_normal, fused_modulation, width, height){};

namespace vision::svgf {
class SVGF;

class PoissonFilter : public Toolkit, public RuntimeObject {
private:
    SVGF *svgf_{nullptr};

    using combined_signature = void(CombinedPoissonParam);
    Shader<combined_signature> combined_shader_;
    Shader<combined_signature> guide_shader_, layer_guide_shader_, legacy_combined_shader_;
    // One layer at a time; reused by all four filter iterations. Full precision.
    Buffer<float4> guide_position_, guide_normal_, guide_shading_normal_, guide_albedo_;
    Buffer<uint4> guide_identity_, guide_surface_;

    Buffer<RadType4> temp_buffer_direct_;
    Buffer<RadType4> temp_buffer_indirect_;

public:
    explicit PoissonFilter(SVGF *svgf)
        : svgf_(svgf) {}
    VS_HOTFIX_MAKE_RESTORE(RuntimeObject, svgf_, combined_shader_, guide_shader_, layer_guide_shader_, legacy_combined_shader_,
                           guide_position_, guide_normal_, guide_shading_normal_, guide_albedo_, guide_identity_, guide_surface_,
                           temp_buffer_direct_, temp_buffer_indirect_)
    
    [[nodiscard]] Buffer<RadType4>& temp_buffer_direct() noexcept { return temp_buffer_direct_; }
    [[nodiscard]] Buffer<RadType4>& temp_buffer_indirect() noexcept { return temp_buffer_indirect_; }
    
    // Spatial filtering finishes before coverage classification. Both may use
    // these full-precision scratch images, but never retain them across passes.
    [[nodiscard]] auto guide_position() const noexcept { return guide_position_.view(); }
    [[nodiscard]] auto guide_normal() const noexcept { return guide_normal_.view(); }
    [[nodiscard]] bool guides_ready(uint count, bool coverage_only = false) const noexcept {
        uint shading_count = coverage_only ? 0u : count;
        return guide_position_.size() == count && guide_shading_normal_.size() == shading_count && guide_albedo_.size() == shading_count;
    }
    [[nodiscard]] auto guide_identity() const noexcept { return guide_identity_.view(); }
    [[nodiscard]] auto guide_surface() const noexcept { return guide_surface_.view(); }

    template<typename Param>
    void bind_guides(Param &param) const {
        param.guide_position = guide_position_.descriptor();
        param.guide_normal = guide_normal_.descriptor();
        if constexpr (requires { param.guide_shading_normal; }) {
            param.guide_shading_normal = guide_shading_normal_.descriptor();
        }
        if constexpr (requires { param.guide_albedo; }) {
            param.guide_albedo = guide_albedo_.descriptor();
        }
        param.guide_identity = guide_identity_.descriptor();
        param.guide_surface = guide_surface_.descriptor();
    }
    [[nodiscard]] CommandBatch dispatch_guide(RealTimeDenoiseInput &input, bool fused_modulation = false) noexcept;
    void prepare() noexcept;
    void prepare_guides(uint pixel_num, bool coverage_only = false) noexcept;
    void compile() noexcept;
    
    void compile_combined() noexcept;
    
    [[nodiscard]] CommandBatch dispatch_combined(vision::RealTimeDenoiseInput &input, 
                                                 uint step_width, 
                                                 uint iteration, bool use_shading_normal = true, bool fused_modulation = false) noexcept;
    
    void update_resolution(uint2 resolution) noexcept;
};

}// namespace vision::svgf
