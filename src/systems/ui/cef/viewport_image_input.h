#pragma once

#include "../../common/aspect_fit.h"

#include <cmath>
#include <optional>

namespace Corona::Systems::UI::InputDetail {

struct ImagePoint {
    double x, y;
};

// Input coordinates may be CSS pixels (picking/gizmo) or frontend render pixels
// (the stereo cursor). The camera rectangle is in native physical pixels.
template <typename Camera>
[[nodiscard]] std::optional<ImagePoint> map_viewport_image_point(
    const Camera& camera, double x, double y, double input_width, double input_height) {
    if (!std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(input_width) || !std::isfinite(input_height) ||
        input_width <= 0.0 || input_height <= 0.0 ||
        camera.width == 0 || camera.height == 0) return std::nullopt;

    const bool embedded = camera.viewport_rect_active && !camera.view_open;
    const auto width = embedded ? camera.view_width : static_cast<double>(camera.width);
    const auto height = embedded ? camera.view_height : static_cast<double>(camera.height);
    if (width <= 0 || height <= 0) return std::nullopt;
    const auto viewport_width = static_cast<std::uint32_t>(width);
    const auto viewport_height = static_cast<std::uint32_t>(height);
    const auto fitted = RenderDetail::fit_image(
        {0, 0, viewport_width, viewport_height}, camera.width, camera.height);
    const double pixel_x = x * viewport_width / input_width;
    const double pixel_y = y * viewport_height / input_height;
    if (pixel_x < fitted.x || pixel_y < fitted.y ||
        pixel_x >= double(fitted.x) + fitted.width ||
        pixel_y >= double(fitted.y) + fitted.height) return std::nullopt;

    return ImagePoint{(pixel_x - fitted.x) * camera.width / fitted.width,
                      (pixel_y - fitted.y) * camera.height / fitted.height};
}

}  // namespace Corona::Systems::UI::InputDetail
