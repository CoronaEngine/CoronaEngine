#pragma once

// Path bridges shared by every editor-API translation unit.
//
// They were anonymous-namespace locals of the handler file, which is why nothing
// outside it could turn a stored route into a filesystem path or compare two project
// roots. Thin, side-effect free wrappers over Corona::Utils plus the forward-slash
// normalisation the editor stores in scene files.

#include <filesystem>
#include <string>

namespace Corona::Systems::UI {

std::filesystem::path path_from_utf8(const std::string& value);
std::string path_to_utf8(const std::filesystem::path& value);

/// Absolute, symlink-resolved form used to compare project roots. Falls back to the
/// absolute path when the filesystem refuses to canonicalise (missing dir, ACL).
std::filesystem::path absolute_normalized_path(const std::filesystem::path& path);

}  // namespace Corona::Systems::UI
