#define PY_SSIZE_T_CLEAN
#include <corona/systems/script/engine_scripts.h>
#include <corona/systems/script/python_runtime_coordinator.h>
#include <horizon/core/logging.h>
#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <string>

#include "python_value.h"

namespace nb = nanobind;

namespace EngineScripts {

namespace {

using Corona::Script::Python::PythonRuntimeRequestKind;
using Corona::Script::Python::PythonRuntimeResponse;

/// Only service calls make sense to the host loop. `Callback` and `LifecycleControl` were
/// the C++-driven worker loop's test-only mechanisms; the inverted loop has nothing that
/// could run them, so their names are needed here only to say so in the log.
const char* request_kind_name(PythonRuntimeRequestKind kind) {
    switch (kind) {
        case PythonRuntimeRequestKind::ServiceCall:
            return "ServiceCall";
        case PythonRuntimeRequestKind::Callback:
            return "Callback";
        case PythonRuntimeRequestKind::LifecycleControl:
            return "LifecycleControl";
    }
    return "Unknown";
}

/// Take the pending Python error out of the interpreter as text. Only call this while an
/// error is set - `nanobind::python_error` refuses to be constructed without one.
std::string take_python_error() {
    if (!PyErr_Occurred()) return "python service value conversion failed";
    try {
        throw nb::python_error();
    } catch (const nb::python_error& error) {
        return error.what();
    }
}

}  // namespace

/// The pull side of the inverted host loop.
///
/// `poll_service_requests` used to be the worker loop's job inside C++ (`wait_pop` +
/// `execute_runtime_request`); now the embedded host loop pulls the queued service calls
/// itself and hands each result back through `complete_service_request`. Neither entry is a
/// script API, and the `InputEvent` type discovered next to `drain_input_events` is still the
/// module's only exported type.
///
/// Both entries are consumer-thread-only by construction: `drain_pending` returns nothing on
/// any other thread, and the host loop is the coordinator's bound consumer. A drained request
/// owns a pending ticket, and only `complete_service_request` can settle it - dropping one
/// hangs the C++ caller inside `submit_and_wait` forever.
void BindCefServiceQueue(nanobind::module_& m) {
    m.def("poll_service_requests", [] {
        nb::list requests;
        auto* coordinator = Corona::Script::Python::active_python_runtime_coordinator();
        if (!coordinator) return requests;

        for (auto& request : coordinator->drain_pending()) {
            if (request.kind != PythonRuntimeRequestKind::ServiceCall) {
                // Test-only mechanisms: warn and drop rather than hand the host loop a
                // request it has no way to run.
                PY_LOG_WARNING("dropping a {} python runtime request the host loop cannot run",
                               request_kind_name(request.kind));
                continue;
            }
            nb::dict entry;
            entry["request_id"] = request.request_id;
            entry["service"] = request.module;
            entry["function"] = request.function;
            if (!request.service_args) {
                entry["args"] = nb::none();
                requests.append(entry);
                continue;
            }
            auto* args = Corona::Script::Python::json_to_python(*request.service_args);
            if (!args) {
                // The conversion owns the pending Python error. Settle the ticket instead of
                // dropping it: the caller of submit_and_wait must not wait forever.
                auto message = take_python_error();
                PY_LOG_WARNING("failing a service request whose arguments did not convert: {}",
                               message.c_str());
                coordinator->complete(request.request_id,
                                      PythonRuntimeResponse::failure(std::move(message)));
                continue;
            }
            entry["args"] = nb::steal(nb::handle(args));
            requests.append(entry);
        }
        return requests;
    }, "Service calls queued for the Python host loop, oldest first; each is a dict");

    m.def("complete_service_request", [](std::uint64_t request_id, nb::object payload) {
        auto* coordinator = Corona::Script::Python::active_python_runtime_coordinator();
        if (!coordinator) {
            PY_LOG_WARNING("complete_service_request without an active python runtime coordinator");
            return;
        }
        // Every path below settles the ticket; the failure branch still settles it, so a
        // payload that does not convert becomes an error response instead of a hang.
        bool completed = false;
        nlohmann::json converted;
        if (Corona::Script::Python::python_to_json(payload.ptr(), converted)) {
            auto response = PythonRuntimeResponse::success();
            response.service_result = std::make_shared<const nlohmann::json>(std::move(converted));
            completed = coordinator->complete(request_id, std::move(response));
        } else {
            auto message = take_python_error();
            completed = coordinator->complete(request_id,
                                              PythonRuntimeResponse::failure(std::move(message)));
        }
        if (!completed) {
            // Unknown ids are not this side's failure: the ticket already timed out or the
            // runtime is stopping, and its caller was settled by that path.
            PY_LOG_WARNING("complete_service_request for unknown request id {}", request_id);
        }
    }, nb::arg("request_id"), nb::arg("payload"),
       "Settle a request returned by poll_service_requests; payload is the service result");
}

}  // namespace EngineScripts
