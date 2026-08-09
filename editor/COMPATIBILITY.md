# Editor 兼容入口总索引

本文件汇总 `editor` 下的历史导入路径、兼容 facade 和 raw-host adapter。它不替代各
目录的局部边界文档，而是用于确定迁移顺序和删除门槛。

## 全局规则与删除条件

- 每个兼容入口必须有明确的 canonical owner，只允许单向转发或兼容语义；
- 每个 `compat/legacy_*.py` 必须登记 canonical owner，并在局部边界文档或本索引中说明旧入口、
  当前调用范围和删除条件；
- 新代码不得新增对兼容路径的依赖，兼容目录禁止新增业务，不得在 wrapper 中新增业务、schema、状态或
  transport；
- 删除前必须确认仓内调用方、外部宿主/插件、旧项目生成物和回退路径，并通过相应回归；
- 仅凭仓内搜索不到调用方不能直接删除，尤其是可能被外部宿主导入的 Python 包；
- `runtime.registry` 是 Python plugin lifecycle owner，`script_runtime` 是受限角色脚本
  owner，`editorApi` 是 Vue/CEF 公共契约 adapter owner。

## 入口映射

| 兼容入口 | canonical owner | 风险/当前范围 | 删除前置条件 |
|---|---|---|---|
| [`backend/COMPATIBILITY.md`](backend/COMPATIBILITY.md) | `runtime.registry`、`script_runtime.blockly`、各聚合 plugin | 历史 Blockly、FileManager、ProjectSettings 和 registry 导入 | 外部宿主和历史生成脚本完成路径迁移，并通过启动/生成回归 |
| [`CoronaCore/COMPATIBILITY.md`](CoronaCore/COMPATIBILITY.md) | `runtime`、`api.editor_api`、`script_runtime` | 最高风险：`CoronaCore.core.entities`、components、managers 可能被旧角色脚本/宿主使用 | 旧脚本、宿主和测试替身完成受限 adapter 迁移；保留可回退提交 |
| [`CoronaPlugin/COMPATIBILITY.md`](CoronaPlugin/COMPATIBILITY.md) | `runtime.plugin_base`、`runtime.plugin_loader` | 外部插件历史 import | 外部插件完成导入迁移并通过显式 registry 回归 |
| [`utils/COMPATIBILITY.md`](utils/COMPATIBILITY.md) | `config.settings`、`runtime.logging` | `utils.settings`、`utils.logging` 历史路径 | 外部插件和旧生成脚本完成配置/日志导入迁移 |
| [`scripts/COMPATIBILITY.md`](scripts/COMPATIBILITY.md) | `tools.pack` | `scripts.pack` 历史打包入口 | CI、开发文档和外部发布脚本切换后通过 dry-run/发布回归 |
| [`plugins/MainView/COMPATIBILITY.md`](plugins/MainView/COMPATIBILITY.md) | `plugins.MainView.compat.legacy_main_view_scene_adapter`、manifest 聚合接口（runtime 路径为 shim） | 迁移中的项目/场景宿主编排 | native project/scene lifecycle 覆盖旧 Scene 生命周期并通过旧宿主回归 |
| [`plugins/SceneDatas/COMPATIBILITY.md`](plugins/SceneDatas/COMPATIBILITY.md) | `scene.*`、`sceneTools.*` manifest | compatibility-only 旧 SceneDatas 服务壳；Vue Object 面板使用独立 `Object` UI ID，正常启动不自动注册 | native scene lifecycle 覆盖旧面板初始化、读写、切换和关闭 |
| [`Frontend/src/compat/BOUNDARY.md`](Frontend/src/compat/BOUNDARY.md) | `Frontend/src/api`、`Frontend/src/services` | 旧 CEF raw adapter、bridge barrel 和 camera lock panel | 外部宿主完成迁移并通过 CEF/多窗口回归 |
| `plugins/ProjectArchive/`（实现：`compat/legacy_project_archive.py`；旧入口：`main.py`） | `runtime/archive`、`project.migrateLegacyScene` | 归档 compatibility facade，根入口仅保留历史注册 | 外部归档工具完成迁移并通过解析/迁移回归 |
| `runtime/compat/legacy_project_copy.py`（插件入口：`plugins/ProjectLauncher/compat/legacy_project_copy.py`；旧入口：`project_copy.py`、`utils/project_copy.py`） | `runtime.legacy_project_copy` | 历史项目复制/打开兼容 owner，保留 `core_path` 注入 | 外部旧 import 完成迁移 |
| `plugins/SceneTools/main.py`（Python service） | `scene.*`、`sceneTools.*`、`viewport.*` manifest；Vision 旧流程由 `plugins/SceneTools/compat/` 持有 | Vue、C++、AITool 和 Script Runtime 不再依赖 Python service；仅旧 Vision 宿主可显式注册 | Vision 旧宿主完成迁移、native handler 覆盖剩余生命周期和导入状态后，删除 service 与 compat facade |
| `plugins/SceneTools/compat/legacy_vision_import_adapter.py` | `sceneTools.*` native manifest；旧 Scene fallback 仍由该 adapter 集中保留 | 原未引用 `legacy_vision_import_helper.py` 与 `vision_import.py` 已删除 | 若外部依赖旧 helper，revert 删除提交后按 native contract 迁移 |

高风险历史族包括 `CoronaCore.core.entities`、`CoronaCore.core.components`、
`backend.blockly`、`CoronaPlugin`、`utils.settings`、`scripts.pack`、旧
`SceneDatas` 注册入口，以及 Frontend 的 `window.coronaBridge` 兼容调用。它们都必须
先确认外部宿主和旧生成物，再执行删除或收紧。

## 推荐迁移顺序

1. 先将仓内 active 代码迁移到 canonical owner，并用边界测试防止新调用回流；
2. 再确认外部宿主、旧项目脚本、CI 和生成物的 import 使用；
3. 为单个兼容族建立回归和回退提交；
4. 最后删除 wrapper，并单独提交 `revert` 可回退的删除变更。

## 仓内生产引用静态审计（2026-08-09）

本次审计只覆盖仓内非测试、非文档源码，排除了 `plugins/AITool/Quasar` 外部子模块；
它不能证明外部调用方不存在，外部调用状态仍为未知。

| 兼容族 | 仓内生产引用证据 | 当前结论 |
|---|---|---|
| `backend`、`CoronaCore`、`CoronaPlugin`、`utils`、`scripts` | 未发现直接 import；仅测试、文档或兼容检查引用 | 可进入外部宿主确认阶段，不直接删除 |
| `script_runtime.runner` 的旧脚本路径 | 读取项目中的 `backend/runScript.py` 作为只读回退 | 迁移旧项目生成物前必须保留 fallback |
| Frontend `legacyEditorAdapter.js` | `src/main.js` 仍启动加载 `compat/legacyEditorAdapter.js`；`src/utils/legacyEditorAdapter.js` 保留旧 import | 仍被启动加载，不能删除 |
| Frontend `legacyCameraLockPanel.js` | `src/index.html` 仍加载该 legacy panel | 仍被启动加载，不能删除 |
| `runtime/compat/legacy_project_copy.py`（插件入口：`ProjectLauncher/compat/legacy_project_copy.py`；旧入口：`project_copy.py`、`utils/project_copy.py`） | 未发现仓内生产引用；实现已集中到兼容 owner，历史 `core_path` 注入仍由该 owner 保留 | 外部调用未知，先保留 wrapper |
| `SceneTools/compat/legacy_vision_import_helper.py`（旧入口：`SceneTools/vision_import.py`） | 已删除；仓内非测试生产引用为零 | 受支持 Vision 流程继续使用 native manifest/登记的 legacy adapter；外部依赖可通过删除提交回滚 |

这是一份仓内静态证据快照，不是外部依赖证明。任何删除仍需外部宿主确认、回归和
可回退提交。

## 迁移验证记录（2026-08-09）

- `95a60fc1` 已将 `SceneDatas` 与 `ProjectLauncher` 的 Python compatibility service
  从正常启动注册中隔离；旧宿主仍可通过显式 legacy registration 恢复。
- 当前 `FileManager` 和 `ProjectSettings` Python compatibility service 也已从正常启动
  注册集合隔离，native `files.*` / `projectSettings.*` UI 流程不变。
- `MainView` Python compatibility service 同样已从正常启动注册集合隔离；主流程继续由
  native lifecycle 负责，旧关闭/外部宿主路径仍保留。
- `27798734` 已将 `SceneTools` Python compatibility service 从正常启动注册集合隔离；Vue、
  C++、AITool 与 Script Runtime 继续使用 native manifest，旧 Vision 宿主可通过显式 legacy
  registration 恢复该 facade。
- `ProjectArchive` 保留为 native `project.*` 迁移所需的 parser facade，但已改为 lazy
  初始化；编辑器通过 `archive_service_ready` 等状态等待，不把 parser import 放在启动阻塞路径。
- `f0a46d11` 已将 SceneTools 的前端 owner 审计切换到 `editorApi`、`services` 和
  `compat` 的实际目录边界；SceneTools 测试通过 `202 passed`、`801 subtests passed`。
- 核心 editor 迁移测试（API、runtime、Frontend、ProjectArchive、ProjectLauncher、
  ProjectSettings、SceneDatas、SceneTools 和 editor 边界测试）通过 `604 passed`、
  `801 subtests passed`。
- 完整 `pytest editor` 集合在 124 秒内没有输出并超时，因此没有被当作通过证据；AITool/Quasar
  等长耗时集合仍需单独验证。
- 上述证据只证明仓内 owner 和回归状态，不改变外部宿主依赖未知、不得直接删除兼容入口的结论。

## 验证入口

各局部边界文档和测试是具体证据来源；全局索引由
`editor/tests/test_compatibility_inventory.py` 校验。任何兼容入口的删除都必须同步
更新本文件、局部文档、`API_OWNERSHIP.md` 和对应测试，且不能把底层 `camera()`、
`geometry()`、`Actor`、`Scene` 重新暴露给 Vue、AITool 或普通编辑器 Python。
