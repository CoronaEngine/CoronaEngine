# Corona Engine Editor

`editor` 是 Corona Engine 的编辑器运行时和 UI 工程。它不是一个独立的 Web 后端：
Python 运行在引擎进程内，负责 AI、Agent、角色脚本、Blockly/Scratch 和自动化任务；
C++ 持有场景和引擎状态的权威事实。

开始阅读前，先看：

- [ARCHITECTURE.md](ARCHITECTURE.md)：层次、目录职责和依赖方向。
- [API_OWNERSHIP.md](API_OWNERSHIP.md)：公共业务语义的 owner、调用方和迁移状态。
- [backend/COMPATIBILITY.md](backend/COMPATIBILITY.md)：历史 backend 入口的 owner、保留范围和删除条件。
- [CoronaCore/COMPATIBILITY.md](CoronaCore/COMPATIBILITY.md)：历史 CoronaCore 入口的 owner、保留范围和删除条件。
- [CoronaPlugin/COMPATIBILITY.md](CoronaPlugin/COMPATIBILITY.md)、[utils/COMPATIBILITY.md](utils/COMPATIBILITY.md)、[scripts/COMPATIBILITY.md](scripts/COMPATIBILITY.md)：小型历史兼容入口的 owner 和删除条件。
- [plugins/SceneDatas/COMPATIBILITY.md](plugins/SceneDatas/COMPATIBILITY.md)：旧 Object 面板插件壳的边界和删除条件。
- [plugins/MainView/COMPATIBILITY.md](plugins/MainView/COMPATIBILITY.md)：主视图编排与旧 Python Scene 生命周期的迁移边界。
- [plugins/ProjectArchive/BOUNDARY.md](plugins/ProjectArchive/BOUNDARY.md)：归档 parser 与迁移 facade 的职责边界。
- [plugins/SceneTools/BOUNDARY.md](plugins/SceneTools/BOUNDARY.md)：聚合 handler、Vision fallback 和未引用 helper 的边界。
- [Frontend/src/compat/BOUNDARY.md](Frontend/src/compat/BOUNDARY.md)：Frontend API/service 与旧 CEF 兼容入口的边界。
- [Frontend/src/BOUNDARY.md](Frontend/src/BOUNDARY.md)：Frontend `src` 顶层目录职责和依赖方向总清单。
- [Frontend/src/services/BOUNDARY.md](Frontend/src/services/BOUNDARY.md)：Frontend manifest facade、节点图和 Cabbage service 的分类边界。
- [Frontend/src/utils/BOUNDARY.md](Frontend/src/utils/BOUNDARY.md)：Frontend 低延迟输入 adapter 与历史 bridge wrapper 的边界。
- [Frontend/src/blockly/BOUNDARY.md](Frontend/src/blockly/BOUNDARY.md)：Blockly/Scratch、节点图 UI、代码生成器和 Script Runtime 的边界。
- [runtime/BOUNDARY.md](runtime/BOUNDARY.md)：编辑器 Python host、插件 registry 和 legacy adapter 的边界。
- [script_runtime/BOUNDARY.md](script_runtime/BOUNDARY.md)：受限角色脚本、Blockly 执行和 manifest adapter 的边界。
- [plugins/BOUNDARY.md](plugins/BOUNDARY.md)：编辑器业务插件、AITool 子目录、兼容 facade 和外部 Quasar 子模块的边界。
- [COMPATIBILITY.md](COMPATIBILITY.md)：所有历史兼容入口的 canonical owner、风险和删除顺序总索引。
- [config/BOUNDARY.md](config/BOUNDARY.md)：路径、活动项目和应用运行时配置的 owner 边界。

这三份文档的优先级是：代码中的 manifest/schema 定义可调用契约；`ARCHITECTURE.md`
定义层次和依赖方向；本文件只提供入口、目录导航和运行方式。文档之间出现冲突时，
以 manifest/schema 为准，并在同一变更中修正文档。

## 运行时分工

| 部分 | 主要职责 | 不应承担的职责 |
|---|---|---|
| `Frontend` | Vue 页面、面板、输入、临时 UI 状态和结果展示 | 场景权威状态、AI 业务、引擎对象操作 |
| C++ Engine/Editor Host | 场景、Actor、资源、变换、渲染、revision、事件和跨层聚合接口 | 把底层 `camera()`、`geometry()` 直接作为上层业务 API |
| Embedded Python | AI/Agent、生成工作流、角色脚本、Blockly/Scratch 和自动化 | 取代 C++ 保存场景事实或充当传统 HTTP 后端 |
| Script Runtime | 受权限控制的角色脚本和积木运行时能力 | 访问编辑器全部 API、网络或 AI 能力 |

跨层编辑器操作必须经过 manifest/schema 定义的聚合契约。该契约由 C++ Editor Host
发布，业务 owner 可以由 C++ Domain Service 或受控的 Python service handler 承担；
Vue 和 Python 中的方法只是同一契约的薄 adapter，不得各自定义参数、校验、错误码或
状态机。

例如摄像机锁定统一使用 `sceneTools.setActorCameraLock`；旧 `sceneDatas.actorOperation`
仅用于兼容未迁移宿主，不作为新 Vue/Python 代码的调用模板。

## 目录概览

| 目录 | 当前定位 | 生命周期/边界 |
|---|---|---|
| `Frontend` | Vue 页面、输入交互和 JS 公共契约 adapter | `active`；`src/api` 是 manifest contract owner，`src/services/BOUNDARY.md` 分类领域 facade，`src/compat/BOUNDARY.md` 登记旧 CEF 入口，`src/utils/viewport*.js` 是低延迟输入 adapter，`node_modules`/`dist` 是生成内容，不属于源码架构 |
| `Frontend/tests` | Frontend 测试总目录 | `active / tests`；Python 边界测试位于 `tests/python`，Node 单元测试位于 `tests/js`，生产 `src` 不混放测试 |
| `main.py` | 嵌入式 Python runtime 启动入口 | `active / host-entry`；只负责初始化 `runtime.editor_host`、注册服务和安装 dispatcher，不承载插件业务 |
| `api` | Python 对 C++ manifest 的唯一公共契约 adapter | `active / public-contract`；不定义第二套 schema 或业务状态机 |
| `CoronaCore` | Engine/Script adapter 和集中 legacy 边界的历史兼容包 | `compatibility`；详见 [`CoronaCore/COMPATIBILITY.md`](CoronaCore/COMPATIBILITY.md)，`core/legacy` 只供 Script Runtime/旧宿主，普通编辑器业务不得直接导入 |
| `runtime` | 嵌入式 Python 服务注册、生命周期、宿主编排和运行时生成数据 owner | `active / host`；不承载场景业务；`runtime/generated` 只存放运行时生成物 |
| `backend` | 历史 Blockly/脚本兼容入口和旧生成输出回退路径 | `compatibility`；详见 [`backend/COMPATIBILITY.md`](backend/COMPATIBILITY.md)，canonical 编排位于 `script_runtime`，生成物写入 `runtime/generated`，名称不代表传统 Web 后端 |
| `config` | 编辑器路径、项目设置、环境和运行配置解析 | `active`；不持有场景事实或业务 API |
| `runtime/plugin_base.py` | 显式注册 Python 插件的基础兼容基类 | `active / runtime-infrastructure`；唯一实现 owner；`CoronaPlugin/compat` 集中保留兼容转发，旧 `core`/`utils` 路径仅为 shim |
| `CoronaPlugin` | Python 插件历史导入兼容包 | `compatibility`；详见 [`CoronaPlugin/COMPATIBILITY.md`](CoronaPlugin/COMPATIBILITY.md)，只保留 wrapper，不定义场景业务 schema |
| `plugins/AITool` | AI、Agent、模型调用和生成工作流 | `active`；通过 `api.editor_api` 和受限 value-object adapter 取得场景值对象，不直接依赖 `CoronaCore` legacy 对象 |
| `plugins/SceneTools` | 场景、视口和资源聚合 handler 的 Python 实现 | `active / aggregate owner`；详见 [`plugins/SceneTools/BOUNDARY.md`](plugins/SceneTools/BOUNDARY.md)，通过 `editor_api` 使用 manifest；Vision 旧场景解析由 `plugins/SceneTools/compat/legacy_vision_scene_adapter.py` 承担；旧宿主仍可使用 native `sceneTools.create_scene`，MainView 场景生命周期归 `main.create_scene` |
| `plugins/MainView` | 项目/场景启动和主视图宿主 adapter | `active / aggregate-adapter`；项目上下文、初始化、创建、切换和删除使用 native contract，关闭兼容仍登记在 [`plugins/MainView/COMPATIBILITY.md`](plugins/MainView/COMPATIBILITY.md)；不得直接访问 scene store |
| `plugins/ProjectLauncher` | 项目创建、打开和迁移流程 | `active`；公共语义归属 `project.*` |
| `plugins/ProjectArchive` | 项目归档迁移 facade | `active / compatibility-boundary`；实现位于 `main.py`；解析归属 `runtime.archive`，迁移语义归属 `project.migrateLegacyScene` |
| `plugins/FileManager` | 文件树、文件操作和项目文件聚合 adapter | `active / aggregate-owner`；文件事实和路径校验归 native `files.*`，service adapter 位于 `main.py`；旧宿主事件由 `plugins/FileManager/compat/legacy_file_scene_adapter.py` 保留，runtime 路径仅为 shim |
| `plugins/ProjectSettings` | 当前项目设置公共 adapter | `active / aggregate-adapter`；事实读取、校验和保存归 C++ `projectSettings.*`，Python 仅保留兼容转发 |
| `plugins/SceneDatas` | 旧 Object 面板的 Python 兼容服务壳 | `compatibility-only`；正常启动不自动注册，旧宿主通过显式 legacy registration 使用；详见 [`plugins/SceneDatas/COMPATIBILITY.md`](plugins/SceneDatas/COMPATIBILITY.md) |
| `scripts` | 历史编辑器脚本兼容目录 | `compatibility`；详见 [`scripts/COMPATIBILITY.md`](scripts/COMPATIBILITY.md)，打包实现已归入仓库顶层 `tools/pack.py` |
| `utils` | `runtime` 与 `config` 的历史导入兼容转发 | `compatibility`；详见 [`utils/COMPATIBILITY.md`](utils/COMPATIBILITY.md)，新代码使用 canonical owner，不得新增业务 |

路径说明：`editor/backend` 是编辑器仓库内的历史兼容 Python 包；`runtime/generated`
是 Script Runtime 的 Blockly/Scratch 运行时生成输出目录，不是 Python 业务模块。
旧 `backend/script` 与 `backend/runScript.py` 仅作为只读回退路径保留，不再由新代码写入。

`editor/media` 与 `editor/models` 是被 `.gitignore` 忽略的运行时数据目录，用于项目媒体和
模型资源缓存；它们不是源码 owner，也不应被当作可迁移的 Python 模块。源码中的路径模型
由 `config/paths_config.py` 统一解析，运行时目录为空或不存在都属于正常状态。

### 实际子目录归属

顶层目录表只说明运行时边界；下面的子目录表用于判断文件是否应该继续留在当前位置。
`目标位置`是后续迁移方向，迁移完成前不直接移动仍被旧宿主引用的文件。

| 当前路径 | 实际职责 | 状态 | 目标位置/处理方式 |
|---|---|---|---|
| `config/paths_config.py` | 仓库、项目媒体、模型、截图和 Script Runtime 生成脚本路径解析 | `active / config-owner` | 唯一 `PathsConfig/get_default_paths` owner；`config.project_state` 复用此路径模型，`config.settings` 仅为历史 facade |
| `config/project_state.py` | CoronaEditor.ini、最近项目和活动项目设置 | `active / settings-owner` | 项目状态 canonical owner；`config/settings.py` 仅保留历史 facade |
| `config/settings.py` | 项目状态历史导入 facade | `compatibility / settings` | 新代码使用 `config.project_state`，不得新增调用 |
| `config/tests` | 配置和路径 owner 的边界测试 | `active / config-tests` | 测试跟随 `config` owner，不属于历史 `backend` 包 |
| `Frontend/tests/js` | Frontend JS/Node 单元测试 | `active / frontend-js-tests` | 测试集中归属此目录；通过相对路径引用 `Frontend/src` 生产模块，不在 `src` 下新增测试 |
| `Frontend/tests/python` | Frontend 专用 Python 架构/兼容边界测试 | `active / frontend-python-tests` | 验证 `Frontend/src` 的 manifest、compat 和入口边界；与跨层 Python 测试 `editor/tests` 分开，不承载生产代码 |
| `tests` | 跨层目录、兼容边界和架构文档测试 | `active / boundary-tests` | 不承载业务实现；验证各 canonical owner 与 legacy wrapper 的边界 |
| `api/editor_api.py` | Python 对 C++ manifest 的公共契约 adapter | `active / public-contract` | 唯一 Python 公共契约入口；旧 `CoronaCore/core/editor_api.py` 仅作兼容模块别名；`scene_datas` legacy 转发实现位于 `script_runtime/compat/legacy_scene_datas_adapter.py` |
| `api/tests` | manifest adapter、项目和文件契约测试 | `active / contract-tests` | canonical API 的测试归属；不再放在 `CoronaCore/core/tests` |
| `script_runtime/compat/legacy_scene_datas_adapter.py` | Script Runtime 专用 SceneDatas 兼容 adapter | `legacy / script-runtime adapter` | 唯一实现 owner；只允许受限 Script Runtime channel，普通 Editor Python 和新业务不得依赖；`script_runtime/legacy_scene_datas_adapter.py`、旧 `CoronaCore/core/legacy_scene_datas_adapter.py` 与 `legacy_editor_api.py` 仅作导入兼容 wrapper |
| `script_runtime/manifest_adapter.py` | Script Runtime 对 manifest 场景路由、场景快照、SceneTools 和视口值对象的受限 adapter | `active / script-runtime adapter` | 唯一实现 owner；`scene.list_routes`/`scene.switch`/`scene.get_snapshot`/`scene.set_actor_transform` 复用公共 schema，通过独立 caller channel；不向普通 Python 或角色脚本开放额外底层对象；旧 `CoronaCore/core/script_runtime_editor_api.py` 仅作兼容 wrapper |
| `script_runtime/native_engine_adapter.py` | 角色脚本/Blockly 的 Engine Runtime 原子能力 adapter | `active / script-runtime adapter` | 唯一 native capability owner；只暴露鼠标、射线、媒体和音频受限能力；`api.editor_api.get_script_runtime_adapter` 仅保留兼容转发 |
| `runtime/legacy_engine_adapter.py` | 旧实体/组件和登记的 legacy/test 注入使用的引擎内部 adapter | `legacy / internal` | 唯一实现 owner；当前无普通编辑器业务调用，新业务必须使用 manifest adapter；旧 `CoronaCore/core/engine_runtime.py` 仅作兼容 wrapper |
| `runtime/legacy_network_adapters.py` | 旧 native Network/LANChat fallback adapter | `legacy / adapter` | 唯一兼容实现 owner；`editor_api.py` 只保留公共 manifest factory 和队列规范化，不得新增旧 native 方法 |
| `runtime/legacy_scene_adapters.py` | 旧 native Scene、SceneTools 和 Viewport fallback adapter | `legacy / adapter` | 唯一兼容实现 owner；`editor_api.py` 只保留公共 manifest namespace 和 factory，不得新增旧 native 方法 |
| `runtime/editor_host.py` | 编辑器宿主生命周期、服务页注册、运行时更新和关闭 | `active / host` | runtime canonical；旧 `CoronaCore/core/corona_editor.py` 仅兼容转发 |
| `runtime/tests` | runtime 宿主、legacy adapter 和兼容边界测试 | `active / runtime-tests` | 测试跟随 runtime owner；不定义新的兼容入口 |
| `runtime/native_engine.py` | 嵌入式 CoronaEngine 模块加载与缓存 | `active / host-support` | 仅供 runtime host；旧 `CoronaCore/core/corona_engine.py` 仅兼容转发 |
| `runtime/response_utils.py` | runtime host 的结构化响应格式化 | `active / host-support` | 仅供 runtime host；旧 `CoronaCore/core/response_utils.py` 仅兼容转发 |
| `runtime/network_sync_policy.py` | Actor 创建同步过滤、事务延迟和去重策略 | `active / runtime-policy` | 供 SceneTools、AITool 协作和 legacy Actor 使用；旧 `CoronaCore/core/network_sync_policy.py` 仅兼容转发，不是公共 API |
| `runtime/project_templates.py` | 项目/场景/Actor 模板和 project.ini 初始化 | `active / project-support` | 模板与初始化 helper 的唯一 owner；`CoronaCore/core/project_utils.py` 和 `CoronaCore/utils/proejct_utils.py` 仅兼容转发 |
| `runtime/scene_support.py` | 场景清单和 legacy 自动保存 | `active / scene-support` | 场景持久化 helper 的唯一 owner；旧 project-support 聚合 wrapper 已删除，新代码按职责直接导入 |
| `runtime/legacy_project_copy.py` | 历史项目复制和打开 facade 实现 | `legacy / project-support` | 只供旧宿主使用；新项目生命周期使用 `project.*` |
| `runtime/project_copy.py` | removed ProjectCopy import shim | `removed compatibility code` | ProjectCopy 已集中到 `runtime/legacy_project_copy.py`，调用方使用显式 `data_root` |
| `data/` | legacy 项目复制的 runtime data 目录 | `runtime-data / compatibility` | 仅供旧项目复制流程使用，不是模板 source 或编辑器业务代码目录 |
| `runtime/legacy_scene_store.py` | 旧 Python Scene 宿主集中转发 | `legacy / canonical` | 旧 `CoronaCore/core/legacy_scene_store.py` 仅兼容转发；保留至旧宿主迁移完成，禁止新增调用 |
| `plugins/FileManager/compat/legacy_file_scene_adapter.py` | 文件操作触发的旧 Scene/Actor 路由同步和兼容事件 | `legacy / plugin adapter` | 仅供外部旧宿主使用；`runtime/legacy_file_scene_adapter.py` 仅为 shim；native `files.*` 已接管文件操作，待公共事件覆盖旧宿主后删除 |
| `plugins/SceneTools/compat/legacy_vision_import_adapter.py` | 旧 Vision 文档导入、代理 Actor 和 derived 文件兼容流程 | `legacy / plugin adapter` | 仅供 `plugins/SceneTools` 的旧 Web 兼容方法使用；`runtime/legacy_vision_import_adapter.py` 仅为兼容 shim；新 Vue/Python/AITool 代码不得依赖，待 native Vision 导入契约覆盖后删除 |
| `plugins/MainView/compat/legacy_main_view_scene_adapter.py` | MainView 旧 Python Scene 关闭和外部宿主兼容 | `legacy / plugin adapter` | 仅供外部旧宿主及 MainView 关闭兼容路径使用；`runtime/legacy_main_view_scene_adapter.py` 仅为 shim；初始化、创建、切换和文件删除已迁移到 native contract，待剩余关闭生命周期覆盖后删除 |
| `script_runtime/engine/host.py` | ScriptsManager 与首个 legacy Scene 的 host 初始化编排 | `active / script-runtime host` | 编辑器 host 只调用该 owner；旧 Scene 查询仍通过登记的 compat adapter，待旧项目脚本迁移后移除 fallback |
| `script_runtime/compat/legacy_scene_adapter.py` | Script Runtime 访问旧 Python Scene Store 的唯一兼容实现 | `legacy / adapter` | 仅供 `script_runtime/engine`、`script_runtime/blockly` 和旧 ScriptsManager adapter 使用；历史 runtime shim 已删除，待角色脚本完全切换到 native scene/project 生命周期后删除本 adapter |
| `plugins/AITool/compat/legacy_aitool_scene_adapter.py` | AITool 访问旧 Python Scene 的唯一兼容 fallback | `legacy / plugin adapter` | 仅供 AITool 的 native scene 兼容入口转发；`runtime/legacy_aitool_scene_adapter.py` 仅为兼容 shim；新业务必须使用 native snapshot/value object，待 AITool 完成 native scene 迁移后删除 |
| `runtime/legacy_camera_follow.py` | 旧宿主逐帧相机跟随和输入处理 | `legacy / adapter` | 仅供旧宿主转发入口使用；待外部面板迁移并完成输入回归后删除 |
| `runtime/legacy/entities` | 旧 Python Scene/Actor/Camera 实体模型 | `legacy / canonical` | 旧 `CoronaCore/core/legacy/entities` 仅兼容转发；逐步替换为 manifest value object |
| `runtime/legacy/components` | 旧实体组件和 Python 场景事实 | `legacy / canonical` | 旧 `CoronaCore/core/legacy/components` 仅兼容转发；由 C++ 权威状态和聚合 adapter 替代 |
| `runtime/legacy/managers` | 旧 Scene manager 实现 | `legacy / canonical` | 旧 `CoronaCore/core/legacy/managers` 仅兼容转发；仅由 `legacy_scene_store` 访问 |
| `CoronaCore/core/entities`, `components`, `managers` | 历史导入路径的兼容转发 | `compatibility` | 不新增调用；外部宿主迁移后删除 |
| `script_runtime/engine` | 角色脚本运行时、脚本实体和生命周期编排 | `active / script-runtime` | canonical Script Runtime；保持与编辑器公共 API 隔离 |
| `script_runtime/tests` | Script Runtime 和角色脚本 adapter 测试 | `active / script-runtime-tests` | 测试跟随 Script Runtime owner；旧 `CoronaCore/core/tests` 不再承载 canonical 测试 |
| `CoronaCore/core/scripts_system` | 角色脚本运行时的历史导入兼容 wrapper | `compatibility` | 新生成脚本使用 `script_runtime.engine`；旧项目脚本迁移完成后删除 |
| `runtime/archive` | 纯项目归档解析与快照规范化 | `active / project-support` | 供 `plugins/ProjectArchive` 和 legacy Scene 解析使用；旧 `CoronaCore/archive` 仅兼容转发 |
| `CoronaCore/utils` | Script Runtime、项目工具和响应工具的历史兼容转发 | `compatibility` | 新代码使用 `script_runtime.engine`、`runtime.project_templates`、`runtime.scene_support` 和公共 adapter；不得新增调用 |
| `runtime/registry.py` | 嵌入式 Python 服务注册和生命周期 | `active / host` | 唯一 canonical 服务宿主；旧 `backend/registry.py` 仅兼容转发 |
| `script_runtime` | 角色脚本运行时、Blockly 编译/预览和生成脚本执行 | `active / script-runtime` | `engine/` 承载角色脚本，`blockly/` 承载积木，`runner.py` 管理生成脚本执行；新输出写入 `runtime/generated`，旧 `backend` 输出只读回退 |
| `backend/blockly` | Blockly 的历史导入兼容转发 | `compatibility` | 新代码使用 `script_runtime.blockly`；外部宿主迁移后删除 |
| `backend/file_system` | FileManager 的旧导入兼容转发 | `compatibility` | 新注册使用 `plugins.FileManager`；确认旧宿主迁移后删除 |
| `backend/project_settings` | ProjectSettings 的旧导入兼容转发 | `compatibility` | 新注册使用 `plugins.ProjectSettings`；确认旧宿主迁移后删除 |
| `backend/script` | Blockly/Scratch 历史生成脚本输出目录 | `compatibility / read-only fallback` | 只读取旧宿主或既有输出，不再写入；新输出归 `runtime/generated` |
| `plugins/AITool` | AI、Agent、模型和场景生成工作流 | `active` | 保持插件边界，只使用公共 adapter/value object |
| `plugins/AITool/configuration` | AITool `.env`、API key 和本地配置加载 | `active / configuration-owner` | canonical secret/config loader；`compat/legacy_local_ai_setting.py` 集中保留旧配置 wrapper，不放通用业务工具 |
| `plugins/AITool/cai_extensions` | Quasar 集成、Agent、MCP 工具和生成工作流扩展 | `active / integration-owner` | 只编排 AITool 扩展和 value-object adapter，不定义引擎底层 API 或本地密钥配置 |
| `plugins/AITool/cai_extensions/agent` | Agent 适配、场景组合、审查和模型提供器的 Quasar 集成 | `active / integration-agent` | 负责 AI/Quasar 侧 Agent 编排；引擎状态必须通过 `native_scene_state` 或公共 adapter 获取 |
| `plugins/AITool/cai_extensions/flows` | 多步场景生成、模型检索和地形工作流 | `active / integration-workflow` | 负责工作流节点和流程，不拥有通用 Agent runtime 或引擎公共 API |
| `plugins/AITool/cai_extensions/mcp` | 场景、相机、模型导入和审查 MCP 工具 | `active / engine-tools` | 负责工具协议与 value-object 转换；旧 Scene fallback 只能经过登记的 legacy adapter |
| `plugins/AITool/cai_extensions/scene_placement` | 场景布置工具及其配置 | `active / placement-tools` | 负责布置领域输入和工具 loader，不复制 Scene/Actor 权威状态 |
| `plugins/AITool/services` | 编辑器侧 AI 请求、对话、协作、生成和运行时服务编排 | `active / service-owner` | 服务实现 owner；通过 `api.editor_api`/受限 adapter 访问引擎，具体 Quasar/Agent 实现（含场景元素分类器）由 worker 等 composition root 注入，不把测试或 Quasar 实现混入通用 runtime |
| `plugins/AITool/services/composition_root.py` | AITool 可选 Quasar/Agent 集成构造和轻量模块加载 | `active / composition-root` | 唯一负责 ModelProvider、EngineWriteGate、scene classifier 的组合根工厂；服务模块只接收注入结果，不在自身加载集成实现 |
| `plugins/AITool/services/runtime_query_policy.py` | AgentRuntime 文本命令、状态查询和 GM 触发器识别 | `active / runtime-policy` | 无状态查询策略唯一 owner；`lanchat_agent_worker.py` 只负责编排并保留兼容转发，不在 worker 内复制词汇或归一化规则 |
| `plugins/AITool/services/runtime_result_policy.py` | AgentRuntime 图和批次结果的兼容形状归一化 | `active / runtime-policy` | 统一处理 `graphs/graph/queued` 与 `batches/batch/queued` 响应形状；不生成回复或读取 Runtime 状态，worker 仅保留兼容转发 |
| `plugins/AITool/services/runtime_report_policy.py` | AgentRuntime 场景、context、execution reply composition、intervention digest/reply/summary、快照、资源阶段、resource adapter/readiness、环境、资源批次、batch tooling、geometry、command、review summary/proposal/confirmation、layout confirmation、engine-write report/readiness/boundary、健康、事实来源、closure、导入、actor-import boundary、队列、对账和工具报告格式化 | `active / presentation-policy` | 只接收结构化摘要并执行有限字段脱敏；`lanchat_agent_worker.py` 保留原始 Runtime 结果到 evidence 的适配和消息编排，不在 policy 中读取 Runtime |
| `plugins/AITool/services/runtime_replay_report_policy.py` | AgentRuntime replay 的总体 composition、命令、工具、队列、state patch、worker-drain、tool graph、GM summary 和 guard 摘要格式化 | `active / presentation-policy` | 只处理 replay summary 展示和有限字段脱敏；不读取 Runtime 状态，worker 仅保留兼容转发 |
| `plugins/AITool/services/runtime_replay_lifecycle_policy.py` | AgentRuntime replay 的 plan lifecycle、intervention 和 geometry 摘要格式化 | `active / presentation-policy` | 只处理生命周期、介入合并和几何事实摘要；不读取 Runtime 状态，worker 仅保留兼容转发 |
| `plugins/AITool/services/runtime_replay_event_policy.py` | RuntimeEvent replay 的 event rows、GM runtime-event digest、report-ready、引擎失败和 disclosure skip 摘要格式化 | `active / presentation-policy` | 只处理结构化事件摘要和有限字段脱敏；不发送事件、不读取 Runtime 状态，worker 仅保留兼容转发 |
| `plugins/AITool/services/runtime_replay_detail_policy.py` | replay failure strategy、layout、VLM、review advisory 和 final adjustment 摘要格式化 | `active / presentation-policy` | 只处理详细 replay 计数与状态展示；不执行回放、不读取 Runtime 状态，worker 仅保留兼容转发 |
| `plugins/AITool/services/runtime_replay_resource_policy.py` | replay environment 和 resource readiness 摘要格式化 | `active / presentation-policy` | 只处理资源事件/就绪计数和有限字段脱敏；不读取 Runtime 状态，worker 仅保留兼容转发 |
| `plugins/AITool/services/runtime_replay_transfer_policy.py` | replay asset transfer 摘要格式化 | `active / presentation-policy` | 只处理资产传输计数、进度和字节摘要；不暴露资产身份/路径、不读取 Runtime 状态，worker 仅保留兼容转发 |
| `plugins/AITool/services/runtime_replay_peer_sync_policy.py` | replay peer sync 摘要格式化 | `active / presentation-policy` | 只处理 peer 事件、房间和 reconcile 计数；不暴露 peer 身份、不读取 Runtime 状态，worker 仅保留兼容转发 |
| `plugins/AITool/services/runtime_sync_policy.py` | Runtime Sync aggregate report、GM Sync replay digest、actor/asset 行、健康和聚合传输摘要格式化 | `active / presentation-policy` | 只处理有界的 aggregate、replay digest、actor/asset 展示、健康计数和传输摘要；不查询 Runtime 或编排同步，worker 仅保留兼容转发 |
| `plugins/AITool/services/runtime_replay_sync_policy.py` | Runtime Sync replay 摘要格式化 | `active / presentation-policy` | 只处理同步回放计数、进度和脱敏错误标签；不执行回放、不读取 Runtime 状态，worker 仅保留兼容转发 |
| `plugins/AITool/services/runtime_message_delivery_policy.py` | Runtime message delivery 摘要格式化 | `active / presentation-policy` | 只处理消息投递计数、通道/阶段摘要和有限脱敏；不发送消息、不读取 Runtime 状态，worker 仅保留兼容转发 |
| `plugins/AITool/services/agent_runtime` | Agent 运行时状态、快照输入、资源 provider 和能力适配 | `active / runtime-service` | 负责运行时生命周期和受限资源能力；不直接成为 Quasar workflow 或引擎对象 owner；具体 legacy ModelProvider 由集成编排层注入 |
| `plugins/AITool/services/agent_collaboration` | Agent 协作任务、artifact、project state 和 readiness contract | `active / collaboration-service` | 负责协作状态与契约；不发起底层 C++ 调用或持有 API key |
| `plugins/AITool/services/tests` | AITool service、Agent runtime 和协作服务测试 | `active / service-tests` | 所有 service 测试统一归属此目录；`services` 根目录只保留生产模块 |
| `plugins/AITool/services/tests/support` | service 测试专用 helper、fixture 和 engine double | `active / service-test-support` | 仅供测试导入；不得从生产模块反向依赖，`services` 根目录不放测试 helper |
| `plugins/AITool/compat/legacy_local_ai_setting.py`、`legacy_aitool_utils.py`、`legacy_image_utils.py` | 外部历史配置、包级 utility 和媒体 helper wrapper | `compatibility / external-import-shim` | 仅供外部旧入口，分别转发到 `configuration.local_secrets` 或 `services.media_storage`；`utils` 内部不再依赖 compat |
| `plugins/AITool/utils` | 历史配置和媒体 helper 导入 wrapper 旧路径 | `compatibility` | 只保留配置、包级 `load_ai_setting` 和 `image_utils.py` shim；媒体实现归 `services/media_storage.py`，不得新增业务 owner |
| `plugins/AITool/tests` | AITool 跨模块边界、配置、Quasar 生命周期测试和非 native 验证 runner | `active / plugin-tests` | 测试插件入口与跨目录契约；`verify_ultimate_plan.py` 只负责验证，不承载生产服务实现 |
| `plugins/SceneTools` | 场景、视口和资源聚合 handler | `active / aggregate-owner` | Vision legacy 导入和 Scene 查询集中到 `plugins/SceneTools/compat/legacy_vision_*.py`；runtime 路径仅保留兼容 shim |
| `plugins/MainView` | 启动、项目打开和主视图编排 | `active / aggregate-adapter` | 只保留宿主编排，项目上下文和场景生命周期走 `main.*`/`projectSettings.*` native 聚合契约；关闭仍走登记的 legacy adapter |
| `plugins/SceneDatas` | 旧 Object 面板插件壳 | `compatibility-only` | 不新增功能，待旧宿主确认后删除 |
| `plugins/ProjectLauncher` | 项目创建、打开和迁移 | `active` | 归属 `project.*`，与 MainView 解耦 |
| `plugins/ProjectArchive` | 旧项目归档/解析迁移 facade | `compatibility-boundary` | 实现位于 `main.py`；归属 `project.migrateLegacyScene` |
| `plugins/FileManager` | 文件操作和文件树聚合 handler | `active` | 归属 `files.*`，不依赖旧 backend 实现 |
| `plugins/ProjectSettings` | 项目设置公共 adapter | `active` | C++ `projectSettings.*` 负责事实、校验和持久化；Python 仅保留旧插件入口 |
| `Frontend/src/api` | C++ manifest 的 JS contract adapter 和 transport owner | `active / public-contract` | `editorApi.js` 是唯一 manifest transport owner；不定义第二套 schema |
| `Frontend/src/services/sceneService.js` | 旧场景便捷 facade | `compatibility / service-adapter` | 只转发到 `editorApi.scene`/`sceneTools`；新 Vue 代码不得新增调用，待外部旧宿主迁移后删除 |
| `Frontend/src/services/projectService.js` | 旧项目/主视图便捷 facade | `compatibility / service-adapter` | 仓内 Vue 调用已迁移；只为 `bridge.js`/外部旧宿主转发到 `editorApi.main`，Dock transport 已归 `appService`；待外部旧宿主迁移后删除 |
| `Frontend/src/services/appService.js` | Dock、CameraView 窗口和应用 transport adapter | `active / window-transport-adapter` | 集中 Dock command 与 `editorApi.app` 调用；组件不得复制窗口协议；外部旧宿主仍可经 `bridge.js` 访问 |
| `Frontend/src/services/lanChatService.js` | 局域网聊天室和 Agent 房间 response adapter | `active / lan-chat-adapter` | 统一 `editorApi.lanChat` 调用和旧响应解包；不定义 LANChat manifest schema |
| `Frontend/src/services/networkService.js` | 网络会话、协作锁和同步队列 response adapter | `active / network-adapter` | 统一 `editorApi.network` 调用和旧响应解包；不定义 NetworkSystem schema 或场景权威状态 |
| `Frontend/src/services/scriptingService.js` | Blockly/角色脚本运行兼容 facade | `compatibility / service-adapter` | 仓内 Vue/节点图调用已迁移；只为 `bridge.js`/外部旧宿主转发到 `editorApi.scratch`；Script Runtime 权限和协议仍由公共 contract/adapter 定义 |
| `Frontend/src/services/aiService.js` | AI、节点图审查/生成和 Cabbage workflow adapter | `active / ai-request-adapter` | 统一 operation 名称、`editorApi.ai` 请求和响应解包；不定义 API key、Agent runtime 或引擎场景状态 |
| `Frontend/src/services/projectLauncherService.js` | 项目打开、legacy 迁移和活动项目生命周期编排 | `active / project-lifecycle-service` | `openProject`、`migrateLegacyScene` 保留切换前保存、localStorage 和事件编排；纯项目查询/创建调用已迁移到 `editorApi.project`，项目语义 owner 仍是 `project.*` |
| `Frontend/src/services/fileService.js` | 文件树、文件创建/删除/重命名和打开 facade | `compatibility / service-adapter` | 仓内 FileManager 调用已迁移；只为 `bridge.js`/外部旧宿主转发到 `editorApi.files`；文件契约 owner 仍是 `files.*` |
| `Frontend/src/services/projectSettingsService.js` | 项目设置读取、保存和场景文件选择 facade | `compatibility / service-adapter` | 仓内调用已迁移；只为 `bridge.js`/外部旧宿主转发到 `editorApi.projectSettings`；项目设置契约 owner 仍是 `projectSettings.*` |
| `Frontend/src/services/resourceService.js` | 场景栏资源搜索 gate 和 disabled fallback | `active / resource-search-gate` | 集中维护 `RESOURCE_SEARCH_ENABLED`、调用方标识和 disabled 响应；启用时转发到 `editorApi.resourceSearch`，不定义第二份资源搜索 schema |
| `Frontend/src/services/logService.js` | 旧日志生命周期 no-op facade | `compatibility / service-adapter` | 仅保持 `setLogReady`/`setLogClose` 兼容返回值；日志 owner 仍是 runtime/application |
| `Frontend/src/services/nodeGraphGenerationService.js` | 节点图 AI 生成请求、确认和轮询编排 | `active / domain-service` | 负责节点图生成交互状态；通过 `aiService` 和事件总线通信，不拥有 AI transport |
| `Frontend/src/services/nodeGraphReviewService.js` | 节点图快照审查和结果通知编排 | `active / domain-service` | 负责审查快照、轮询和事件；不定义 AI API 或节点图公共 schema |
| `Frontend/src/services/nodeGraphRuntimeService.js` | 节点图保存、脚本运行和运行时状态编排 | `active / domain-service` | 组合 `appService`/`scriptingService` 与事件总线，不直接访问引擎原子 API |
| `Frontend/src/services/cabbageAssistantContextService.js` | Cabbage 助手上下文、目标计划和变换同步 | `active / domain-service` | 负责跨面板上下文事件与本地同步状态；不持有 API key 或 Agent runtime |
| `Frontend/src/services/cabbageGuidanceService.js` | Cabbage 引导面板的 UI 状态和窗口编排 | `active / ui-service` | 只管理引导面板展示与 Dock 窗口，不承担 AI 请求或场景权威状态 |
| `Frontend/src/services/cabbageTutorialSessionService.js` | Cabbage 教程会话恢复和跨窗口协调 | `active / ui-service` | 只负责教程会话消息与恢复器，不定义编辑器公共契约 |
| `Frontend/src/utils` | 低延迟输入 adapter 和历史 bridge 导出兼容层 | `active / adapter` | `bridge.js` 仅保留外部旧宿主兼容 re-export；生产代码必须直接导入 `api` 或 `services`，不放业务状态机 |
| `Frontend/src/compat` | 旧 CEF/宿主兼容 adapter | `legacy / compatibility` | 只保留旧宿主桥接；新 Vue 业务不得依赖 raw CEF 请求格式 |
| `Frontend/index.html` | CEF 启动页和 legacy panel 加载入口 | `legacy / host-entry` | 只保留迁移说明、`src/compat` 的 CSS 和 module loader；panel 实现不得重新内嵌 |

以下目录或入口虽然名称较旧，但当前不能仅凭“看起来过时”删除：

| 对象 | 当前用途 | 处理规则 |
|---|---|---|
| `runtime/generated` | Blockly/Scratch 生成的运行时脚本输出目录 | Script Runtime 运行时生成，不作为 Python 业务模块；由 `config.paths_config` 提供路径 |
| `backend/script`、`backend/runScript.py` | Blockly/Scratch 历史运行时输出 | 只读兼容回退；新代码不再写入，外部宿主迁移完成后可删除 |
| `plugins/SceneDatas` | 旧脚本宿主的插件壳 | 仅允许兼容调用，新的编辑器业务不得依赖 |
| `runtime/legacy_scene_store.py` | 旧 Python Scene 宿主的集中入口 | 只允许登记的 legacy fallback 使用；旧 `CoronaCore/core/legacy_scene_store.py` 仅作兼容 alias |
| `include/corona/systems/script/corona_engine_api.h` | 旧 C++ include 转发头 | 新代码使用 `corona/engine/engine_runtime_api.h`，待外部 include 迁移后再删除 |
| `plugins/AITool/Quasar` | 外部 AI 子模块 | 不在本架构整理范围内，不直接修改其内容 |

目录边界的快速判断：

| 如果代码…… | 应放在…… | 不应放在…… |
|---|---|---|
| 定义场景写入、revision、错误或事件语义 | manifest owner 对应的 handler | Vue、AITool 或通用 `utils` |
| 只把契约转换为 JS/Python/Script 调用 | 对应 adapter | 业务插件内部重新定义协议 |
| 只提供角色脚本需要的引擎原子能力 | Script Runtime adapter | 编辑器公共契约或 Vue |
| 兼容旧宿主 | 集中的 `legacy_*` adapter | 新生产调用方 |

以下内容属于生成或环境目录，不应被当作架构模块：`Frontend/node_modules`、`Frontend/dist`、
各目录下的 `__pycache__`、测试缓存和运行时输出。它们应由版本控制忽略或打包排除规则处理。

目录状态的详细 owner、调用方和删除条件见 [API_OWNERSHIP.md](API_OWNERSHIP.md)；如果目录定位
与公共契约 owner 冲突，以 manifest/schema 和 ownership 清单为准。

目录名不等于调用权限。新增代码应先确认它属于哪一层，再通过对应 adapter 调用
manifest；旧目录和旧 binding 只有在文档明确标记为 `legacy` 时才可继续作为兼容路径。

`Frontend/src/utils/bridge.js` 中的 service aliases 只是 `editorApi` namespace 的命名兼容
别名；它们不拥有 schema、错误码或状态机。新代码必须直接使用 `editorApi` 或对应的
`src/services` owner。当前仓内 Vue 生产组件已迁移完成，aliases 仅为外部旧宿主或未同步
升级的插件保留；不得在新代码中新增调用。待外部宿主确认后，才能按
[API_OWNERSHIP.md](API_OWNERSHIP.md) 的删除条件移除。

## 当前整理状态

架构整理按“契约 → adapter → 调用方 → legacy”推进，不以目录改名作为起点。当前状态：

| 阶段 | 状态 | 证据/下一步 |
|---|---|---|
| 公共 manifest 与 owner | active | 以 C++ manifest/schema 为唯一契约来源 |
| Vue 聚合 adapter | active | `editorApi.scene`、`editorApi.sceneTools` 和 `editorApi.viewport` |
| Vue 生产调用迁移 | 已完成 | 生产组件不再导入 `utils/bridge.js`；公共 API 和 facade 均直接使用其 owner，bridge 仅供外部旧宿主 |
| Python/AITool adapter | 迁移中 | 公共路径通过 `api.editor_api` 和 native value adapter；仅登记的 legacy fallback 经过 `runtime/legacy_*` |
| Script Runtime | 受限运行时 | 不向角色脚本开放完整编辑器 API |
| legacy 删除 | 未开始批量删除 | 必须完成外部宿主确认、回退测试和发布兼容评估 |

最近完成的前端调用方收敛：`MainPage`、`SceneBar` 的项目/主视图操作已直接调用
`editorApi.main`；`DockTitleBar`、`Pet`、`CameraView` 的拖拽区域操作统一归
`appService`。随后 `App`、`MainPage`、`CameraView`、Blockly 工作区和节点图运行时的
脚本调用已直接使用 `editorApi.scratch`。因此 `projectService.js` 与
`scriptingService.js` 当前仅保留兼容导出，不应再被仓内 Vue 生产代码导入。
`NodeGraphWorkspace`、`NewGame`、`Network` 和 `ProjectSettings` 的项目设置调用也已直接使用
`editorApi.projectSettings`，因此 `projectSettingsService.js` 同样仅保留兼容导出。
`FileManager` 的文件树、文件操作和打开文件调用已直接使用 `editorApi.files`；其 legacy 项目
迁移动作仍经 `projectLauncherService.migrateLegacyScene`，因为该动作包含项目生命周期状态更新。

验证整理结果时，应同时检查生产调用搜索、adapter 边界测试、相关功能回归和
`git diff --check`；不能只根据目录是否为空或名称是否统一判断迁移完成。

## 构建与运行

- Editor 作为 CoronaEngine 的内置模块构建，不维护独立的一键构建入口。
- Python 依赖由顶层 CMake 检查 `editor/requirements.txt`。
- 前端构建由顶层 CMake post-build 步骤触发，使用仓库配置的 Node/npm。
- Python 入口和运行时加载位置由顶层引擎宿主决定，不应根据历史目录名推断架构职责。

## 修改前检查

新增或迁移跨层调用前，先完成以下检查：

1. 在 manifest 中找到已有业务语义，或先登记新的 owner/schema；
2. 确认调用方属于 Vue、Editor Python 或 Script Runtime 哪个边界；
3. 让 adapter 只负责传输、权限和结果转换，业务规则回到 owner；
4. 若保留旧入口，补齐替代接口、当前调用方、删除条件和回退测试；
5. 运行对应边界测试，再进行目录移动或删除。
