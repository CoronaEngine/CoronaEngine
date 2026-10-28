#include <corona/systems/optics/vision_pipeline_key.h>
#include <unordered_map>
#include <stdexcept>

int main() {
    using namespace Corona::Systems::Vision;
    using Corona::CameraVisionRenderMode;
    std::unordered_map<VisionPipelineKey, int, VisionPipelineKeyHash> runtimes;
    const VisionPipelineKey pt{CameraVisionRenderMode::PathTracing};
    const VisionPipelineKey restir{CameraVisionRenderMode::ReSTIR};
    runtimes[pt] = 1;
    runtimes[VisionPipelineKey{}] = 2;
    runtimes[restir] = 3;
    if (runtimes.size() != 2 || runtimes[pt] != 2 || runtimes[restir] != 3)
        throw std::runtime_error("Modes need independent runtimes with stable identity");
    return 0;
}
