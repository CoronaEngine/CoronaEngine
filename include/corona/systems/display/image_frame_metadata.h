#pragma once

#include <cstdint>

namespace Corona::Systems::Detail {

/** Metadata published under the same storage lock as its image and submit receipt. */
struct ImageFrameMetadata {
    uint64_t frame_index = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t viewport_x = 0;
    uint32_t viewport_y = 0;
    uint32_t viewport_width = 0;
    uint32_t viewport_height = 0;
};

}  // namespace Corona::Systems::Detail
