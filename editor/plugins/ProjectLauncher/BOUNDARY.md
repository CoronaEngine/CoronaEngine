# `ProjectLauncher` 插件边界

`ProjectLauncher` 是项目流程编排的 canonical owner，负责项目创建、打开、复制和迁移
流程的 UI/宿主编排。当前 Vue/C++ 主流程使用 `project.*` native aggregate；Python
`ProjectLauncher` service 仅作为旧宿主 adapter，正常启动不自动注册。项目公共语义
归 `project.*` manifest；项目模板和路径 helper 分别归 `runtime.project_templates`
与 `config`，本插件不拥有第二套项目 schema。

## 子目录职责

| 路径 | 负责内容 | 不负责 |
|---|---|---|
| `compat/legacy_project_launcher.py` | 旧宿主注册入口、最近项目和项目操作请求转发 | 不直接创建 C++ Scene/Actor，不定义公共契约；不作为 Vue/native 启动入口 |
| `main.py` | 历史导入 shim | 不承载项目业务实现 |
| `runtime/project_templates.py` | 项目模板复制、路径规范化和 INI 初始化 helper owner | 不维护新的项目生命周期或公共 schema |
| `runtime/compat/legacy_project_support.py` | 历史 project helper 兼容 facade owner | 不新增项目 helper 实现；`runtime/project_support.py` 仅为旧路径 shim |
| `runtime/legacy_project_copy.py` | 历史项目复制和打开 facade 的实现 owner | 不维护新的项目生命周期或公共 schema |
| `runtime/compat/legacy_project_copy.py` | legacy ProjectCopy 的兼容实现 owner | 不新增复制逻辑 |
| `runtime/project_copy.py` | legacy ProjectCopy 的历史 import shim | 不新增复制逻辑 |
| `compat/legacy_project_copy.py` | ProjectLauncher 历史复制入口的兼容 wrapper，转发到 `runtime.compat.legacy_project_copy` | 不新增复制逻辑 |
| `editor/data/` | legacy 项目复制的 runtime data 目标目录 | 不作为模板 source 或 native 项目状态 owner |
| `project_copy.py` | 历史插件导入兼容 wrapper，转发到 `compat/legacy_project_copy.py` | 不新增复制逻辑 |
| `utils/project_copy.py` | 更早的历史导入兼容 wrapper，转发到 `compat/legacy_project_copy.py` | 不新增复制逻辑 |

项目创建、打开和场景初始化的权威状态由 C++/runtime host 管理；Python 只负责请求
转换、模板 helper 和兼容编排。不得直接导入 `CoronaCore` 或 `backend`。

## 删除条件

当 `project.*` handler 覆盖项目创建、打开、复制、迁移和错误回滚，并完成旧宿主与
发布项目回归后，才能删除旧 `utils` wrapper 或迁移期编排代码。旧宿主若仍需要
Python service，必须显式使用 runtime registry 的 legacy registration。
