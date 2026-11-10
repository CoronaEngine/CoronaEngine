#pragma once

// The C++ -> Python service channel.
//
// This file used to own the JSON editor manifest (an api-name table, caller masks
// and the two `_invoke_cpp_*` bindings). That contract is retired: Web calls the
// typed V8 bindings, and the ScriptRuntime calls the named `script_scene_*` entry
// points in `cef/script_runtime_api.h`. What is left here is the part that was never
// the manifest's: handing a service request to the Python runtime and holding its
// dispatcher registration.

#include "cef_editor_native_api_registry.h"

#include <Python.h>

#include <chrono>
#include <string>

namespace Corona::Systems::UI {

void register_python_script_service_dispatcher(PyObject* dispatcher);
void unregister_python_script_service_dispatcher();
bool python_script_service_dispatcher_registered();

NativeResult invoke_python_script_service(const NativeRequest& request,
                                          const char* route = "python-script",
                                          std::chrono::milliseconds timeout = std::chrono::seconds(2));

}  // namespace Corona::Systems::UI
