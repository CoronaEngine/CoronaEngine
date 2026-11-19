#pragma once

#include <corona/shared_data_hub.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace Corona::Systems::Vision {

struct VisionPipelineKey {
    CameraVisionRenderMode mode{CameraVisionRenderMode::PathTracing};
    bool participating_media{false};
    std::shared_ptr<const nlohmann::json> authored_media;
    std::size_t authored_media_revision{0};

    friend bool operator==(const VisionPipelineKey& lhs,
                           const VisionPipelineKey& rhs) noexcept = default;
};

struct VisionPipelineKeyHash {
    [[nodiscard]] std::size_t operator()(const VisionPipelineKey& key) const noexcept {
        auto seed = std::hash<int>{}(static_cast<int>(key.mode));
        const auto mix = [&seed](std::size_t value) {
            seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
        };
        mix(key.participating_media);
        mix(std::hash<const nlohmann::json*>{}(key.authored_media.get()));
        mix(key.authored_media_revision);
        return seed;
    }
};

[[nodiscard]] constexpr bool vision_media_enabled(CameraVisionRenderMode mode, bool accumulation,
                                                 bool denoise, bool requested) noexcept {
    return requested && accumulation && !denoise &&
           (mode == CameraVisionRenderMode::PathTracing ||
            mode == CameraVisionRenderMode::ProgressivePathTracing);
}

}  // namespace Corona::Systems::Vision
