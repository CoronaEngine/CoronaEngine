#pragma once

#include "native_appearance.h"
#include <corona/resource/types/scene.h>

namespace Corona::Systems::OpticsDetail {

// Environment image and its small lighting metadata. Geometry and materials
// are consumed directly by the VBuffer and solar RSM raster passes.
struct NativeLighting {
    Horizon::HardwareBuffer data;
    Horizon::HardwareImage environment;
    std::shared_ptr<Memory::GpuMemToken> environment_memory;
    Horizon::SubmitReceipt ready;
    std::vector<Horizon::HardwareBuffer> uploads;
    bool authored_environment{};
};

class NativeLightingCache {
    struct State;
    std::shared_ptr<State> state_;
    std::shared_ptr<NativeLighting> value_;
public:
    std::shared_ptr<NativeLighting> obtain(const EnvironmentDevice& environment);
};

} // namespace Corona::Systems::OpticsDetail
