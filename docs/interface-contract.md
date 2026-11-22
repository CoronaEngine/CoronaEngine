# 接口契约（Web / Python）

这份文档是接口层的**单一入口**：三条边界、两侧的形状与失败码表。名字的权威来源是
`src/systems/ui/cef/editor_operation_table.h`（Web 面）与绑定源码（Python 面）；本文是手工维护的快照。

## 三条边界

| 边界 | 谁在说 | 落到哪里 | 传输 |
|---|---|---|---|
| Web → C++ | 编辑器前端 | `window.corona.*` | 同步值 / Promise |
| Web → Python | 前端与 C++ 都可能发起 | `corona.bridge.call` → Python 服务 | JSON 信封 |

没有任何按名字分发的桥；名字都是**具名函数**。

命名空间按职责划分：项目文档与项目元数据在 `project`，视口配置在 `viewport`，脚本输入派发在
`input`，音频按身份分 `play`/`stop`。
**键名规则**：过原生的键一律 snake_case（`panel_id`、`route_path`、`archive_session`），
请求和回复用同一套写法。窗口命令对象**本身就是线上对象**：`appService` 把命令参数
原样交给绑定，没有 camelCase↔snake_case 的转换层。**方法名也是 snake_case**：表里的
`name` 原样发布成 `window.corona.<group>.<name>`，前端 facade 用同一个拼写，没有推导。

## Web 面

共 9 个命名空间 / 54 个方法。名字的唯一声明处是
`src/systems/ui/cef/editor_operation_table.h`；下面是它的手工快照：

| 命名空间 | 说明 |
|---|---|
| `app` | 进程级操作（关闭） |
| `audio` | Audio 资源与 Actor 播放 |
| `bridge` | 唯一的 Web → Python 入口，一函数宽 |
| `input` | 脚本输入派发（目标是一个场景相机） |
| `network` | 协作会话与发现 |
| `project` | 项目新建/打开/最近列表 |
| `scene` | 场景读、写、Actor 与相机 |
| `viewport` | 视口配置 |
| `window` | Dock、面板与相机窗口 |

### 全部方法清单

| 命名空间 | 方法 |
|---|---|
| `app` | `close_process` |
| `audio` | `play`、`stop` |
| `bridge` | `call` |
| `input` | `dispatch` |
| `network` | `clear_discovered_peers`、`connect_to_peer`、`get_discovered_peers`、`get_session_info`、`search_lan`、`set_project_root`、`start_session`、`stop_session` |
| `project` | `create_multiplayer_project`、`create_world_project`、`get_project_load_status`、`get_recent_projects`、`open_document`、`open_project`、`open_project_file`、`save_active_project_info` |
| `scene` | `create_actor`、`create_camera`、`delete_actor`、`delete_camera`、`focus_actor`、`import_resource_file`、`read_active`、`read_actor_properties`、`read_actor_rig`、`read_camera_render_settings`、`read_environment`、`read_model_nodes`、`read_scene`、`read_transforms`、`rebind_actor_resource`、`save`、`select_actor`、`select_model_file`、`set_actor_ik_chains`、`set_actor_transform`、`set_camera_render_settings`、`set_environment`、`set_model_node_state`、`update_actor`、`update_camera` |
| `viewport` | `capture_screenshot`、`configure` |
| `window` | `close`、`create_panel`、`focus`、`on_close_requested`、`on_closed`、`set_window_presentation` |

### 每个操作的用途与归类

归类只有三种：**capability**（读或写引擎/编辑器状态）、**event**（UI 才需要的订阅）、
**cross-boundary**（边界本身）。

| 命名空间 | 操作 | 归类 | 用途 |
|---|---|---|---|
| `scene` | `read_scene` | capability | The scene tree, cameras and actor projection the Web reads. |
| `scene` | `read_active` | capability | Archive session, scene route, name, project path, tree revision, selection. |
| `scene` | `select_model_file` | capability | Native file dialog that answers the chosen model path. |
| `scene` | `import_resource_file` | capability | Imports a resource file of the named type into the project. |
| `scene` | `read_model_nodes` | capability | One page of a model's node hierarchy. |
| `scene` | `set_model_node_state` | capability | Writes model node visibility and per-node overrides. |
| `scene` | `create_actor` | capability | Creates an actor from an asset path. |
| `scene` | `read_actor_properties` | capability | The actor's own state: name, visibility, mechanics, animation sources. |
| `scene` | `read_actor_rig` | capability | Skeleton nodes and IK chains for one actor. |
| `scene` | `set_actor_ik_chains` | capability | Writes IK chains against the expected revision. |
| `scene` | `read_transforms` | capability | Transforms for several guids; unknown guids are skipped, not an error. |
| `scene` | `select_actor` | capability | Writes the editor selection the scene tree renders. |
| `scene` | `set_actor_transform` | capability | Writes transform components; omitted components keep their value. |
| `scene` | `update_actor` | capability | Patches the actor's own state (not its body). |
| `scene` | `delete_actor` | capability | Removes an actor and reports what was removed. |
| `scene` | `rebind_actor_resource` | capability | Points an actor at a different resource path. |
| `scene` | `save` | capability | Writes the scene through the archive service and reports unresolved actors. |
| `scene` | `read_environment` | capability | The scene environment: sun, grid, physics scalars. |
| `scene` | `set_environment` | capability | Patches the environment. |
| `scene` | `focus_actor` | capability | Moves a camera so it frames one actor. |
| `scene` | `update_camera` | capability | Patches camera properties and pose. |
| `scene` | `set_camera_render_settings` | capability | Writes Vision render settings for one camera. |
| `scene` | `read_camera_render_settings` | capability | Reads Vision render settings for one camera. |
| `scene` | `create_camera` | capability | Creates a camera in a scene. |
| `scene` | `delete_camera` | capability | Deletes a camera; the last remaining camera is protected. |
| `audio` | `play` | capability | Plays the audio attached to an actor; the Web also plays a bare resource. |
| `audio` | `stop` | capability | Stops the audio attached to an actor; the Web also stops a bare resource. |
| `app` | `close_process` | capability | Closes the whole process; only the main window may ask. |
| `viewport` | `capture_screenshot` | capability | Captures one viewport frame to a file. |
| `viewport` | `configure` | capability | Configures a viewport's mode, bounds and calibration. |
| `network` | `start_session` | capability | Starts a collaboration session as host or client. |
| `network` | `stop_session` | capability | Ends the running collaboration session. |
| `network` | `get_session_info` | capability | The running session's identity, role and port. |
| `network` | `get_discovered_peers` | capability | The peers LAN discovery has reported so far. |
| `network` | `clear_discovered_peers` | capability | Forgets the peers discovered so far. |
| `network` | `search_lan` | capability | Broadcasts a LAN discovery probe. |
| `network` | `connect_to_peer` | capability | Joins a peer's session by address, port and name. |
| `network` | `set_project_root` | capability | Scopes discovery to one project root. |
| `project` | `open_project` | capability | Opens a recent project in the editor. |
| `project` | `open_project_file` | capability | Asks the native launcher for a project file to open. |
| `project` | `create_world_project` | capability | Creates a world project; it takes no fields - the archive service names it. |
| `project` | `create_multiplayer_project` | capability | Creates a multiplayer project. |
| `project` | `get_recent_projects` | capability | The recent-project list the launcher renders. |
| `project` | `get_project_load_status` | capability | Whether the project the launcher asked for has finished loading. |
| `project` | `open_document` | capability | Reloads the current scene source (scene.ini) from disk; the only file type it accepts. |
| `project` | `save_active_project_info` | capability | Writes the active project's metadata. |
| `window` | `close` | capability | Closes a window or panel; the identity decides which. |
| `window` | `focus` | capability | Focuses the window the panel identity belongs to. |
| `window` | `create_panel` | capability | Creates a panel and answers its identity. |
| `window` | `set_window_presentation` | capability | Moves a window to the named presentation state. |
| `window` | `on_closed` | event | Subscribes to the event a window publishes after it closes. |
| `window` | `on_close_requested` | event | Subscribes to a window's close request before it closes. |
| `input` | `dispatch` | cross-boundary | Dispatches one input event to a camera's viewport. |
| `bridge` | `call` | cross-boundary | The one Web -> Python request entry; Python owns every service behind it. |

### 面板与窗口：三个合并后的入口

打开面板、关闭窗口、改变呈现各有一个入口，差异用参数表达。**面板停靠由 ImGui 负责**：
面板打开后就是一个 ImGui 窗口，用户拖动标签页停靠，布局由 imgui.ini 记住，Web 层不发送停靠位置。

| 方法 | 请求 | 说明 |
|---|---|---|
| `createPanel` | `{kind?: 'panel' \| 'camera', width?, height?, x?, y?, panel_id?, route_path?, title?}` | `kind` 缺省为 `panel`。相机窗口要求 `archive_session`/`scene_id`/`camera_id` 同时非空；`camera_handle` 一律拒绝，相机身份由原生解析。`x`/`y` 只给相机窗口——停靠面板的位置由 ImGui 决定，面板带 `x`/`y` 会被拒绝而不是静默忽略；`archive_session`/`scene_id`/`camera_id` 同样只属于相机窗口。 |
| `close` | 无参数 / `{panel_id}` / `{scene_id, camera_id}` / `{scene_id}` | 无参数关自己；`panel_id` 是 `createPanel` 的稳定面板身份（只有主窗口能关别人，主窗口不能被当作面板关）；`scene_id`+`camera_id` 关一个相机窗口（主窗口可关任意，相机窗口只能关自己）；只给 `scene_id` 是复数形式，关掉该场景全部相机窗口。返回 `{closed: n}`，`0` 表示没有匹配窗口（幂等清理）。空 `scene_id` 不关任何窗口，但仍在下一次主线程调度里应答——窗口会话用它作退休栅栏。 |
| `focus` | `{panel_id}` | 聚焦该面板（只有主窗口能聚焦别人）。 |
| `setWindowPresentation` | `{target?: 'camera' \| 'main', mode, scene_id?, camera_id?, width?, height?}` | 请求要么给 `mode`（`windowed`/`maximized`/`borderless`/`toggle`），要么给 `width`+`height`（相机窗口改尺寸），不能同时给；主窗口只有 `windowed`/`borderless`/`toggle` 且没有尺寸请求。相机窗口可用 `sceneId`+`cameraId` 指定，缺省为自己。 |

`mode` 描述**窗口要处于的状态**，不是要执行的开关；`toggle` 是唯一不带状态值的请求，
表示“往前走一步”（相机窗口：无边框 → 窗口化，窗口化 ⇄ 最大化）。
`createPanel` 返回 `{window_token, panel_id, existing}`；`close` 返回
`{closed: n}`（`0` 表示没有匹配窗口）；`focus` 返回布尔。

`panel_id` 是**稳定面板身份**，开/关/聚焦都用它。`window_token` 是**这一次创建**的
不透明令牌：同一个 `panel_id` 重开后令牌不同，用来把 `onClosed` 的负载和当初那次创建的
返回值对齐（不暴露原始 tab id）。窗口不查询自身状态：身份来自 `createPanel` 的返回值
和打开时带在 URL 上的参数。

**权限**：窗口操作按调用者身份授权，规则集中在 `cef_window_operations.cpp` 的每个 case：

| 调用者 \ 操作 | 关自己 | `close({panel_id})` | `close({scene_id, camera_id})` | `close({scene_id})` | `focus({panel_id})` | `setWindowPresentation({target:'main'})` |
|---|---|---|---|---|---|---|
| 主工作区 | ✗（主窗口不是面板） | ✓ 除主窗口外的任意面板 | ✓ 任意相机 | ✓ 该场景的全部相机窗口 | ✓ 任意面板 | ✓ |
| 面板 | ✓ | ✗ | ✗ | ✗ | ✓ 只有自己 | ✗ |
| 相机窗口 | ✓ | ✗ | ✓ 只有自己 | ✓ 仅当该场景只剩自己 | ✓ 只有自己 | ✗ |

`createPanel` 不限调用者；`createCameraView` 也不限主窗口——它由相机租约（场景身份 + 
`camera_authority` 是否仍 active）授权，因为 SceneTools 自己就在一个原生 Dock 上下文里。
越权一律 `PERMISSION_DENIED`，不会静默忽略。

**失败形状**：Promise 拒绝时携带下表的码，原样透传给 Web；`rejection_codes.py` 声明全集。

| 分组 | 谁给出 | 码 |
|---|---|---|
| 基础设施 | V8 绑定本体，原生工作开始前 | `CONTEXT_CHANGED`、`INVALID_ARGUMENT`、`NATIVE_FAILURE`、`NATIVE_UNAVAILABLE`、`PERMISSION_DENIED` |
| 窗口 | 原生窗口宿主（窗口消失、建不出来） | `CREATE_FAILED`、`WINDOW_CLOSED` |
| 场景/归档 | 场景与归档层（对象不存在、只读、冲突…） | `ACTOR_AUDIO_UNAVAILABLE`、`ACTOR_CREATE_FAILED`、`ACTOR_NOT_FOUND`、`ACTOR_NOT_READY`、`ACTOR_VERSION_CONFLICT`、`CAMERA_NOT_DELETABLE`、`CAMERA_NOT_FOUND`、`CAPTURE_FAILED`、`COMPONENT_NOT_FOUND`、`CONFLICT`、`DOCUMENT_CONFLICT`、`DUPLICATE_ACTOR_NAME`、`DUPLICATE_CAMERA_NAME`、`FILE_NOT_FOUND`、`FILE_OPERATION_FAILED`、`INVALID_BOUNDS`、`INVALID_PROJECT_DOCUMENT`、`INVALID_PROJECT_PATH`、`LAST_CAMERA`、`MODEL_DECODE_FAILED`、`NODE_EDIT_REJECTED`、`NODE_NOT_FOUND`、`NO_ACTIVE_PROJECT`、`PROJECT_CONTEXT_CHANGED`、`PROJECT_CREATION_FAILED`、`PROJECT_OPEN_FAILED`、`READ_ONLY`、`READ_ONLY_PROJECT`、`RESOURCE_NOT_FOUND`、`SAVE_FAILED`、`UNSUPPORTED_RESOURCE_TYPE` |
| Web → Python | bridge 传输与 Python 服务 | `EDITOR_API_TIMEOUT`、`SERVICE_BUSY`、`SERVICE_FAILED`、`SERVICE_UNAVAILABLE` |

**可分支的稳定码**：`ACTOR_NOT_FOUND`、`ACTOR_VERSION_CONFLICT`、`CAMERA_NOT_FOUND`、`CONFLICT`、`CONTEXT_CHANGED`、`CREATE_FAILED`、`DOCUMENT_CONFLICT`、`EDITOR_API_TIMEOUT`、`FILE_NOT_FOUND`、`INVALID_ARGUMENT`、`NATIVE_UNAVAILABLE`、`PERMISSION_DENIED`、`PROJECT_CONTEXT_CHANGED`、`READ_ONLY`、`READ_ONLY_PROJECT`、`RESOURCE_NOT_FOUND`、`SERVICE_BUSY`、`SERVICE_FAILED`、`SERVICE_UNAVAILABLE`、`WINDOW_CLOSED`。
Web 可以按这些码决定重试、禁用控件或提示；其余是**细节码**，照常拒绝，但只应进日志——
细节码会随实现改名或合并。稳定码的每一次增删都是契约变更，需要写进
`docs/interface-decisions.md`。

Python 服务的**业务拒绝**统一是 Promise 拒绝，带 `error.code`：`project_archive` 与
`project_files` 在 Python 侧抛 `BridgeError(code, message)`，dispatcher 把它写成
符号码落在 `data.code`（与 C++ 场景层同一字段），C++ 桥不解析消息，因此 `PROJECT_CONTEXT_CHANGED`、
`DOCUMENT_CONFLICT`、`READ_ONLY_PROJECT`、`CONFLICT` 等业务码原样到达前端。
同步方法**永不拒绝**：返回值，找不到时返回文档化的空值。


## Python 服务清单（bridge 可达）

`corona.bridge.call` 只接受这些服务。白名单在 `runtime/editor_host.py` 的 `ALLOWED_SERVICES`。
信封是 `{service, function, args}`，业务拒绝是带 `error.code` 的 Promise 拒绝。

| 服务 | 模块 | 函数 |
|---|---|---|

`project_archive.file_manager` 是**带 op 判别**的服务：`op` 只能是 `create_file`、`create_folder`、`delete_item`、`get_file_tree`、`get_files`、`rename_item`，其余按 `INVALID_ARGUMENT` 拒绝；
每个 op 的请求键由 `runtime/archive/arguments.py` 的 `require_keys` 白名单校验，
与 C++ reader 的 `object_keys` 同语义（未知键拒绝，不静默忽略）。
| `project_archive` | `plugins.ProjectArchive.main` | `asset_operation`、`commit_document`、`create_project_in`、`editor_settings`、`file_manager`、`parse`、`project_info`、`read_document`、`recent_projects`、`save`、`update_metadata` |
| `project_files` | `runtime.archive.script_files` | `load`、`save` |


## Python 宿主面（`CoronaEngine.*`）

模块由两个组按序安装：`BindHostSupport` → `BindCef`（`BindCef` 内部再调
`BindCefServiceQueue`）。它是被引擎启动的宿主，下面绝大多数条目是宿主管道，不是能力。

**C++ → Python 只有两处**：`python_api.cpp` 按名字取 `main.run` 与 `main.loop`，启动时
各调用一次，之后 C++ 不再调用 Python 函数。

| 组 | 接口 | 归类 | 用途 | 定义 |
|---|---|---|---|---|
| `BindHostSupport` | `send_log` | 诊断 | 把 Python 日志写进引擎日志；一类「可选工具配置缺失」的 ERROR 降级为 WARNING，避免误报 | [`host_support_bindings.cpp`](../src/systems/script/python/host_support_bindings.cpp#L26) |
| `BindHostSupport` | `drain_input_events` | 宿主控制 | 排空 SDL 与 Web 共用的输入队列。**返回值当前被丢弃**——它是唯一排空者，作用是防增长 | [`host_support_bindings.cpp`](../src/systems/script/python/host_support_bindings.cpp#L84) |
| `BindHostSupport` | `python_runtime_phase` | 诊断 | 更新原生侧运行时诊断相位，供关停诊断与超时报告读取 | [`host_support_bindings.cpp`](../src/systems/script/python/host_support_bindings.cpp#L88) |
| `BindHostSupport` | `should_stop` | 宿主控制 | 宿主是否已请求停止；无协调器时返回 `false`（没有循环可停） | [`host_support_bindings.cpp`](../src/systems/script/python/host_support_bindings.cpp#L96) |
| `BindCef` | `register_python_script_service_dispatcher` | 传输接线 | 把 Python 的服务分发器接到 CEF，使 Web 请求有落点 | [`cef_py_bind.cpp`](../src/systems/ui/cef/cef_py_bind.cpp#L22) |
| `BindCef` | `unregister_python_script_service_dispatcher` | 传输接线 | 摘掉分发器（关停时） | [`cef_py_bind.cpp`](../src/systems/ui/cef/cef_py_bind.cpp#L25) |
| `BindCef` | `active_project_path` | 引擎状态读取 | 编辑器当前场景的项目根，空串表示没有 | [`cef_py_bind.cpp`](../src/systems/ui/cef/cef_py_bind.cpp#L30) |
| `BindCef` | `active_project_session` | 引擎状态读取 | 会话身份的唯一权威副本：`archive_session` 由 Python 铸造、原生持有，单看路径无法区分重开 | [`cef_py_bind.cpp`](../src/systems/ui/cef/cef_py_bind.cpp#L37) |
| `BindCefServiceQueue` | `poll_service_requests` | 宿主控制 | 拉取排队的服务调用（`{request_id, service, function, args}`）；拉出的每条都必须结算 | [`service_queue_bindings.cpp`](../src/systems/script/python/service_queue_bindings.cpp#L80) |
| `BindCefServiceQueue` | `complete_service_request` | 宿主控制 | 结算一条 ticket；转换失败也结算（以失败），否则等待方永久挂起 | [`service_queue_bindings.cpp`](../src/systems/script/python/service_queue_bindings.cpp#L130) |
| — | `InputEvent`（类型） | 载荷 | SDL 与 Web 共用输入队列的载荷。15 个只读字段；身份字段（`archive_session`/`scene_id`/`camera_id`）对 SDL 输入为空。**当前无消费者。** | [`host_support_bindings.cpp`](../src/systems/script/python/host_support_bindings.cpp#L61) |

### 两侧对照：不对称之处是设计，不是遗漏

| 概念 | Web | Python | 为什么不能统一 |
|---|---|---|---|
| 循环控制 | 无 | `should_stop` | 只有拥有循环的一侧需要它；页面没有帧循环 |
| 事件订阅 | `window.on_closed` / `on_close_requested` | 无 | 宿主循环不关心窗口生命周期，只关心自己该不该退 |
| 输入 | `input.dispatch`（写，带 viewport/相机权威校验） | `drain_input_events`（读后丢弃） | 信任前提不同：前者来自不可信前端，后者是引擎自己起的宿主 |
| 项目会话 | 随 `scene.read_active` 带出 | `active_project_path` + `active_project_session` | 宿主需要**权威**会话身份（`archive_session` 单看路径无法区分重开），UI 只需要展示 |
| 归档/文档 | 只有 `project.*` 两个入口，其余走 `bridge.call` | 全部服务在 Python 内 | 实现就在 Python 侧，跨过去只能经过桥 |

## 超时与并发

传输层的超时只有一个语义：Python 运行时没有在期限内回应时，C++ 把回复状态映射成
`504`，Web 收到 `EDITOR_API_TIMEOUT`（`python_service_bridge.cpp`）。
**每个操作自己负责超时**——传输层不叠加第二条 deadline：一个慢的解析和一个挂死的进程要用
不同的方式处理，混在传输层会让两者都不可诊断。

Python 侧是单消费者串行：bridge 请求在同一个 worker 上依次执行，所以两个请求不会
并发改同一个项目；需要并行的工作由操作自己在内部派发。

## 已知缺口

脚本侧的能力缺口（文件音频播放、原生射线查询、指针捕获）随脚本运行时一起删除；重新设计时
先加具名入口，不要恢复"探测不存在的函数再静默降级"的写法。

接口侧的覆盖缺口：

- 窗口/音频/面板请求的**参数校验**只有手动 CEF 冒烟覆盖（`web_messages_cef_smoke.mjs`）。
  读者（`read_close_request`、`read_window_presentation`、`read_audio_control`、`read_panel_options`）
  依赖 `CefV8Value`，单测需要真实 V8 上下文；`tests/tools` 的守卫只检查拒绝码与集成调用，
  不校验这些参数读取器。
- 窗口没有 `getCurrent()`：身份来自 `createPanel` 的返回值与 URL 参数，不提供自查询。
- `SceneReply` 按读/写分族（`SceneReadReply` / `SceneWriteReply`），每个方法只填、
  只读自己族里的字段。
