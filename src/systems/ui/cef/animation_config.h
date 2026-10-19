#pragma once
#include <corona/resource/types/animation_player.h>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace Corona::Systems::UI::AnimationConfig {
inline std::vector<std::string> parse_sources(const nlohmann::json& value) {
    if (!value.is_array()) throw std::invalid_argument("animation_sources must be an array of file paths");
    std::vector<std::string> result;
    std::set<std::string> unique;
    for (const auto& entry : value) {
        if (!entry.is_string() || entry.get_ref<const std::string&>().empty() ||
            entry.get_ref<const std::string&>().find('\0') != std::string::npos)
            throw std::invalid_argument("animation_sources entries must be non-empty paths");
        auto path = std::filesystem::u8path(entry.get<std::string>()).lexically_normal().generic_u8string();
        std::string source(reinterpret_cast<const char*>(path.data()), path.size());
        if (!unique.insert(source).second) throw std::invalid_argument("Duplicate animation source: " + source);
        result.push_back(std::move(source));
    }
    return result;
}
inline nlohmann::json serialize(const Resource::AnimationSnapshot& snapshot) {
    auto clips = nlohmann::json::array();
    for (const auto& clip : snapshot.clips)
        clips.push_back({{"id", clip.id}, {"name", clip.name}, {"source", clip.source},
                         {"source_index", clip.source_index}, {"duration_seconds", clip.duration_seconds}});
    const auto nullable = [](const std::string& value) -> nlohmann::json {
        return value.empty() ? nlohmann::json(nullptr) : nlohmann::json(value);
    };
    return {{"animation_sources", snapshot.sources}, {"animations", std::move(clips)},
            {"animation", {{"current", nullable(snapshot.current)}, {"target", nullable(snapshot.target)},
                           {"transitioning", !snapshot.target.empty()}, {"paused", snapshot.paused},
                           {"time_seconds", snapshot.time_seconds},
                           {"transition_weight", snapshot.transition_weight}, {"load_status", snapshot.load_status},
                           {"load_error", nullable(snapshot.load_error)}, {"requested_sources", snapshot.requested_sources}}}};
}
} // namespace Corona::Systems::UI::AnimationConfig
