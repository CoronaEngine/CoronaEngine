#include "popup_overlay.h"

#include <algorithm>
#include <cstring>

namespace Corona::Systems::UI {
namespace {

constexpr std::size_t kChannels = 4;

}  // namespace

void PopupOverlay::set_visible(bool visible) {
    visible_ = visible;
}

void PopupOverlay::set_rect(int x, int y, int width, int height) {
    x_ = x;
    y_ = y;
    if (width > 0 && height > 0) {
        width_ = width;
        height_ = height;
    }
}

void PopupOverlay::update_pixels(const void* bgra, int width, int height) {
    if (bgra == nullptr || width <= 0 || height <= 0) {
        return;
    }

    const auto* src = static_cast<const std::uint8_t*>(bgra);
    const std::size_t pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    pixels_.resize(pixel_count * kChannels);

    for (std::size_t i = 0; i < pixel_count; ++i) {
        pixels_[i * kChannels + 0] = src[i * kChannels + 2];
        pixels_[i * kChannels + 1] = src[i * kChannels + 1];
        pixels_[i * kChannels + 2] = src[i * kChannels + 0];
        pixels_[i * kChannels + 3] = src[i * kChannels + 3];
    }

    width_ = width;
    height_ = height;
}

bool PopupOverlay::empty() const noexcept {
    return !visible_ || pixels_.empty() || width_ <= 0 || height_ <= 0;
}

bool PopupOverlay::composite_over(std::span<std::uint8_t> view_rgba, int view_width, int view_height) const {
    if (!visible_ || pixels_.empty() || width_ <= 0 || height_ <= 0) {
        return false;
    }
    if (view_width <= 0 || view_height <= 0) {
        return false;
    }

    const std::size_t required =
        static_cast<std::size_t>(view_width) * static_cast<std::size_t>(view_height) * kChannels;
    if (view_rgba.size() < required) {
        return false;
    }

    const int dst_x0 = std::max(x_, 0);
    const int dst_y0 = std::max(y_, 0);
    const int dst_x1 = std::min(x_ + width_, view_width);
    const int dst_y1 = std::min(y_ + height_, view_height);
    if (dst_x1 <= dst_x0 || dst_y1 <= dst_y0) {
        return false;
    }

    const std::size_t row_bytes = static_cast<std::size_t>(dst_x1 - dst_x0) * kChannels;
    for (int y = dst_y0; y < dst_y1; ++y) {
        const int src_y = y - y_;
        const int src_x = dst_x0 - x_;
        const std::uint8_t* src_row =
            pixels_.data() + (static_cast<std::size_t>(src_y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(src_x)) * kChannels;
        std::uint8_t* dst_row =
            view_rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(view_width) + static_cast<std::size_t>(dst_x0)) * kChannels;
        std::memcpy(dst_row, src_row, row_bytes);
    }
    return true;
}

void PopupOverlay::clear() {
    visible_ = false;
    x_ = 0;
    y_ = 0;
    width_ = 0;
    height_ = 0;
    pixels_.clear();
}

}  // namespace Corona::Systems::UI
