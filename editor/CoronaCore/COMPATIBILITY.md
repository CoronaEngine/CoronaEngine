# `CoronaCore` 兼容入口

`editor/CoronaCore` 是历史 Python 导入兼容包，不是 Engine、Editor API、Script Runtime
或 legacy Scene 的实现 owner。新代码必须使用表格中的 canonical owner；本目录只能保留
兼容声明、单向转发或模块 alias。

## 兼容层自身的目录规则

`CoronaCore` 已经是最外层兼容边界，不再在其中新增 `compat/` 子层。`archive/`、`core/`
和 `utils/` 下的旧路径直接保留为 shim，并分别转发到表格中的 canonical owner；再增加
一层 `CoronaCore/compat` 只会重复转发，不能减少旧导入风险。新的兼容实现若确实需要
业务适配，应放在对应 canonical owner 的 `compat` 或 `runtime/legacy_*` 中，由旧路径
直接转发。

## Owner 映射

| 兼容路径族 | canonical owner | 当前调用范围 | 删除条件 |
|---|---|---|---|
| `archive/*` | `runtime.archive` | 历史项目归档导入 | 外部归档工具完成迁移并通过解析回归 |
| `core/corona_editor.py` | `runtime.editor_host` | 旧编辑器宿主 | 外部宿主完成 runtime host 迁移 |
| `core/corona_engine.py` | `runtime.native_engine` | 旧 native loader | 外部宿主完成 native loader 迁移 |
| `core/editor_api.py` | `api.editor_api` | 历史 Editor API 导入 | 外部宿主完成公共 adapter 迁移 |
| `core/engine_runtime.py` | `runtime.legacy_engine_adapter` | 旧宿主和登记的测试替身 | 外部宿主迁移且测试替身改用 manifest adapter |
| `core/components/*` | `runtime.legacy.components` | 旧 Python Scene/Script 宿主 | 角色脚本和旧宿主完成受限 runtime 迁移 |
| `core/entities/*` | `runtime.legacy.entities` | 旧 Python Scene/Script 宿主 | 角色脚本和旧宿主完成值对象迁移 |
| `core/managers/*` | `runtime.legacy.managers` | 旧 Scene manager 调用方 | 所有旧宿主改用集中 legacy adapter |
| `core/legacy/*` | `runtime.legacy` | 旧 Scene 实体、组件和 manager | 旧 Python Scene 宿主完全退出 |
| `core/scripts_system/*` | `script_runtime.engine` | 历史角色脚本和生成脚本 | 旧项目脚本和外部生成物完成迁移 |
| `core/network_sync_policy.py` | `runtime.network_sync_policy` | 历史同步策略导入 | 外部宿主完成 runtime policy 迁移 |
| `core/project_utils.py` | `runtime.project_templates` + `runtime.scene_support` | 历史项目 helper 导入 | 外部项目工具完成迁移 |
| `core/response_utils.py` | `runtime.response_utils` | 历史响应 helper 导入 | 外部宿主完成 runtime helper 迁移 |
| `core/script_runtime_editor_api.py` | `script_runtime.manifest_adapter` | 旧角色脚本 adapter | 外部脚本完成受限 adapter 迁移 |
| `core/legacy_editor_api.py` | `script_runtime.compat.legacy_scene_datas_adapter` | 历史 SceneDatas 导入 | 旧 SceneDatas 宿主完成迁移 |
| `core/legacy_scene_datas_adapter.py` | `script_runtime.compat.legacy_scene_datas_adapter` | 历史 SceneDatas 导入 | 旧 SceneDatas 宿主完成迁移 |
| `core/legacy_scene_store.py` | `runtime.legacy_scene_store` | 登记的 legacy fallback | 所有旧 Scene fallback 删除并通过回归 |
| `utils/*` | `runtime.` 或 `script_runtime.` | 历史工具和生成脚本 | 外部工具完成 canonical import 迁移 |

## 约束

- 不在本目录新增 class、function、manifest、schema、业务状态机或场景事实；
- canonical owner 不得反向导入 `CoronaCore`；
- 新编辑器业务不得直接导入 `CoronaCore.core`；
- legacy 实体只允许由登记的 runtime adapter 使用，不因兼容 wrapper 存在而成为公共 API；
- 兼容路径修改时必须保持原有导入、参数、返回值、异常和生命周期语义。

## 删除前检查

删除任一路径族前必须确认：

1. 仓内生产代码、生成脚本和测试替身没有历史导入；
2. 外部宿主、旧项目脚本和已发布生成物完成迁移；
3. 对应的宿主、Script Runtime 或归档回归通过；
4. `API_OWNERSHIP.md` 中的 legacy 行已更新；
5. 删除提交包含兼容测试调整、发布说明和回滚路径。
