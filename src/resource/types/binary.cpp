#include "corona/resource/types/binary.h"

namespace Corona::Resource {

Binary::Binary(const std::filesystem::path& path) : IResource(path) {}

BinaryParser::BinaryParser() {
    // register_extension(".bin");
    // register_extension(".dat");
}

}  // namespace Corona::Resource