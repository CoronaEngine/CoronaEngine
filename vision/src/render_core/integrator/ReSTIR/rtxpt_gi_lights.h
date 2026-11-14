#pragma once

#include "common.h"

namespace vision {
class ReSTIRGI;
}

namespace vision::rtxpt_gi {

// GI-private native lighting boundary. Radiance is Le, never Le / pdf.
struct NativeLightSample {
    Float3 direction{make_float3(0.f)}, radiance{make_float3(0.f)};
    Float3 position{make_float3(0.f)}, normal{make_float3(0.f)};
    Float distance{0.f}, solid_angle_pdf{0.f}, selection_pdf{0.f};
    Uint light_index{InvalidUI32};
    Bool valid{false}, sampleable_by_bsdf{false};
};

struct NativeEmission {
    Float3 radiance{make_float3(0.f)};
    Float light_pdf{0.f}; // Selection PMF times conditional solid-angle PDF.
    Uint light_index{InvalidUI32};
};

// Standard GI's 1/1 global-proxy lighting path. This does not change the scene's
// LightSampler, Falcor DI resources, ordinary PT or their random streams.
class NativeLightSampler final : private Toolkit {
    static constexpr uint environment_resolution = 1024u;
    static constexpr uint environment_light_count = 4096u;
    ReSTIRGI *owner_{};
    uint2 extent_{};
    uint light_count_{}, triangle_count_{};
    bool has_environment_{}, prepared_{}, initialized_{}, feedback_enabled_{};
    uint previous_frame_{InvalidUI32};
    Buffer<uint4> primitive_ids_, lights_, environment_nodes_;
    Buffer<uint> instance_bases_, proxy_counts_, proxy_indices_, proxy_offsets_, control_;
    Buffer<float> weights_, weight_block_sums_;
    Buffer<uint> proxy_group_sums_, proxy_group_bases_;
    Buffer<uint4> proxy_tasks_;
    mutable RegistrableBuffer<uint2> feedback_{pipeline()->bindless_array()};
    Buffer<uint> feedback_counts_;
    Buffer<uint2> environment_cube_;
    Buffer<float4> environment_mips_;
    Shader<void()> bake_environment_, bake_cube_, bake_triangles_, bake_environment_lights_, build_proxies_, fill_proxies_;
    Shader<void()> compute_weights_, reduce_weight_sum_, prefix_proxies_, create_proxy_jobs_;
    vector<Shader<void()>> environment_reduce_, cube_reduce_;
    Shader<void()> reset_feedback_counts_;
    Shader<void(uint2, uint)> process_feedback_, clear_feedback_;
    Shader<void(uint)> update_feedback_state_;

    void compile_environment();
    void compile_triangles();
    void compile_environment_lights();
    void compile_proxies();
    void compile_feedback();
    void build_environment_nodes();
    [[nodiscard]] Float3 environment_radiance(const Float3 &world_dir, const Float &mip) const noexcept;

public:
    explicit NativeLightSampler(ReSTIRGI &owner) noexcept : owner_(&owner) {}
    void rebind_owner(ReSTIRGI &owner) noexcept { owner_ = &owner; }
    void prepare();
    void compile();
    void update_resolution(uint2 resolution) noexcept;
    [[nodiscard]] CommandBatch dispatch(uint frame);
    [[nodiscard]] NativeLightSample sample(const Float3 &receiver, const Float &u_select,
                                           const Float2 &u_shape) const noexcept;
    [[nodiscard]] NativeEmission environment(const Float3 &world_dir,
                                             const Uint &diffuse_bounces = 0u) const noexcept;
    [[nodiscard]] NativeEmission emissive(const Interaction &hit,
                                          const Float3 &receiver) const noexcept;
    [[nodiscard]] Float mis_from_light(const NativeLightSample &light,
                                        const Float &bsdf_pdf) const noexcept;
    [[nodiscard]] Float mis_from_bsdf(const Float &mixture_light_pdf,
                                       const Float &bsdf_pdf) const noexcept;
    [[nodiscard]] uint light_count() const noexcept { return light_count_; }
    [[nodiscard]] bool temporal_feedback_enabled() const noexcept { return feedback_enabled_; }
    void record_nee(const Uint2 &pixel, const Bool &indirect, const NativeLightSample &light,
                    const Float &pixel_contribution_average, const Float &random) const noexcept;
    void record_bsdf(const Uint2 &pixel, const Bool &indirect, const Uint &light_index,
                     const Float &pixel_average_without_mis, const Float &bsdf_mis,
                     const Float &random) const noexcept;
    // Explicit validation readback only; never called by the render loop.
    void download_validation(const fs::path &directory) const;
};
}// namespace vision::rtxpt_gi
