#pragma once

#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Corona::Systems::UI {

// Shared native IO primitive: Web owns the document's schema and business rules.
// The raw version is compared while holding the same lock as the atomic replace.
inline nlohmann::json project_document_store(const std::filesystem::path& project_root,
                                            const nlohmann::json& payload, bool commit) {
    static std::mutex document_mutex;
    const std::lock_guard<std::mutex> guard(document_mutex);
    const auto root = std::filesystem::weakly_canonical(project_root);
    const auto relative = std::filesystem::u8path(payload.value("document", std::string{}));
    const auto target = std::filesystem::weakly_canonical(root / relative);
    const auto contained = target.lexically_relative(root);
    if (relative.empty() || relative.is_absolute() || contained.empty() ||
        *contained.begin() == ".." || target.extension() != ".json") {
        return nlohmann::json({{"success", false}, {"error", "INVALID_PROJECT_DOCUMENT"}});
    }
    std::string previous;
    if (std::filesystem::exists(target)) {
        if (std::filesystem::file_size(target) > 8 * 1024 * 1024) throw std::runtime_error("Project document exceeds size limit");
        std::ifstream input(target, std::ios::binary);
        if (!input) throw std::runtime_error("Cannot read project document");
        previous.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    const auto parsed = previous.empty() ? nlohmann::json{} : nlohmann::json::parse(previous, nullptr, false);
    if (!commit) {
        return nlohmann::json({{"success", true}, {"version", previous},
            {"content", parsed.is_discarded() ? nlohmann::json{} : parsed},
            {"corrupt", parsed.is_discarded()}});
    }
    if (!payload.contains("version") || payload["version"] != previous) {
        return nlohmann::json({{"success", false}, {"error", "DOCUMENT_CONFLICT"}});
    }
    const auto content = payload.at("content").dump(2);
    if (content.size() > 8 * 1024 * 1024) throw std::runtime_error("Project document exceeds size limit");
    std::filesystem::create_directories(target.parent_path());
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    if (parsed.is_discarded()) {
        auto backup = target;
        backup += std::filesystem::u8path("." + stamp + ".corrupt.json");
        std::filesystem::copy_file(target, backup);
    }
    auto temporary = target;
    temporary += std::filesystem::u8path("." + stamp + ".tmp");
    try {
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(content.data(), static_cast<std::streamsize>(content.size()));
            output.flush();
            if (!output) throw std::runtime_error("Cannot write project document");
        }
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("Cannot commit project document");
        }
#else
        std::filesystem::rename(temporary, target);
#endif
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
    return nlohmann::json({{"success", true}, {"version", content}});
}

}  // namespace Corona::Systems::UI
