#include <corona/engine/engine_runtime_api.h>
#include <corona/systems/script/camera_follow_controller.h>
#include <nanobind/nanobind.h>

namespace EngineScripts {

void BindEditorHost(nanobind::module_& m) {
    namespace nb = nanobind;

    m.def("camera_follow_set_input_enabled", [](bool enabled) {
        Corona::API::set_editor_camera_input_enabled(enabled);
    }, nb::arg("enabled"),
       "Enable or disable editor camera-follow input.");
}

}  // namespace EngineScripts
