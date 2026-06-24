#include <corona/systems/ui/viewport_gizmo_manager.h>

#include <corona/shared_data_hub.h>

#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace Corona::Systems::UI {
namespace {

constexpr float kDegreesToRadians = 0.017453292519943295769f;
constexpr float kRadiansToDegrees = 57.295779513082320876f;

[[nodiscard]] float dot_vec3(const ktm::fvec3& a, const ktm::fvec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

[[nodiscard]] ktm::fvec3 make_fvec3(float x, float y, float z) {
    ktm::fvec3 value;
    value[0] = x;
    value[1] = y;
    value[2] = z;
    return value;
}

[[nodiscard]] ktm::fvec3 cross_vec3(const ktm::fvec3& a, const ktm::fvec3& b) {
    return make_fvec3(a[1] * b[2] - a[2] * b[1],
                      a[2] * b[0] - a[0] * b[2],
                      a[0] * b[1] - a[1] * b[0]);
}

[[nodiscard]] ktm::fvec3 normalize_vec3(const ktm::fvec3& value,
                                        const ktm::fvec3& fallback) {
    const float length_sq = dot_vec3(value, value);
    if (length_sq <= 1.0e-12f) {
        return fallback;
    }
    const float inv_length = 1.0f / std::sqrt(length_sq);
    return make_fvec3(value[0] * inv_length, value[1] * inv_length, value[2] * inv_length);
}

void make_imguizmo_view_matrix(const CameraDevice& camera, float out[16]) {
    std::fill(out, out + 16, 0.0f);
    const auto forward = normalize_vec3(camera.forward, make_fvec3(0.0f, 0.0f, 1.0f));
    auto up = normalize_vec3(camera.world_up, make_fvec3(0.0f, 1.0f, 0.0f));
    const auto right = normalize_vec3(cross_vec3(up, forward), make_fvec3(1.0f, 0.0f, 0.0f));
    up = normalize_vec3(cross_vec3(forward, right), make_fvec3(0.0f, 1.0f, 0.0f));

    out[0] = right[0];
    out[4] = right[1];
    out[8] = right[2];
    out[12] = -dot_vec3(right, camera.position);

    out[1] = up[0];
    out[5] = up[1];
    out[9] = up[2];
    out[13] = -dot_vec3(up, camera.position);

    out[2] = forward[0];
    out[6] = forward[1];
    out[10] = forward[2];
    out[14] = -dot_vec3(forward, camera.position);
    out[15] = 1.0f;
}

void make_imguizmo_projection_matrix(const CameraDevice& camera, float out[16]) {
    std::fill(out, out + 16, 0.0f);
    const float near_plane = std::max(camera.near_plane, 1.0e-4f);
    const float far_plane = std::max(camera.far_plane, near_plane + 1.0f);
    const float aspect = camera.aspect > 1.0e-4f ? camera.aspect : 1.0f;
    const float half_tan = std::tan(camera.fov * kDegreesToRadians * 0.5f);
    const float y_scale = half_tan > 1.0e-6f ? 1.0f / half_tan : 1.0f;
    const float x_scale = y_scale / aspect;

    out[0] = x_scale;
    out[5] = y_scale;
    out[10] = far_plane / (far_plane - near_plane);
    out[11] = 1.0f;
    out[14] = -(near_plane * far_plane) / (far_plane - near_plane);
}

void make_imguizmo_model_matrix(const ModelTransform& transform, float out[16]) {
    float translation[3] = {
        transform.position[0],
        transform.position[1],
        transform.position[2],
    };
    float rotation[3] = {
        transform.euler_rotation[0] * kRadiansToDegrees,
        transform.euler_rotation[1] * kRadiansToDegrees,
        transform.euler_rotation[2] * kRadiansToDegrees,
    };
    float scale[3] = {
        transform.scale[0],
        transform.scale[1],
        transform.scale[2],
    };
    ImGuizmo::RecomposeMatrixFromComponents(translation, rotation, scale, out);
}

[[nodiscard]] std::uintptr_t resolve_transform_handle(std::uintptr_t actor_handle) {
    if (actor_handle == 0) {
        return 0;
    }

    auto& hub = Corona::SharedDataHub::instance();
    std::vector<std::uintptr_t> profile_handles;
    if (auto actor = hub.actor_storage().try_acquire_read_nowait(actor_handle)) {
        profile_handles = actor->profile_handles;
    }

    std::vector<std::uintptr_t> geometry_handles;
    auto append_geometry = [&geometry_handles](std::uintptr_t geometry_handle) {
        if (geometry_handle == 0) {
            return;
        }
        if (std::find(geometry_handles.begin(), geometry_handles.end(), geometry_handle) ==
            geometry_handles.end()) {
            geometry_handles.push_back(geometry_handle);
        }
    };

    for (const auto profile_handle : profile_handles) {
        std::uintptr_t optics_handle = 0;
        std::uintptr_t acoustics_handle = 0;
        std::uintptr_t mechanics_handle = 0;
        if (auto profile = hub.profile_storage().try_acquire_read_nowait(profile_handle)) {
            append_geometry(profile->geometry_handle);
            optics_handle = profile->optics_handle;
            acoustics_handle = profile->acoustics_handle;
            mechanics_handle = profile->mechanics_handle;
        }
        if (auto optics = hub.optics_storage().try_acquire_read_nowait(optics_handle)) {
            append_geometry(optics->geometry_handle);
        }
        if (auto acoustics = hub.acoustics_storage().try_acquire_read_nowait(acoustics_handle)) {
            append_geometry(acoustics->geometry_handle);
        }
        if (auto mechanics = hub.mechanics_storage().try_acquire_read_nowait(mechanics_handle)) {
            append_geometry(mechanics->geometry_handle);
        }
    }

    for (const auto geometry_handle : geometry_handles) {
        if (auto geometry = hub.geometry_storage().try_acquire_read_nowait(geometry_handle)) {
            if (geometry->transform_handle != 0) {
                return geometry->transform_handle;
            }
        }
    }
    return 0;
}

[[nodiscard]] ImGuizmo::OPERATION to_imguizmo_operation(ViewportGizmoManager::Mode mode) {
    switch (mode) {
        case ViewportGizmoManager::Mode::Scale:
            return ImGuizmo::SCALE;
        case ViewportGizmoManager::Mode::Rotate:
            return ImGuizmo::ROTATE;
        case ViewportGizmoManager::Mode::Move:
        default:
            return ImGuizmo::TRANSLATE;
    }
}

[[nodiscard]] bool rect_contains(const ImVec2& origin, const ImVec2& size, const ImVec2& point) {
    return point.x >= origin.x && point.y >= origin.y &&
           point.x < origin.x + size.x && point.y < origin.y + size.y;
}

}  // namespace

ViewportGizmoManager& ViewportGizmoManager::instance() {
    static ViewportGizmoManager manager;
    return manager;
}

void ViewportGizmoManager::set_mode(const std::string& mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode == "scale") {
        mode_ = Mode::Scale;
    } else if (mode == "rotate") {
        mode_ = Mode::Rotate;
    } else {
        mode_ = Mode::Move;
    }
}

void ViewportGizmoManager::clear_selection() {
    std::lock_guard<std::mutex> lock(mutex_);
    primary_camera_handle_ = 0;
    selections_.clear();
}

void ViewportGizmoManager::clear_camera(std::uintptr_t camera_handle) {
    if (camera_handle == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (primary_camera_handle_ == camera_handle) {
        primary_camera_handle_ = 0;
    }
    selections_.erase(camera_handle);

    const auto pending_it = pending_picks_.find(camera_handle);
    if (pending_it == pending_picks_.end()) {
        return;
    }

    const auto pending_pick = pending_it->second;
    if (pending_pick.pick_handle != 0) {
        auto& hub = Corona::SharedDataHub::instance();
        if (auto pick = hub.actor_pick_storage().try_acquire_write(pending_pick.pick_handle);
            pick && pick->request_id == pending_pick.request_id) {
            pick->pending = false;
            pick->result_ready = false;
            pick->actor_handle = 0;
            pick->result_request_id.clear();
        }
    }
    pending_picks_.erase(pending_it);
}

void ViewportGizmoManager::set_selection(std::string scene_id,
                                         std::uintptr_t camera_handle,
                                         std::uintptr_t actor_handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    primary_camera_handle_ = camera_handle;
    selections_[camera_handle] = Selection{
        std::move(scene_id),
        camera_handle,
        actor_handle,
    };
}

std::uintptr_t ViewportGizmoManager::selected_camera_handle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return primary_camera_handle_;
}

std::vector<ViewportGizmoManager::SelectionEvent>
ViewportGizmoManager::drain_selection_events() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SelectionEvent> events;
    events.swap(selection_events_);
    return events;
}

std::vector<ViewportGizmoManager::TransformEvent>
ViewportGizmoManager::drain_transform_events() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TransformEvent> events;
    events.swap(transform_events_);
    return events;
}

bool ViewportGizmoManager::render(const std::string& scene_id,
                                  std::uintptr_t camera_handle,
                                  const ImVec2& origin,
                                  const ImVec2& size,
                                  ImDrawList* draw_list) {
    if (camera_handle == 0 || size.x <= 1.0f || size.y <= 1.0f || draw_list == nullptr) {
        return false;
    }

    Mode mode = Mode::Move;
    Selection selection;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        consume_pick_result_locked();
        mode = mode_;
        const auto selection_it = selections_.find(camera_handle);
        if (selection_it != selections_.end()) {
            selection = selection_it->second;
        }
    }

    auto& hub = Corona::SharedDataHub::instance();
    CameraDevice camera;
    if (auto camera_read = hub.camera_storage().try_acquire_read_nowait(camera_handle)) {
        camera = *camera_read;
    } else {
        return false;
    }

    const ImVec2 mouse_pos = ImGui::GetIO().MousePos;
    const bool inside = rect_contains(origin, size, mouse_pos);
    const auto maybe_start_pick = [&]() {
        if (!inside || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        (void)start_pick_locked(scene_id, camera_handle, origin, size, mouse_pos);
    };

    if (selection.actor_handle == 0) {
        maybe_start_pick();
        return false;
    }
    if (!selection.scene_id.empty() && !scene_id.empty() && selection.scene_id != scene_id) {
        maybe_start_pick();
        return false;
    }

    const std::uintptr_t transform_handle = resolve_transform_handle(selection.actor_handle);
    if (transform_handle == 0) {
        maybe_start_pick();
        return false;
    }

    ModelTransform transform;
    if (auto transform_read = hub.model_transform_storage().try_acquire_read_nowait(transform_handle)) {
        transform = *transform_read;
    } else {
        maybe_start_pick();
        return false;
    }

    float view_matrix[16]{};
    float projection_matrix[16]{};
    float model_matrix[16]{};
    make_imguizmo_view_matrix(camera, view_matrix);
    make_imguizmo_projection_matrix(camera, projection_matrix);
    make_imguizmo_model_matrix(transform, model_matrix);

    ImGuizmo::SetDrawlist(draw_list);
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetRect(origin.x, origin.y, size.x, size.y);
    ImGuizmo::SetID(static_cast<int>((selection.actor_handle ^ camera_handle) & 0x7fffffff));

    const bool changed = ImGuizmo::Manipulate(view_matrix,
                                              projection_matrix,
                                              to_imguizmo_operation(mode),
                                              ImGuizmo::WORLD,
                                              model_matrix);
    if (changed) {
        float translation[3]{};
        float rotation_degrees[3]{};
        float scale[3]{};
        ImGuizmo::DecomposeMatrixToComponents(model_matrix,
                                              translation,
                                              rotation_degrees,
                                              scale);
        if (auto transform_write = hub.model_transform_storage().try_acquire_write(transform_handle)) {
            transform_write->position = ktm::fvec3{translation[0], translation[1], translation[2]};
            transform_write->euler_rotation = ktm::fvec3{
                rotation_degrees[0] * kDegreesToRadians,
                rotation_degrees[1] * kDegreesToRadians,
                rotation_degrees[2] * kDegreesToRadians,
            };
            transform_write->scale = ktm::fvec3{scale[0], scale[1], scale[2]};
        }
        std::lock_guard<std::mutex> lock(mutex_);
        TransformEvent event;
        event.scene_id = selection.scene_id.empty() ? scene_id : selection.scene_id;
        event.camera_handle = camera_handle;
        event.actor_handle = selection.actor_handle;
        event.position[0] = translation[0];
        event.position[1] = translation[1];
        event.position[2] = translation[2];
        event.rotation[0] = rotation_degrees[0] * kDegreesToRadians;
        event.rotation[1] = rotation_degrees[1] * kDegreesToRadians;
        event.rotation[2] = rotation_degrees[2] * kDegreesToRadians;
        event.scale[0] = scale[0];
        event.scale[1] = scale[1];
        event.scale[2] = scale[2];
        transform_events_.push_back(event);
    }

    const bool gizmo_captures_mouse = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
    if (!gizmo_captures_mouse) {
        maybe_start_pick();
    }
    return gizmo_captures_mouse;
}

bool ViewportGizmoManager::start_pick_locked(const std::string& scene_id,
                                             std::uintptr_t camera_handle,
                                             const ImVec2& origin,
                                             const ImVec2& size,
                                             const ImVec2& mouse_pos) {
    auto& hub = Corona::SharedDataHub::instance();
    CameraDevice camera;
    if (auto camera_read = hub.camera_storage().try_acquire_read_nowait(camera_handle)) {
        camera = *camera_read;
    } else {
        return false;
    }
    if (camera.actor_pick_handle == 0 || camera.width == 0 || camera.height == 0) {
        return false;
    }

    const float local_x = std::clamp(mouse_pos.x - origin.x, 0.0f, size.x - 1.0f);
    const float local_y = std::clamp(mouse_pos.y - origin.y, 0.0f, size.y - 1.0f);
    const auto pick_x = static_cast<std::uint32_t>(
        std::clamp((local_x / size.x) * static_cast<float>(camera.width),
                   0.0f,
                   static_cast<float>(camera.width - 1)));
    const auto pick_y = static_cast<std::uint32_t>(
        std::clamp((local_y / size.y) * static_cast<float>(camera.height),
                   0.0f,
                   static_cast<float>(camera.height - 1)));

    if (auto pick = hub.actor_pick_storage().try_acquire_write(camera.actor_pick_handle)) {
        auto& pending_pick = pending_picks_[camera_handle];
        pending_pick.request_id = "native-gizmo-" + std::to_string(++next_pick_sequence_);
        pending_pick.scene_id = scene_id;
        pending_pick.camera_handle = camera_handle;
        pending_pick.pick_handle = camera.actor_pick_handle;
        pending_pick.active = true;

        pick->request_id = pending_pick.request_id;
        pick->x = pick_x;
        pick->y = pick_y;
        pick->actor_handle = 0;
        pick->result_request_id.clear();
        pick->result_ready = false;
        pick->pending = true;
        return true;
    }
    return false;
}

void ViewportGizmoManager::consume_pick_result_locked() {
    auto& hub = Corona::SharedDataHub::instance();
    for (auto it = pending_picks_.begin(); it != pending_picks_.end();) {
        auto& pending_pick = it->second;
        if (!pending_pick.active || pending_pick.pick_handle == 0) {
            it = pending_picks_.erase(it);
            continue;
        }
        auto pick = hub.actor_pick_storage().try_acquire_write(pending_pick.pick_handle);
        if (!pick || !pick->result_ready || pick->result_request_id != pending_pick.request_id) {
            ++it;
            continue;
        }

        selections_[pending_pick.camera_handle] = Selection{
            pending_pick.scene_id,
            pending_pick.camera_handle,
            pick->actor_handle,
        };
        selection_events_.push_back(SelectionEvent{
            pending_pick.scene_id,
            pending_pick.camera_handle,
            pick->actor_handle,
        });

        pick->result_ready = false;
        it = pending_picks_.erase(it);
    }
}

}  // namespace Corona::Systems::UI
