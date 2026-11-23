# API Ownership

跨层契约按调用方分裂：Web 走 typed V8 具名绑定；Python 侧是**两组**——宿主支持（4 个 `m.def`
加它们的载荷类型 `InputEvent`）与编辑器传输（服务派发器与项目会话）。Web 面与这两组都不重叠。
JSON manifest 与脚本运行时契约都已删除；随脚本运行时一起留下的引擎对象模型也已删除（见"历史"）。

| 语义 | C++ owner | Python 侧 | Vue 侧 |
|---|---|---|---|
| Web ↔ C++ | `src/systems/ui/cef/cef_v8_binding.cpp` 的 typed V8 具名绑定 | 不参与 | `src/api/editorApi.js` |
| Web → Python 服务 | `src/systems/ui/editor_api/python_service_bridge.cpp`（只做传输） | `runtime/editor_host.py` 的服务白名单 | `corona.bridge.call` |
| Python 宿主 ↔ C++ | `src/systems/script/python/host_support_bindings.cpp` 的 `send_log`、`drain_input_events`、`python_runtime_phase`、`should_stop`；`src/systems/ui/cef/cef_py_bind.cpp` 的 `active_project_path`、`active_project_session`、`register_python_script_service_dispatcher`、`unregister_python_script_service_dispatcher` | `runtime/native_engine.py` 只取模块，不包 schema | 不参与 |
| 载荷类型 | 同一文件的 `InputEvent`（顶层，字段与 `input.dispatch` 同名） | 与 `drain_input_events` 同层；名字由 `host_support_bindings.cpp` 声明 | 不参与 |

## 规则

1. C++ 持有场景、Actor、资源、变换和 revision 的权威事实。
2. Web 只能调用 typed V8 具名契约；不得再引入 api-name 表、caller 掩码或 JSON 传输。
3. Python 只有一类入口：`CoronaEngine` 顶层的宿主支持（含 `InputEvent`）。编辑器语义
   （场景、相机、窗口、项目）**不能**从 Python 直接到达 C++，只能经 Web 的 typed V8 或
   Python 服务；跨层 adapter 不复制校验、状态机或错误码。这条规则有可执行断言兜底：
   `test_python_native_surface.py` 拒绝任何引用场景操作层（`execute_scene_operation`、
   `SceneRequest`、`SceneMethod`、`cef_scene_operations.h` 等）的 Python 绑定。
4. 新增语义：Web 加一份 V8 具名绑定（`editor_operation_table.h` 里的一行），宿主支持再加
   一个 `m.def`；两者都不再需要在 `tests/tools/` 的手写名单里逐名登记（逐名校验已删除）。
   契约文档 `docs/interface-contract.md` 是手工维护的快照，改完接口要自己更新。禁止新增
   第二份 schema。
5. 服务调用只有一个入口：`corona.bridge.call` 经 `WebBridgeService` 白名单进入 Python，
   Python 结果按 `{success, data, code}` 原样回传，不在中途重建 schema。
6. **载荷类型跟着产出它的函数走**：`drain_input_events()` 是顶层函数，`InputEvent` 就留在
   顶层。这条规则决定了宿主面的分组结构。
7. **模块上不放大块能力**：引擎对象模型（`Scene`/`Actor`/`Camera`/…）曾经导出过一次，
   仓内没有任何消费者，于是被删除。要不要重新给 Python 场景读写能力，是产品决定；
   重启它应当是一次明确的、带调用方的设计，而不是把旧绑定搬回来。

## 历史

`backend`、`CoronaCore`、`CoronaPlugin`、`utils`、`scripts`、`runtime.legacy*`、
`script_runtime/`（含 `native_scene_adapter.py`）、`api/`（`editor_api.py`）、
`FileScripts` 服务与 JSON manifest（api-name 表 + `EditorApiRegistry` +
`NativeApiRegistry` + `_invoke_cpp_*` 绑定）均已删除，不提供兼容层。
引擎对象模型（`engine_bindings.cpp` 的 `BindEngineObjectModel`、`CoronaEngine.script` 下的
9 个类）随后也删除了：它是脚本运行时的脚本 API，随脚本运行时一起失去调用方。
