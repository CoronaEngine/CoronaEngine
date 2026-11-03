#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Corona::Systems::UI {

struct EmbeddedCameraViewportRequest {
    std::uintptr_t camera_handle{};
    double x{};
    double y{};
    double width{};
    double height{};
    double source_width{};
    double source_height{};
    int render_width{};
    int render_height{};
};

struct EmbeddedCameraViewportBounds {
    std::uintptr_t camera_handle{};
    int x{};
    int y{};
    int width{};
    int height{};
    int render_width{};
    int render_height{};

    bool operator==(const EmbeddedCameraViewportBounds&) const = default;
};

// The request rectangle and source extent share the sender's CEF coordinate space.
// The destination panel rectangle is in physical surface pixels.
[[nodiscard]] inline EmbeddedCameraViewportBounds map_embedded_camera_viewport(
    const EmbeddedCameraViewportRequest& request,
    double panel_x, double panel_y, double panel_width, double panel_height) {
    const double scale_x = panel_width / request.source_width;
    const double scale_y = panel_height / request.source_height;
    const int x = static_cast<int>(std::lround(panel_x + request.x * scale_x));
    const int y = static_cast<int>(std::lround(panel_y + request.y * scale_y));
    const int right = static_cast<int>(std::lround(panel_x + (request.x + request.width) * scale_x));
    const int bottom = static_cast<int>(std::lround(panel_y + (request.y + request.height) * scale_y));
    return {
        .camera_handle = request.camera_handle,
        .x = std::max(x, 0),
        .y = std::max(y, 0),
        .width = std::max(right - x, 1),
        .height = std::max(bottom - y, 1),
        .render_width = request.render_width,
        .render_height = request.render_height,
    };
}

}  // namespace Corona::Systems::UI
