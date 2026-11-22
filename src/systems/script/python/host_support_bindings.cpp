#include <horizon/core/logging.h>
#include <corona/systems/script/engine_scripts.h>
#include <corona/systems/script/python_runtime_coordinator.h>
#include <corona/systems/script/script_input.h>
#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <string>
#include <vector>

namespace nb = nanobind;

namespace EngineScripts {

/// The entries the embedded Python host needs from the running engine.
///
/// These are not the editor transport (`BindCef` owns the service dispatcher and the project
/// session) and not a script API - the object model that used to be exported under
/// `CoronaEngine.script` was deleted with the script runtime it served. They are what
/// `embedded_python` itself calls: engine logging, the runtime diagnostic phase, and the input
/// queue it drains once per frame. Their payloads are top-level names too - a type lives with
/// the function that hands it out - which is why `InputEvent` sits here.
void BindHostSupport(nanobind::module_& m) {
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

    // SDL and scoped Web V8 events share one owned native queue.
    //
    // The fields are the names the Web request used, not positional slots: `kind` is the
    // wire `type` ("keyDown"/"keyUp"/"mouse"), and the identity fields say which archive,
    // scene and camera the accepted request belonged to. They are empty for SDL input.
    using InputEvent = Corona::Systems::UI::InputEvent;
    nb::class_<InputEvent>(m, "InputEvent")
        .def_ro("kind", &InputEvent::kind, "\"keyDown\", \"keyUp\" or \"mouse\" - the Web spelling")
        .def_ro("archive_session", &InputEvent::archive_session,
                "Archive of the accepted Web request; empty for SDL input")
        .def_ro("scene_id", &InputEvent::scene_id,
                "Scene of the accepted Web request; empty for SDL input")
        .def_ro("camera_id", &InputEvent::camera_id,
                "Camera of the accepted Web request; empty for SDL input")
        .def_ro("code", &InputEvent::code, "Key code, e.g. \"KeyJ\"; empty for mouse events")
        .def_ro("key", &InputEvent::key, "Key as the layout displays it; empty for mouse events")
        .def_ro("modifiers", &InputEvent::modifiers,
                "Modifiers string on keyDown; empty on keyUp and for mouse events")
        .def_ro("event", &InputEvent::event,
                "Mouse event: move/mousedown/mouseup/click/wheel; empty for key events")
        .def_ro("button", &InputEvent::button,
                "Mouse button for mousedown/mouseup/click; may be empty")
        .def_ro("x", &InputEvent::x, "Mouse position in CSS pixels")
        .def_ro("y", &InputEvent::y, "Mouse position in CSS pixels")
        .def_ro("viewport_x", &InputEvent::viewport_x, "Viewport rectangle the event was scoped to")
        .def_ro("viewport_y", &InputEvent::viewport_y)
        .def_ro("viewport_width", &InputEvent::viewport_width)
        .def_ro("viewport_height", &InputEvent::viewport_height);

    m.def("drain_input_events", []() -> std::vector<InputEvent> {
        return Corona::Systems::UI::drain_input_events();
    }, "Drain current SDL and scoped Web input events");

    m.def("python_runtime_phase", [](const std::string& phase) {
        if (auto* coordinator = Corona::Script::Python::active_python_runtime_coordinator()) {
            coordinator->set_execution_phase(phase);
        }
    }, nb::arg("phase"), "Update the native Python runtime diagnostic phase");

    // The host loop asks this once per iteration; with no coordinator installed there is no
    // loop to stop, so it reports "keep going".
    m.def("should_stop", [] {
        const auto* coordinator = Corona::Script::Python::active_python_runtime_coordinator();
        return coordinator && coordinator->stop_requested();
    }, "True once the host has asked the runtime to stop");
}

}  // namespace EngineScripts
