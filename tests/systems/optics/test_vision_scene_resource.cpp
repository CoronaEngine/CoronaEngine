#include <corona/systems/optics/vision_scene_resource.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>
#include <unordered_map>

namespace {

using Corona::Systems::Vision::VisionLogicalInstanceKey;
using Corona::Systems::Vision::VisionLogicalInstanceRecord;
using Corona::Systems::Vision::VisionSceneResource;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void expect(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}





void scene_gpu_resource_lives_on_scene_resource() {
    VisionSceneResource resource;
    expect(!resource.has_scene_gpu_resource(),
           "fresh scene resource should not own a scene GPU resource");
    expect(resource.scene_gpu_resource_identity() == 0u,
           "fresh scene resource GPU resource identity should be empty");

    auto* fake_gpu_resource =
        reinterpret_cast<::vision::GeometryGpuResource*>(0x1000);
    std::shared_ptr<::vision::GeometryGpuResource> shared_gpu_resource(
        fake_gpu_resource,
        [](::vision::GeometryGpuResource*) {});
    resource.set_scene_gpu_resource(shared_gpu_resource);

    expect(resource.has_scene_gpu_resource(),
           "scene GPU resource should be stored on VisionSceneResource");
    expect(resource.scene_gpu_resource_identity() ==
               reinterpret_cast<std::uintptr_t>(fake_gpu_resource),
           "scene GPU resource identity should be the shared resource pointer");
    expect(resource.scene_gpu_resource == shared_gpu_resource,
           "scene GPU resource should reuse the supplied shared pointer");

    resource.set_scene_gpu_resource({});
    expect(!resource.has_scene_gpu_resource(),
           "scene GPU resource should be clearable when the scene resource is released");
}

void logical_scene_lives_on_scene_resource() {
    VisionSceneResource resource;
    expect(!resource.has_logical_scene(),
           "fresh scene resource should not own a logical scene");
    expect(resource.logical_scene_identity() == 0u,
           "fresh logical scene identity should be empty");

    auto* fake_scene = reinterpret_cast<::vision::SceneData*>(0x1800);
    std::shared_ptr<::vision::SceneData> shared_scene(
        fake_scene,
        [](::vision::SceneData*) {});
    resource.set_logical_scene(shared_scene);

    expect(resource.has_logical_scene(),
           "logical scene should be stored on VisionSceneResource");
    expect(resource.logical_scene_identity() ==
               reinterpret_cast<std::uintptr_t>(fake_scene),
           "logical scene identity should be the shared SceneData pointer");
    expect(resource.logical_scene == shared_scene,
           "VisionSceneResource should reuse the supplied logical scene pointer");
}

void logical_scene_is_created_once_per_scene_resource() {
    VisionSceneResource resource;
    int factory_calls = 0;
    auto* fake_scene = reinterpret_cast<::vision::SceneData*>(0x2800);
    auto factory = [&]() {
        ++factory_calls;
        return std::shared_ptr<::vision::SceneData>(
            fake_scene,
            [](::vision::SceneData*) {});
    };

    auto first = resource.ensure_logical_scene(factory);
    auto second = resource.ensure_logical_scene(factory);

    expect(factory_calls == 1,
           "logical scene factory should run only once per VisionSceneResource");
    expect(first == second,
           "same VisionSceneResource should return the same logical scene");
    expect(resource.logical_scene == first,
           "VisionSceneResource should retain the shared logical scene");
}

void scene_gpu_resource_is_created_once_per_scene_resource() {
    VisionSceneResource resource;
    int factory_calls = 0;
    auto* fake_gpu_resource =
        reinterpret_cast<::vision::GeometryGpuResource*>(0x2000);
    auto factory = [&]() {
        ++factory_calls;
        return std::shared_ptr<::vision::GeometryGpuResource>(
            fake_gpu_resource,
            [](::vision::GeometryGpuResource*) {});
    };

    auto first = resource.ensure_scene_gpu_resource(factory);
    auto second = resource.ensure_scene_gpu_resource(factory);

    expect(factory_calls == 1,
           "scene GPU resource factory should run only once per VisionSceneResource");
    expect(first == second,
           "same VisionSceneResource should return the same scene GPU resource");
    expect(resource.scene_gpu_resource == first,
           "VisionSceneResource should retain the shared scene GPU resource");
}









}  // namespace

int main() {
    scene_gpu_resource_lives_on_scene_resource();
    logical_scene_lives_on_scene_resource();
    logical_scene_is_created_once_per_scene_resource();
    scene_gpu_resource_is_created_once_per_scene_resource();
    VisionSceneResource resource;
    resource.upsert_logical_instance({{1, 2}, 42, 123, {}});
    expect(resource.find_logical_instance({1, 2})->actor_handle == 42, "logical instance identity survives updates");
    resource.mark_transforms_changed();
    expect(resource.scene_gpu_needs_transform_upload(), "edited transforms must be uploaded");
    resource.mark_scene_gpu_transforms_uploaded();
    expect(!resource.scene_gpu_needs_transform_upload(), "uploaded transforms must not upload again");
    resource.reset_loaded_scene();
    expect(resource.logical_instance_count() == 0 && !resource.has_logical_scene() && !resource.has_scene_gpu_resource(), "scene reset must retire all scene resources");
    expect(!resource.scene_gpu_needs_transform_upload(), "reset clears version state");
}
