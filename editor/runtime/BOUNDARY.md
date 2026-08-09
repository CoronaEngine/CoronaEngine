# `editor/runtime` 边界

`editor/runtime` 是编辑器 Python host 层（embedded Python host），负责启动、插件注册（插件 registry）、生命周期、项目
上下文和 host support；不负责 Vue transport、C++ 场景事实或角色脚本能力。

| 路径 | 职责 |
|---|---|
| `bootstrap.py`、`editor_host.py` | Python host 启动、dispatcher、更新和关闭 |
| `registry.py`、`plugin_loader.py`、`plugin_base.py` | service 注册、加载、初始化和关闭顺序 |
| `project_context.py` | native 活动项目路径解析 |
| `archive/`、`project_templates.py`、`scene_support.py` | 归档、模板和项目辅助 |
| `native_engine.py`、`response_utils.py`、`logging.py` | host support |

`runtime.registry` 是唯一 service registry，`ACTIVE_PYTHON_SCRIPT_SERVICES` 是正常启动
清单；AITool/ProjectArchive 按 lazy 生命周期初始化。所有跨层业务仍须经过
`api.editor_api` 的 manifest adapter。

`editor/main.py` 只做入口转发，`runtime/bootstrap.py` 负责一次性注册；注册和初始化必须
幂等，关闭后不得重复初始化。

旧 `backend`、`CoronaCore`、`CoronaPlugin`、`utils`、`scripts` 和 `runtime.legacy*` 已
删除，不提供外部兼容入口。关闭后不得在同一 host 生命周期中重新注册或复活 worker。
