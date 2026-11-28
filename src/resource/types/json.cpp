#include "corona/resource/types/json.h"

namespace Corona::Resource {

Json::Json(const std::filesystem::path& path) : IResource(path) {}

JsonParser::JsonParser() {
    // register_extension(".json");
}

}  // namespace Corona::Resource