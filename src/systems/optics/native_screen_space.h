#pragma once
#include <algorithm>
#include <cstdint>

namespace Corona::Systems::OpticsDetail {
// Scene-independent work/memory budget. 8K uses quarter-resolution GI; all
// final shading/material evaluation still uses the full-resolution VBuffer.
struct ScreenSpaceLayout {
    uint32_t width, height, stride, levels;
    uint64_t hierarchy_words;
    static ScreenSpaceLayout make(uint32_t width, uint32_t height) {
        const uint32_t stride=std::max(2u,(std::max(width,height)+1919u)/1920u);
        ScreenSpaceLayout result{(width+stride-1u)/stride,(height+stride-1u)/stride,stride,1u,0u};
        while(width>1u || height>1u) {
            width=(width+1u)/2u;height=(height+1u)/2u;
            result.hierarchy_words+=uint64_t(width)*height;
            ++result.levels;
        }
        // One-word allocation for the 1x1 degenerate case; L0 is the existing depth.
        result.hierarchy_words=std::max(uint64_t(1),result.hierarchy_words);
        return result;
    }
    uint64_t bytes() const {return hierarchy_words*4u+uint64_t(width)*height*48u;}
};
} // namespace Corona::Systems::OpticsDetail
