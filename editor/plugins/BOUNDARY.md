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
| `AITool/utils/` | removed compatibility code | 配置统一由 `configuration/` 负责，媒体统一由 `services/media_storage.py` 负责 | 已删除 |
| `AITool/Quasar/` | external submodule | 外部 AI 子模块内容 | 本次迁移不修改；按其自身 upstream 生命周期处理 |
| `FileManager/` | native aggregate + service facade | `files.*` 负责文件事实和操作；`main.py` 仅转发公共 contract，旧 Scene/Actor 文件绑定 adapter 已删除 | 外部旧宿主应迁移到 `files.*`，不得重新引入文件插件兼容层 |
| `MainView/` | native lifecycle + service adapter | native `main.*` 负责项目/场景生命周期；`main.py` 只做 service 编排，不持有 Python Scene 生命周期 | 保持 adapter 与 manifest 一致 |
| `ProjectArchive/` | archive migration facade | `main.py` 负责旧归档请求编排，实际 parser 归 `runtime/archive` | 外部旧宿主迁移且 `project.migrateLegacyScene` 回归通过后删除 facade |
| `ProjectLauncher/` | native project flow + service adapter | native `project.*` 负责项目生命周期和 ProjectCopy；`main.py` 只做 service 编排 | 保持 adapter 与 manifest 一致 |
| ProjectLauncher Python ProjectCopy wrapper | removed compatibility code | 已迁移到 `project.copyExistingToData`，插件层不拥有复制实现 | 如发现外部插件 import，按 aggregate contract 迁移 |
| `ProjectSettings/` | native aggregate + service adapter | native `projectSettings.*` 负责设置事实；`main.py` 只做 service 编排 | 保持 adapter 与 manifest 一致 |
| `SceneTools/` | native aggregate + service adapter | Vue/C++/Script Runtime 使用 native manifest 聚合接口；Python 只做参数转换和 Vision 编排 | 保持 adapter 与 manifest 一致 |
| `SceneTools/compat/legacy_vision_import_helper.py` 与旧 Vision adapters | removed compatibility code | 无仓内生产调用；受支持流程由 canonical `SceneTools/vision_import.py` 和 native Vision manifest 负责 | 如发现外部依赖，按 aggregate contract 迁移 |

## 依赖方向

- `runtime/registry.py` 是唯一插件注册、初始化和关闭 owner；插件不得自行注册第二套
  host lifecycle；
- 插件通过 `api.editor_api`/manifest 聚合接口访问编辑器语义，不得直接暴露
  `camera()`、`geometry()`、`Actor`、`Scene` 等底层对象给 Vue 或普通 Python；
- AITool 的 API key 只能由 `AITool/configuration` 从 `.env`/受控本地配置读取，不能写入
  插件源码、Vue bundle、日志或通用 service；
- `SceneTools`、`MainView` 和 `ProjectArchive` 不得重新引入 Python Scene 事实或
  compatibility 实现；场景操作必须走 manifest；
- `AITool/Quasar` 是外部子模块，明确不修改其内容、不把其内部目录加入本仓迁移 owner；
- 新增插件功能必须先确定公共 manifest/service owner、生命周期、测试 owner 和删除条件。

## 删除条件与验证

边界由各插件本地测试、`editor/runtime/tests` 和本目录边界测试共同验证。删除或移动
插件目录前必须确认 runtime registry、Vue/外部宿主调用方、配置/文件持久化和回退路径；
仅凭仓内搜索不到调用方不能直接删除。
