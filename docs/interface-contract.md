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

## Web 面

共 12 个命名空间 / 72 个方法。清单由
`tests/tools/test_typed_native_surface.py` 精确钉死（多一个、少一个都会失败）：

| 命名空间 | 说明 |
|---|---|
| `app` | 进程级操作（关闭） |
| `audio` | Audio 资源与 Actor 播放 |
| `bridge` | 唯一的 Web → Python 入口，一函数宽 |
| `files` | 文件管理器打开（其余文件操作已走 bridge） |
| `gizmo` | 视口 gizmo 指针与目标 |
| `input` | 输入事件派发 |
| `network` | 协作会话与发现 |
| `project` | 项目新建/打开/最近列表 |
| `project_settings` | 当前项目元数据保存 |
| `scene` | 场景读、写、Actor 与相机 |
| `viewport` | 视口配置与拾取 |
| `window` | Dock、面板与相机窗口 |

### window.corona 的完整方法清单

| 命名空间 | 方法 |
|---|---|
| `app` | `closeProcess` |
| `audio` | `playActor`、`playResource`、`stopActor`、`stopResource` |
| `bridge` | `call` |
| `files` | `openFile` |
| `gizmo` | `pointer`、`setTarget` |
| `input` | `dispatch` |
| `network` | `clearDiscoveredPeers`、`connectToPeer`、`getDiscoveredPeers`、`getSessionInfo`、`searchLan`、`setProjectRoot`、`startSession`、`stopSession` |
| `project` | `createMultiplayerProject`、`createWorldProject`、`getProjectLoadStatus`、`getRecentProjects`、`openProject`、`openProjectFile` |
| `project_settings` | `saveActiveProjectInfo` |
| `scene` | `captureScreenshot`、`createActor`、`createCamera`、`deleteActor`、`deleteCamera`、`focusActor`、`getEnvironment`、`importResourceFile`、`isVisionAvailable`、`readActive`、`readActorProperties`、`readActorRig`、`readCameraRenderSettings`、`readModelNodes`、`readScene`、`readTransforms`、`rebindActorResource`、`save`、`selectActor`、`selectModelFile`、`setActorIkChains`、`setActorTransform`、`setCameraRenderSettings`、`setEnvironment`、`setModelNodeState`、`updateActor`、`updateCamera` |
| `viewport` | `configure`、`pick` |
| `window` | `close`、`closeCameraView`、`closeTab`、`createCameraView`、`createPanel`、`cycleCameraWindowMode`、`detachPanel`、`focusTab`、`getCurrent`、`onCloseRequested`、`onClosed`、`redockPanel`、`resizeCameraView`、`setDragRegions`、`suspendCameraViews`、`toggleCameraBorderless`、`toggleCameraMaximized`、`toggleMainBorderless` |

**失败形状**：Promise 拒绝时携带本文件的五个基础设施码之一
（`PERMISSION_DENIED`、`CONTEXT_CHANGED`、`INVALID_ARGUMENT`、`NATIVE_UNAVAILABLE`、
`NATIVE_FAILURE`），或者场景层的符号码（如 `PROJECT_CONTEXT_CHANGED`）原样透传。
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
| [`web_layer_budget.py`](../tests/tools/web_layer_budget.py) | 接口核心行数 ratchet |

## 已知缺口

`embedded_python/script_runtime/BOUNDARY.md` 记录了脚本侧**有意保留**的能力缺口：
文件音频播放（Actor 自带音频已支持）、原生射线查询、指针捕获。补的时候要先在
`script_runtime_api.h` 加具名入口，而不是恢复"探测一个不存在的函数再静默降级"的写法。
