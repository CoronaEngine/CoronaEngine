#include "editor_camera_views.h"

#include "camera_window_lifetime.h"
#include "editor_scene_projection.h"
#include "editor_text_utils.h"
#include "native_api_types.h"

#include <algorithm>
#include <string>

namespace Corona::Systems::UI {

std::string make_actor_guid(const std::string& scene_route,
                            const std::string& actor_name,
                            size_t index) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto nonce = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    std::ostringstream out;
    out << "native-" << std::hex << std::hash<std::string>{}(scene_route + ":" + actor_name)
        << "-" << index << "-" << nonce;
    return out.str();
}

NativeEditorCamera* find_native_camera(NativeEditorScene& scene, const std::string& camera_name) {
    if (camera_name.empty() && !scene.cameras.empty()) {
        return &scene.cameras[std::min(scene.active_camera_index, scene.cameras.size() - 1)];
    }
    for (auto& camera : scene.cameras) {
        if (camera.name == camera_name || camera.camera_id == camera_name ||
            (camera.engine_camera && std::to_string(camera.engine_camera->get_handle()) == camera_name))
        {
            return &camera;
        }
    }
    return nullptr;
}

std::string unique_camera_name(const NativeEditorScene& scene, const std::string& preferred_name) {
    const auto base = trim_ascii(preferred_name).empty() ? std::string("Camera") : trim_ascii(preferred_name);
    auto exists = [&](const std::string& candidate) {
        return std::any_of(scene.cameras.begin(), scene.cameras.end(), [&](const NativeEditorCamera& camera) {
            return camera.name == candidate || camera.camera_id == candidate;
        });
    };
    if (!exists(base)) {
        return base;
    }
    for (int index = 1; index < 10000; ++index) {
        const auto candidate = base + "_" + std::to_string(index);
        if (!exists(candidate)) {
            return candidate;
        }
    }
    return base + "_" + std::to_string(scene.cameras.size() + 1);
}

NativeEditorCamera& create_native_camera_view(NativeEditorScene& scene, const std::string& requested_name, bool initially_open) {
    NativeEditorCamera item;
    ++scene.tree_revision;
    item.name = unique_camera_name(scene, requested_name);
    item.camera_id = make_actor_guid(scene.route, item.name, scene.cameras.size());
    item.deletable = true;
    item.width = 1920;
    item.height = 1080;
    item.view_open = initially_open;
    item.view_x = 120;
    item.view_y = 120;
    item.view_width = 960;
    item.view_height = 540;
    item.move_speed = 1.0f;

    std::array<float, 3> position{0.0f, 0.0f, -5.0f};
    std::array<float, 3> forward{0.0f, 0.0f, 1.0f};
    std::array<float, 3> world_up{0.0f, 1.0f, 0.0f};
    float fov = 45.0f;
    if (auto* source = find_native_camera(scene, {}); source && source->engine_camera) {
        position = source->engine_camera->get_pose().position;
        forward = source->engine_camera->get_pose().forward;
        world_up = source->engine_camera->get_pose().world_up;
        fov = source->engine_camera->get_pose().fov;
        item.width = source->width;
        item.height = source->height;
        item.move_speed = source->move_speed;
    }

    item.engine_camera = std::make_unique<Corona::API::Camera>(position, forward, world_up, fov);
    item.engine_camera->set_size(item.width, item.height);
    item.engine_camera->set_output_mode("final_color");
    item.engine_camera->set_render_backend("native");
    item.engine_camera->set_vision_render_mode("path_tracing");
    item.engine_camera->set_view({item.view_open, item.view_x, item.view_y,
                                  item.view_width, item.view_height, item.move_speed});
    item.engine_camera->set_offscreen_capture_mode(true);
    item.engine_camera->set_surface(0);

    scene.cameras.push_back(std::move(item));
    auto& camera = scene.cameras.back();
    if (scene.engine_scene && camera.engine_camera) {
        scene.engine_scene->add_camera(camera.engine_camera.get());
    }
    return camera;
}

NativeResult camera_not_found_result(const std::string& camera_name) {
    return native_failure("Camera not found: " + camera_name, 2);
}

}
  // namespace Corona::Systems::UI