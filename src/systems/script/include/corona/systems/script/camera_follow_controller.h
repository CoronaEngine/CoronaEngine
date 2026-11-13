#pragma once

#include <corona/kernel/entity_id.h>
#include <ktm/ktm.h>

#include <atomic>
#include <cstdint>

namespace Corona::Systems {

class CameraFollowController {
   public:
    static CameraFollowController& instance();

    CameraFollowController(const CameraFollowController&) = delete;
    CameraFollowController& operator=(const CameraFollowController&) = delete;
    CameraFollowController(CameraFollowController&&) = delete;
    CameraFollowController& operator=(CameraFollowController&&) = delete;

    void set_target(EntityId actor_handle, EntityId camera_handle,
                    float offset_x, float offset_y, float offset_z);
    void clear_target();
    bool is_active() const;
    void set_input_enabled(bool enabled);
    bool is_input_enabled() const;

    void update(float delta_time);

    void inject_key(int vk_code, bool down);
    void inject_rmb(bool down, int screen_x, int screen_y);

   private:
    CameraFollowController() = default;

    void update_wasd(ktm::fvec3& obj_pos, const ktm::fvec3& offset);
    void update_rmb_orbit(ktm::fvec3& obj_pos, const ktm::fvec3& offset);
    void update_camera(const ktm::fvec3& obj_pos, const ktm::fvec3& offset);

    static ktm::fvec3 normalize(const ktm::fvec3& v);
    static ktm::fvec3 cross(const ktm::fvec3& a, const ktm::fvec3& b);

    bool active_{false};
    EntityId actor_handle_{0};
    /// Resolved actor -> profile -> geometry -> transform handle. Cached so the
    /// per-frame path only touches the transform snapshot, not actor/profile slots.
    EntityId follow_transform_{0};
    EntityId camera_handle_{0};
    ktm::fvec3 offset_{0.0f, 0.0f, 2.0f};
    bool camera_look_at_{true};

    bool rmb_down_{false};
    int prev_mouse_x_{0};
    int prev_mouse_y_{0};
    std::atomic_bool input_enabled_{true};

    float elapsed_since_last_log_{0.0f};
};

}  // namespace Corona::Systems
