# `AITool` 插件边界

`AITool` 是编辑器内嵌的 AI/Agent 与生成工作流插件。它是业务编排层，不是 C++
引擎对象、Vue transport、Python host lifecycle 或公共 manifest 的 owner。

## 子目录职责

| 路径 | canonical owner | 负责内容 | 不负责 |
|---|---|---|---|
| `configuration/` | AITool configuration owner | `.env`、API key 和本地配置读取 | 不保存密钥到源码、日志或 Vue bundle |
| `services/` | AITool service owner | 对话、协作、Agent runtime、生成请求和报告编排 | 不直接操作 `Actor`/`Scene`，不定义公共 manifest |
| `services/media_storage.py` | AITool 媒体落盘 owner | 默认将输入媒体写入活动项目 `media`，显式输出路径保持调用方控制 | 不依赖进程 `cwd`，不写入仓库 `uploads/` |
| `cai_extensions/` | AITool integration owner | Quasar 集成、Agent、MCP 工具和生成 workflow | 不成为引擎底层 API 或场景权威状态 owner |
| `compat/`、`utils/` | removed compatibility code | 目录和旧导入入口已删除；配置统一由 `configuration/` 负责，媒体统一由 `services/media_storage.py` 负责 | 不得重新创建旧入口 |
| `Quasar/` | 外部子模块 | 上游 AI runtime 内容 | 本仓迁移不修改 |

## 依赖与生命周期

- `runtime/registry.py` 负责注册、后台初始化和关闭；插件不得自行创建第二套 host
  lifecycle。
- 引擎场景只能通过 `api.editor_api` 或 `native_scene_state`
  访问；不得直接导入 `CoronaCore`、native `Actor`、`Scene` 或底层 binding。
- 场景列表统一通过 native `scene.list_routes`，不得从 legacy Scene store 枚举路由。
- Agent Runtime 的相对资源路径先解析到 `runtime.project_context` 的活动项目根目录；
  绝对路径不重定位。
- API key 只能由 `configuration/` 从 `.env` 或受控本地配置读取，日志不得输出秘钥。
- 基础 API 在 runtime ready 前必须保留 `initializing` 语义；初始化失败进入
  `degraded`，不得无界重试。

## 删除条件

服务调用方已迁移到明确的 runtime/service owner；仓内无生产引用的旧配置、媒体和导入
wrapper 已删除。AITool 的 scene routes、camera tools、Actor 和 Vision 场景访问均使用
native contract；Quasar 子模块按其自身 upstream 生命周期处理。
