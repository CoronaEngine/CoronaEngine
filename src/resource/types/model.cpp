#include "corona/resource/types/model.h"

#include <assimp/scene.h>

#include "corona/resource/resource_manager.h"

namespace Corona::Resource {

Model::Model(const std::filesystem::path& path) : IResource(path) {}

ModelParser::ModelParser() {
    register_extension(".obj", [this](const auto& path, ResourceCache& cache) { return test(path); });
    // register_extension(".fbx", [this](const auto& path, ResourceCache& cache) { return test(path); });
    // register_extension(".gltf", [this](const auto& path, ResourceCache& cache) { return test(path); });
    // register_extension(".glb", [this](const auto& path, ResourceCache& cache) { return test(path); });
    // register_extension(".dae", [this](const auto& path, ResourceCache& cache) { return test(path); });
}

std::shared_ptr<IResource> ModelParser::test(const std::filesystem::path& path) {
    std::printf("Test %s\n", path.string().c_str());
    return std::make_shared<Model>(path);
}

}  // namespace Corona::Resource