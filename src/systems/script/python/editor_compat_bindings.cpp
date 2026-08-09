#include <corona/engine/engine_runtime_api.h>
#include <corona/systems/script/camera_follow_controller.h>
#include <nanobind/nanobind.h>

#include <cstdint>
#include <string>

#include <SDL3/SDL.h>

// These bindings are retained only for explicit compatibility adapters and
// older Script Runtime hosts. The stable editor surface is defined by
// cef_editor_api.cpp and is reached through the aggregate editor API.
namespace Corona::Systems::UI {
std::string create_editor_actor_from_python(const std::string& scene_name,
                                            const std::string& asset_path,
                                            const std::string& actor_type,
                                            const std::string& actor_data_json);
std::string remove_editor_actor_from_python(const std::string& scene_name,
                                            const std::string& actor_name);
std::string get_editor_actor_bounds_from_python(const std::string& scene_name,
                                                const std::string& actor_name);
std::string get_editor_actor_geometry_status_from_python(const std::string& scene_name,
                                                         const std::string& actor_name);
std::string get_editor_scene_bounds_from_python(const std::string& scene_name);
std::string get_editor_scene_snapshot_from_python(const std::string& scene_name);
std::string set_editor_actor_transform_from_python(const std::string& scene_name,
                                                   const std::string& actor_name,
                                                   const std::string& transform_json);
std::string set_editor_camera_transform_from_python(const std::string& scene_name,
                                                    const std::string& camera_name,
                                                    const std::string& camera_data_json);
std::string capture_editor_camera_view_from_python(const std::string& scene_name,
                                                   const std::string& camera_name,
                                                   const std::string& camera_data_json,
                                                   const std::string& output_path);
}  // namespace Corona::Systems::UI

namespace EngineScripts {

using namespace Corona::API;

void BindEditorCompatibility(nanobind::module_& m) {
    namespace nb = nanobind;

    // Vision and media bindings remain available for explicitly registered
    // legacy editor adapters. New editor code must use the manifest-backed
    // sceneTools/viewport contracts instead of calling these raw entries.
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
    m.def("load_vision_scene_from_json", &load_vision_scene_from_json,
          nb::arg("json_text"), nb::arg("base_dir"), nb::arg("scene_key"),
          nb::arg("external_live") = false,
          "Load an external Vision scene from in-memory JSON. base_dir resolves "
          "relative resources and scene_key identifies the runtime cache entry.");

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
          "Stop playing an imported audio resource.");

    // Legacy editor host lifecycle entry. New code should use the host
    // lifecycle service instead of exposing SDL directly to Python.
    m.def("request_engine_exit", []() {
        SDL_Event quit_event;
        SDL_zero(quit_event);
        quit_event.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit_event);
    }, "Request graceful engine shutdown. "
       "Pushes an SDL_QUIT event, same as clicking the window close button.");

    // Camera-follow fast path used by the legacy compatibility adapter. The
    // editor aggregate handler owns the persisted camera-lock state.
    m.def("camera_follow_set_target", [](std::uintptr_t actor_handle,
                                         std::uintptr_t camera_handle,
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
        Corona::API::set_editor_camera_input_enabled(enabled);
    }, nb::arg("enabled"),
       "Enable or disable editor camera-follow keyboard/mouse input");

    m.def("camera_follow_inject_rmb", [](bool down, int x, int y) {
        Corona::Systems::CameraFollowController::instance().inject_rmb(down, x, y);
    }, nb::arg("down"), nb::arg("screen_x"), nb::arg("screen_y"),
       "Inject right mouse button state for camera orbit");

    m.def("create_editor_actor",
          [](const std::string& scene_name,
             const std::string& asset_path,
             const std::string& actor_type,
             const std::string& actor_data_json) {
              return Corona::Systems::UI::create_editor_actor_from_python(
                  scene_name, asset_path, actor_type, actor_data_json);
          },
          nb::arg("scene_name"),
          nb::arg("asset_path"),
          nb::arg("actor_type") = "model",
          nb::arg("actor_data_json") = "{}",
          "Create an editor actor in the native C++ scene and return its actor JSON.");

    m.def("remove_editor_actor",
          [](const std::string& scene_name,
             const std::string& actor_name) {
              return Corona::Systems::UI::remove_editor_actor_from_python(
                  scene_name, actor_name);
          },
          nb::arg("scene_name"),
          nb::arg("actor_name"),
          "Remove an editor actor from the native C++ scene and persist the scene.");

    m.def("get_editor_actor_bounds",
          [](const std::string& scene_name,
             const std::string& actor_name) {
              return Corona::Systems::UI::get_editor_actor_bounds_from_python(
                  scene_name, actor_name);
          },
          nb::arg("scene_name"),
          nb::arg("actor_name"),
          "Return native editor actor bounds as JSON.");

    m.def("get_editor_actor_geometry_status",
          [](const std::string& scene_name,
             const std::string& actor_name) {
              return Corona::Systems::UI::get_editor_actor_geometry_status_from_python(
                  scene_name, actor_name);
          },
          nb::arg("scene_name"),
          nb::arg("actor_name"),
          "Return native editor actor async geometry/GPU build status as JSON.");

    m.def("get_editor_scene_bounds",
          [](const std::string& scene_name) {
              return Corona::Systems::UI::get_editor_scene_bounds_from_python(scene_name);
          },
          nb::arg("scene_name"),
          "Return native editor scene aggregate actor bounds as JSON.");

    m.def("get_editor_scene_snapshot",
          [](const std::string& scene_name) {
              return Corona::Systems::UI::get_editor_scene_snapshot_from_python(scene_name);
          },
          nb::arg("scene_name"),
          "Return native editor scene actors, transforms, and bounds as JSON.");

    m.def("set_editor_actor_transform",
          [](const std::string& scene_name,
             const std::string& actor_name,
             const std::string& transform_json) {
              return Corona::Systems::UI::set_editor_actor_transform_from_python(
                  scene_name, actor_name, transform_json);
          },
          nb::arg("scene_name"),
          nb::arg("actor_name"),
          nb::arg("transform_json"),
          "Set a native editor actor transform, persist it, and return actor JSON.");

    m.def("set_editor_camera_transform",
          [](const std::string& scene_name,
             const std::string& camera_name,
             const std::string& camera_data_json) {
              return Corona::Systems::UI::set_editor_camera_transform_from_python(
                  scene_name, camera_name, camera_data_json);
          },
          nb::arg("scene_name"),
          nb::arg("camera_name") = "",
          nb::arg("camera_data_json") = "{}",
          "Set the active native editor camera transform; pass persist=false for runtime preview updates.");

    m.def("capture_editor_camera_view",
          [](const std::string& scene_name,
             const std::string& camera_name,
             const std::string& camera_data_json,
             const std::string& output_path) {
              return Corona::Systems::UI::capture_editor_camera_view_from_python(
                  scene_name, camera_name, camera_data_json, output_path);
          },
          nb::arg("scene_name"),
          nb::arg("camera_name"),
          nb::arg("camera_data_json"),
          nb::arg("output_path"),
          "Set an offscreen native editor camera and save a screenshot, returning JSON.");
}

}  // namespace EngineScripts
