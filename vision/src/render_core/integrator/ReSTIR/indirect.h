//
// Created by Zero on 2023/9/11.
//

#pragma once

#include "common.h"
#include "radiance_storage.h"
#include "base/mgr/switch_profile.h"
#include "reservoir.h"
#include "rtxpt_gi_prefix_storage.h"
#include "rtxpt_gi_denoiser.h"
#include "base/integral/path_work_diagnostic.h"

namespace vision {
struct GIParam {
    uint camera_jitter{1u};
    uint precise_lobes{}, denoiser_di_active{};
    float denoiser_di_factor{}, denoiser_gi_factor{};
    uint max_age{};

    /// spatial
    uint spatial{1};
    uint N{};
    float s_dot{};
    float s_depth{};
    float s_radius{};

    /// temporal
    uint temporal{1};
    uint history_limit{};
    float t_dot{};
    float t_depth{};

    uint rtxpt_bias_correction{1u}, rtxpt_final_visibility{1u}, rtxpt_final_mis{1u};
    uint rtxpt_permutation{1u}, rtxpt_fallback{1u}, rtxpt_vary_age{1u}, rtxpt_boiling{1u};
    float rtxpt_boiling_strength{0.2f};
};
}// namespace vision

OC_PARAM_STRUCT(vision, GIParam, camera_jitter, precise_lobes, denoiser_di_active, denoiser_di_factor, denoiser_gi_factor, max_age, spatial, N,
                s_dot, s_depth, s_radius, temporal, history_limit,
                t_dot, t_depth, rtxpt_bias_correction, rtxpt_final_visibility, rtxpt_final_mis,
                rtxpt_permutation, rtxpt_fallback, rtxpt_vary_age, rtxpt_boiling, rtxpt_boiling_strength){};

namespace vision {

class RtxptGI;
namespace rtxpt_gi {
class NativeLightSampler;
class StandardMaterialInputs;
}

class ReSTIRGI : public ReSTIR {
private:
    SP<ScreenBuffer> radiance_{make_shared<ScreenBuffer>("ReSTIRGI::radiance_ [gi-radiance-v3-rtxpt]")};
    RegistrableBuffer<GISample> samples_{pipeline()->bindless_array()};
    mutable RegistrableBuffer<uint2> raw_radiance_{pipeline()->bindless_array()};
    bool compact_radiance_{};
    bool radiance_prepared_{};
    std::string unsupported_backend_;
    SP<RtxptGI> rtxpt_;
    RegistrableBuffer<HitBSDF> rtxpt_proposals_{pipeline()->bindless_array()};
    RegistrableBuffer<float4> rtxpt_bypass_{pipeline()->bindless_array()};
    SP<rtxpt_gi::NativeLightSampler> rtxpt_lights_;
    SP<rtxpt_gi::StandardMaterialInputs> rtxpt_materials_;
    RegistrableBuffer<RtxptGIPrefixPlane> rtxpt_prefix_planes_{pipeline()->bindless_array()};
    RegistrableBuffer<uint4> rtxpt_prefix_headers_{pipeline()->bindless_array()};
    RegistrableBuffer<float4> rtxpt_prefix_stable_{pipeline()->bindless_array()};
    RegistrableBuffer<RtxptGIPrefixAux> rtxpt_prefix_aux_{pipeline()->bindless_array()};
    Shader<void(uint, Buffer<uint>)> rtxpt_prefix_build_;
    Shader<void(uint, RtxptGICameraParam, Buffer<uint>)> rtxpt_initial_samples_;
    ocarina::Buffer<path_cost::PathWorkCounters> path_work_counters_;
    bool native_reblur_enabled_{};
    std::string backend_error_;
    mutable Buffer<float4> native_reblur_diffuse_, native_reblur_specular_, native_reblur_bypass_;
    mutable Buffer<float> native_reblur_motion_y_;

    [[nodiscard]] rtxpt_gi::PrefixStorage prefix_storage() const noexcept {
        auto &bindings = pipeline()->bindless_array();
        return {bindings.buffer_var<RtxptGIPrefixPlane>(rtxpt_prefix_planes_.index_var()),
                bindings.buffer_var<uint4>(rtxpt_prefix_headers_.index_var()),
                bindings.buffer_var<float4>(rtxpt_prefix_stable_.index_var()), Uint{pipeline()->pixel_num()}, native_reblur_storage()};
    }

    void prepare_rtxpt_candidate_buffers() noexcept;
    void prepare_native_reblur_buffers() noexcept;

    [[nodiscard]] static TSampler &sampler() noexcept { return renderer().sampler(); }
    void compile_initial_samples() noexcept;

public:
    ReSTIRGI() = default;
    ReSTIRGI(IntegratorPtr integrator, const ParameterSet &desc);
    void restore(RuntimeObject *old_obj) noexcept override;
    OC_MAKE_MEMBER_GETTER(open, )
    OC_MAKE_MEMBER_GETTER(radiance, &)
    [[nodiscard]] float factor() const noexcept { return open() ? factor_ : 0.f; }
    void prepare(bool compact_radiance = false) noexcept;
    void set_compact_radiance(bool compact) noexcept;
    [[nodiscard]] bool compact_radiance() const noexcept { return compact_radiance_; }
    [[nodiscard]] const RegistrableBuffer<uint2> &raw_radiance() const noexcept { return raw_radiance_; }
    [[nodiscard]] Float4 read_radiance(const Uint &index) const noexcept {
        return compact_radiance_ ? unpack_restir_radiance(raw_radiance_.read(index)) : radiance_->read(index);
    }
    void write_radiance(const Uint &index, const Float4 &value) const noexcept {
        if (compact_radiance_) raw_radiance_.write(index, pack_restir_radiance(value));
        else radiance_->write(index, value);
    }
    [[nodiscard]] vector<float4> download_radiance() const noexcept;
    void download_storage(vector<GISample> &samples, vector<GIReservoir> &reservoirs) const noexcept;
    bool render_UI(Widgets *widgets) noexcept override;
    void render_sub_UI(Widgets *widgets) noexcept override;
    void compile(bool refresh_lighting = false) noexcept;
    [[nodiscard]] const char *backend_incompatibility() const noexcept {
        return backend_error_.empty() ? nullptr : backend_error_.c_str();
    }
    // Caller drains the stream and activates this scene's GPU context before a
    // mode switch. An enabled switch recompiles only private Build/Fill/Final.
    bool set_native_reblur_enabled(bool enabled) noexcept;
    [[nodiscard]] bool native_reblur_enabled() const noexcept { return native_reblur_enabled_; }
    [[nodiscard]] BufferView<float4> native_reblur_diffuse(uint layer) const noexcept {
        return native_reblur_diffuse_.view(size_t(layer) * pipeline()->pixel_num(), pipeline()->pixel_num());
    }
    [[nodiscard]] BufferView<float4> native_reblur_specular(uint layer) const noexcept {
        return native_reblur_specular_.view(size_t(layer) * pipeline()->pixel_num(), pipeline()->pixel_num());
    }
    [[nodiscard]] BufferView<float4> native_reblur_bypass() const noexcept { return native_reblur_bypass_.view(); }
    [[nodiscard]] BufferView<std::byte> native_reblur_prefix() const noexcept {
        return rtxpt_prefix_planes_.view().view_as<std::byte>();
    }
    [[nodiscard]] BufferView<uint4> native_reblur_headers() const noexcept {
        return rtxpt_prefix_headers_.view();
    }
    [[nodiscard]] BufferView<float> native_reblur_motion_y() const noexcept {
        return native_reblur_motion_y_.view();
    }
    [[nodiscard]] rtxpt_gi::NativeReblurStorage native_reblur_storage() const noexcept {
        return native_reblur_enabled_ ? rtxpt_gi::NativeReblurStorage{&native_reblur_diffuse_, &native_reblur_specular_,
            &native_reblur_bypass_, nullptr, &native_reblur_motion_y_, pipeline()->pixel_num()} : rtxpt_gi::NativeReblurStorage{};
    }
    [[nodiscard]] const RegistrableBuffer<GISample> &sample_buffer() const noexcept { return samples_; }
    [[nodiscard]] const RegistrableBuffer<HitBSDF> &rtxpt_proposals() const noexcept { return rtxpt_proposals_; }
    [[nodiscard]] const RegistrableBuffer<float4> &rtxpt_bypass() const noexcept { return rtxpt_bypass_; }
    [[nodiscard]] const rtxpt_gi::NativeLightSampler &rtxpt_lights() const noexcept { return *rtxpt_lights_; }
    [[nodiscard]] rtxpt_gi::StandardMaterialInputs &rtxpt_materials() const noexcept { return *rtxpt_materials_; }
    [[nodiscard]] const RegistrableBuffer<RtxptGIPrefixAux> &rtxpt_prefix_aux() const noexcept { return rtxpt_prefix_aux_; }
    void update_resolution(ocarina::uint2 res) noexcept override;
    [[nodiscard]] GIParam construct_param() const noexcept;
    [[nodiscard]] CommandBatch dispatch(uint frame_index, bool precise_lobes, BufferView<uint> primary_flags,
                                        float di_factor, float gi_factor, bool di_active) const noexcept;
};
}// namespace vision
