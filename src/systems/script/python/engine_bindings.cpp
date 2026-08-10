#include <corona/kernel/core/callback_sink.h>
#include <corona/kernel/core/i_logger.h>
#include <corona/engine/engine_runtime_api.h>
#include <corona/systems/script/engine_scripts.h>
#include <corona/systems/script/python_runtime_coordinator.h>
#include <nanobind/nanobind.h>
#include <nanobind/stl/array.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

#include <nlohmann/json.hpp>

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
std::atomic<std::uint64_t> g_next_python_callback_token{1};
std::unordered_map<std::uint64_t, PyObject*> g_python_callback_registry;

PyObject* json_to_python(const nlohmann::json& value) {
    if (value.is_null()) return Py_NewRef(Py_None);
    if (value.is_boolean()) return PyBool_FromLong(value.get<bool>() ? 1 : 0);
    if (value.is_number_integer()) return PyLong_FromLongLong(value.get<long long>());
    if (value.is_number_unsigned()) return PyLong_FromUnsignedLongLong(value.get<unsigned long long>());
    if (value.is_number_float()) return PyFloat_FromDouble(value.get<double>());
    if (value.is_string()) return PyUnicode_FromString(value.get_ref<const std::string&>().c_str());
    if (value.is_array()) {
        PyObject* tuple = PyTuple_New(static_cast<Py_ssize_t>(value.size()));
        if (!tuple) return nullptr;
        for (std::size_t index = 0; index < value.size(); ++index) {
            PyObject* item = json_to_python(value[index]);
            if (!item) {
                Py_DECREF(tuple);
                return nullptr;
            }
            PyTuple_SET_ITEM(tuple, static_cast<Py_ssize_t>(index), item);
        }
        return tuple;
    }
    if (value.is_object()) {
        PyObject* dict = PyDict_New();
        if (!dict) return nullptr;
        for (auto it = value.begin(); it != value.end(); ++it) {
            PyObject* item = json_to_python(it.value());
            if (!item || PyDict_SetItemString(dict, it.key().c_str(), item) != 0) {
                Py_XDECREF(item);
                Py_DECREF(dict);
                return nullptr;
            }
            Py_DECREF(item);
        }
        return dict;
    }
    PyErr_SetString(PyExc_TypeError, "unsupported callback JSON value");
    return nullptr;
}

Corona::Script::Python::PythonRuntimeResponse execute_python_callback(
    const Corona::Script::Python::PythonRuntimeRequest& request) {
    using Corona::Script::Python::PythonRuntimeResponse;
    if (!PyGILState_Check()) {
        return PythonRuntimeResponse::failure("python callback executed without the GIL");
    }
    const auto it = g_python_callback_registry.find(request.callback_token);
    if (request.function == "release") {
        if (it != g_python_callback_registry.end()) {
            Py_DECREF(it->second);
            g_python_callback_registry.erase(it);
        }
        return PythonRuntimeResponse::success();
    }
    if (it == g_python_callback_registry.end()) {
        return PythonRuntimeResponse::failure("python callback token is no longer registered");
    }
    const auto args_json = nlohmann::json::parse(request.payload_json.empty() ? "[]" : request.payload_json,
                                                 nullptr, false);
    if (args_json.is_discarded() || !args_json.is_array()) {
        return PythonRuntimeResponse::failure("python callback arguments are invalid");
    }
    PyObject* args = json_to_python(args_json);
    if (!args) {
        PyErr_Print();
        return PythonRuntimeResponse::failure("failed to convert python callback arguments");
    }
    PyObject* result = PyObject_CallObject(it->second, args);
    Py_DECREF(args);
    if (!result) {
        PyErr_Print();
        return PythonRuntimeResponse::failure("python callback raised an exception");
    }
    Py_DECREF(result);
    return PythonRuntimeResponse::success();
}

bool enqueue_python_callback(std::uint64_t callback_token,
                             nlohmann::json args,
                             std::string function = "invoke") {
    auto* coordinator = Corona::Script::Python::active_python_runtime_coordinator();
    if (!coordinator) return false;
    Corona::Script::Python::PythonRuntimeRequest request;
    request.kind = Corona::Script::Python::PythonRuntimeRequestKind::Callback;
    request.source = "Mechanics";
    request.function = std::move(function);
    request.callback_token = callback_token;
    request.payload_json = std::move(args).dump();
    request.handler = &execute_python_callback;
    return coordinator->submit(std::move(request)).accepted;
}

std::uint64_t register_python_callback(PyObject* callback) {
    if (!PyGILState_Check() || !callback || !PyCallable_Check(callback)) return 0;
    const auto callback_token = g_next_python_callback_token.fetch_add(1);
    Py_INCREF(callback);
    g_python_callback_registry.emplace(callback_token, callback);
    return callback_token;
}

struct PythonCallbackLease {
    explicit PythonCallbackLease(std::uint64_t value) : callback_token(value) {}
    ~PythonCallbackLease() {
        if (callback_token != 0) {
            enqueue_python_callback(callback_token, nlohmann::json::array(), "release");
        }
    }
    std::uint64_t callback_token = 0;
};

std::shared_ptr<PythonCallbackLease> make_python_callback_lease(PyObject* callback) {
    const auto callback_token = register_python_callback(callback);
    return callback_token == 0 ? nullptr : std::make_shared<PythonCallbackLease>(callback_token);
}
}  // namespace

void clear_python_callback_registry() {
    if (!PyGILState_Check()) {
        CFW_LOG_ERROR("[Bindings::callback_registry] clear requested without the GIL");
        return;
    }
    for (auto& [_, callback] : g_python_callback_registry) Py_DECREF(callback);
    g_python_callback_registry.clear();
}

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
             "Get model AABB [min_x, min_y, min_z, max_x, max_y, max_z]")
        .def("is_valid", &Geometry::is_valid,
             "Return whether the underlying geometry resource loaded successfully");

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

                auto callback_lease = make_python_callback_lease(callback.ptr());
                if (!callback_lease) return;

                CallbackType cb = [callback_lease](std::uintptr_t other, bool began, const std::array<float, 3>& normal, const std::array<float, 3>& point) {
                    enqueue_python_callback(callback_lease->callback_token,
                                            nlohmann::json::array({other, began, normal, point}));
                };

                 self.set_collision_callback(cb); },

             nb::arg("callback"), "Set collision callback. Callback receives (other_handle, normal, point) where normal and point are (x,y,z) tuples.")
        .def("set_on_move_callback", [](Mechanics& self, nb::object callback) {
                using CallbackType = std::function<void()>;

                if (callback.is_none()) {
                    self.set_on_move_callback(CallbackType{});
                    return;
                }

                auto callback_lease = make_python_callback_lease(callback.ptr());
                if (!callback_lease) return;

                CallbackType cb = [callback_lease]() {
                    enqueue_python_callback(callback_lease->callback_token,
                                            nlohmann::json::array());
                };

                self.set_on_move_callback(cb); }, nb::arg("callback"), "Set move callback for geometry.")
        .def("set_collision_shape", &Mechanics::set_collision_shape, nb::arg("shape"))
        .def("get_collision_shape", &Mechanics::get_collision_shape);

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
             "Get whether audio is enabled for this object")
        .def("set_audio_resource", &Acoustics::set_audio_resource, nb::arg("resource_id"),
             "Bind the audio resource id to play at this object's position")
        .def("get_audio_resource", &Acoustics::get_audio_resource,
             "Get the bound audio resource id")
        .def("play", &Acoustics::play, nb::arg("loop") = false,
             "Play the bound audio at this object's world position (spatial)")
        .def("stop", &Acoustics::stop,
             "Stop this object's spatial playback");

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
             nb::arg("model_path"), nb::arg("visible") = true)
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
             "Save a screenshot and block until it completes. Returns True on success.",
             nb::call_guard<nb::gil_scoped_release>())
        .def("set_output_mode", &Camera::set_output_mode, nb::arg("mode"),
             "Set camera output mode. mode: 'final_color', 'base_color', 'normal', 'position', 'object_id', 'visibility_buffer', 'ssao_raw', 'ssao', 'shadow_mask_raw', 'shadow_mask'")
        .def("get_output_mode", &Camera::get_output_mode,
             "Get current camera output mode as string")
        .def("set_render_backend", &Camera::set_render_backend, nb::arg("mode"))
        .def("get_render_backend", &Camera::get_render_backend)
        .def("set_vision_render_mode", &Camera::set_vision_render_mode, nb::arg("mode"))
        .def("get_vision_render_mode", &Camera::get_vision_render_mode)
        .def("set_shadow_cascade_debug", &Camera::set_shadow_cascade_debug, nb::arg("enabled"))
        .def("get_shadow_cascade_debug", &Camera::get_shadow_cascade_debug)
        .def("set_ssao_enabled", &Camera::set_ssao_enabled, nb::arg("enabled"))
        .def("get_ssao_enabled", &Camera::get_ssao_enabled)
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
        .def("set_sun_intensity", &Environment::set_sun_intensity, nb::arg("intensity"),
             "Set sun light intensity")
        .def("get_sun_intensity", &Environment::get_sun_intensity,
             "Get sun light intensity")
        .def("set_sky_intensity", &Environment::set_sky_intensity, nb::arg("intensity"),
             "Set atmospheric sky intensity")
        .def("get_sky_intensity", &Environment::get_sky_intensity,
             "Get atmospheric sky intensity")
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

    m.def("python_runtime_phase", [](const std::string& phase) {
        if (auto* coordinator = Corona::Script::Python::active_python_runtime_coordinator()) {
            coordinator->set_execution_phase(phase);
        }
    }, nb::arg("phase"), "Update the native Python runtime diagnostic phase");

}

}  // namespace EngineScripts
