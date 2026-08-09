# `utils` 兼容入口

`editor/utils` 是历史编辑器 Python 导入兼容目录，不是配置或日志实现 owner。

| 兼容路径 | canonical owner | 删除条件 |
|---|---|---|
| `utils/compat/legacy_settings.py`（旧入口：`settings.py`） | `config.settings` | 外部插件和历史生成脚本完成配置导入迁移 |
| `utils/compat/legacy_logging.py`（旧入口：`logging.py`） | `runtime.logging` | 外部插件和历史宿主完成日志导入迁移 |

本目录只能转发，不得重新定义路径、设置、日志格式或运行时状态。删除前必须通过配置、
宿主启动和外部插件兼容回归。
