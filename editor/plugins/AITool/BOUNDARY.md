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
| `cai_extensions/flows/terrain_generation_workflow/paths.py` | 地形生成物路径 owner | 将地形输出归属到活动项目 `Resource/generated/terrain`，保留显式 `output_base` | 不定义地形生成算法或跨层 API |
| `cai_extensions/flows/multi_scene_parallel_workflow/paths.py` | 多场景生成物路径 owner | 将子场景输出归属到活动项目 `Resource/generated/multi_scene`，保留显式父目录 | 不定义场景拆解、模型生成或跨层 API |
| `cai_extensions/scene_placement/paths.py` | 场景放置中间物路径 owner | 将布局 JSON 和下载模型归属到活动项目 `Resource/generated/scene_placement`；绝对配置路径保持显式覆盖 | 不写入 native `Scene/*.scene`，不负责引擎场景状态 |
| `cai_extensions/flows/model_retrieval_workflow/model_library_paths.py` | 本地模型库路径 owner | 新模型库写入活动项目 `Resource/local_model_library`，旧 `assets/local_model_library` 仅作读取兼容 | 不把模型库写入仓库源码或 Quasar 默认目录 |
| `cai_extensions/agent/scene_composer.py:_generated_asset_project_root` | SceneComposer 生成资产根路径 owner | 使用活动项目的 `Resource/generated/scene_composer` | 不复制 project context 或使用源码目录作为业务输出目录 |
| `cai_extensions/agent/scene_composer.py` 导入后处理 | SceneComposer Actor 变换和物理后处理编排 | 通过 native-first Actor value object 获取视图；native 写入走聚合契约，旧宿主 fallback 正在收口 | 不直接导入 legacy Scene/manager，不定义第二套场景或物理 schema |
| `compat/legacy_local_ai_setting.py`、`compat/legacy_aitool_utils.py`、`compat/legacy_image_utils.py` | removed compatibility code | 仓内无生产引用；配置统一由 `configuration/` 负责，媒体统一由 `services/media_storage.py` 负责 | 已删除；外部旧 import 如仍存在，应迁移到 canonical owner |
| `utils/` | historical import facade | 继续提供旧包路径，但内部直接导入 configuration/service canonical owner | 不得再依赖 `compat/`，外部旧 import 迁移后删除 |
| `Quasar/` | 外部子模块 | 上游 AI runtime 内容 | 本仓迁移不修改 |

## 依赖与生命周期

- `runtime/registry.py` 负责注册、后台初始化和关闭；插件不得自行创建第二套 host
  lifecycle。
- 引擎场景只能通过 `api.editor_api`、`native_scene_state` 或登记的 legacy adapter
  访问；不得直接导入 `CoronaCore`、native `Actor`、`Scene` 或底层 binding。
- 场景列表统一通过 native `scene.list_routes`，不得从 legacy Scene store 枚举路由。
- Agent Runtime 的相对资源路径先解析到 `runtime.project_context` 的活动项目根目录，旧
  `cwd` 相对路径仅作为兼容回退；绝对路径不重定位。
- API key 只能由 `configuration/` 从 `.env` 或受控本地配置读取，日志不得输出秘钥。
- 基础 API 在 runtime ready 前必须保留 `initializing` 语义；初始化失败进入
  `degraded`，不得无界重试。

## 删除条件

服务调用方已迁移到明确的 runtime/service owner；本轮已删除仓内无生产引用的旧配置和
媒体 wrapper。AITool 的 scene routes、camera tools 已使用 native contract；Actor 和
Vision 场景 fallback 仍需等待 native 场景覆盖后再删除；Quasar 子模块按其自身 upstream
生命周期处理。
