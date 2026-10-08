#pragma once

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace vision::evaluation_debug {

[[nodiscard]] inline bool denoiser_disabled() noexcept {
    const char *value = std::getenv("VISION_DISABLE_DENOISER");
    return value && value[0] != '\0' && value[0] != '0';
}

// Shared by the pre/post-denoise hook and the post-render guide readback.
// The original single-frame mode keeps its flat directory layout.
[[nodiscard]] inline std::filesystem::path directory(unsigned frame) {
    const char *root = std::getenv("VISION_EVAL_DEBUG_DIR");
    if (!root || !root[0]) return {};
    const char *frames = std::getenv("VISION_EVAL_DEBUG_FRAMES");
    const bool multiple = frames && frames[0];
    if (!multiple) frames = std::getenv("VISION_EVAL_DEBUG_FRAME");
    if (!frames || !frames[0]) return {};
    std::string list(frames);
    for (char &c : list) if (c == ',') c = ' ';
    std::istringstream input(list);
    std::string token;
    bool selected = false;
    while (input >> token) {
        unsigned value{};
        const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size())
            throw std::runtime_error("Invalid VISION_EVAL_DEBUG_FRAME(S)");
        selected |= value == frame;
    }
    if (!selected) return {};
    if (!multiple) return root;
    std::ostringstream name;
    name << "frame_" << std::setw(4) << std::setfill('0') << frame;
    return std::filesystem::path(root) / name.str();
}

}// namespace vision::evaluation_debug
