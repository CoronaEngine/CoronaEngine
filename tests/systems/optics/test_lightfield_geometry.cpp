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

}  // namespace

int main() {
    lightfield_geometry_uses_camera_output_aspect();
    portrait_camera_output_updates_lightfield_geometry_aspect();
    invalid_camera_output_uses_neutral_lightfield_geometry_aspect();
    return 0;
}
