#include <corona/systems/optics/vision_pipeline_key.h>
#include <corona/systems/optics/vision_scene_resource.h>

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <unordered_map>

namespace {

using Corona::CameraVisionRenderMode;
using Corona::Systems::Vision::VisionPipelineKey;
using Corona::Systems::Vision::VisionPipelineKeyHash;
using Corona::Systems::Vision::VisionPipelineSource;
using Corona::Systems::Vision::VisionSceneResourceKey;
using Corona::Systems::Vision::VisionSceneResourceKeyHash;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void expect(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

void equivalent_keys_compare_and_hash_equal() {
    const VisionPipelineKey lhs{"scene.json",
                                CameraVisionRenderMode::SVGF,
                                VisionPipelineSource::ExternalFile};
    const VisionPipelineKey rhs{"scene.json",
                                CameraVisionRenderMode::SVGF,
                                VisionPipelineSource::ExternalFile};

    expect(lhs == rhs, "equivalent pipeline keys should compare equal");
    expect(VisionPipelineKeyHash{}(lhs) == VisionPipelineKeyHash{}(rhs),
           "equivalent pipeline keys should hash equally");
}

void mode_and_source_are_part_of_identity() {
    const VisionPipelineKey svgf{"scene.json",
                                 CameraVisionRenderMode::SVGF,
                                 VisionPipelineSource::ExternalFile};
    const VisionPipelineKey ssat{"scene.json",
                                 CameraVisionRenderMode::SSAT,
                                 VisionPipelineSource::ExternalFile};
    const VisionPipelineKey live{"scene.json",
                                 CameraVisionRenderMode::SVGF,
                                 VisionPipelineSource::ExternalLive};

    expect(!(svgf == ssat), "render mode should be part of runtime key identity");
    expect(!(svgf == live), "source type should be part of runtime key identity");

    std::unordered_map<VisionPipelineKey, int, VisionPipelineKeyHash> runtimes;
    runtimes.emplace(svgf, 1);
    runtimes.emplace(ssat, 2);
    runtimes.emplace(live, 3);
    expect(runtimes.size() == 3,
           "runtime map should keep different modes and source types separate");
}

void pt_and_ssat_runtimes_share_one_external_scene_resource_key() {
    const std::string scene_path = "d:/scene/vision_scene.json";
    const VisionPipelineKey path_tracing{scene_path,
                                         CameraVisionRenderMode::PathTracing,
                                         VisionPipelineSource::ExternalFile};
    const VisionPipelineKey ssat{scene_path,
                                 CameraVisionRenderMode::SSAT,
                                 VisionPipelineSource::ExternalFile};
    const VisionSceneResourceKey shared_scene{scene_path,
                                              VisionPipelineSource::ExternalFile};

    std::unordered_map<VisionPipelineKey, int, VisionPipelineKeyHash> runtimes;
    runtimes.emplace(path_tracing, 1);
    runtimes.emplace(ssat, 2);

    std::unordered_map<VisionSceneResourceKey,
                       int,
                       VisionSceneResourceKeyHash>
        scene_resources;
    scene_resources.emplace(shared_scene, 1);
    scene_resources[shared_scene] = 2;

    expect(runtimes.size() == 2,
           "PT and SSAT should be separate runtime keys for simultaneous rendering");
    expect(scene_resources.size() == 1,
           "PT and SSAT should share one external Vision scene resource key");
    expect(scene_resources.at(shared_scene) == 2,
           "shared scene resource slot should be reused across render modes");
}

void source_names_are_stable() {
    expect(Corona::Systems::Vision::vision_pipeline_source_name(
               VisionPipelineSource::EngineBuilt) == "engine_built",
           "engine-built source name should be stable");
    expect(Corona::Systems::Vision::vision_pipeline_source_name(
               VisionPipelineSource::ExternalFile) == "external_file",
           "external-file source name should be stable");
    expect(Corona::Systems::Vision::vision_pipeline_source_name(
               VisionPipelineSource::ExternalLive) == "external_live",
           "external-live source name should be stable");
}

}  // namespace

int main() {
    equivalent_keys_compare_and_hash_equal();
    mode_and_source_are_part_of_identity();
    pt_and_ssat_runtimes_share_one_external_scene_resource_key();
    source_names_are_stable();
    return 0;
}
