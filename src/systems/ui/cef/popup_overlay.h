#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace Corona::Systems::UI {

class PopupOverlay {
   public:
    void set_visible(bool visible);

    void set_rect(int x, int y, int width, int height);

    void update_pixels(const void* bgra, int width, int height);

    [[nodiscard]] bool visible() const noexcept { return visible_; }

    [[nodiscard]] bool empty() const noexcept;

    [[nodiscard]] int x() const noexcept { return x_; }
    [[nodiscard]] int y() const noexcept { return y_; }
    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }
    [[nodiscard]] const std::vector<std::uint8_t>& pixels() const noexcept { return pixels_; }

    [[nodiscard]] bool composite_over(std::span<std::uint8_t> view_rgba,
                                      int view_width,
                                      int view_height) const;

    void clear();

   private:
    bool visible_ = false;
    int x_ = 0;
    int y_ = 0;
    int width_ = 0;
    int height_ = 0;
    std::vector<std::uint8_t> pixels_;
};

}  // namespace Corona::Systems::UI
