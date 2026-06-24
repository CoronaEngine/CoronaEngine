#include <corona/kernel/core/callback_sink.h>
#include <corona/kernel/core/i_logger.h>
#include <corona/kernel/core/kernel_context.h>
#include <corona/systems/network/network_system.h>
#include <corona/systems/script/camera_follow_controller.h>
#include <corona/systems/script/corona_engine_api.h>
#include <corona/systems/script/engine_scripts.h>
#include <nanobind/nanobind.h>
#include <nanobind/stl/array.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>

#include <SDL3/SDL.h>

// Forward declare InputEvent from cef_bridge_helpers.h (UI system header,
// avoid adding UI include dirs to script system which lives in its own
// namespace).  Definition lives in Corona::Systems::UI in cef_bridge_helpers.h.
namespace Corona::Systems::UI {
struct InputEvent {
    int type;
    std::string arg0;
    std::string arg1;
    std::string arg2;
    double arg3;
    double arg4;
};
std::vector<InputEvent> drain_input_events();
}  // namespace Corona::Systems::UI

namespace nb = nanobind;
using namespace Corona::API;

namespace EngineScripts {

namespace {

std::shared_ptr<Corona::Systems::NetworkSystem> get_network_system() {
    auto sys_mgr = Corona::Kernel::KernelContext::instance().system_manager();
    if (!sys_mgr) {
        return nullptr;
    }
    return std::dynamic_pointer_cast<Corona::Systems::NetworkSystem>(
        sys_mgr->get_system("Network"));
}

uint64_t network_now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

nb::dict lanchat_message_to_dict(const Corona::Network::LanChatMessage& message) {
    nb::dict item;
    item["message_id"] = message.message_id;
    item["sender_id"] = message.sender_id;
    item["sender_name"] = message.sender_name;
    item["room_id"] = message.room_id;
    item["seq"] = message.seq;
    item["from"] = message.sender_name;
    item["text"] = message.text;
    item["ts"] = message.timestamp_ms / 1000;
    item["sender_type"] = message.sender_type;
    item["message_kind"] = message.message_kind;
    item["target_agent_id"] = message.target_agent_id;
    item["source_user_id"] = message.source_user_id;
    item["correlation_id"] = message.correlation_id;
    item["metadata_json"] = message.metadata_json;
    return item;
}

}  // namespace

void BindAll(nanobind::module_& m) {
    // ============================================================================
    // Geometry: 作为所有组件的锚点，存储位置/旋转/缩放和模型数据
    // ============================================================================
    nb::class_<Geometry>(m, "Geometry")
        .def(nb::init<const std::string&>(), nb::arg("model_path"),
             "Create a Geometry from a model file path")
        .def_static("from_image", &Geometry::from_image, nb::arg("image_path"),
                    "Create a Geometry as a textured quad (UI plane) from an image file")
        .def("set_position", &Geometry::set_position, nb::arg("position"),
             "Set local position [x, y, z]")
        .def("set_rotation", &Geometry::set_rotation, nb::arg("euler"),
             "Set local rotation (Euler angles ZYX order) [pitch, yaw, roll]")
        .def("set_scale", &Geometry::set_scale, nb::arg("scale"),
             "Set local scale [x, y, z]")
        .def("set_native_local_correction", &Geometry::set_native_local_correction,
             nb::arg("offset"), nb::arg("scale"),
             "Set native-only local correction applied before rendering")
        .def("get_position", &Geometry::get_position,
             "Get local position [x, y, z]")
        .def("get_rotation", &Geometry::get_rotation,
             "Get local rotation (Euler angles) [pitch, yaw, roll]")
        .def("get_scale", &Geometry::get_scale,
             "Get local scale [x, y, z]")
        .def("get_aabb", &Geometry::get_aabb,
             "Get model AABB [min_x, min_y, min_z, max_x, max_y, max_z]");

    // ============================================================================
    // Mechanics: 物理/力学组件
    // ============================================================================
    nb::class_<Mechanics>(m, "Mechanics")
        .def(nb::init<Geometry&>(), nb::arg("geometry"),
             "Create a Mechanics component attached to a Geometry")
        .def("set_mass", &Mechanics::set_mass, nb::arg("mass"),
             "Set object mass")
        .def("get_mass", &Mechanics::get_mass,
             "Get object mass")
        .def("set_restitution", &Mechanics::set_restitution, nb::arg("restitution"),
             "Set object restitution (bounciness)")
        .def("get_restitution", &Mechanics::get_restitution,
             "Get object restitution")
        .def("set_damping", &Mechanics::set_damping, nb::arg("damping"),
             "Set velocity damping factor")
        .def("get_damping", &Mechanics::get_damping,
             "Get velocity damping factor")
        .def("set_physics_enabled", &Mechanics::set_physics_enabled, nb::arg("enabled"),
             "Enable or disable physics simulation for this object")
        .def("get_physics_enabled", &Mechanics::get_physics_enabled,
             "Get whether physics simulation is enabled for this object")
        .def("set_collision_enabled", &Mechanics::set_collision_enabled, nb::arg("enabled"),
             "Enable or disable collision detection for this object")
        .def("get_collision_enabled", &Mechanics::get_collision_enabled,
             "Get whether collision detection is enabled for this object")
        .def("set_linear_lock", &Mechanics::set_linear_lock,
             nb::arg("lock_x"), nb::arg("lock_y"), nb::arg("lock_z"),
             "Lock/unlock linear movement on X/Y/Z axes")
        .def("get_linear_lock", &Mechanics::get_linear_lock,
             "Get linear axis lock state as (lock_x, lock_y, lock_z) tuple")
        .def("set_angular_lock", &Mechanics::set_angular_lock,
             nb::arg("lock_x"), nb::arg("lock_y"), nb::arg("lock_z"),
             "Lock/unlock angular rotation on X/Y/Z axes")
        .def("get_angular_lock", &Mechanics::get_angular_lock,
             "Get angular axis lock state as (lock_x, lock_y, lock_z) tuple")
        .def("set_collision_callback",
             [](Mechanics& self, nb::object callback) {
                 using CallbackType = std::function<void(std::uintptr_t, bool, const std::array<float, 3>&, const std::array<float, 3>&)>;

                 if (callback.is_none()) {
                     self.set_collision_callback(CallbackType{});
                     return;
                 }

                auto func_ptr = std::shared_ptr<nb::object>(new nb::object(callback), [](nb::object* p) {
                    try {
                        nb::gil_scoped_acquire gil;
                        delete p;
                    } catch (...) {
                        delete p;
                    }
                });

                CallbackType cb = [func_ptr](std::uintptr_t other, bool began, const std::array<float, 3>& normal, const std::array<float, 3>& point) mutable {
                    nb::gil_scoped_acquire gil;
                    try {
                        (*func_ptr).attr("__call__")(other, began, normal, point);
                    }  catch (const std::exception &e) {
                        CFW_LOG_ERROR("[Bindings::collision_callback] std::exception when invoking Python callback: {}", e.what());
                    } catch (...) {
                        CFW_LOG_ERROR("[Bindings::collision_callback] Unknown exception when invoking Python callback");
                    }
                };

                 self.set_collision_callback(cb); },

             nb::arg("callback"), "Set collision callback. Callback receives (other_handle, normal, point) where normal and point are (x,y,z) tuples.")
        .def("set_on_move_callback", [](Mechanics& self, nb::object callback) {
                using CallbackType = std::function<void()>;

                if (callback.is_none()) {
                    self.set_on_move_callback(CallbackType{});
                    return;
                }

                auto func_ptr = std::shared_ptr<nb::object>(new nb::object(callback), [](nb::object* p) {
                    try {
                        nb::gil_scoped_acquire gil;
                        delete p;
                    } catch (...) {
                        delete p;
                    }
                });

                CallbackType cb = [func_ptr]() mutable {
                    nb::gil_scoped_acquire gil;
                    try {
                        (*func_ptr).attr("__call__")();
                    } catch (const std::exception& e) {
                        CFW_LOG_ERROR("[Bindings::move_callback] std::exception when invoking Python callback: {}", e.what());
                    } catch (...) {
                        CFW_LOG_ERROR("[Bindings::move_callback] Unknown exception when invoking Python callback");
                    }
                };

                self.set_on_move_callback(cb); }, nb::arg("callback"), "Set move callback for geometry.");

    // ============================================================================
    // Optics: 光学/渲染组件
    // ============================================================================
    nb::class_<Optics>(m, "Optics")
        .def(nb::init<Geometry&>(), nb::arg("geometry"),
             "Create an Optics component attached to a Geometry")
        .def("set_visible", &Optics::set_visible, nb::arg("visible"),
             "Set whether this model is rendered")
        .def("get_visible", &Optics::get_visible,
             "Get whether this model is rendered")
        .def("set_lighting_enabled", &Optics::set_lighting_enabled, nb::arg("enabled"),
             "Enable or disable lighting influence on this object")
        .def("get_lighting_enabled", &Optics::get_lighting_enabled,
             "Get whether lighting influence is enabled for this object")
        .def("set_metallic", &Optics::set_metallic, nb::arg("metallic"))
        .def("get_metallic", &Optics::get_metallic)
        .def("set_roughness", &Optics::set_roughness, nb::arg("roughness"))
        .def("get_roughness", &Optics::get_roughness)
        .def("set_subsurface", &Optics::set_subsurface, nb::arg("subsurface"))
        .def("get_subsurface", &Optics::get_subsurface)
        .def("set_specular", &Optics::set_specular, nb::arg("specular"))
        .def("get_specular", &Optics::get_specular)
        .def("set_specular_tint", &Optics::set_specular_tint, nb::arg("specular_tint"))
        .def("get_specular_tint", &Optics::get_specular_tint)
        .def("set_anisotropic", &Optics::set_anisotropic, nb::arg("anisotropic"))
        .def("get_anisotropic", &Optics::get_anisotropic)
        .def("set_sheen", &Optics::set_sheen, nb::arg("sheen"))
        .def("get_sheen", &Optics::get_sheen)
        .def("set_sheen_tint", &Optics::set_sheen_tint, nb::arg("sheen_tint"))
        .def("get_sheen_tint", &Optics::get_sheen_tint)
        .def("set_clearcoat", &Optics::set_clearcoat, nb::arg("clearcoat"))
        .def("get_clearcoat", &Optics::get_clearcoat)
        .def("set_clearcoat_gloss", &Optics::set_clearcoat_gloss, nb::arg("clearcoat_gloss"))
        .def("get_clearcoat_gloss", &Optics::get_clearcoat_gloss)
        .def("set_ambient", &Optics::set_ambient, nb::arg("ambient"))
        .def("get_ambient", &Optics::get_ambient)
        .def("set_diffuse", &Optics::set_diffuse, nb::arg("diffuse"))
        .def("get_diffuse", &Optics::get_diffuse)
        .def("set_specular_color", &Optics::set_specular_color, nb::arg("specular_color"))
        .def("get_specular_color", &Optics::get_specular_color)
        .def("set_shininess", &Optics::set_shininess, nb::arg("shininess"))
        .def("get_shininess", &Optics::get_shininess);

    // ============================================================================
    // Acoustics: 声学组件
    // ============================================================================
    nb::class_<Acoustics>(m, "Acoustics")
        .def(nb::init<Geometry&>(), nb::arg("geometry"),
             "Create an Acoustics component attached to a Geometry")
        .def("set_volume", &Acoustics::set_volume, nb::arg("volume"),
             "Set audio volume")
        .def("get_volume", &Acoustics::get_volume,
             "Get audio volume")
        .def("set_audio_enabled", &Acoustics::set_audio_enabled, nb::arg("enabled"),
             "Enable or disable audio for this object")
        .def("get_audio_enabled", &Acoustics::get_audio_enabled,
             "Get whether audio is enabled for this object");

    // ============================================================================
    // Actor: OOP 风格的实体类，支持多套组件配置（Profile）
    // ============================================================================
    nb::class_<Actor::Profile>(m, "ActorProfile")
        .def(nb::init<>())
        .def_rw("optics", &Actor::Profile::optics, "Optics component")
        .def_rw("acoustics", &Actor::Profile::acoustics, "Acoustics component")
        .def_rw("mechanics", &Actor::Profile::mechanics, "Mechanics component")
        .def_rw("geometry", &Actor::Profile::geometry, "Geometry anchor");

    nb::class_<Actor>(m, "Actor")
        .def(nb::init<>(), "Create an empty Actor")
        .def("add_profile", &Actor::add_profile, nb::arg("profile"),
             "Add a component profile to this actor. Returns pointer to the stored profile.",
             nb::rv_policy::reference_internal)
        .def("remove_profile", &Actor::remove_profile, nb::arg("profile"),
             "Remove a component profile from this actor")
        .def("set_active_profile", &Actor::set_active_profile, nb::arg("profile"),
             "Set the active profile for this actor")
        .def("get_active_profile", &Actor::get_active_profile,
             "Get the currently active profile",
             nb::rv_policy::reference_internal)
        .def("profile_count", &Actor::profile_count,
             "Get number of profiles in this actor")
        .def("set_follow_camera", &Actor::set_follow_camera, nb::arg("enabled"),
             "Render this actor in camera-local orthographic pass 2")
        .def("get_follow_camera", &Actor::get_follow_camera,
             "Return whether this actor renders in camera-local orthographic pass 2")
        .def("set_actor_guid", &Actor::set_actor_guid, nb::arg("actor_guid"))
        .def("get_actor_guid", &Actor::get_actor_guid)
        .def("set_external_vision_binding", &Actor::set_external_vision_binding,
             nb::arg("source_path"), nb::arg("shape_guid"), nb::arg("shape_index"),
             nb::arg("json_path"), nb::arg("shape_type"), nb::arg("shape_identity_key"),
             nb::arg("model_path"))
        .def("clear_external_vision_binding", &Actor::clear_external_vision_binding)
        .def("has_external_vision_binding", &Actor::has_external_vision_binding)
        .def("get_handle", &Actor::get_handle, "Get the underlying handle of this actor");

    // ============================================================================
    // Camera: 相机类（合并了原 Viewport 功能）
    // ============================================================================
    nb::class_<Camera>(m, "Camera")
        .def(nb::init<>(), "Create a default Camera")
        .def(nb::init<const std::array<float, 3>&, const std::array<float, 3>&,
                      const std::array<float, 3>&, float>(),
             nb::arg("position"), nb::arg("forward"), nb::arg("world_up"), nb::arg("fov"),
             "Create a Camera with specified parameters")
        .def("set", &Camera::set,
             nb::arg("position"), nb::arg("forward"), nb::arg("world_up"), nb::arg("fov"),
             "Set all camera parameters at once")
        .def("get_handle", &Camera::get_handle, "Get camera handle")
        .def("save_screenshot", &Camera::save_screenshot, nb::arg("path"),
             "Save a screenshot from this camera's perspective to file (async)")
        .def("save_screenshot_sync", &Camera::save_screenshot_sync, nb::arg("path"),
             "Save a screenshot and block until it completes. Returns True on success.")
        .def("set_output_mode", &Camera::set_output_mode, nb::arg("mode"),
             "Set camera output mode. mode: 'final_color', 'base_color', 'normal', 'position', 'object_id'")
        .def("get_output_mode", &Camera::get_output_mode,
             "Get current camera output mode as string")
        .def("set_render_backend", &Camera::set_render_backend, nb::arg("mode"))
        .def("get_render_backend", &Camera::get_render_backend)
        .def("set_vision_render_mode", &Camera::set_vision_render_mode, nb::arg("mode"))
        .def("get_vision_render_mode", &Camera::get_vision_render_mode)
        .def("set_view_state", &Camera::set_view_state, nb::arg("open"), nb::arg("x"),
             nb::arg("y"), nb::arg("width"), nb::arg("height"), nb::arg("move_speed"))
        .def("get_view_state", &Camera::get_view_state)
        .def("set_surface", [](Camera& self, std::uintptr_t surface) { self.set_surface(reinterpret_cast<void*>(surface)); }, nb::arg("surface"), "Set render surface (pass window ID as integer)")
        .def("get_surface", [](const Camera& self) -> std::uintptr_t { return reinterpret_cast<std::uintptr_t>(self.get_surface()); }, "Get render surface handle as integer (0 if none)")
        .def("set_offscreen_capture_mode", &Camera::set_offscreen_capture_mode, nb::arg("enabled"), "Detach camera from the default surface for screenshot-only rendering")
        .def("get_position", &Camera::get_position, "Get camera position [x, y, z]")
        .def("get_forward", &Camera::get_forward, "Get camera forward direction [x, y, z]")
        .def("get_world_up", &Camera::get_world_up, "Get camera world up vector [x, y, z]")
        .def("get_fov", &Camera::get_fov, "Get field of view in degrees")
        .def("set_image_effects", &Camera::set_image_effects, nb::arg("effects"), "Set image effects for this camera")
        .def("get_image_effects", &Camera::get_image_effects, "Get image effects attached to this camera", nb::rv_policy::reference)
        .def("has_image_effects", &Camera::has_image_effects, "Check if camera has image effects")
        .def("remove_image_effects", &Camera::remove_image_effects, "Remove image effects from this camera")
        .def("set_size", &Camera::set_size, nb::arg("width"), nb::arg("height"), "Set camera render dimensions")
        .def("get_size", &Camera::get_size, "Get camera render dimensions [width, height]")
        .def("set_viewport_rect", &Camera::set_viewport_rect, nb::arg("x"), nb::arg("y"), nb::arg("width"), nb::arg("height"), "Set viewport rectangle")
        .def("pick_actor_at_pixel", &Camera::pick_actor_at_pixel, nb::arg("x"), nb::arg("y"), "Pick actor at pixel coordinates");

    // ============================================================================
    // ImageEffects: 图像效果类
    // ============================================================================
    nb::class_<ImageEffects>(m, "ImageEffects")
        .def(nb::init<>(), "Create an ImageEffects instance");

    // ============================================================================
    // Environment: 环境类
    // ============================================================================
    nb::class_<Environment>(m, "Environment")
        .def(nb::init<>(), "Create an Environment")
        .def("set_sun_direction", &Environment::set_sun_direction, nb::arg("direction"),
             "Set sun light direction [x, y, z]")
        .def("get_sun_direction", &Environment::get_sun_direction,
             "Get sun light direction [x, y, z]")
        .def("set_floor_grid", &Environment::set_floor_grid, nb::arg("enabled"),
             "Enable or disable floor grid rendering")
        .def("get_floor_grid", &Environment::get_floor_grid,
             "Get floor grid rendering state")
        .def("set_gravity", &Environment::set_gravity, nb::arg("gravity"),
             "Set gravity vector [x, y, z]")
        .def("get_gravity", &Environment::get_gravity,
             "Get gravity vector [x, y, z]")
        .def("set_floor_y", &Environment::set_floor_y, nb::arg("y"),
             "Set floor plane Y height")
        .def("get_floor_y", &Environment::get_floor_y,
             "Get floor plane Y height")
        .def("set_floor_restitution", &Environment::set_floor_restitution, nb::arg("restitution"),
             "Set floor restitution (bounciness)")
        .def("get_floor_restitution", &Environment::get_floor_restitution,
             "Get floor restitution")
        .def("set_fixed_dt", &Environment::set_fixed_dt, nb::arg("dt"),
             "Set physics fixed time step")
        .def("get_fixed_dt", &Environment::get_fixed_dt,
             "Get physics fixed time step");

    // ============================================================================
    // Scene: 场景类
    // ============================================================================
    nb::class_<Scene>(m, "Scene")
        .def(nb::init<>(), "Create an empty Scene")
        // Environment management
        .def("set_environment", &Scene::set_environment, nb::arg("environment"),
             "Set the scene environment")
        .def("get_environment", &Scene::get_environment,
             "Get the scene environment",
             nb::rv_policy::reference)
        .def("has_environment", &Scene::has_environment,
             "Check if scene has an environment")
        .def("remove_environment", &Scene::remove_environment,
             "Remove environment from scene")
        // Actor management
        .def("add_actor", &Scene::add_actor, nb::arg("actor"),
             "Add an actor to the scene")
        .def("remove_actor", &Scene::remove_actor, nb::arg("actor"),
             "Remove an actor from the scene")
        .def("clear_actors", &Scene::clear_actors,
             "Remove all actors from the scene")
        .def("actor_count", &Scene::actor_count,
             "Get number of actors in the scene")
        .def("has_actor", &Scene::has_actor, nb::arg("actor"),
             "Check if actor is in the scene")
        // Camera management
        .def("add_camera", &Scene::add_camera, nb::arg("camera"),
             "Add a camera to the scene")
        .def("remove_camera", &Scene::remove_camera, nb::arg("camera"),
             "Remove a camera from the scene")
        .def("clear_cameras", &Scene::clear_cameras,
             "Remove all cameras from the scene")
        .def("set_active_camera", &Scene::set_active_camera, nb::arg("camera"),
             "Set the active camera for this scene")
        .def("get_active_camera_handle", &Scene::get_active_camera_handle,
             "Get the active camera handle")
        .def("camera_count", &Scene::camera_count,
             "Get number of cameras in the scene")
        .def("has_camera", &Scene::has_camera, nb::arg("camera"),
             "Check if camera is in the scene")
        .def("get_aabb", &Scene::get_aabb,
             "Get scene world AABB as [min_x, min_y, min_z, max_x, max_y, max_z]")
        // Scene enable/disable
        .def("set_enabled", &Scene::set_enabled, nb::arg("enabled"),
             "Enable or disable the scene (disabled scenes skip rendering and physics)")
        .def("is_enabled", &Scene::is_enabled,
             "Return True if the scene is currently enabled")
        // Scene simulation control
        .def("set_simulation_enabled", &Scene::set_simulation_enabled, nb::arg("enabled"),
             "Enable or disable physics simulation for this scene (does not affect rendering)")
        .def("is_simulation_enabled", &Scene::is_simulation_enabled,
             "Return True if physics simulation is enabled for this scene");

    // ============================================================================
    // Scene I/O utilities
    // ============================================================================
    // m.def("read_scene", &read_scene, nb::arg("scene_path"),
    //       "Load a scene from file",
    //       nb::rv_policy::take_ownership);
    // m.def("write_scene", &write_scene, nb::arg("scene"), nb::arg("scene_path"),
    //       "Save a scene to file");

    // ============================================================================
    // Logger: 日志前端转发接口
    // ============================================================================
    nb::class_<Corona::Kernel::LogEntry>(m, "LogEntry")
        .def_ro("level", &Corona::Kernel::LogEntry::level,
                "Log level string: TRACE/DEBUG/INFO/WARNING/ERROR/CRITICAL")
        .def_ro("message", &Corona::Kernel::LogEntry::message,
                "Formatted log message")
        .def_ro("timestamp", &Corona::Kernel::LogEntry::timestamp,
                "Timestamp in nanoseconds since epoch");

    m.def("drain_logs", []() -> std::vector<Corona::Kernel::LogEntry> { return Corona::Kernel::CoronaLogger::drain_logs(); }, "Drain all pending log entries from the engine log queue");

    m.def("send_log", [](const std::string& level, const std::string& message) {
              const auto is_optional_tool_config_error = [&message]() {
                  return message.find("Failed to load tools from") != std::string::npos &&
                         (message.find("api_key") != std::string::npos ||
                          message.find("base_url") != std::string::npos ||
                          message.find("placeholder") != std::string::npos ||
                          message.find("not configured") != std::string::npos ||
                          message.find("missing") != std::string::npos);
              };
              if (level == "TRACE") {
                  PY_LOG_TRACE("{}", message.c_str());
              } else if (level == "DEBUG") {
                  PY_LOG_DEBUG("{}", message.c_str());
              } else if (level == "INFO") {
                  PY_LOG_INFO("{}", message.c_str());
              } else if (level == "WARNING") {
                  PY_LOG_WARNING("{}", message.c_str());
              } else if (level == "ERROR") {
                  if (is_optional_tool_config_error()) {
                      PY_LOG_WARNING("{}", message.c_str());
                  } else {
                      PY_LOG_ERROR("{}", message.c_str());
                  }
              } else if (level == "CRITICAL") {
                  PY_LOG_CRITICAL("{}", message.c_str());
              } else {
                  PY_LOG_INFO("{}", message.c_str());  // Default to INFO
              } }, nb::arg("level"), nb::arg("message"), "Send a log message to the engine logger with specified level");

    // ============================================================================
    // Network collaboration bridge: Python may run AI locally; C++ owns state/network.
    // ============================================================================
    m.def("network_local_peer_id", []() -> std::string {
        auto sys = get_network_system();
        return sys ? sys->local_peer_id() : std::string{};
    }, "Return the local NetworkSystem peer id.");

    m.def("network_register_agent", [](const std::string& agent_id,
                                        const std::string& name,
                                        const std::string& persona) -> bool {
        auto sys = get_network_system();
        return sys && sys->lanchat_register_agent(agent_id, name, persona).ok;
    }, nb::arg("agent_id"), nb::arg("name"), nb::arg("persona"),
       "Register an AI agent in the C++ collaboration roster.");

    m.def("network_remove_agent", [](const std::string& agent_id) -> bool {
        auto sys = get_network_system();
        return sys && sys->lanchat_remove_agent(agent_id).ok;
    }, nb::arg("agent_id"), "Remove an AI agent from the C++ collaboration roster.");

    m.def("network_send_agent_reply", [](const std::string& agent_id,
                                          const std::string& agent_name,
                                          const std::string& text) -> bool {
        auto sys = get_network_system();
        return sys && sys->lanchat_send_agent_reply(agent_id, agent_name, text).accepted;
    }, nb::arg("agent_id"), nb::arg("agent_name"), nb::arg("text"),
       "Send an AI agent reply through the C++ reliable collaboration channel.");

    m.def("network_send_agent_reply_ex", [](const std::string& agent_id,
                                             const std::string& agent_name,
                                             const std::string& text,
                                             const std::string& message_kind,
                                             const std::string& target_agent_id,
                                             const std::string& correlation_id,
                                             const std::string& metadata_json) -> bool {
        auto sys = get_network_system();
        return sys && sys->lanchat_send_agent_reply_ex(
            agent_id, agent_name, text, "agent",
            message_kind.empty() ? "agent_reply" : message_kind,
            target_agent_id, "", correlation_id, metadata_json).accepted;
    }, nb::arg("agent_id"), nb::arg("agent_name"), nb::arg("text"),
       nb::arg("message_kind") = "agent_reply", nb::arg("target_agent_id") = "",
       nb::arg("correlation_id") = "", nb::arg("metadata_json") = "",
       "Send a structured AI agent reply through the C++ LANChat channel.");

    m.def("network_send_system_message_to_host_ex",
          [](const std::string& sender_id,
             const std::string& sender_name,
             const std::string& text,
             const std::string& message_kind,
             const std::string& correlation_id,
             const std::string& metadata_json) -> bool {
              auto sys = get_network_system();
              return sys && sys->lanchat_send_system_message_to_host_ex(
                  sender_id, sender_name, text, message_kind, correlation_id,
                  metadata_json).accepted;
          },
          nb::arg("sender_id"), nb::arg("sender_name"), nb::arg("text"),
          nb::arg("message_kind") = "action_status",
          nb::arg("correlation_id") = "", nb::arg("metadata_json") = "",
          "Send a host-only LANChat system message to the local host UI.");

    m.def("network_send_system_message_to_user_ex",
          [](const std::string& target_user_id,
             const std::string& sender_id,
             const std::string& sender_name,
             const std::string& text,
             const std::string& message_kind,
             const std::string& correlation_id,
             const std::string& metadata_json) -> bool {
              auto sys = get_network_system();
              return sys && sys->lanchat_send_system_message_to_user_ex(
                  target_user_id, sender_id, sender_name, text, message_kind,
                  correlation_id, metadata_json).accepted;
          },
          nb::arg("target_user_id"), nb::arg("sender_id"), nb::arg("sender_name"),
          nb::arg("text"), nb::arg("message_kind") = "action_status",
          nb::arg("correlation_id") = "", nb::arg("metadata_json") = "",
          "Send a local-user LANChat system message without room broadcast.");

	    m.def("network_pop_lanchat_agent_trigger", []() -> nb::object {
	        auto sys = get_network_system();
	        if (!sys) {
	            return nb::none();
        }
        auto trigger = sys->lanchat_pop_agent_trigger();
        if (!trigger.has_value()) {
            return nb::none();
        }

        nb::dict result;
        result["trigger_id"] = trigger->trigger_id;
        result["message_id"] = trigger->message_id;
        result["room_id"] = trigger->room_id;
        result["sender_id"] = trigger->sender_id;
        result["sender_name"] = trigger->sender_name;
        result["sender_type"] = trigger->sender_type;
        result["message_kind"] = trigger->message_kind;
        result["target_agent_id"] = trigger->target_agent_id;
        result["source_user_id"] = trigger->source_user_id;
        result["correlation_id"] = trigger->correlation_id;
        result["metadata_json"] = trigger->metadata_json;
        result["agent_id"] = trigger->agent_id;
        result["agent_name"] = trigger->agent_name;
        result["persona"] = trigger->persona;
        result["text"] = trigger->text;

        nb::list history;
        for (const auto& message : trigger->history) {
            nb::dict item;
            item["message_id"] = message.message_id;
            item["sender_id"] = message.sender_id;
            item["room_id"] = message.room_id;
            item["seq"] = message.seq;
            item["from"] = message.sender_name;
            item["text"] = message.text;
            item["ts"] = message.timestamp_ms / 1000;
            item["sender_type"] = message.sender_type;
            item["message_kind"] = message.message_kind;
            item["target_agent_id"] = message.target_agent_id;
            item["source_user_id"] = message.source_user_id;
            item["correlation_id"] = message.correlation_id;
            item["metadata_json"] = message.metadata_json;
            history.append(item);
        }
	        result["history"] = history;
	        return nb::object(result);
		    }, "Pop one pending LANChat AI agent trigger owned by this peer.");

        m.def("network_pop_lanchat_coordinator_sync_message", []() -> nb::object {
            auto sys = get_network_system();
            if (!sys) {
                return nb::none();
            }
            auto message = sys->lanchat_pop_coordinator_sync_message();
            if (!message.has_value()) {
                return nb::none();
            }
            return nb::object(lanchat_message_to_dict(*message));
        }, "Pop one ordinary LANChat message that should be synced into InteractionCoordinator.");

        m.def("network_session_role_name", []() -> std::string {
            auto sys = get_network_system();
            if (!sys) {
                return "none";
            }
            return std::string(sys->session_role_name());
        }, "Return current networking session role: none, host, or client.");

        m.def("network_pop_lanchat_room_event", []() -> nb::object {
            auto sys = get_network_system();
            if (!sys) {
                return nb::none();
            }
            auto event = sys->lanchat_pop_room_event();
            if (!event.has_value()) {
                return nb::none();
            }
            nb::dict item;
            item["channel"] = "lanchat";
            item["event"] = event->event;
            item["room_id"] = event->room_id;
            return nb::object(item);
        }, "Pop one LANChat room lifecycle event for Python scheduler/Coordinator cleanup.");

		    m.def("network_lanchat_history_snapshot", [](int limit) -> nb::list {
	        nb::list history;
	        auto sys = get_network_system();
	        if (!sys) {
	            return history;
	        }
	        const auto& messages = sys->lanchat_history();
	        const size_t total = messages.size();
	        const size_t keep = limit > 0 ? std::min<size_t>(static_cast<size_t>(limit), total) : total;
	        const size_t start = total > keep ? total - keep : 0;
	        for (size_t i = start; i < total; ++i) {
	            const auto& message = messages[i];
	            nb::dict item;
	            item["message_id"] = message.message_id;
	            item["sender_id"] = message.sender_id;
	            item["room_id"] = message.room_id;
	            item["seq"] = message.seq;
            item["from"] = message.sender_name;
            item["text"] = message.text;
            item["ts"] = message.timestamp_ms / 1000;
            item["sender_type"] = message.sender_type;
            item["message_kind"] = message.message_kind;
            item["target_agent_id"] = message.target_agent_id;
            item["source_user_id"] = message.source_user_id;
            item["correlation_id"] = message.correlation_id;
            item["metadata_json"] = message.metadata_json;
            history.append(item);
	        }
	        return history;
	    }, nb::arg("limit") = 20, "Return a recent LANChat history snapshot for Python AI/GM logic.");

	    m.def("network_lanchat_agents_snapshot", []() -> nb::list {
	        nb::list agents;
	        auto sys = get_network_system();
	        if (!sys) {
	            return agents;
	        }
	        for (const auto& agent : sys->lanchat_agents()) {
	            nb::dict item;
	            item["agent_id"] = agent.agent_id;
	            item["name"] = agent.name;
	            item["persona"] = agent.persona;
	            item["owner"] = agent.owner_id;
	            agents.append(item);
	        }
	        return agents;
	    }, "Return the C++ LANChat agent roster for Python AI/GM logic.");

	    m.def("network_send_system_message", [](const std::string& sender_id,
	                                             const std::string& sender_name,
	                                             const std::string& text) -> bool {
	        auto sys = get_network_system();
	        return sys && sys->lanchat_send_agent_reply_ex(
                sender_id, sender_name, text, "system", "agent_reply").accepted;
	    }, nb::arg("sender_id"), nb::arg("sender_name"), nb::arg("text"),
	       "Send a GM/system message through the C++ reliable LANChat channel.");

	    m.def("network_send_system_message_ex", [](const std::string& sender_id,
	                                                const std::string& sender_name,
	                                                const std::string& text,
	                                                const std::string& message_kind,
	                                                const std::string& correlation_id,
	                                                const std::string& metadata_json) -> bool {
	        auto sys = get_network_system();
	        return sys && sys->lanchat_send_agent_reply_ex(
                sender_id, sender_name, text, "system",
                message_kind.empty() ? "agent_reply" : message_kind,
                "", "", correlation_id, metadata_json).accepted;
	    }, nb::arg("sender_id"), nb::arg("sender_name"), nb::arg("text"),
           nb::arg("message_kind") = "agent_reply", nb::arg("correlation_id") = "",
           nb::arg("metadata_json") = "",
	       "Send a structured GM/system message through the C++ reliable LANChat channel.");

	    m.def("network_lock_object", [](const std::string& object_id,
	                                     const std::string& user_id,
                                     const std::string& operation) -> bool {
        auto sys = get_network_system();
        return sys && sys->lanchat_lock_object(
            object_id, user_id, operation, network_now_ms()).ok;
    }, nb::arg("object_id"), nb::arg("user_id"), nb::arg("operation") = "modify",
       "Acquire an object collaboration lock through C++ state.");

    m.def("network_unlock_object", [](const std::string& object_id,
                                       const std::string& user_id) -> bool {
        auto sys = get_network_system();
        return sys && sys->lanchat_unlock_object(object_id, user_id).ok;
    }, nb::arg("object_id"), nb::arg("user_id"),
       "Release an object collaboration lock through C++ state.");

    m.def("network_locked_by", [](const std::string& object_id) -> std::string {
        auto sys = get_network_system();
        return sys ? sys->lanchat_locked_by(object_id, network_now_ms()) : std::string{};
    }, nb::arg("object_id"), "Return the owner of a C++ collaboration lock.");

    m.def("network_broadcast_intent", [](const std::string& user_id,
                                          const std::string& tooltip,
                                          const std::array<float, 3>& position,
                                          const std::string& status) {
        auto sys = get_network_system();
        if (sys) {
            sys->lanchat_broadcast_intent(user_id, tooltip, position, status, network_now_ms());
        }
    }, nb::arg("user_id"), nb::arg("tooltip"), nb::arg("position"),
       nb::arg("status") = "placing_object",
       "Record a local operation preview intent in C++ collaboration state.");

    m.def("network_check_preview_collision", [](const std::string& user_id,
                                                 const std::array<float, 3>& position,
                                                 float delta) -> std::string {
        auto sys = get_network_system();
        return sys ? sys->lanchat_check_preview_collision(
                         user_id, position, delta, network_now_ms())
                   : std::string{};
    }, nb::arg("user_id"), nb::arg("position"), nb::arg("delta") = 0.5f,
       "Return the conflicting user id for a preview placement, if any.");

    // ============================================================================
    // Render backend control (Native vs Vision)
    // ============================================================================
    m.def("is_vision_available", &is_vision_available,
          "Return True if the engine was compiled with Vision (CORONA_ENABLE_VISION) support");
    m.def("set_render_backend", &set_render_backend, nb::arg("mode"),
          nb::arg("camera_handle") = 0,
          "Request a render backend switch. mode: 'native' or 'vision'. Only effective when Vision is available.");
    m.def("get_render_backend", &get_render_backend, nb::arg("camera_handle") = 0,
          "Get the currently requested render backend as 'native' or 'vision'");
    m.def("set_vision_render_mode", &set_vision_render_mode, nb::arg("mode"),
          nb::arg("camera_handle") = 0,
          "Set the requested Vision render mode: 'path_tracing', 'svgf', or 'ssat'");
    m.def("get_vision_render_mode", &get_vision_render_mode,
          nb::arg("camera_handle") = 0,
          "Get the requested Vision render mode");
    m.def("load_vision_scene", &load_vision_scene, nb::arg("path"),
          "Load an external Vision scene file (.json). Pass an empty string to "
          "unload and restore the engine-built scene. Only effective when Vision "
          "is available and the Vision backend is active.");

    // ============================================================================
    // Media (video/audio) import — standalone resources, not 3D actors
    // ============================================================================
    nb::class_<MediaInfo>(m, "MediaInfo")
        .def_ro("resource_id", &MediaInfo::resource_id, "Resource ID (0 means import failed)")
        .def_ro("media_type", &MediaInfo::media_type, "'video' / 'audio' / '' (failed)")
        .def_ro("duration_seconds", &MediaInfo::duration_seconds, "Duration in seconds")
        .def_ro("codec", &MediaInfo::codec, "Codec name")
        .def_ro("width", &MediaInfo::width, "Video width in pixels")
        .def_ro("height", &MediaInfo::height, "Video height in pixels")
        .def_ro("fps", &MediaInfo::fps, "Video frames per second")
        .def_ro("sample_rate", &MediaInfo::sample_rate, "Audio sample rate in Hz")
        .def_ro("channels", &MediaInfo::channels, "Audio channel count");

    m.def("import_media", &import_media, nb::arg("path"),
          "Import an audio or video file as a standalone resource. "
          "Returns a MediaInfo (resource_id is 0 / media_type is '' on failure).");

    m.def("play_audio", &play_audio, nb::arg("resource_id"), nb::arg("loop") = false,
          "Play an imported audio resource. Pass the resource_id from MediaInfo.");
    m.def("stop_audio", &stop_audio, nb::arg("resource_id"),
          "Stop playing an audio resource.");

    // ============================================================================
    // Engine lifecycle
    // ============================================================================
    m.def("request_engine_exit", []() {
        SDL_Event quit_event;
        SDL_zero(quit_event);
        quit_event.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit_event);
    }, "Request graceful engine shutdown. "
       "Pushes an SDL_QUIT event, same as clicking the window close button.");

    // ============================================================================
    // CameraFollowController: 摄像头跟随 C++ 快速通道
    // ============================================================================
    m.def("camera_follow_set_target", [](std::uintptr_t actor_handle, std::uintptr_t camera_handle,
                                         float ox, float oy, float oz) {
        Corona::Systems::CameraFollowController::instance().set_target(
            actor_handle, camera_handle, ox, oy, oz);
    }, nb::arg("actor_handle"), nb::arg("camera_handle"),
       nb::arg("offset_x"), nb::arg("offset_y"), nb::arg("offset_z"),
       "Set camera follow target with actor/camera handles and offset");

    m.def("camera_follow_clear", []() {
        Corona::Systems::CameraFollowController::instance().clear_target();
    }, "Clear the camera follow target");

    m.def("camera_follow_set_input_enabled", [](bool enabled) {
        Corona::Systems::CameraFollowController::instance().set_input_enabled(enabled);
    }, nb::arg("enabled"),
       "Enable or disable editor camera-follow keyboard/mouse input");

    m.def("camera_follow_inject_rmb", [](bool down, int x, int y) {
        Corona::Systems::CameraFollowController::instance().inject_rmb(down, x, y);
    }, nb::arg("down"), nb::arg("screen_x"), nb::arg("screen_y"),
       "Inject right mouse button state for camera orbit");

    // ============================================================================
    // Input 事件队列：积木脚本键盘/鼠标注入 → CEF ProcessMessage → 队列 → Python 消费
    // InputEvent / drain_input_events 定义在 src/systems/ui/cef/cef_bridge_helpers.h
    // (forward-declared above because script system shouldn't depend on UI includes)
    // ============================================================================
    nb::class_<Corona::Systems::UI::InputEvent>(m, "InputEvent")
        .def_ro("type", &Corona::Systems::UI::InputEvent::type,
                "0=keyDown, 1=keyUp, 2=mouseEvent")
        .def_ro("arg0", &Corona::Systems::UI::InputEvent::arg0,
                "key code (keyDown/keyUp) or eventType (mouse)")
        .def_ro("arg1", &Corona::Systems::UI::InputEvent::arg1,
                "modifiers (keyDown) / button (mouse) / displayKey (keyUp)")
        .def_ro("arg2", &Corona::Systems::UI::InputEvent::arg2,
                "displayKey (keyDown only)")
        .def_ro("arg3", &Corona::Systems::UI::InputEvent::arg3,
                "x (mouse)")
        .def_ro("arg4", &Corona::Systems::UI::InputEvent::arg4,
                "y (mouse)");

    m.def("drain_input_events", []() -> std::vector<Corona::Systems::UI::InputEvent> {
        return Corona::Systems::UI::drain_input_events();
    }, "Drain all pending input events from the CEF InputInject queue");

}

}  // namespace EngineScripts
