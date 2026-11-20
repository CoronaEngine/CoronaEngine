#pragma once

#include <corona/engine/engine_runtime_api.h>
#include <nanobind/nanobind.h>

namespace EngineScripts {

// The entries the embedded Python host needs: engine logging, the runtime diagnostic phase
// and the input queue it drains. Their payload types live here too - `InputEvent` is top
// level because `drain_input_events` hands it out.
void BindHostSupport(nanobind::module_& m);

// The editor transport the Python host needs: the service dispatcher registration and the
// project session the host compares incoming requests against. See `cef_py_bind.cpp`.
void BindCef(nanobind::module_& m);

}  // namespace EngineScripts
