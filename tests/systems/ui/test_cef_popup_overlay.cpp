
#include "cef/popup_overlay.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
    if (condition) {
        return;
    }
    std::cerr << "PopupOverlayTests failed: " << message << '\n';
    std::exit(1);
}

using Corona::Systems::UI::PopupOverlay;

std::vector<std::uint8_t> make_view(int width, int height) {
    return std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4u, 0xAB);
}

std::uint8_t pixel_channel(const std::vector<std::uint8_t>& buffer, int width, int x, int y, int channel) {
    return buffer[(static_cast<std::size_t>(y) * width + x) * 4u + channel];
}

bool pixel_equals(const std::vector<std::uint8_t>& buffer, int width, int x, int y,
                  std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    return pixel_channel(buffer, width, x, y, 0) == r &&
           pixel_channel(buffer, width, x, y, 1) == g &&
           pixel_channel(buffer, width, x, y, 2) == b &&
           pixel_channel(buffer, width, x, y, 3) == a;
}

bool pixel_is_sentinel(const std::vector<std::uint8_t>& buffer, int width, int x, int y) {
    return pixel_equals(buffer, width, x, y, 0xAB, 0xAB, 0xAB, 0xAB);
}

std::vector<std::uint8_t> make_bgra_2x2() {
    return {
        10, 20, 30, 255,
        40, 50, 60, 128,
        70, 80, 90, 255,
        100, 110, 120, 255
    };
}

void test_default_overlay_is_hidden() {
    PopupOverlay overlay;
    require(!overlay.visible(), "fresh overlay must be hidden");
    require(overlay.empty(), "fresh overlay must be empty");
    auto view = make_view(4, 4);
    require(!overlay.composite_over(view, 4, 4), "hidden overlay must not composite");
    require(pixel_is_sentinel(view, 4, 0, 0), "hidden overlay must not touch pixels");
}

void test_update_pixels_converts_bgra_to_rgba_and_keeps_alpha() {
    PopupOverlay overlay;
    overlay.set_rect(0, 0, 2, 2);
    overlay.set_visible(true);
    auto bgra = make_bgra_2x2();
    overlay.update_pixels(bgra.data(), 2, 2);

    require(!overlay.empty(), "overlay with pixels must not be empty");
    const auto& pixels = overlay.pixels();
    require(pixels.size() == 16, "overlay must store RGBA bytes for every pixel");
    require(pixels[0] == 30 && pixels[1] == 20 && pixels[2] == 10 && pixels[3] == 255,
            "first pixel must be RGBA(30,20,10,255)");
    require(pixels[7] == 128, "alpha must be preserved, not forced opaque");
}

void test_composite_writes_at_rect_offset() {
    PopupOverlay overlay;
    overlay.set_rect(1, 1, 2, 2);
    overlay.set_visible(true);
    auto bgra = make_bgra_2x2();
    overlay.update_pixels(bgra.data(), 2, 2);

    auto view = make_view(4, 4);
    require(overlay.composite_over(view, 4, 4), "visible overlay must composite");

    require(pixel_equals(view, 4, 1, 1, 30, 20, 10, 255), "popup origin pixel misplaced");
    require(pixel_equals(view, 4, 2, 1, 60, 50, 40, 128), "popup top-right pixel misplaced");
    require(pixel_equals(view, 4, 1, 2, 90, 80, 70, 255), "popup bottom-left pixel misplaced");
    require(pixel_equals(view, 4, 2, 2, 120, 110, 100, 255), "popup bottom-right pixel misplaced");

    require(pixel_is_sentinel(view, 4, 0, 0), "pixel left of popup must be untouched");
    require(pixel_is_sentinel(view, 4, 3, 3), "pixel below-right of popup must be untouched");
}

void test_composite_clips_at_bottom_right_edge() {
    PopupOverlay overlay;
    overlay.set_rect(3, 3, 2, 2);
    overlay.set_visible(true);
    auto bgra = make_bgra_2x2();
    overlay.update_pixels(bgra.data(), 2, 2);

    auto view = make_view(4, 4);
    require(overlay.composite_over(view, 4, 4), "partially visible overlay must composite");
    require(pixel_equals(view, 4, 3, 3, 30, 20, 10, 255), "clipped origin pixel must be written");
    require(pixel_is_sentinel(view, 4, 0, 0), "clipping must not spill into other pixels");
}

void test_composite_clips_at_top_left_edge() {
    PopupOverlay overlay;
    overlay.set_rect(-1, -1, 2, 2);
    overlay.set_visible(true);
    auto bgra = make_bgra_2x2();
    overlay.update_pixels(bgra.data(), 2, 2);

    auto view = make_view(4, 4);
    require(overlay.composite_over(view, 4, 4), "overlay overlapping top-left must composite");
    require(pixel_equals(view, 4, 0, 0, 120, 110, 100, 255), "bottom-right popup pixel must land at 0,0");
    require(pixel_is_sentinel(view, 4, 1, 0), "no spill to the right");
    require(pixel_is_sentinel(view, 4, 0, 1), "no spill below");
}

void test_composite_returns_false_when_fully_outside() {
    PopupOverlay overlay;
    overlay.set_rect(10, 10, 2, 2);
    overlay.set_visible(true);
    auto bgra = make_bgra_2x2();
    overlay.update_pixels(bgra.data(), 2, 2);

    auto view = make_view(4, 4);
    require(!overlay.composite_over(view, 4, 4), "fully outside overlay must not composite");
    require(pixel_is_sentinel(view, 4, 0, 0), "fully outside overlay must not touch pixels");
}

void test_hidden_overlay_stops_compositing_but_keeps_pixels() {
    PopupOverlay overlay;
    overlay.set_rect(0, 0, 2, 2);
    overlay.set_visible(true);
    auto bgra = make_bgra_2x2();
    overlay.update_pixels(bgra.data(), 2, 2);

    overlay.set_visible(false);
    require(!overlay.visible(), "overlay must be hidden after set_visible(false)");
    auto view = make_view(4, 4);
    require(!overlay.composite_over(view, 4, 4), "hidden overlay must not composite");
    require(pixel_is_sentinel(view, 4, 0, 0), "hidden overlay must not touch pixels");

    overlay.set_visible(true);
    require(overlay.composite_over(view, 4, 4), "re-shown overlay must composite again");
}

void test_clear_removes_state() {
    PopupOverlay overlay;
    overlay.set_rect(0, 0, 2, 2);
    overlay.set_visible(true);
    auto bgra = make_bgra_2x2();
    overlay.update_pixels(bgra.data(), 2, 2);
    overlay.clear();

    require(!overlay.visible(), "cleared overlay must be hidden");
    require(overlay.empty(), "cleared overlay must be empty");
    require(overlay.pixels().empty(), "cleared overlay must release pixels");
    auto view = make_view(4, 4);
    require(!overlay.composite_over(view, 4, 4), "cleared overlay must not composite");
}

void test_invalid_inputs_are_ignored() {
    PopupOverlay overlay;
    overlay.set_visible(true);

    auto bgra = make_bgra_2x2();
    overlay.update_pixels(nullptr, 2, 2);
    require(overlay.empty(), "null pixel source must be ignored");
    overlay.update_pixels(bgra.data(), 0, 2);
    require(overlay.empty(), "zero-width pixel source must be ignored");
    overlay.update_pixels(bgra.data(), 2, -1);
    require(overlay.empty(), "negative-height pixel source must be ignored");

    auto view = make_view(4, 4);
    require(!overlay.composite_over(view, 4, 4), "overlay without pixels must not composite");
}

void test_undersized_view_buffer_is_rejected() {
    PopupOverlay overlay;
    overlay.set_rect(0, 0, 2, 2);
    overlay.set_visible(true);
    auto bgra = make_bgra_2x2();
    overlay.update_pixels(bgra.data(), 2, 2);

    std::vector<std::uint8_t> too_small(8, 0xAB);
    require(!overlay.composite_over(too_small, 4, 4), "undersized view buffer must be rejected");
    require(too_small[0] == 0xAB, "undersized view buffer must stay untouched");
}

void test_pixel_source_overrides_declared_size() {
    PopupOverlay overlay;
    overlay.set_rect(0, 0, 4, 4);
    overlay.set_visible(true);
    auto bgra = make_bgra_2x2();
    overlay.update_pixels(bgra.data(), 2, 2);

    require(overlay.width() == 2 && overlay.height() == 2,
            "overlay dimensions must follow the delivered pixel buffer");

    auto view = make_view(4, 4);
    require(overlay.composite_over(view, 4, 4), "overlay must composite");
    require(pixel_equals(view, 4, 1, 1, 120, 110, 100, 255), "delivered 2x2 pixels must be used");
    require(pixel_is_sentinel(view, 4, 2, 2), "declared 4x4 must not be trusted");
}

}  // namespace

int main() {
    test_default_overlay_is_hidden();
    test_update_pixels_converts_bgra_to_rgba_and_keeps_alpha();
    test_composite_writes_at_rect_offset();
    test_composite_clips_at_bottom_right_edge();
    test_composite_clips_at_top_left_edge();
    test_composite_returns_false_when_fully_outside();
    test_hidden_overlay_stops_compositing_but_keeps_pixels();
    test_clear_removes_state();
    test_invalid_inputs_are_ignored();
    test_undersized_view_buffer_is_rejected();
    test_pixel_source_overrides_declared_size();
    std::cout << "PopupOverlayTests passed\n";
    return 0;
}
