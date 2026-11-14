#include <horizon/core/logging.h>
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
#include "python_value.h"

#include <corona/systems/script/script_input.h>

namespace nb = nanobind;
using namespace Corona::API;

namespace EngineScripts {

namespace {
std::atomic<std::uint64_t> g_next_python_callback_token{1};
std::unordered_map<std::uint64_t, PyObject*> g_python_callback_registry;

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
    if (request.cancelled()) {
        return PythonRuntimeResponse::timeout();
    }
    const auto args_json = nlohmann::json::parse(request.payload_json.empty() ? "[]" : request.payload_json,
                                                 nullptr, false);
    if (args_json.is_discarded() || !args_json.is_array()) {
        return PythonRuntimeResponse::failure("python callback arguments are invalid");
    }
    PyObject* args = Corona::Script::Python::json_to_python(args_json, true);
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
        .def("set_transform", &Geometry::set_transform, nb::arg("position"), nb::arg("rotation"),
             nb::arg("scale"),
             "Set the complete local transform; pass all three vectors, read the current ones first "
             "if the caller only wants to change one")
        .def("set_matrix", &Geometry::set_matrix, nb::arg("matrix"),
             "Set a column-major affine matrix, preserving shear and reflection")
        .def("get_matrix", &Geometry::get_matrix)
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
        .def("set_properties",
             [](Mechanics& self, nb::object mass, nb::object restitution, nb::object damping,
                nb::object body_type, nb::object linear_lock, nb::object angular_lock) {
                 MechanicsProperties properties;
                 // Omitted keyword arguments leave the corresponding field untouched.
                 if (!mass.is_none()) properties.mass = nb::cast<float>(mass);
                 if (!restitution.is_none()) properties.restitution = nb::cast<float>(restitution);
                 if (!damping.is_none()) properties.damping = nb::cast<float>(damping);
                 if (!body_type.is_none()) properties.body_type = nb::cast<std::string>(body_type);
                 auto read_lock = [](const nb::object& value, std::array<bool, 3>& out) {
                     const auto sequence = nb::cast<nb::list>(value);
                     if (nb::len(sequence) != 3) {
                         throw std::invalid_argument("axis lock needs three booleans");
                     }
                     for (std::size_t i = 0; i < 3; ++i) {
                         out[i] = nb::cast<bool>(sequence[i]);
                     }
                 };
                 if (!linear_lock.is_none()) {
                     std::array<bool, 3> lock{};
                     read_lock(linear_lock, lock);
                     properties.linear_lock = lock;
                 }
                 if (!angular_lock.is_none()) {
                     std::array<bool, 3> lock{};
                     read_lock(angular_lock, lock);
                     properties.angular_lock = lock;
                 }
                 self.set_properties(properties);
             },
             nb::arg("mass") = nb::none(), nb::arg("restitution") = nb::none(),
             nb::arg("damping") = nb::none(), nb::arg("body_type") = nb::none(),
             nb::arg("linear_lock") = nb::none(), nb::arg("angular_lock") = nb::none(),
             "Set any of mass / restitution / damping / body_type / linear_lock / angular_lock in "
             "one write; body_type is \"dynamic\", \"kinematic\", \"static\" or \"phantom\" and the "
             "locks are three booleans")
        .def("get_state", [](const Mechanics& self) {
                 const auto state = self.get_state();
                 nb::dict out;
                 out["mass"] = state.mass;
                 out["restitution"] = state.restitution;
                 out["damping"] = state.damping;
                 out["body_type"] = state.body_type;
                 out["linear_lock"] = state.linear_lock;
                 out["angular_lock"] = state.angular_lock;
                 return out;
             },
             "Every mechanics field from one snapshot: mass, restitution, damping, body_type, "
             "linear_lock and angular_lock")
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
        ;

    // ============================================================================
    // Optics: 光学/渲染组件
    // ============================================================================
    nb::class_<Optics>(m, "Optics")
        .def(nb::init<Geometry&>(), nb::arg("geometry"),
             "Create an Optics component attached to a Geometry")
        .def("set_properties",
             [](Optics& self, nb::object visible, nb::object lighting_enabled,
                nb::object metallic, nb::object roughness, nb::object subsurface,
                nb::object specular, nb::object specular_tint, nb::object anisotropic,
                nb::object sheen, nb::object sheen_tint, nb::object clearcoat,
                nb::object clearcoat_gloss, nb::object ambient, nb::object diffuse,
                nb::object specular_color, nb::object shininess) {
                 OpticsProperties properties;
                 // Omitted keyword arguments leave the corresponding field untouched.
                 auto read_float3 = [](const nb::object& value, std::array<float, 3>& out) {
                     const auto sequence = nb::cast<nb::list>(value);
                     if (nb::len(sequence) != 3) {
                         throw std::invalid_argument("expected three numbers");
                     }
                     for (std::size_t i = 0; i < 3; ++i) out[i] = nb::cast<float>(sequence[i]);
                 };
                 if (!visible.is_none()) properties.visible = nb::cast<bool>(visible);
                 if (!lighting_enabled.is_none()) properties.lighting_enabled = nb::cast<bool>(lighting_enabled);
                 if (!metallic.is_none()) properties.metallic = nb::cast<float>(metallic);
                 if (!roughness.is_none()) properties.roughness = nb::cast<float>(roughness);
                 if (!subsurface.is_none()) properties.subsurface = nb::cast<float>(subsurface);
                 if (!specular.is_none()) properties.specular = nb::cast<float>(specular);
                 if (!specular_tint.is_none()) properties.specular_tint = nb::cast<float>(specular_tint);
                 if (!anisotropic.is_none()) properties.anisotropic = nb::cast<float>(anisotropic);
                 if (!sheen.is_none()) properties.sheen = nb::cast<float>(sheen);
                 if (!sheen_tint.is_none()) properties.sheen_tint = nb::cast<float>(sheen_tint);
                 if (!clearcoat.is_none()) properties.clearcoat = nb::cast<float>(clearcoat);
                 if (!clearcoat_gloss.is_none()) properties.clearcoat_gloss = nb::cast<float>(clearcoat_gloss);
                 if (!ambient.is_none()) {
                     std::array<float, 3> value{};
                     read_float3(ambient, value);
                     properties.ambient = value;
                 }
                 if (!diffuse.is_none()) {
                     std::array<float, 3> value{};
                     read_float3(diffuse, value);
                     properties.diffuse = value;
                 }
                 if (!specular_color.is_none()) {
                     std::array<float, 3> value{};
                     read_float3(specular_color, value);
                     properties.specular_color = value;
                 }
                 if (!shininess.is_none()) properties.shininess = nb::cast<float>(shininess);
                 self.set_properties(properties);
             },
             nb::arg("visible") = nb::none(), nb::arg("lighting_enabled") = nb::none(),
             nb::arg("metallic") = nb::none(), nb::arg("roughness") = nb::none(),
             nb::arg("subsurface") = nb::none(), nb::arg("specular") = nb::none(),
             nb::arg("specular_tint") = nb::none(), nb::arg("anisotropic") = nb::none(),
             nb::arg("sheen") = nb::none(), nb::arg("sheen_tint") = nb::none(),
             nb::arg("clearcoat") = nb::none(), nb::arg("clearcoat_gloss") = nb::none(),
             nb::arg("ambient") = nb::none(), nb::arg("diffuse") = nb::none(),
             nb::arg("specular_color") = nb::none(), nb::arg("shininess") = nb::none(),
             "Set any optics field in one write")
        .def("get_state", [](const Optics& self) {
                 const auto state = self.get_state();
                 nb::dict out;
                 out["metallic"] = state.metallic;
                 out["roughness"] = state.roughness;
                 out["subsurface"] = state.subsurface;
                 out["specular"] = state.specular;
                 out["specular_tint"] = state.specular_tint;
                 out["anisotropic"] = state.anisotropic;
                 out["sheen"] = state.sheen;
                 out["sheen_tint"] = state.sheen_tint;
                 out["clearcoat"] = state.clearcoat;
                 out["clearcoat_gloss"] = state.clearcoat_gloss;
                 out["visible"] = state.visible;
                 out["lighting_enabled"] = state.lighting_enabled;
                 out["ambient"] = state.ambient;
                 out["diffuse"] = state.diffuse;
                 out["specular_color"] = state.specular_color;
                 out["shininess"] = state.shininess;
                 return out;
             },
             "Every optics field from one snapshot");

    // ============================================================================
    // Acoustics: 声学组件
    // ============================================================================
    nb::class_<Acoustics>(m, "Acoustics")
        .def(nb::init<Geometry&>(), nb::arg("geometry"),
             "Create an Acoustics component attached to a Geometry")
        .def("set_properties",
             [](Acoustics& self, nb::object volume, nb::object audio_enabled, nb::object audio_resource) {
                 AcousticsProperties properties;
                 // Omitted keyword arguments leave the corresponding field untouched.
                 if (!volume.is_none()) properties.volume = nb::cast<float>(volume);
                 if (!audio_enabled.is_none()) properties.audio_enabled = nb::cast<bool>(audio_enabled);
                 if (!audio_resource.is_none()) {
                     properties.audio_resource = nb::cast<std::uint64_t>(audio_resource);
                 }
                 self.set_properties(properties);
             },
             nb::arg("volume") = nb::none(), nb::arg("audio_enabled") = nb::none(),
             nb::arg("audio_resource") = nb::none(),
             "Set volume, audio enable or the bound audio resource id in one write")
        .def("get_state", [](const Acoustics& self) {
                 const auto state = self.get_state();
                 nb::dict out;
                 out["volume"] = state.volume;
                 out["audio_enabled"] = state.audio_enabled;
                 out["audio_resource"] = state.audio_resource;
                 return out;
             },
             "Every acoustics field from one snapshot")
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
        .def("set_vision_denoise", &Camera::set_vision_denoise, nb::arg("enabled"))
        .def("get_vision_denoise", &Camera::get_vision_denoise)
        .def("set_vision_denoiser", &Camera::set_vision_denoiser, nb::arg("type"))
        .def("get_vision_denoiser", &Camera::get_vision_denoiser)
        .def("set_vision_accumulation", &Camera::set_vision_accumulation, nb::arg("enabled"))
        .def("get_vision_accumulation", &Camera::get_vision_accumulation)
        .def("get_requested_vision_accumulation", &Camera::get_requested_vision_accumulation)
        .def("set_vision_stable_planes", &Camera::set_vision_stable_planes, nb::arg("enabled"))
        .def("get_vision_stable_planes", &Camera::get_vision_stable_planes)
        .def("get_requested_vision_stable_planes", &Camera::get_requested_vision_stable_planes)
        .def("set_view", [](Camera& self, nb::object open, int x, int y, int width, int height,
                            float move_speed) {
                Corona::API::CameraViewState state;
                // None leaves the open state untouched; a bool also drives the window lifecycle.
                if (!open.is_none()) state.open = nb::cast<bool>(open);
                state.x = x; state.y = y; state.width = width; state.height = height;
                state.move_speed = move_speed;
                self.set_view(state);
            }, nb::arg("open"), nb::arg("x"), nb::arg("y"), nb::arg("width"), nb::arg("height"),
            nb::arg("move_speed") = 1.0f,
            "Set the viewport; pass open=True/False to also drive the window lifecycle, or None to "
            "change only the rectangle and speed")
        .def("get_view", [](const Camera& self) {
                 const auto state = self.get_view();
                 nb::dict out;
                 out["open"] = state.open.value_or(false);
                 out["x"] = state.x;
                 out["y"] = state.y;
                 out["width"] = state.width;
                 out["height"] = state.height;
                 out["move_speed"] = state.move_speed;
                 return out;
             },
             "The viewport state: open, x, y, width, height and move speed")
        .def("set_surface", [](Camera& self, std::uintptr_t surface) { self.set_surface(reinterpret_cast<void*>(surface)); }, nb::arg("surface"), "Set render surface (pass window ID as integer)")
        .def("get_surface", [](const Camera& self) -> std::uintptr_t { return reinterpret_cast<std::uintptr_t>(self.get_surface()); }, "Get render surface handle as integer (0 if none)")
        .def("set_offscreen_capture_mode", &Camera::set_offscreen_capture_mode, nb::arg("enabled"), "Detach camera from the default surface for screenshot-only rendering")
        .def("get_pose", [](const Camera& self) {
                 const auto pose = self.get_pose();
                 nb::dict out;
                 out["position"] = pose.position;
                 out["forward"] = pose.forward;
                 out["world_up"] = pose.world_up;
                 out["fov"] = pose.fov;
                 return out;
             },
             "Every camera pose field from one snapshot")
        .def("set_size", &Camera::set_size, nb::arg("width"), nb::arg("height"), "Set camera render dimensions")
        .def("get_size", &Camera::get_size, "Get camera render dimensions [width, height]");

    // ============================================================================
    // Environment: 环境类
    // ============================================================================
    nb::class_<Environment>(m, "Environment")
        .def(nb::init<>(), "Create an Environment")
        .def("set_properties",
             [](Environment& self, nb::object sun_direction, nb::object sun_intensity,
                nb::object sky_intensity, nb::object floor_grid, nb::object gravity,
                nb::object floor_y, nb::object floor_restitution, nb::object fixed_dt) {
                 EnvironmentProperties properties;
                 // Omitted keyword arguments leave the corresponding field untouched.
                 auto read_float3 = [](const nb::object& value, std::array<float, 3>& out) {
                     const auto sequence = nb::cast<nb::list>(value);
                     if (nb::len(sequence) != 3) {
                         throw std::invalid_argument("expected three numbers");
                     }
                     for (std::size_t i = 0; i < 3; ++i) out[i] = nb::cast<float>(sequence[i]);
                 };
                 if (!sun_direction.is_none()) {
                     std::array<float, 3> value{};
                     read_float3(sun_direction, value);
                     properties.sun_direction = value;
                 }
                 if (!sun_intensity.is_none()) properties.sun_intensity = nb::cast<float>(sun_intensity);
                 if (!sky_intensity.is_none()) properties.sky_intensity = nb::cast<float>(sky_intensity);
                 if (!floor_grid.is_none()) properties.floor_grid = nb::cast<bool>(floor_grid);
                 if (!gravity.is_none()) {
                     std::array<float, 3> value{};
                     read_float3(gravity, value);
                     properties.gravity = value;
                 }
                 if (!floor_y.is_none()) properties.floor_y = nb::cast<float>(floor_y);
                 if (!floor_restitution.is_none()) {
                     properties.floor_restitution = nb::cast<float>(floor_restitution);
                 }
                 if (!fixed_dt.is_none()) properties.fixed_dt = nb::cast<float>(fixed_dt);
                 self.set_properties(properties);
             },
             nb::arg("sun_direction") = nb::none(), nb::arg("sun_intensity") = nb::none(),
             nb::arg("sky_intensity") = nb::none(), nb::arg("floor_grid") = nb::none(),
             nb::arg("gravity") = nb::none(), nb::arg("floor_y") = nb::none(),
             nb::arg("floor_restitution") = nb::none(), nb::arg("fixed_dt") = nb::none(),
             "Set any environment field in one write: sun direction/intensity, sky intensity, "
             "floor grid, gravity, floor height/restitution and the physics step")
        .def("get_state", [](const Environment& self) {
                 const auto state = self.get_state();
                 nb::dict out;
                 out["sun_direction"] = state.sun_direction;
                 out["sun_intensity"] = state.sun_intensity;
                 out["sky_intensity"] = state.sky_intensity;
                 out["floor_grid"] = state.floor_grid;
                 out["gravity"] = state.gravity;
                 out["floor_y"] = state.floor_y;
                 out["floor_restitution"] = state.floor_restitution;
                 out["fixed_dt"] = state.fixed_dt;
                 return out;
             },
             "Every environment field from one snapshot");

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

    // Write Python diagnostics to the engine logger.
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
    // SDL and scoped Web V8 events share one owned native queue.
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
                "y (mouse)")
        .def_ro("viewport_x", &Corona::Systems::UI::InputEvent::viewport_x)
        .def_ro("viewport_y", &Corona::Systems::UI::InputEvent::viewport_y)
        .def_ro("viewport_width", &Corona::Systems::UI::InputEvent::viewport_width)
        .def_ro("viewport_height", &Corona::Systems::UI::InputEvent::viewport_height);

    m.def("drain_input_events", []() -> std::vector<Corona::Systems::UI::InputEvent> {
        return Corona::Systems::UI::drain_input_events();
    }, "Drain current SDL and scoped Web input events");

    m.def("python_runtime_phase", [](const std::string& phase) {
        if (auto* coordinator = Corona::Script::Python::active_python_runtime_coordinator()) {
            coordinator->set_execution_phase(phase);
        }
    }, nb::arg("phase"), "Update the native Python runtime diagnostic phase");

}

}  // namespace EngineScripts
