# `editor/api` 边界

`editor/api` 是编辑器 Python 的公共契约 adapter。它把 C++ manifest 的方法、参数、
返回值、事件和 caller 权限映射为 `CoronaEditorApi`，不拥有引擎场景状态，也不是
传统 Web backend。

## 负责内容

- 校验并调用 C++ manifest/schema；
- 提供 `CoronaEditorApi.scene`、`scene_tools`、`viewport`、`project`、`files`、
  `network` 和 `lan_chat` 等公共语义 namespace；
- 作为 adapter 工厂边界，优先返回 manifest contract；当旧宿主只提供旧 native
  binding 时，返回 `runtime/legacy_*_adapters.py` 的兼容视图。

## 不负责

- 不定义第二份 schema、权限、revision、事件协议或场景事实；
- 不直接实现 `Actor`、`Scene`、`Camera`、`Geometry` 等引擎对象操作；
- 不持有宿主生命周期、活动项目兼容状态、网络 fallback 或旧 native adapter 实现；
  这些分别归 `runtime` 的对应 owner；
- 不允许 Vue、AITool 或普通 Python 绕过公共 contract 直接取得底层 binding。

Scene/Viewport 工厂保留在本目录，是因为它们必须在同一个入口选择公共 manifest contract
和 legacy fallback；具体 fallback 实现位于 `runtime/legacy_scene_adapters.py`，runtime
不得反向导入本模块来取得公共 API。

## 删除条件

只有当所有调用方完成 manifest contract 迁移、旧宿主和测试替身不再需要 fallback、且
对应 adapter 回归通过后，才能删除旧兼容工厂或缩减其导出。公共 `CoronaEditorApi` 的
契约 owner 在此期间保持唯一。
