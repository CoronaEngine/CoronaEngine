# Backend 兼容入口收敛设计

## 目标

明确 `editor/backend` 的唯一职责：为旧嵌入宿主和历史生成脚本提供导入兼容，不再承担
服务注册、Blockly 编排、文件服务或项目设置的 canonical 实现。仓内新代码必须依赖
对应的 canonical owner。

本单元只整理目录边界和可验证的迁移记录，不删除可能被仓外宿主使用的导入路径，也不
引入新的公共 API 层。

## 现状与 owner 映射

| 兼容路径 | canonical owner | 保留原因 |
|---|---|---|
| `backend.registry` | `runtime.registry` | 旧嵌入宿主可能直接导入服务注册入口 |
| `backend.blockly.main` | `script_runtime.blockly.main` | 历史 Blockly/生成脚本导入路径 |
| `backend.blockly.ai_node_graph_contract` | `script_runtime.blockly.ai_node_graph_contract` | 历史节点图契约导入路径 |
| `backend.blockly.check_ai_contract_catalog` | `script_runtime.blockly.check_ai_contract_catalog` | 历史契约检查器入口 |
| `backend.file_system.main` | `plugins.FileManager.main` | FileManager 已迁移到插件 owner |
| `backend.project_settings.main` | `plugins.ProjectSettings.main` | ProjectSettings 已迁移到插件 owner |

`backend/__init__.py` 只声明兼容边界，不定义服务或业务语义。`backend` 下不得新增
canonical 实现、schema、状态机或测试 owner。

## 依赖方向

```text
旧宿主/历史生成物 -> editor.backend.* -> canonical owner
新 runtime        -> runtime / script_runtime / plugins
```

canonical owner 不得反向依赖 `backend`。兼容 wrapper 只能转发，不能复制实现或修改
参数、返回值、错误码和生命周期语义。

## 验证范围

本单元需要验证：

1. 所有受版本控制的 `backend` 生产文件都是兼容声明或单向转发 wrapper；
2. `runtime`、`script_runtime`、`plugins` 和其他新业务代码不导入 `backend`；
3. wrapper 的导入对象与 canonical owner 一致，至少覆盖服务类、Blockly 契约和 registry；
4. 兼容入口的保留理由、调用方范围和删除条件在架构文档中可追溯；
5. 删除条件未满足前不删除旧路径。

## 删除条件

只有同时满足以下条件，才可删除对应 wrapper：

- 仓内搜索没有生产调用方；
- 外部嵌入宿主和历史生成脚本已确认迁移到 canonical import；
- 对应兼容导入测试已迁移或删除；
- 至少一个完整启动、Blockly/脚本运行和插件注册回归通过；
- 删除及回滚路径已单独记录。

## 后续拆分

本设计之后的实现按以下独立功能提交：

- 边界测试：验证 wrapper 形态和仓内依赖方向；
- 文档登记：同步 `editor/README.md`、`editor/ARCHITECTURE.md` 和 owner 清单；
- 如有必要的 wrapper 修复：只修正转发，不混入其他目录迁移。

