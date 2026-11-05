#pragma once

#include "cef/scene_folder.h"
#include <iostream>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>

// Native tests exercise the same archive operations through the Python driver.
namespace ArchiveFixture {
using namespace Corona::Systems::UI::SceneFolders;
using Json = nlohmann::json;
namespace fs = std::filesystem;
inline Json call(const char* method, Json data, const char* module = "ProjectArchive") {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    std::cout << "ARCHIVE_RPC " << Json({{"module", module}, {"function", method},
                              {"args", Json::array({std::move(data)})}}).dump() << std::endl;
    std::string line;
    if (!std::getline(std::cin, line)) throw std::runtime_error("Archive driver disconnected");
    return Json::parse(line);
}
inline std::string sha256_file(const fs::path& path) {
    return call("sha256", {{"path", path}}, "ArchiveFixture").at("data").get<std::string>();
}
struct Layout { fs::path root, scene_file; std::string scene_name; };
inline std::optional<Layout> detect_scene_folder(const fs::path& path) {
    const auto reply = call("project_info", {{"path", path}});
    if (!reply.at("success")) return {};
    const auto& info = reply.at("data");
    auto root = fs::u8path(info.at("project_path").get<std::string>());
    return Layout{root, root / "scene.ini", info.at("name")};
}
inline std::optional<Layout> create_scene_folder(const fs::path& path, const std::string& name) {
    if (!call("create_project", {{"path", path}, {"name", name}}).at("success")) return {};
    return detect_scene_folder(path);
}
struct Validation {
    std::vector<Diagnostic> diagnostics;
    std::uint64_t asset_count{}, total_bytes{};
    bool ok() const { return diagnostics.empty(); }
};
inline Validation validate_portable_scene(const fs::path& path, bool hashes = true) {
    const auto data = call("validate_project", {{"path", path}, {"verifyHashes", hashes}}).at("data");
    Validation result{{}, data.at("assetCount"), data.at("totalBytes")};
    for (const auto& d : data.at("diagnostics"))
        result.diagnostics.push_back({d.at("code"), d.at("message"), {}, {}, d.value("field", "")});
    return result;
}
inline CleanupResult cleanup_portable_scene_assets(const fs::path& root, bool dry_run) {
    auto validation = call("validate_project", {{"path", root}}).at("data");
    if (!validation.at("ok")) return CleanupResult{0, 0, 0, {{"invalid_archive", "Unsafe cleanup"}}};
    return cleanup_unreferenced_assets(root, validation.at("references").get<std::set<std::string>>(), dry_run);
}
struct SceneDocumentStore {
    fs::path root;
    explicit SceneDocumentStore(const fs::path& path) : root(path) {
        call("recover", {{"path", root}}, "ArchiveFixture");
    }
    bool replace_sections(const std::map<std::string, std::vector<std::string>>& sections, std::vector<Diagnostic>&) {
        return call("sections", {{"path", root}, {"sections", sections}}, "ArchiveFixture").at("success");
    }
};
}  // namespace ArchiveFixture
