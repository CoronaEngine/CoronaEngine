#pragma once
#include "corona/resource/resource.h"

namespace Corona::Resource {

class Binary : public IResource {
   public:
    explicit Binary(const std::filesystem::path& path);
    ~Binary() override = default;
};

class BinaryParser : public IParser {
   public:
    BinaryParser();
    ~BinaryParser() override = default;

   protected:
};

}  // namespace Corona::Resource