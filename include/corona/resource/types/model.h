#pragma once
#include "corona/resource/resource.h"

namespace Corona::Resource {

using MeshID = std::uint64_t;

struct ModelData {
    std::vector<MeshID> meshes;
};

class Model : public IResource {
   public:
    explicit Model(const std::filesystem::path& path);
    ~Model() override = default;

    ModelData data_;
};

class ModelParser : public IParser {
   public:
    ModelParser();
    ~ModelParser() override = default;

   protected:
    std::shared_ptr<IResource> test(const std::filesystem::path& path);
};

}  // namespace Corona::Resource
