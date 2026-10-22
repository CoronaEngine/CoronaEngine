#pragma once
#include "native_nrd.h"

namespace vision::reblur {
// NRD supplies CPU constants/resource scheduling; every GPU pass uses EDSL.
class EdslReblur {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit EdslReblur(Device &device);
    ~EdslReblur();
    EdslReblur(const EdslReblur &) = delete;
    EdslReblur &operator=(const EdslReblur &) = delete;
    [[nodiscard]] CommandBatch dispatch(FilterParam parameters, const NativeCameraParams &camera,
                                         uint layer, const Settings &settings, bool reset_history = false);
    // Diagnostic packed-input replay: transfers are separate from the NRD GPU
    // interval, and every plane owns an independent NRD instance (identifier 0).
    [[nodiscard]] CommandBatch upload_replay_inputs(uint2 size, uint layer,
                                                    std::shared_ptr<const PackedNrdInputs> inputs);
    [[nodiscard]] CommandBatch replay_dispatch(uint2 size, uint layer,
                                               const nrd::CommonSettings &common,
                                               const nrd::ReblurSettings &settings);
    [[nodiscard]] CommandBatch readback_replay_outputs(uint2 size, uint layer,
                                                      std::shared_ptr<PackedNrdOutputs> outputs);
};
}
