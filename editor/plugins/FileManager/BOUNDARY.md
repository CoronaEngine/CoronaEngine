# `FileManager` 插件边界

`FileManager` 是文件树和项目文件操作的聚合 handler；其 canonical owner 是本插件的
聚合 adapter，并通过公共 `files.*` 契约为
编辑器提供文件能力，不是通用文件系统工具、场景权威状态或 Python host owner。其
Python `FileManager` service 仅为旧宿主兼容，正常编辑器启动不自动注册。

## 负责内容

- 当前项目文件树、文件读写、重命名、删除和资源操作的请求编排；
- Python 入口只转发到 native `files.*`；旧 Scene/Actor 文件绑定 adapter 已删除。

## 不负责

- 不定义 `files.*` manifest schema、权限、revision 或公共事件；这些由 C++ manifest
  handler 负责；
- 不创建或持有 Scene/Actor 权威状态；
- 不导入 `CoronaCore`、`backend` 或直接暴露 native engine 对象；
- `editor/backend/file_system/main.py` 仅是历史导入 wrapper，不得新增实现。

## 删除记录

所有仓内文件面板已切换到 `files.*` handler，且没有仓内生产代码调用旧 Scene/Actor
绑定 adapter。`compat/legacy_file_scene_adapter.py` 已删除；后续新增文件事件必须
由 native `files.*` contract 提供，不得恢复新的插件级 Scene/Actor 绑定。

## 删除条件

只有外部旧宿主完成 `FileManager` 历史入口迁移，并通过文件树、读写、重命名和删除
操作的回归测试后，才可删除剩余的历史入口 wrapper。
