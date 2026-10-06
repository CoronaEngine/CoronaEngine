#include <corona/systems/optics/viewport_gizmo_math.h>

#include <cmath>
#include <iostream>

using namespace Corona::Systems::OpticsDetail;
using Corona::ViewportGizmoAxis;
using Corona::ViewportGizmoWarpCalibration;

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool near(float lhs, float rhs, float epsilon = 1.0e-3f) {
    return std::abs(lhs - rhs) <= epsilon;
}

}  // namespace

int main() {
    bool ok = true;

    const auto identity = ktm::fmat4x4::from_eye();
    const auto layout = make_viewport_gizmo_layout(
        identity, {0.0f, 0.0f, 0.0f}, 800, 600, 128.0f);
    ok &= expect(layout.visible, "origin in front of identity clip space must be visible");
    ok &= expect(near(layout.origin.x, 400.0f) && near(layout.origin.y, 300.0f),
                 "origin must project to viewport center");
    ok &= expect(near(ktm::length(layout.axes[0].tip - layout.origin), 128.0f),
                 "X axis must keep a constant 128 pixel length");
    ok &= expect(near(ktm::length(layout.axes[1].tip - layout.origin), 128.0f),
                 "Y axis must keep a constant 128 pixel length");

    ok &= expect(near(kViewportGizmoSpriteMetadata[1].anchor.x, 808.3f) &&
                     near(kViewportGizmoSpriteMetadata[1].anchor.y, 511.1f) &&
                     near(kViewportGizmoSpriteMetadata[1].tip.x, 266.0f) &&
                     near(kViewportGizmoSpriteMetadata[1].tip.y, 780.0f),
                 "Y metadata must follow the green Y sprite");
    ok &= expect(near(kViewportGizmoSpriteMetadata[2].anchor.x, 622.5f) &&
                     near(kViewportGizmoSpriteMetadata[2].anchor.y, 752.8f) &&
                     near(kViewportGizmoSpriteMetadata[2].tip.x, 622.0f) &&
                     near(kViewportGizmoSpriteMetadata[2].tip.y, 224.0f),
                 "Z metadata must follow the blue Z sprite");

    const auto behind = make_viewport_gizmo_layout(
        identity, {0.0f, 0.0f, -2.0f}, 800, 600, 128.0f);
    ok &= expect(!behind.visible, "origin outside clip depth must be hidden");

    const auto hit = hit_test_viewport_gizmo(layout, layout.axes[0].tip, 8.0f);
    ok &= expect(hit == ViewportGizmoAxis::X, "X arrow tip must hit X");
    const auto miss = hit_test_viewport_gizmo(layout, {50.0f, 50.0f}, 8.0f);
    ok &= expect(miss == ViewportGizmoAxis::None, "distant point must miss gizmo");

    const std::array<std::uint8_t, 16> alpha{{
        0, 0, 0, 0,
        0, 0, 0, 255,
        0, 0, 0, 0,
        0, 0, 0, 0,
    }};
    std::array<ViewportGizmoSpriteMask, 3> masks{};
    masks[0] = {
        .alpha = alpha,
        .width = 4,
        .height = 4,
        .anchor = {0.0f, 1.0f},
        .tip = {3.0f, 1.0f},
    };
    const auto alpha_hit = hit_test_viewport_gizmo_alpha(
        layout, layout.axes[0].tip, masks, 0.0f, 128.0f);
    ok &= expect(alpha_hit == ViewportGizmoAxis::X,
                 "opaque arrow-tip pixel must hit its axis");
    const auto alpha_miss = hit_test_viewport_gizmo_alpha(
        layout, layout.origin + layout.axes[0].direction * 64.0f,
        masks, 0.0f, 128.0f);
    ok &= expect(alpha_miss == ViewportGizmoAxis::None,
                 "transparent sprite pixels must not hit");

    ViewportGizmoWarpCalibration calibration;
    calibration.lenticular_pitch = 20.0f;
    calibration.slant_angle_radians = 0.0f;
    calibration.phase_offset = 0.0f;
    calibration.parallax_scale = 10.0f;
    calibration.green_subpixel_offset = 0.0f;
    const auto prewarp = viewport_ui_prewarp_point({10.0f, 12.0f}, calibration);
    ok &= expect(near(prewarp.x, 10.0f) && near(prewarp.y, 12.0f),
                 "centered phase must preserve the sample position");

    const auto parameter = closest_axis_parameter(
        {1.0f, 0.0f, -5.0f}, {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
    ok &= expect(parameter.has_value() && near(*parameter, 1.0f),
                 "ray-axis closest point must recover the world-axis parameter");

    const auto parallel = closest_axis_parameter(
        {0.0f, 0.0f, -5.0f}, {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    ok &= expect(!parallel.has_value(), "parallel ray and axis must request fallback dragging");
    const auto nearly_parallel = closest_axis_parameter(
        {0.0f, 0.0f, -5.0f}, {0.01f, 0.0f, 1.0f},
        {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    ok &= expect(!nearly_parallel.has_value(),
                 "near-view-axis drag must use stable screen-space fallback");

    return ok ? 0 : 1;
}
