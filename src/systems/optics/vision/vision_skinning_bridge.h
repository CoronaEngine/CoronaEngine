#pragma once

#include "vision_geometry_adapter.h"
#include <memory>

namespace vision { class Pipeline; }
namespace ocarina { class Device; }

namespace Corona::Systems::Vision {

class VisionSkinningBridge {
public:
    VisionSkinningBridge(ocarina::Device& device, std::vector<VisionSkinnedMesh> meshes);
    ~VisionSkinningBridge();

    // Consume the latest completed poses once before tracing any cameras.
    // Returns true if the runtime's GPU geometry changed.
    bool update(::vision::Pipeline& pipeline, bool transforms_changed);

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace Corona::Systems::Vision
