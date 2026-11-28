#pragma once
#include "corona/resource/resource.h"

namespace Corona::Resource {

class Json : public IResource {
   public:
    explicit Json(const std::filesystem::path& path);
    ~Json() override = default;
};

class JsonParser : public IParser {
   public:
    JsonParser();
    ~JsonParser() override = default;

   protected:
};

}  // namespace Corona::Resource