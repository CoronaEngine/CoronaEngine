#pragma once

#include "filter.h"
#include "rhi/command.h"
#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <vector>

namespace nrd { struct CommonSettings; struct ReblurSettings; }

namespace vision::reblur {

// Diagnostic replay of the exact native-format NRD boundary. Input order:
// MV, normal/roughness, viewZ, diffuse, specular, disocclusion-threshold mix.
using PackedNrdInputs = std::array<std::vector<std::byte>, 6>;
using PackedNrdOutputs = std::array<std::vector<std::byte>, 2>;

// NRD expects non-jittered matrices, column-major storage and column vectors.
// Keep the camera interface separate from FilterParam's raster transforms.
struct NativeCameraParams {
    std::array<float, 16> world_to_view{};
    std::array<float, 16> view_to_clip{};
    std::array<float, 16> world_to_view_prev{};
    std::array<float, 16> view_to_clip_prev{};
    std::array<float, 2> jitter{}, jitter_prev{};
    float frame_delta_ms{1000.f / 60.f};
    float disocclusion_threshold{.01f};
    float disocclusion_threshold_alternate{.1f};
    float denoising_range{100000.f};
};

// Original NRD 4.14.3 backend, with independent history for each render layer.
class NativeReblur {
    struct Impl;
    std::shared_ptr<Impl> impl_;

public:
    explicit NativeReblur(std::filesystem::path shader_directory);
    ~NativeReblur();
    NativeReblur(const NativeReblur &) = delete;
    NativeReblur &operator=(const NativeReblur &) = delete;
    NativeReblur(NativeReblur &&) noexcept = default;
    NativeReblur &operator=(NativeReblur &&) noexcept = default;

    // REBLUR_NATIVE_NRD=0 selects the original-algorithm EDSL implementation.
    [[nodiscard]] static bool requested() noexcept;
    // Capture the caller's resource owners in the containing command batch as
    // usual; device addresses in FilterParam are borrowed, not owning handles.
    [[nodiscard]] ocarina::Command *dispatch(FilterParam parameters,
                                             NativeCameraParams camera,
                                             uint layer,
                                             Settings settings,
                                             bool reset_history = false,
                                             std::shared_ptr<void> resource_owner = {});
    // Transfers run separately from the timed NRD dispatch. Each replay plane
    // owns a separate identifier-0 NRD instance, matching RTXPT.
    [[nodiscard]] ocarina::Command *upload_replay_inputs(uint2 size, uint layer,
                                                        std::shared_ptr<const PackedNrdInputs> inputs);
    [[nodiscard]] ocarina::Command *replay_dispatch(uint2 size, uint layer,
                                                   const nrd::CommonSettings &common,
                                                   const nrd::ReblurSettings &settings);
    [[nodiscard]] ocarina::Command *readback_replay_outputs(uint2 size, uint layer,
                                                           std::shared_ptr<PackedNrdOutputs> outputs);
    // Diagnostic common frontend: exact six-float4 SoA input, not Guide data.
    // Uploads/readbacks stay outside the separately profiled conversion call.
    [[nodiscard]] ocarina::Command *upload_canonical_inputs(uint2 size, uint layer,
        std::shared_ptr<const std::vector<std::byte>> inputs, std::filesystem::path frontend_spirv,
        std::shared_ptr<const PackedNrdOutputs> nrd_outputs = {}, std::filesystem::path backend_spirv = {});
    [[nodiscard]] ocarina::Command *dispatch_canonical_conversion(uint2 size, uint layer,
        float4 hit_distance_parameters, float clamp_max);
    [[nodiscard]] ocarina::Command *readback_replay_inputs(uint2 size, uint layer,
        std::shared_ptr<PackedNrdInputs> inputs);
    [[nodiscard]] ocarina::Command *readback_canonical_outputs(uint2 size, uint layer,
        std::shared_ptr<PackedNrdOutputs> outputs);
    // Diagnostic-only linear-clamp sampling oracle. Writes 256 float4 values;
    // the caller retains the borrowed output allocation through completion.
    // probe_spirv uses the same Vulkan binding offsets as the native shaders.
    [[nodiscard]] ocarina::Command *sampling_probe(ocarina::BufferDesc<float4> output,
                                                   std::filesystem::path probe_spirv);
    // Diagnostic-only native image-store conversion oracle. Writes the exact
    // input float4 bytes and tightly packed rgb10a2/rgba16f/r16f/r8 textures to
    // output_prefix + "-<format>.bin"; the callback waits for these readbacks.
    [[nodiscard]] ocarina::Command *format_store_probe(std::vector<float4> samples,
                                                       std::filesystem::path probe_spirv,
                                                       std::filesystem::path output_prefix);
};

}// namespace vision::reblur
