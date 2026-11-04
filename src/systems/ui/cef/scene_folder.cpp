#include "scene_folder.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include <horizon/core/logging.h>
#include <nlohmann/json.hpp>
#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

namespace Corona::Systems::UI::SceneFolders {
namespace {

namespace fs = std::filesystem;

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string path_utf8(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

fs::path path_from_utf8(std::string_view value) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

std::vector<std::string> split_words(std::string value) {
    std::istringstream stream(std::move(value));
    std::vector<std::string> words;
    for (std::string word; stream >> std::quoted(word);) words.push_back(std::move(word));
    return words;
}

std::string percent_decode_uri(std::string_view value) {
    auto hex = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    std::string decoded;
    decoded.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' && index + 2 < value.size()) {
            const auto high = hex(value[index + 1]);
            const auto low = hex(value[index + 2]);
            if (high < 0 || low < 0) throw std::runtime_error("Invalid percent-encoded URI");
            decoded.push_back(static_cast<char>((high << 4) | low));
            index += 2;
        } else {
            decoded.push_back(value[index]);
        }
    }
    return decoded;
}





bool is_relative_inside(const fs::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name()) return false;
    for (const auto& part : path) {
        if (part == "..") return false;
    }
    return true;
}

struct SourceFile {
    fs::path source;
    fs::path relative;
};

// optional=true 表示"缺了也能用"的依赖（贴图）。模型引用但磁盘上不存在的贴图很常见
// （导出时没带上、路径写死在美术机器上）。引擎加载器遇到这种情况只是少一张贴图，不会
// 拒绝整个模型；打包器若当成硬错误，一张缺失贴图就能让整个模型导不进来。
// 结构性依赖（OBJ 的 MTL、glTF 的 buffer）仍然是硬错误——缺了模型就是坏的。
void add_dependency(std::vector<SourceFile>& files,
                    std::vector<Diagnostic>& diagnostics,
                    const fs::path& source,
                    const fs::path& relative,
                    bool optional = false) {
    if (!is_relative_inside(relative)) {
        diagnostics.push_back({"unsafe_dependency", "Resource dependency escapes its bundle", source});
        return;
    }
    if (!fs::is_regular_file(source)) {
        if (optional) {
            CFW_LOG_WARNING("[SceneFolders] Optional model texture missing, skipping: '{}'",
                            path_utf8(source));
            return;
        }
        diagnostics.push_back({"missing_dependency", "Resource dependency is missing", source});
        return;
    }
    const auto normalized = relative.lexically_normal();
    if (std::none_of(files.begin(), files.end(), [&](const SourceFile& item) {
            std::error_code ec;
            return fs::equivalent(item.source, source, ec) && !ec;
        })) {
        files.push_back({source, normalized});
    }
}

std::string unquote(std::string value) {
    value = trim(std::move(value));
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

void collect_mtl(const fs::path& mtl,
                 const fs::path& source_root,
                 std::vector<SourceFile>& files,
                 std::vector<Diagnostic>& diagnostics) {
    std::ifstream stream(mtl);
    for (std::string line; std::getline(stream, line);) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto space = line.find_first_of(" \t");
        const auto key = lower(line.substr(0, space));
        static const std::set<std::string> texture_keys = {
            "map_ka", "map_kd", "map_ks", "map_ke", "map_ns", "map_d", "bump", "map_bump", "disp", "decal", "norm"};
        if (!texture_keys.contains(key) || space == std::string::npos) continue;
        auto words = split_words(line.substr(space + 1));
        if (words.empty()) continue;
        const auto texture_text = unquote(words.back());
        const auto texture = (mtl.parent_path() / path_from_utf8(texture_text)).lexically_normal();
        std::error_code ec;
        const auto relative = fs::relative(texture, source_root, ec);
        if (ec) {
            diagnostics.push_back({"unsafe_dependency", "Unable to relativize material texture", texture});
        } else {
            add_dependency(files, diagnostics, texture, relative, /*optional=*/true);
        }
    }
}

void collect_assimp_textures(const fs::path& source,
                             const fs::path& source_root,
                             std::vector<SourceFile>& files,
                             std::vector<Diagnostic>& diagnostics) {
    Assimp::Importer importer;
    // source.string() 在 Windows 上走 ANSI 窄化，非 ASCII 路径会变成乱码导致 Assimp
    // 直接打不开文件。引擎真正的加载器（SceneParser::parse_assimp）传的是 UTF-8，
    // 这里必须保持一致，否则"引擎能加载但打包器说文件无效"。
    const auto source_utf8 = path_utf8(source);
    // 依赖扫描只读取材质引用，不做结构校验；否则合法 KHR_materials_specular
    // 组合会被 Assimp 5.4.3 的校验误报，导致外部贴图依赖漏收集。
    const auto* scene = importer.ReadFile(source_utf8, 0);
    if (!scene) {
        // 这里只负责"收集贴图依赖"，不是权威的模型校验。引擎加载器用的是自定义
        // UnicodeIOSystem + 完整 postprocess 流程，能读它读不了的文件；把解析失败
        // 当成导入失败会把本来可用的模型挡在门外（FBX/USDC 尤其明显）。
        // 失败时退化为"不收集额外贴图"，主文件照常导入。
        CFW_LOG_WARNING(
            "[SceneFolders] Assimp dependency scan failed for '{}': {} — importing main file only, "
            "external textures may be missing",
            source_utf8, importer.GetErrorString());
        return;
    }
    for (unsigned int material_index = 0; material_index < scene->mNumMaterials; ++material_index) {
        const auto* material = scene->mMaterials[material_index];
        for (int type_value = static_cast<int>(aiTextureType_DIFFUSE);
             type_value <= static_cast<int>(aiTextureType_UNKNOWN); ++type_value) {
            const auto type = static_cast<aiTextureType>(type_value);
            for (unsigned int texture_index = 0; texture_index < material->GetTextureCount(type);
                 ++texture_index) {
                aiString texture_text;
                if (material->GetTexture(type, texture_index, &texture_text) != AI_SUCCESS) continue;
                const std::string value = texture_text.C_Str();
                if (value.empty() || value.front() == '*') continue;
                if (value.find("://") != std::string::npos) {
                    diagnostics.push_back({"remote_dependency", "Remote model dependencies are unsupported",
                                           path_from_utf8(value)});
                    continue;
                }
                const auto candidate = path_from_utf8(percent_decode_uri(value));
                const auto dependency = candidate.is_absolute()
                                            ? candidate.lexically_normal()
                                            : (source_root / candidate).lexically_normal();
                std::error_code ec;
                const auto relative = fs::relative(dependency, source_root, ec);
                if (ec) {
                    diagnostics.push_back({"unsafe_dependency", "Unable to relativize model texture", dependency});
                } else {
                    add_dependency(files, diagnostics, dependency, relative, /*optional=*/true);
                }
            }
        }
    }
}

std::vector<SourceFile> collect_bundle(const fs::path& source,
                                       std::vector<Diagnostic>& diagnostics) {
    std::vector<SourceFile> files;
    if (!fs::is_regular_file(source)) {
        diagnostics.push_back({"missing_model", "Model source does not exist", source});
        return files;
    }
    const auto source_root = source.parent_path();
    add_dependency(files, diagnostics, source, source.filename());
    const auto extension = lower(source.extension().string());

    if (extension == ".obj") {
        std::ifstream stream(source);
        for (std::string line; std::getline(stream, line);) {
            line = trim(line);
            if (line.rfind("mtllib", 0) != 0 || line.size() <= 6) continue;
            for (const auto& mtl_text : split_words(line.substr(6))) {
                const auto mtl = (source_root / path_from_utf8(unquote(mtl_text))).lexically_normal();
                std::error_code ec;
                const auto relative = fs::relative(mtl, source_root, ec);
                if (ec) {
                    diagnostics.push_back({"unsafe_dependency", "Unable to relativize OBJ material", mtl});
                    continue;
                }
                const auto before = diagnostics.size();
                add_dependency(files, diagnostics, mtl, relative);
                if (before == diagnostics.size()) collect_mtl(mtl, source_root, files, diagnostics);
            }
        }
    } else if (extension == ".gltf") {
        try {
            std::ifstream stream(source);
            const auto document = nlohmann::json::parse(stream);
            auto collect_uris = [&](const char* key) {
                if (!document.contains(key) || !document[key].is_array()) return;
                for (const auto& item : document[key]) {
                    if (!item.is_object() || !item.contains("uri") || !item["uri"].is_string()) continue;
                    const auto uri = item["uri"].get<std::string>();
                    if (uri.rfind("data:", 0) == 0) continue;
                    if (uri.find("://") != std::string::npos) {
                        diagnostics.push_back({"remote_dependency", "Remote glTF dependencies are unsupported",
                                               path_from_utf8(uri)});
                        continue;
                    }
                    const auto dependency =
                        (source_root / path_from_utf8(percent_decode_uri(uri))).lexically_normal();
                    std::error_code ec;
                    const auto relative = fs::relative(dependency, source_root, ec);
                    if (ec) diagnostics.push_back({"unsafe_dependency", "Unable to relativize glTF dependency", dependency});
                    else add_dependency(files, diagnostics, dependency, relative);
                }
            };
            collect_uris("buffers");
            collect_uris("images");
        } catch (const std::exception& error) {
            diagnostics.push_back({"invalid_gltf", error.what(), source});
        }
    } else if (extension == ".dae" || extension == ".usd" || extension == ".usda") {
        std::ifstream stream(source, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(stream)),
                               std::istreambuf_iterator<char>());
        std::vector<std::string> references;
        if (extension == ".dae") {
            // COLLADA 里 <init_from> 有两种含义，不能一把梭全文搜：
            //   <library_images><image><init_from>textures/x.png  ← 真正的文件路径
            //   <profile_COMMON><surface><init_from>file1-image   ← 只是 image 的 id 引用
            // 之前不分场合全抓，于是把 id "file1-image" 当文件名拼成
            // <模型目录>/file1-image 去找，必然 missing_dependency，整个模型被挡下。
            constexpr std::string_view open_tag = "<init_from>";
            constexpr std::string_view close_tag = "</init_from>";
            constexpr std::string_view library_close = "</library_images>";

            // 在 [begin, end) 区间内收集 <init_from> 文本。
            auto scan_range = [&](size_t begin, size_t end) {
                size_t cursor = begin;
                while ((cursor = text.find(open_tag, cursor)) != std::string::npos && cursor < end) {
                    const auto start = cursor + open_tag.size();
                    const auto stop = text.find(close_tag, start);
                    if (stop == std::string::npos || stop > end) break;
                    references.push_back(trim(text.substr(start, stop - start)));
                    cursor = stop + close_tag.size();
                }
            };

            bool scanned_library = false;
            size_t library_cursor = 0;
            while ((library_cursor = text.find("<library_images", library_cursor)) != std::string::npos) {
                const auto header_end = text.find('>', library_cursor);
                if (header_end == std::string::npos) break;
                // 自闭合 <library_images/> 没有内容，跳过且不算"扫过"，
                // 否则会白白压掉下面的全文兜底。
                if (text[header_end - 1] == '/') {
                    library_cursor = header_end + 1;
                    continue;
                }
                const auto library_end = text.find(library_close, header_end);
                scan_range(header_end + 1, library_end == std::string::npos ? text.size() : library_end);
                scanned_library = true;
                if (library_end == std::string::npos) break;
                library_cursor = library_end + library_close.size();
            }
            // 没有 <library_images> 的文档里不存在 id 引用可混淆（surface 引用的 image
            // 根本没声明），退回全文扫描以兼容精简/非标准导出。
            if (!scanned_library) scan_range(0, text.size());
        } else {
            size_t cursor = 0;
            while ((cursor = text.find('@', cursor)) != std::string::npos) {
                const auto end = text.find('@', cursor + 1);
                if (end == std::string::npos) break;
                references.push_back(text.substr(cursor + 1, end - cursor - 1));
                cursor = end + 1;
            }
        }
        for (auto reference : references) {
            if (reference.empty()) continue;
            if (reference.find("://") != std::string::npos) {
                diagnostics.push_back({"remote_dependency", "Remote model dependencies are unsupported",
                                       path_from_utf8(reference)});
                continue;
            }
            const auto dependency = (source_root / path_from_utf8(reference)).lexically_normal();
            std::error_code ec;
            const auto relative = fs::relative(dependency, source_root, ec);
            if (ec) diagnostics.push_back({"unsafe_dependency", "Unable to relativize model dependency", dependency});
            else add_dependency(files, diagnostics, dependency, relative);
        }
        collect_assimp_textures(source, source_root, files, diagnostics);
    } else if (extension == ".fbx" || extension == ".usdc" || extension == ".usdz" ||
               extension == ".3ds" || extension == ".stl") {
        collect_assimp_textures(source, source_root, files, diagnostics);
    }
    return files;
}

nlohmann::json import_plan(const std::vector<SourceFile>& files) {
    nlohmann::json plan = nlohmann::json::array();
    for (const auto& file : files) {
        plan.push_back({{"source", path_utf8(file.source)}, {"relative", path_utf8(file.relative)}});
    }
    return plan;
}

std::vector<Diagnostic> diagnostics(const nlohmann::json& response) {
    std::vector<Diagnostic> errors;
    for (const auto& d : response.value("diagnostics", nlohmann::json::array())) {
        errors.push_back({d.value("code", ""), d.value("message", ""), path_from_utf8(d.value("path", "")),
                          d.value("actor", ""), d.value("field", "")});
    }
    return errors;
}

ImportResult import_result(const nlohmann::json& response) {
    ImportResult out;
    out.diagnostics = diagnostics(response);
    if (out.diagnostics.empty()) {
        const auto& bundle = response.at("bundle");
        out.main_route = bundle.at("main");
        out.type = bundle.at("type");
        out.bundle_sha256 = bundle.at("sha256");
        out.dependencies = bundle.at("dependencies").get<std::vector<std::string>>();
        for (const auto& file : bundle.at("files")) {
            out.files.push_back({{}, file.at("path"), file.at("sha256"), file.at("size")});
        }
    }
    return out;
}
}  // namespace

bool is_valid_asset_route(std::string_view route) {
    if (route.empty() || route.find('\\') != std::string_view::npos) return false;
    const auto path = path_from_utf8(route);
    if (!is_relative_inside(path)) return false;
    const auto iterator = path.begin();
    return iterator != path.end() && *iterator == "Assets";
}

SceneAssetStore::SceneAssetStore(fs::path root, bool allow_missing)
    : root_(std::move(root)), allow_missing_(allow_missing) {}

nlohmann::json SceneAssetStore::request(nlohmann::json payload) const {
    payload["root"] = path_utf8(root_);
    payload["allowMissing"] = allow_missing_;
    return archive_asset_request(std::move(payload));
}

ImportResult SceneAssetStore::import_model_source(const fs::path& source, const fs::path& actor) {
    static const std::set<std::string> supported{
        ".obj", ".ply", ".gltf", ".glb", ".fbx", ".dae", ".usd", ".usda", ".usdc", ".usdz", ".3ds", ".stl"};
    ImportResult result;
    if (!supported.contains(lower(source.extension().string()))) {
        result.diagnostics.push_back({"unsupported_model", "Unsupported model extension", source});
        return result;
    }
    const auto files = collect_bundle(fs::absolute(source).lexically_normal(), result.diagnostics);
    if (!result.diagnostics.empty()) return result;
    result = import_result(request({{"op", "import"}, {"source", path_utf8(source)}, {"actor", path_utf8(actor)},
                                    {"category", "Models"}, {"files", import_plan(files)}}));
    if (result.ok() && !actor.empty()) result.files.front().source = actor;
    else if (result.files.size() == files.size()) {
        for (size_t i = 0; i < files.size(); ++i) result.files[i].source = files[i].source;
    }
    return result;
}

ImportResult SceneAssetStore::import_model(const fs::path& source) {
    return import_model_source(source, {});
}

ImportResult SceneAssetStore::import_actor(const fs::path& actor_source, const fs::path& model_source) {
    return import_model_source(model_source, actor_source);
}

ImportResult SceneAssetStore::import_file(const fs::path& source, std::string_view category) {
    const auto absolute = fs::absolute(source).lexically_normal();
    auto result = import_result(request({{"op", "import"}, {"source", path_utf8(absolute)}, {"category", category},
                                         {"files", import_plan({{absolute, absolute.filename()}})}}));
    if (result.ok()) result.files.front().source = absolute;
    return result;
}

bool SceneAssetStore::write_manifest() const {
    return request({{"op", "ensure"}}).value("ok", false);
}

std::vector<Diagnostic> SceneAssetStore::validate_manifest(bool verify_hashes) const {
    return diagnostics(request({{"op", "validate"}, {"hashes", verify_hashes}}));
}

bool SceneAssetStore::contains_route(std::string_view route) const {
    return request({{"op", "contains"}, {"route", route}}).value("found", false);
}

CleanupResult cleanup_unreferenced_assets(const fs::path& root, const std::set<std::string>& referenced_routes, bool dry_run) {
    const auto response = archive_asset_request({{"root", path_utf8(root)}, {"op", "cleanup"},
                                                 {"references", referenced_routes}, {"dryRun", dry_run}});
    return {response.value("removedBundles", uint64_t{}), response.value("removedFiles", uint64_t{}),
            response.value("reclaimedBytes", uint64_t{}), diagnostics(response)};
}

}  // namespace Corona::Systems::UI::SceneFolders
