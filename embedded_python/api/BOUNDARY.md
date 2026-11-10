# `editor/api` 边界

`editor/api` 只剩一个职责：向 Script Runtime 提供受限的编辑器契约。它映射 16 条
`CoronaEngine.script_scene_*` 具名绑定，不持有场景事实，也不是传统 Web backend。

提供 `get_script_runtime_editor_api()`，返回 `scene` / `scene_tools` / `viewport`
三个受限 namespace。普通 Python 没有编辑器 API。

不负责定义第二份 schema、实现 Actor/Scene/Camera/Geometry 底层操作，或管理 host 生命周期。
Web 走 C++ typed V8 具名契约，Script Runtime 走 `script_scene_*`，两者都不经 Python。
