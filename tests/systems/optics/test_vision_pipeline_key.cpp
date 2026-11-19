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
    const VisionPipelineKey media_pt{CameraVisionRenderMode::PathTracing, true, {}, 1};
    const VisionPipelineKey edited_media_pt{CameraVisionRenderMode::PathTracing, true, {}, 2};
    runtimes[media_pt] = 4;
    runtimes[edited_media_pt] = 5;
    if (runtimes.size() != 4 || runtimes[pt] != 2 || runtimes[media_pt] != 4)
        throw std::runtime_error("Media policy and source revision need independent runtimes");
    const auto mode = CameraVisionRenderMode::PathTracing;
    if (!vision_media_enabled(mode, true, false, true) ||
        !vision_media_enabled(CameraVisionRenderMode::ProgressivePathTracing, true, false, true) ||
        vision_media_enabled(mode, false, false, true) ||
        vision_media_enabled(mode, true, true, true) ||
        vision_media_enabled(mode, true, false, false) ||
        vision_media_enabled(CameraVisionRenderMode::ReSTIR, true, false, true) ||
        vision_media_enabled(CameraVisionRenderMode::SVGF, true, false, true))
        throw std::runtime_error("Only explicitly enabled accumulated pure PT may use media");
    return 0;
}
