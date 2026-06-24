#include <corona/systems/script/camera_follow_controller.h>

#include <corona/kernel/core/i_logger.h>
#include <corona/shared_data_hub.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>

namespace Corona::Systems {

CameraFollowController& CameraFollowController::instance() {
    static CameraFollowController inst;
    return inst;
}

void CameraFollowController::set_target(std::uintptr_t actor_handle, std::uintptr_t camera_handle,
                                        float offset_x, float offset_y, float offset_z) {
    actor_handle_ = actor_handle;
    camera_handle_ = camera_handle;
    offset_ = ktm::fvec3{offset_x, offset_y, offset_z};
    active_ = true;
    CFW_LOG_INFO("CameraFollowController: target set (actor=0x{:x}, camera=0x{:x}, offset={},{},{})",
                 actor_handle, camera_handle, offset_x, offset_y, offset_z);
}

void CameraFollowController::clear_target() {
    active_ = false;
    actor_handle_ = 0;
    camera_handle_ = 0;
    rmb_down_ = false;
    CFW_LOG_INFO("CameraFollowController: target cleared");
}

bool CameraFollowController::is_active() const {
    return active_;
}

void CameraFollowController::set_input_enabled(bool enabled) {
    const bool previous = input_enabled_.exchange(enabled);
    if (!enabled) {
        rmb_down_ = false;
    }
}

bool CameraFollowController::is_input_enabled() const {
    return input_enabled_.load();
}

void CameraFollowController::inject_key(int vk_code, bool down) {
    // Key injection from Vue/CEF for Blockly script input — handled in Phase 3
    (void)vk_code;
    (void)down;
}

void CameraFollowController::inject_rmb(bool down, int screen_x, int screen_y) {
    if (!input_enabled_.load()) {
        rmb_down_ = false;
        return;
    }
    rmb_down_ = down;
    if (down) {
        prev_mouse_x_ = screen_x;
        prev_mouse_y_ = screen_y;
    }
}

ktm::fvec3 CameraFollowController::normalize(const ktm::fvec3& v) {
    float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len < 1e-10f) {
        return ktm::fvec3{0.0f, 0.0f, 1.0f};
    }
    return ktm::fvec3{v.x / len, v.y / len, v.z / len};
}

ktm::fvec3 CameraFollowController::cross(const ktm::fvec3& a, const ktm::fvec3& b) {
    return ktm::fvec3{
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

void CameraFollowController::update(float delta_time) {
    if (!active_ || actor_handle_ == 0 || camera_handle_ == 0) {
        return;
    }

    elapsed_since_last_log_ += delta_time;

    // Read actor position from SharedDataHub
    auto& hub = SharedDataHub::instance();
    ktm::fvec3 obj_pos{0.0f, 0.0f, 0.0f};

    // Resolve actor -> profile -> geometry -> transform_handle -> position
    std::vector<std::uintptr_t> profile_handles;
    if (auto actor = hub.actor_storage().try_acquire_read(actor_handle_)) {
        profile_handles = actor->profile_handles;
    }

    bool found_transform = false;
    for (const auto profile_handle : profile_handles) {
        if (auto profile = hub.profile_storage().try_acquire_read(profile_handle)) {
            if (profile->geometry_handle != 0) {
                if (auto geo = hub.geometry_storage().try_acquire_read(profile->geometry_handle)) {
                    if (geo->transform_handle != 0) {
                        if (auto transform = hub.model_transform_storage().try_acquire_read(geo->transform_handle)) {
                            obj_pos = transform->position;
                            found_transform = true;
                            break;
                        }
                    }
                }
            }
        }
    }
    if (!found_transform) return;

    if (input_enabled_.load()) {
        // WASD movement (modifies obj_pos directly)
        update_wasd(obj_pos, offset_);

        // RMB orbit (modifies obj_pos directly)
        update_rmb_orbit(obj_pos, offset_);

        // Write back actor position only while editor input owns movement.
        for (const auto profile_handle : profile_handles) {
            if (auto profile = hub.profile_storage().try_acquire_read(profile_handle)) {
                if (profile->geometry_handle != 0) {
                    if (auto geo = hub.geometry_storage().try_acquire_read(profile->geometry_handle)) {
                        if (geo->transform_handle != 0) {
                            if (auto transform = hub.model_transform_storage().try_acquire_write(geo->transform_handle)) {
                                transform->position = obj_pos;
                            }
                        }
                    }
                }
            }
        }
    } else {
        rmb_down_ = false;
    }

    // Update camera position and look-at
    update_camera(obj_pos, offset_);
}

void CameraFollowController::update_wasd(ktm::fvec3& obj_pos, const ktm::fvec3& offset) {
#ifdef _WIN32
    bool w_down = (GetAsyncKeyState(0x57) & 0x8000) != 0;
    bool a_down = (GetAsyncKeyState(0x41) & 0x8000) != 0;
    bool s_down = (GetAsyncKeyState(0x53) & 0x8000) != 0;
    bool d_down = (GetAsyncKeyState(0x44) & 0x8000) != 0;

    if (!w_down && !a_down && !s_down && !d_down) return;

    ktm::fvec3 look_dir = normalize(ktm::fvec3{-offset.x, -offset.y, -offset.z});
    ktm::fvec3 fwd_xz = normalize(ktm::fvec3{look_dir.x, 0.0f, look_dir.z});
    ktm::fvec3 right_xz = normalize(cross(ktm::fvec3{0.0f, 1.0f, 0.0f}, fwd_xz));

    float step = 0.5f;
    float move_x = 0.0f;
    float move_z = 0.0f;

    if (w_down) { move_x += fwd_xz.x * step; move_z += fwd_xz.z * step; }
    if (s_down) { move_x -= fwd_xz.x * step; move_z -= fwd_xz.z * step; }
    if (a_down) { move_x -= right_xz.x * step; move_z -= right_xz.z * step; }
    if (d_down) { move_x += right_xz.x * step; move_z += right_xz.z * step; }

    obj_pos.x += move_x;
    obj_pos.z += move_z;
#endif
}

void CameraFollowController::update_rmb_orbit(ktm::fvec3& obj_pos, const ktm::fvec3& offset) {
#ifdef _WIN32
    if (!rmb_down_) return;

    POINT pt;
    if (!GetCursorPos(&pt)) return;
    int cur_x = pt.x;
    int cur_y = pt.y;

    int dx = cur_x - prev_mouse_x_;
    int dy = cur_y - prev_mouse_y_;
    prev_mouse_x_ = cur_x;
    prev_mouse_y_ = cur_y;

    if (dx == 0 && dy == 0) return;

    ktm::fvec3 look_dir = normalize(ktm::fvec3{-offset.x, -offset.y, -offset.z});
    ktm::fvec3 fwd_xz = normalize(ktm::fvec3{look_dir.x, 0.0f, look_dir.z});
    ktm::fvec3 right_xz = normalize(cross(ktm::fvec3{0.0f, 1.0f, 0.0f}, fwd_xz));

    float rmb_speed = 0.02f;
    obj_pos.x += right_xz.x * static_cast<float>(dx) * rmb_speed;
    obj_pos.z += right_xz.z * static_cast<float>(dx) * rmb_speed;
    obj_pos.x += fwd_xz.x * static_cast<float>(-dy) * rmb_speed;
    obj_pos.z += fwd_xz.z * static_cast<float>(-dy) * rmb_speed;
#endif
}

void CameraFollowController::update_camera(const ktm::fvec3& obj_pos, const ktm::fvec3& offset) {
    // Camera position: obj_pos + offset
    ktm::fvec3 cam_pos{
        obj_pos.x + offset.x,
        obj_pos.y + offset.y,
        obj_pos.z + offset.z,
    };

    // Camera look-at: gaze toward the actor
    ktm::fvec3 cam_fwd = normalize(ktm::fvec3{-offset.x, -offset.y, -offset.z});
    ktm::fvec3 cam_up{0.0f, 1.0f, 0.0f};

    // Write camera state into SharedDataHub in ONE acquire_write
    auto& hub = SharedDataHub::instance();
    if (auto cam = hub.camera_storage().try_acquire_write(camera_handle_)) {
        cam->position = cam_pos;
        cam->forward = cam_fwd;
        cam->world_up = cam_up;
    }
}

}  // namespace Corona::Systems
