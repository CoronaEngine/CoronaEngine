# `FileManager` 插件边界

`FileManager` 是文件树和项目文件操作的聚合 handler；其 canonical owner 是本插件的
聚合 adapter，并通过公共 `files.*` 契约为
编辑器提供文件能力，不是通用文件系统工具、场景权威状态或 Python host owner。其
Python `FileManager` service 仅为旧宿主兼容，正常编辑器启动不自动注册。

## 负责内容

- 当前项目文件树、文件读写、重命名、删除和资源操作的请求编排；
- Python 入口只转发到 native `files.*`；`compat/legacy_file_scene_adapter.py` 保留外部旧宿主兼容事件，
  `runtime/legacy_file_scene_adapter.py` 仅为历史导入 shim。

## 不负责

- 不定义 `files.*` manifest schema、权限、revision 或公共事件；这些由 C++ manifest
  handler 负责；
- 不创建或持有 Scene/Actor 权威状态；
- 不导入 `CoronaCore`、`backend` 或直接暴露 native engine 对象；
- `editor/backend/file_system/main.py` 仅是历史导入 wrapper，不得新增实现。

## 删除条件

当所有支持的文件面板切换到 `files.*` handler、兼容事件由公共 owner 覆盖并通过文件
面板和项目切换回归后，才可删除 legacy file adapter 或旧导入路径。
