#include <corona/systems/ui/embedded_camera_viewport.h>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using Corona::Systems::UI::EmbeddedCameraViewportBounds;
using Corona::Systems::UI::EmbeddedCameraViewportRequest;
using Corona::Systems::UI::map_embedded_camera_viewport;

void require_bounds(const EmbeddedCameraViewportBounds& actual,
                    const EmbeddedCameraViewportBounds& expected,
                    std::string_view message) {
    if (actual == expected) {
        return;
    }
    std::cerr << "EmbeddedCameraViewportTests failed: " << message << '\n'
              << "expected rect " << expected.x << ',' << expected.y << ' '
              << expected.width << 'x' << expected.height << ", render "
              << expected.render_width << 'x' << expected.render_height << '\n'
              << "actual rect " << actual.x << ',' << actual.y << ' '
              << actual.width << 'x' << actual.height << ", render "
              << actual.render_width << 'x' << actual.render_height << '\n';
    std::exit(1);
}

EmbeddedCameraViewportRequest full_viewport_request() {
    return {
        .camera_handle = 42,
        .x = 0.0,
        .y = 0.0,
        .width = 1536.0,
        .height = 825.0,
        .source_width = 1536.0,
        .source_height = 825.0,
        .render_width = 1920,
        .render_height = 1080,
    };
}

void test_full_viewport_at_common_dpi_scales() {
    const auto request = full_viewport_request();
    require_bounds(map_embedded_camera_viewport(request, 0.0, 0.0, 1536.0, 825.0),
                   {42, 0, 0, 1536, 825, 1920, 1080},
                   "100% DPI must preserve the embedded rectangle");
    require_bounds(map_embedded_camera_viewport(request, 0.0, 0.0, 1920.0, 1031.0),
                   {42, 0, 0, 1920, 1031, 1920, 1080},
                   "125% DPI must cover the whole physical panel");
    require_bounds(map_embedded_camera_viewport(request, 0.0, 0.0, 2304.0, 1238.0),
                   {42, 0, 0, 2304, 1238, 1920, 1080},
                   "150% DPI must cover the whole physical panel");
}

void test_fractional_edges_include_the_panel_offset() {
    const EmbeddedCameraViewportRequest request{
        .camera_handle = 42,
        .x = 1.1,
        .y = 2.5,
        .width = 10.0,
        .height = 7.3,
        .source_width = 100.0,
        .source_height = 80.0,
        .render_width = 1920,
        .render_height = 1080,
    };
    // Mapped edges are (3.775, 9.35)-(16.275, 20.3). Rounding each edge
    // preserves coverage; rounding width independently would produce 13 pixels.
    require_bounds(map_embedded_camera_viewport(request, 2.4, 5.6, 125.0, 120.0),
                   {42, 4, 9, 12, 11, 1920, 1080},
                   "fractional bounds must round their mapped edges with the panel offset");
}

void test_source_device_pixel_ratio_is_not_applied_twice() {
    auto request = full_viewport_request();
    request.width = 3072.0;
    request.height = 1650.0;
    request.source_width = 3072.0;
    request.source_height = 1650.0;
    require_bounds(map_embedded_camera_viewport(request, 0.0, 0.0, 1920.0, 1031.0),
                   {42, 0, 0, 1920, 1031, 1920, 1080},
                   "a DPR 2 source must map once into the physical destination");
}

void test_retained_request_follows_layout_and_dpi_changes() {
    const auto request = full_viewport_request();
    require_bounds(map_embedded_camera_viewport(request, 32.0, 48.0, 1920.0, 1031.0),
                   {42, 32, 48, 1920, 1031, 1920, 1080},
                   "the retained request must use the current panel position");
    require_bounds(map_embedded_camera_viewport(request, 80.0, 120.0, 2304.0, 1238.0),
                   {42, 80, 120, 2304, 1238, 1920, 1080},
                   "the same request must follow a new physical layout without a DOM resize");
}

void test_forced_render_resolution_is_independent_of_display_bounds() {
    const auto request = full_viewport_request();
    require_bounds(map_embedded_camera_viewport(request, 24.0, 36.0, 960.0, 516.0),
                   {42, 24, 36, 960, 516, 1920, 1080},
                   "a smaller panel must preserve the requested 1920x1080 render resolution");
}

}  // namespace

int main() {
    test_full_viewport_at_common_dpi_scales();
    test_fractional_edges_include_the_panel_offset();
    test_source_device_pixel_ratio_is_not_applied_twice();
    test_retained_request_follows_layout_and_dpi_changes();
    test_forced_render_resolution_is_independent_of_display_bounds();
    std::cout << "EmbeddedCameraViewportTests passed\n";
    return 0;
}
