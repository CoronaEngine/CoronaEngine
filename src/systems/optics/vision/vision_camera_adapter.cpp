#include "vision_camera_adapter.h"
#include "vision_camera_state.h"

#include <cstdlib>

#include "base/mgr/pipeline.h"
#include "base/sensor/sensor.h"

namespace Corona::Systems::Vision {

void sync_vision_camera(::vision::Pipeline& pipeline, const CameraDevice& camera) {
    auto& sensor = pipeline.scene().sensor();
    const auto state = make_vision_camera_state(camera);
    bool invalidate = false;
    if (ocarina::any(pipeline.resolution() != state.resolution)) {
        pipeline.change_resolution(state.resolution);
        // Resolution changes resize the pixel buffers, but the output texture
        // must also be recreated before tone mapping writes into it.
        pipeline.frame_buffer()->prepare_view_texture();
        invalidate = true;
    }

    const auto change = sensor->set_external_camera(
        state.camera_to_world, state.camera_to_screen, state.resolution,
        state.fov_y, state.near_plane, state.far_plane);

    // Realtime denoisers reproject pose changes using the previous full camera
    // matrices. Accumulation averages fixed pixels and must start over. Every
    // projection change (including a reflected X axis or clip range) invalidates
    // both histories, even when the field of view itself has not changed.
    invalidate |= change.projection ||
        (change.pose && pipeline.frame_buffer()->enable_accumulation());
    sensor->update_device_data();

    if (invalidate) {
        if (std::getenv("CORONA_VISION_CAMERA_LOG") != nullptr) {
            OC_INFO_FORMAT("Vision camera invalidated frame_index={} resolution=({}, {}) "
                           "fov={} projection_x_sign={} clip=({}, {}) pose_changed={}",
                           pipeline.frame_index(), state.resolution.x, state.resolution.y,
                           state.fov_y, camera.projection_x_sign,
                           state.near_plane, state.far_plane, change.pose);
        }
        pipeline.invalidate();
    }
}

}  // namespace Corona::Systems::Vision
