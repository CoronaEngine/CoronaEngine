#pragma once

#include <algorithm>
#include <cstdint>

namespace Corona::Systems::RenderDetail {

struct PixelRect {
    std::uint32_t x, y, width, height;
};

// Shared by presentation and input so odd pixel rounding agrees at both edges.
[[nodiscard]] inline PixelRect fit_image(PixelRect viewport,
                                         std::uint32_t source_width,
                                         std::uint32_t source_height) {
    const auto width = std::uint64_t{viewport.width};
    const auto height = std::uint64_t{viewport.height};
    auto fitted = viewport;
    if (width * source_height > height * source_width) {
        fitted.width = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
            (height * source_width + source_height / 2) / source_height, 1, width));
        fitted.x += (viewport.width - fitted.width) / 2;
    } else {
        fitted.height = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
            (width * source_height + source_width / 2) / source_width, 1, height));
        fitted.y += (viewport.height - fitted.height) / 2;
    }
    return fitted;
}

}  // namespace Corona::Systems::RenderDetail
