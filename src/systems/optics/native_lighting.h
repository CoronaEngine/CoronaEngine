#pragma once

#include "native_appearance.h"
#include "native_lpv_data.h"
#include <corona/resource/types/scene.h>

namespace Corona::Systems::OpticsDetail {

// Immutable scene-wide voxel geometry and local-light injection, including
// off-screen surfaces. LPV propagation runs on the GPU without ray traversal.
struct NativeLighting {
    Horizon::HardwareBuffer data;
    Horizon::HardwareImage environment;
    Memory::GpuMemToken memory;
    Memory::GpuMemToken geometry_memory;
    uint32_t triangles{}, emitters{}, directionals{}, occupied{};
    float voxel_size{};
    LpvData::Vec bounds_low{},bounds_high{};
    std::array<LpvData::Layout,LpvData::cascade_count> layouts{};
    bool authored_environment{};
};

class NativeLightingCache {
    std::size_t signature_{};
    std::shared_ptr<NativeLighting> value_;
public:
    std::shared_ptr<NativeLighting> obtain(const SceneDevice& scene,
        const EnvironmentDevice& environment, std::size_t geometry_signature,
        LpvData::Vec eye,LpvData::Vec forward);
};

} // namespace Corona::Systems::OpticsDetail
