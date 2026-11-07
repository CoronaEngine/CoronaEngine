#pragma once

#include <corona/shared_data_hub.h>

#include <algorithm>
#include <utility>

namespace Corona::Systems {

// Called by the Optics owner. A retained authority also rejects a drained
// command if its numeric storage address has since been reused by a new scene.
inline bool apply_camera_viewport_update(SharedDataHub& hub, const CameraViewportUpdateCommand& update) {
    std::unique_lock<std::mutex> lease;
    if (update.authority) {
        lease = std::unique_lock(update.authority->mutex);
        if (!update.authority->active) return false;
    }
    auto& storage = hub.camera_storage();
    {
        auto camera = storage.try_acquire_write(update.camera_handle);
        if (!camera) return false;
        camera->surface = update.surface;
        camera->follows_default_surface = false;
        camera->view_open = update.view_open;
        camera->viewport_rect_active = true;
        camera->view_x = update.x;
        camera->view_y = update.y;
        camera->view_width = update.width;
        camera->view_height = update.height;
        camera->width = static_cast<std::uint32_t>(std::max(update.render_width, 1));
        camera->height = static_cast<std::uint32_t>(std::max(update.render_height, 1));
        camera->aspect = static_cast<float>(camera->width) / static_cast<float>(camera->height);
    }
    if (update.surface && !update.view_open) {
        // The live embedded viewport owns this surface. A retired command must
        // never detach another camera, even when its old handle no longer exists.
        std::vector<std::uintptr_t> obsolete;
        for (const auto& camera : std::as_const(storage)) {
            const auto handle = reinterpret_cast<std::uintptr_t>(&camera);
            if (handle != update.camera_handle && camera.surface == update.surface && !camera.view_open)
                obsolete.push_back(handle);
        }
        for (const auto handle : obsolete) {
            if (auto camera = storage.try_acquire_write(handle)) {
                if (camera->surface == update.surface && !camera->view_open) {
                    camera->surface = nullptr;
                    camera->follows_default_surface = false;
                }
            }
        }
    }
    return true;
}

}  // namespace Corona::Systems
