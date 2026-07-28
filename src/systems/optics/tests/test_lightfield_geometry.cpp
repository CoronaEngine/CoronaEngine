#include "base/sensor/light_field_types.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void expect_near(float actual, float expected, const char* message) {
    if (std::abs(actual - expected) > 1e-6f) {
        std::cerr << "FAIL: " << message << " (actual=" << actual
                  << ", expected=" << expected << ")\n";
        std::exit(1);
    }
}

void lightfield_geometry_uses_camera_output_aspect() {
    expect_near(vision::lightfield_aspect_from_resolution(
                    ocarina::make_uint2(1920u, 1080u)),
                16.0f / 9.0f,
                "lightfield geometry aspect should match the camera output aspect");
}

void portrait_camera_output_updates_lightfield_geometry_aspect() {
    expect_near(vision::lightfield_aspect_from_resolution(
                    ocarina::make_uint2(1080u, 1920u)),
                9.0f / 16.0f,
                "lightfield geometry aspect should follow a portrait camera output");
}

void invalid_camera_output_uses_neutral_lightfield_geometry_aspect() {
    expect_near(vision::lightfield_aspect_from_resolution(
                    ocarina::make_uint2(1920u, 0u)),
                1.0f,
                "an invalid camera output height should use a neutral aspect");
}

void viewer_view_count_and_index_handling_are_stable() {
    if (vision::lightfield_view_count(48.9f) != 48u ||
        vision::lightfield_view_count(0.0f) != 1u ||
        vision::lightfield_view_count(-4.0f) != 1u ||
        vision::lightfield_view_count(NAN) != 1u) {
        std::cerr << "FAIL: invalid or fractional view counts were not normalized\n";
        std::exit(1);
    }
    if (vision::lightfield_effective_view_index(
            vision::kLightFieldAutoViewIndex, 48u) != 23u ||
        vision::lightfield_effective_view_index(999u, 48u) != 47u ||
        vision::lightfield_effective_view_index(0u, 1u) != 0u) {
        std::cerr << "FAIL: viewer index clamping or center selection is incorrect\n";
        std::exit(1);
    }
}

void viewer_view_coordinates_use_zero_based_normalized_angles() {
    expect_near(vision::lightfield_view_u(0u, 48u), 0.0f,
                "first view should map to u=0");
    expect_near(vision::lightfield_view_u(23u, 48u), 23.0f / 47.0f,
                "middle view should use the normalized view index");
    expect_near(vision::lightfield_view_u(47u, 48u), 1.0f,
                "last view should map to u=1");
    expect_near(vision::lightfield_view_u(0u, 1u), 0.5f,
                "a single view should use the center angular coordinate");
}

}  // namespace

int main() {
    lightfield_geometry_uses_camera_output_aspect();
    portrait_camera_output_updates_lightfield_geometry_aspect();
    invalid_camera_output_uses_neutral_lightfield_geometry_aspect();
    viewer_view_count_and_index_handling_are_stable();
    viewer_view_coordinates_use_zero_based_normalized_angles();
    return 0;
}
