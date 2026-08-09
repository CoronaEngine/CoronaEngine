#pragma once

#include <corona/engine/engine_runtime_api.h>
#include <nanobind/nanobind.h>

namespace EngineScripts {

// 在给定的 nanobind 模块上注册引擎脚本 API（Actor/Scene 等）。
void BindAll(nanobind::module_& m);

// Register only legacy editor bindings. New editor Python code must use the
// manifest-backed aggregate API instead of these compatibility functions.
void BindEditorCompatibility(nanobind::module_& m);

// Register the migration-only editor/AITool network compatibility bridge.
void BindEditorNetwork(nanobind::module_& m);

void BindCef(nanobind::module_& m);

// Called by PythonAPI on the ScriptSystem Python thread while holding the GIL.
void clear_python_callback_registry();

}  // namespace EngineScripts
