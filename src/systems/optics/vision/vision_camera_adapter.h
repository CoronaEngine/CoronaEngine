#pragma once

namespace vision {
class Pipeline;
}

namespace Corona {
struct CameraDevice;
}

namespace Corona::Systems::Vision {

void sync_vision_camera(::vision::Pipeline& pipeline, const CameraDevice& camera);

}  // namespace Corona::Systems::Vision
