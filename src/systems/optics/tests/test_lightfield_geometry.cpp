#include "base/sensor/light_field_types.h"

#include <cmath>
#include <cstdint>
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

std::uint32_t reference_subpixel_view_id(
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t channel,
    const vision::LenticularParams& lenticular) {
    const auto view_count =
        vision::lightfield_view_count(lenticular.num_views);
    const float d = 3.f * static_cast<float>(x) +
                    3.f * static_cast<float>(y) * std::tan(lenticular.angle) +
                    static_cast<float>(channel) + lenticular.offset;
    const float a =
        d - std::floor(d / lenticular.pe) * lenticular.pe;
    const auto raw_view = static_cast<std::uint32_t>(
        std::floor(a / (lenticular.pe / static_cast<float>(view_count))));
    return view_count - 1u - std::min(raw_view, view_count - 1u);
}

void shared_subpixel_view_mapping_matches_ray_generation_formula() {
    vision::LenticularParams lenticular;
    lenticular.pe = 19.1849f;
    lenticular.angle = 0.2333f;
    lenticular.offset = 10.f;
    lenticular.num_views = 48.f;

    for (std::uint32_t y = 0u; y < 32u; ++y) {
        for (std::uint32_t x = 0u; x < 64u; ++x) {
            for (std::uint32_t channel = 0u; channel < 3u; ++channel) {
                const auto actual = vision::lightfield_subpixel_view_id(
                    x, y, channel, lenticular);
                const auto expected = reference_subpixel_view_id(
                    x, y, channel, lenticular);
                if (actual != expected || actual >= 48u) {
                    std::cerr
                        << "FAIL: shared subpixel view mapping diverged"
                        << " (x=" << x << ", y=" << y
                        << ", channel=" << channel
                        << ", actual=" << actual
                        << ", expected=" << expected << ")\n";
                    std::exit(1);
                }
            }
        }
    }
}

void subpixel_view_mapping_uses_canonical_view_count() {
    vision::LenticularParams integral;
    integral.pe = 19.1849f;
    integral.angle = 0.2333f;
    integral.offset = 10.f;
    integral.num_views = 48.f;
    vision::LenticularParams fractional = integral;
    fractional.num_views = 48.9f;

    for (std::uint32_t channel = 0u; channel < 3u; ++channel) {
        const auto expected = vision::lightfield_subpixel_view_id(
            17u, 11u, channel, integral);
        const auto actual = vision::lightfield_subpixel_view_id(
            17u, 11u, channel, fractional);
        if (actual != expected) {
            std::cerr
                << "FAIL: fractional view count changed subpixel mapping\n";
            std::exit(1);
        }
    }

    fractional.num_views = 1.f;
    for (std::uint32_t channel = 0u; channel < 3u; ++channel) {
        if (vision::lightfield_subpixel_view_id(
                17u, 11u, channel, fractional) != 0u) {
            std::cerr << "FAIL: single-view subpixel mapping was not zero\n";
            std::exit(1);
        }
    }

    fractional.num_views = NAN;
    if (vision::lightfield_subpixel_view_id(
            17u, 11u, 1u, fractional) != 0u) {
        std::cerr << "FAIL: invalid view count was not normalized\n";
        std::exit(1);
    }
}

}  // namespace

int main() {
    lightfield_geometry_uses_camera_output_aspect();
    portrait_camera_output_updates_lightfield_geometry_aspect();
    invalid_camera_output_uses_neutral_lightfield_geometry_aspect();
    viewer_view_count_and_index_handling_are_stable();
    viewer_view_coordinates_use_zero_based_normalized_angles();
    shared_subpixel_view_mapping_matches_ray_generation_formula();
    subpixel_view_mapping_uses_canonical_view_count();
    return 0;
}
