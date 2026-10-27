#pragma once

#include <corona/shared_data_hub.h>

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace Corona::Systems::Vision {

struct VisionPipelineKey {
    CameraVisionRenderMode mode{CameraVisionRenderMode::PathTracing};

    friend bool operator==(const VisionPipelineKey& lhs,
                           const VisionPipelineKey& rhs) noexcept {
        return lhs.mode == rhs.mode;
    }
};

struct VisionPipelineKeyHash {
    [[nodiscard]] std::size_t operator()(const VisionPipelineKey& key) const noexcept {
        return std::hash<int>{}(static_cast<int>(key.mode));
    }
};


}  // namespace Corona::Systems::Vision
