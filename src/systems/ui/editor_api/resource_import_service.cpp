#include "resource_import_service.h"

#include <algorithm>
#include <array>

namespace Corona::Systems::UI::ResourceImportService {

namespace {

constexpr std::array kSupportedFileTypes{"model", "multimedia", "scene", "actor", "ui_image"};

}  // namespace

bool supports_file_type(const std::string& file_type) {
    return std::any_of(kSupportedFileTypes.begin(), kSupportedFileTypes.end(),
                       [&](const char* supported) { return file_type == supported; });
}

bool requires_media_import(const std::string& file_type) {
    return file_type == "multimedia";
}

ValidationError validate_request(const std::optional<std::string>& archive_session,
                                 const std::string& scene,
                                 const std::string& file_type) {
    if (!archive_session || archive_session->empty() || scene.empty() || file_type.empty()) {
        return {"INVALID_ARGUMENT", "Resource import requires scene scope and file type"};
    }
    return {};
}

bool apply_media_import(ResourceImportSnapshot& imported,
                        const MediaImportResult& info,
                        const std::string& media_name) {
    if (info.resource_id == 0 || info.media_type.empty()) {
        imported.status = "error";
        imported.code = "media_unrecognized";
        imported.message = "无法识别的音视频文件: " + imported.path;
        return false;
    }
    imported.status = "success";
    imported.media_name = media_name;
    imported.media_type = info.media_type;
    imported.resource_id = std::to_string(info.resource_id);
    imported.duration = info.duration_seconds;
    imported.codec = info.codec;
    imported.width = info.width;
    imported.height = info.height;
    imported.fps = info.fps;
    imported.sample_rate = info.sample_rate;
    imported.channels = info.channels;
    return true;
}

}  // namespace Corona::Systems::UI::ResourceImportService
