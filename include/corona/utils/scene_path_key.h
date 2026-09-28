#pragma once

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace Corona::Utils {

// Runtime identity only. Keep the original path separately for project storage.
// Relative sources require an explicit absolute scene/project directory.
inline std::string normalize_scene_path_key(const std::string& raw_path,
                                            const std::string& base_dir = {}) {
    if (raw_path.empty()) {
        return {};
    }
    auto path = std::filesystem::u8path(raw_path);
    if (!path.is_absolute()) {
        const auto base = std::filesystem::u8path(base_dir);
        if (!base.is_absolute() || path.has_root_name() || path.has_root_directory()) {
            throw std::invalid_argument("Relative Vision source requires an absolute base directory");
        }
        path = base / path;
    }
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        normalized = path;
    }
    const auto utf8 = normalized.lexically_normal().generic_u8string();
    std::string key(utf8.begin(), utf8.end());
#ifdef _WIN32
    // Preserve the existing Windows ASCII case-insensitive key convention.
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
        return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
    });
#endif
    return key;
}

}  // namespace Corona::Utils
