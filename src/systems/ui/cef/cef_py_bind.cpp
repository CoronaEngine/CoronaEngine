#pragma once

#include "cef_editor_api.h"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <string>

namespace EngineScripts {

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

    m.def("emit_editor_api_event", [](const std::string& event_name, const std::string& payload_json) {
        auto payload = nlohmann::json::parse(payload_json.empty() ? "{}" : payload_json, nullptr, false);
        if (payload.is_discarded()) {
            return static_cast<std::size_t>(0);
        }
        return Corona::Systems::UI::emit_editor_api_event(event_name, payload);
    }, nb::arg("event_name"), nb::arg("payload_json"));

    m.def("_invoke_cpp_editor_api", [](const std::string& api_name, const std::string& args_json) {
        auto args = nlohmann::json::parse(args_json.empty() ? "[]" : args_json, nullptr, false);
        if (args.is_discarded()) {
            nlohmann::json response = {
                {"success", false},
                {"error", "Invalid Editor API args JSON"},
            };
            return response.dump();
        }

        Corona::Systems::UI::register_builtin_native_api_handlers();
        Corona::Systems::UI::PythonScriptApiClientEndpoint endpoint;
        Corona::Systems::UI::NativeContext context;
        auto result = endpoint.invoke(api_name, args, context);
        nlohmann::json response = {
            {"success", result.success},
            {"route", result.route},
        };
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
            nlohmann::json response = {
                {"success", false},
                {"error", "Invalid Script Runtime API args JSON"},
            };
            return response.dump();
        }

        Corona::Systems::UI::register_builtin_native_api_handlers();
        Corona::Systems::UI::ScriptRuntimeApiClientEndpoint endpoint;
        Corona::Systems::UI::NativeContext context;
        auto result = endpoint.invoke(api_name, args, context);
        nlohmann::json response = {
            {"success", result.success},
            {"route", result.route},
        };
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
