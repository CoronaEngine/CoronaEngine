// 资源导入决策（WNC-013）：纯 DTO/引擎 POD 输入，无 CEF 依赖。
#include "editor_api/resource_import_service.h"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace Corona::Systems::UI;
using namespace Corona::Systems::UI::ResourceImportService;

namespace {

int checks = 0;

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_supported_types() {
    for (const char* type : {"model", "multimedia", "scene", "actor", "ui_image"}) {
        check(supports_file_type(type), "known resource type supported");
    }
    check(!supports_file_type("audio_only"), "unknown type rejected");
    check(!supports_file_type(""), "empty type rejected");
    check(requires_media_import("multimedia"), "media types run through the importer");
    check(!requires_media_import("model"), "model files finish after pick");
}

void test_request_validation() {
    const std::optional<std::string> archive = std::string("archive");
    check(!validate_request(archive, "scene.ini", "model"), "scoped request accepted");

    check(validate_request(std::nullopt, "scene.ini", "model").code == "INVALID_ARGUMENT" &&
          validate_request(std::nullopt, "scene.ini", "model").message ==
              "Resource import requires scene scope and file type",
          "missing archive rejected");

    check(static_cast<bool>(validate_request(archive, "", "model")), "missing scene rejected");
    check(static_cast<bool>(validate_request(archive, "scene.ini", "")), "missing file type rejected");
    check(static_cast<bool>(validate_request(std::string{}, "scene.ini", "model")), "empty archive rejected");
}

void test_media_mapping() {
    ResourceImportSnapshot imported;
    imported.path = "assets/music.theme.mp3";

    MediaImportResult info;    info.resource_id = 42;
    info.media_type = "audio";
    info.duration_seconds = 12.5;
    info.codec = "mp3";
    info.sample_rate = 44100;
    info.channels = 2;
    check(apply_media_import(imported, info, "music.theme"), "recognized media succeeds");
    check(imported.status == "success", "status success");
    check(imported.media_name == "music.theme" && imported.media_type == "audio", "identity mapped");
    check(imported.resource_id == "42" && imported.duration == 12.5, "resource id and duration mapped");
    check(imported.codec == "mp3" && imported.sample_rate == 44100 && imported.channels == 2,
          "audio metadata mapped");

    ResourceImportSnapshot unrecognized;
    unrecognized.path = "assets/broken.mp4";
    MediaImportResult missing;
    check(!apply_media_import(unrecognized, missing, "broken"), "unrecognized media fails");
    check(unrecognized.status == "error" && unrecognized.code == "media_unrecognized",
          "unrecognized media error status");
    check(unrecognized.message == "无法识别的音视频文件: assets/broken.mp4",
          "unrecognized media error message keeps the project path");
}

}  // namespace

int main() {
    test_supported_types();
    test_request_validation();
    test_media_mapping();
    std::cout << "resource_import_service: " << checks << " checks passed\n";
    return 0;
}
