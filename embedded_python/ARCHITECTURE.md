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
- `src/systems/ui/editor_api/cef_editor_api.h` 与 manifest 实现：编辑器聚合契约。
- `editor/api/editor_api.py`：Python 聚合契约 adapter。
- `editor/Frontend/src/api`：Vue 聚合契约 adapter。
- `editor/script_runtime/manifest_adapter.py`：角色脚本可用的受限聚合子集。

底层接口只有一个 owner；跨层 adapter 不复制 schema，而是按 caller channel 调用同一
manifest。新增业务语义应先登记 manifest，再补 Vue/Python 薄 adapter。

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
