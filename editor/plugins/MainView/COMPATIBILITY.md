# `MainView` 迁移边界

`plugins/MainView` 是主视图的项目/场景编排 owner。当前 Vue/C++
主流程使用 native aggregate；Python `MainView` service 位于 `main.py`，仅作为旧宿主兼容 adapter，
正常启动不自动注册。它可以协调项目
配置、native 场景列表、Vision 来源、运行项目和公共 manifest 调用；旧 Python Scene 对象
的创建、获取、禁用和删除不属于 MainView 的状态 owner。

仓内生产代码已完成 native 项目/场景生命周期迁移：初始化、创建、切换、保存和删除均
通过 manifest 聚合接口完成。旧 Python Scene 生命周期 adapter 已删除；仓内没有该 adapter 的生产调用方。
MainView 不再持有或转发旧 Python Scene 生命周期。

## 当前边界

| 能力 | 当前 owner | MainView 允许做什么 | 迁移目标 |
|---|---|---|---|
| 项目/场景列表和项目字段 | C++ `main.on_init` / `main.create_scene` / `main.remove_scene` / `sceneTools.reload_scene`；项目路径由 `projectSettings.*` 提供 | 调用 native 生命周期并编排返回的列表和项目页面状态；不直接读取 `settings_manager` | `project.*` / `scene.*` 聚合契约 |
| 旧 Python Scene 生命周期 | 已删除的兼容代码 | 仓内无生产调用方；所有受支持流程使用 native lifecycle | `main.on_init`、`main.create_scene`、`main.remove_scene` 和 `sceneTools.reload_scene` |
| Vision 来源加载 | `SceneTools` + `sceneTools.*` manifest | 选择来源并调用聚合 handler | native Vision lifecycle 和事件 |
| 场景保存 | `CoronaEditorApi.main.scene_save` | 转发请求并格式化结果 | 保持同一 manifest owner |
| 场景运行和 Blockly 执行 | `CoronaEditorApi.scene.get_snapshot` + `script_runtime.runner` | 编排运行流程 | 保持 Script Runtime 独立边界 |

## 约束

- MainView 不得导入 `CoronaCore` 或 `scene_manager`；
- MainView 不得重新引入 `get_or_create_scene`、`get_scene` 或 `discard_scene` 等旧 Python Scene 生命周期入口；
- 场景文件删除必须使用 `CoronaEditorApi.main.remove_scene`，不得在 MainView
  直接调用 `os.remove`；
- MainView 不得把旧 Python Scene/Actor 对象发布为 Vue、普通 Python 或 AITool API；
- 新的场景读写必须使用 `project.*`、`scene.*`、`sceneTools.*` 或 `viewport.*`；
- adapter 保留期间不得复制 manifest schema、revision、事件或新的业务状态机。

## 删除记录

`compat/legacy_main_view.py` 和旧 Python Scene 生命周期 adapter 均已删除。仓内生产调用方
已经迁移到 native project/scene lifecycle，并由 MainView、runtime registry、Frontend
和场景切换边界测试覆盖。后续若外部宿主仍依赖旧模块，应按 native manifest 契约迁移，
不得恢复新的插件级兼容实现。
