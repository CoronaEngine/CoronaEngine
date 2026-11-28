#include "corona/resource/types/text.h"

#include <fstream>

namespace Corona::Resource {

Text::Text(const std::filesystem::path& path) : IResource(path) {}

TextParser::TextParser() {
    register_extension(".txt", [this](const auto& path, ResourceCache& cache) { return parse_text(path); });
    register_extension(".log", [this](const auto& path, ResourceCache& cache) { return parse_text(path); });
    register_extension(".xml", [this](const auto& path, ResourceCache& cache) { return parse_text(path); });
    register_extension(".ini", [this](const auto& path, ResourceCache& cache) { return parse_text(path); });
    register_extension(".cfg", [this](const auto& path, ResourceCache& cache) { return parse_text(path); });
    register_extension(".md", [this](const auto& path, ResourceCache& cache) { return parse_text(path); });
    register_extension(".glsl", [this](const auto& path, ResourceCache& cache) { return parse_text(path); });
}

std::shared_ptr<IResource> TextParser::parse_text(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return nullptr;
    }

    auto ptr = std::make_shared<Text>(path);

    try {
        ptr->text = read_string_file(path);
    } catch (...) {
        return nullptr;
    }
    return ptr;
}

std::string TextParser::read_string_file(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open the file.");
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    file.close();
    return buffer.str();
}

}  // namespace Corona::Resource