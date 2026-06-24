//
// Created by Zero on 2023/5/30.
//

#pragma once

#include "dsl/dsl.h"
#include "node.h"
#include "GUI/widgets.h"
#include "sensor/frame_buffer.h"
#include "sensor/light_field_types.h"
#include "base/using.h"

namespace vision {

struct RealTimeDenoiseInput {
    /// Semantics of the two radiance channels (direct/indirect buffers). The denoiser
    /// (e.g. SVGF modulator) demodulates differently per kind, so the producer MUST set
    /// this. PathTracingIntegrator splits diffuse/specular; RealTimeIntegrator (ReSTIR)
    /// produces direct/indirect lighting. Mislabeling demodulates by the wrong albedo.
    enum class ChannelKind : uint {
        DiffuseSpecular = 0,///< direct = diffuse, indirect = specular
        DirectIndirect = 1  ///< direct = direct lighting, indirect = indirect lighting
    };

    uint2 resolution{};
    uint frame_index{};

//    BufferView<float4> output;
//    BufferView<float4> radiance;

    BufferView<RadType4> direct;
    BufferView<RadType4> indirect;

    BufferView<float2> motion_vec;

    /// Primary geometry source - other geometry data computed from this
    BufferView<TriangleHit> visibility;
    BufferView<TriangleHit> prev_visibility;

    /// Camera positions for depth calculation from visibility buffer
    array_float3 camera_pos{};
    array_float3 prev_camera_pos{};

    ChannelKind channel_kind{ChannelKind::DiffuseSpecular};
};

/// Light field specific denoising input (extends RealTimeDenoiseInput)
/// Used by SSAT denoiser for 5D phase-space reconstruction
struct LightFieldDenoiseInput : RealTimeDenoiseInput {
    // Light field parameters (use common types from light_field_types.h)
    LenticularParams lenticular{};
    LightFieldGeometry geometry{};
    
    // Transforms
    float4x4 l2w{make_float4x4(1.f)};
    float4x4 prev_l2w{make_float4x4(1.f)};
    
    /// Get subpixel resolution (width*3, height)
    [[nodiscard]] uint2 subpixel_resolution() const noexcept {
        return make_uint2(static_cast<uint>(lenticular.res_w) * 3u,
                          static_cast<uint>(lenticular.res_h));
    }
    
    /// Get total subpixels
    [[nodiscard]] uint total_subpixels() const noexcept {
        return static_cast<uint>(lenticular.res_w) * static_cast<uint>(lenticular.res_h) * 3u;
    }
    
    /// Get z_ref (focal distance = ZPP depth)
    [[nodiscard]] float z_ref() const noexcept { return geometry.d_f; }
};

class Denoiser : public Node, public GUIRenderable {
public:
    enum Mode {
        RT = 0,
        RTLightmap = 1
    };

    enum Backend {
        GPU = 0,
        CPU = 1
    };

protected:
    Mode mode_{};
    Backend backend_{};

public:
    using Desc = DenoiserDesc;

public:
    Denoiser() = default;
    explicit Denoiser(const DenoiserDesc &desc)
        : Node(desc),
          mode_(RT),
          backend_(to_upper(desc["backend"].as_string()) == "CPU" ? CPU : GPU) {}

    virtual void compile() noexcept {}

    bool render_UI(Widgets *widgets) noexcept override {
        return widgets->use_folding_header(ocarina::format("{} denoiser", impl_type().data()), [&] {
            render_sub_UI(widgets);
        });
    }

    /// for real time denoise
    virtual CommandBatch dispatch(RealTimeDenoiseInput &input) noexcept {
        return {};
    }
    
    /// For light field denoising (SSAT)
    /// Override in subclasses that support light field displays
    /// @return true if this denoiser supports light field input
    [[nodiscard]] virtual bool supports_lightfield() const noexcept { return false; }
    
    /// Dispatch light field denoising
    /// Default implementation falls back to standard dispatch
    virtual CommandBatch dispatch_lightfield(LightFieldDenoiseInput &input) noexcept {
        return dispatch(input);
    }

    /// Runtime override hook for headless evaluation and experiments.
    virtual void set_enabled(bool enabled) noexcept {}
    
    [[nodiscard]] Backend backend() const noexcept { return backend_; }

    [[nodiscard]] virtual bool enabled() noexcept {
        return false;
    }
    
    /// @brief Update internal buffers when resolution changes
    virtual void update_resolution(uint2 resolution) noexcept {}
};

}// namespace vision
