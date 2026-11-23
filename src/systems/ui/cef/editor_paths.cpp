#include "editor_paths.h"

#include <corona/utils/path_utils.h>

#include <algorithm>

namespace Corona::Systems::UI {

std::filesystem::path path_from_utf8(const std::string& value) {
    return Corona::Utils::utf8_to_path(value);
}

std::string path_to_utf8(const std::filesystem::path& value) {
    auto text = Corona::Utils::path_to_utf8(value);
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

std::filesystem::path absolute_normalized_path(const std::filesystem::path& path) {
    std::error_code ec;
    auto absolute = path.is_absolute() ? path : std::filesystem::absolute(path, ec);
    if (ec) {
        absolute = path;
        ec.clear();
    }
    const auto canonical = std::filesystem::weakly_canonical(absolute, ec);
    return ec ? absolute : canonical;
}

}
  // namespace Corona::Systems::UI
