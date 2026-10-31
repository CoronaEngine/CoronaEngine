#pragma once

#include <corona/systems/display/image_frame_metadata.h>

namespace Corona::Systems::Detail {

struct SceneViewport {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;

    bool operator==(const SceneViewport&) const = default;
};

[[nodiscard]] inline SceneViewport scene_viewport_for_composite(
    const ImageFrameMetadata& ui, const ImageFrameMetadata& optics,
    uint32_t output_width, uint32_t output_height) {
    // UI placement is current even while Optics is preparing a resized image.
    if (ui.viewport_width != 0 && ui.viewport_height != 0) {
        return {ui.viewport_x, ui.viewport_y, ui.viewport_width, ui.viewport_height};
    }
    return {optics.viewport_x, optics.viewport_y,
            optics.viewport_width != 0 ? optics.viewport_width : output_width,
            optics.viewport_height != 0 ? optics.viewport_height : output_height};
}

}  // namespace Corona::Systems::Detail
