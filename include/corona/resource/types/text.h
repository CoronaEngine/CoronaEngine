#pragma once
#include "corona/resource/resource.h"

namespace Corona::Resource {

class Text : public IResource {
   public:
    explicit Text(const std::filesystem::path& path);
    ~Text() override = default;

    std::string text;
};

class TextParser : public IParser {
   public:
    TextParser();
    ~TextParser() override = default;

   protected:
    std::shared_ptr<IResource> parse_text(const std::filesystem::path& path);
    std::string read_string_file(const std::filesystem::path& path);
};

}  // namespace Corona::Resource
