#pragma once

#include <corona/engine/engine_runtime_api.h>
#include <nanobind/nanobind.h>

namespace EngineScripts {

// 在给定的 nanobind 模块上注册引擎脚本 API（Actor/Scene 等）。
void BindAll(nanobind::module_& m);

// Register the two host-lifecycle operations that are not editor business API:
// graceful process exit and the editor camera-input gate.
void BindEditorHost(nanobind::module_& m);

void BindCef(nanobind::module_& m);

// Called by PythonAPI on the ScriptSystem Python thread while holding the GIL.
void clear_python_callback_registry();

}  // namespace EngineScripts
