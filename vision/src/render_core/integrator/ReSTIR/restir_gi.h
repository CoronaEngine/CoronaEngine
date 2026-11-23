#pragma once

#include "base/integral/path_work_diagnostic.h"
#include "base/mgr/switch_profile.h"
#include "common.h"
#include "radiance_storage.h"
#include "restir_gi_denoiser.h"
#include "reservoir.h"
#include "restir_gi_pt.h"
#include "restir_gi_prefix_storage.h"

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

    uint restir_gi_bias_correction{1u}, restir_gi_final_visibility{1u}, restir_gi_final_mis{1u};
    uint restir_gi_permutation{1u}, restir_gi_fallback{1u}, restir_gi_vary_age{1u}, restir_gi_boiling{1u};
    float restir_gi_boiling_strength{0.2f};
};

// Each of the two reservoir slices stores 32 bytes per pixel.
struct ReSTIRGIStorage {
    array<uint, 8> words{};
};
static_assert(sizeof(ReSTIRGIStorage) == 32u);
struct ReSTIRGIReservoir {
    GISample sample{};
    float weight{};
    uint M{}, age{};
};
}// namespace vision

OC_PARAM_STRUCT(vision, GIParam, camera_jitter, precise_lobes, denoiser_di_active, denoiser_di_factor, denoiser_gi_factor, max_age, spatial, N,
                s_dot, s_depth, s_radius, temporal, history_limit,
                t_dot, t_depth, restir_gi_bias_correction, restir_gi_final_visibility, restir_gi_final_mis,
                restir_gi_permutation, restir_gi_fallback, restir_gi_vary_age, restir_gi_boiling, restir_gi_boiling_strength){};

OC_STRUCT(vision, ReSTIRGIStorage, words) {};
OC_STRUCT(vision, ReSTIRGIReservoir, sample, weight, M, age) {};

namespace vision::restir_gi {
class NativeLightSampler;
class StandardMaterialInputs;

[[nodiscard]] Uint encode_radiance(const Float3 &rgb) noexcept;
[[nodiscard]] Float3 decode_radiance(const Uint &packed) noexcept;
[[nodiscard]] Uint encode_normal(const Float3 &normal) noexcept;
[[nodiscard]] Float3 decode_normal(const Uint &packed) noexcept;
[[nodiscard]] ReSTIRGIStorageVar pack(const ReSTIRGIReservoirVar &r) noexcept;
[[nodiscard]] ReSTIRGIReservoirVar unpack(const ReSTIRGIStorageVar &r) noexcept;
[[nodiscard]] Float jacobian(const Float3 &receiver, const Float3 &source,
                             const Float3 &position, const Float3 &normal) noexcept;
[[nodiscard]] Bool validate_jacobian(Float &value) noexcept;
[[nodiscard]] Bool combine(ReSTIRGIReservoirVar &out, const ReSTIRGIReservoirVar &source,
                           const Float &random, const Float &target) noexcept;
void finalize(ReSTIRGIReservoirVar &r, const Float &numerator, const Float &denominator) noexcept;
[[nodiscard]] Float mis_initial_weight(const Float3 &true_f, const Float3 &rough_f) noexcept;
}// namespace vision::restir_gi

namespace vision {
class ReSTIRGI final : public ReSTIR {
    SP<ScreenBuffer> radiance_{make_shared<ScreenBuffer>("ReSTIRGI::radiance_ [restir-gi-runtime-v4]")};
    RegistrableBuffer<GISample> samples_{pipeline()->bindless_array()};
    mutable RegistrableBuffer<uint2> raw_radiance_{pipeline()->bindless_array()};
    bool compact_radiance_{};
    bool radiance_prepared_{};
    std::string unsupported_backend_;
    RegistrableBuffer<HitBSDF> restir_gi_proposals_{pipeline()->bindless_array()};
    RegistrableBuffer<float4> restir_gi_bypass_{pipeline()->bindless_array()};
    SP<restir_gi::NativeLightSampler> restir_gi_lights_;
    SP<restir_gi::StandardMaterialInputs> restir_gi_materials_;
    RegistrableBuffer<ReSTIRGIPrefixPlane> restir_gi_prefix_planes_{pipeline()->bindless_array()};
    RegistrableBuffer<uint4> restir_gi_prefix_headers_{pipeline()->bindless_array()};
    RegistrableBuffer<float4> restir_gi_prefix_stable_{pipeline()->bindless_array()};
    RegistrableBuffer<ReSTIRGIPrefixAux> restir_gi_prefix_aux_{pipeline()->bindless_array()};
    Shader<void(uint, Buffer<uint>)> restir_gi_prefix_build_;
    Shader<void(uint, ReSTIRGICameraParam, Buffer<uint>)> restir_gi_initial_samples_;
    ocarina::Buffer<path_cost::PathWorkCounters> path_work_counters_;
    bool native_reblur_enabled_{};
    std::string backend_error_;
    mutable Buffer<float4> native_reblur_diffuse_, native_reblur_specular_, native_reblur_bypass_;
    mutable Buffer<float> native_reblur_motion_y_;

    RegistrableBuffer<ReSTIRGIStorage> reservoirs_{pipeline()->bindless_array()};
    RegistrableBuffer<float2> neighbors_{pipeline()->bindless_array()};
    RegistrableBuffer<ReSTIRGIPTSurface> surfaces_{pipeline()->bindless_array()};
    RegistrableBuffer<ReSTIRGIPTReference> references_{pipeline()->bindless_array()};
    Shader<void(GIParam, uint, uint2)> temporal_shader_;
    Shader<void(GIParam, uint, uint2)> spatial_shader_, final_shader_;
    Shader<void(uint)> pt_validation_;
    Buffer<float4> pt_validation_output_;
    mutable bool pt_validation_pending_{};
    uint2 extent_{};
    mutable uint previous_frame_{InvalidUI32}, previous_jitter_{InvalidUI32};

    [[nodiscard]] restir_gi::PrefixStorage prefix_storage() const noexcept {
        auto &bindings = pipeline()->bindless_array();
        return {bindings.buffer_var<ReSTIRGIPrefixPlane>(restir_gi_prefix_planes_.index_var()),
                bindings.buffer_var<uint4>(restir_gi_prefix_headers_.index_var()),
                bindings.buffer_var<float4>(restir_gi_prefix_stable_.index_var()), Uint{pipeline()->pixel_num()}, native_reblur_storage()};
    }

    void prepare_restir_gi_candidate_buffers() noexcept;
    void prepare_native_reblur_buffers() noexcept;

    [[nodiscard]] static TSampler &sampler() noexcept { return renderer().sampler(); }
    void compile_initial_samples() noexcept;
    void prepare_reuse_buffers() noexcept;
    void resize_reuse_buffers(uint2 resolution) noexcept;
    void reset_history() noexcept;
    void compile_denoiser_passes() noexcept;
    void write_pt_surface(const Uint &index, const restir_gi::PTSurfaceView &surface) const noexcept;
    [[nodiscard]] GIParam construct_param() const noexcept;
    [[nodiscard]] CommandBatch dispatch_reuse(uint frame, GIParam param) const noexcept;

    [[nodiscard]] ReSTIRGIReservoirVar read(const Uint &index, uint slice) const noexcept;
    void write(const Uint &index, uint slice, const ReSTIRGIReservoirVar &r) const noexcept;
    [[nodiscard]] ReSTIRGIReservoirVar candidate(const Uint &index,
                                               const Interaction &receiver) const noexcept;
    [[nodiscard]] Float target(const restir_gi::PTSurfaceView &receiver, const ReSTIRGIReservoirVar &r) const noexcept;
    [[nodiscard]] Bool similar(const restir_gi::PTSurfaceView &a, const restir_gi::PTSurfaceView &b,
                               const Float &normal_threshold, const Float &depth_threshold,
                               const Float &expected_depth, const Bool &fallback = false) const noexcept;
    [[nodiscard]] restir_gi::PTSurfaceView read_pt_surface(const Uint &index, const Uint &frame, bool previous = false) const noexcept;
    void initialize(const Uint &frame, const Uint2 &pixel) const noexcept;
    void compile_temporal() noexcept;
    void compile_spatial() noexcept;
    void compile_final() noexcept;
    void compile_pt_validation() noexcept;

public:
    ReSTIRGI() = default;
    ReSTIRGI(IntegratorPtr integrator, const ParameterSet &desc);
    void restore(RuntimeObject *old_obj) noexcept override;
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
        return restir_gi_prefix_planes_.view().view_as<std::byte>();
    }
    [[nodiscard]] BufferView<uint4> native_reblur_headers() const noexcept {
        return restir_gi_prefix_headers_.view();
    }
    [[nodiscard]] BufferView<float> native_reblur_motion_y() const noexcept {
        return native_reblur_motion_y_.view();
    }
    [[nodiscard]] restir_gi::NativeReblurStorage native_reblur_storage() const noexcept {
        return native_reblur_enabled_ ? restir_gi::NativeReblurStorage{&native_reblur_diffuse_, &native_reblur_specular_,
            &native_reblur_bypass_, &native_reblur_motion_y_, pipeline()->pixel_num()} : restir_gi::NativeReblurStorage{};
    }
    [[nodiscard]] const restir_gi::NativeLightSampler &restir_gi_lights() const noexcept { return *restir_gi_lights_; }
    [[nodiscard]] restir_gi::StandardMaterialInputs &restir_gi_materials() const noexcept { return *restir_gi_materials_; }
    void update_resolution(ocarina::uint2 res) noexcept override;
    [[nodiscard]] CommandBatch dispatch(uint frame_index, bool precise_lobes, BufferView<uint> primary_flags,
                                        float di_factor, float gi_factor, bool di_active) const noexcept;
};
}// namespace vision
