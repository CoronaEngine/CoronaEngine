#pragma once

#include <algorithm>
#include <corona/shared_data_hub.h>
#include "math/transform.h"

namespace Corona::Systems::Vision {

struct VisionCameraState {
    ocarina::float4x4 camera_to_world;
    ocarina::float4x4 camera_to_screen;
    ocarina::uint2 resolution;
    float fov_y;
    float near_plane;
    float far_plane;
};

// Keep the Corona projection, including its authored image-right axis. Only
// world coordinates change basis; Vision sensor-local rays also look along +Z.
[[nodiscard]] inline VisionCameraState make_vision_camera_state(const CameraDevice& camera) {
    auto projection = ocarina::make_float4x4(1.f);
    auto camera_to_world = ocarina::make_float4x4(1.f);
    const auto native_view = camera.compute_view_matrix();
    const auto native_projection = camera.compute_projection_matrix();
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            projection[col][row] = native_projection[col][row];
        }
    }
    // The view basis is orthonormal. Transpose only its rotation and preserve
    // the authored position exactly instead of recovering it through a general
    // matrix inverse (which amplifies roundoff for distant, narrow-FOV views).
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row)
            camera_to_world[col][row] = native_view[row][col];
    camera_to_world[3] = ocarina::make_float4(camera.position.x, camera.position.y, camera.position.z, 1.f);
    for (int col = 0; col < 4; ++col) camera_to_world[col][2] *= -1.f;
    return {camera_to_world, projection,
            ocarina::make_uint2(std::max(camera.width, 1u), std::max(camera.height, 1u)),
            camera.fov, camera.near_plane, camera.far_plane};
}

} // namespace Corona::Systems::Vision
