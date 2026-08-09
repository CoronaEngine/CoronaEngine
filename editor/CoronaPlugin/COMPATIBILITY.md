# `CoronaPlugin` 兼容入口

`editor/CoronaPlugin` 只保留历史 Python 插件导入路径。插件基类和服务加载的 canonical
owner 位于 `runtime`，本目录不负责 RPC 注册、场景操作或插件生命周期。

| 兼容路径 | canonical owner | 删除条件 |
|---|---|---|
| `CoronaPlugin/compat/legacy_plugin_base.py`（旧入口：`core/corona_plugin_base.py`） | `runtime.plugin_base` | 外部插件完成导入迁移，并通过显式 `runtime.registry` 注册回归 |
| `CoronaPlugin/compat/legacy_load_utils.py`（旧入口：`utils/load_utils.py`） | `runtime.plugin_loader` | 外部插件完成 loader 迁移，并通过插件加载回归 |

wrapper 只能保持历史导入、装饰器标记和返回语义；不得新增服务注册、manifest、schema
或业务状态。删除前必须确认仓内、外部插件和旧宿主均不再使用历史路径。
