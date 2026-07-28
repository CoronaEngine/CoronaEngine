#include "scene_folder.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include <nlohmann/json.hpp>
#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

namespace Corona::Systems::UI::SceneFolders {
namespace {

namespace fs = std::filesystem;

std::recursive_mutex& scene_file_mutex(const fs::path& root) {
    static std::mutex registry_mutex;
    static std::unordered_map<std::wstring, std::unique_ptr<std::recursive_mutex>> mutexes;
    const auto key = fs::absolute(root).lexically_normal().native();
    std::scoped_lock registry_lock(registry_mutex);
    auto& mutex = mutexes[key];
    if (!mutex) mutex = std::make_unique<std::recursive_mutex>();
    return *mutex;
}

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

bool resolves_within(const fs::path& root, const fs::path& candidate) {
    std::error_code ec;
    const auto canonical_root = fs::weakly_canonical(root, ec);
    if (ec) return false;
    const auto canonical_candidate = fs::weakly_canonical(candidate, ec);
    if (ec) return false;
    const auto relative = fs::relative(canonical_candidate, canonical_root, ec);
    return !ec && !relative.empty() && relative != "." &&
           relative.native().find(L':') == std::wstring::npos &&
           std::none_of(relative.begin(), relative.end(), [](const auto& component) {
               return component == "..";
           });
}

std::map<std::string, std::map<std::string, std::string>> read_ini(const fs::path& path) {
    std::ifstream stream(path);
    std::map<std::string, std::map<std::string, std::string>> result;
    std::string section;
    std::string line;
    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const auto equals = line.find('=');
        if (equals != std::string::npos) {
            result[section][lower(trim(line.substr(0, equals)))] = trim(line.substr(equals + 1));
        }
    }
    return result;
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

std::string decode_base64(std::string_view input) {
    auto value_of = [](unsigned char ch) -> int {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '+') return 62;
        if (ch == '/') return 63;
        return -1;
    };
    std::string output;
    std::uint32_t value = 0;
    int bits = -8;
    for (const auto ch : input) {
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == '=') continue;
        const auto decoded = value_of(static_cast<unsigned char>(ch));
        if (decoded < 0) throw std::runtime_error("Invalid base64 Vision document");
        value = (value << 6) | static_cast<std::uint32_t>(decoded);
        bits += 6;
        if (bits >= 0) {
            output.push_back(static_cast<char>((value >> bits) & 0xff));
            bits -= 8;
        }
    }
    return output;
}

std::uint32_t adler32(std::string_view payload) {
    constexpr std::uint32_t mod = 65521;
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const auto ch : payload) {
        a = (a + static_cast<unsigned char>(ch)) % mod;
        b = (b + a) % mod;
    }
    return (b << 16) | a;
}

nlohmann::json decode_embedded_vision_document(std::string_view encoded) {
    const auto payload = decode_base64(encoded);
    if (payload.size() < 6) throw std::runtime_error("Embedded Vision document is truncated");
    size_t offset = 2;
    const auto checksum_offset = payload.size() - 4;
    std::string json_text;
    while (offset < checksum_offset) {
        const auto header = static_cast<unsigned char>(payload[offset++]);
        const bool final_block = (header & 0x01) != 0;
        if (((header >> 1) & 0x03) != 0) {
            throw std::runtime_error("Embedded Vision document uses unsupported compression");
        }
        if (offset + 4 > checksum_offset) throw std::runtime_error("Embedded Vision block is truncated");
        const auto len = static_cast<std::uint16_t>(
            static_cast<unsigned char>(payload[offset]) |
            (static_cast<unsigned char>(payload[offset + 1]) << 8));
        const auto nlen = static_cast<std::uint16_t>(
            static_cast<unsigned char>(payload[offset + 2]) |
            (static_cast<unsigned char>(payload[offset + 3]) << 8));
        offset += 4;
        if (static_cast<std::uint16_t>(~len) != nlen || offset + len > checksum_offset) {
            throw std::runtime_error("Embedded Vision block length is invalid");
        }
        json_text.append(payload.data() + offset, len);
        offset += len;
        if (final_block) break;
    }
    const auto expected =
        (static_cast<std::uint32_t>(static_cast<unsigned char>(payload[checksum_offset])) << 24) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(payload[checksum_offset + 1])) << 16) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(payload[checksum_offset + 2])) << 8) |
        static_cast<std::uint32_t>(static_cast<unsigned char>(payload[checksum_offset + 3]));
    if (adler32(json_text) != expected) throw std::runtime_error("Embedded Vision checksum mismatch");
    auto document = nlohmann::json::parse(json_text);
    if (!document.is_object()) throw std::runtime_error("Embedded Vision document must be an object");
    return document;
}

bool vision_resource_key(std::string key) {
    key = lower(std::move(key));
    return key == "fn" || key == "path" || key == "file" || key == "filename" ||
           key == "texture" || key == "image";
}

template <typename Callback>
void visit_vision_resource_routes(const nlohmann::json& value,
                                  const std::string& field,
                                  Callback&& callback) {
    if (value.is_object()) {
        for (const auto& item : value.items()) {
            if (is_vision_output_section_key(item.key())) continue;
            const auto child_field = field.empty() ? item.key() : field + "." + item.key();
            if (vision_resource_key(item.key()) && item.value().is_string()) {
                callback(item.value().get<std::string>(), child_field);
            } else {
                visit_vision_resource_routes(item.value(), child_field, callback);
            }
        }
    } else if (value.is_array()) {
        for (size_t index = 0; index < value.size(); ++index) {
            visit_vision_resource_routes(value[index], field + "[" + std::to_string(index) + "]", callback);
        }
    }
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

void add_dependency(std::vector<SourceFile>& files,
                    std::vector<Diagnostic>& diagnostics,
                    const fs::path& source,
                    const fs::path& relative) {
    if (!is_relative_inside(relative)) {
        diagnostics.push_back({"unsafe_dependency", "Resource dependency escapes its bundle", source});
        return;
    }
    if (!fs::is_regular_file(source)) {
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
            add_dependency(files, diagnostics, texture, relative);
        }
    }
}

void collect_assimp_textures(const fs::path& source,
                             const fs::path& source_root,
                             std::vector<SourceFile>& files,
                             std::vector<Diagnostic>& diagnostics,
                             bool require_parse) {
    Assimp::Importer importer;
    const auto* scene = importer.ReadFile(source.string(), aiProcess_ValidateDataStructure);
    if (!scene) {
        if (require_parse) {
            diagnostics.push_back({"invalid_model", importer.GetErrorString(), source});
        }
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
                    add_dependency(files, diagnostics, dependency, relative);
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
            constexpr std::string_view open_tag = "<init_from>";
            constexpr std::string_view close_tag = "</init_from>";
            size_t cursor = 0;
            while ((cursor = text.find(open_tag, cursor)) != std::string::npos) {
                const auto start = cursor + open_tag.size();
                const auto end = text.find(close_tag, start);
                if (end == std::string::npos) break;
                references.push_back(trim(text.substr(start, end - start)));
                cursor = end + close_tag.size();
            }
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
        collect_assimp_textures(source, source_root, files, diagnostics, false);
    } else if (extension == ".fbx" || extension == ".usdc") {
        collect_assimp_textures(source, source_root, files, diagnostics, true);
    }
    return files;
}

class Sha256Hasher {
   public:
    Sha256Hasher() {
        DWORD copied{};
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
            BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size_),
                              sizeof(object_size_), &copied, 0) != 0 ||
            BCryptGetProperty(algorithm_, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size_),
                              sizeof(hash_size_), &copied, 0) != 0) {
            cleanup();
            throw std::runtime_error("Unable to initialize SHA-256");
        }
        object_.resize(object_size_);
        if (BCryptCreateHash(algorithm_, &hash_, object_.data(), object_size_, nullptr, 0, 0) != 0) {
            cleanup();
            throw std::runtime_error("Unable to initialize SHA-256");
        }
    }

    Sha256Hasher(const Sha256Hasher&) = delete;
    Sha256Hasher& operator=(const Sha256Hasher&) = delete;
    ~Sha256Hasher() { cleanup(); }

    void update(const void* data, size_t size) {
        const auto* cursor = static_cast<const UCHAR*>(data);
        while (size != 0) {
            const auto chunk = static_cast<ULONG>(std::min<size_t>(size, 1024 * 1024));
            if (BCryptHashData(hash_, const_cast<PUCHAR>(cursor), chunk, 0) != 0) {
                throw std::runtime_error("Unable to compute SHA-256");
            }
            cursor += chunk;
            size -= chunk;
        }
    }

    std::string finish() {
        std::vector<UCHAR> digest(hash_size_);
        if (BCryptFinishHash(hash_, digest.data(), hash_size_, 0) != 0) {
            throw std::runtime_error("Unable to compute SHA-256");
        }
        std::ostringstream result;
        for (const auto byte : digest) {
            result << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
        }
        return result.str();
    }

   private:
    void cleanup() noexcept {
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
        hash_ = nullptr;
        algorithm_ = nullptr;
    }

    BCRYPT_ALG_HANDLE algorithm_{};
    BCRYPT_HASH_HANDLE hash_{};
    DWORD object_size_{};
    DWORD hash_size_{};
    std::vector<UCHAR> object_;
};

void hash_file_into(Sha256Hasher& hasher, const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Unable to open file: " + path.string());
    std::array<char, 64 * 1024> buffer{};
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = stream.gcount();
        if (count > 0) hasher.update(buffer.data(), static_cast<size_t>(count));
    }
    if (!stream.eof()) throw std::runtime_error("Unable to read file: " + path.string());
}

std::string bundle_hash(std::vector<SourceFile> files) {
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return left.relative.generic_u8string() < right.relative.generic_u8string();
    });
    Sha256Hasher hasher;
    for (const auto& file : files) {
        const auto relative = path_utf8(file.relative);
        hasher.update(relative.data(), relative.size());
        constexpr char separator = '\0';
        hasher.update(&separator, 1);
        hash_file_into(hasher, file.source);
    }
    return hasher.finish();
}

nlohmann::json manifest_json(const std::vector<ImportResult>& bundles) {
    nlohmann::json result = {{"format", "corona_scene_assets"}, {"version", 1}, {"bundles", nlohmann::json::array()}};
    for (const auto& bundle : bundles) {
        nlohmann::json files = nlohmann::json::array();
        for (const auto& file : bundle.files) {
            files.push_back({{"path", file.route}, {"sha256", file.sha256}, {"size", file.size}});
        }
        result["bundles"].push_back({{"type", bundle.type}, {"sha256", bundle.bundle_sha256},
                                     {"main", bundle.main_route}, {"files", std::move(files)},
                                     {"dependencies", bundle.dependencies}});
    }
    return result;
}

std::string safe_category(std::string_view category) {
    static const std::set<std::string> allowed = {
        "Models", "Actors", "Images", "Audio", "Scripts", "Terrain", "Vision"};
    const std::string value(category);
    if (!allowed.contains(value)) throw std::invalid_argument("Unsupported asset category: " + value);
    return value;
}

ImportResult import_collected_files(const fs::path& root,
                                    std::vector<ImportResult>& bundles,
                                    std::vector<SourceFile> files,
                                    std::string type,
                                    std::string category,
                                    const fs::path& source) {
    ImportResult result;
    result.type = std::move(type);
    try {
        result.bundle_sha256 = bundle_hash(files);
        const auto bundle_rel = fs::path("Assets") / safe_category(category) /
                                result.bundle_sha256.substr(0, 12);
        const auto destination = root / bundle_rel;
        if (!resolves_within(root / "Assets", destination)) {
            throw std::runtime_error("Asset bundle destination escapes Assets");
        }
        const auto staging = root / ".asset-stage" / result.bundle_sha256;
        std::error_code ec;
        fs::remove_all(staging, ec);
        for (const auto& file : files) {
            const auto target = staging / file.relative;
            fs::create_directories(target.parent_path());
            fs::copy_file(file.source, target, fs::copy_options::overwrite_existing);
            ImportedFile imported;
            imported.source = file.source;
            imported.route = path_utf8(bundle_rel / file.relative);
            imported.sha256 = sha256_file(file.source);
            imported.size = fs::file_size(file.source);
            result.files.push_back(std::move(imported));
        }
        if (!fs::exists(destination)) {
            fs::create_directories(destination.parent_path());
            fs::rename(staging, destination);
        } else {
            std::vector<SourceFile> existing_files;
            bool complete = true;
            for (const auto& file : files) {
                const auto existing = destination / file.relative;
                if (!fs::is_regular_file(existing)) {
                    complete = false;
                    break;
                }
                existing_files.push_back({existing, file.relative});
            }
            if (!complete || bundle_hash(existing_files) != result.bundle_sha256) {
                fs::remove_all(staging, ec);
                result.main_route.clear();
                result.diagnostics.push_back(
                    {"bundle_collision", "Bundle hash-prefix directory contains different content",
                     destination});
                return result;
            }
            fs::remove_all(staging, ec);
        }
        fs::remove(root / ".asset-stage", ec);
        result.main_route = path_utf8(bundle_rel / files.front().relative);
        for (const auto& file : result.files) {
            if (file.route != result.main_route) result.dependencies.push_back(file.route);
        }
        const auto existing = std::find_if(bundles.begin(), bundles.end(), [&](const ImportResult& item) {
            return item.bundle_sha256 == result.bundle_sha256 && item.type == result.type;
        });
        if (existing == bundles.end()) bundles.push_back(result);
    } catch (const std::exception& error) {
        result.main_route.clear();
        result.diagnostics.push_back({"asset_import_failed", error.what(), source});
        std::error_code ec;
        fs::remove_all(root / ".asset-stage", ec);
    }
    return result;
}

void write_ini_file(const fs::path& path,
                    const std::map<std::string, std::map<std::string, std::string>>& ini) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("Unable to write scene.ini");
    for (const auto& [section, values] : ini) {
        stream << '[' << section << "]\n";
        for (const auto& [key, value] : values) stream << key << " = " << value << '\n';
        stream << '\n';
    }
}

struct LegacyPaths {
    fs::path project_root;
    fs::path scene_file;
};

std::optional<LegacyPaths> locate_legacy_scene(const fs::path& input,
                                               std::vector<Diagnostic>& diagnostics) {
    auto source = fs::absolute(input).lexically_normal();
    if (fs::is_directory(source)) source /= "project.ini";
    if (source.filename() == "project.ini") {
        if (!fs::is_regular_file(source)) {
            diagnostics.push_back({"missing_project", "Legacy project.ini is missing", source});
            return std::nullopt;
        }
        const auto project = read_ini(source);
        auto route = fs::path("Scene") / "default.scene";
        if (const auto section = project.find("project"); section != project.end() &&
            section->second.contains("entrance_scene")) {
            route = path_from_utf8(section->second.at("entrance_scene"));
        }
        const auto scene = source.parent_path() / route;
        if (!fs::is_regular_file(scene)) {
            diagnostics.push_back({"missing_scene", "Legacy scene file is missing", scene});
            return std::nullopt;
        }
        return LegacyPaths{source.parent_path(), scene};
    }
    if (lower(source.extension().string()) == ".scene" && fs::is_regular_file(source)) {
        auto root = source.parent_path();
        if (lower(root.filename().string()) == "scene") root = root.parent_path();
        return LegacyPaths{root, source};
    }
    diagnostics.push_back({"unsupported_legacy_source", "Expected project.ini or a legacy .scene", source});
    return std::nullopt;
}

fs::path resolve_legacy_route(const fs::path& project_root, std::string_view route) {
    const auto path = path_from_utf8(route);
    return path.is_absolute() ? path : project_root / path;
}

}  // namespace

bool is_valid_asset_route(std::string_view route) {
    if (route.empty() || route.find('\\') != std::string_view::npos) return false;
    const auto path = path_from_utf8(route);
    if (!is_relative_inside(path)) return false;
    const auto iterator = path.begin();
    return iterator != path.end() && *iterator == "Assets";
}

bool is_vision_output_section_key(std::string_view key) {
    return lower(std::string(key)) == "output";
}

std::optional<SceneFolderLayout> detect_scene_folder(const fs::path& input) {
    auto root = fs::is_directory(input) ? input : input.parent_path();
    const auto scene_file = fs::is_directory(input) ? input / "scene.ini" : input;
    if (scene_file.filename() != "scene.ini" || !fs::is_regular_file(scene_file)) return std::nullopt;
    const auto ini = read_ini(scene_file);
    const auto format = ini.find("format");
    if (format == ini.end() || !format->second.contains("type") ||
        format->second.at("type") != "corona_scene_folder" ||
        !format->second.contains("version") || format->second.at("version") != "1") return std::nullopt;
    SceneFolderLayout layout{root, scene_file};
    if (const auto scene = ini.find("scene"); scene != ini.end() && scene->second.contains("name")) {
        layout.scene_name = scene->second.at("name");
    }
    if (layout.scene_name.empty()) layout.scene_name = path_utf8(root.filename());
    return layout;
}

std::optional<SceneFolderLayout> create_scene_folder(const fs::path& root,
                                                     std::string_view scene_name) {
    if (root.empty() || fs::exists(root)) return std::nullopt;
    const auto staging = root.parent_path() / ("." + root.filename().string() + ".creating");
    std::error_code ec;
    fs::remove_all(staging, ec);
    try {
        fs::create_directories(staging / "Assets");
        std::map<std::string, std::map<std::string, std::string>> ini;
        ini["format"] = {{"type", "corona_scene_folder"}, {"version", "1"}};
        ini["scene"] = {{"name", scene_name.empty() ? root.filename().string() : std::string(scene_name)}};
        ini["sun"] = {{"sun_direction", "1.0, 1.0, 1.0"}, {"enabled", "true"}};
        ini["grid"] = {{"enabled", "true"}};
        ini["actors"] = {};
        ini["scripts"] = {{"path", ""}};
        ini["terrain"] = {{"type", ""}, {"path", ""}};
        write_ini_file(staging / "scene.ini", ini);
        SceneAssetStore store(staging, true);
        if (!store.write_manifest()) throw std::runtime_error("Unable to create asset manifest");
        fs::rename(staging, root);
        return detect_scene_folder(root);
    } catch (...) {
        fs::remove_all(staging, ec);
        return std::nullopt;
    }
}

std::string sha256_file(const fs::path& path) {
    Sha256Hasher hasher;
    hash_file_into(hasher, path);
    return hasher.finish();
}

SceneAssetStore::SceneAssetStore(fs::path scene_root, bool allow_missing_manifest)
    : root_(std::move(scene_root)) {
    std::scoped_lock lock(scene_file_mutex(root_));
    const auto manifest = root_ / "assets.manifest.json";
    if (!fs::is_regular_file(manifest)) {
        if (!allow_missing_manifest && detect_scene_folder(root_)) {
            manifest_error_ = Diagnostic{"invalid_manifest", "Portable asset manifest is missing", manifest};
        }
        return;
    }
    try {
        std::ifstream stream(manifest);
        const auto document = nlohmann::json::parse(stream);
        if (document.value("format", "") != "corona_scene_assets" ||
            document.value("version", 0) != 1 || !document.contains("bundles") ||
            !document["bundles"].is_array()) {
            manifest_error_ = Diagnostic{"invalid_manifest", "Unsupported asset manifest", manifest};
            return;
        }
        for (const auto& item : document["bundles"]) {
            ImportResult bundle;
            bundle.type = item.value("type", "model");
            bundle.bundle_sha256 = item.value("sha256", "");
            bundle.main_route = item.value("main", "");
            if (item.contains("files") && item["files"].is_array()) {
                for (const auto& file_item : item["files"]) {
                    ImportedFile file;
                    file.route = file_item.value("path", "");
                    file.sha256 = file_item.value("sha256", "");
                    file.size = file_item.value("size", std::uint64_t{});
                    bundle.files.push_back(std::move(file));
                }
            }
            if (item.contains("dependencies") && item["dependencies"].is_array()) {
                for (const auto& dependency : item["dependencies"]) {
                    if (dependency.is_string()) bundle.dependencies.push_back(dependency.get<std::string>());
                }
            }
            if (!bundle.bundle_sha256.empty() && !bundle.main_route.empty()) {
                bundles_.push_back(std::move(bundle));
            }
        }
    } catch (const std::exception& error) {
        bundles_.clear();
        manifest_error_ = Diagnostic{"invalid_manifest", error.what(), manifest};
    }
}

ImportResult SceneAssetStore::import_model(const fs::path& source) {
    std::scoped_lock lock(scene_file_mutex(root_));
    ImportResult result;
    if (manifest_error_) {
        result.diagnostics.push_back(*manifest_error_);
        return result;
    }
    static const std::set<std::string> supported{
        ".obj", ".gltf", ".glb", ".fbx", ".dae", ".usd", ".usda", ".usdc"};
    if (!supported.contains(lower(source.extension().string()))) {
        result.diagnostics.push_back({"unsupported_model", "Unsupported model extension", source});
        return result;
    }
    auto files = collect_bundle(fs::absolute(source).lexically_normal(), result.diagnostics);
    if (!result.diagnostics.empty()) return result;
    return import_collected_files(root_, bundles_, std::move(files), "model", "Models", source);
}

ImportResult SceneAssetStore::import_file(const fs::path& source, std::string_view category) {
    std::scoped_lock lock(scene_file_mutex(root_));
    ImportResult result;
    if (manifest_error_) {
        result.diagnostics.push_back(*manifest_error_);
        return result;
    }
    const auto absolute = fs::absolute(source).lexically_normal();
    std::vector<SourceFile> files;
    add_dependency(files, result.diagnostics, absolute, absolute.filename());
    if (!result.diagnostics.empty()) return result;
    return import_collected_files(root_, bundles_, std::move(files), lower(std::string(category)),
                                  std::string(category), source);
}

ImportResult SceneAssetStore::import_actor(const fs::path& actor_source,
                                           const fs::path& model_source) {
    std::scoped_lock lock(scene_file_mutex(root_));
    auto model = import_model(model_source);
    if (!model.ok()) return model;

    ImportResult result;
    const auto preparation = root_ / ".actor-prep";
    const auto prepared_actor = preparation / actor_source.filename();
    std::error_code ec;
    try {
        auto ini = read_ini(actor_source);
        if (!ini.contains("base")) {
            result.diagnostics.push_back({"invalid_actor", "Actor file has no [base] section", actor_source});
            return result;
        }
        ini["base"]["path"] = model.main_route;
        fs::remove_all(preparation, ec);
        write_ini_file(prepared_actor, ini);
        result = import_file(prepared_actor, "Actors");
        if (result.ok()) {
            result.dependencies.push_back(model.main_route);
            const auto stored = std::find_if(bundles_.begin(), bundles_.end(), [&](const ImportResult& bundle) {
                return bundle.bundle_sha256 == result.bundle_sha256 && bundle.type == result.type;
            });
            if (stored != bundles_.end() &&
                std::find(stored->dependencies.begin(), stored->dependencies.end(), model.main_route) ==
                    stored->dependencies.end()) {
                stored->dependencies.push_back(model.main_route);
            }
        }
        fs::remove_all(preparation, ec);
        return result;
    } catch (const std::exception& error) {
        fs::remove_all(preparation, ec);
        result.diagnostics.push_back({"actor_import_failed", error.what(), actor_source});
        return result;
    }
}

bool SceneAssetStore::write_manifest() const {
    std::scoped_lock lock(scene_file_mutex(root_));
    if (manifest_error_) return false;
    try {
        fs::create_directories(root_);
        const auto target = root_ / "assets.manifest.json";
        const auto temporary = root_ / "assets.manifest.json.tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream << manifest_json(bundles_).dump(2) << '\n';
            if (!stream) return false;
        }
        if (!MoveFileExW(temporary.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "Unable to replace asset manifest");
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<Diagnostic> SceneAssetStore::validate_manifest(bool verify_hashes) const {
    std::scoped_lock lock(scene_file_mutex(root_));
    std::vector<Diagnostic> diagnostics;
    const auto manifest_path = root_ / "assets.manifest.json";
    try {
        std::ifstream stream(manifest_path);
        const auto document = nlohmann::json::parse(stream);
        if (document.value("format", "") != "corona_scene_assets" || document.value("version", 0) != 1 ||
            !document.contains("bundles") || !document["bundles"].is_array()) {
            diagnostics.push_back({"invalid_manifest", "Unsupported asset manifest", manifest_path});
            return diagnostics;
        }
        std::set<std::string> manifest_routes;
        for (const auto& bundle : document["bundles"]) {
            if (!bundle.contains("files") || !bundle["files"].is_array()) continue;
            for (const auto& item : bundle["files"]) {
                const auto route = item.value("path", "");
                if (is_valid_asset_route(route)) manifest_routes.insert(route);
            }
        }
        for (const auto& bundle : document["bundles"]) {
            if (!bundle.contains("files") || !bundle["files"].is_array() ||
                !bundle.contains("dependencies") || !bundle["dependencies"].is_array()) {
                diagnostics.push_back({"invalid_manifest", "Bundle has no files", manifest_path});
                continue;
            }
            const auto main_route = bundle.value("main", "");
            if (!is_valid_asset_route(main_route)) {
                diagnostics.push_back({"unsafe_route", "Bundle main resource has an unsafe route",
                                       path_from_utf8(main_route)});
                continue;
            }
            std::set<std::string> listed_routes;
            std::vector<SourceFile> hash_files;
            const auto main_path = path_from_utf8(main_route);
            fs::path bundle_root;
            int component_count = 0;
            for (const auto& component : main_path) {
                if (component_count++ == 3) break;
                bundle_root /= component;
            }
            for (const auto& item : bundle["files"]) {
                const auto route = item.value("path", "");
                if (!is_valid_asset_route(route)) {
                    diagnostics.push_back({"unsafe_route", "Manifest contains an unsafe route", path_from_utf8(route)});
                    continue;
                }
                listed_routes.insert(route);
                const auto path = root_ / path_from_utf8(route);
                if (!resolves_within(root_ / "Assets", path)) {
                    diagnostics.push_back({"unsafe_route", "Manifest asset resolves outside Assets", path});
                } else if (!fs::is_regular_file(path)) {
                    diagnostics.push_back({"missing_asset", "Manifest asset is missing", path});
                } else if (verify_hashes && sha256_file(path) != item.value("sha256", "")) {
                    diagnostics.push_back({"hash_mismatch", "Manifest asset hash mismatch", path});
                } else if (fs::file_size(path) != item.value("size", std::uint64_t{})) {
                    diagnostics.push_back({"size_mismatch", "Manifest asset size mismatch", path});
                } else {
                    std::error_code relative_ec;
                    const auto relative = fs::relative(path_from_utf8(route), bundle_root, relative_ec);
                    if (relative_ec || !is_relative_inside(relative)) {
                        diagnostics.push_back({"unsafe_route", "Manifest file escapes its bundle", path});
                    } else {
                        hash_files.push_back({path, relative});
                    }
                }
            }
            if (!listed_routes.contains(main_route)) {
                diagnostics.push_back({"invalid_manifest", "Bundle main resource is not listed", manifest_path});
            }
            std::set<std::string> expected_dependencies = listed_routes;
            expected_dependencies.erase(main_route);
            std::set<std::string> actual_dependencies;
            for (const auto& dependency : bundle["dependencies"]) {
                if (!dependency.is_string() || !is_valid_asset_route(dependency.get<std::string>())) {
                    diagnostics.push_back({"unsafe_route", "Bundle dependency has an unsafe route", manifest_path});
                    continue;
                }
                const auto route = dependency.get<std::string>();
                actual_dependencies.insert(route);
                if (!manifest_routes.contains(route)) {
                    diagnostics.push_back({"missing_dependency", "Bundle dependency is not listed in the manifest",
                                           path_from_utf8(route)});
                }
            }
            for (const auto& dependency : expected_dependencies) {
                if (!actual_dependencies.contains(dependency)) {
                    diagnostics.push_back({"invalid_dependencies", "Bundle file dependency is not recorded",
                                           path_from_utf8(dependency)});
                }
            }
            if (verify_hashes && hash_files.size() == listed_routes.size() && !hash_files.empty() &&
                bundle_hash(hash_files) != bundle.value("sha256", "")) {
                diagnostics.push_back({"bundle_hash_mismatch", "Manifest bundle hash mismatch", manifest_path});
            }
        }
    } catch (const std::exception& error) {
        diagnostics.push_back({"invalid_manifest", error.what(), manifest_path});
    }
    return diagnostics;
}

SceneValidationResult validate_portable_scene(const fs::path& scene_root,
                                               bool verify_hashes) {
    SceneValidationResult result;
    const auto layout = detect_scene_folder(scene_root);
    if (!layout) {
        result.diagnostics.push_back(
            {"invalid_scene_format", "Expected a portable scene folder version 1", scene_root});
        return result;
    }

    SceneDocumentStore document(layout->root);
    result.diagnostics.insert(result.diagnostics.end(),
                              document.recovery_diagnostics().begin(),
                              document.recovery_diagnostics().end());
    if (!result.diagnostics.empty()) return result;

    SceneAssetStore assets(layout->root);
    result.diagnostics = assets.validate_manifest(verify_hashes);
    try {
        const auto manifest = nlohmann::json::parse(
            std::ifstream(layout->root / "assets.manifest.json"));
        if (manifest.contains("bundles") && manifest["bundles"].is_array()) {
            for (const auto& bundle : manifest["bundles"]) {
                if (!bundle.contains("files") || !bundle["files"].is_array()) continue;
                for (const auto& file : bundle["files"]) {
                    ++result.asset_count;
                    result.total_bytes += file.value("size", std::uint64_t{});
                }
            }
        }
    } catch (...) {
        // validate_manifest already reports the parse failure with its path.
    }
    const auto ini = read_ini(layout->scene_file);
    const auto validate_route = [&](std::string_view route,
                                    std::string field,
                                    std::string actor = {}) {
        if (trim(std::string(route)).empty()) return;
        if (!is_valid_asset_route(route) || !assets.contains_route(route)) {
            result.diagnostics.push_back({"untrusted_asset_route",
                                          "Persisted resource is outside Assets or absent from the manifest",
                                          path_from_utf8(route), std::move(actor), std::move(field)});
        }
    };
    if (const auto actors = ini.find("actors"); actors != ini.end()) {
        for (const auto& [key, value] : actors->second) {
            if (key.ends_with(".route")) {
                const auto actor = key.substr(0, key.size() - std::string(".route").size());
                validate_route(value, "actors." + key, actor);
            } else if (key.ends_with(".material.texture")) {
                const auto actor = key.substr(0, key.size() - std::string(".material.texture").size());
                validate_route(value, "actors." + key, actor);
            }
        }
    }
    const auto validate_section_path = [&](const char* section_name, const char* key) {
        if (const auto section = ini.find(section_name);
            section != ini.end() && section->second.contains(key)) {
            validate_route(section->second.at(key), std::string(section_name) + "." + key);
        }
    };
    validate_section_path("scripts", "path");
    validate_section_path("terrain", "path");
    validate_section_path("vision", "source_path");
    if (const auto section = ini.find("vision_document");
        section != ini.end() && section->second.contains("asset_root")) {
        const auto& root = section->second.at("asset_root");
        if (!root.empty() && root != "Assets") {
            result.diagnostics.push_back({"untrusted_asset_route",
                                          "Vision asset_root must be Assets for a portable scene",
                                          path_from_utf8(root), {}, "vision_document.asset_root"});
        }
        if (section->second.contains("data") && !section->second.at("data").empty()) {
            try {
                const auto document = decode_embedded_vision_document(section->second.at("data"));
                visit_vision_resource_routes(
                    document, "vision_document.data",
                    [&](const std::string& route, const std::string& field) {
                        if (lower(route).rfind("data:", 0) == 0) return;
                        if (route.find("://") != std::string::npos) {
                            result.diagnostics.push_back({"remote_dependency",
                                                          "Remote Vision resources are unsupported",
                                                          path_from_utf8(route), {}, field});
                        } else {
                            validate_route(route, field);
                        }
                    });
            } catch (const std::exception& error) {
                result.diagnostics.push_back({"invalid_vision_document", error.what(), layout->scene_file,
                                              {}, "vision_document.data"});
            }
        }
    }
    return result;
}

namespace {

void strip_bom(std::string& line) {
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
        line.erase(0, 3);
    }
}

std::vector<std::string> read_text_lines(const fs::path& path) {
    std::vector<std::string> lines;
    std::ifstream input(path);
    for (std::string line; std::getline(input, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (lines.empty()) strip_bom(line);
        lines.push_back(std::move(line));
    }
    return lines;
}

bool section_header_matches(const std::string& line, std::string_view section) {
    const auto value = trim(line);
    return value.size() >= 3 && value.front() == '[' && value.back() == ']' &&
           lower(trim(value.substr(1, value.size() - 2))) == lower(std::string(section));
}

bool is_section_header(const std::string& line) {
    const auto value = trim(line);
    return value.size() >= 3 && value.front() == '[' && value.back() == ']';
}

void replace_text_section(std::vector<std::string>& lines,
                          const std::string& section,
                          const std::vector<std::string>& replacement) {
    auto begin = std::find_if(lines.begin(), lines.end(),
                              [&](const auto& line) { return section_header_matches(line, section); });
    if (begin == lines.end()) {
        if (!lines.empty() && !lines.back().empty()) lines.emplace_back();
        lines.insert(lines.end(), replacement.begin(), replacement.end());
        return;
    }
    auto end = std::next(begin);
    while (end != lines.end() && !is_section_header(*end)) ++end;
    const auto insertion = lines.erase(begin, end);
    lines.insert(insertion, replacement.begin(), replacement.end());
}

bool atomic_replace_text(const fs::path& target,
                         const std::vector<std::string>& lines,
                         std::vector<Diagnostic>& diagnostics) {
    const auto temporary = target.parent_path() / ".scene.ini.tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        for (const auto& line : lines) output << line << '\n';
        output.flush();
        if (!output) {
            diagnostics.push_back({"scene_write_failed", "Unable to write scene transaction", temporary});
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        diagnostics.push_back({"scene_replace_failed", "Unable to replace scene.ini", target});
        return false;
    }
    return true;
}

}  // namespace

SceneDocumentStore::SceneDocumentStore(fs::path scene_root) : root_(std::move(scene_root)) {
    std::scoped_lock lock(scene_file_mutex(root_));
    recover_interrupted_transaction();
}

void SceneDocumentStore::recover_interrupted_transaction() {
    const auto marker = root_ / ".scene-save.transaction";
    const auto backup = root_ / ".scene.ini.backup";
    const auto scene = root_ / "scene.ini";
    std::error_code ec;
    if (fs::is_regular_file(marker)) {
        if (!fs::is_regular_file(backup)) {
            recovery_diagnostics_.push_back(
                {"scene_recovery_failed", "Scene transaction backup is missing", backup});
            return;
        }
        fs::copy_file(backup, scene, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            recovery_diagnostics_.push_back({"scene_recovery_failed", ec.message(), backup});
            return;
        }
        fs::remove(marker, ec);
        if (ec) {
            recovery_diagnostics_.push_back({"scene_recovery_failed", ec.message(), marker});
            return;
        }
    }
    fs::remove(backup, ec);
    fs::remove(root_ / ".scene.ini.tmp", ec);
}

bool SceneDocumentStore::replace_sections(
    const std::map<std::string, std::vector<std::string>>& sections,
    std::vector<Diagnostic>& diagnostics) {
    std::scoped_lock lock(scene_file_mutex(root_));
    recovery_diagnostics_.clear();
    recover_interrupted_transaction();
    if (!recovery_diagnostics_.empty()) {
        diagnostics.insert(diagnostics.end(), recovery_diagnostics_.begin(),
                           recovery_diagnostics_.end());
        return false;
    }
    const auto scene = root_ / "scene.ini";
    const auto backup = root_ / ".scene.ini.backup";
    const auto marker = root_ / ".scene-save.transaction";
    auto lines = read_text_lines(scene);
    for (const auto& [name, replacement] : sections) {
        replace_text_section(lines, name, replacement);
    }
    std::error_code ec;
    fs::copy_file(scene, backup, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        diagnostics.push_back({"scene_backup_failed", ec.message(), backup});
        return false;
    }
    {
        std::ofstream output(marker, std::ios::binary | std::ios::trunc);
        output << "scene.ini\n";
        if (!output) {
            diagnostics.push_back({"scene_transaction_failed", "Unable to create transaction marker", marker});
            fs::remove(backup, ec);
            return false;
        }
    }
    if (!atomic_replace_text(scene, lines, diagnostics)) {
        recovery_diagnostics_.clear();
        recover_interrupted_transaction();
        diagnostics.insert(diagnostics.end(), recovery_diagnostics_.begin(),
                           recovery_diagnostics_.end());
        return false;
    }
    fs::remove(marker, ec);
    if (ec) {
        diagnostics.push_back({"scene_commit_failed", ec.message(), marker});
        recovery_diagnostics_.clear();
        recover_interrupted_transaction();
        diagnostics.insert(diagnostics.end(), recovery_diagnostics_.begin(),
                           recovery_diagnostics_.end());
        return false;
    }
    fs::remove(backup, ec);
    return true;
}

CleanupResult cleanup_portable_scene_assets(const fs::path& scene_root, bool dry_run) {
    CleanupResult result;
    std::scoped_lock lock(scene_file_mutex(scene_root));
    const auto layout = detect_scene_folder(scene_root);
    if (!layout) {
        result.diagnostics.push_back(
            {"invalid_scene_format", "Expected a portable scene folder version 1", scene_root});
        return result;
    }
    SceneAssetStore store(layout->root);
    result.diagnostics = store.validate_manifest();
    if (!result.diagnostics.empty()) return result;

    const auto manifest_path = layout->root / "assets.manifest.json";
    nlohmann::json manifest;
    try {
        manifest = nlohmann::json::parse(std::ifstream(manifest_path));
    } catch (const std::exception& error) {
        result.diagnostics.push_back({"invalid_manifest", error.what(), manifest_path});
        return result;
    }

    std::set<std::string> referenced_routes;
    const auto ini = read_ini(layout->scene_file);
    for (const auto& [section_name, section] : ini) {
        for (const auto& [key, value] : section) {
            if (is_valid_asset_route(value)) referenced_routes.insert(value);
        }
    }
    // Embedded Vision paths are compressed inside scene.ini.  Until they are
    // decoded here, conservatively retain every bundle rather than risk data loss.
    if (ini.contains("vision_document") && ini.at("vision_document").contains("data") &&
        !ini.at("vision_document").at("data").empty()) {
        try {
            const auto document = decode_embedded_vision_document(ini.at("vision_document").at("data"));
            visit_vision_resource_routes(document, "vision_document.data",
                                          [&](const std::string& route, const std::string&) {
                                              if (is_valid_asset_route(route)) referenced_routes.insert(route);
                                          });
        } catch (const std::exception& error) {
            result.diagnostics.push_back({"invalid_vision_document", error.what(), layout->scene_file,
                                          {}, "vision_document.data"});
            return result;
        }
    }

    const auto& bundles = manifest["bundles"];
    std::vector<bool> keep(bundles.size(), false);
    const auto bundle_for_route = [&](const std::string& route) -> std::optional<size_t> {
        for (size_t index = 0; index < bundles.size(); ++index) {
            if (!bundles[index].contains("files") || !bundles[index]["files"].is_array()) continue;
            for (const auto& file : bundles[index]["files"]) {
                if (file.value("path", "") == route) return index;
            }
        }
        return std::nullopt;
    };
    for (const auto& route : referenced_routes) {
        if (const auto index = bundle_for_route(route)) keep[*index] = true;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t index = 0; index < bundles.size(); ++index) {
            if (!keep[index] || !bundles[index].contains("dependencies")) continue;
            for (const auto& dependency : bundles[index]["dependencies"]) {
                if (!dependency.is_string()) continue;
                if (const auto owner = bundle_for_route(dependency.get<std::string>());
                    owner && !keep[*owner]) {
                    keep[*owner] = true;
                    changed = true;
                }
            }
        }
    }

    nlohmann::json retained = nlohmann::json::array();
    std::set<fs::path> bundle_directories;
    for (size_t index = 0; index < bundles.size(); ++index) {
        if (keep[index]) {
            retained.push_back(bundles[index]);
            continue;
        }
        ++result.removed_bundles;
        if (bundles[index].contains("files") && bundles[index]["files"].is_array()) {
            for (const auto& file : bundles[index]["files"]) {
                ++result.removed_files;
                result.reclaimed_bytes += file.value("size", std::uint64_t{});
            }
        }
        const auto main = path_from_utf8(bundles[index].value("main", ""));
        fs::path directory;
        size_t components = 0;
        for (const auto& component : main) {
            directory /= component;
            if (++components == 3) break;
        }
        if (components == 3 && is_relative_inside(directory)) bundle_directories.insert(directory);
    }
    if (dry_run || result.removed_bundles == 0) return result;

    manifest["bundles"] = std::move(retained);
    const auto temporary = layout->root / "assets.manifest.json.tmp";
    try {
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output << manifest.dump(2) << '\n';
            output.flush();
            if (!output) throw std::runtime_error("Unable to write cleaned manifest");
        }
        if (!MoveFileExW(temporary.c_str(), manifest_path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "Unable to replace cleaned manifest");
        }
        std::error_code ec;
        for (const auto& directory : bundle_directories) {
            ec.clear();
            fs::remove_all(layout->root / directory, ec);
            if (ec) {
                result.diagnostics.push_back(
                    {"cleanup_failed", ec.message(), layout->root / directory});
            }
        }
    } catch (const std::exception& error) {
        result.diagnostics.push_back({"cleanup_failed", error.what(), layout->root});
    }
    return result;
}

bool SceneAssetStore::contains_route(std::string_view route) const {
    std::scoped_lock lock(scene_file_mutex(root_));
    if (!is_valid_asset_route(route)) return false;
    return std::any_of(bundles_.begin(), bundles_.end(), [&](const ImportResult& bundle) {
        return std::any_of(bundle.files.begin(), bundle.files.end(), [&](const ImportedFile& file) {
            return file.route == route;
        });
    });
}

LegacyMigrationResult migrate_legacy_scene(const LegacyMigrationRequest& request) {
    LegacyMigrationResult result;
    if (request.target_root.empty()) {
        result.diagnostics.push_back({"empty_target", "Target scene folder is empty", request.target_root});
        return result;
    }
    const auto target = fs::absolute(request.target_root).lexically_normal();
    if (fs::exists(target)) {
        result.diagnostics.push_back({"target_exists", "Target scene folder already exists", target});
        return result;
    }
    const auto legacy = locate_legacy_scene(request.source_path, result.diagnostics);
    if (!legacy) return result;

    const auto staging = target.parent_path() / ("." + target.filename().string() + ".migrating");
    std::error_code ec;
    fs::remove_all(staging, ec);
    try {
        fs::create_directories(staging);
        auto ini = read_ini(legacy->scene_file);
        auto name = request.scene_name;
        if (name.empty()) {
            if (const auto base = ini.find("base"); base != ini.end() && base->second.contains("name")) {
                name = base->second.at("name");
            }
        }
        if (name.empty()) name = target.filename().string();
        ini.erase("base");
        ini.erase("project");
        ini["format"] = {{"type", "corona_scene_folder"}, {"version", "1"}};
        ini["scene"] = {{"name", name}};

        SceneAssetStore store(staging);
        if (auto actors = ini.find("actors"); actors != ini.end()) {
            std::vector<std::string> route_keys;
            for (const auto& [key, value] : actors->second) {
                if (key.size() > 6 && key.ends_with(".route")) route_keys.push_back(key);
            }
            for (const auto& route_key : route_keys) {
                const auto actor_key = route_key.substr(0, route_key.size() - 6);
                const auto type_key = actor_key + ".actor_type";
                const auto actor_type = actors->second.contains(type_key) ? actors->second.at(type_key) : "model";
                const auto source = resolve_legacy_route(legacy->project_root, actors->second.at(route_key));
                ImportResult imported;
                if (actor_type == "ui_image") {
                    imported = store.import_file(source, "Images");
                } else if (actor_type == "audio") {
                    imported = store.import_file(source, "Audio");
                } else if (actor_type == "actor" && lower(source.extension().string()) == ".actor") {
                    const auto actor_ini = read_ini(source);
                    const auto model_route = actor_ini.contains("base") && actor_ini.at("base").contains("path")
                                                 ? actor_ini.at("base").at("path")
                                                 : std::string{};
                    if (model_route.empty()) {
                        imported.diagnostics.push_back({"invalid_actor", "Actor file is missing [base].path", source});
                    } else {
                        auto model_source = path_from_utf8(model_route);
                        if (!model_source.is_absolute()) {
                            const auto beside_actor = (source.parent_path() / model_source).lexically_normal();
                            model_source = fs::is_regular_file(beside_actor)
                                               ? beside_actor
                                               : (legacy->project_root / model_source).lexically_normal();
                        }
                        imported = store.import_actor(source, model_source);
                    }
                } else {
                    imported = store.import_model(source);
                }
                if (!imported.ok()) {
                    for (auto diagnostic : imported.diagnostics) {
                        diagnostic.actor = actor_key;
                        result.diagnostics.push_back(std::move(diagnostic));
                    }
                } else {
                    actors->second[route_key] = imported.main_route;
                }
            }
            std::vector<std::string> texture_keys;
            for (const auto& [key, value] : actors->second) {
                if (key.ends_with(".material.texture") && !trim(value).empty()) texture_keys.push_back(key);
            }
            for (const auto& texture_key : texture_keys) {
                const auto source = resolve_legacy_route(legacy->project_root, actors->second.at(texture_key));
                const auto imported = store.import_file(source, "Images");
                if (!imported.ok()) {
                    const auto suffix_size = std::string(".material.texture").size();
                    const auto actor_key = texture_key.substr(0, texture_key.size() - suffix_size);
                    for (auto diagnostic : imported.diagnostics) {
                        diagnostic.actor = actor_key;
                        result.diagnostics.push_back(std::move(diagnostic));
                    }
                } else {
                    actors->second[texture_key] = imported.main_route;
                }
            }
        }

        const auto migrate_section_path = [&](const char* section_name, const char* key,
                                              std::string_view category) {
            auto section = ini.find(section_name);
            if (section == ini.end() || !section->second.contains(key) ||
                trim(section->second.at(key)).empty()) return;
            const auto route = trim(section->second.at(key));
            const auto source = resolve_legacy_route(legacy->project_root, route);
            auto normalized_route = lower(route);
            std::replace(normalized_route.begin(), normalized_route.end(), '\\', '/');
            if (category == "Scripts" && normalized_route == "scripts/scene_script.py" &&
                !fs::is_regular_file(source)) {
                // The legacy project template historically emitted this placeholder
                // even when no scene script was created. Do not turn that stale
                // default into a migration-blocking dependency.
                section->second[key].clear();
                return;
            }
            const auto imported = store.import_file(source, category);
            if (!imported.ok()) {
                result.diagnostics.insert(result.diagnostics.end(), imported.diagnostics.begin(),
                                          imported.diagnostics.end());
            } else {
                section->second[key] = imported.main_route;
            }
        };
        migrate_section_path("scripts", "path", "Scripts");
        migrate_section_path("terrain", "path", "Terrain");
        migrate_section_path("vision", "source_path", "Vision");

        if (!result.diagnostics.empty()) throw std::runtime_error("Legacy assets are incomplete");
        write_ini_file(staging / "scene.ini", ini);
        if (!store.write_manifest()) throw std::runtime_error("Unable to write asset manifest");
        const auto validation = store.validate_manifest();
        if (!validation.empty()) {
            result.diagnostics.insert(result.diagnostics.end(), validation.begin(), validation.end());
            throw std::runtime_error("Migrated assets failed validation");
        }
        if (!detect_scene_folder(staging)) throw std::runtime_error("Migrated scene.ini failed validation");
        fs::rename(staging, target);
        result.root = target;
    } catch (const std::exception& error) {
        if (result.diagnostics.empty()) {
            result.diagnostics.push_back({"migration_failed", error.what(), request.source_path});
        }
        fs::remove_all(staging, ec);
        result.root.clear();
    }
    return result;
}

}  // namespace Corona::Systems::UI::SceneFolders
