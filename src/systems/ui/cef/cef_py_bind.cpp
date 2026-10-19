#pragma once

#include "cef_editor_api.h"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <string>

namespace EngineScripts {

namespace {

/// 调用编辑器 API 时必须**放开 GIL**。
///
/// `_invoke_cpp_editor_api` 现在会把 handler 派发到 CEF UI（编辑器状态 owner）线程并等它
/// 执行（见 `editor_api/editor_dispatch.h`）。而 CEF UI 线程自己也会同步等 Python
/// （`invoke_python_script_service` → `submit_and_wait`，例如 `ScratchTool.mouse_event`）。
/// 如果这里握着 GIL 等 CEF 线程，Python 消费线程就拿不到 GIL 去答复 CEF 线程，形成
/// "CEF 等 Python、Python 等 CEF"的环路。实测（LANChat 后台轮询每 ~2.5 s 一次）：
/// 每次轮询撞上鼠标事件的 Python 往返就握手成 5 s 超时，期间每帧鼠标事件都在 2 s 超时里
/// 冻住 UI。放开 GIL 之后这段等待不再拖住整个 Python 运行时。
///
/// handler 全是 C++，Python 相关工作本来就走 coordinator 队列（不在这里直接触 Python
/// C-API），所以释放 GIL 不会让任何 handler 摸到没有 GIL 的 Python 对象。
std::string invoke_editor_endpoint_off_gil(const std::string& api_name,
                                          const nlohmann::json& args,
                                          Corona::Systems::UI::EditorApiCaller caller) {
    Corona::Systems::UI::register_builtin_native_api_handlers();
    Corona::Systems::UI::NativeContext context;
    Corona::Systems::UI::NativeResult result;
    {
        nanobind::gil_scoped_release release;
        result = Corona::Systems::UI::EditorApiRegistry::instance().invoke(
            {api_name, args, caller}, context);
    }
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
        return invoke_editor_endpoint_off_gil(api_name, args,
                                              Corona::Systems::UI::EditorApiCaller::PythonScript);
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
        return invoke_editor_endpoint_off_gil(api_name, args,
                                              Corona::Systems::UI::EditorApiCaller::ScriptRuntime);
    }, nb::arg("api_name"), nb::arg("args_json"));
}

}  // namespace EngineScripts
