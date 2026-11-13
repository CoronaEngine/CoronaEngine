# 接口契约（Web / ScriptRuntime）

这份文档是接口层的**单一入口**。它不重复代码，只给出三条边界、各自的形状，以及
**哪条测试在保护它**——契约信息散在守卫和注释里时，新人没有起点。

由 `tests/tools/generate_interface_contract.py` 生成；`test_interface_contract_doc.py`
会在它与代码不一致时失败。

## 三条边界

| 边界 | 谁在说 | 落到哪里 | 传输 |
|---|---|---|---|
| Web → C++ | 编辑器前端 | `window.corona.*` | 同步值 / Promise |
| ScriptRuntime → C++ | 角色脚本（Python） | 18 条 `script_scene_*` 具名绑定 | JSON 数组字符串 |
| Web → Python | 前端与 C++ 都可能发起 | `corona.bridge.call` → Python 服务 | JSON 信封 |

manifest（api 名称表 + caller mask + 注册表）已删除，三条边界里没有任何按名字分发
的桥。名字都是**具名函数**。

命名空间按职责划分，不再是「一个方法一个命名空间」：项目文档与项目元数据在 `project`，
gizmo 叠加层在 `viewport`，脚本输入派发在 `scene`，音频按身份分 `play`/`stop`。
**键名规则**：请求键用 camelCase（`panelId`、`routePath`），回复键用 snake_case
（`window_token`、`panel_id`、`actor_guid`、`archive_session`）；`createPanel` 的回复是
既有规则，不是笔误。

## Web 面

共 8 个命名空间 / 58 个方法。清单由
`tests/tools/test_typed_native_surface.py` 精确钉死（多一个、少一个都会失败）：

| 命名空间 | 说明 |
|---|---|
| `app` | 进程级操作（关闭） |
| `audio` | Audio 资源与 Actor 播放 |
| `bridge` | 唯一的 Web → Python 入口，一函数宽 |
| `network` | 协作会话与发现 |
| `project` | 项目新建/打开/最近列表 |
| `scene` | 场景读、写、Actor 与相机 |
| `viewport` | 视口配置与拾取 |
| `window` | Dock、面板与相机窗口 |

### 全部方法清单

| 命名空间 | 方法 |
|---|---|
| `app` | `closeProcess` |
| `audio` | `play`、`stop` |
| `bridge` | `call` |
| `network` | `clearDiscoveredPeers`、`connectToPeer`、`getDiscoveredPeers`、`getSessionInfo`、`searchLan`、`setProjectRoot`、`startSession`、`stopSession` |
| `project` | `createMultiplayerProject`、`createWorldProject`、`getProjectLoadStatus`、`getRecentProjects`、`openDocument`、`openProject`、`openProjectFile`、`saveActiveProjectInfo` |
| `scene` | `captureScreenshot`、`createActor`、`createCamera`、`deleteActor`、`deleteCamera`、`dispatchInput`、`focusActor`、`getEnvironment`、`importResourceFile`、`isVisionAvailable`、`readActive`、`readActorProperties`、`readActorRig`、`readCameraRenderSettings`、`readModelNodes`、`readScene`、`readTransforms`、`rebindActorResource`、`save`、`selectActor`、`selectModelFile`、`setActorIkChains`、`setActorTransform`、`setCameraRenderSettings`、`setEnvironment`、`setModelNodeState`、`updateActor`、`updateCamera` |
| `viewport` | `configure`、`pick`、`setGizmoPointer`、`setGizmoTarget` |
| `window` | `close`、`createPanel`、`focus`、`onCloseRequested`、`onClosed`、`setWindowPresentation` |

### 面板与窗口：三个合并后的入口

同一件事（打开一个面板、关闭一个窗口、改变窗口呈现）以前按“哪个面板 / 哪个开关”拆成
多个方法。现在各留一个，差异用参数表达。**面板停靠由 ImGui 负责**：面板打开后就是一个
ImGui 窗口，用户拖动标签页停靠，布局由 imgui.ini 记住，Web 层不再发送停靠位置。

| 方法 | 请求 | 说明 |
|---|---|---|
| `createPanel` | `{kind?: 'panel' \| 'camera', width?, height?, x?, y?, panelId?, routePath?, title?}` | `kind` 缺省为 `panel`。相机窗口要求 `archiveSession`/`sceneId`/`cameraId` 同时非空；`cameraHandle` 一律拒绝，相机身份由原生解析。`x`/`y` 只给相机窗口——停靠面板的位置由 ImGui 决定，面板带 `x`/`y` 会被拒绝而不是静默忽略；`archiveSession`/`sceneId`/`cameraId` 同样只属于相机窗口。 |
| `close` | 无参数 / `{panelId}` / `{sceneId, cameraId}` / `{sceneId}` | 无参数关自己；`panelId` 是 `createPanel` 的稳定面板身份（只有主窗口能关别人，主窗口不能被当作面板关）；`sceneId`+`cameraId` 关一个相机窗口（主窗口可关任意，相机窗口只能关自己）；只给 `sceneId` 是复数形式，关掉该场景全部相机窗口。返回 `{closed: n}`，`0` 表示没有匹配窗口（幂等清理）。空 `sceneId` 不关任何窗口，但仍在下一次主线程调度里应答——窗口会话用它作退休栅栏。 |
| `focus` | `{panelId}` | 聚焦该面板（只有主窗口能聚焦别人）。 |
| `setWindowPresentation` | `{target?: 'camera' \| 'main', mode, sceneId?, cameraId?, width?, height?}` | 请求要么给 `mode`（`windowed`/`maximized`/`borderless`/`toggle`），要么给 `width`+`height`（相机窗口改尺寸），不能同时给；主窗口只有 `windowed`/`borderless`/`toggle` 且没有尺寸请求。相机窗口可用 `sceneId`+`cameraId` 指定，缺省为自己。 |

`mode` 描述**窗口要处于的状态**，不是要执行的开关；`toggle` 是唯一不带状态值的请求，
表示“往前走一步”（相机窗口：无边框 → 窗口化，窗口化 ⇄ 最大化）。
`createPanel` 返回 `{window_token, panel_id, existing}`；`close` 返回
`{closed: n}`（`0` 表示没有匹配窗口）；`focus` 返回布尔。

`panelId` 是**稳定面板身份**，开/关/聚焦都用它。`windowToken` 是**这一次创建**的
不透明令牌：同一个 `panelId` 重开后令牌不同，用来把 `onClosed` 的负载和当初那次创建的
返回值对齐（原始 tab id 不再暴露）。窗口不查询自身状态：身份来自 `createPanel` 的返回值
和打开时带在 URL 上的参数。

**权限**：窗口操作按调用者身份授权，规则集中在 `cef_window_operations.cpp` 的每个 case，
这里列全，接入方不必读源码：

| 调用者 \ 操作 | 关自己 | `close({panelId})` | `close({sceneId, cameraId})` | `close({sceneId})` | `focus({panelId})` | `setWindowPresentation({target:'main'})` |
|---|---|---|---|---|---|---|
| 主工作区 | ✗（主窗口不是面板） | ✓ 除主窗口外的任意面板 | ✓ 任意相机 | ✓ 该场景的全部相机窗口 | ✓ 任意面板 | ✓ |
| 面板 | ✓ | ✗ | ✗ | ✗ | ✓ 只有自己 | ✗ |
| 相机窗口 | ✓ | ✗ | ✓ 只有自己 | ✓ 仅当该场景只剩自己 | ✓ 只有自己 | ✗ |

`createPanel` 不限调用者；`createCameraView` 也不限主窗口——它由相机租约（场景身份 + 
`camera_authority` 是否仍 active）授权，因为 SceneTools 自己就在一个原生 Dock 上下文里。
越权一律 `PERMISSION_DENIED`，不会静默忽略。

**失败形状**：Promise 拒绝时携带下表的码，原样透传给 Web。`rejection_codes.py`
声明全集，`test_rejection_vocabulary.py` 保证源码只发这些码。

| 分组 | 谁给出 | 码 |
|---|---|---|
| 基础设施 | V8 绑定本体，原生工作开始前 | `CONTEXT_CHANGED`、`INVALID_ARGUMENT`、`NATIVE_FAILURE`、`NATIVE_UNAVAILABLE`、`PERMISSION_DENIED` |
| 窗口 | 原生窗口宿主（窗口消失、建不出来） | `CREATE_FAILED`、`WINDOW_CLOSED` |
| 场景/归档 | 场景与归档层（对象不存在、只读、冲突…） | `ACTOR_AUDIO_UNAVAILABLE`、`ACTOR_CREATE_FAILED`、`ACTOR_NOT_FOUND`、`ACTOR_NOT_READY`、`ACTOR_VERSION_CONFLICT`、`CAMERA_NOT_DELETABLE`、`CAMERA_NOT_FOUND`、`CAMERA_UNAVAILABLE`、`CAPTURE_FAILED`、`COMPONENT_NOT_FOUND`、`CONFLICT`、`DOCUMENT_CONFLICT`、`FILE_NOT_FOUND`、`FILE_OPERATION_FAILED`、`INVALID_BOUNDS`、`INVALID_PROJECT_DOCUMENT`、`INVALID_PROJECT_PATH`、`LAST_CAMERA`、`MODEL_DECODE_FAILED`、`NODE_EDIT_REJECTED`、`NODE_NOT_FOUND`、`NO_ACTIVE_PROJECT`、`PICK_FAILED`、`PICK_TIMEOUT`、`PROJECT_CONTEXT_CHANGED`、`PROJECT_CREATION_FAILED`、`PROJECT_OPEN_FAILED`、`READ_ONLY`、`READ_ONLY_PROJECT`、`RESOURCE_NOT_FOUND`、`RUN_ACTIVE`、`SAVE_FAILED`、`UNSUPPORTED_RESOURCE_TYPE` |
| Web → Python | bridge 传输与 Python 服务 | `EDITOR_API_TIMEOUT`、`SERVICE_BUSY`、`SERVICE_FAILED`、`SERVICE_UNAVAILABLE` |

**可分支的稳定码**：`ACTOR_NOT_FOUND`、`ACTOR_VERSION_CONFLICT`、`CAMERA_NOT_FOUND`、`CONFLICT`、`CONTEXT_CHANGED`、`CREATE_FAILED`、`EDITOR_API_TIMEOUT`、`FILE_NOT_FOUND`、`INVALID_ARGUMENT`、`NATIVE_UNAVAILABLE`、`PERMISSION_DENIED`、`PICK_TIMEOUT`、`PROJECT_CONTEXT_CHANGED`、`READ_ONLY`、`READ_ONLY_PROJECT`、`RESOURCE_NOT_FOUND`、`SERVICE_BUSY`、`SERVICE_FAILED`、`SERVICE_UNAVAILABLE`、`WINDOW_CLOSED`。
Web 可以按这些码决定重试、禁用控件或提示；其余是**细节码**，照常拒绝，但只应进日志——
细节码会随实现改名或合并。稳定码的每一次增删都是契约变更，需要写进
`docs/interface-decisions.md`。

Python 服务的**业务拒绝**统一是 Promise 拒绝，带 `error.code`：`ProjectArchive` 与
`ProjectFiles` 在 Python 侧抛 `BridgeError(code, message)`，dispatcher 把它写成
`CODE: message`，C++ 桥只从消息前缀取码，因此 `PROJECT_CONTEXT_CHANGED`、
`DOCUMENT_CONFLICT`、`READ_ONLY_PROJECT`、`CONFLICT` 等业务码原样到达前端。
`FileScripts` 是例外：它的回复是**运行状态记录**（`status` 是
`starting/running/completed/error` 状态机），按值返回，由 Blockly 面板按状态渲染。
只有传输或服务本身失败（信封解析、服务未启动、Python 抛未知异常）才回落到
`SERVICE_FAILED`。
同步方法**永不拒绝**：返回值，找不到时返回文档化的空值。

## ScriptRuntime 面

18 条具名绑定。调用方传一个 JSON 数组的字符串；`arity` 是运行时强制的下限
（传少了直接 400，不会进到场景逻辑里）。

| 绑定 | 参数 | arity | 说明 |
|---|---|---|---|
| `get_environment` | `[scene]` | 1 | The scene environment: sun, grid, physics scalars. |
| `read_active_scene` | `[]` | 0 | Archive session, scene route, name, project path, tree revision, selection. |
| `read_actor` | `[{archive_session, actor}]` | 1 | One actor by guid or name, projected the way the Web sees it. |
| `read_transforms` | `[{archive_session, actors[]}]` | 1 | Transforms for several guids; unknown guids are skipped, not an error. |
| `read_archive_scene` | `[{archive_session}]` | 1 | The persisted scene values: actors, cameras, environment, terrain. |
| `get_scene_snapshot` | `[scene]` | 1 | The full scene snapshot built from native state. |
| `set_actor_transform` | `[scene, actor, {position?, rotation?, scale?, archive_session?}]` | 3 | Writes transform components; omitted components keep their value. |
| `set_environment` | `[scene, {archive_session?, sun?, grid?, physics?}]` | 2 | Patches the environment. |
| `set_actor_state` | `[scene, actor, {archive_session?, name?, visible?, ...}]` | 3 | Patches the actor's own state (not its body). |
| `set_actor_animation` | `[scene, actor, animation, blend_seconds?]` | 4 | Requests an animation clip; fails with ANIMATION_NOT_READY when no clip is loaded. |
| `pause_actor_animation` | `[scene, actor]` | 2 | Pauses the actor's animation player. |
| `resume_actor_animation` | `[scene, actor]` | 2 | Resumes the actor's animation player. |
| `set_actor_physics` | `[scene, actor, {archive_session?, mass?, body_type?, ...}]` | 3 | Patches the actor's body properties. |
| `set_camera_pose` | `[scene, camera, {position?, forward?, up?, fov?, ...}]` | 3 | Writes a camera pose. The script channel may write pose; it may not retarget cameras. |
| `create_actor` | `[scene, source_path, actor_type, actor_snapshot]` | 4 | Creates an actor from an asset path. ctor_snapshot is the same actor object the  archive stores (name, actor_guid, transform, visible, optics, mechanics, ik_chains,  model_node_overrides, animation_sources); rchive_session inside it is validated.  The script channel owns no assets, so it never imports one. |
| `play_actor_audio` | `[scene, actor, {archive_session?, loop?}]` | 3 | Plays the audio attached to an actor; the engine owns the sound device. |
| `stop_actor_audio` | `[scene, actor, {archive_session?}]` | 3 | Stops the audio attached to an actor. |
| `remove_actor` | `[scene, actor]` | 2 | Removes an actor and reports what was removed. |

**失败形状**：`{success: false, error, error_code, data}`。

- `error_code` 是数字类：**409** 归档过期、**403** 只读、**404** Actor 不存在、
  **400** 参数不合法、**2** 其它。
- `data.code` 保留场景层的符号码（`PROJECT_CONTEXT_CHANGED`、`ACTOR_NOT_READY`…）。
  按数字分支，日志里引用符号。Python 侧抛出的异常形如
  `RuntimeError("PROJECT_CONTEXT_CHANGED: ...")`。

## 文件脚本路由

测试探针用 `Scripts.*` 名字和 C++ 通信，但生产代码里没有名字分发：
[`file_script_api.h`](../include/corona/systems/ui/file_script_api.h) 提供五条具名入口，
名字到函数的映射留在探针内部。

## 保护这份契约的守卫

| 守卫 | 保护什么 |
|---|---|
| [`test_manifest_retirement.py`](../tests/tools/test_manifest_retirement.py) | 已删除的 manifest 不许回来 |
| [`test_typed_native_surface.py`](../tests/tools/test_typed_native_surface.py) | Web 面方法 + 拒绝码词表 |
| [`test_script_runtime_contract.py`](../tests/tools/test_script_runtime_contract.py) | 脚本绑定：名字 / 参数位 / 数量 / 实现不越界 |
| [`test_interface_contract_doc.py`](../tests/tools/test_interface_contract_doc.py) | 这份文档与代码一致 |
| [`test_docs_reference_live_files.py`](../tests/tools/test_docs_reference_live_files.py) | 文档不指向已删除的文件 |
| [`test_editor_settings_owner.py`](../tests/tools/test_editor_settings_owner.py) | 编辑器 ini 只有一个 Python 所有者 |
| [`test_reply_shapes.py`](../tests/tools/test_reply_shapes.py) | Python 产出的回复透传，引擎产出保留类型 |
| [`test_integration_surface.py`](../tests/tools/test_integration_surface.py) | 集成冒烟只调用、只断言仍然存在的原生命中 |
| [`test_rejection_vocabulary.py`](../tests/tools/test_rejection_vocabulary.py) | 拒绝码词表：源码只发已声明的码；稳定码可分支、细节码只进日志 |
| [`test_window_permission_matrix.py`](../tests/tools/test_window_permission_matrix.py) | 每个窗口方法都带调用者检查，例外必须逐条说明 |
| [`test_geometry_external_access.py`](../tests/tools/test_geometry_external_access.py) | 外部只读几何场景快照，写一律走命令通道 |
| [`test_python_runtime_deploy.py`](../tests/tools/test_python_runtime_deploy.py) | Python 运行时部署是增量的：未变化的树只做 stat |
| [`test_source_pins.py`](../tests/tools/test_source_pins.py) | pin 改写幂等；只有已推送到远程的提交才算可 pin |
| [`test_frontend_facade.py`](../tests/tools/test_frontend_facade.py) | 原生方法↔facade 一一对应；前端只经 facade 触达原生对象 |
| [`test_frontend_regressions.py`](../tests/tools/test_frontend_regressions.py) | 逐个跑前端窗口/音频/facade 回归文件（带超时） |
| [`test_web_layer_budget.py`](../tests/tools/test_web_layer_budget.py) | 接口核心行数 ratchet（度量在 web_layer_budget.py） |
| [`test_scene_api_consolidation.py`](../tests/tools/test_scene_api_consolidation.py) | 场景 V8 只保留合并后的方法名与总数 |
| [`test_web_python_bridge.py`](../tests/tools/test_web_python_bridge.py) | Web→Python 信封格式与错误码映射 |
| [`test_python_refusal_codes.py`](../tests/tools/test_python_refusal_codes.py) | Python 侧拒绝码都在契约词表内 |
| [`test_lanchat_removal.py`](../tests/tools/test_lanchat_removal.py) | 局域网聊天接口已删除，不得回归 |
| [`test_project_allocator.py`](../tests/tools/test_project_allocator.py) | 项目目录分配器：重名与原子创建 |
| [`test_vision_thermal_run.py`](../tests/tools/test_vision_thermal_run.py) | Vision 热运行路径 |

## 已知缺口

`embedded_python/script_runtime/BOUNDARY.md` 记录了脚本侧**有意保留**的能力缺口：
文件音频播放（Actor 自带音频已支持）、原生射线查询、指针捕获。补的时候要先在
`script_runtime_api.h` 加具名入口，而不是恢复"探测一个不存在的函数再静默降级"的写法。

接口侧的覆盖缺口（记录在此，避免被当成已完成）：

- 窗口/音频/面板请求的**参数校验**只有手动 CEF 冒烟覆盖（`web_messages_cef_smoke.mjs`）。
  读者（`read_close_request`、`read_window_presentation`、`read_audio_control`、`read_panel_options`）
  依赖 `CefV8Value`，单测需要真实 V8 上下文；`tests/tools` 的守卫只钉方法名、拒绝码和集成调用。
- 窗口不再有 `getCurrent()`：它唯一的产品消费者是测试包装，身份改由 `createPanel` 的返回值和
  URL 参数提供。需要「问问自己是谁」的新页面应走同样的两条路，而不是恢复这个查询。
- `SceneReply` 已按读/写分族（`SceneReadReply` / `SceneWriteReply`），每个方法只填、
  只读自己族里的字段，不再是一个 30 字段、所有方法共用的结构。
