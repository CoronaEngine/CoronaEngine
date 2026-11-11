# API Ownership

跨层契约按调用方分裂成两份，互相不重叠；JSON manifest 已删除。

| 语义 | C++ owner | Python 侧 | Vue 侧 |
|---|---|---|---|
| Web ↔ C++ | `src/systems/ui/cef/cef_v8_binding.cpp` 的 typed V8 具名绑定 | 不参与 | `src/api/editorApi.js` |
| Web → Python 服务 | `src/systems/ui/editor_api/python_service_bridge.cpp`（只做传输） | `runtime/editor_host.py` 的服务白名单 | `corona.bridge.call` |
| 脚本 ↔ C++ | `src/systems/ui/cef/script_runtime_api.h` 的 16 条具名入口 | `script_runtime/native_scene_adapter.py` | 不参与 |

## 规则

1. C++ 持有场景、Actor、资源、变换和 revision 的权威事实。
2. Web 只能调用 typed V8 具名契约；不得再引入 api-name 表、caller 掩码或 JSON 传输。
3. 角色脚本只能使用 `script_scene_*` 具名入口；模块边界即权限边界。
4. adapter 只负责参数/结果转换，不复制校验、状态机或错误码。
5. 新增语义：Web 加一份 V8 具名绑定，脚本加到 `script_runtime_api.h`；禁止新增第二份 schema。

## 历史

`backend`、`CoronaCore`、`CoronaPlugin`、`utils`、`scripts`、`runtime.legacy*` 与
JSON manifest（api-name 表 + `EditorApiRegistry` + `NativeApiRegistry` +
`_invoke_cpp_*` 绑定）均已删除，不提供兼容层。
