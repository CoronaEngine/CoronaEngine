#pragma once

#include "cef/cef_scene_operations.h"

#include <cstdint>
#include <optional>
#include <string>

namespace Corona::Systems::UI::ResourceImportService {

struct ValidationError {
    std::string code;
    std::string message;
    explicit operator bool() const { return !code.empty(); }
};

// Neutral copy of the engine importer result. The adapter maps
// Corona::API::MediaInfo into this POD so the service stays engine-header free.
struct MediaImportResult {
    std::uint64_t resource_id{0};
    std::string media_type;
    double duration_seconds{0.0};
    std::string codec;
    int width{0};
    int height{0};
    double fps{0.0};
    int sample_rate{0};
    int channels{0};
};

// Single source of truth for the importable resource types. The dialog filter
// table in the adapter must stay in sync (only presentation lives there).
bool supports_file_type(const std::string& file_type);

// Media files run through the engine importer; other types finish after the
// picker returns.
bool requires_media_import(const std::string& file_type);

// Identity requirements before any dialog or import work.
ValidationError validate_request(const std::optional<std::string>& archive_session,
                                 const std::string& scene,
                                 const std::string& file_type);

// Maps the importer result onto the Web-facing snapshot. Returns false and
// fills the media_unrecognized error when the file was not recognized.
bool apply_media_import(ResourceImportSnapshot& imported,
                        const MediaImportResult& info,
                        const std::string& media_name);

}  // namespace Corona::Systems::UI::ResourceImportService
