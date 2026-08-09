# `backend` 兼容入口

`editor/backend` 是历史导入兼容目录，不是编辑器服务或 Blockly 的实现目录。
新代码必须使用表格中的 canonical owner；本目录只能保留单向转发 wrapper。

这里提到的历史 `backend/script` 和 `backend/runScript.py` 是旧项目根目录中的生成输出，
不是当前 `editor/backend` 下的文件；当前项目的新 Blockly 输出由 `script_runtime` 管理。

## Owner 映射

| 兼容路径 | canonical owner | 当前调用范围 | 删除条件 |
|---|---|---|---|
| `registry.py` | `runtime.registry` | 旧嵌入宿主 | 外部宿主不再导入 `backend.registry`，且启动回归通过 |
| `blockly/main.py` | `script_runtime.blockly.main` | 历史 Blockly/生成脚本 | 历史生成脚本和外部宿主完成导入迁移 |
| `blockly/ai_node_graph_contract.py` | `script_runtime.blockly.ai_node_graph_contract` | 历史节点图导入 | 外部节点图生成器完成导入迁移 |
| `blockly/check_ai_contract_catalog.py` | `script_runtime.blockly.check_ai_contract_catalog` | 历史契约检查入口 | 外部检查脚本完成导入迁移 |
| `file_system/main.py` | `plugins.FileManager.compat.legacy_file_manager` | 旧 FileManager 注册入口 | 外部宿主完成插件路径迁移 |
| `project_settings/main.py` | `plugins.ProjectSettings.compat.legacy_project_settings` | 旧 ProjectSettings 注册入口 | 外部宿主完成插件路径迁移 |

## 约束

- wrapper 不得定义新的 class、function、schema、状态机或生命周期；
- canonical owner 不得反向导入 `backend`；
- 不在此目录新增业务测试，测试归属 canonical owner 或跨层 `editor/tests`；
- 未确认外部宿主迁移前，不删除兼容路径；
- 若兼容路径必须修改，只能保持原有导入、参数、返回值和异常语义。

## 删除前检查

删除任一 wrapper 前必须确认：

1. 仓内生产代码和生成脚本没有引用该路径；
2. 外部嵌入宿主已切换到 canonical owner；
3. 对应的启动、Blockly/脚本运行或插件注册回归通过；
4. 删除前后的兼容测试和回滚方式已记录在迁移提交中。
