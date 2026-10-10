
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
using Corona::Systems::UI::compose_pending_popup_frame;

std::vector<std::uint8_t> make_view(int width, int height) {
    return std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4u, 0xAB);
}

std::uint8_t pixel_channel(const std::vector<std::uint8_t>& buffer, int width, int x, int y, int channel) {
    return buffer[(static_cast<std::size_t>(y) * width + x) * 4u + channel];
}

// 参数是**字节序**（c0..c3），不是 RGBA 语义：CEF 位图在整条管线里都是 BGRA，
// 第 0 字节是蓝、第 2 字节是红。按字节断言可以避免"语义顺序"被悄悄改掉。
bool pixel_equals(const std::vector<std::uint8_t>& buffer, int width, int x, int y,
                  std::uint8_t c0, std::uint8_t c1, std::uint8_t c2, std::uint8_t c3) {
    return pixel_channel(buffer, width, x, y, 0) == c0 &&
           pixel_channel(buffer, width, x, y, 1) == c1 &&
           pixel_channel(buffer, width, x, y, 2) == c2 &&
           pixel_channel(buffer, width, x, y, 3) == c3;
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

void test_update_pixels_keeps_bgra_bytes_and_alpha() {
    PopupOverlay overlay;
    overlay.set_rect(0, 0, 2, 2);
    overlay.set_visible(true);
    auto bgra = make_bgra_2x2();
    overlay.update_pixels(bgra.data(), 2, 2);

    require(!overlay.empty(), "overlay with pixels must not be empty");
    const auto& pixels = overlay.pixels();
    require(pixels.size() == 16, "overlay must store 4 bytes for every pixel");
    // 管线全程 BGRA：popup 像素必须原样保留，不能就地转成 RGBA，
    // 否则 composite_over() 把它们 memcpy 进同为 BGRA 的 view 缓冲后颜色会对调。
    require(pixels == bgra, "popup pixels must be stored byte-for-byte as delivered (BGRA)");
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

    // 断言的是字节序：popup 的 BGRA 像素现在原样落进同为 BGRA 的 view 缓冲。
    require(pixel_equals(view, 4, 1, 1, 10, 20, 30, 255), "popup origin pixel misplaced");
    require(pixel_equals(view, 4, 2, 1, 40, 50, 60, 128), "popup top-right pixel misplaced");
    require(pixel_equals(view, 4, 1, 2, 70, 80, 90, 255), "popup bottom-left pixel misplaced");
    require(pixel_equals(view, 4, 2, 2, 100, 110, 120, 255), "popup bottom-right pixel misplaced");

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
    require(pixel_equals(view, 4, 3, 3, 10, 20, 30, 255), "clipped origin pixel must be written");
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
    require(pixel_equals(view, 4, 0, 0, 100, 110, 120, 255), "bottom-right popup pixel must land at 0,0");
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
    require(pixel_equals(view, 4, 1, 1, 100, 110, 120, 255), "delivered 2x2 pixels must be used");
    require(pixel_is_sentinel(view, 4, 2, 2), "declared 4x4 must not be trusted");
}

void test_popup_scroll_and_hide_publish_without_another_view_paint() {
    const auto base = make_view(4, 4);
    PopupOverlay overlay;
    std::vector<std::uint8_t> frame;
    bool dirty = true;
    require(compose_pending_popup_frame(base, 4, 4, overlay, dirty, frame),
            "initial view must publish");
    require(!dirty && frame == base, "initial publication must consume only the dirty flag");

    overlay.set_rect(1, 1, 2, 2);
    overlay.set_visible(true);
    auto popup = make_bgra_2x2();
    overlay.update_pixels(popup.data(), 2, 2);
    dirty = true;
    require(compose_pending_popup_frame(base, 4, 4, overlay, dirty, frame),
            "opening a popup must publish without a new view paint");
    require(pixel_equals(frame, 4, 1, 1, 10, 20, 30, 255), "opened popup must be visible");

    popup[0] = 200;
    overlay.update_pixels(popup.data(), 2, 2);
    dirty = true;
    require(compose_pending_popup_frame(base, 4, 4, overlay, dirty, frame),
            "popup-only scrolling must publish each updated frame");
    require(pixel_equals(frame, 4, 1, 1, 200, 20, 30, 255), "scrolled popup must replace old pixels");

    overlay.set_visible(false);
    dirty = true;
    require(compose_pending_popup_frame(base, 4, 4, overlay, dirty, frame),
            "hiding a popup must publish without a new view paint");
    require(frame == base, "popup dismissal must restore the clean view without a ghost");
    require(pixel_is_sentinel(base, 4, 1, 1), "publication must never contaminate the view base");
    require(!compose_pending_popup_frame(base, 4, 4, overlay, dirty, frame),
            "an unchanged frame must not be uploaded again");
}

void test_popup_move_and_resize_clear_previous_pixels() {
    const auto base = make_view(4, 3);
    PopupOverlay overlay;
    auto popup = make_bgra_2x2();
    overlay.set_rect(0, 0, 2, 2);
    overlay.set_visible(true);
    overlay.update_pixels(popup.data(), 2, 2);
    std::vector<std::uint8_t> frame;
    bool dirty = true;
    require(compose_pending_popup_frame(base, 4, 3, overlay, dirty, frame), "first popup must publish");

    overlay.set_rect(2, 1, 2, 2);
    dirty = true;
    require(compose_pending_popup_frame(base, 4, 3, overlay, dirty, frame),
            "moving a cached popup must publish independently");
    require(pixel_is_sentinel(frame, 4, 0, 0), "moving the popup must erase its previous position");
    require(pixel_equals(frame, 4, 2, 1, 10, 20, 30, 255), "paint width must define the row stride");
    require(pixel_equals(frame, 4, 3, 2, 100, 110, 120, 255), "paint height must define clipping");

    overlay.clear();
    dirty = true;
    require(compose_pending_popup_frame(base, 4, 3, overlay, dirty, frame) && frame == base,
            "clearing a popup during resize must restore the old view until the new paint arrives");
    const auto resized = make_view(2, 5);
    dirty = true;
    require(compose_pending_popup_frame(resized, 2, 5, overlay, dirty, frame) && frame == resized,
            "a new view size must replace the frame with its own row layout");
}

void test_missing_view_does_not_consume_popup_update() {
    PopupOverlay overlay;
    auto popup = make_bgra_2x2();
    overlay.set_visible(true);
    overlay.update_pixels(popup.data(), 2, 2);
    std::vector<std::uint8_t> frame;
    bool dirty = true;
    require(!compose_pending_popup_frame({}, 0, 0, overlay, dirty, frame) && dirty,
            "a popup before the first view must remain pending");
    const auto base = make_view(4, 3);
    require(!compose_pending_popup_frame(base, 3, 5, overlay, dirty, frame) && dirty,
            "mismatched paint dimensions must not consume pending pixels");
    require(compose_pending_popup_frame(base, 4, 3, overlay, dirty, frame),
            "the first valid view must publish the pending popup");
    require(pixel_equals(frame, 4, 0, 0, 10, 20, 30, 255), "pending popup pixels must be preserved");
}

void test_popup_resize_waits_for_matching_pixels() {
    const auto base = make_view(5, 5);
    const auto popup = make_bgra_2x2();
    for (const auto size : {3, 1}) {
        PopupOverlay overlay;
        overlay.set_rect(1, 1, 2, 2);
        overlay.set_visible(true);
        overlay.update_pixels(popup.data(), 2, 2);
        std::vector<std::uint8_t> frame;
        bool dirty = true;
        require(compose_pending_popup_frame(base, 5, 5, overlay, dirty, frame), "old popup must publish");

        // Both a larger buffer (3x4) and equal bytes with a different stride
        // (1x4) must discard the old 2x2 paint when the size callback arrives.
        overlay.set_rect(1, 1, size, 4);
        dirty = true;
        require(overlay.empty(), "resizing must invalidate popup pixels with the old dimensions");
        require(compose_pending_popup_frame(base, 5, 5, overlay, dirty, frame) && frame == base,
                "size-only publication must erase the old popup and wait for fresh pixels");

        const std::vector<std::uint8_t> resized(static_cast<std::size_t>(size) * 4u * 4u, 0xCD);
        overlay.update_pixels(resized.data(), size, 4);
        dirty = true;
        require(compose_pending_popup_frame(base, 5, 5, overlay, dirty, frame), "new popup paint must publish");
        require(pixel_equals(frame, 5, size, 4, 0xCD, 0xCD, 0xCD, 0xCD),
                "the resized popup must use the newly delivered row stride and height");
    }
}

}  // namespace

int main() {
    test_default_overlay_is_hidden();
    test_update_pixels_keeps_bgra_bytes_and_alpha();
    test_composite_writes_at_rect_offset();
    test_composite_clips_at_bottom_right_edge();
    test_composite_clips_at_top_left_edge();
    test_composite_returns_false_when_fully_outside();
    test_hidden_overlay_stops_compositing_but_keeps_pixels();
    test_clear_removes_state();
    test_invalid_inputs_are_ignored();
    test_undersized_view_buffer_is_rejected();
    test_pixel_source_overrides_declared_size();
    test_popup_scroll_and_hide_publish_without_another_view_paint();
    test_popup_move_and_resize_clear_previous_pixels();
    test_missing_view_does_not_consume_popup_update();
    test_popup_resize_waits_for_matching_pixels();
    std::cout << "PopupOverlayTests passed\n";
    return 0;
}
