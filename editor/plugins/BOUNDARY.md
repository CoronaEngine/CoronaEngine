# `editor/plugins` 边界

`editor/plugins` 只承载按 Python service 注册的编辑器业务插件。插件可以组合
`editor/api`、`editor/runtime` 和对应的 host support，但不能成为 C++ manifest、
Vue transport、引擎对象或角色脚本运行时的第二个 owner。

## 插件职责

| 目录 | 类型 | canonical 职责 | 删除/迁移条件 |
|---|---|---|---|
| `AITool/` | active AI/Agent | AI 对话、Agent、模型调用、场景生成和节点图工作流；配置 owner 在 `configuration/` | AITool 业务迁移到明确 runtime/service owner 后删除；Quasar 不在本次整理范围 |
| `AITool/configuration/` | active config owner | `.env`、API key 和本地 AITool 配置加载 | 所有调用方迁移到新的 secret/config owner 后删除 |
| `AITool/cai_extensions/` | active AI extension | Agent、MCP 工具、场景组合和生成 workflow 扩展 | 对应 workflow/runtime 完整替代后删除 |
| `AITool/services/` | active orchestration | 编辑器侧 AI 请求、对话、协作、生成和 Agent runtime 报告编排 | 对应 service owner 完整替代并通过集成回归 |
| `AITool/utils/` | compatibility | 历史配置和媒体 helper 导入 wrapper；不得新增通用业务 | 外部旧 import 迁移后删除 |
| `AITool/Quasar/` | external submodule | 外部 AI 子模块内容 | 本次迁移不修改；按其自身 upstream 生命周期处理 |
| `FileManager/` | native aggregate + legacy compatibility facade | `files.*` 负责文件事实和操作；`compat/legacy_file_manager.py` 仅保留旧 Python service 入口 | 外部旧宿主迁移后删除 facade |
| `MainView/` | native lifecycle + legacy compatibility facade | native `main.*` 负责项目/场景生命周期；`compat/legacy_main_view.py` 仅保留旧 Python service 编排 | 外部旧宿主和旧 Scene adapter 迁移后删除 facade |
| `ProjectArchive/` | compatibility facade | 旧归档插件入口；实际 parser 归 `runtime/archive` | 旧宿主迁移且 facade 回归通过后删除 |
| `ProjectLauncher/` | native project flow + legacy compatibility facade | native `project.*` 负责项目生命周期；`compat/legacy_project_launcher.py` 仅保留旧 Python service 入口 | 外部旧宿主迁移后删除 facade |
| `ProjectLauncher/compat/legacy_project_copy.py`（旧入口：`project_copy.py`、`utils/project_copy.py`） | compatibility wrapper | 历史 ProjectLauncher import 路径集中转发到 `runtime.compat.legacy_project_copy` | 外部旧 import 迁移后删除 |
| `ProjectSettings/` | native aggregate + legacy compatibility facade | native `projectSettings.*` 负责设置事实；`compat/legacy_project_settings.py` 仅保留旧 Python service 入口 | 外部旧宿主迁移后删除 facade |
| `SceneDatas/` | compatibility-only | 旧 Object 面板插件壳和宿主注册点 | 旧宿主/脚本迁移并完成回归后删除 |
| `SceneTools/` | native aggregate + legacy compatibility facade | Vue/C++/Script Runtime 使用 native manifest 聚合接口；Python facade 仅服务 Vision 旧宿主 | Vision legacy host 迁移、native lifecycle handler 完整替代且回归通过后删除 |
| `SceneTools/compat/legacy_vision_import_helper.py`（旧入口：`SceneTools/vision_import.py`） | removed compatibility code | 无仓内生产调用；受支持流程由 native Vision manifest 和 `legacy_vision_import_adapter.py` 负责 | 如发现外部依赖，revert 删除提交后按契约迁移 |

## 依赖方向

- `runtime/registry.py` 是唯一插件注册、初始化和关闭 owner；插件不得自行注册第二套
  host lifecycle；
- 插件通过 `api.editor_api`/manifest 聚合接口访问编辑器语义，不得直接暴露
  `camera()`、`geometry()`、`Actor`、`Scene` 等底层对象给 Vue 或普通 Python；
- AITool 的 API key 只能由 `AITool/configuration` 从 `.env`/受控本地配置读取，不能写入
  插件源码、Vue bundle、日志或通用 service；
- `SceneTools`、`MainView` 和 `ProjectArchive` 的旧 Scene 兼容逻辑只能经登记的
  `runtime` adapter 使用，不得重新把 compatibility 实现扩散到其它插件；
- `AITool/Quasar` 是外部子模块，明确不修改其内容、不把其内部目录加入本仓迁移 owner；
- 新增插件功能必须先确定公共 manifest/service owner、生命周期、测试 owner 和删除条件。

## 删除条件与验证

边界由各插件本地测试、`editor/runtime/tests` 和本目录边界测试共同验证。删除或移动
插件目录前必须确认 runtime registry、Vue/外部宿主调用方、配置/文件持久化和回退路径；
仅凭仓内搜索不到调用方不能直接删除。
