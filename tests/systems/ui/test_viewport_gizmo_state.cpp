#include <corona/shared_data_hub.h>

#include <iostream>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    auto& hub = Corona::SharedDataHub::instance();
    hub.set_viewport_gizmo_target({
        .camera_handle = 101,
        .scene_id = "Scene/default.scene",
        .actor_name = "Cube",
        .actor_handle = 202,
    });

    const auto first = hub.viewport_gizmo_state(101);
    bool ok = true;
    ok &= expect(first.target.actor_handle == 202, "target must round-trip per camera");
    ok &= expect(first.target.actor_name == "Cube", "actor name must round-trip");
    ok &= expect(hub.viewport_gizmo_state(102).target.actor_handle == 0,
                 "gizmo targets must be isolated by camera");

    hub.update_viewport_gizmo_interaction(101, Corona::ViewportGizmoAxis::Y, true);
    const auto dragging = hub.viewport_gizmo_state(101);
    ok &= expect(dragging.active_axis == Corona::ViewportGizmoAxis::Y && dragging.dragging,
                 "active axis and dragging state must update atomically");

    hub.clear_viewport_gizmo_target(101);
    const auto cleared = hub.viewport_gizmo_state(101);
    ok &= expect(cleared.target.actor_handle == 0 && !cleared.dragging,
                 "clearing a target must clear transient interaction state");

    const auto transform_handle = hub.model_transform_storage().allocate();
    const auto geometry_handle = hub.geometry_storage().allocate();
    const auto optics_handle = hub.optics_storage().allocate();
    const auto profile_handle = hub.profile_storage().allocate();
    const auto actor_handle = hub.actor_storage().allocate();
    {
        auto transform = hub.model_transform_storage().acquire_write(transform_handle);
        transform->position = {0.0f, 0.0f, 0.0f};
    }
    {
        auto geometry = hub.geometry_storage().acquire_write(geometry_handle);
        geometry->transform_handle = transform_handle;
    }
    {
        auto optics = hub.optics_storage().acquire_write(optics_handle);
        optics->geometry_handle = geometry_handle;
    }
    {
        auto profile = hub.profile_storage().acquire_write(profile_handle);
        profile->geometry_handle = 0;
        profile->optics_handle = optics_handle;
    }
    {
        auto actor = hub.actor_storage().acquire_write(actor_handle);
        actor->profile_handles.push_back(profile_handle);
    }

    const auto geometry_handles = hub.resolve_actor_geometry_handles(actor_handle);
    ok &= expect(geometry_handles.size() == 1 &&
                     geometry_handles.front() == geometry_handle,
                 "gizmo geometry resolution must follow the optics profile");
    const auto resolved_transform =
        hub.resolve_actor_primary_transform_handle(actor_handle);
    ok &= expect(resolved_transform &&
                     *resolved_transform == transform_handle,
                 "gizmo transform resolution must use optics geometry");
    Corona::Systems::OpticsDetail::ViewportGizmoLayout layout;
    if (resolved_transform) {
        if (const auto transform =
                hub.model_transform_storage().try_acquire_read(
                    *resolved_transform)) {
            layout =
                Corona::Systems::OpticsDetail::make_viewport_gizmo_layout(
                    ktm::fmat4x4::from_eye(), transform->position,
                    800, 600, 128.0f);
        }
    }
    ok &= expect(layout.visible,
                 "an optics-only actor transform must produce a visible gizmo layout");

    hub.actor_storage().deallocate(actor_handle);
    hub.profile_storage().deallocate(profile_handle);
    hub.optics_storage().deallocate(optics_handle);
    hub.geometry_storage().deallocate(geometry_handle);
    hub.model_transform_storage().deallocate(transform_handle);
    return ok ? 0 : 1;
}
