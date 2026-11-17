# API Ownership

跨层契约按调用方分裂：Web 走 typed V8 具名绑定，Python 宿主只有 host-support 具名入口，
两者互不重叠。JSON manifest 与脚本运行时契约都已删除。

| 语义 | C++ owner | Python 侧 | Vue 侧 |
|---|---|---|---|
| Web ↔ C++ | `src/systems/ui/cef/cef_v8_binding.cpp` 的 typed V8 具名绑定 | 不参与 | `src/api/editorApi.js` |
| Web → Python 服务 | `src/systems/ui/editor_api/python_service_bridge.cpp`（只做传输） | `runtime/editor_host.py` 的服务白名单 | `corona.bridge.call` |
| Python 宿主 ↔ C++ | `src/systems/script/python/engine_bindings.cpp`、`editor_host_bindings.cpp` 与 `src/systems/ui/cef/cef_py_bind.cpp` 的 7 个 `m.def`（日志、输入排空、生命周期相位、活动项目、相机输入、服务注册） | `runtime/native_engine.py` 只取模块，不包 schema | 不参与 |

## 规则

1. C++ 持有场景、Actor、资源、变换和 revision 的权威事实。
2. Web 只能调用 typed V8 具名契约；不得再引入 api-name 表、caller 掩码或 JSON 传输。
3. Python 只能调用 `CoronaEngine` 上的 host-support 具名入口；跨层 adapter 不复制校验、
   状态机或错误码。
4. 新增语义：Web 加一份 V8 具名绑定，宿主支持再加一个 `m.def`；禁止新增第二份 schema。
5. 服务调用只有一个入口：`corona.bridge.call` 经 `WebBridgeService` 白名单进入 Python，
   Python 结果按 `{success, data, code}` 原样回传，不在中途重建 schema。

## 历史

`backend`、`CoronaCore`、`CoronaPlugin`、`utils`、`scripts`、`runtime.legacy*`、
`script_runtime/`（含 `native_scene_adapter.py`）、`api/`（`editor_api.py`）、
`FileScripts` 服务与 JSON manifest（api-name 表 + `EditorApiRegistry` +
`NativeApiRegistry` + `_invoke_cpp_*` 绑定）均已删除，不提供兼容层。
