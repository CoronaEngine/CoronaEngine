# Editor API Ownership 清单

本文是“公共语义 → owner → 调用方 → 生命周期”的登记表，不是第二份 API 定义，也不替代
C++ manifest。manifest 实现与 schema 位于 `src/systems/ui/editor_api/cef_editor_api.cpp`，是
参数、返回值、`allowed_callers`、错误和事件的唯一机器可读来源；本文只记录架构归属和
迁移决策。

## 权威来源与维护边界

| 内容 | 唯一权威来源 | 本文是否复制 |
|---|---|---:|
| 方法名、参数、返回值、错误、权限、事件 | C++ manifest/schema | 否 |
| 业务语义的 owner 和调用方 | 本文 | 是 |
| Vue/Python/Script 的命名和传输转换 | 各自薄 adapter | 仅记录位置 |
| 引擎场景、Actor、revision 等事实 | C++ Engine/Domain Service | 否 |
| 旧入口的保留、替代和删除条件 | 本文 legacy 清单 | 是 |

修改 manifest 后必须同步检查本文的 owner 和状态；修改 adapter 时必须证明仍指向原
manifest method。本文的示例和代表性清单不是可复制的 schema，也不能作为 API 数量统计
来源。

## 如何阅读

表中的每一行代表一个公共业务语义，不代表一套独立的语言 API。`Manifest method` 是唯一
契约标识；`Python adapter` 和 `Vue adapter` 只是同一方法在不同调用边界上的名称和传输
形式。例如：

```text
scene.set_actor_transform
├─ CoronaEditorApi.scene.set_actor_transform
└─ editorApi.scene.setActorTransform
```

两者必须共享同一套 schema、权限、错误、revision 和事件顺序。增加 wrapper 不增加公共
API 数量；增加独立业务规则才增加公共 API。

以下内容不计入公共 API 表的独立数量：

- `include/corona/engine/engine_runtime_api.h` 中的底层原子接口；
- C++ Domain Service 内部函数；
- Vue/Python/Script 的命名或传输适配器；
- 为旧宿主保留的 legacy binding 和 fallback。

它们必须要么属于内部实现，要么能追溯到本表中的某个公共语义。角色脚本专用的受限运行时能力可以独立于编辑器公共契约存在，但必须在权限和生命周期上明确标注，不能与编辑器 API 混用或隐式升级。

## 使用规则

先在 manifest 中确认接口是否存在、参数和调用权限，再在本表确认 owner、生产调用方和迁移状态。若一个入口不在 manifest 中，它只能是 Engine Runtime、内部 Domain Service、adapter 或 legacy 兼容实现，不能直接作为 Vue/Python 公共 API 发布。

本表的“核心已登记”部分只列出已完成边界收敛的代表性公共语义，不保证覆盖 manifest 的
全部方法；“尚未完成统一登记”部分用于阻止新代码扩散未审计接口。公共 API 总数和具体
schema 始终以 manifest 为准，不从本表手工统计。

历史 `CoronaCore` 导入路径的 owner、当前调用范围和删除条件集中记录在
[`CoronaCore/COMPATIBILITY.md`](CoronaCore/COMPATIBILITY.md)。该清单只描述兼容边界，
不定义第二份公共 API；`CoronaCore` 中的实体、组件和 manager wrapper 不能作为编辑器
业务接口使用。

## 新代码快速检查

提交跨层调用前，至少回答以下问题：

1. 这是已有 manifest method，还是新的业务语义？
2. 场景写入是否只经过 C++ owner？
3. Vue/Python 是否只是参数转换、调用和结果解包？
4. 是否绕过了 `CoronaCore`、Script Runtime 或 legacy adapter？
5. 若保留兼容入口，是否写明替代接口、调用方和删除条件？

任一问题无法回答时，先补充契约或 owner 记录，再实现调用方代码。

## 如何判断是否产生了“新 API”

项目只维护一套公共语义契约。以下情况只是同一接口的适配，不算新 API：

- JS 与 Python wrapper 的命名不同；
- Vue/Python 使用不同的传输机制；
- 测试替身或旧宿主的显式兼容回退；
- C++ handler 在内部组合多个 Engine Runtime API 原子操作。

只有出现新的业务语义、状态模型、错误协议或事件协议，才算新增公共 API。新增接口必须同时登记 manifest、Domain/aggregate handler、Vue/Python/Script adapter、权限和边界测试。

适配器数量不等于 API 数量。维护时以 manifest 的方法和事件语义计数，不以 C++ handler、Vue wrapper、Python 方法、Script 函数或兼容入口的数量计数。任何 adapter 都不能成为第二个契约来源；参数、返回值、错误和事件的权威定义只能存在于 manifest/schema。

### 快速判定表

| 变更内容 | 是否新增公共 API | 处理方式 |
|---|---:|---|
| JS `setActorTransform` 改为 Python `set_actor_transform` | 否 | 调整 adapter，继续指向同一 manifest 方法 |
| C++ handler 内部组合多个 Runtime 原子操作 | 否 | 保持为同一 Domain Service，不向上层泄漏 Runtime API |
| 新增编辑器业务动作、状态、错误码或事件 | 是 | 先修改 manifest/schema，再补 handler、adapter 和测试 |
| 旧宿主继续使用旧函数名 | 否 | 增加受限 legacy adapter，记录替代接口和移除条件 |
| Vue/Python 各自添加一套校验或状态机 | 不应发生 | 将规则统一回 C++ handler 或 schema |

判断标准是“调用方是否获得新的业务语义”，而不是“仓库里是否多了一个函数”。

## 状态和层次

### 生命周期状态

| 状态 | 含义 |
|---|---|
| `proposed` | 已设计但未承诺可用，不得被生产调用 |
| `active` | 当前生产公共接口 |
| `deprecated` | 有替代接口，仍保留兼容行为；必须记录替代接口和期限 |
| `removed` | 已删除；只能在迁移和测试完成后使用 |

### 所属层次

| 层次 | 说明 | 允许直接调用者 |
|---|---|---|
| `engine_internal` | C++ 引擎原子能力，如 Camera、Geometry、ECS、资源对象 | C++ Domain Service、Python 引擎适配层 |
| `domain_service` | 组合能力、校验、事务、revision 和事件 | 公共契约 handler |
| `public_contract` | manifest 中稳定的编辑器语义 | Vue、编辑器 Python、受限 Script Adapter |
| `adapter` | 公共契约的调用方 wrapper | 对应调用方 |
| `legacy` | 迁移期兼容入口 | 兼容层，不得新增业务调用 |

普通 Vue 页面和 AITool 业务代码不得直接依赖 `engine_internal` 或 `legacy`。

## 核心已登记的公共契约

下面的表格记录本轮已经按“公共语义契约 + 适配器”完成边界收敛的核心接口，不声称替代 manifest 的完整清单。未出现在本表中的方法仍以 `src/systems/ui/editor_api/cef_editor_api.cpp` 为准，并按下方“尚未完成统一登记的接口组”管理。接口数量变化必须以 manifest 的实际声明为准，不能手工维护另一份总表。

| 公共 API | 契约 handler/owner | Python adapter | Vue adapter | 当前调用方 | 状态 |
|---|---|---|---|---|---|
| `scene.get_snapshot` | `SceneTools.get_scene_snapshot` | `CoronaEditorApi.scene.get_snapshot` | `editorApi.scene.getSnapshot` | AITool 场景状态、VLM 包围盒、`camera_get`、`camera_list`、`camera_screenshot` | active |
| `scene.get_environment` / `scene.set_environment` | `SceneTools.get_environment` / `set_environment` | `CoronaEditorApi.scene.get_environment` / `set_environment` | — | Script Runtime Blockly 预览环境快照与恢复 | active / ScriptRuntime |
| `scene.list_routes` | `SceneTools.list_routes` | `CoronaEditorApi.scene.list_routes` | — | Script Runtime 场景路由列表和值对象 | active / ScriptRuntime |
| `scene.switch` | `SceneTools.switch` | `CoronaEditorApi.scene.switch` | — | Script Runtime 场景切换和值对象绑定 | active / ScriptRuntime |
| `scene.set_actor_transform` | `SceneTools.set_actor_transform` | `CoronaEditorApi.scene.set_actor_transform` | `editorApi.scene.setActorTransform` | AITool Actor 变换 | active |
| `scene.select_actor` | `SceneTools.select_actor` | `CoronaEditorApi.scene.select_actor` | `editorApi.sceneTools.selectActor` | 主视图、SceneBar、CameraView 的选择同步；可选 viewport context；发布 `actorSelectionChanged` | active |
| `viewport.capture` | `SceneTools.capture_viewport` | `CoronaEditorApi.viewport.capture` | `editorApi.viewport.capture` | AITool VLM 多视图、`camera_screenshot` | active |
| `viewport.set_camera_pose` | `SceneTools.set_camera_pose` | `CoronaEditorApi.viewport.set_camera_pose` | `editorApi.viewport.setCameraPose` | 编辑器相机命令、AITool `camera_move` | active |
| `network.lock_object` | `Network.lock_object` | `CoronaEditorApi.network.lock_object` | `editorApi.network.lockObject` | 协作对象锁 | active |
| `network.unlock_object` | `Network.unlock_object` | `CoronaEditorApi.network.unlock_object` | `editorApi.network.unlockObject` | 协作对象解锁 | active |
| `network.get_lock_owner` | `Network.get_lock_owner` | `CoronaEditorApi.network.get_lock_owner` | `editorApi.network.getLockOwner` | 协作锁状态查询 | active |
| `network.broadcast_intent` | `Network.broadcast_intent` | `CoronaEditorApi.network.broadcast_intent` | `editorApi.network.broadcastIntent` | LANChat/协作预览意图 | active |
| `network.check_preview_collision` | `Network.check_preview_collision` | `CoronaEditorApi.network.check_preview_collision` | `editorApi.network.checkPreviewCollision` | 协作预览碰撞 | active |
| `sceneTools.create_actor` | `SceneTools.create_actor` | `CoronaEditorApi.scene_tools.create_actor` | `editorApi.sceneTools.createActor` | AITool 场景生成、模型导入 | active |
| `sceneTools.create_scene` | C++ `SceneTools` native handler（创建 `.scene` 文件） | `CoronaEditorApi.scene_tools.create_scene` | `editorApi.sceneTools.createScene` | 外部旧宿主和底层资源创建调用 | compatibility；MainView 使用 `main.create_scene` |
| `sceneTools.remove_actor` | `SceneTools.remove_actor` | `CoronaEditorApi.scene_tools.remove_actor` | `editorApi.sceneTools.removeActor` | AITool 模型删除 | active |
| `sceneTools.focus_actor` | `SceneTools.focus_actor` | `CoronaEditorApi.scene_tools.focus_actor` | `editorApi.sceneTools.focusActor` | AITool `camera_focus`（新 manifest 宿主） | active |
| `sceneTools.set_actor_state` | `SceneTools.set_actor_state` | `CoronaEditorApi.scene_tools.set_actor_state` | `editorApi.sceneTools.setActorState` | SceneBar 可见性、Object 面板跟随摄像机 | active |
| `sceneTools.set_actor_camera_lock` | `SceneTools.set_actor_camera_lock` | `CoronaEditorApi.scene_tools.set_actor_camera_lock` | `editorApi.sceneTools.setActorCameraLock` | Object 面板摄像机跟随、位置/旋转偏移和持久化 | active |
| `sceneTools.save_actor` | `SceneTools.save_actor` | `CoronaEditorApi.scene_tools.save_actor` | `editorApi.sceneTools.saveActor` | Gizmo、Object 和 CameraView 的 Actor 持久化 | active |
| `sceneTools.select_model_file` | `SceneTools.select_model_file` | `CoronaEditorApi.scene_tools.select_model_file` | `editorApi.sceneTools.selectModelFile` | Object/SceneBar 资源选择 | active |
| `sceneTools.sun_direction` / `floor_grid` | `SceneTools.sun_direction` / `floor_grid` | `CoronaEditorApi.scene_tools.sun_direction` / `floor_grid` | `editorApi.sceneTools.sunDirection` / `floorGrid` | 主视图环境光和地面网格设置 | active |
| `sceneTools.set_physics_params` / `get_physics_params` | `SceneTools.set_physics_params` / `get_physics_params` | `CoronaEditorApi.scene_tools.set_physics_params` / `get_physics_params` | `editorApi.sceneTools.setPhysicsParams` / `getPhysicsParams` | 主视图场景物理参数设置和读取 | active |
| `sceneTools.set/get_render_backend`、`set/get_output_mode` | `SceneTools` 相机调试 handler | `CoronaEditorApi.scene_tools` 对应方法 | `editorApi.sceneTools` 对应方法 | CameraView/MainPage 渲染后端和输出模式 | active |
| `sceneTools.set/get_vision_render_mode`、`set/get_shadow_cascade_debug`、`set/get_ssao_enabled` | `SceneTools` 相机调试 handler | `CoronaEditorApi.scene_tools` 对应方法 | `editorApi.sceneTools` 对应方法 | CameraView/MainPage Vision、阴影和 SSAO 调试 | active |
| `sceneTools.create/open/close/rename/list/update_camera_view`、`delete_camera` | `SceneTools` 相机视图 handler | `CoronaEditorApi.scene_tools` 对应方法 | `editorApi.sceneTools` 对应方法 | CameraView 创建、打开、布局更新、重命名和删除 | active |
| `sceneTools.is_vision_available` / `load_vision_scene` | `SceneTools` Vision 基础 handler | `CoronaEditorApi.scene_tools.is_vision_available` / `load_vision_scene` | `editorApi.sceneTools.isVisionAvailable` / `loadVisionScene` | Vision 后端检测和场景加载 | active |
| `sceneTools.save_screenshot` | `SceneTools.save_screenshot` | `CoronaEditorApi.scene_tools.save_screenshot` | `editorApi.sceneTools.saveScreenshot` | CameraView/AI 截图输出 | active |
| `files.*`（文件树、创建、删除、重命名、打开） | `FileManager.*` | `CoronaEditorApi.files.*` | `editorApi.files.*` / `fileService.*` | 文件面板和项目资源浏览 | active |
| `project.*`（项目创建、打开、模式和加载状态） | `ProjectLauncher.*` | `CoronaEditorApi.project.*` | `editorApi.project.*`；`projectLauncherService.openProject` / `migrateLegacyScene` 仅负责生命周期编排 | 项目启动器、最近项目和新建项目 | active |
| `projectSettings.*` | C++ `ProjectSettings.*` native handler | `CoronaEditorApi.project_settings.*` | `editorApi.projectSettings.*` / `projectSettingsService.*` | 项目设置和场景文件选择 | active |
| `main.*`（主视图流程） | `MainView.*` | `CoronaEditorApi.main.*` | `editorApi.main.*` / `mainViewService.*` | 主视图初始化、运行项目和工具状态 | active |
| `main.scene_save` | `MainView.scene_save` | `CoronaEditorApi.main.scene_save` | `editorApi.main.sceneSave` | Python/Vue 场景保存 | active |
| `main.on_init` | C++ `MainView` native project/scene index handler | `CoronaEditorApi.main.on_init` | `editorApi.main.onInit`（如需） | MainView 首次激活项目、解析场景列表和 active scene | active |
| `main.create_scene` | C++ `MainView` native scene/project lifecycle handler | `CoronaEditorApi.main.create_scene` | `editorApi.main.createScene` | MainView 创建场景文件并更新 project.ini 场景索引 | active |
| `main.remove_scene` | C++ `MainView` native scene/project lifecycle handler | `CoronaEditorApi.main.remove_scene` | `editorApi.main.removeScene` | 删除非活动 `.scene` 文件并更新场景索引；活动场景先切换到后备场景 | active；portable scene lifecycle 仍待迁移 |
| `app.close_process` | `CoronaEditor.close_process` | `CoronaEditorApi.app.close_process` | `editorApi.app.closeProcess` | 编辑器进程关闭 | active |
| `resourceSearch.*`（索引、模糊/图像搜索、统计和 Actor 聚焦） | `ResourceSearch.*` | `CoronaEditorApi.resource_search.*` | `editorApi.resourceSearch.*` | 资源面板、模型检索和 Actor 聚焦 | active |
| `ai.submit_request` | `AITool.submit_request` | `CoronaEditorApi.ai.submit_request` | `editorApi.ai.submitRequest` | 对话、提醒、生成任务 | active |
| `lan_chat.start_room` | `LANChat.start_room` | `CoronaEditorApi.lan_chat.start_room` | `editorApi.lanChat.startRoom` | LANChat 房间启动 | active |
| `lan_chat.start_local_room` | `LANChat.start_local_room` | `CoronaEditorApi.lan_chat.start_local_room` | `editorApi.lanChat.startLocalRoom` | 本地房间启动 | active |
| `lan_chat.stop_room` / `stop_local_room` | `LANChat.stop_*` | `CoronaEditorApi.lan_chat.stop_*` | `editorApi.lanChat.stop*` | 房间停止 | active |
| `lan_chat.join_room` / `leave_room` | `LANChat.join_room` / `leave_room` | `CoronaEditorApi.lan_chat.join_room` / `leave_room` | `editorApi.lanChat.joinRoom` / `leaveRoom` | 房间加入和离开 | active |
| `lan_chat.send_message` | `LANChat.send_message` | `CoronaEditorApi.lan_chat.send_message` | `editorApi.lanChat.sendMessage` | 公共聊天消息 | active |
| `lan_chat.send_agent_reply` | `LANChat.send_agent_reply` | `CoronaEditorApi.lan_chat.send_agent_reply` | `editorApi.lanChat.sendAgentReply` | 可靠 Agent 回复 | active |
| `lan_chat.send_system_message` | `LANChat.send_system_message` | `CoronaEditorApi.lan_chat.send_system_message` | `editorApi.lanChat.sendSystemMessage` | 系统/GM 消息 | active |
| `lan_chat.send_system_message_to_host` | `LANChat.send_system_message_to_host` | `CoronaEditorApi.lan_chat.send_system_message_to_host` | `editorApi.lanChat.sendSystemMessageToHost` | Host 定向系统消息 | active |
| `lan_chat.send_system_message_to_user` | `LANChat.send_system_message_to_user` | `CoronaEditorApi.lan_chat.send_system_message_to_user` | `editorApi.lanChat.sendSystemMessageToUser` | 用户定向系统消息 | active |
| `lan_chat.poll_agent_trigger` | `LANChat.poll_agent_trigger` | `CoronaEditorApi.lan_chat.poll_agent_trigger` | `editorApi.lanChat.pollAgentTrigger` | Agent 触发队列 | active |
| `lan_chat.poll_coordinator_sync_message` | `LANChat.poll_coordinator_sync_message` | `CoronaEditorApi.lan_chat.poll_coordinator_sync_message` | `editorApi.lanChat.pollCoordinatorSyncMessage` | 协调器消息队列 | active |
| `lan_chat.poll_room_event` | `LANChat.poll_room_event` | `CoronaEditorApi.lan_chat.poll_room_event` | `editorApi.lanChat.pollRoomEvent` | 房间生命周期事件 | active |
| `lan_chat.poll_sync_event` | `LANChat.poll_sync_event` | `CoronaEditorApi.lan_chat.poll_sync_event` | `editorApi.lanChat.pollSyncEvent` | 场景/资源同步事件 | active |
| `lan_chat.get_history` / `list_history_rooms` / `load_history_room` | `LANChat.*` | `CoronaEditorApi.lan_chat.*` | `editorApi.lanChat.*` | 历史记录 | active |
| `lan_chat.add_agent` / `remove_agent` / `list_agents` | `LANChat.*` | `CoronaEditorApi.lan_chat.*` | `editorApi.lanChat.*` | AI Agent 名册 | active |
| `lan_chat.get_local_ip` | `LANChat.get_local_ip` | `CoronaEditorApi.lan_chat.get_local_ip` | `editorApi.lanChat.getLocalIp` | 本机网络信息 | active |

表中的“C++ handler”列表示 manifest 的分发/实现 owner，不强制表示所有业务代码都位于
C++。例如 `FileManager`、`MainView` 和部分 `AITool` 能力可以由受控 Python service
handler 实现，但仍必须由 C++ manifest 统一定义调用入口、schema、权限和错误边界。
`SceneTools` 负责场景/视口，`Network` 负责协作网络，`LANChat` 负责聊天和房间，
`AITool` 负责 AI 任务，`MainView` 负责编辑器主流程。若一个接口跨越多个模块，必须
新增明确的 Domain Service owner，不能让调用方自行组合多个公共接口来形成隐含语义。

## 尚未完成统一登记的接口组

这些接口已经存在并可能被现有 UI 或兼容运行时使用，但尚未全部完成逐项 owner、权限和迁移期限登记。它们不能被当作新的业务 API 扩散，也不能仅凭名称批量删除。

| 接口组 | 代表性 manifest wrapper | 当前归属 | 后续动作 |
|---|---|---|---|
| SceneDatas 兼容入口 | `scene_datas.get_scene`、`scene_datas.get_actor`、`scene_datas.actor_operation` | `plugins/SceneDatas/main.py` 注册壳、Scratch/旧脚本宿主兼容层 | 标记为 `legacy`；编辑器新代码必须使用 `scene.get_snapshot`、`scene.set_actor_transform` 或对应 `sceneTools.*` 聚合方法；Script Runtime 的受限物理操作使用 `actor_operation`，直到专用 ScriptRuntime 聚合 adapter 完成；完成旧宿主迁移和回退测试后再移除 |
| SceneTools 视口、相机和 Vision | `scene_tools.*`、`viewport.*` | SceneTools 聚合 handler；部分仍是 UI 专用能力 | `viewport.capture`、`viewport.set_camera_pose` 和 manifest 宿主的 `focus_actor` 已提供公共契约；旧宿主的 focus 回退仍保留，禁止向 Python 暴露引擎对象 |
| 协作同步和 pending 队列 | `network.*` 的 actor/snapshot/sync 方法 | Network handler 与 Vue 网络面板 | 将队列消息统一为值对象，补充 revision、所有权和事件顺序 |
| AI 和媒体入口 | `ai.*`、媒体/资源相关 wrapper | AITool 与编辑器入口 | 明确对话、提醒、生成任务的任务类型和生命周期 |
| Scratch / Blockly | `scratch.*` | 受限 Script Runtime 和兼容层 | 场景路由、环境快照、Actor 变换和编辑器相机已优先经过聚合 adapter；旧 Scene 只保留显式 fallback，其余脚本专用能力保持权限子集，禁止把兼容函数升级为编辑器公共 API |
| Script Runtime 媒体能力 | `CoronaEngine.import_media`、`play_audio`、`stop_audio` | `script_runtime.native_engine_adapter.get_script_runtime_adapter()`（兼容入口：`api.editor_api.get_script_runtime_adapter()`） | 角色脚本、Scratch/Blockly 的受限音频和媒体运行时能力；不属于编辑器 Public Contract，不能由 Vue 或普通编辑器 Python 直接调用 |
| AITool 旧 Agent 编辑路径 | `agent_adapter.py`、部分 `lanchat_agent_worker` 调整逻辑 | 迁移期值对象/兼容操作；由 runtime gate 控制 | Actor 列表和变换数据优先经过 `native_scene_state` 的 `scene.*` adapter；新增和稳定生产路径不得把 Python `scene_manager` 当作场景事实来源 |
| 场景组合 tier2 放置 | `flows/scene_composition_workflow_v2/nodes_tier_place.py` | `scene.get_snapshot` 值对象 + `native_actor_views_with_legacy_fallback` | 参考 Actor 的位置/缩放优先来自 native snapshot；旧 Actor 仅由集中 adapter 在旧宿主无聚合接口时回退 |
| Native 场景工具注册 | `mcp/tools/scene_tools.py`、`mcp/tools/place_object_near.py` | `native_scene_state` | 注册阶段不得导入 `scene_manager`；可保留可选兼容参数，旧 manager 只能在实际兼容执行路径中延迟解析 |
| Legacy Scene 解析 | `native_scene_state.resolve_scene_value`（legacy 查询由 `editor/plugins/AITool/compat/legacy_aitool_scene_adapter.py:get_legacy_scene` 承担） | 迁移期兼容 adapter | AITool 业务模块统一通过 native-first route resolver 取得场景值对象或旧 Python Scene；不得在 Agent、VLM、相机、多视图或场景组合模块复制 manager 查找逻辑 |
| Python Scene 宿主存储 | `editor/runtime/legacy_scene_store.py:legacy_scene_store` | 迁移期兼容 adapter | `CoronaEditor`、`SceneTools`、`MainView`、`plugins/FileManager/compat/legacy_file_scene_adapter`、Blockly 预览快照和 `corona_engine_scratch` 只能通过集中 adapter 访问旧 `scene_manager`；`CoronaCore/core/legacy_scene_store.py` 仅为兼容 alias；兼容 Actor 创建也必须经过该入口；待 C++ 场景生命周期契约覆盖后替换，不得扩散新的直接导入 |
| 物理沉降兼容路径 | `flows/scene_composition_workflow_v2/nodes_tier_place.py` 的 `_apply_physics_settlement` | `native_scene_state.set_actor_physics_value`；native 路径转发 `sceneTools.setActorPhysics`，旧宿主由同一 adapter 转发 Mechanics | 沉降时序和参数保持原行为；Tier2 不直接访问 `_mechanics`，物理值对象和 legacy fallback 只在 adapter 内维护 |
| Mechanics 诊断读取 | `flows/scene_composition_workflow_v2/nodes_tier_place.py` 的 `_verify_mechanics_available` | `native_scene_state.NativeActorView.mechanics`、`scene.get_snapshot` 中的 `actor.mechanics` 与 bounds 值对象 | 只读诊断优先使用 native snapshot；旧 Actor 读取仅作没有 Mechanics snapshot 字段的宿主兼容回退 |
| Actor 物理聚合命令 | `sceneTools.setActorPhysics` / `scene_tools.set_actor_physics` | `active` | 参数为 `scene_name`、`actor_name` 和 physics value object；C++ 统一校验、持久化、递增 `actor_version` 并发布现有 `SceneTools.actorChanged` 事件；旧沉降回退仅供无该 manifest 的宿主使用；不得以 `scene_datas.actor_operation` 作为替代 |
| Actor 摄像机跟随聚合命令 | `sceneTools.setActorCameraLock` / `scene_tools.set_actor_camera_lock` | `active` | C++ 统一管理启用状态、位置/旋转偏移、`CameraFollowController`、持久化、`actor_version` 和 Actor 变更事件；旋转偏移当前只保存和返回，跟随控制器暂不使用旋转偏移 |

这一区分了“manifest 中已存在”与“已经完成架构收敛”两个概念。新的调用方只能使用核心表中已登记的契约，或先完成新增接口验收条件；未分类接口只能由其现有 owner 和兼容调用方继续使用。`viewport.set_camera_pose` 的生产调用已通过公共适配器完成；旧宿主仍可通过显式兼容适配器提供同一语义。

### 当前 Vue legacy 调用点

当前没有已知的生产 Vue 调用点应继续使用 `sceneDatas.actorOperation`。旧入口仍保留给
明确的兼容宿主，但不代表它是新的公共 API，也不得复制到 Vue 或普通编辑器 Python：

| 调用点 | 当前用途 | 替代契约 |
|---|---|---|
| `scene_datas.actor_operation` 的旧调用方 | 旧脚本/兼容宿主的 `SetCameraLock*` 等操作 | `sceneTools.setActorCameraLock`；Vue 的摄像机锁定已迁移，旧入口仅作为兼容回退 |

旧入口保留是为了兼容尚未迁移的脚本宿主。它现在由同一 native 状态和
`CameraFollowController` 实现，避免新旧入口产生两套行为；新功能不得新增表外调用。

当前仓内 Vue 生产代码的兼容别名状态：

| 入口 | 仓内生产调用 | 当前处理 | 删除条件 |
|---|---:|---|---|
| `Frontend/src/utils/bridge.js` 中的 service aliases（`sceneService`、`projectService`、`appService`、`lanChatService`、`networkService`、`scriptingService`、`aiService`、`projectLauncherService`、`fileService`、`projectSettingsService`、`resourceService`、`logService`） | 0 | 统一从 `Frontend/src/services` re-export 给外部旧宿主；实现不再分散到兼容目录 | 外部宿主确认迁移、兼容回归通过、发布说明允许按 alias 分批移除 |

## 适配器和实现归属

| 适配器/实现 | 文件或目录 | 规则 |
|---|---|---|
| Vue adapter | `editor/Frontend/src/utils/bridge.js` | 只组装参数、调用 manifest、解包响应和订阅事件 |
| Python plugin base | `editor/runtime/plugin_base.py`（兼容 owner：`editor/CoronaPlugin/compat/legacy_plugin_base.py`；旧 alias：`core/corona_plugin_base.py`） | 只保留历史 `PluginBase.register_web` 装饰器标记；服务必须由 `runtime.registry` 显式注册，不在此执行 RPC 或场景逻辑 |
| Editor Python adapter | `editor/api/editor_api.py` | 只通过 `python_wrapper` 调用公共契约；旧 `CoronaCore/core/editor_api.py` 仅为兼容模块别名 |
| Legacy SceneDatas adapter | `editor/script_runtime/compat/legacy_scene_datas_adapter.py:LegacySceneDatasApi`（兼容 alias：`editor/script_runtime/legacy_scene_datas_adapter.py`、`editor/CoronaCore/core/legacy_scene_datas_adapter.py`、`editor/CoronaCore/core/legacy_editor_api.py`） | canonical 唯一实现；仅通过注入的 Script Runtime C++ channel 转发 `scene_datas.*`；不属于普通 Editor Python API，也不定义新的场景语义 |
| ScriptRuntime manifest adapter | `editor/script_runtime/manifest_adapter.py:ScriptRuntimeEditorApi`（兼容 alias：`editor/CoronaCore/core/script_runtime_editor_api.py`） | canonical 唯一实现；复用 `scene.*`、`sceneTools.*`、`viewport.*` 的同一 schema，通过 ScriptRuntime channel 调用；不复制业务协议或扩大到普通脚本能力 |
| Legacy scene-store adapter | `editor/runtime/legacy_scene_store.py`（兼容 alias：`editor/CoronaCore/core/legacy_scene_store.py`） | 集中保留旧 Python 场景宿主；不属于新的公共编辑器 API |
| Legacy Vision import adapter | `editor/plugins/SceneTools/compat/legacy_vision_import_adapter.py` + `legacy_vision_scene_adapter.py`（runtime 路径仅为兼容 alias） | SceneTools 集中保留旧 Vision 导入、derived 文件、代理 Actor 和 legacy Scene 查询；仅供旧 Web 兼容入口，不定义新的公共场景 API |
| Legacy MainView scene adapter | `editor/plugins/MainView/compat/legacy_main_view_scene_adapter.py`（runtime 路径仅为兼容 alias） | MainView 只保留项目页面编排；旧 Python Scene 关闭和外部旧宿主兼容集中在此，不定义新的场景公共 API；初始化、创建、切换和文件删除已走 native contract |
| Script Runtime host initialization | `editor/script_runtime/engine/host.py` | 负责 `ScriptsManager` 生命周期初始化编排；旧 Scene 查询仍经登记的 `compat/legacy_scene_adapter.py`；历史 host import shim 已删除 |
| Script Runtime legacy scene adapter | `editor/script_runtime/compat/legacy_scene_adapter.py` | Script Runtime 访问旧 Python Scene Store 的唯一兼容实现；`script_runtime/engine` 和 `script_runtime/blockly` 只能通过 canonical adapter 访问，adapter 不定义新的场景语义；历史 runtime shim 已删除 |
| Legacy AITool scene adapter | `editor/plugins/AITool/compat/legacy_aitool_scene_adapter.py`（runtime 路径仅为兼容 alias） | AITool 旧 Python Scene fallback 的唯一 owner；native scene 解析文件只转发兼容入口，不定义第二套场景查找协议 |
| AITool configuration adapter | `editor/plugins/AITool/configuration/local_secrets.py` | `.env` 和环境变量 API key 的唯一加载 owner；`compat/legacy_local_ai_setting.py`、`legacy_aitool_utils.py` 仅保留外部历史 import，`utils` 旧路径直接导入 canonical owner，不得新增配置语义 |
| Python engine adapter | `editor/runtime/legacy_engine_adapter.py`、`runtime/legacy/entities`、`runtime/legacy/components` | 由 `legacy_engine_adapter.py` 集中解析底层运行时，供旧实体/组件和登记的 legacy/test 注入兼容；不是普通编辑器或 AITool API；旧 `CoronaCore/core/engine_runtime.py` 仅兼容转发 |
| Legacy Network/LANChat adapter | `editor/runtime/legacy_network_adapters.py` | 集中保留注入式旧 native Network、LANChat 和队列 fallback，并规范化 manifest LANChat transport/queue adapter；公共生产调用必须经过 `CoronaEditorApi.network` / `CoronaEditorApi.lan_chat`，不得在此复制 manifest schema |
| Legacy Scene/SceneTools/Viewport adapter | `editor/runtime/legacy_scene_adapters.py` | 集中保留注入式旧 native 场景、Actor 创建/删除、相机截图/姿态 fallback；公共生产调用必须经过 `CoronaEditorApi.scene` / `scene_tools` / `viewport`，不得在此复制 manifest schema |
| Native scene value adapter | `editor/plugins/AITool/cai_extensions/mcp/tools/native_scene_state.py` | 将 `scene.get_snapshot` 的值对象包装成只读/命令式视图，集中 native-first 场景 route 解析和 actor-view legacy fallback；不持有 Python Scene 实体，不构成第二套场景 API |
| Legacy camera-lock adapter | `runtime/editor_host.py:CoronaEditor.camera_lock_set` | 旧 CEF 面板入口；优先转发 `sceneTools.setActorCameraLock`，仅在旧宿主缺少契约时保留清理回退，不定义新的状态协议 |
| Legacy camera-follow adapter | `runtime/legacy_camera_follow.py:update_camera_follow`，由 `runtime/editor_host.py:CoronaEditor._update_camera_follow` 兼容转发 | 集中保留旧 CEF/宿主逐帧相机跟随、WASD 和右键拖动逻辑；访问 `legacy_scene_store`，不定义新的编辑器公共 API |
| Realtime viewport adapter | `editor/Frontend/src/utils/viewport*.js`、`MainPage.vue`、`CameraView.vue` | 仅传输低延迟输入、句柄和视口值对象；不定义持久化、权限、revision 或业务错误语义；不得扩散到普通业务组件 |
| Script adapter | `script_runtime/native_engine_adapter.py`、`script_runtime/engine`、`script_runtime/blockly`、`corona_engine_scratch.py` | 只暴露角色脚本允许的受限能力；鼠标、射线和音频等运行时原子能力必须经过该边界，不属于编辑器 Public Contract；`api.editor_api.get_script_runtime_adapter` 仅为兼容入口，旧 `CoronaCore/core/scripts_system` 仅兼容转发 |
| Generated script runner | `script_runtime/runner.py:run_generated_script` | 集中加载和清理 `runtime/generated/blockly_code.py`；旧 `backend/runScript.py`、`backend/script` 仅作为只读回退；MainView 不直接导入兼容包 |
| Project template helper | `editor/runtime/project_templates.py` | 提供模板复制、路径规范化和 project.ini 初始化；不定义 manifest schema、场景权威状态或公共 API；`CoronaCore` project utils 仅兼容转发 |
| Scene persistence helper | `editor/runtime/scene_support.py` | 提供场景清单和 legacy 自动保存；不拥有 native Scene/Actor 状态；旧 project-support 聚合转发已删除 |
| Archive parser | `editor/runtime/archive` | 只解析项目归档并生成校验后的快照；不创建 Engine/Scene/Actor；`ProjectArchive` 和 legacy Scene 共享此实现，旧 `CoronaCore/archive` 仅兼容转发 |
| Project template assets | `editor/plugins/ProjectLauncher/templates` | ProjectLauncher 唯一模板 owner；Python/C++ 创建项目流程必须使用此路径，不在 `CoronaCore` 或 C++ handler 中复制模板 |
| Aggregate handler 内部实现 | C++ `SceneTools` manifest；旧 Python facade owner 位于 `editor/plugins/SceneTools/main.py` | 通过 `editor_api` 使用 manifest 聚合契约；不得把内部 Engine adapter 重新扩散为跨层业务入口 |
| Internal Engine adapter | `editor/runtime/legacy_engine_adapter.py:EditorEngineAdapter`（兼容 alias：`editor/CoronaCore/core/engine_runtime.py`） | 当前无普通编辑器生产调用，仅保留给明确登记的外部 legacy/test 注入；不是新的编辑器公共 API，新增生产调用前必须先补 manifest 聚合契约 |
| Editor host lifecycle | `editor/runtime/editor_host.py:CoronaEditor` | 仅负责嵌入式宿主生命周期、服务注册和兼容事件桥；不是 Vue/Python 业务 API；旧 `CoronaCore.core.corona_editor` 仅为导入兼容 |
| 配置入口 | `config.paths_config`（路径模型）、`config.project_state.settings_manager`（编辑器设置）、`runtime/legacy_editor_adapters.py:get_active_project_path`（活动项目兼容读取） | 路径模型和编辑器设置分工明确；`config.settings` 与 `utils.settings` 仅为兼容转发，兼容宿主属性只在 runtime adapter 内作为回退 |

`editor/Frontend/src/utils/bridge.js` 中的 service aliases 不单独登记为公共 API。它们只是
`editorApi` namespace 的 Vue 兼容 facade；新增能力必须添加到对应的 manifest wrapper 或
正式 domain service，不能只添加到 alias。当前仓内 Vue 生产调用已迁移到正式 API/service
owner，这些 alias 只为外部旧宿主保留。删除前必须完成外部宿主确认、行为等价回归和发布
兼容评估；仓内搜索清零本身不是充分删除条件。

其中，`projectService.js` 的仓内调用方已在 `ed31aed7` 中完成收敛：项目和主视图方法由
`editorApi.main` 直接承载，拖拽区域 Dock 方法由 canonical `appService` 承载；`MainPage`、
`SceneBar`、`DockTitleBar`、`Pet` 和 `CameraView` 不再导入旧 compat facade。相关 service
名称仍可由 `bridge.js` 为外部旧宿主提供，但实现统一位于 `src/services`。

`scriptingService.js` 的仓内调用方已在 `e5aca9b6` 中完成收敛：`App`、`MainPage`、
`CameraView`、Blockly 工作区、节点图工作区和节点图运行时均直接使用 `editorApi.scratch`。
该 facade 仍通过 `bridge.js` 为外部旧宿主保留；Script Runtime 的权限边界和受限能力不因
Vue adapter 迁移而扩大。

`projectSettingsService.js` 的仓内调用方已在 `bc5ca452` 中完成收敛：`NodeGraphWorkspace`、
`NewGame`、`Network` 和 `ProjectSettings` 均直接使用 `editorApi.projectSettings`。该 facade
仍通过 `bridge.js` 为外部旧宿主保留，项目设置的 schema、校验和状态语义没有复制到 Vue。

`projectLauncherService` 不属于可直接删除的纯 facade：`openProject` 负责切换前节点图保存、
活动项目 localStorage 和 `corona-active-project-changed` 事件，`migrateLegacyScene` 负责迁移
成功后的活动项目状态更新。`bf4397a0` 已将其余纯项目操作（查询、创建、模式设置、打开文件）
改为直接调用 `editorApi.project`；因此后续只能继续收敛生命周期编排，不能把这些状态副作用
复制到每个 Vue 页面。

`fileService.js` 的仓内 FileManager 调用已在 `f53db092` 中完成收敛，文件树、创建、删除、重命名
和打开文件均直接使用 `editorApi.files`。`FileManager` 保留对
`projectLauncherService.migrateLegacyScene` 的调用是有意的：它属于项目迁移生命周期，而不是
文件契约的第二个 owner。

`resourceService.js` 保留为 active resource-search gate，而不是待删除的纯兼容 facade：它集中
维护当前禁用开关、`SceneBar` caller 标识和稳定的 disabled 响应。启用时所有请求仍映射到
`editorApi.resourceSearch`；只有开关和兼容响应策略属于该 gate 的职责，schema、权限和资源
搜索语义仍由 manifest/`ResourceSearch.*` owner 定义。

`appService` 的实现位于 `Frontend/src/services/appService.js`，集中 Dock/window transport
和进程操作；`aiService`、
`networkService` 和 `lanChatService` 仍属于 active adapter，分别集中 AI operation/响应、
Network response 和 LANChat response 的边界转换。它们不能成为第二份 manifest schema、权限、
状态机或引擎事实 owner；`appService` 和其他 service 的 `bridge.js` re-export 仅为外部旧宿主保留。

AITool 的旧场景兼容查找也必须集中在 `native_scene_state`：业务工具可以接收旧宿主注入的
manager 作为兼容参数，但不得直接调用其 `get()` 或 `list_all()`；这样可以在不改变旧宿主
行为的前提下，避免 legacy 存储知识继续扩散。

Blockly 生成的 `CoronaEngine.*` 函数属于 `ScriptRuntime` 命名空间，实际实现和权限边界
由 `corona_engine_scratch.py` 与 `get_script_runtime_adapter()` 负责；它们不计入编辑器
Public Contract，也不能被 Vue、AITool 或普通编辑器 Python 直接调用。

适配器默认在 `editor_api.py` 或登记的 runtime adapter 内解析嵌入式宿主；包括 AITool 在内的 Python 业务代码不得导入 `CoronaEditor` 来取得 `CoronaEngine`，也不得把 native engine 作为新的业务层依赖。若外部 legacy 宿主或测试替身确实无法调用 manifest，才可显式注入 `EditorEngineAdapter`；已登记的旧场景回退必须通过对应 `legacy_*` adapter。

`runtime/legacy/entities`、`components` 和 `managers` 的底层对象包装属于 `engine_internal`/Script Runtime Adapter，不属于 `public_contract`。它们的允许调用方仅限角色脚本、Blockly/Scratch、旧 Scene 宿主和引擎适配层；AITool、普通 Python 服务和 Vue 生产代码不得直接导入。`CoronaCore/core/legacy` 及 `CoronaCore/core/entities`、`components`、`managers` 仅保留历史导入兼容转发，不得新增调用。若实体包装中出现编辑器场景语义，应先在 manifest 中登记对应聚合命令，再由实体或旧宿主通过 adapter 调用，而不是把实体方法升级为新的跨层 API。
LANChat worker/host action executor 的消息、队列和协作状态也必须通过 `get_network_adapter()`、`get_lan_chat_adapter()`、`get_lan_chat_transport_adapter()` 等入口；AITool runtime 不再导入或持有原生 `CoronaEngine`，`corona_engine` 参数仅保留为旧宿主注入和测试替身的兼容参数。
`services/composition_root.py` 是 AITool 可选集成的组合根工厂；worker 组合根对
`engine_write_gate` 采用一次性 lazy 注入，HostActionExecutor 和各 Runtime provider
共享同一 worker gate，不得在 service 或 provider 内重复构造。ModelProvider 和 scene
classifier 也只能由该组合根加载后注入。

`services/runtime_query_policy.py` 是 AgentRuntime 文本命令、状态查询和 GM 触发器识别的
唯一 owner。它只做输入归一化和无状态判断，不访问 worker、场景或网络状态；
`lanchat_agent_worker.py` 只负责消息编排，并通过同名静态/类静态方法保留旧调用方兼容。
新增查询词、命令或触发条件必须先在该策略模块和边界测试中登记，不能重新写回 worker。

`services/runtime_result_policy.py` 是 AgentRuntime 图/批次响应形状归一化的唯一 owner，
只处理 `graphs`/`graph`/`queued` 以及对应批次字段，不负责 Runtime 查询、业务状态或用户回复。
worker 可以保留同名兼容转发，但不得在处理器中重新实现这些 fallback 形状。

`services/runtime_report_policy.py` 是 AgentRuntime 场景摘要、context、execution reply composition、intervention digest/reply/summary、scene snapshot、resource stage、
resource adapter/readiness、环境、资源批次 flow、batch tooling、geometry、command、review summary/proposal/confirmation、layout confirmation、layout、engine-write report/readiness/boundary、report health、fact-source boundary、closure、import stage、actor-import boundary、tool queue health、世界对账和工具执行摘要的格式化 owner。它只接收结构化摘要，输出有限长度、脱敏后的展示文本；`services/runtime_sync_policy.py` 另外负责 Runtime Sync aggregate report、GM Sync replay digest、actor/asset row、health 和 asset transfer 展示格式化。
Runtime 查询、消息路由和回复生命周期仍归 worker。新增报告字段应在该模块及其边界测试中
登记，不能在 worker 中复制格式化和敏感字段过滤规则。

`lanchat_agent_worker.py` 中的 `_agent_runtime_evidence_summary` 仍属于结果适配边界：它从
AgentRuntime 原始结果读取 `report`、图和批次字段，生成 execution reply 所需的结构化 evidence；
它不是展示 formatter，不能直接移入 `runtime_report_policy.py`。只有当同一份 evidence 被多个
调用方复用时，才单独抽取为 result/evidence policy，避免为了目录整齐新增重复的跨层 API。

`services/runtime_replay_report_policy.py` 是 replay 总体 composition、命令、工具执行、队列、state patch、worker-drain、tool graph、GM summary 和
guard 摘要的格式化 owner。它只接收 replay summary，不负责回放、Runtime 查询或消息路由；
worker 的同名方法只是兼容转发。replay 展示字段和脱敏规则应集中在该模块维护。

`services/runtime_replay_lifecycle_policy.py` 是 replay plan lifecycle、intervention 和
geometry 摘要的格式化 owner。它只处理结构化计数、状态和最新事件，不执行回放或修改
Runtime 状态；新增生命周期展示字段应在该模块及其边界测试中登记。

`services/runtime_replay_event_policy.py` 是 RuntimeEvent replay 摘要的格式化 owner，负责
event rows、事件计数、GM runtime-event digest、report-ready、环境/引擎失败和 disclosure skip 的有限展示与脱敏。它不发送事件、
查询 Runtime 或决定 disclosure 权限；这些职责仍属于 worker/runtime，worker 只保留兼容转发。

`services/runtime_replay_detail_policy.py` 是 replay failure strategy、layout、VLM、review
advisory 和 final adjustment 摘要的格式化 owner。它只接收结构化 replay summary，不能执行
回放、修改 Runtime 状态或自行读取敏感配置；详细展示字段和边界测试集中维护在该模块。

`services/runtime_replay_resource_policy.py` 是 replay environment 和 resource readiness 摘要
的格式化 owner，负责资源事件、发布/查询就绪计数和有限字段脱敏。它不查询 Runtime、发布
事件或决定 provider 状态；worker/runtime 仍拥有这些运行时职责。

`services/runtime_replay_transfer_policy.py` 是 replay asset transfer 摘要的格式化 owner，
负责传输事件、启动/进度/完成/失败计数、分块进度和字节摘要。它只接收结构化 summary，
不读取 Runtime、不暴露资产 ID、peer ID、路径或其他资源身份；worker 的同名方法只保留
兼容转发。资产传输的展示字段和边界测试应集中在该模块维护。

`services/runtime_replay_peer_sync_policy.py` 是 replay peer sync 摘要的格式化 owner，负责
peer 事件、房间状态和 sync/state reconcile 计数。它只接收结构化 summary，不读取 Runtime，
不暴露 peer ID、消息 ID 或其他身份字段；worker 的同名方法只保留兼容转发。peer sync 展示
字段和边界测试应集中在该模块维护。

`services/runtime_sync_policy.py` 是 Runtime Sync actor/asset 行预览的格式化 owner，负责
将 Runtime 已提供的最新行限制为最多五项并生成 actor 生命周期、asset 传输状态、分块和
字节进度文本；同时负责 GM Sync replay digest、sync health 和聚合 asset transfer 摘要。它不查询 Runtime、不执行
同步或路由消息；worker 的同名方法只保留兼容转发。新增行字段、健康计数和展示规则应在
该模块及其边界测试中维护。

`services/runtime_replay_sync_policy.py` 是 Runtime Sync replay 摘要的格式化 owner，负责
同步记录、actor/peer 变更、传输进度和错误码的有限展示与脱敏。它只接收结构化 summary，
不执行回放、不读取 Runtime 或暴露 provider、prompt、URL 等内部标签；worker 的同名方法
只保留兼容转发。同步回放字段和脱敏规则应集中在该模块及其边界测试中维护。

`services/runtime_message_delivery_policy.py` 是 Runtime message delivery 摘要的格式化 owner，
负责投递请求/成功/失败计数、消息类型、通道、阶段和失败码展示，并保留
`redact_agent_reply` 兼容开关。它不发送消息、不读取 Runtime；worker 的同名方法只保留
兼容转发。消息投递字段和脱敏规则应集中在该模块及其边界测试中维护。

AITool 场景规划的 `scene_element_classifier` 属于 `cai_extensions/agent` 集成实现；
`services/lanchat_scene_runtime.py` 只接收注入的分类器并在未配置时使用本地 fallback，
不得反向导入 Quasar/Agent 集成包。加载和配置由 `services/lanchat_agent_worker.py`
组合根负责，并使用轻量文件加载避免执行 `cai_extensions.agent` 的包级工作流初始化。
`services/agent_runtime/tools.py` 的规划和分类工具同样只接收 `AgentRuntime` 注入的分类器；
直接构造 `AgentRuntime` 时使用规则 fallback，不能在 service 内重新加载集成模块。

## 迁移中的 legacy 入口

以下表格是 legacy 生命周期登记表。表外的旧入口不得新增调用；表内入口也只能由“当前调用方”使用。
“删除条件”必须全部满足后才能移除，不能仅凭目录名称或搜索结果批量删除。

| Legacy 入口 | 当前调用方 | 替代接口 | 保留原因 | 删除条件 | 回退测试 |
|---|---|---|---|---|---|
| `AITool/cai_extensions/mcp/tools/model_import_tools.py` 的 `create_editor_actor` / `remove_editor_actor` 回退 | 无新宿主的旧 AITool/测试替身 | `sceneTools.createActor` / `sceneTools.removeActor`，经 `get_scene_tools_adapter()` | 兼容没有 manifest 聚合接口的旧宿主 | 所有受支持宿主均提供聚合接口，且旧宿主创建/删除回退测试移除 | model-import boundary tests |
| `plugins/SceneDatas/main.py` 的兼容插件壳 | `runtime/registry.py` 的历史服务注册和旧 CEF 宿主；Vue Object panel 已脱离 Object panel ID 的历史绑定 | `scene.*` / `sceneTools.*` manifest 与 native scene/project lifecycle；Object 面板 does not call SceneDatas API | 保留旧宿主的服务名和启动兼容；壳本身不定义 Scene API，SceneDatas native lifecycle 尚未完成 | native lifecycle 覆盖旧面板初始化、场景/Actor 绑定、读写、切换和关闭，并通过旧宿主回归；之后移除注册和兼容壳 | SceneTools native API、Frontend panel 和 runtime registry boundary tests |
| AITool 协作服务中的旧网络方法 | 旧宿主、测试替身 | `network.*`、`lan_chat.*` adapter | 保留旧 worker/host action 的消息格式 | 所有 worker 和 host action 都只使用值对象 adapter，旧消息回退测试不再需要 | collaboration API boundary tests |
| `AITool/cai_extensions/mcp/tools/native_scene_state.py` 的场景树通知 | AITool 旧宿主事件桥 | C++ handler 的统一 Actor/Scene change 事件 | 统一事件发布尚未覆盖所有旧宿主 | C++ 事件覆盖快照、树变化和错误顺序，并通过事件回归测试 | native-scene-state boundary tests |
| `plugins/MainView` 的 `emit_compat_editor_event` 场景切换事件 | 旧前端宿主 | `project.*` / `scene.*` manifest 事件 | 兼容旧页面的事件订阅 | 旧页面不再订阅兼容事件，且新事件顺序回归通过 | MainView API boundary tests |
| `plugins/MainView/compat/legacy_main_view_scene_adapter.py` 的 Python Scene 生命周期 | MainView 的旧关闭兼容路径及外部旧宿主 | `main.on_init`、`main.create_scene`、`sceneTools.reload_scene` 与 `main.remove_scene` 的 native 生命周期 | 保留旧宿主对 Python Scene 对象和兼容事件的依赖；MainView 已不再用 adapter 初始化、创建或切换场景；runtime 路径仅作 shim | 外部宿主完成关闭迁移，并通过首次加载、创建、删除、切换和 Vision 回归后删除 adapter | Python runtime boundary tests |
| `script_runtime/compat/legacy_scene_adapter.py` 的旧 Scene Store 访问 | `script_runtime/engine/corona_engine.py` 和 `script_runtime/blockly/main.py` 的旧项目/角色脚本兼容路径 | native scene/project 生命周期与 Script Runtime 受限聚合 adapter | 保留旧生成脚本对 Python Scene 的读取、查找和切换兼容；Script Runtime 主模块不再持有 store 实现路径；旧 runtime 路径仅作 shim | 旧生成脚本完成 native scene/project 迁移，并通过目标解析、Actor 查找、场景切换回归 | Script Runtime legacy scene boundary tests |
| `backend/script`、`backend/runScript.py` 历史生成输出 | 旧宿主和 `script_runtime.runner` 的兼容回退 | `runtime/generated` 与 `script_runtime.runner.run_generated_script` | 保留既有生成脚本和旧宿主的只读加载能力；Blockly workspace、持久化脚本和 manifest 写入项目 `Scripts/blockly`，预览临时脚本写入 `runtime/generated` | 外部旧宿主不再读取旧路径、兼容回退测试移除，且发布说明允许清理历史运行时数据后删除 | `script_runtime/tests/test_script_runtime_runner.py`、路径边界测试 |
| `plugins/AITool/compat/legacy_aitool_scene_adapter.py` 的旧 Scene fallback | AITool native scene 兼容入口及其现有调用方 | `scene.get_snapshot` 和 AITool native value adapters | 保留旧宿主 fallback；`native_scene_state.py` 只负责 native 快照、Actor view 和兼容转发；runtime 路径仅作 shim | AITool 所有受支持流程切换到 native scene/value object，并通过 legacy fallback 删除回归 | AITool legacy scene boundary tests |
| `model_reviewer._get_current_scene` 的场景解析 | 模型审查截图前的当前场景查询 | `native_scene_state.resolve_scene_value` | 已移除审查模块内的重复 native/legacy 查询；截图和审查流程不变 | 统一 resolver 覆盖所有受支持宿主后，随同其他旧场景回退一起清理 | model reviewer API boundary、native scene state tests |
| `vlm_capture._resolve_scene` 的场景解析 | VLM 多视图截图前的当前场景查询 | `native_scene_state.resolve_scene_value` | 已移除 VLM 模块内的重复 native/legacy 查询；无场景时仍返回本地空 route 值对象 | 统一 resolver 覆盖所有受支持宿主后，随同其他旧场景回退一起清理 | VLM/camera scene boundary、native scene state tests |
| `scene_composer_progressive._get_current_scene` 的只读场景视图 | Progressive layout 的 Actor 查询和 AABB 读取 | `native_scene_state.NativeSceneRef` / `resolve_scene_value` | 共享 route、Actor 查找和 legacy fallback；不改变 Progressive 的布局或物理流程 | Progressive 所有场景读取完成 native value-object 迁移后清理旧宿主回退 | progressive scene boundary、native scene state tests |
| `scene_composer.py` 导入后 Actor/Mechanics 后处理 | 旧 SceneComposer 直接生成流程的导入后位置、旋转、缩放和短时物理脉冲 | `native_actor_views_with_legacy_fallback`；native 路径使用 `scene.set_actor_transform` + `sceneTools.set_actor_physics` 聚合契约，旧宿主仅由集中 adapter 回退 | SceneComposer 不直接导入旧 Scene/manager；壁挂、边界钳制、整平和物理脉冲仍属于生成工作流内部编排 | AgentRuntime/native handler 覆盖位置校正、壁挂策略、物理脉冲和事件顺序，并完成旧流程回归后删除 fallback | SceneComposer aggregate boundary、SceneTools actor-physics tests |
| `nodes_tier_place.py` Tier2 物理沉降/挂墙回退 | 旧宿主无 `sceneTools.set_actor_physics` 或 native Actor transform 时的放置修正 | `sceneTools.set_actor_physics`、`scene.set_actor_transform`、`native_scene_state.NativeActorView`、`find_actor_with_legacy_fallback` 和 `set_actor_physics_value` | 挂墙和物理沉降均已统一经集中 adapter；旧 Mechanics 只在 adapter 内作为兼容实现，不能扩散到 Tier2 业务模块 | 所有受支持宿主提供 Actor physics/transform 聚合能力，且沉降、挂墙、AABB 回归通过后删除 | Tier2 placement/mechanics boundary tests |
| `script_runtime/engine/corona_engine.py` 的 scene/viewport/SceneTools adapter 调用 | Script Runtime 的 native 场景路由、快照、变换恢复、相机姿态和运行时 Actor 恢复 | `script_runtime/manifest_adapter.py` 的 ScriptRuntime manifest adapter | 场景列表/切换/快照/变换均经独立 ScriptRuntime channel；不在脚本引擎中直接调用公共 API；实体兼容仅保留显式 fallback | 旧生成脚本完成 native SceneScriptTarget 迁移后删除 legacy Scene fallback | Script Runtime API boundary tests |
| `plugins/FileManager/compat/legacy_file_scene_adapter.py` 的文件/场景兼容事件 | 外部旧文件面板宿主 | `files.*` / `project.*` manifest 事件 | native `files.*` 已负责文件操作；adapter 仅维护外部旧宿主的 Scene/Actor 路由和兼容事件；runtime 路径仅作 shim | 所有支持的外部宿主切换到公共事件，且 C++ handler 覆盖 Scene/Actor 绑定更新 | Python runtime boundary tests |
| `runtime/legacy_camera_follow.py:update_camera_follow` | 旧 CEF/宿主逐帧相机跟随入口 | `sceneTools.setActorCameraLock`、`scene.setActorTransform`、`viewport.setCameraPose` | 兼容旧宿主的逐帧输入与跟随行为；宿主入口已缩减为转发，不再持有实现 | 旧宿主完成入口迁移，并通过首次加载、锁定、WASD、右键拖动、偏移和视口姿态回归 | Python runtime boundary tests |
| `backend/blockly` 合同转发包 | 外部旧 Blockly 导入 | `script_runtime.blockly` | 保留旧模块路径兼容；AITool 生产代码已不再回退到该路径 | 外部宿主完成导入迁移并通过 Script Runtime 回退测试后移除 | Script Runtime layout boundary tests |
| `backend/registry.py` 服务注册转发包 | 外部旧嵌入式宿主 | `runtime.registry` 与 `runtime.plugin_loader` | 保留旧注册入口兼容；canonical loader 不再探测该路径 | 外部宿主完成导入迁移并通过 runtime loader 回归后移除 | Runtime loader layout boundary tests |
| `CoronaCore/core/scripts_system` 角色脚本转发包 | 历史生成脚本、旧宿主和外部脚本导入 | `script_runtime.engine` | 保留旧模块 alias 以维持脚本线程/上下文行为；新 Blockly 生成器已使用 canonical 路径 | 旧项目脚本和外部宿主完成导入迁移，并通过 Script Runtime 回退测试后移除 | Script Runtime layout and core runtime tests |
| `CoronaCore` `entities` / `components` 底层对象包装 | 角色脚本、Blockly/Scratch、旧 Scene 宿主 | `script_runtime.native_engine_adapter.get_script_runtime_adapter()`；编辑器业务使用 `scene.*` / `sceneTools.*` | 角色脚本仍需要 Engine Runtime 原子能力 | 角色脚本和 Blockly 完成受限 runtime 迁移，且实体导入审计为零 | script adapter and Scratch boundary tests |
| `runtime/legacy_engine_adapter.py:EditorEngineAdapter`（旧路径：`CoronaCore/core/engine_runtime.py`） | 外部 legacy 宿主或登记的测试替身（仓内无普通编辑器生产调用） | 对应 `scene.*` / `sceneTools.*` / `viewport.*` manifest 聚合方法 | 保留旧宿主注入和测试替身的兼容能力，避免把内部 native 对象扩散到业务层 | 外部宿主完成迁移、测试替身改用 manifest adapter，并通过兼容回归后删除 | runtime boundary tests |
| `plugins/SceneTools/main.py`（历史入口：`compat/legacy_scene_tools.py`）的相机视图生命周期和 Vision 旧场景回退 | SceneTools 聚合 handler + `plugins/SceneTools/compat/legacy_vision_import_adapter.py` + `legacy_vision_scene_adapter.py` | `sceneTools.open/close/update/deleteCameraView` 及 Vision 聚合方法；Vision 导入的 native 操作已切换到 manifest，旧 Web 方法仅转发到 SceneTools adapters | 相机视图持久化、Vision 文件绑定和代理清理仍需兼容旧 Python Scene 状态 | 为剩余能力补齐 native handler、schema、revision/事件和旧宿主回归后再移除 compat adapters | SceneTools native screenshot / legacy scene boundary tests |
| `AITool/cai_extensions/agent/agent_adapter.py` 及旧 Agent 调整逻辑 | 迁移期 Agent runtime gate | `scene.*` / `sceneTools.*` 值对象 adapter | 兼容旧 Agent 的输入和状态格式 | 所有 Agent 调整路径只消费 snapshot/value object，且旧格式回退测试删除 | Agent scene boundary tests |
| `include/corona/systems/script/corona_engine_api.h` | 旧 C++ include 路径 | `include/corona/engine/engine_runtime_api.h` | 保持外部/旧模块编译兼容 | 所有仓内 include 已迁移，发布说明允许移除旧路径 | C++ include/build regression |

每个入口必须同时具备替代接口、当前调用方、删除条件和回退测试。迁移期间只能收紧调用方，
不得在 legacy adapter 内复制一套新的 schema、校验、状态机或事件协议。

## 底层头文件归属

`include/corona/engine/engine_runtime_api.h` 是 C++ Engine Runtime API，适用于引擎内部和 Python 引擎适配层，不是 Vue/Python 编辑器公共 API。`camera()`、`geometry()` 等原子操作只能在底层或 Domain Service 内使用；上层必须使用 `scene.*`、`viewport.*` 等值对象和命令。

Engine Runtime 实现位于 `src/engine/engine_runtime_api.cpp`；Script Runtime 原子 binding
位于 `src/systems/script/python/engine_bindings.cpp`，迁移期旧编辑器 binding 位于
`src/systems/script/python/editor_compat_bindings.cpp`，编辑器/AITool 网络兼容 binding
位于 `src/systems/script/python/editor_network_bindings.cpp`。三者由 Script system target
链接，后两类 binding 只供登记的兼容 adapter 使用。

## C++ Script binding 审计

当前 `src/systems/script/python/engine_bindings.cpp`、
`src/systems/script/python/editor_compat_bindings.cpp` 和
`src/systems/script/python/editor_network_bindings.cpp` 中的导出按以下规则管理：

| binding 类别 | 代表内容 | 当前状态 | 允许用途 |
|---|---|---|---|
| Engine Runtime 对象 | `Geometry`、`Mechanics`、`Optics`、`Acoustics`、`Actor`、`Camera`、`Environment`、`Scene` | `active` / `engine_internal` | 角色脚本、Blockly/Scratch 和 Python Engine Adapter |
| 旧编辑器场景入口 | `editor_compat_bindings.cpp` 中的 `create_editor_actor`、`remove_editor_actor`、`get_editor_scene_snapshot`、`set_editor_actor_transform`、`get_editor_actor_bounds` | `deprecated` / `legacy` | 对应 adapter 的显式兼容回退 |
| 旧编辑器视口入口 | `editor_compat_bindings.cpp` 中的 `set_editor_camera_transform`、`capture_editor_camera_view` | `deprecated` / `legacy` | `viewport` adapter 的显式兼容回退 |
| 旧编辑器相机跟随快速通道 | `editor_compat_bindings.cpp` 中的 `camera_follow_set_target`、`camera_follow_clear`、`camera_follow_set_input_enabled`、`camera_follow_inject_rmb` | `deprecated` / `legacy` | 仅供 `runtime/legacy_camera_follow.py` 和旧宿主使用；不得由 Script Runtime 原子 binding 或新业务调用 |
| 旧编辑器宿主生命周期入口 | `editor_compat_bindings.cpp` 中的 `request_engine_exit` | `deprecated` / `legacy` | 仅供 `runtime/editor_host.py:close_process` 使用；新代码应通过宿主生命周期 service，不直接操作 SDL |
| 旧编辑器 Vision/媒体入口 | `editor_compat_bindings.cpp` 中的 `is_vision_available`、渲染后端、Vision 加载、媒体和音频函数 | `deprecated` / `legacy` | 仅由已登记的兼容 adapter 使用；编辑器 UI/Python 必须使用 `sceneTools.*` / `viewport.*` manifest |

底层类的存在不等于它们成为编辑器公共 API。普通 Vue、AITool 和编辑器 Python 业务不得从 `CoronaEngine` 取得这些对象；需要编辑器语义时必须经过 manifest 聚合方法。旧 binding 不得新增业务调用方，只有在 adapter 中才能保留。

当前已增加独立的 `EditorApiCaller::ScriptRuntime` 通道。`scene_datas.*` legacy 方法只允许 CEF 和 ScriptRuntime，编辑器 Python adapter 不能再通过普通 `PythonScript` 通道调用它们；Scratch 的 `CoronaEditorApi.scene_datas` 通过 `_invoke_cpp_script_api` 使用该通道。Script Runtime 不再直接调用 `sceneTools.*` 或 `_invoke_cpp_editor_api`；角色脚本物理兼容操作使用 `scene_datas.actor_operation`，鼠标拾取使用前端输入事件携带的结果。其他公共聚合方法仍保持现有 `PythonScript` 兼容范围，待逐项确认角色脚本所需能力后再继续收紧，不能直接修改现有 caller 位掩码。

协作网络属于编辑器/AITool 能力，不属于角色脚本运行时能力：`network.*` 和 LANChat 的公共 manifest 方法只允许 CEF 与 `PythonScript`，不得加入 `ScriptRuntime`。同样，编辑器 `sceneTools.playAudio`、`sceneTools.stopAudio` 及 Actor 音频接口只面向 CEF/PythonScript；角色脚本的媒体播放必须使用 `get_script_runtime_adapter()` 的受限能力。C++ 中保留的 `CoronaEngine.network_*` 绑定集中位于 `editor_network_bindings.cpp`，仅是迁移期兼容实现；生产 Python 代码必须经 `get_network_adapter()`、`get_lan_chat_adapter()` 或对应 transport/queue adapter 调用。新增网络或媒体能力必须先进入公共 manifest 或明确登记为 Script Runtime 原子能力，并同步检查 caller mask、旧 binding 调用方和回退测试。

## 新增接口验收条件

新增跨层能力必须同时具备：

1. manifest 方法、参数/返回 schema、`allowed_callers`；
2. C++ Domain Service 或 aggregate handler；
3. Vue 和/或 Python wrapper；
4. 明确的错误、revision 和事件语义；
5. 至少一个调用方边界测试；
6. 若保留旧入口，明确标记 `legacy`、替代接口和兼容期限。

## 公共 API 变更流程

所有跨层能力按以下顺序变更，避免三套实现逐渐独立：

```text
业务语义与 schema
        ↓
manifest
        ↓
C++ Domain Service / handler
        ↓
Vue、Python、Script adapter
        ↓
生产调用方与边界测试
```

如果变更只涉及某个 adapter 的命名或传输方式，应在变更说明中写明“无契约变化”，并验证
它仍映射到原 manifest 方法。若无法证明映射关系，就必须按新增公共 API 审核，不能以
wrapper 名称掩盖新的业务语义。

## 迁移安全检查

架构整理以能力为单位进行，不以目录重命名为起点。每次迁移必须确认：

- 请求、返回、错误、权限和事件顺序不变；
- 旧入口不会与新入口同时写入或重复广播；
- 测试替身和兼容宿主仍可运行；
- 生产调用方已切换到 manifest adapter；
- 相关测试、C++ 编译（若涉及 C++）和 `git diff --check` 均通过。

## 维护规则

每个公共语义的最小维护单元只有：

1. 一个 owner 和一个 Domain Service/handler（C++ 或受控 Python service）；
2. 一份 manifest/schema；
3. 每个实际调用边界一个薄 adapter；
4. 覆盖 adapter 映射、权限和兼容回退的边界测试。

维护成本按公共语义方法和事件计算，不按 Vue/Python 函数数量、目录数量或 wrapper 数量
计算。同一语义出现两个 owner、两套 schema、不同业务校验或不同事件结果时，应先合并到
公共 handler；只有存在不同业务目标时才新增接口。

每次 manifest、handler 或 adapter 变更后，应同步检查本表的 owner、调用方和状态。旧入口
必须记录替代接口、保留原因、删除条件和回退测试；未满足条件前不得批量删除或重命名。

维护审计至少检查以下事实，而不是只检查名称是否统一：

- manifest method 与各 adapter 的映射仍然一一对应；
- `allowed_callers` 没有因迁移而意外扩大；
- 场景写入、revision 和事件只由 owner 产生；
- legacy fallback 没有被新的生产调用方引用；
- Script Runtime 没有通过底层 binding 获得编辑器专用能力。

建议在每次接口迁移的提交说明中记录：契约是否变化、受影响的 adapter、legacy 调用方、
回退测试和未完成的删除条件。这样可以区分“重命名/传输调整”和“真正新增业务 API”，
避免随着 wrapper 数量增加而误判维护成本。
