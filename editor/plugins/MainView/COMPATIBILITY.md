# `MainView` 迁移边界

`plugins/MainView` 是主视图的项目/场景编排 owner，但仍处于迁移状态。当前 Vue/C++
主流程使用 native aggregate；Python `MainView` service 仅作为旧宿主兼容 adapter，
正常启动不自动注册。它可以协调项目
配置、native 场景列表、Vision 来源、运行项目和公共 manifest 调用；旧 Python Scene 对象
的创建、获取、禁用和删除不属于 MainView 的状态 owner。

仓内生产代码已完成 native 项目/场景生命周期迁移：初始化、创建、切换、保存和删除均
通过 manifest 聚合接口完成。该 legacy adapter 当前无仓内生产调用（具体入口为
`legacy_main_view_scene_adapter`），
仅为关闭和外部宿主兼容保留；关闭和外部宿主兼容仍需确认后，才能删除该 adapter 及其
runtime shim。

## 当前边界

| 能力 | 当前 owner | MainView 允许做什么 | 迁移目标 |
|---|---|---|---|
| 项目/场景列表和项目字段 | C++ `main.on_init` / `main.create_scene` / `main.remove_scene` / `sceneTools.reload_scene`；项目路径由 `projectSettings.*` 提供 | 调用 native 生命周期并编排返回的列表和项目页面状态；不直接读取 `settings_manager` | `project.*` / `scene.*` 聚合契约 |
| 旧 Python Scene 生命周期 | `plugins/MainView/compat/legacy_main_view_scene_adapter.py`；runtime 路径仅为兼容 shim | 仅保留关闭和外部旧宿主兼容；初始化、创建、切换和文件删除已走 native contract | native project/scene lifecycle |
| Vision 来源加载 | `SceneTools` + `sceneTools.*` manifest | 选择来源并调用聚合 handler | native Vision lifecycle 和事件 |
| 场景保存 | `CoronaEditorApi.main.scene_save` | 转发请求并格式化结果 | 保持同一 manifest owner |
| 场景运行和 Blockly 执行 | `CoronaEditorApi.scene.get_snapshot` + `script_runtime.runner` | 编排运行流程 | 保持 Script Runtime 独立边界 |

## 约束

- MainView 不得导入 `CoronaCore` 或 `scene_manager`；
- `get_or_create_scene`、`get_scene`、`discard_scene` 只能来自
  `plugins.MainView.compat.legacy_main_view_scene_adapter`，且不得用于新的初始化、创建/切换/删除流程；
- 场景文件删除必须使用 `CoronaEditorApi.main.remove_scene`，不得在 MainView
  直接调用 `os.remove`；
- MainView 不得把旧 Python Scene/Actor 对象发布为 Vue、普通 Python 或 AITool API；
- 新的场景读写必须使用 `project.*`、`scene.*`、`sceneTools.*` 或 `viewport.*`；
- adapter 保留期间不得复制 manifest schema、revision、事件或新的业务状态机。

## 删除条件

删除 `plugins/MainView/compat/legacy_main_view_scene_adapter.py` 及其 runtime 兼容 shim 前，必须同时满足：

1. native project/scene lifecycle 覆盖首次加载、创建、删除、切换和关闭；当前首次加载、创建、删除和切换已完成，关闭仍待覆盖；
2. Vision 绑定、场景列表和 active/entrance scene 持久化回归通过；
3. 旧宿主和外部脚本不再依赖 Python Scene 对象或兼容事件；
4. MainView、runtime registry、Frontend 首次加载和场景切换回归通过；
5. `API_OWNERSHIP.md` 中的 legacy 行更新并记录回滚路径。
