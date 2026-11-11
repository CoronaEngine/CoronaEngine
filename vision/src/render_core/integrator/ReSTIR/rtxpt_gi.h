#pragma once

#include "common.h"
#include "reservoir.h"

namespace vision {
class ReSTIRGI;
struct GIParam;

// RTXPT's SDK GI ABI: two 32-byte reservoir slices, independent of legacy GI44.
struct RtxptGIStorage {
    array<uint, 8> words{};
};
static_assert(sizeof(RtxptGIStorage) == 32u);
struct RtxptGIReservoir {
    GISample sample{};
    float weight{};
    uint M{}, age{};
};
}// namespace vision

OC_STRUCT(vision, RtxptGIStorage, words) {};
OC_STRUCT(vision, RtxptGIReservoir, sample, weight, M, age) {};

namespace vision::rtxpt_gi {
[[nodiscard]] Uint encode_radiance(const Float3 &rgb) noexcept;
[[nodiscard]] Float3 decode_radiance(const Uint &packed) noexcept;
[[nodiscard]] Uint encode_normal(const Float3 &normal) noexcept;
[[nodiscard]] Float3 decode_normal(const Uint &packed) noexcept;
[[nodiscard]] RtxptGIStorageVar pack(const RtxptGIReservoirVar &r) noexcept;
[[nodiscard]] RtxptGIReservoirVar unpack(const RtxptGIStorageVar &r) noexcept;
[[nodiscard]] Float jacobian(const Float3 &receiver, const Float3 &source,
                             const Float3 &position, const Float3 &normal) noexcept;
[[nodiscard]] Bool validate_jacobian(Float &value) noexcept;
[[nodiscard]] Bool combine(RtxptGIReservoirVar &out, const RtxptGIReservoirVar &source,
                           const Float &random, const Float &target) noexcept;
void finalize(RtxptGIReservoirVar &r, const Float &numerator, const Float &denominator) noexcept;
[[nodiscard]] Float3 depth_guide_position(const Float3 &physical, const Float3 &virtual_position,
                                         const Float3 &depth_position, const Bool &use_virtual,
                                         const Bool &approximate) noexcept;
[[nodiscard]] Float mis_initial_weight(const Float3 &true_f, const Float3 &rough_f) noexcept;
}// namespace vision::rtxpt_gi

namespace vision {
// Uses Corona's existing PT candidates, material evaluators, receiver history and
// denoiser outputs. Only reservoir selection/reconnection follows native RTXPT.
class RtxptGI final : private Toolkit {
    ReSTIRGI *owner_;
    RegistrableBuffer<RtxptGIStorage> reservoirs_{pipeline()->bindless_array()};
    RegistrableBuffer<float2> neighbors_{pipeline()->bindless_array()};
    Shader<void(GIParam, uint, uint2)> temporal_, spatial_, final_;
    uint2 extent_{};
    mutable uint previous_frame_{InvalidUI32}, previous_jitter_{InvalidUI32};

    [[nodiscard]] RtxptGIReservoirVar read(const Uint &index, uint slice) const noexcept;
    void write(const Uint &index, uint slice, const RtxptGIReservoirVar &r) const noexcept;
    [[nodiscard]] RtxptGIReservoirVar candidate(const Uint &index,
                                               const Interaction &receiver) const noexcept;
    [[nodiscard]] Float target(const Interaction &receiver, const RtxptGIReservoirVar &r,
                               MaterialEvaluator *bsdf = nullptr) const noexcept;
    [[nodiscard]] Bool similar(const SurfaceDataVar &a, const SurfaceDataVar &b,
                               const Float &normal_threshold, const Float &depth_threshold,
                               const Float &expected_depth, const Bool &fallback = false) const noexcept;
    [[nodiscard]] Interaction interaction(const SurfaceDataVar &s, const Uint &index,
                                         bool previous = false) const noexcept;
    void initialize(const Uint &frame, const Uint2 &pixel) const noexcept;
    void compile_temporal() noexcept;
    void compile_spatial() noexcept;
    void compile_final() noexcept;

public:
    explicit RtxptGI(ReSTIRGI &owner) noexcept : owner_(&owner) {}
    void rebind_owner(ReSTIRGI &owner) noexcept { owner_ = &owner; }
    void prepare() noexcept;
    void update_resolution(uint2 resolution) noexcept;
    void reset_history() noexcept;
    void compile() noexcept;
    void download_storage(vector<GISample> &samples, vector<GIReservoir> &reservoirs) const noexcept;
    [[nodiscard]] CommandBatch dispatch(uint frame, GIParam param) const noexcept;
};
}// namespace vision
