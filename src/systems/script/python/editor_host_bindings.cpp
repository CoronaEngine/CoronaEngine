#include <corona/engine/engine_runtime_api.h>
#include <corona/systems/script/camera_follow_controller.h>
#include <nanobind/nanobind.h>

#include <cstdint>

#include <SDL3/SDL.h>

namespace EngineScripts {

void BindEditorHost(nanobind::module_& m) {
    namespace nb = nanobind;

    m.def("request_engine_exit", []() {
        SDL_Event quit_event;
        SDL_zero(quit_event);
        quit_event.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit_event);
    }, "Request graceful engine shutdown.");

    m.def("camera_follow_set_input_enabled", [](bool enabled) {
        Corona::API::set_editor_camera_input_enabled(enabled);
    }, nb::arg("enabled"),
       "Enable or disable editor camera-follow input.");
}

}  // namespace EngineScripts
