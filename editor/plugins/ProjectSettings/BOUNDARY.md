# `ProjectSettings` 插件边界

`ProjectSettings` 是当前活动项目设置的公共 adapter；项目设置的事实读取、校验和
持久化归 C++ `projectSettings.*` native handler，后者是该领域的 canonical owner。
Python 入口只负责把请求转发到 manifest，不复制配置解析或文件写入逻辑。

## 负责内容

- 将设置读取、保存和场景文件选择转换为 `CoronaEditorApi.project_settings` 请求；

## 不负责

- 不重新定义 `settings_manager`、路径模型、INI 解析或项目生命周期；
- 不持有 Scene/Actor 权威状态，不直接访问 `camera()`、`geometry()` 或 native 对象；
- 不从 Vue 模块或底层 Engine 对象导入实现。

删除条件：旧外部入口不再受支持；后续能力统一扩展 `projectSettings.*` manifest。
