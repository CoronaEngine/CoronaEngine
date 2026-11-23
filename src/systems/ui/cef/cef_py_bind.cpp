#include "python_service_bridge.h"

#include "editor_paths.h"
#include "native_editor_state.h"

#include <corona/systems/script/engine_scripts.h>
#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <string>

namespace EngineScripts {

namespace UI = Corona::Systems::UI;

/// The editor-facing half of the embedded module.
///
/// The ScriptRuntime contract (18 scene/audio/viewport bindings) was removed with the script
/// runtime and will be redesigned; what is left is the transport the Python host needs: the
/// service dispatcher registration, and the one piece of editor state the host itself reads.
void BindCef(nanobind::module_& m) {
    m.def("register_python_script_service_dispatcher", [] {
        UI::register_python_script_service_dispatcher();
    });
    m.def("unregister_python_script_service_dispatcher", [] {
        UI::unregister_python_script_service_dispatcher();
    });
    // Host support, not a script API: `runtime.project_context` reads the project this
    // editor owns to resolve the active project path.
    m.def("active_project_path", [] {
        const auto* scene = UI::native_editor_state().scene.get();
        return scene ? UI::path_to_utf8(scene->project_root) : std::string{};
    }, "Project root of the scene the editor owns, or an empty string");
    // Host support: the session identity a Python service compares a request against.
    // `archive_session` is minted by the Python parser and owned here afterwards, so this
    // is the only authoritative copy; the path alone cannot tell a reopened project apart.
    m.def("active_project_session", [] {
        nanobind::dict session;
        const auto* scene = UI::native_editor_state().scene.get();
        if (!scene) return session;
        session["project_path"] = UI::path_to_utf8(scene->project_root);
        session["archive_session"] = scene->archive_session;
        session["read_only"] = scene->read_only;
        return session;
    }, "Session identity of the project the editor owns, or an empty dict");
    // The service queue the Python host loop pulls from. It lives in the script module
    // (`service_queue_bindings.cpp`) because the JSON<->Python conversion helpers do; this
    // group only installs it, so the module keeps calling two groups.
    BindCefServiceQueue(m);
}

}  // namespace EngineScripts
