#include "corona/resource/parser_registry.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace Corona::Resource {

namespace {

// 将字符串转换为小写
inline std::string to_lower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return str;
}

/// 共享锁下按扩展名查表；命中后拷贝 shared_ptr 即可放锁。
std::shared_ptr<IParser> lookup_extension(
    const std::unordered_map<std::string, std::shared_ptr<IParser>>& table,
    std::shared_mutex& mutex,
    const std::string& ext) {
    if (ext.empty()) return nullptr;
    std::shared_lock lock(mutex);
    const auto it = table.find(ext);
    return it == table.end() ? nullptr : it->second;
}

}  // namespace

void ParserRegistry::clear() {
    std::unique_lock lock(mutex_);
    parsers_.clear();
    parser_registry_.clear();
    export_parser_registry_.clear();
}

bool ParserRegistry::register_parser_impl(std::shared_ptr<IParser> parser) {
    if (!parser) {
        return false;
    }

    std::unique_lock lock(mutex_);

    // 同类型解析器只允许注册一次
    const auto it = std::ranges::find_if(parsers_,
                                         [&parser](const std::shared_ptr<IParser>& p) {
                                             return typeid(*p) == typeid(*parser);
                                         });
    if (it != parsers_.end()) {
        return false;
    }
    parsers_.push_back(parser);

    // 扩展名冲突时保留首次注册者（与原 insert-if-absent 语义一致）
    for (const auto& ext : parser->get_supported_extensions()) {
        parser_registry_.try_emplace(to_lower(ext.first), parser);
    }
    for (const auto& ext : parser->get_supported_export_extensions()) {
        export_parser_registry_.try_emplace(to_lower(ext.first), parser);
    }

    return true;
}

std::shared_ptr<IParser> ParserRegistry::find_parser(const std::filesystem::path& path) {
    const auto ext = to_lower(path.extension().string());

    if (auto cached = lookup_extension(parser_registry_, mutex_, ext)) {
        return cached;
    }

    // 扩展名未命中：先拷贝候选列表再放锁，避免在锁内做可能读文件内容的探测。
    std::vector<std::shared_ptr<IParser>> candidates;
    {
        std::shared_lock lock(mutex_);
        candidates = parsers_;
    }

    std::shared_ptr<IParser> found;
    for (const auto& parser : candidates) {
        if (parser->is_supported(path)) {
            found = parser;
            break;
        }
    }
    if (!found) {
        return nullptr;
    }

    if (!ext.empty()) {
        std::unique_lock lock(mutex_);
        parser_registry_.try_emplace(ext, found);
    }
    return found;
}

std::shared_ptr<IParser> ParserRegistry::find_export_parser(const std::filesystem::path& path) {
    const auto ext = to_lower(path.extension().string());

    if (auto cached = lookup_extension(export_parser_registry_, mutex_, ext)) {
        return cached;
    }

    std::vector<std::shared_ptr<IParser>> candidates;
    {
        std::shared_lock lock(mutex_);
        candidates = parsers_;
    }

    std::shared_ptr<IParser> found;
    for (const auto& parser : candidates) {
        if (parser->is_export_supported(path)) {
            found = parser;
            break;
        }
    }
    if (!found) {
        return nullptr;
    }

    if (!ext.empty()) {
        std::unique_lock lock(mutex_);
        export_parser_registry_.try_emplace(ext, found);
    }
    return found;
}

}  // namespace Corona::Resource
