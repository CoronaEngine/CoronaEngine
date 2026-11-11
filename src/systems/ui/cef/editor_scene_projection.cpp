#include "editor_scene_projection.h"

#include "editor_actor_projection.h"

#include <corona/engine/engine_runtime_api.h>

#include <array>
#include <cstdint>
#include <string>

namespace Corona::Systems::UI {

EnvironmentSnapshot native_environment_snapshot(const NativeEditorScene& scene) {
    if (!scene.environment) {
        throw std::runtime_error("Native scene environment is unavailable");
    }
    const auto physics = scene.environment->get_state();
    return {scene.archive_session, scene.route, scene.sun_enabled, scene.floor_grid_enabled, scene.sun_direction,
        {physics.gravity, physics.floor_y, physics.floor_restitution, physics.fixed_dt}};
}

nlohmann::json native_scene_environment_payload(const NativeEditorScene& scene) {
    const auto value = native_environment_snapshot(scene);
    return {
        {"status", "success"},
        {"scene", value.scene},
        {"sun", {
            {"enabled", value.sun_enabled},
            {"direction", value.sun_direction},
        }},
        {"authored_lighting", scene.authored_lighting},
        {"grid", {{"enabled", value.grid_enabled}}},
        {"physics", {
            {"gravity", value.physics.gravity},
            {"floor_y", value.physics.floor_y},
            {"floor_restitution", value.physics.floor_restitution},
            {"fixed_dt", value.physics.fixed_dt},
        }},
    };
}

std::string current_viewport_session() {
    const auto* scene = native_editor_state().scene.get();
    return scene ? scene->archive_session : "";
}

bool requested_camera_view_open(const NativeEditorCamera& camera) {
    const auto state = camera.window_state->requested_open.load();
    return state < 0 ? camera.view_open : state != 0;
}

nlohmann::json camera_to_json(const NativeEditorCamera& camera) {
    nlohmann::json item;
    item["archive_session"] = current_viewport_session();
    item["id"] = camera.camera_id;
    item["camera_id"] = camera.camera_id;
    item["name"] = camera.name;
    item["handle"] = camera.engine_camera ? camera.engine_camera->get_handle() : 0;
    item["opaque_handle"] = std::to_string(camera.engine_camera ? camera.engine_camera->get_handle() : 0);
    item["position"] = camera.engine_camera ? camera.engine_camera->get_pose().position : std::array<float, 3>{0.0f, 0.0f, -5.0f};
    item["forward"] = camera.engine_camera ? camera.engine_camera->get_pose().forward : std::array<float, 3>{0.0f, 0.0f, 1.0f};
    item["world_up"] = camera.engine_camera ? camera.engine_camera->get_pose().world_up : std::array<float, 3>{0.0f, 1.0f, 0.0f};
    item["fov"] = camera.engine_camera ? camera.engine_camera->get_pose().fov : 45.0f;
    const auto projection = camera.engine_camera ? camera.engine_camera->get_projection()
                                                : std::array<float, 3>{1.0f, 0.1f, 100.0f};
    item["projection_x_sign"] = projection[0];
    item["near_plane"] = projection[1];
    item["far_plane"] = projection[2];
    item["width"] = camera.width;
    item["height"] = camera.height;
    item["output_mode"] = camera.engine_camera ? camera.engine_camera->get_output_mode() : "final_color";
    item["render_backend"] = camera.engine_camera ? camera.engine_camera->get_render_backend() : "native";
    item["vision_render_mode"] = camera.engine_camera ? camera.engine_camera->get_vision_render_mode() : "path_tracing";
    item["vision_spp"] = camera.vision_spp;
    item["vision_max_depth"] = camera.vision_max_depth;
    item["vision_denoise"] = camera.engine_camera ? camera.engine_camera->get_vision_denoise() : true;
    item["vision_denoiser"] = camera.engine_camera ? camera.engine_camera->get_vision_denoiser() : "reblur";
    item["vision_reblur_settings"] = camera.engine_camera
        ? API::get_requested_vision_settings(API::VisionSettingsFamily::Reblur, camera.engine_camera->get_handle())
        : nlohmann::json::object();
    item["vision_pt_settings"] = camera.engine_camera
        ? API::get_requested_vision_settings(API::VisionSettingsFamily::Pt, camera.engine_camera->get_handle())
        : nlohmann::json::object();
    item["vision_restir_settings"] = camera.engine_camera
        ? API::get_requested_vision_settings(API::VisionSettingsFamily::Restir, camera.engine_camera->get_handle())
        : nlohmann::json::object();
    item["vision_relax_settings"] = camera.engine_camera
        ? API::get_requested_vision_settings(API::VisionSettingsFamily::Relax, camera.engine_camera->get_handle())
        : nlohmann::json::object();
    item["vision_accumulation"] = camera.engine_camera ? camera.engine_camera->get_vision_accumulation() : false;
    item["vision_stable_planes"] = camera.engine_camera ? camera.engine_camera->get_vision_stable_planes() : true;
    item["move_speed"] = camera.move_speed;
    item["view_open"] = requested_camera_view_open(camera);
    item["view_x"] = camera.view_x;
    item["view_y"] = camera.view_y;
    item["view_width"] = camera.view_width;
    item["view_height"] = camera.view_height;
    item["deletable"] = camera.deletable;
    return item;
}

nlohmann::json scene_to_json(const NativeEditorScene& scene) {
    nlohmann::json cameras = nlohmann::json::array();
    for (const auto& camera : scene.cameras) {
        cameras.push_back(camera_to_json(camera));
    }
    nlohmann::json actors = nlohmann::json::array();
    for (const auto& actor : scene.actors) {
        actors.push_back(actor_to_json(scene, actor, /*best_effort=*/true));
    }
    const auto active_index = scene.cameras.empty()
                                  ? 0
                                  : std::min(scene.active_camera_index, scene.cameras.size() - 1);
    const auto active_camera = scene.cameras.empty()
                                   ? nlohmann::json(nullptr)
                                   : camera_to_json(scene.cameras[active_index]);
    return {
        {"id", scene.route},
        {"scene_id", scene.route},
        {"name", scene.name},
        {"active_camera_id", active_camera.is_null() ? "" : active_camera.value("camera_id", "")},
        {"active_camera_name", active_camera.is_null() ? "" : active_camera.value("name", "")},
        {"camera", active_camera},
        {"cameras", cameras},
        {"sun", {{"enabled", scene.sun_enabled}, {"direction", scene.sun_direction}}},
        {"grid", {{"enabled", scene.floor_grid_enabled}}},
        {"terrain", {{"path", scene.terrain_path}, {"type", scene.terrain_type}}},

        {"script", scene.script_path},
        {"actors", actors},
    };
}

nlohmann::json archive_scene_values(const NativeEditorScene& scene) {
    auto state = scene_to_json(scene);
    state["core_version"] = scene.core_version;
    state["physics"] = native_scene_environment_payload(scene).at("physics");
    for (size_t i = 0; i < scene.actors.size(); ++i) {
        const auto& actor = scene.actors[i];
        auto& item = state["actors"][i];
        item["animation_sources"] = actor.persisted_animation_sources;
        item["authored_optics"] = nlohmann::json::object();
        auto& optics = item["authored_optics"];
        const auto& values = actor.persisted_optics;
        if (values.diffuse) optics["diffuse"] = *values.diffuse;
        if (values.emission) optics["emission"] = *values.emission;
        if (values.metallic) optics["metallic"] = *values.metallic;
        if (values.roughness) optics["roughness"] = *values.roughness;
        if (values.specular) optics["specular"] = *values.specular;
        if (values.shininess) optics["shininess"] = *values.shininess;
    }
    for (size_t i = 0; i < scene.cameras.size(); ++i) {
        if (const auto& camera = scene.cameras[i].engine_camera) {
            auto& item = state["cameras"][i];
            item["vision_denoise"] = camera->get_requested_vision_denoise();
            item["vision_denoiser"] = camera->get_requested_vision_denoiser();
            item["vision_accumulation"] = camera->get_requested_vision_accumulation();
            item["vision_stable_planes"] = camera->get_requested_vision_stable_planes();
        }
    }
    return state;
}

}
  // namespace Corona::Systems::UI
