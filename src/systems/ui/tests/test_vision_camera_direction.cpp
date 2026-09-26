#include "cef/vision_camera_direction.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

using Corona::Systems::UI::vision_camera_direction;

void expect_direction(const std::array<float, 3>& actual,
                      const std::array<float, 3>& expected,
                      const char* message) {
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (!std::isfinite(actual[i]) || std::abs(actual[i] - expected[i]) > 1e-5f) {
            std::cerr << "FAIL: " << message << " (component " << i
                      << ", actual=" << actual[i] << ", expected=" << expected[i] << ")\n";
            std::exit(1);
        }
    }
}

int main() {
    // Vision position (1, 2, 3), target (4, 6, 3): forward is (3, 4, 0) / 5.
    expect_direction(vision_camera_direction({{"target_pos", {4, 6, 3}}},
                                              {1, 2, -3}, {0, 0, 1}),
                     {0.6f, 0.8f, 0}, "look_at must subtract position and normalize");
    expect_direction(vision_camera_direction({{"target_pos", {1, 2, 8}}},
                                              {1, 2, -3}, {0, 0, 1}),
                     {0, 0, -1}, "Vision target Z must convert to Corona coordinates");
    expect_direction(vision_camera_direction(
                         {{"target_pos", {2.32801938, 1.65162766, 0.336404592}}},
                         {6.91181946f, 1.65162790f, -2.55413651f}, {0, 0, 1}),
                     {-0.900177411f, -4.68211998e-8f, 0.435523397f},
                     "staircase2 must face the stairs instead of the wall");
    const std::array<float, 3> fallback{1, 0, 0};
    expect_direction(vision_camera_direction({}, {1, 2, -3}, fallback), fallback,
                     "explicit forward/direction fallback must remain compatible");
    expect_direction(vision_camera_direction({{"target_pos", {1, 2, 3}}},
                                              {1, 2, -3}, fallback), fallback,
                     "coincident target must not produce NaN");
    expect_direction(vision_camera_direction({{"target_pos", {1, 2}}},
                                              {1, 2, -3}, fallback), fallback,
                     "malformed target must retain the fallback direction");
    std::cout << "Vision camera direction tests passed\n";
}
