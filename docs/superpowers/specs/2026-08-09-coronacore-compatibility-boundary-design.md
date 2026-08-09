# CoronaCore 兼容边界收敛设计

## 目标

明确 `editor/CoronaCore` 是历史 Python 导入兼容包，不是 Engine、Editor API、Script
Runtime 或 legacy Scene 的 canonical 实现目录。仓内新代码必须使用 `api`、`runtime`
或 `script_runtime` 中登记的 owner。

本单元只增加可追溯的目录边界和验证，不删除旧导入路径，不改变实体、组件、脚本运行时
或 Scene store 的行为。

## Owner 映射

| CoronaCore 路径族 | canonical owner | 兼容范围 |
|---|---|---|
| `archive/*` | `runtime.archive` | 历史项目归档导入 |
| `core/corona_editor.py` | `runtime.editor_host` | 旧编辑器宿主 |
| `core/corona_engine.py` | `runtime.native_engine` | 旧 native engine loader |
| `core/editor_api.py` | `api.editor_api` | 历史 Editor API 导入 |
| `core/engine_runtime.py` | `runtime.legacy_engine_adapter` | 旧宿主和登记的测试替身 |
| `core/entities/*`、`core/components/*`、`core/managers/*` | `runtime.legacy.*` | 旧 Python Scene/Script 宿主 |
| `core/legacy/*` | `runtime.legacy.*` | 旧 Scene 实体和组件导入 |
| `core/scripts_system/*` | `script_runtime.engine` | 历史角色脚本和生成脚本 |
| `core/*_utils.py`、`utils/*` | `runtime.*` 或 `script_runtime.*` | 历史工具导入 |
| `core/*editor_api.py`、`legacy_scene*` | `api`、`script_runtime` 或 `runtime` 对应 adapter | 旧跨层 adapter |

## 约束

- CoronaCore 文件只能是兼容声明、单向转发或明确的模块 alias；
- 不在 CoronaCore 新增 manifest、schema、业务状态机或场景事实；
- canonical owner 不得反向导入 CoronaCore；
- 新编辑器业务不得直接导入 `CoronaCore.core`；
- legacy 实体仍可由登记的 runtime adapter 使用，不能因为 wrapper 存在而升级为公共 API。

## 删除条件

删除任一 CoronaCore 路径族前必须满足：

1. 仓内生产代码和生成脚本没有该历史导入；
2. 外部宿主、旧项目脚本和已发布生成物完成迁移；
3. 对应的 Script Runtime、宿主生命周期或归档回归通过；
4. 兼容测试、发布说明和回滚路径已更新；
5. `API_OWNERSHIP.md` 中的 legacy 行已标记完成。

## 实施拆分

- 设计记录：登记本单元范围和 owner；
- 边界测试：验证 wrapper 形态、canonical 反向依赖和历史入口分类；
- 目录清单：在 `CoronaCore/COMPATIBILITY.md` 记录具体路径族、调用范围和删除条件；
- 文档同步：更新编辑器目录导航和 legacy owner 清单。

