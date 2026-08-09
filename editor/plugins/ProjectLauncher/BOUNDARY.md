# `ProjectLauncher` 插件边界

`ProjectLauncher` 是项目流程编排的 canonical owner，负责项目创建、打开、复制和迁移
流程的 UI/宿主编排。当前 Vue/C++ 主流程使用 `project.*` native aggregate；Python
`ProjectLauncher` service 位于 `main.py`，仅作为显式旧宿主 adapter。项目公共语义
归 `project.*` manifest；项目模板和路径 helper 分别归 `runtime.project_templates`
与 `config`，本插件不拥有第二套项目 schema。

## 子目录职责

| 路径 | 负责内容 | 不负责 |
|---|---|---|
| `main.py` | 旧宿主 service adapter；将历史项目操作转发到 `CoronaEditorApi.project` | 不直接创建 C++ Scene/Actor，不定义第二套公共契约；不作为 Vue/native 启动入口 |
| `runtime/project_templates.py` | 项目模板复制、路径规范化和 INI 初始化 helper owner | 不维护新的项目生命周期或公共 schema |
| `project.copyExistingToData` | native aggregate；C++ handler 负责已有项目复制、重名、路径规范化和失败回滚 | 不暴露 Engine 对象，不在 Python 重建复制实现 |
| `editor/data/` | native 项目复制的 runtime data 目标目录 | 不作为模板 source 或 native 项目状态 owner |

项目创建、打开和场景初始化的权威状态由 C++/runtime host 管理；Python 只负责请求
转换、模板 helper 和兼容编排。不得直接导入 `CoronaCore` 或 `backend`。

## 删除条件

`ProjectLauncher` service 和 ProjectCopy 已完成 owner 迁移；旧 service 专用 shim 与
`runtime/legacy_project_copy.py` 均已删除。后续旧宿主应迁移到
`project.copyExistingToData`，不得重新增加 Python 复制实现。
