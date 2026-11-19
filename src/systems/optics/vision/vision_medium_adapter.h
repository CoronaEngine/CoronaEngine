#pragma once

#include <cstddef>
#include <nlohmann/json_fwd.hpp>

namespace vision {
class Scene;
class ShapeInstance;
}

namespace Corona {
struct OpticsDevice;
}

namespace Corona::Systems::Vision {

struct VisionMediaTelemetry {
    std::size_t medium_count{};
    std::size_t boundary_count{};
    bool enabled{};
};

// Configure a newly allocated logical scene after its private Geometry GPU
// resource is bound. Source definitions remain unchanged in the scene archive.
VisionMediaTelemetry configure_vision_media(::vision::Scene& scene,
    const nlohmann::json* media, bool enabled);

// Undeclared surface interfaces inherit the incident medium. An explicit pair
// of null references is a transition to vacuum and must retain that distinction.
void attach_vision_medium_interface(::vision::Scene& scene,
    ::vision::ShapeInstance& instance, const OpticsDevice& optics);

}  // namespace Corona::Systems::Vision
