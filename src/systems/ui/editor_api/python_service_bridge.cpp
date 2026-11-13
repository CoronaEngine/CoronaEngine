#include "python_service_bridge.h"

#include "editor_dispatch.h"

#include <corona/systems/script/python_runtime_coordinator.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>

namespace Corona::Systems::UI {
namespace {

std::atomic<bool> g_python_script_service_dispatcher_registered{false};

std::string python_script_error_message(const nlohmann::json& payload) {
    if (auto it = payload.find("error"); it != payload.end() && it->is_string()) {
        return it->get<std::string>();
    }
    if (auto it = payload.find("message"); it != payload.end() && it->is_string()) {
        return it->get<std::string>();
    }
    return "Python script service returned an error";
}

}  // namespace

void register_python_script_service_dispatcher(PyObject* dispatcher) {
    g_python_script_service_dispatcher_registered.store(
        PyGILState_Check() && dispatcher && PyCallable_Check(dispatcher),
        std::memory_order_release);
}

void unregister_python_script_service_dispatcher() {
    g_python_script_service_dispatcher_registered.store(false, std::memory_order_release);
}

bool python_script_service_dispatcher_registered() {
    return g_python_script_service_dispatcher_registered.load(std::memory_order_acquire);
}

NativeResult invoke_python_script_service(const NativeRequest& request, const char* route) {
    const std::string route_name = route && *route ? route : "python-script";
    auto* coordinator = Script::Python::active_python_runtime_coordinator();
    if (!coordinator) {
        return native_failure("Python script runtime is not initialized", 503, route_name);
    }

    Script::Python::PythonRuntimeRequest runtime_request;
    runtime_request.kind = Script::Python::PythonRuntimeRequestKind::ServiceCall;
    runtime_request.source = route_name;
    runtime_request.module = request.module;
    runtime_request.function = request.function;
    runtime_request.service_args = std::make_shared<const nlohmann::json>(
        request.args.is_null() ? nlohmann::json::array() : request.args);
    // 从这里到 submit_and_wait 返回为止，owner 线程不再碰编辑器状态：于是 Python 消费
    // 线程此刻若回调编辑器 API，可以就地执行（见 editor_dispatch.h 的三段交接协议）。
    Detail::EditorWaitingForPythonScope waiting_for_python;
    const auto response = coordinator->submit_and_wait(std::move(runtime_request));
    if (response.status != Script::Python::PythonRuntimeResponseStatus::Success) {
        int error_code = 500;
        if (response.status == Script::Python::PythonRuntimeResponseStatus::Timeout) {
            error_code = 504;
        } else if (response.status == Script::Python::PythonRuntimeResponseStatus::QueueFull) {
            error_code = 429;
        } else if (response.status == Script::Python::PythonRuntimeResponseStatus::RuntimeStopping) {
            error_code = 503;
        }
        return native_failure(
            response.error.empty() ? "Python script function call failed" : response.error,
            error_code, route_name);
    }

    if (!response.service_result) {
        return native_failure("Python script service returned no structured result", 500, route_name);
    }
    const auto& parsed = *response.service_result;
    if (parsed.is_object() && parsed.value("success", true) == false) {
        return native_failure(python_script_error_message(parsed), 500, route_name);
    }
    if (parsed.is_object()) {
        if (auto it = parsed.find("data"); it != parsed.end()) {
            return native_success(*it, route_name);
        }
    }
    return native_success(parsed, route_name);
}

}  // namespace Corona::Systems::UI
