#pragma once

#include <corona/shared_data_hub.h>
#include <cmath>

namespace Corona::Systems::OpticsDetail {

// Raster depth uses reversed Z in D32. Keep the authored clip distances and
// image projection; changing the near plane would hide close source geometry.
// Vision and editor projection helpers retain their existing depth convention.
[[nodiscard]] inline ktm::fmat4x4 native_projection(const CameraDevice& camera) {
    auto projection = camera.compute_projection_matrix();
    const double near = camera.near_plane, far = camera.far_plane;
    projection[2][2] = static_cast<float>(near / (near - far));
    projection[3][2] = static_cast<float>((near * far) / (far - near));
    return projection;
}

[[nodiscard]] inline ktm::fmat4x4 native_inverse_projection(const CameraDevice& camera) {
    const auto projection = native_projection(camera);
    ktm::fmat4x4 inverse{};
    inverse[0][0] = 1.f / projection[0][0];
    inverse[1][1] = 1.f / projection[1][1];
    inverse[3][2] = 1.f;
    inverse[2][3] = 1.f / projection[3][2];
    inverse[3][3] = -projection[2][2] / projection[3][2];
    return inverse;
}

} // namespace Corona::Systems::OpticsDetail
