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
};
}
