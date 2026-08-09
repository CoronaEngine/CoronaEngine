# `ProjectSettings` 插件边界

`ProjectSettings` 是当前活动项目设置的公共 adapter；项目设置的事实读取、校验和
持久化归 C++ `projectSettings.*` native handler，后者是该领域的 canonical owner。
Python 入口只保留历史插件注册兼容，不再复制配置解析或文件写入逻辑；正常编辑器
启动不自动注册该 Python service。

## 负责内容

- 将设置读取、保存和场景文件选择转换为 `CoronaEditorApi.project_settings` 请求；
- 保持旧宿主需要的插件类和方法名兼容。

## 不负责

- 不重新定义 `settings_manager`、路径模型、INI 解析或项目生命周期；
- 不持有 Scene/Actor 权威状态，不直接访问 `camera()`、`geometry()` 或 native 对象；
- 不从 `CoronaCore`、`backend` 或 Vue 模块导入实现；
- `editor/backend/project_settings/main.py` 仅是历史导入 wrapper，不得新增业务。

## 删除条件

当外部旧宿主完成 `plugins.ProjectSettings`/`projectSettings.*` 迁移并通过回归后，
才能删除 `editor/backend/project_settings` 兼容入口。
