# `scripts` 兼容入口

`editor/scripts` 只保留历史打包命令的执行入口；打包实现的 canonical owner 是仓库顶层
`tools/pack.py`，它不属于编辑器 Python runtime。

| 兼容路径 | canonical owner | 删除条件 |
|---|---|---|
| `scripts/compat/legacy_pack.py`（旧入口：`scripts/pack.py`） | `tools.pack` | 外部构建脚本改用 `tools/pack.py`，并通过打包 dry-run 与发布回归 |

wrapper 不得复制打包规则或引入编辑器运行时依赖。删除前必须确认历史 CI、开发文档和外部
发布脚本均已迁移。
