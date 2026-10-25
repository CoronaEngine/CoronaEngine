#pragma once

#include "native_appearance.h"
#include <corona/resource/types/scene.h>

namespace Corona::Systems::OpticsDetail {

// Direct-light visibility uses the whole scene, including off-screen emitters
// and blockers. This is an immutable, CPU-built shadow acceleration structure,
// not a path tracer. Indirect transport remains screen-space SSR/SSAO.
struct NativeLighting {
    Horizon::HardwareBuffer data;
    Horizon::HardwareImage environment;
    Memory::GpuMemToken memory;
    Memory::GpuMemToken geometry_memory;
    std::vector<std::shared_ptr<NativeAppearance>> appearances;
    uint32_t triangles{}, emitters{}, directionals{};
    bool authored_environment{};
};

class NativeLightingCache {
    std::size_t signature_{};
    std::shared_ptr<NativeLighting> value_;
public:
    std::shared_ptr<NativeLighting> obtain(const SceneDevice& scene,
        const EnvironmentDevice& environment, std::size_t geometry_signature,
        NativeAppearanceCache& appearances);
};

} // namespace Corona::Systems::OpticsDetail
