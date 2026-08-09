# CoronaEditor 架构与职责边界

本文说明 Vue、C++、Python、Script Runtime 之间的职责和依赖方向；历史兼容入口总索引见 [`COMPATIBILITY.md`](COMPATIBILITY.md)。

接口的参数、返回值、权限、错误和事件以 C++ manifest/schema 为唯一机器可读来源：
`src/systems/ui/editor_api/cef_editor_api.cpp`。manifest 的注册/分发实现可以调用 Python
service，但这不改变契约的唯一来源。接口 owner、调用方和迁移状态见
[API_OWNERSHIP.md](API_OWNERSHIP.md)。本文不复制 manifest，也不统计 API 数量。

## 文档使用方式

遇到跨层调用时按以下顺序判断：

1. 先在 manifest 中确认已有的业务语义、schema、`allowed_callers` 和事件；
2. 再在 [API_OWNERSHIP.md](API_OWNERSHIP.md) 确认 owner、适配器和迁移状态；
3. 最后在对应 adapter 中实现调用方转换。

如果 manifest 没有该语义，不得先在 Vue、Python 或 Script 中各自加一个同名功能。
应先决定它是 Engine Runtime 原子能力、内部 Domain Service、受限 Script 能力，还是
需要登记的新公共契约。

## 核心结论

项目只维护一套跨层公共语义契约。它的维护单元是一个业务语义，而不是同一语义在
Vue、Python 和 Script 中的函数数量。

```text
Engine Runtime 原子能力
        ↓
Domain Service / Aggregate Handler
        ↓
唯一公共契约（manifest + schema）
        ├─ Vue adapter
        ├─ Editor Python adapter
        └─ 受限 Script Runtime adapter
```

例如，下面三个入口是同一个公共 API，而不是三套 API：

```text
scene.set_actor_transform
├─ Vue:    editorApi.scene.setActorTransform(...)
├─ Python: CoronaEditorApi.scene.set_actor_transform(...)
└─ Script: 仅在权限允许时使用对应的受限 adapter
```

它们必须共享同一份参数/返回 schema、权限、错误语义、revision 和事件顺序。wrapper
可以改变命名、传输方式或同步模型，但不能增加业务校验、状态机或另一套错误协议。

只有新增独立的业务语义、权威状态、错误协议或事件协议，才算新增公共 API，必须指定
owner 并登记 manifest/schema。

当前 camera-lock 的公共语义由 `sceneTools.setActorCameraLock` 统一承载。Vue 和 Python
只传递同一个 `camera_lock` value object；旧 `scene_datas.actor_operation` 仍是兼容入口，
但不得继续扩散。这样保留旧宿主时不会形成第二套摄像机跟随状态或事件协议。

## 四类边界

| 边界 | 作用 | 允许调用方 | 是否为公共 API |
|---|---|---|---:|
| Engine Runtime | `camera()`、`geometry()`、ECS、资源等原子能力 | C++ 内部、受限 Python/Script Engine adapter | 否 |
| Domain Service | 组合原子能力，完成校验、事务、revision 和事件 | manifest handler | 否，属于实现 |
| Public Contract | 稳定的编辑器业务语义及 schema | Vue、编辑器 Python、获授权的 Script adapter | 是 |
| Adapter / Legacy | 传输转换或旧宿主兼容 | 对应调用方、兼容层 | 否 |

`camera()`、`geometry()` 等底层函数不得直接暴露给 Vue、AITool 或普通编辑器 Python
业务。需要“设置 Actor 变换”“捕获视口”这类编辑器语义时，应由 Domain Service
组合底层能力并发布一个 manifest 方法。若当前实现由 Python handler 完成组合，也必须
通过同一 manifest、schema、权限和事件路径，不能因此形成第二个 Python API 来源。

## 依赖方向

```text
Vue Frontend ───────┐
Editor Python ──────┼──> Public Contract ──> Domain Service / Handler ──> Engine Runtime
Script Runtime ────┘          ↑
                         manifest/schema
```

禁止以下反向依赖：

- Vue 不导入 Python 模块或持有 C++ 对象；
- Python 业务不依赖 Vue，也不把 native engine 作为业务状态；
- AITool、普通插件和普通 backend 不直接导入 `CoronaEditor`、`scene_manager`、
  `runtime.legacy` 或其历史兼容路径；
- 调用方不得组合多个公共接口，形成未登记的隐含业务协议；重复出现的组合必须回收到
  C++ Domain Service。

## 各层职责

### Vue Frontend

目录：`editor/Frontend`

负责页面、面板、工具栏、鼠标/键盘/触控/Gizmo 输入、临时 UI 状态、结果展示和事件订阅。
通过 `editor/Frontend/src/api/editorApi.js` 的 wrapper 调用 manifest；`src/utils/bridge.js`
仅保留历史导出兼容。

Frontend `src` 顶层目录职责见 `editor/Frontend/src/BOUNDARY.md`；公共契约和 manifest transport 位于 `editor/Frontend/src/api`，领域 service 的分类见
`editor/Frontend/src/services/BOUNDARY.md`，其旧 CEF 兼容边界见 `editor/Frontend/src/compat/BOUNDARY.md`；低延迟输入 adapter 位于
`editor/Frontend/src/utils`，具体职责见 `editor/Frontend/src/utils/BOUNDARY.md`；生产组件直接依赖对应的 API/service owner，旧 CEF/宿主实现集中在
`editor/Frontend/src/compat`，不得把 raw CEF 请求格式重新放回 active utils 或 Vue 业务。
旧 `sceneService` facade 位于 `editor/Frontend/src/services/sceneService.js`，只允许兼容调用，
不得重新成为新的场景契约 owner；同目录的 `projectService.js` 只负责旧项目/主视图 facade。
`appService.js` 只保留历史路径 wrapper；旧 Dock、CameraView 窗口和进程操作 facade 位于
`editor/Frontend/src/compat/appService.js`。
`lanChatService.js` 只负责旧 LANChat/Agent 响应兼容，不定义网络协议或 AI 业务状态。
`networkService.js` 只负责旧协作网络响应兼容，不定义 NetworkSystem 协议或场景权威状态。
`scriptingService.js` 只负责旧 Blockly/脚本调用兼容，不扩大 Script Runtime 权限。
`aiService.js` 只负责旧 AI/节点图/Cabbage 调用兼容，不持有密钥、Agent runtime 或场景事实。
`projectLauncherService.js` 只负责旧项目启动/迁移 facade，不定义新的 project schema。
`fileService.js` 只负责旧文件面板 facade，不定义文件树或文件操作契约；新代码优先使用
`editorApi.files`。
`projectSettingsService.js` 只负责旧项目设置面板 facade，不定义项目设置契约；新代码优先使用
`editorApi.projectSettings`。
`resourceService.js` 只负责旧资源搜索面板 facade，并保留当前 disabled fallback；新代码优先使用
`editorApi.resourceSearch`，不得在 facade 外复制搜索协议。
`logService.js` 只负责旧日志生命周期 no-op facade，不拥有日志实现或日志状态；新代码不得以此
创建第二套日志协议。
`nodeGraphGenerationService.js`、`nodeGraphReviewService.js` 和 `nodeGraphRuntimeService.js`
是节点图领域编排 service，负责 UI 请求生命周期、轮询和事件协调；它们可以组合兼容 facade，
但不得成为 `editorApi` 契约或引擎状态 owner。
`cabbageAssistantContextService.js`、`cabbageGuidanceService.js` 和
`cabbageTutorialSessionService.js` 是 Cabbage 的上下文/UI service，分别负责跨面板上下文、
引导窗口和教程会话；它们不持有密钥、Agent runtime 或 Scene/Actor 权威状态。
`Frontend/index.html` 只负责启动页、迁移说明和 compat module/CSS loader；旧相机跟随面板
由 `editor/Frontend/src/compat/legacyCameraLockPanel.js` 与对应 CSS 承担，panel 实现不得重新内嵌。
`src/utils/bridge.js` 仅作为外部旧宿主的兼容 barrel，`src` 内生产代码不得重新导入它。
Frontend 的 Node 单元测试统一位于 `editor/Frontend/tests/js`，通过 `npm run test:unit`
执行；Frontend 专用的 Python manifest/compat 边界测试位于 `editor/Frontend/tests/python`，
跨层架构测试位于 `editor/tests`。`src` 只保留生产代码，避免测试与 Vue/adapter owner 混放。

Vue 不持有 Scene/Actor 的权威状态，不执行 AI 或角色脚本，不访问引擎对象和 Python 私有模块。

### Realtime 输入与视口快速通道

`window.coronaBridge` 上的 `cameraMove`、`setCameraViewport`、`pickActor`、
`viewportGizmoPointer`、`setViewportUiMode`、`setViewportUiCalibration`、
`viewportUiPointer` 和 `injectInput` 属于 CEF/SharedDataHub 的低延迟传输通道，不是
编辑器公共 API。它们只允许由 `editor/Frontend/src/utils/viewport*.js`、视口控制器或
明确登记的输入组件使用，原因是拖拽、相机连续移动和触控输入不能承受 manifest 请求的
往返开销。

快速通道不得暴露 `Camera`、`Actor`、`Geometry` 等对象，也不得在业务组件中新增私有
参数或状态语义。需要持久化、权限、revision、事务或跨模块事件的操作，必须回到
`editorApi` 的 manifest 聚合契约；快速通道只传输句柄、值对象和输入事件。

### C++ Editor Host 与 Engine

负责编辑器宿主、CEF、渲染、ECS、Scene、Actor、资源、输入、网络同步，以及 Python
解释器和 Script Service 生命周期；同时负责发布和校验跨层 manifest 契约。

C++ 是 Scene、Actor、资源、变换、revision 和协作状态的唯一权威。跨层修改必须在
Domain Service 中完成校验、状态更新和事件发布。

`runtime/native_engine.py` 仅负责解析嵌入式原生模块，`runtime/response_utils.py` 仅负责
宿主响应格式化；二者不是编辑器业务 API。旧 `CoronaCore/core/corona_engine.py` 和
`CoronaCore/core/response_utils.py` 只保留兼容转发。

`runtime/network_sync_policy.py` 是跨宿主的内部同步策略，负责 Actor 创建过滤、事务边界和
去重；它不定义 manifest 契约，也不持有场景权威状态。旧
`CoronaCore/core/network_sync_policy.py` 只保留兼容转发。

旧编辑器宿主上下文（活动项目路径、兼容事件、相机输入锁和选择状态）集中在
`runtime/legacy_editor_adapters.py`；`api/editor_api.py` 只保留公共 manifest adapter 和
历史导入转发，不在契约模块中实现宿主状态兼容逻辑。

`runtime/archive` 是无引擎副作用的项目归档解析器，只产生校验后的快照和值对象；它不创建
Scene/Actor，也不定义 `project.*` manifest。`CoronaCore/archive` 仅为旧导入路径保留兼容包。

项目模板资产归 `plugins/ProjectLauncher/templates` 所有；`runtime/project_templates.py`
提供模板复制和 project.ini 初始化，`runtime/scene_support.py` 提供自动保存 helper，
不通过自身文件位置推断模板目录。`runtime/project_support.py` 仅保留历史兼容转发。旧
`CoronaCore/core/project_utils.py` 与 `CoronaCore/utils/proejct_utils.py` 仅为兼容转发。C++ 与
Python 的项目创建流程必须使用同一个模板 owner。

底层头文件归属为 `include/corona/engine/engine_runtime_api.h`。旧的
`include/corona/systems/script/corona_engine_api.h` 仅是兼容转发头，不是新的公共 API。
Engine Runtime 实现位于 `src/engine/engine_runtime_api.cpp`；Script Runtime 的原子
Python binding 位于 `src/systems/script/python/engine_bindings.cpp`，迁移期旧编辑器
binding 单独位于 `src/systems/script/python/editor_compat_bindings.cpp`，编辑器/AITool
网络兼容 binding 位于 `src/systems/script/python/editor_network_bindings.cpp`。其中
`engine_bindings.cpp` 不再承载旧编辑器相机跟随入口；`camera_follow_*` 全部位于
`editor_compat_bindings.cpp`，仅供 `runtime/legacy_camera_follow.py` 等登记的兼容 adapter
使用；旧宿主生命周期的 `request_engine_exit` 以及旧 Vision/媒体函数也归入该兼容文件。
这些 raw binding 仍保留在 `CoronaEngine` 模块中只是为了兼容导入路径，不构成编辑器公共 API。
输入队列
`InputEvent`/`drain_input_events` 和 `python_runtime_phase` 仍保留在
`engine_bindings.cpp`，因为它们是 Script Runtime 输入/诊断通道。三者都通过 Script system
的 CMake target 链接；后两者只供登记的兼容 adapter，不是编辑器公共 API owner。

### Python Embedded Runtime

Python 用于嵌入式 AI、Agent、模型调用、生成工作流、角色脚本、Blockly/Scratch 和自动化，
不是传统 Web 后端。

| 目录 | 职责 |
|---|---|
| `editor/api` | Python manifest adapter；唯一的 Editor Python 公共契约入口 |
| `editor/Frontend/src/api` | C++ manifest 的 JS contract adapter 和 transport owner |
| `editor/main.py` | 嵌入式 Python runtime 启动入口；只负责 host 初始化和服务注册，不承载插件业务 |
| `editor/CoronaCore` | Engine/Script adapter 和 legacy adapter 的历史兼容包；具体 owner 和删除条件见 [`CoronaCore/COMPATIBILITY.md`](CoronaCore/COMPATIBILITY.md) |
| `editor/config` | 路径、活动项目和应用运行时配置；具体边界见 [`config/BOUNDARY.md`](config/BOUNDARY.md) |
| `editor/runtime` | 嵌入式 Python 服务注册、生命周期和宿主编排 |
| `editor/backend` | 历史 Blockly/脚本生成路径和兼容入口；仅保留单向 wrapper，具体 owner 和删除条件见 [`backend/COMPATIBILITY.md`](backend/COMPATIBILITY.md) |
| `editor/script_runtime` | 受限角色脚本运行时和 Blockly 编排；不持有编辑器公共场景状态 |
| `editor/plugins/AITool` | AI、Agent、模型和生成工作流；`configuration` 负责 `.env`/API key 加载；插件总边界见 [`plugins/BOUNDARY.md`](plugins/BOUNDARY.md) |
| `editor/plugins/SceneTools` | 场景编辑器聚合 handler 及迁移期内部实现；Vision fallback 和未引用 helper 边界见 [`plugins/SceneTools/BOUNDARY.md`](plugins/SceneTools/BOUNDARY.md) |
| `editor/plugins/MainView` | 项目/场景宿主编排；旧 Python Scene 生命周期边界见 [`plugins/MainView/COMPATIBILITY.md`](plugins/MainView/COMPATIBILITY.md) |
| `editor/plugins/ProjectArchive` | 归档迁移 facade；解析实现归属 `runtime.archive`，边界见 [`plugins/ProjectArchive/BOUNDARY.md`](plugins/ProjectArchive/BOUNDARY.md) |
| `editor/runtime/plugin_base.py` | 显式注册 Python 插件的基础兼容设施；旧 `CoronaPlugin` 仅保留 [`CoronaPlugin/COMPATIBILITY.md`](CoronaPlugin/COMPATIBILITY.md) 登记的导入 wrapper |
| `tools/pack.py` | 仓库项目打包工具；不参与编辑器运行时依赖方向，旧 `editor/scripts/pack.py` 仅保留 [`scripts/COMPATIBILITY.md`](scripts/COMPATIBILITY.md) 登记的执行入口 |

Python 可以保存任务状态、计划、缓存和临时结果，但不能把这些当作场景事实。场景变更
必须以 manifest handler 的返回值或事件为准；C++ 仍然持有最终的场景、Actor、资源和
revision 权威状态。

编辑器 Python 业务通过 `api.editor_api` 使用公共契约；角色脚本、Blockly
和 Scratch 通过 `script_runtime.native_engine_adapter.get_script_runtime_adapter()` 使用受限子集。
`api.editor_api.get_script_runtime_adapter()` 仅作为旧调用方的兼容转发。旧 Python Scene 宿主只能通过
登记的 `runtime/legacy_*` adapter 访问 `runtime.legacy_scene_store`（旧
`CoronaCore.core.legacy_scene_store` 仅为兼容 alias），不得将 legacy 对象扩散到新业务代码。
MainView 的项目场景生命周期、宿主脚本初始化和旧相机跟随分别由
`plugins/MainView/compat/legacy_main_view_scene_adapter.py`（runtime 路径仅为 shim）、
`script_runtime/compat/legacy_script_runtime_adapter.py`（runtime 路径仅为 shim）和
`legacy_camera_follow.py` 承担。

Script Runtime 的 Engine 与 Blockly 旧场景访问分别通过统一的
`script_runtime/compat/legacy_scene_adapter.py`；旧的
`runtime/legacy_script_scene_adapter.py` 仅保留历史导入兼容转发；AITool 的旧场景 fallback 由
`plugins/AITool/compat/legacy_aitool_scene_adapter.py` 负责，旧的
`runtime/legacy_aitool_scene_adapter.py` 仅保留历史导入兼容转发；`native_scene_state.resolve_scene_value` 统一执行
native-first route 解析并转发该兼容入口，
不再直接持有旧 store 实现路径。AITool 的本地密钥和 `.env` 加载 canonical owner 是
`editor/plugins/AITool/configuration`，`plugins/AITool/compat/legacy_local_ai_setting.py` 和 `legacy_aitool_utils.py` 集中保留兼容 wrapper，`utils` 仅为旧路径 shim。

AITool 若必须支持旧宿主，只能通过
`native_scene_state.get_legacy_scene()` 或 `list_legacy_scene_routes()` 集中解析；业务
工具不得直接调用 legacy manager 的 `get()` / `list_all()`。

AITool 的 `cai_extensions/mcp/tools/native_scene_state.py` 中的 `NativeSceneRef`、
`NativeActorView` 和相关 `native_*` 函数是 C++ scene snapshot 的值对象 adapter。它们可以
提供 `get_position()`、`set_position()`、bounds 等便捷方法，但不等同于
`CoronaCore.core.legacy.entities.Actor/Camera/Scene`，也不持有 Python 场景事实。AITool 新代码应
优先使用这些值对象和 `scene.*`/`sceneTools.*` 契约；Agent actor 列表通过
`native_actor_views_with_legacy_fallback` 集中处理旧宿主回退；只有明确标记的旧宿主回退才可取得
`legacy_scene_store` 中的实体。

### Script Runtime

Script Runtime 的目录职责和删除边界见 `editor/script_runtime/BOUNDARY.md`；它是受权限控制的运行时，不是编辑器公共 API 的旁路。它可以获得角色脚本
需要的鼠标、射线、媒体等能力，但不自动获得编辑器场景、网络、LANChat 或 AI 能力。

Blockly 生成的 `CoronaEngine.lock_mouse()`、`camera_raycast()`、角色摄像机跟随等调用
属于 Script Runtime 的兼容命名空间，由 `script_runtime.engine` 承载；旧的
`CoronaCore.utils.corona_engine_scratch` 仅为转发入口，
不是 Vue 或编辑器 Python 的公共 API。新增积木能力必须先进入受限 Script Runtime
adapter，并明确权限、生命周期和宿主回退；不得把这些函数当作编辑器聚合契约的第二份来源。

`SceneDatas` 和旧 binding 只能作为明确标记的兼容路径。新增脚本能力必须单独确认值对象、
权限和生命周期，不能因为已有 C++ binding 就直接对 Vue 或普通 Python 开放。

## 目录和接口命名规则

目录名称只表示代码组织位置，不表示调用权限。权限以 manifest 的 `allowed_callers`
和 adapter 边界为准。

推荐使用：

- `scene.*`、`viewport.*`、`project.*`、`files.*` 等编辑器业务语义；
- `CoronaEditorApi.*` 与 `editorApi.*` 作为同一契约的 Python/JS 适配形式；
- `Engine Runtime` 名称仅用于底层原子能力；
- `legacy` 明确标识兼容入口、替代接口和删除条件。

不应把 `camera()`、`geometry()`、`Actor`、`Scene` 等底层对象包装成新的编辑器公共 API。
若一个实体方法表达的是编辑器业务语义，应先登记 manifest，再由实体或兼容宿主经 adapter
调用，而不是让实体方法成为跨层入口。

## 目录职责判定

目录迁移不是架构整理的起点。判断代码应放在哪里时，按“谁拥有状态、谁定义语义、谁
负责适配”划分：

| 代码类型 | 首选位置 | 判定依据 |
|---|---|---|
| 页面和输入交互 | `editor/Frontend` | 只管理 UI 状态，不保存场景事实 |
| 稳定编辑器业务语义 | manifest / Domain Service handler | 需要统一校验、revision、事务或事件 |
| Python 调用转换 | `api/editor_api.py` | 只映射公共契约，不新增业务规则；旧 `CoronaCore/core/editor_api.py` 仅为兼容别名 |
| AI/Agent/生成流程 | `plugins/AITool` | 生成任务和模型流程，不直接操作引擎对象 |
| 角色脚本/Blockly 能力 | Script Runtime adapter | 受权限和生命周期限制的运行时原子能力 |
| 旧宿主兼容 | 名称含 `legacy` 的集中 adapter | 有明确调用方、替代接口和删除条件 |

“backend”是历史目录名称，不代表 Python 是传统后端；它是
`compatibility / generated` 目录。Frontend Blockly 的文件职责和生成边界见
`editor/Frontend/src/blockly/BOUNDARY.md`。服务生命周期应放在 `editor/runtime`，Blockly 编排
应放在 `editor/script_runtime/blockly`，新业务不应仅因为目录名而放入 `editor/backend`。
`editor/backend/COMPATIBILITY.md` 是该目录的本地入口清单，记录每个 wrapper 的 canonical
owner、当前调用范围和删除条件；如果要删除旧入口，必须先满足清单中的外部宿主迁移条件。

### 迁移期对象的处理原则

以下对象属于“可运行但不可扩散”的迁移边界：

- `runtime/generated` 只承载 Script Runtime 生成文件，不承载新的 Python 业务模块；旧
  `backend/script` 和 `backend/runScript.py` 仅作为只读兼容回退，不再写入；
- `SceneDatas` 和 `legacy_scene_store` 只服务旧脚本宿主及登记的 fallback；
- `sceneService` 只提供 Vue 兼容别名，不能添加新的业务方法或直接调用 CEF；
- `corona_engine_api.h` 只作为旧 include 转发头，新代码必须使用 canonical engine header；
- Quasar 子模块由其自身生命周期管理，不属于本架构整理的可修改范围。

删除或移动这些对象前，必须先完成调用方迁移、回退测试和外部宿主确认；目录为空或
搜索结果暂时为零，不足以作为删除证据。

## 一套契约、多个调用边界

项目允许存在多个 adapter 和多个传输通道，但不允许存在多个业务契约来源。以下内容
必须保持唯一：

- 公共方法的业务语义和 owner；
- 参数、返回值、错误码、权限、revision 和事件顺序；
- 场景事实及其写入路径。

以下内容可以按调用边界不同而不同：

- JS/Python 的命名风格；
- CEF、Python wrapper 或 Script Runtime 的传输方式；
- legacy 宿主的兼容转换；
- UI 快速输入通道与业务命令通道的实现细节。

因此，看到同一语义在多个文件出现时，先检查它们是否都映射到同一个 manifest method；
只有出现第二份 schema、校验、状态机或事件语义时，才是架构重复，需要合并。

编辑器公共契约与 Script Runtime 不是“同一 API 的两份实现”：前者面向编辑器业务，
后者面向角色脚本的受限运行时。两者可以共享底层值对象或原子能力，但必须分别声明
权限、生命周期和可见范围；Script Runtime 不得借由已有 C++ binding 获得编辑器全部能力。

## 变更与维护规则

每个公共语义只维护：一个 owner、一个 Domain Service/handler、一份 manifest/schema、每个调用边界
一个薄 adapter，以及对应的边界测试。

变更顺序固定为：

```text
业务语义与 schema
        ↓
manifest / allowed_callers
        ↓
C++ Domain Service / handler
        ↓
Vue、Python、Script adapter
        ↓
调用方与边界测试
```

如果只改 wrapper 命名或传输方式，变更说明必须写明“无契约变化”。如果改动业务校验、
状态机、错误码、revision 或事件顺序，必须回到公共契约层评审，不能只在某个 adapter
中修补。

legacy 入口可以保留，但必须记录现有调用方、替代接口、保留原因、移除条件和回退测试。
没有完成迁移和验证前，不因目录看起来过时就批量删除或移动文件。

## 架构整理的执行顺序

架构整理按“契约先行、适配器随后、调用方最后、legacy 最后删除”的顺序推进。每个阶段都
必须保持旧宿主可运行；不能先移动目录或删除旧入口，再倒推功能是否仍然存在。

| 阶段 | 主要动作 | 完成证据 | 禁止事项 |
|---|---|---|---|
| 0. 冻结边界 | 在 `API_OWNERSHIP.md` 登记 owner、调用方、legacy 原因和删除条件 | 清单行完整，且能找到现有回退测试 | 新代码继续直接导入 Engine Runtime 或旧 binding |
| 1. 收敛契约 | 在 manifest/schema 和对应 handler 中定义唯一业务语义 | 参数、返回、错误、权限、revision、事件顺序均有来源 | 只在 Vue/Python wrapper 中增加业务校验或状态机 |
| 2. 收敛适配器 | Vue、Editor Python、Script Runtime 分别只做传输和权限边界转换 | adapter 映射测试通过，调用方不持有 native 对象 | 以 wrapper 名称不同为理由复制一套协议 |
| 3. 迁移调用方 | 逐个将生产调用切换到公共契约，保留显式 legacy fallback | 生产调用搜索清零或仅剩登记的兼容路径；功能回归通过 | 批量替换 `sceneDatas`、实体包装或底层 binding |
| 4. 删除 legacy | 在所有删除条件满足后移除入口和对应兼容测试 | 删除前后的行为、事件顺序和宿主启动回归通过 | 仅凭“当前搜索无结果”删除仍可能被旧宿主调用的入口 |

当前执行位置：阶段 2 已覆盖主要 `scene.*`、`sceneTools.*`、`viewport.*`、`network.*` 和
`lan_chat.*` adapter；阶段 3 已完成仓内 Vue 场景/视口生产调用迁移，正在收敛
Scratch/Blockly 兼容路径和少数 AITool fallback。`sceneService` 在仓内已无生产调用，
仍作为 bridge 导出保留给外部旧宿主；只有完成外部宿主确认、回退测试和发布兼容评估后，
才可删除。阶段状态必须以 `API_OWNERSHIP.md` 的当前调用方和回退测试为准，不能只根据
目录名称判断。

每完成一个迁移单元，必须同时更新 [API_OWNERSHIP.md](API_OWNERSHIP.md) 的调用方和删除条件，
并运行对应的边界测试。这样目录整理不会变成无法验证的全局重构。

## 验收清单

新增跨层能力必须具备：

1. manifest 方法、参数/返回 schema 和 `allowed_callers`；
2. 一个明确的 Domain Service/handler owner（C++ 或受控 Python service）；
3. 所需的 Vue/Python/Script 薄 adapter；
4. 明确的错误、revision 和事件语义；
5. 至少一个调用方边界测试；
6. 若保留旧入口，提供替代接口、迁移条件和兼容回退测试。

迁移完成前还要确认请求、返回、权限、事件顺序和测试替身行为没有意外变化，并通过相关
测试、必要的 C++ 编译和 `git diff --check`。
