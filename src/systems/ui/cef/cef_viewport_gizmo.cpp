#include "cef_viewport_gizmo.h"
#include <corona/utils/runtime_paths.h>
#include "cef_editor_api.h"
#include "ui_resource_read.h"

#include <horizon/core/logging.h>
#include <corona/resource/resource_manager.h>
#include <corona/resource/types/image.h>
#include <corona/resource/types/scene.h>
#include <corona/shared_data_hub.h>
#include <corona/engine/engine_runtime_api.h>
#include <include/cef_values.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "browser_manager.h"
#include "cef_editor_native_api_handlers.h"

namespace Corona::Systems::UI {

namespace {


struct ViewportGizmoDragRuntime {
    std::uintptr_t actor_handle{};
    Corona::ViewportGizmoAxis axis{Corona::ViewportGizmoAxis::None};
    ktm::fvec3 initial_position{};
    ktm::fvec3 last_position{};
    ktm::fvec2 initial_pointer{};
    ktm::fvec2 fallback_screen_direction{};
    std::optional<float> initial_axis_parameter;
    bool fallback_logged{false};
    bool write_failure_logged{false};
    std::string scene_id;
    std::string actor_name;
};

static std::mutex s_viewport_gizmo_mutex;
static std::unordered_map<std::uintptr_t, ViewportGizmoDragRuntime>
    s_viewport_gizmo_drags;

struct ViewportGizmoCpuMasks {
    std::array<std::vector<std::uint8_t>, 3> alpha;
    std::array<Corona::Systems::OpticsDetail::ViewportGizmoSpriteMask, 3> views;
    bool valid{false};
};

const ViewportGizmoCpuMasks& viewport_gizmo_cpu_masks() {
    static ViewportGizmoCpuMasks masks;
    static std::once_flag load_once;
    std::call_once(load_once, [] {
        constexpr std::array<const char*, 3> paths{{
            "assets/icon/axis_x.png",
            "assets/icon/axis_y.png",
            "assets/icon/axis_z.png",
        }};
        std::error_code ec;
        auto root = Utils::executable_directory();
        for (std::size_t i = 0; i < paths.size(); ++i) {
            auto path = std::filesystem::path(paths[i]);
            if (!ec) {
                for (auto dir = root; !dir.empty(); dir = dir.parent_path()) {
                    const auto candidate = dir / paths[i];
                    if (std::filesystem::exists(candidate, ec) && !ec) {
                        path = candidate;
                        break;
                    }
                    ec.clear();
                    if (dir == dir.parent_path()) break;
                }
            }
            const auto image_id =
                Corona::Resource::ResourceManager::get_instance().import_sync(path);
            if (image_id == Corona::Resource::IResource::INVALID_UID) {
                CFW_LOG_WARNING("Viewport gizmo alpha mask load failed: {}", path.string());
                return;
            }
            auto image =
                read_resource_for_ui<Corona::Resource::Image>(image_id);
            if (!image || image->get_width() <= 0 || image->get_height() <= 0 ||
                image->get_data() == nullptr) {
                CFW_LOG_WARNING("Viewport gizmo alpha mask is invalid: {}", path.string());
                return;
            }
            const auto width = static_cast<std::uint32_t>(image->get_width());
            const auto height = static_cast<std::uint32_t>(image->get_height());
            const int channels = image->get_channels();
            if (channels != 4) {
                CFW_LOG_WARNING("Viewport gizmo image has no alpha channel: {}", path.string());
                return;
            }
            const auto pixel_count =
                static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
            masks.alpha[i].resize(pixel_count);
            const auto* pixels = image->get_data();
            for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
                masks.alpha[i][pixel] = pixels[pixel * 4 + 3];
            }
            const auto& metadata =
                Corona::Systems::OpticsDetail::kViewportGizmoSpriteMetadata[i];
            masks.views[i] = {
                .alpha = masks.alpha[i],
                .width = width,
                .height = height,
                .anchor = metadata.anchor,
                .tip = metadata.tip,
            };
        }
        masks.valid = true;
    });
    return masks;
}

[[nodiscard]] std::uintptr_t select_scene_camera_handle(const Corona::SceneDevice& scene) {
    if (scene.active_camera_handle != 0 &&
        std::find(scene.camera_handles.begin(),
                  scene.camera_handles.end(),
                  scene.active_camera_handle) != scene.camera_handles.end()) {
        return scene.active_camera_handle;
    }
    return scene.camera_handles.empty() ? 0 : scene.camera_handles.front();
}

[[nodiscard]] ktm::fvec3 make_fvec3(float x, float y, float z) {
    ktm::fvec3 value;
    value[0] = x;
    value[1] = y;
    value[2] = z;
    return value;
}

}  // namespace (input queue)

namespace {

const char* viewport_gizmo_axis_name(Corona::ViewportGizmoAxis axis) {
    switch (axis) {
        case Corona::ViewportGizmoAxis::X: return "x";
        case Corona::ViewportGizmoAxis::Y: return "y";
        case Corona::ViewportGizmoAxis::Z: return "z";
        default: return "none";
    }
}

ktm::fvec3 viewport_gizmo_axis_vector(Corona::ViewportGizmoAxis axis) {
    switch (axis) {
        case Corona::ViewportGizmoAxis::X: return make_fvec3(1.0f, 0.0f, 0.0f);
        case Corona::ViewportGizmoAxis::Y: return make_fvec3(0.0f, 1.0f, 0.0f);
        case Corona::ViewportGizmoAxis::Z: return make_fvec3(0.0f, 0.0f, 1.0f);
        default: return make_fvec3(0.0f, 0.0f, 0.0f);
    }
}

std::optional<ktm::fvec3> actor_gizmo_position(std::uintptr_t actor_handle) {
    auto& hub = Corona::SharedDataHub::instance();
    for (const auto geometry_handle :
         hub.resolve_actor_geometry_handles(actor_handle)) {
        auto geometry = hub.geometry_storage().try_acquire_read(geometry_handle);
        if (!geometry || geometry->transform_handle == 0) {
            continue;
        }
        if (auto transform =
                hub.model_transform_storage().try_acquire_read(geometry->transform_handle)) {
            return transform->position;
        }
    }
    return std::nullopt;
}

bool write_actor_gizmo_position(std::uintptr_t actor_handle,
                                const ktm::fvec3& position) {
    bool wrote = false;
    auto& hub = Corona::SharedDataHub::instance();
    for (const auto geometry_handle :
         hub.resolve_actor_geometry_handles(actor_handle)) {
        auto geometry = hub.geometry_storage().try_acquire_read(geometry_handle);
        if (!geometry || geometry->transform_handle == 0) {
            continue;
        }
        // P5：改走命令通道（原先是 UI 线程直接写槽位，违反单写者约束）
        Corona::TransformUpdateCommand command{};
        command.transform_handle = geometry->transform_handle;
        command.fields = Corona::TransformField::Position;
        command.position = position;
        hub.enqueue_transform_update(std::move(command));
        wrote = true;
    }
    return wrote;
}

ktm::fvec3 viewport_gizmo_pointer_ray(const Corona::CameraDevice& camera,
                                      ktm::fvec2 pixel,
                                      float width,
                                      float height) {
    const auto forward = ktm::normalize(camera.forward);
    auto right = ktm::cross(camera.world_up, forward);
    right = ktm::length(right) > 1.0e-5f
                ? ktm::normalize(right)
                : make_fvec3(1.0f, 0.0f, 0.0f);
    const auto up = ktm::normalize(ktm::cross(forward, right));
    const float ndc_x = 2.0f * pixel.x / std::max(width, 1.0f) - 1.0f;
    const float ndc_y = 2.0f * pixel.y / std::max(height, 1.0f) - 1.0f;
    const float tan_half_fov = std::tan(ktm::radians(camera.fov) * 0.5f);
    return ktm::normalize(
        forward +
        right * (ndc_x * tan_half_fov * camera.aspect * camera.projection_x_sign) -
        up * (ndc_y * tan_half_fov));
}

struct GizmoComputationResult {
    Corona::ViewportGizmoAxis axis{Corona::ViewportGizmoAxis::None};
    bool consumed{false}, dragging{false}, ended{false}, cancelled{false};
    ktm::fvec3 position{};
};

void complete_gizmo_reply(const GizmoComputationResult& result,
                          ViewportGizmoReply* direct) {
    if (direct) {
        direct->axis = viewport_gizmo_axis_name(result.axis);
        direct->consumed = result.consumed;
        direct->dragging = result.dragging;
        direct->ended = result.ended;
        direct->cancelled = result.cancelled;
        direct->position = {result.position.x, result.position.y, result.position.z};
    }
}

bool run_viewport_gizmo_pointer(const ViewportGizmoPointerRequest& request,
                                ViewportGizmoReply* direct = nullptr) {
    const auto camera_handle = request.camera_handle;
    std::string event_type = request.event;
    std::transform(event_type.begin(), event_type.end(), event_type.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    auto& hub = Corona::SharedDataHub::instance();
    const auto state = hub.viewport_gizmo_state(camera_handle);
    GizmoComputationResult result;
    if (state.target.actor_handle == 0 || request.width <= 0.0 || request.height <= 0.0) {
        complete_gizmo_reply(result, direct);
        return true;
    }

    auto camera = hub.camera_storage().try_acquire_read(camera_handle);
    const auto current_position = actor_gizmo_position(state.target.actor_handle);
    if (!camera || !current_position) {
        hub.clear_viewport_gizmo_target(camera_handle);
        complete_gizmo_reply(result, direct);
        return true;
    }

    const float camera_width = static_cast<float>(std::max(camera->width, 1u));
    const float camera_height = static_cast<float>(std::max(camera->height, 1u));
    ktm::fvec2 pointer{
        static_cast<float>(request.x / request.width) * camera_width,
        static_cast<float>(request.y / request.height) * camera_height,
    };
    const auto ui_state = hub.viewport_ui_state(camera_handle);
    if (ui_state.mode == Corona::ViewportUiMode::Stereo3D) {
        pointer = Corona::Systems::OpticsDetail::viewport_ui_prewarp_point(
            pointer,
            {
                .lenticular_pitch = ui_state.calibration.lenticular_pitch,
                .slant_angle_radians = ui_state.calibration.slant_angle_radians,
                .phase_offset = ui_state.calibration.phase_offset,
                .green_subpixel_offset = ui_state.calibration.rgb_subpixel_offsets[1],
                .parallax_scale = ui_state.calibration.parallax_scale,
            });
    }

    const auto layout = Corona::Systems::OpticsDetail::make_viewport_gizmo_layout(
        camera->compute_view_proj_matrix(), *current_position,
        camera->width, camera->height, 128.0f);
    const auto& cpu_masks = viewport_gizmo_cpu_masks();
    const auto hover_axis =
        cpu_masks.valid
            ? Corona::Systems::OpticsDetail::hit_test_viewport_gizmo_alpha(
                  layout, pointer, cpu_masks.views, 8.0f, 128.0f)
            : Corona::ViewportGizmoAxis::None;

    std::lock_guard lock(s_viewport_gizmo_mutex);
    auto drag_it = s_viewport_gizmo_drags.find(camera_handle);
    const bool cancel_event =
        event_type == "cancel" || event_type == "pointercancel" ||
        event_type == "blur" || event_type == "escape";
    const bool up_event = event_type == "up" || event_type == "pointerup";
    const bool down_event = event_type == "down" || event_type == "pointerdown";

    if (cancel_event && drag_it != s_viewport_gizmo_drags.end()) {
        const bool restored = write_actor_gizmo_position(
            drag_it->second.actor_handle, drag_it->second.initial_position);
        CFW_LOG_WARNING("Viewport gizmo drag cancelled: camera={} actor='{}' axis={} event='{}' restored={}",
                        camera_handle,
                        drag_it->second.actor_name,
                        static_cast<int>(drag_it->second.axis),
                        event_type,
                        restored);
        result.axis = drag_it->second.axis;
        result.consumed = true;
        result.cancelled = true;
        result.position = drag_it->second.initial_position;
        s_viewport_gizmo_drags.erase(drag_it);
        hub.update_viewport_gizmo_interaction(
            camera_handle, Corona::ViewportGizmoAxis::None, false);
        complete_gizmo_reply(result, direct);
        return true;
    }

    if (down_event && request.button == 0 &&
        drag_it == s_viewport_gizmo_drags.end() &&
        hover_axis != Corona::ViewportGizmoAxis::None) {
        const auto axis_vector = viewport_gizmo_axis_vector(hover_axis);
        const auto ray =
            viewport_gizmo_pointer_ray(*camera, pointer, camera_width, camera_height);
        ViewportGizmoDragRuntime drag;
        drag.actor_handle = state.target.actor_handle;
        drag.axis = hover_axis;
        drag.initial_position = *current_position;
        drag.last_position = *current_position;
        drag.initial_pointer = pointer;
        const std::size_t axis_index =
            hover_axis == Corona::ViewportGizmoAxis::X ? 0u :
            (hover_axis == Corona::ViewportGizmoAxis::Y ? 1u : 2u);
        drag.fallback_screen_direction = layout.axes[axis_index].direction;
        drag.initial_axis_parameter =
            Corona::Systems::OpticsDetail::closest_axis_parameter(
                camera->position, ray, *current_position, axis_vector);
        drag.scene_id = state.target.scene_id;
        drag.actor_name = state.target.actor_name;
        s_viewport_gizmo_drags[camera_handle] = drag;
        CFW_LOG_INFO("Viewport gizmo drag start: camera={} actor='{}' axis={} initial_parameter={}",
                     camera_handle,
                     state.target.actor_name,
                     static_cast<int>(hover_axis),
                     drag.initial_axis_parameter
                         ? std::to_string(*drag.initial_axis_parameter)
                         : std::string("degenerate"));
        hub.update_viewport_gizmo_interaction(camera_handle, hover_axis, true);
        result.axis = hover_axis;
        result.consumed = true;
        result.dragging = true;
        result.position = *current_position;
        complete_gizmo_reply(result, direct);
        return true;
    }

    drag_it = s_viewport_gizmo_drags.find(camera_handle);
    if (drag_it != s_viewport_gizmo_drags.end()) {
        auto& drag = drag_it->second;
        const auto axis_vector = viewport_gizmo_axis_vector(drag.axis);
        const auto ray =
            viewport_gizmo_pointer_ray(*camera, pointer, camera_width, camera_height);
        const auto parameter =
            Corona::Systems::OpticsDetail::closest_axis_parameter(
                camera->position, ray, drag.initial_position, axis_vector);
        const float depth = std::max(
            ktm::dot(drag.initial_position - camera->position,
                     ktm::normalize(camera->forward)),
            camera->near_plane);
        const float world_per_pixel =
            2.0f * depth * std::tan(ktm::radians(camera->fov) * 0.5f) /
            camera_height;
        float delta = 0.0f;
        if (parameter && drag.initial_axis_parameter) {
            delta = *parameter - *drag.initial_axis_parameter;
        } else {
            if (!drag.fallback_logged) {
                CFW_LOG_WARNING("Viewport gizmo drag fallback: camera={} actor='{}' axis={} parameter_valid={} initial_parameter_valid={} screen_direction=({}, {}) world_per_pixel={}",
                                camera_handle,
                                drag.actor_name,
                                static_cast<int>(drag.axis),
                                parameter.has_value(),
                                drag.initial_axis_parameter.has_value(),
                                drag.fallback_screen_direction.x,
                                drag.fallback_screen_direction.y,
                                world_per_pixel);
                drag.fallback_logged = true;
            }
            delta = ktm::dot(pointer - drag.initial_pointer,
                             drag.fallback_screen_direction) * world_per_pixel;
        }
        delta = std::clamp(delta, -10000.0f, 10000.0f);
        const float previous_delta =
            ktm::dot(drag.last_position - drag.initial_position, axis_vector);
        const float max_frame_delta = std::max(world_per_pixel * 256.0f, 0.25f);
        delta = std::clamp(
            delta,
            previous_delta - max_frame_delta,
            previous_delta + max_frame_delta);
        const auto next_position = drag.initial_position + axis_vector * delta;
        const bool wrote = write_actor_gizmo_position(drag.actor_handle, next_position);
        if (wrote) {
            drag.last_position = next_position;
        } else if (!drag.write_failure_logged) {
            CFW_LOG_WARNING("Viewport gizmo transform write failed: camera={} actor='{}' axis={} handle={}",
                            camera_handle,
                            drag.actor_name,
                            static_cast<int>(drag.axis),
                            drag.actor_handle);
            drag.write_failure_logged = true;
        }
        result.axis = drag.axis;
        result.consumed = true;
        result.position = next_position;
        if (up_event) {
            CFW_LOG_INFO("Viewport gizmo drag ended: camera={} actor='{}' axis={} position=({}, {}, {})",
                         camera_handle,
                         drag.actor_name,
                         static_cast<int>(drag.axis),
                         next_position.x,
                         next_position.y,
                         next_position.z);
            result.ended = true;
            result.dragging = false;
            s_viewport_gizmo_drags.erase(drag_it);
            hub.update_viewport_gizmo_interaction(
                camera_handle, Corona::ViewportGizmoAxis::None, false);
        } else {
            result.dragging = true;
            hub.update_viewport_gizmo_interaction(camera_handle, drag.axis, true);
        }
        complete_gizmo_reply(result, direct);
        return true;
    }

    hub.update_viewport_gizmo_interaction(
        camera_handle, Corona::ViewportGizmoAxis::None, false, hover_axis);
    result.axis = hover_axis;
    result.position = *current_position;
    complete_gizmo_reply(result, direct);
    return true;
}


}  // namespace

ViewportGizmoReply execute_viewport_gizmo_target(const ViewportGizmoTargetRequest& request) {
    ViewportGizmoReply reply;
    auto& hub = Corona::SharedDataHub::instance();
    if (request.actor_handle == 0) {
        hub.clear_viewport_gizmo_target(request.camera_handle);
        std::lock_guard lock(s_viewport_gizmo_mutex);
        s_viewport_gizmo_drags.erase(request.camera_handle);
    } else {
        hub.set_viewport_gizmo_target({.camera_handle = request.camera_handle, .scene_id = request.scene_id,
            .actor_name = request.actor_guid.value_or(std::string{}), .actor_handle = request.actor_handle});
    }
    return reply;
}

ViewportGizmoReply execute_viewport_gizmo_pointer(const ViewportGizmoPointerRequest& request) {
    ViewportGizmoReply reply;
    run_viewport_gizmo_pointer(request, &reply);
    return reply;
}

}  // namespace Corona::Systems::UI
