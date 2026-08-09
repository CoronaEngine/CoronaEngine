# `editor/api` 边界

`editor/api` 是 Python 对 C++ manifest 的唯一公共契约 adapter。它映射参数、返回值、
事件和 caller 权限，不持有场景事实，也不是传统 Web backend。

负责提供 `CoronaEditorApi.scene`、`scene_tools`、`viewport`、`project`、`files`、
`network` 和 `lan_chat` 等聚合 namespace，并规范化 LANChat 的 value object。

不负责定义第二份 schema、实现 Actor/Scene/Camera/Geometry 底层操作，或管理 host 生命周期。
Vue、AITool 和普通 Python 不得绕过该入口取得底层 binding。旧外部 import 兼容已放弃，
所有调用方必须使用 manifest contract。
