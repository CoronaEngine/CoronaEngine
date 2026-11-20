# Editor Architecture

## 三层职责

| 层 | 负责 | 不负责 |
|---|---|---|
| Vue | 页面、输入、临时 UI 状态、结果展示和 `src/api` 聚合 adapter | 场景事实、AI 业务、引擎对象 |
| C++ | 场景/Actor/资源的当前内存值、渲染和 typed V8 具名绑定 | 在移动 Actor 时写入场景存档 |
| Python | 项目场景存档的解析、校验与显式保存，以及项目文档/脚本字节的读写 | 场景事实、Blockly 图编辑与代码生成 |

编辑器写操作只能经过 C++ 的 typed V8 具名绑定。底层引擎 API（例如 `camera()`、
`geometry()`）属于 C++ 内部实现，Vue 不直接导入它们。

Python 侧分两组：顶层的宿主支持（7 个 `m.def` + 载荷类型 `InputEvent`）与 `cef_py_bind.cpp`
的编辑器传输。`tests/tools/python_surface.py` 把两组一起钉死，新增名字必须同步。
`CoronaEngine.script` 下的引擎对象模型曾作为第三组存在，随脚本运行时失去全部调用方后被删除。

## API 分层

- `include/corona/engine/engine_runtime_api.h`：C++ Engine Runtime 底层接口。
- `src/systems/ui/cef/cef_v8_binding.cpp`：Web 唯一的 C++ 契约（typed V8 具名绑定）。
- `src/systems/script/python/host_support_bindings.cpp`：`CoronaEngine` 顶层的宿主支持与 `InputEvent`。
- `src/systems/ui/cef/cef_py_bind.cpp`：`CoronaEngine` 顶层的编辑器传输。
- `editor/Frontend/src/api`：Vue 聚合契约 adapter。
- `runtime/editor_host.py`：Python 宿主唯一入口——`WebBridgeService` 的服务白名单。

JSON manifest（api-name 表 + caller 掩码 + `EditorApiRegistry`/`NativeApiRegistry`）
已删除；`api/editor_api.py` 与 `script_runtime/` 随脚本运行时一起删除，Python 只剩上面两组。

底层接口只有一个 owner；跨层 adapter 不复制业务状态机。新增 Web 语义加一份 typed V8
具名绑定，新增宿主支持再加一个 `m.def` 并登记到 `python_surface.py`；禁止新增第二份 schema。

## Python 目录

| 目录 | 责任 |
|---|---|
| `main.py`、`runtime/bootstrap.py` | C++ 的 import 入口和一次性注册 |
| `config` | 宿主启动路径和运行时配置值（不创建目录、不读配置文件） |
| `runtime` | host、registry、生命周期和运行时数据 |
| `plugins` | 项目存档服务，以及文件管理器使用的 Actor 模板 |

历史 `api`、`backend`、`CoronaCore`、`CoronaPlugin`、`utils`、`scripts`、`script_runtime`
目录已删除；仓内不再提供外部旧 import 兼容，新增代码不得重新引入这些路径。

## 生命周期

`runtime.editor_host` 负责 native module、Python service 注册、项目上下文、服务调用入口
和关闭；`runtime.registry` 是唯一服务注册 owner。插件不自行创建第二个 host 或直接操作
native 全局状态。

## 数据边界

C++ 返回具名绑定定义的值对象。Python 可以维护任务状态和缓存，但不能把缓存当作场景
事实；Vue 的状态也必须以 C++ 返回值或事件为准。

AI 功能由 Web 会话运行时承担：业务编排、模型网络请求、流式解析、队列、取消和内存密钥
均在前端。主文档为独立面板提供启动会话。宿主保留通用项目文档读写，不注册 AITool。
