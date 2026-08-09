# `FileManager` 插件边界

`FileManager` 是文件树和项目文件操作的聚合 handler；其 canonical owner 是本插件的
聚合 adapter，并通过公共 `files.*` 契约为
编辑器提供文件能力，不是通用文件系统工具、场景权威状态或 Python host owner。

## 负责内容

- 当前项目文件树、文件读写、重命名、删除和资源操作的请求编排；
- Python 入口只转发到 native `files.*`；旧 Scene/Actor 文件绑定 adapter 已删除。

## 不负责

- 不定义 `files.*` manifest schema、权限、revision 或公共事件；这些由 C++ manifest
  handler 负责；
- 不创建或持有 Scene/Actor 权威状态；
- 不直接暴露 native engine 对象。

## 迁移记录

所有仓内文件面板已切换到 `files.*` handler，且没有仓内生产代码调用旧 Scene/Actor
绑定 adapter。`compat/legacy_file_scene_adapter.py` 已删除；后续新增文件事件必须
由 native `files.*` contract 提供，不得恢复新的插件级 Scene/Actor 绑定。

删除条件：无旧 wrapper 或外部兼容入口需要保留；后续维护只扩展 native `files.*` contract。
