#include "corona/resource/resource.h"

#include <algorithm>
#include <cctype>
#include <iostream>

namespace {
// 将字符串转换为小写
inline std::string to_lower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return str;
}
}  // namespace

std::uint64_t Corona::Resource::IResource::generate_uid(const std::filesystem::path& path) {
    // 使用路径的哈希值作为UID
    return std::filesystem::hash_value(path);
}

Corona::Resource::IResource::IResource(const std::filesystem::path& path)
    : uid(INVALID_UID) {
    this->uid = generate_uid(path);
}

bool Corona::Resource::IResource::valid() const {
    return this->uid != INVALID_UID;
}

std::uint64_t Corona::Resource::IResource::get_uid() const {
    return this->uid;
}

std::shared_ptr<Corona::Resource::IResource> Corona::Resource::IParser::import_from(const std::filesystem::path& path, ResourceCache& cache) {
    // 验证路径和扩展名支持
    if (!is_supported(path)) {
        std::cerr << "Unsupported file extension for path: " << path << std::endl;
        return nullptr;
    }
    if (!std::filesystem::exists(path)) {
        std::cerr << "File does not exist at path: " << path << std::endl;
        return nullptr;
    }

    auto ext = to_lower(path.extension().string());
    if (auto it = supported_extensions_.find(ext); it != supported_extensions_.end()) {
        return it->second(path, cache);
    }

    return nullptr;
}

bool Corona::Resource::IParser::export_to(const IResource& resource, const std::filesystem::path& path) {
    // 验证路径支持
    if (!is_export_supported(path)) {
        std::cerr << "Unsupported export file extension for path: " << path << std::endl;
        return false;
    }

    auto ext = to_lower(path.extension().string());
    if (auto it = supported_export_extensions_.find(ext); it != supported_export_extensions_.end()) {
        return it->second(resource, path);
    }

    return false;
}

bool Corona::Resource::IParser::is_supported(const std::filesystem::path& path) const {
    auto ext = to_lower(path.extension().string());
    return supported_extensions_.contains(ext);
}

bool Corona::Resource::IParser::is_export_supported(const std::filesystem::path& path) const {
    auto ext = to_lower(path.extension().string());
    return supported_export_extensions_.contains(ext);
}

void Corona::Resource::IParser::register_extension(const std::string& extension, ImportHandler handler) {
    supported_extensions_[extension] = std::move(handler);
}

void Corona::Resource::IParser::register_exporter(const std::string& extension, ExportHandler handler) {
    supported_export_extensions_[extension] = std::move(handler);
}
