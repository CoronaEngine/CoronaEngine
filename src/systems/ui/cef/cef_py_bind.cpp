#pragma once

#include "cef_editor_api.h"
#include "script_runtime_api.h"

#include "editor_dispatch.h"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <include/cef_task.h>
#include <include/base/cef_callback.h>
#include <wrapper/cef_closure_task.h>
#include <wrapper/cef_helpers.h>

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace EngineScripts {

namespace {

namespace UI = Corona::Systems::UI;

/// Runs an editor operation on the CEF UI (editor state owner) thread.
///
/// `editor_dispatch.h` guarantees that every reader of the native editor scene runs
/// on one thread; `EditorApiRegistry` used to install these hooks, and with the JSON
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

using ScriptOperation = UI::NativeResult (*)(const UI::NativeRequest&, const UI::NativeContext&);

/// Runs one named ScriptRuntime operation and returns the JSON envelope the Python
/// adapter already expects.
///
/// The GIL is released while the operation is dispatched: an operation can wait on
/// the Python runtime (the scene snapshot comes back through the dispatcher), and the
/// CEF UI thread that runs it can itself be waiting for Python.  Holding the GIL here
/// turned that handshake into a 5 s timeout in the field.
std::string invoke_script_operation(ScriptOperation operation, const std::string& args_json) {
    auto args = nlohmann::json::parse(args_json.empty() ? "[]" : args_json, nullptr, false);
    if (args.is_discarded() || !args.is_array()) {
        return nlohmann::json{{"success", false},
                              {"error", "Invalid Script Runtime API args JSON"},
                              {"error_code", 400}}
            .dump();
    }

    ensure_editor_dispatch_hooks();
    UI::NativeRequest request;
    request.args = std::move(args);
    UI::NativeContext context;
    context.caller = UI::EditorApiCaller::ScriptRuntime;

    const bool on_owner_thread = UI::Detail::on_editor_owner_thread();
    const auto route = UI::Detail::decide_editor_dispatch_route(
        on_owner_thread,
        UI::Detail::editor_thread_waiting_for_python(),
        UI::Detail::editor_owner_thread_seen());

    UI::Detail::EditorDispatchReport report;
    std::optional<UI::NativeResult> result;
    {
        nanobind::gil_scoped_release release;
        result = UI::Detail::call_editor_dispatch(
            [operation, request, context] { return operation(request, context); },
            route,
            kEditorDispatchTimeout,
            &report);
    }

    if (!result) {
        const bool timed_out = report.run == UI::Detail::EditorOwnerRun::Timeout;
        return nlohmann::json{{"success", false},
                              {"error", timed_out ? "Script Runtime API timed out waiting for the editor thread"
                                                  : "Script Runtime API could not be posted to the editor thread"},
                              {"error_code", timed_out ? 504 : 503}}
            .dump();
    }

    nlohmann::json response = {
        {"success", result->success},
        {"route", "script-runtime"},
    };
    if (result->success) {
        response["data"] = result->data;
    } else {
        response["error"] = result->error;
        response["error_code"] = result->error_code;
    }
    return response.dump();
}

}  // namespace

void BindCef(nanobind::module_& m) {
    namespace nb = nanobind;
    m.def("register_python_script_service_dispatcher", [](nb::object dispatcher) {
        Corona::Systems::UI::register_python_script_service_dispatcher(dispatcher.ptr());
    }, nb::arg("dispatcher"));

    m.def("unregister_python_script_service_dispatcher", []() {
        Corona::Systems::UI::unregister_python_script_service_dispatcher();
    });

    m.def("register_python_script_callback", [](const std::string& event_name, nb::object callback) {
        return Corona::Systems::UI::register_python_script_callback_callable(event_name, callback.ptr());
    }, nb::arg("event_name"), nb::arg("callback"));
    m.def("unregister_python_script_callback", [](std::uint64_t token) {
        return Corona::Systems::UI::EditorApiCallbackRegistry::instance().unregister(token);
    }, nb::arg("token"));

    m.def("emit_editor_api_event", [](const std::string& event_name, const std::string& payload_json) {
        auto payload = nlohmann::json::parse(payload_json.empty() ? "{}" : payload_json, nullptr, false);
        if (payload.is_discarded()) {
            return static_cast<std::size_t>(0);
        }
        return Corona::Systems::UI::emit_editor_api_event(event_name, payload);
    }, nb::arg("event_name"), nb::arg("payload_json"));

    // The ScriptRuntime editor contract. One named binding per operation: the Python
    // adapter below the interpreter decides which one to call, so C++ carries no name
    // table, no argument schema and no caller mask.
    const auto bind_script_operation = [&m](const char* name, ScriptOperation operation) {
        m.def(name, [operation](const std::string& args_json) {
            return invoke_script_operation(operation, args_json);
        }, nb::arg("args_json"));
    };
    bind_script_operation("script_scene_get_environment", Corona::Systems::UI::script_scene_get_environment);
    bind_script_operation("script_scene_read_active_scene", Corona::Systems::UI::script_scene_read_active_scene);
    bind_script_operation("script_scene_read_actor", Corona::Systems::UI::script_scene_read_actor);
    bind_script_operation("script_scene_read_transforms", Corona::Systems::UI::script_scene_read_transforms);
    bind_script_operation("script_scene_read_archive_scene", Corona::Systems::UI::script_scene_read_archive_scene);
    bind_script_operation("script_scene_get_scene_snapshot", Corona::Systems::UI::script_scene_get_scene_snapshot);
    bind_script_operation("script_scene_set_actor_transform", Corona::Systems::UI::script_scene_set_actor_transform);
    bind_script_operation("script_scene_set_environment", Corona::Systems::UI::script_scene_set_environment);
    bind_script_operation("script_scene_set_actor_state", Corona::Systems::UI::script_scene_set_actor_state);
    bind_script_operation("script_scene_set_actor_animation", Corona::Systems::UI::script_scene_set_actor_animation);
    bind_script_operation("script_scene_pause_actor_animation", Corona::Systems::UI::script_scene_pause_actor_animation);
    bind_script_operation("script_scene_resume_actor_animation", Corona::Systems::UI::script_scene_resume_actor_animation);
    bind_script_operation("script_scene_set_actor_physics", Corona::Systems::UI::script_scene_set_actor_physics);
    bind_script_operation("script_scene_set_camera_pose", Corona::Systems::UI::script_scene_set_camera_pose);
    bind_script_operation("script_scene_create_actor", Corona::Systems::UI::script_scene_create_actor);
    bind_script_operation("script_scene_remove_actor", Corona::Systems::UI::script_scene_remove_actor);

    // Retired with the JSON manifest; kept until the table, the registry and the
    // Python adapter that validates against them are deleted in the next step.
    m.def("_invoke_cpp_editor_api", [](const std::string& api_name, const std::string& args_json) {
        auto args = nlohmann::json::parse(args_json.empty() ? "[]" : args_json, nullptr, false);
        if (args.is_discarded()) {
            return nlohmann::json{{"success", false}, {"error", "Invalid Editor API args JSON"}}.dump();
        }
        Corona::Systems::UI::NativeResult result;
        {
            nanobind::gil_scoped_release release;
            result = Corona::Systems::UI::EditorApiRegistry::instance().invoke(
                {api_name, args, Corona::Systems::UI::EditorApiCaller::PythonScript}, {});
        }
        nlohmann::json response = {{"success", result.success}, {"route", result.route}};
        if (result.success) {
            response["data"] = result.data;
        } else {
            response["error"] = result.error;
            response["error_code"] = result.error_code;
        }
        return response.dump();
    }, nb::arg("api_name"), nb::arg("args_json"));

    m.def("_invoke_cpp_script_api", [](const std::string& api_name, const std::string& args_json) {
        auto args = nlohmann::json::parse(args_json.empty() ? "[]" : args_json, nullptr, false);
        if (args.is_discarded()) {
            return nlohmann::json{{"success", false}, {"error", "Invalid Script Runtime API args JSON"}}.dump();
        }
        Corona::Systems::UI::NativeResult result;
        {
            nanobind::gil_scoped_release release;
            result = Corona::Systems::UI::EditorApiRegistry::instance().invoke(
                {api_name, args, Corona::Systems::UI::EditorApiCaller::ScriptRuntime}, {});
        }
        nlohmann::json response = {{"success", result.success}, {"route", result.route}};
        if (result.success) {
            response["data"] = result.data;
        } else {
            response["error"] = result.error;
            response["error_code"] = result.error_code;
        }
        return response.dump();
    }, nb::arg("api_name"), nb::arg("args_json"));
}

}  // namespace EngineScripts
