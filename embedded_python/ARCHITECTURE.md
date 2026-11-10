# Editor Architecture

## 三层职责

| 层 | 负责 | 不负责 |
|---|---|---|
| Vue | 页面、输入、临时 UI 状态、结果展示和 JS manifest adapter | 场景事实、AI 业务、引擎对象 |
| C++ | 场景/Actor/资源的当前内存值、渲染和跨层 manifest handler | 在移动 Actor 或停止脚本时写入场景存档 |
| Python | 项目场景存档解析、校验与显式保存，Blockly 生成文件的按路径执行、输入与恢复 | Blockly 图编辑与代码生成 |

编辑器写操作必须经过 C++ manifest 聚合接口。底层引擎 API（例如 `camera()`、
`geometry()`）只属于 C++ 内部实现；Vue 和普通 Python 不直接导入或暴露它们。

## API 分层

- `include/corona/engine/engine_runtime_api.h`：C++ Engine Runtime 底层接口。
- `src/systems/ui/cef/cef_v8_binding.cpp`：Web 唯一的 C++ 契约（typed V8 具名绑定）。
- `src/systems/ui/cef/script_runtime_api.h`：Script Runtime 的 16 条具名入口。
- `embedded_python/api/editor_api.py`：Python 侧受限契约入口。
- `editor/Frontend/src/api`：Vue 聚合契约 adapter。
- `script_runtime/native_scene_adapter.py`：角色脚本可用的受限子集。

JSON manifest（api-name 表 + caller 掩码 + `EditorApiRegistry`/`NativeApiRegistry`）
已删除；Web 与 Script Runtime 各走一份具名契约。

底层接口只有一个 owner；跨层 adapter 不复制业务状态机。新增 Web 语义直接加 typed
V8 具名绑定；新增脚本语义加到 `script_runtime_api.h` 的具名入口。

## Python 目录

| 目录 | 责任 |
|---|---|
| `api` | Python 公共契约 |
| `config` | 路径和项目状态 |
| `runtime` | host、registry、生命周期和运行时数据 |
| `script_runtime` | Blockly 生成文件的受限执行环境 |
| `plugins` | 项目存档服务和原生模板 |

历史 `backend`、`CoronaCore`、`CoronaPlugin`、`utils`、`scripts` 目录已删除；仓内不再
提供外部旧 import 兼容。旧项目应迁移到上述 canonical owner。

## 生命周期

`runtime.editor_host` 负责 native module、Python service 注册、项目上下文、dispatcher
和关闭；`runtime.registry` 是唯一服务注册 owner。插件不自行创建第二个 host 或直接
操作 native 全局状态。

## 数据边界

C++ 返回 manifest 定义的 JSON/value object。Python 可以维护任务状态和缓存，但不能把
缓存当作场景事实；Vue 的状态也必须以 C++ 返回值或事件为准。

AI 功能由 Web 会话运行时承担：业务编排、模型网络请求、流式解析、队列、取消和内存密钥均在前端。主文档为独立面板提供启动会话。宿主保留通用项目文档读写，不注册 AITool。
