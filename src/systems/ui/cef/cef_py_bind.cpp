#include "python_service_bridge.h"
#include "script_operation_entry.h"

#include "editor_dispatch.h"
#include "editor_operation_table.h"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <include/cef_task.h>
#include <include/base/cef_callback.h>
#include <wrapper/cef_closure_task.h>
#include <wrapper/cef_helpers.h>

#include <Python.h>

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace EngineScripts {

namespace {

namespace nb = nanobind;
namespace UI = Corona::Systems::UI;

/// Runs an editor operation on the CEF UI (editor state owner) thread.
///
/// `editor_dispatch.h` guarantees that every reader of the native editor scene runs
/// on one thread; the retired JSON manifest used to install these hooks, and with it
/// manifest retired the only remaining caller installs them here.
class EditorOwnerTask final : public CefTask {
   public:
    explicit EditorOwnerTask(std::function<void()> fn) : fn_(std::move(fn)) {}
    void Execute() override {
        if (fn_) fn_();
    }

   private:
    std::function<void()> fn_;
    IMPLEMENT_REFCOUNTING(EditorOwnerTask);
};

void ensure_editor_dispatch_hooks() {
    static std::once_flag once;
    std::call_once(once, [] {
        UI::Detail::EditorDispatchHooks hooks;
        hooks.is_owner_thread = [] { return CefCurrentlyOn(TID_UI); };
        hooks.post_to_owner = [](std::function<void()> task) {
            return CefPostTask(TID_UI, CefRefPtr<CefTask>(new EditorOwnerTask(std::move(task))));
        };
        UI::Detail::set_editor_dispatch_hooks(std::move(hooks));
    });
}

/// A dispatch that never returns leaves the caller blocked forever; the bound is the
/// same one the manifest used, so a busy editor still answers with a timeout.
constexpr std::chrono::milliseconds kEditorDispatchTimeout{5000};

/// Converts one Python value into the JSON shape the shared scene helpers take.
///
/// This is not an extra hop invented for Python: the Web adapter builds the very same
/// `nlohmann::json` before calling the same helper. `create_native_editor_actor`,
/// `set_native_editor_actor_transform` and `set_native_actor_properties` take JSON by
/// design - the CreateActor case in editor_scene_service.cpp says so:
///
///   > Reuse the same native business operation as Python/ScriptRuntime. Its internal
///   > JSON object stays on this owner thread, never serialized or passed to the renderer.
///
/// So a dict has to become one somewhere; doing it here is what keeps the two channel
/// adapters symmetric (V8 reader -> typed -> json on the Web side, dict -> json here).
/// If those shared helpers ever take typed structs, this function goes away with the JSON.
///
/// The script contract speaks JSON-compatible values only; anything else is a caller bug
/// and raises TypeError rather than being silently stringified.
nlohmann::json to_json(nb::handle value) {
    if (value.is_none()) return nullptr;
    if (nb::isinstance<nb::bool_>(value)) return nb::cast<bool>(value);
    if (nb::isinstance<nb::int_>(value)) return nb::cast<std::int64_t>(value);
    if (nb::isinstance<nb::float_>(value)) return nb::cast<double>(value);
    if (nb::isinstance<nb::str>(value)) return nb::cast<std::string>(value);
    if (nb::isinstance<nb::dict>(value)) {
        nlohmann::json result = nlohmann::json::object();
        for (const auto& item : nb::cast<nb::dict>(value)) {
            result[nb::cast<std::string>(nb::str(item.first))] = to_json(item.second);
        }
        return result;
    }
    if (nb::isinstance<nb::list>(value) || nb::isinstance<nb::tuple>(value)) {
        nlohmann::json result = nlohmann::json::array();
        for (const auto& item : value) result.push_back(to_json(item));
        return result;
    }
    throw nb::type_error("script arguments must be JSON-compatible values");
}

/// The Python exception type failures are raised as. Cached because the translator runs
/// on the interpreter thread and must not re-import on every call.
PyObject* editor_error_type() {
    static PyObject* type = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        nb::object module = nb::module_::import_("api.errors");
        nb::object cls = module.attr("EditorApiError");
        type = cls.ptr();
        Py_INCREF(type);  // outlive the imported module handle
    });
    return type;
}

/// Registers the translator once; a failed entry answers EditorApiError(code, message).
void install_exception_translator() {
    static std::once_flag once;
    std::call_once(once, [] {
        nb::register_exception_translator([](const std::exception_ptr& error, void*) {
            try {
                std::rethrow_exception(error);
            } catch (const UI::EditorApiError& api_error) {
                PyObject* type = editor_error_type();
                nb::object instance = nb::steal<nb::object>(
                    PyObject_CallFunction(type, "ss", api_error.code.c_str(), api_error.what()));
                if (!instance) return;
                PyErr_SetObject(type, instance.ptr());
            }
        });
    });
}

/// Dispatches one entry on the editor owner thread and turns its result into a Python value.
///
/// Success answers `result.data`; failure raises EditorApiError with the scene layer's
/// symbolic code. The GIL is released while the operation runs (it can wait on the Python
/// runtime, which may itself be waiting for the owner thread), and the exception is raised
/// only after the release scope ends - never from the owner thread.
nlohmann::json run_script_entry(const char* name, std::function<UI::NativeResult()> call) {
    install_exception_translator();
    ensure_editor_dispatch_hooks();

    const UI::NativeRequest diagnostic{"ScriptRuntime", name, nlohmann::json::array()};
    const auto route = UI::Detail::decide_editor_dispatch_route(
        UI::Detail::on_editor_owner_thread(),
        UI::Detail::editor_thread_waiting_for_python(),
        UI::Detail::editor_owner_thread_seen());

    UI::Detail::EditorDispatchReport report;
    std::optional<UI::NativeResult> result;
    {
        nb::gil_scoped_release release;
        result = UI::Detail::call_editor_dispatch(
            [&call, &diagnostic] {
                UI::NativeApiRequestScope scope(diagnostic);
                auto completed = call();
                scope.report_if_slow(completed);
                return completed;
            },
            route, kEditorDispatchTimeout, &report);
    }

    if (!result) {
        const bool timed_out = report.run == UI::Detail::EditorOwnerRun::Timeout;
        throw UI::EditorApiError(
            "NATIVE_UNAVAILABLE",
            timed_out ? "Script Runtime API timed out waiting for the editor thread"
                      : "Script Runtime API could not be posted to the editor thread");
    }
    if (!result->success) {
        const auto code = result->data.is_object() && result->data.contains("code")
                              ? result->data.at("code").get<std::string>()
                              : std::string{"NATIVE_FAILURE"};
        throw UI::EditorApiError(code, result->error);
    }
    return result->data;
}

}  // namespace

void BindCef(nanobind::module_& m) {
    m.def("register_python_script_service_dispatcher", [](nb::object dispatcher) {
        Corona::Systems::UI::register_python_script_service_dispatcher(dispatcher.ptr());
    }, nb::arg("dispatcher"));

    m.def("unregister_python_script_service_dispatcher", []() {
        Corona::Systems::UI::unregister_python_script_service_dispatcher();
    });

    // The ScriptRuntime contract: one typed binding per table row, installed under the same
    // namespace and name the Web surface publishes (snake_case here, camelCase there).
    std::map<std::string_view, nb::module_> submodules = {
        {"scene", m.def_submodule("scene")},
        {"audio", m.def_submodule("audio")},
        {"viewport", m.def_submodule("viewport")},
    };

    const auto bind_script = [&submodules](const char* group, const char* name, auto fn,
                                           auto&&... args) {
        const auto* spec = Corona::Systems::UI::find_editor_operation(group, name);
        if (spec == nullptr || !spec->script) {
            throw std::logic_error(std::string("script binding without a table row: ") + group + "." + name);
        }
        submodules.at(group).def(name, std::move(fn), std::forward<decltype(args)>(args)...);
    };

    bind_script("scene", "read_active", [] {
        return run_script_entry("read_active", [] { return UI::script_entry_read_active(); });
    });
    bind_script("scene", "read_environment", [](std::string scene_name, nb::dict scope) {
        auto scope_json = to_json(scope);
        return run_script_entry("read_environment", [scene_name, scope_json] {
            return UI::script_entry_read_environment(scene_name, scope_json);
        });
    }, nb::arg("scene"), nb::arg("scope") = nb::dict());
    bind_script("scene", "read_actor", [](nb::dict request) {
        auto request_json = to_json(request);
        return run_script_entry("read_actor", [request_json] {
            return UI::script_entry_read_actor(request_json);
        });
    }, nb::arg("request"));
    bind_script("scene", "read_transforms", [](nb::dict request) {
        auto request_json = to_json(request);
        return run_script_entry("read_transforms", [request_json] {
            return UI::script_entry_read_transforms(request_json);
        });
    }, nb::arg("request"));
    bind_script("scene", "read_archive_scene", [](nb::dict request) {
        auto request_json = to_json(request);
        return run_script_entry("read_archive_scene", [request_json] {
            return UI::script_entry_read_archive_scene(request_json);
        });
    }, nb::arg("request"));
    bind_script("scene", "read_scene", [](nb::dict request) {
        auto request_json = to_json(request);
        return run_script_entry("read_scene", [request_json] {
            return UI::script_entry_read_scene(request_json);
        });
    }, nb::arg("request"));
    bind_script("scene", "set_actor_transform", [](std::string scene, std::string actor, nb::dict patch) {
        auto patch_json = to_json(patch);
        return run_script_entry("set_actor_transform", [scene, actor, patch_json] {
            return UI::script_entry_set_actor_transform(scene, actor, patch_json);
        });
    }, nb::arg("scene"), nb::arg("actor"), nb::arg("patch"));
    bind_script("scene", "update_actor", [](std::string scene, std::string actor, nb::dict patch) {
        auto patch_json = to_json(patch);
        return run_script_entry("update_actor", [scene, actor, patch_json] {
            return UI::script_entry_update_actor(scene, actor, patch_json);
        });
    }, nb::arg("scene"), nb::arg("actor"), nb::arg("patch"));
    bind_script("scene", "set_environment", [](std::string scene, nb::dict patch) {
        auto patch_json = to_json(patch);
        return run_script_entry("set_environment", [scene, patch_json] {
            return UI::script_entry_set_environment(scene, patch_json);
        });
    }, nb::arg("scene"), nb::arg("patch"));
    bind_script("scene", "set_actor_animation",
                [](std::string scene, std::string actor, std::string animation, double blend_seconds) {
        return run_script_entry("set_actor_animation", [scene, actor, animation, blend_seconds] {
            return UI::script_entry_set_actor_animation(scene, actor, animation, blend_seconds);
        });
    }, nb::arg("scene"), nb::arg("actor"), nb::arg("animation"), nb::arg("blend_seconds") = 0.2);
    bind_script("scene", "pause_actor_animation", [](std::string scene, std::string actor) {
        return run_script_entry("pause_actor_animation", [scene, actor] {
            return UI::script_entry_pause_actor_animation(scene, actor);
        });
    }, nb::arg("scene"), nb::arg("actor"));
    bind_script("scene", "resume_actor_animation", [](std::string scene, std::string actor) {
        return run_script_entry("resume_actor_animation", [scene, actor] {
            return UI::script_entry_resume_actor_animation(scene, actor);
        });
    }, nb::arg("scene"), nb::arg("actor"));
    bind_script("scene", "set_actor_physics", [](std::string scene, std::string actor, nb::dict patch) {
        auto patch_json = to_json(patch);
        return run_script_entry("set_actor_physics", [scene, actor, patch_json] {
            return UI::script_entry_set_actor_physics(scene, actor, patch_json);
        });
    }, nb::arg("scene"), nb::arg("actor"), nb::arg("patch"));
    bind_script("scene", "create_actor", [](std::string scene, std::string path, nb::dict options) {
        auto options_json = to_json(options);
        return run_script_entry("create_actor", [scene, path, options_json] {
            return UI::script_entry_create_actor(scene, path, options_json);
        });
    }, nb::arg("scene"), nb::arg("path"), nb::arg("options"));
    bind_script("scene", "delete_actor", [](std::string scene, std::string actor, nb::dict scope) {
        auto scope_json = to_json(scope);
        return run_script_entry("delete_actor", [scene, actor, scope_json] {
            return UI::script_entry_delete_actor(scene, actor, scope_json);
        });
    }, nb::arg("scene"), nb::arg("actor"), nb::arg("scope") = nb::dict());

    bind_script("audio", "play", [](nb::dict request) {
        auto request_json = to_json(request);
        return run_script_entry("play", [request_json] {
            return UI::script_entry_play_actor_audio(request_json);
        });
    }, nb::arg("request"));
    bind_script("audio", "stop", [](nb::dict request) {
        auto request_json = to_json(request);
        return run_script_entry("stop", [request_json] {
            return UI::script_entry_stop_actor_audio(request_json);
        });
    }, nb::arg("request"));

    bind_script("viewport", "set_camera_pose", [](std::string scene, std::string camera, nb::dict patch) {
        auto patch_json = to_json(patch);
        return run_script_entry("set_camera_pose", [scene, camera, patch_json] {
            return UI::script_entry_set_camera_pose(scene, camera, patch_json);
        });
    }, nb::arg("scene"), nb::arg("camera"), nb::arg("patch"));
}

}  // namespace EngineScripts
