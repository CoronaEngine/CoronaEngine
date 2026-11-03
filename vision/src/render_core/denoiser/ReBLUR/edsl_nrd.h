#pragma once
#include "edsl_runtime.h"
#include "scheduler/include/NRD.h"
#include "filter.h"

namespace vision::reblur {
namespace edsl {
struct Dispatch;
struct ComposeParam;
}
// Repository-local CPU scheduling and Ocarina EDSL GPU kernels.
class EdslReblur {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    using PackCommand = std::function<ocarina::Command *(const edsl::Dispatch &)>;
    explicit EdslReblur(Device &device, bool compact_guide = false);
    ~EdslReblur();
    EdslReblur(const EdslReblur &) = delete;
    EdslReblur &operator=(const EdslReblur &) = delete;
    [[nodiscard]] CommandBatch dispatch(FilterParam parameters, const NativeCameraParams &camera,
                                         uint layer, const Settings &settings, bool reset_history = false,
                                         const PackCommand &prepare_and_pack = {},
                                         const edsl::ComposeParam *composition = nullptr, bool defer_unpack = false);
    // Diagnostic packed-input replay: transfers are separate from the filtering GPU
    // interval, and every plane owns an independent ReBLUR instance (identifier 0).
    [[nodiscard]] CommandBatch upload_replay_inputs(uint2 size, uint layer,
                                                    std::shared_ptr<const PackedNrdInputs> inputs);
    [[nodiscard]] CommandBatch replay_dispatch(uint2 size, uint layer,
                                               const nrd::CommonSettings &common,
                                               const nrd::ReblurSettings &settings);
    [[nodiscard]] CommandBatch readback_replay_outputs(uint2 size, uint layer,
                                                      std::shared_ptr<PackedNrdOutputs> outputs);
};
}
