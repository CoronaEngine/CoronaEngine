# 跨线程交互收敛：绕过命令通道的清单与改造记录

> 基线：`feat/systems-thread-dag` @ `f1d4ef0a` · 本文档随改造逐批更新
> 判据与背景见 `docs/interface-decisions.md` 第 8 节与 `include/corona/kernel/command/snapshot_channel.h` 的头注释

## 0. 什么算"绕过"

| 分类 | 判定 | 处理 |
| --- | --- | --- |
| **A 跨线程写槽位** | 非拥有者线程 `acquire_write()` / `try_acquire_write()` 某个 `Storage<T>` | 改命令通道 |
| **B 跨线程直接调用** | 一条线程直接调用另一条线程所拥有对象的非原子方法/字段 | 改命令 / 快照 / 事件；纯原子量可保留 |
| **C 构造期初始化** | 刚 `allocate()` 完、尚无其他可见者时的默认值填充 | 保留（这是对象的"出生"，不是竞争） |
| **D 外部拥有的结构体** | DAG 系统从不写它 | 保留（本就在自己的 owner 线程上写） |
| **E GPU 帧交换** | `ImageDevice.image` / `submit_receipt` / `consumed_receipt` | 保留（receipt 协议，非线程原语） |
| **F 句柄语义改造** | actor create/destroy 的同步构造契约 | 延后（需 Entity(index+generation)，见设计文档 P4） |

## 1. A 类：跨线程写槽位

### 1.1 已完成

| # | 位置 | 存储 | 改为 | 批次 |
| --- | --- | --- | --- | --- |
| A1 | `engine_runtime_api.cpp` `Geometry::set_position/set_rotation/set_scale` | ModelTransform | `TransformUpdateCommand`（Position/Rotation/Scale） | 1 |
| A2 | `engine_runtime_api.cpp` `Geometry::set_matrix` | ModelTransform (+affine_basis) | `TransformUpdateCommand` + 新增 `TransformField::Matrix` | 1 |
| A3 | 同上四个 getter | — | 改读 `transform_view()`（快照 ⊕ pending） | 1 |
| A4 | `sync_engine.cpp` 远程 `xform` | ModelTransform | `TransformUpdateCommand`（All） | 2 |
| A5 | `sync_engine.cpp` 远程 `optics` | OpticsDevice | `OpticsUpdateCommand`（16 标量位） | 2 |
| A6 | `sync_engine.cpp` 远程 `__scene__/environment` | EnvironmentDevice | `EnvironmentUpdateCommand` + 新增 `EnvironmentField::Exposure` | 2 |
| A7 | `engine_runtime_api.cpp` Scene 的 11 个 setter | SceneDevice | 新 `SceneCommandChannel`（Fifo，tick 边界应用） | 3 |
| A8 | `engine_runtime_api.cpp` `Scene::is_enabled/is_simulation_enabled` | — | 改读对象内镜像（读己之写） | 3 |
| A9 | `engine_runtime_api.cpp` `set_default_surface` | CameraDevice.surface | `CameraStateUpdateCommand`（Surface 位）；原来是**未加锁**的 range-for 直接赋值 | 4 |
| A10 | `engine_runtime_api.cpp` `Geometry::set_native_local_correction` | GeometryDevice | 新 `GeometryCommandChannel`（Fifo），应用在 `GeometrySystem::update` 开头 | 5 |
| A11 | `engine_runtime_api.cpp` `Environment::set_authored_lighting` | EnvironmentDevice（`shared_ptr<json>` + revision + 派生 sun） | 扩 `EnvironmentField::AuthoredLighting` / `SunColor`，整份配置随命令下发，tick 边界统一应用 | 6 |
| A14 | `engine_runtime_api.cpp` `Camera::pick_model_node_at_pixel` | ActorPickDevice | 写侧改入队 `enqueue_actor_pick_request`；读侧仍读 `result_*`（只读锁）。原先直接写 `pending/x/y` | 7 |
| B1 | `engine_runtime_api.cpp` `Geometry::get_render_status` | 跨线程直调 `GeometrySystem::query_mesh_slots()` | 改为按 slot 直接统计（判据与 `build_mesh_slots` 的 `slot.valid` 对齐），不再进入 DAG 系统的 `impl_` | 8 |
| A12 | `engine_runtime_api.cpp` `Geometry::set_model_node_state` / `set_model_node_overrides` | GeometryDevice | 同步只校验"几何存在 / 索引越界 / 非有限数"，改写入队（Fifo）；应用点负责 `ModelNodes::set`/`update_instances`/`apply_overrides` 与失败回滚 | 9 |
| A15 | `editor_scene_service.cpp` 三处 IK 写入（场景恢复 / 远端协作应用 / `setActorIkChains`） | GeometryDevice.ik_chains | 改入队 `GeometryField::IkChains`；`setActorIkChains` 的期望版本与 `preserve_runtime` 改读快照，回复用请求推导的 revision（有界等待快照追上） | 12 |

配套修复：
- `publish_transform_snapshot_from_storage()` 原先**全仓无调用点**，TransformSnapshot 通道是死的；已在 `OpticsSystem::apply_camera_and_transform_updates()` 里补上发布。
- `EnvironmentUpdateKey` 补 `merge()`：网络线程与 UI 线程共队列后，部分字段命令不再互相覆盖。
- `OpticsUpdateKey` 已有 `merge()`，确认无需改动。

### 1.2 待完成

| # | 位置 | 存储 | 计划 |
| --- | --- | --- | --- |
| A13 | ~~`cef_editor_native_api_handlers.cpp` ×4（该文件已随 manifest 退休删除）~~ | GeometryDevice（`ik_chains` + revision） | **更正**：原判定"D 类"有误——`MechanicsSystem::unregister_foot_plant_chain`（`mechanics_system.cpp:2022-2031`）也写 `ik_chains`，所以 CEF 直写是真正的 A 类。已在 A15 改成命令通道 |
| B2 | `editor_scene_service.cpp` `get_network_system()` + 约 40 处调用（旧 handler 文件已删除） | CEF → `NetworkSystem` | 已完成：命令 + 快照，见 §2.0 |

### 1.3 A 类完成度

`src/systems/network` 与 `src/engine` 中**已无值路径直写**。剩余 `acquire_write` 全部属于：

| 类别 | 数量 | 说明 |
| --- | --- | --- |
| C 构造期 | 11 | Environment / Geometry / from_image / Optics / Mechanics / Acoustics / Camera×2 / add_profile 及其 deallocate |
| D 外部拥有 | 2 | `Actor::remove_profile`、`set_follow_camera`（CEF 的 `ik_chains` 已迁到 A15） |
| E GPU 生产者 | 6 | `vulkan_backend.cpp` 的 `ImageDevice`（receipt 协议） |

### 1.3 保留（非绕过）

| 位置 | 理由 |
| --- | --- |
| `Environment::Environment()` / `Geometry::Geometry()` / `Geometry::from_image()` / `Optics|Mechanics|Acoustics` 构造 / `Camera` 构造 | C 类：槽位刚分配，仅本线程可见 |
| `Actor::add_profile` / `remove_profile` / `set_follow_camera` | D 类：`ActorStorage` / `ProfileStorage` 在 `src/systems/**` 中零写入（已 grep 确认） |
| `vulkan_backend.cpp` ×6 | E 类：UI 层自己产出的 `ImageDevice`，受 `SurfaceFrameCoordinator` + receipt 保护 |

## 2. B 类：跨线程直接调用

| # | 位置 | 从 → 到 | 判定 | 处理 |
| --- | --- | --- | --- | --- |
| B1 | `engine_runtime_api.cpp` `Geometry::get_render_status()` | 外部 → `GeometrySystem::query_mesh_slots()` | **违规**（跨线程直调 DAG 系统） | 待办：Geometry 发布 mesh-slot 快照，改读快照 |
| B2 | `editor_scene_service.cpp` `get_network_system()` + 约 40 处调用（旧 handler 文件已删除） | CEF → `NetworkSystem` | **已完成**：`NetworkCommandChannel`（控制面）+ `NetworkStateChannel` 快照（状态面）；数据面 `peek_pending_*` 本身是内部队列握手，保留 |
| B3 | `display_system.cpp:466` `optics_system_` | Display → Optics | 允许 | 同一 DAG 线程 |
| B4 | `mechanics_system.cpp:66` `geometry_sys` | Mechanics → Geometry | 允许 | 同一 DAG 线程 |
| B5 | `engine.cpp:269` `get_system("UI")` | 主线程 → UI | 允许 | 同一主线程 |
| B6 | `set_editor_camera_input_enabled` → `CameraFollowController` | 主/CEF → 脚本线程对象 | 允许 | 目标字段是 `std::atomic_bool`，无撕裂 |

### 2.0 B2 完成情况（第 5 轮）

新建 `include/corona/systems/network/network_commands.h`：

- `NetworkStateSnapshot` + `NetworkStateChannel` — 会话 active/role/端口 + 发现对端列表，
  由 `NetworkSystem::update()` 每 tick 发布
- `NetworkCommand` + `NetworkCommandChannel`（Fifo）— 9 种只写控制命令，
  在 `NetworkSystem::update()` **开头** drain（早于 "session 不 Active 就 return"，
  这样 `set_project_root` / `clear_discovered_peers` / `set_sync_paused` 在空闲态也生效）

CEF 侧改动：

| 原调用 | 改为 |
| --- | --- |
| `read_network_peers(sys)` → `sys->discovered_peers()` | 读 `NetworkStateChannel` 快照 |
| `read_network_session(sys)` → 6 个 getter | 读同一份快照 |
| `sys->clear_discovered_peers()` | `NetworkCommand` |
| `sys->set_project_root(...)` | `NetworkCommand` |
| `sys->set_sync_paused(...)` | `NetworkCommand` |

仍待迁移：`broadcast_actor_transform_update` / `broadcast_actor_delete` /
`broadcast_actor_state_update` / `request_actor_scene_snapshot` /
`broadcast_actor_scene_snapshot` / `lanchat_broadcast_intent`（命令种类已在通道里定义好，
只是 CEF 侧调用点还没替换），以及请求/应答那 6 个方法。

**第 6 轮补完**：`stop_session` / `lanchat_broadcast_intent` / `broadcast_actor_transform` /
`broadcast_actor_delete` / `request_actor_scene_snapshot` / `broadcast_actor_scene_snapshot`
六个 void 调用点已改命令通道。

### 2.0.2 B2-3 回执机制（第 7 轮）

约束：**只改 CoronaEngine，不动 CoronaEditor / Horizon** → §2.1 的路线 1（异步化到前端）排除，
采用路线 2：**命令 + 有界同步等待**。

```cpp
// 入队并等回执（默认 150ms：60Hz 下典型延迟 < 20ms）
std::optional<NetworkCommandResult> submit_and_wait(NetworkCommand command,
                                    std::chrono::milliseconds timeout = kNetworkReplyTimeout);
```

- `NetworkCommand::request_id`（0 = fire-and-forget）
- 应用点在每个需要回执的分支调用局部 `reply(ok)` → `NetworkReplyQueue::push({id, ok})`
- 超时后 `discard(request_id)` 清掉迟到回执，避免队列无限增长
- 调用方只阻塞在回执条件变量上，**不再摸 NetworkSystem 内部**；超时按失败处理

已迁移 8 类命令：`RegisterActorIdentity` / `ClaimActorOwnership` / `LanChatStartRoom` /
`LanChatStartLocalRoom` / `LanChatJoinRoom` / `LanChatRestoreHistoryRoom` /
`LanChatStopLocalRoom` / `LanChatLeaveRoom` / `SetLanChatEventCallback`。

CEF 侧同步调用点全部替换（含 `register_actor_identity` 在 actor 应用路径的两处）。

**第 8 轮**：回执新增 `has_message` + `LanChatMessage message` 负载，
CEF 侧加两个 helper 把回执还原成调用方原本期望的结果类型
（`LanChatResult` / `LanChatMessageResult`），因此各站点周围的 `reply.*` 赋值一行未动：

```cpp
const auto result = submit_lanchat_message(
    Corona::Network::NetworkCommandKind::LanChatSendSystemToUser, {...});
return native_success(build_lanchat_message_result(result));   // 原样保留
```

已迁移：`lanchat_lock_object` / `lanchat_unlock_object` / `lanchat_send_message_ex` /
`lanchat_send_agent_reply_ex`×2 / `lanchat_send_system_message_to_host_ex` /
`lanchat_send_system_message_to_user_ex` / `lanchat_register_agent` / `lanchat_remove_agent`。
超时统一返回 `NETWORK_TIMEOUT` 错误串。

### 2.0.1 CEF 侧剩余直调清单（第 6 轮实测，第 7 轮后已大幅缩减）

用 `grep -o "sys->[a-z_]*"` 统计，剩余 52 处：

| 组 | 方法（出现次数） | 处理 |
| --- | --- | --- |
| LanChat 读 | `lanchat_members` `lanchat_agents`(2) `lanchat_history`(2) `lanchat_history_rooms` `lanchat_locked_by` `lanchat_check_preview_collision` | 扩 `NetworkStateSnapshot`（面板型数据），或单独开 `LanChatStateChannel` |
| LanChat 轮询 | `lanchat_pop_sync_event` `lanchat_pop_room_event` `lanchat_pop_agent_trigger` `lanchat_pop_coordinator_sync_message` | **保留**：内部队列握手，与 `peek_pending_*` 同类 |
| LanChat 写 | `lanchat_start_room` `lanchat_start_local_room` `lanchat_stop_local_room` `lanchat_join_room` `lanchat_leave_room`(2) `lanchat_register_agent` `lanchat_remove_agent` `lanchat_send_message_ex` `lanchat_send_system_message_to_host_ex` `lanchat_send_system_message_to_user_ex` `lanchat_send_agent_reply_ex`(2) `lanchat_load_history_room` `lanchat_load_history_agents` `lanchat_restore_history_room` `set_lanchat_event_callback`(2) | 扩 `NetworkCommandKind` |
| 请求/应答 | `start_session` `start_discovery` `connect_to_peer` `register_actor_identity`(3) `claim_actor_ownership` `lanchat_lock_object` `lanchat_unlock_object` | 等 §2.1 取舍 |
| 状态读 | `session_port`(4) `local_peer_id`(4) `host_port`(2) `session_state` `discovered_peers` | 快照（`local_peer_id` 尚未进快照，需补） |
| 数据面 | `peek_pending_*` `ack_pending_*` `pop_pending_actor_scene_snapshot*`（8 处） | **保留** |

`local_peer_id` 是漏项：`read_network_session()` 改快照后仍有 4 处直接读它。
第 6 轮已补进 `NetworkStateSnapshot::local_peer_id` 并改为读快照（`network_snapshot_peer_id()`）。

### 2.0.3 直调数量演进

（上表是第 6 轮基线 52 处，下面是逐轮收敛结果。）

第 6 轮 52 → 第 7 轮 44 → 第 8 轮 35 → 第 9 轮 28 → **第 10 轮 18**。剩余 18 处构成：

| 组 | 方法 | 数量 | 状态 |
| --- | --- | --- | --- |
| 轮询/数据面握手 | `peek_pending_*`(4) `ack_pending_*`(5) `pop_pending_actor_scene_snapshot*`(2) `lanchat_pop_*`(4) | 15 | **保留**（内部队列，按设计不做迁移） |
| 广播写（第 10 轮漏计） | `broadcast_actor_state_update` `broadcast_actor_create` | 2 | 第 11 轮 |
| 冲突探测 | `lanchat_check_preview_collision` | 1 | 第 11 轮 |

**LanChat 面板读的发布策略**（第 10 轮定）：不做每 tick 深拷贝——`history` 可能上千条。
改为**按需拉取**：CEF 投 `RefreshLanChatPanel` / `LoadLanChatRoomHistory` 命令，
网络线程填好后发布到 `LanChatQueryChannel`，调用方等回执后再读。
单生产者单消费者 + 回执先行，覆盖式发布安全。

| 剩余组 | 数量 | 下一轮处理 |
| --- | --- | --- |
| 状态读（`session_port`×4 `host_port`×2 `session_state`） | 7 | 并入快照 |
| LanChat 面板读（`history`×2 `agents`×2 `members` `locked_by` `load_history_*`） | 8 | 单独 `LanChatStateChannel` |
| LanChat 写（`send_message_ex` `send_system_*`×2 `send_agent_reply_ex`×2 `register_agent` `remove_agent` `lock/unlock_object`） | 11 | `submit_and_wait` + `LanChatResult` 负载 |
| `start_session` / `start_discovery` | 2 | 同上 |
| `lanchat_check_preview_collision` | 1 | 读快照 |
| 轮询/数据面（`lanchat_pop_*` `peek_pending_*` `ack_pending_*` `pop_pending_*`） | 12 | **保留** |

### 2.1 B2 设计（CEF → NetworkSystem）

CEF 线程直接调用 `NetworkSystem` 约 40 处，按语义分三组：

| 组 | 方法 | 是否需要同步返回值 | 处理 |
| --- | --- | --- | --- |
| **只读状态** | `session_state` / `session_role_name` / `peer_count` / `host_address` / `host_port` / `session_port` / `discovered_peers` | 否（可以落后一拍） | **快照**：`NetworkStateSnapshot` 由 `NetworkSystem::update` 每 tick 发布 |
| **只写控制** | `set_sync_paused` / `set_project_root` / `clear_discovered_peers` / `register_actor_identity` / `claim_actor_ownership` / `broadcast_actor_transform_update` / `broadcast_actor_delete` / `request_actor_scene_snapshot` / `broadcast_actor_scene_snapshot` / `lanchat_broadcast_intent` | 否 | **命令**：`NetworkCommandChannel`（Fifo，顺序有意义） |
| **请求/应答** | `start_session` / `stop_session` / `start_discovery` / `connect_to_peer` / `lanchat_lock_object` / `lanchat_unlock_object` | **是**（返回值直接进 JSON 回包） | 见下方取舍 |
| **数据面握手** | `peek_pending_actor_create` / `ack_*` / `pop_pending_actor_scene_snapshot*` | 是（轮询式） | **保留**：它本身就是 NetworkSystem 内部 mutex 保护的生产者→消费者队列，不是直写共享状态 |

**请求/应答那组的取舍**（必须在动手前定，否则会做出半吊子）：

1. **异步化到底**：命令带 `request_id`，`NetworkSystem` 应用后 push
   `NetworkCommandResult{request_id, ok, payload_json}` 到完成队列；
   CEF handler 返回 `{pending: true, request_id}`，前端轮询/等待。
   → 架构最干净，但要改前端调用点（LanChat 已移除，这些调用点随之删除）。
2. **有界同步等待**：只把 handler 改成"发命令 + 在完成队列上带超时等结果"。
   → 不改前端，但仍有一次跨线程阻塞等待，只是从"抢内部锁"变成"走通道 + 等回执"。
3. **维持现状并登记**：把这 6 个方法标注为"控制面同步 API"。

推荐顺序：先做**只读状态快照**（无契约风险）→ 再做**只写控制命令**（无返回值）
→ 最后在 1 与 2 之间做决定，并记录理由。

`NetworkSystem` 的 `start_session` / `connect_to_peer` 内部本身已经持 `impl_->mutex`，
所以当前直调是"线程安全但跨线程"，不是数据竞争；B2 的收益主要是**所有权清晰**与
**不再让 CEF 线程摸对方内部结构**，优先级低于 A 类。

## 3. 批次状态

| 批次 | 内容 | 状态 |
| --- | --- | --- |
| 1 | 变换收口（A1–A3 + 快照发布修复） | ✅ 编译 + 测试 |
| 2 | 网络线程（A4–A6 + merge） | ✅ 编译 + 测试 |
| 3 | SceneDevice（A7–A8 + tick 边界钩子） | ✅ 编译 + 测试 |
| 4 | 默认 surface（A9） | ✅ 编译 + 测试 |
| 5 | GeometryDevice 通道 + 标量位（A10） | ✅ 编译 + 测试 |
| 6 | Environment 结构化负载（A11） | ✅ 编译 + 测试 |
| 7 | 拾取请求（A14） | ✅ 编译 + 测试 |
| 8 | 渲染状态不再跨线程直调（B1） | ✅ 编译 + 测试 |
| 9 | Geometry 逐节点编辑（A12） | ✅ 编译 + 测试 |
| 10 | 网络只读状态快照（B2-1） | ✅ 编译 + 测试 |
| 11 | 网络只写控制命令（B2-2，3/9 调用点） | ✅ 编译 + 测试 |
| 12 | 网络广播类调用点（B2-2 剩余 6 处） | ✅ 编译 + 测试 |
| 13 | `local_peer_id` 等状态读补进快照 | ✅ 编译 + 测试 |
| 14 | LanChat 写命令（17 处） | ✅ 编译 + 测试（8 类已迁移） |
| 15 | LanChat 面板读快照（约 8 处） | ⏳ 下一轮 |
| 16 | 剩余状态读并入快照（7 + 1 处） | ⏳ |
| 17 | LanChat 结果型写（`LanChatResult` 负载，9 处） | ✅ 编译 + 测试 |
| 18 | `start_session` / `start_discovery` + 状态读并入快照（7 处） | ✅ 编译 + 测试 |
| 19 | LanChat 面板读快照（`LanChatQueryChannel` 按需拉取，7 处）+ `connect_to_peer` + `locked_by` | ✅ 编译 + 测试 |
| 20 | 广播写 `broadcast_actor_state_update` / `broadcast_actor_create` + `check_preview_collision` | ✅ 编译 + 测试 |
| 21 | `seed_network_editor_state` 内部两处直调（最后一个直调面） | ✅ 编译 + 测试 |

### 2.0.4 最终状态（第 11 轮收尾）

`grep -o "sys->[a-z_]*\|network_system\.[a-z_]*"` 在 CEF 侧只剩 **15 处**，全部是内部队列轮询：

```
peek_pending_actor_{create,delete,state_update,transform_update}   4
ack_pending_actor_{create,delete,state_update,transform_update}    5
pop_pending_actor_scene_snapshot{,_request}                        2
lanchat_pop_{sync_event,room_event,agent_trigger,coordinator_sync_message}  4
```

它们不是"绕过通道的写入"：`NetworkSystem` 内部用 `impl_->pending_mutex` 保护的
生产者→消费者队列，CEF 只是消费者。改成命令通道只会把"取走一条待办"变成
"再排一条命令去取待办"，没有收益。

`seed_network_editor_state(NetworkSystem&, role)` 这个把对方对象整个传进来的 helper
也改掉了：两个调用改命令，签名去掉 `NetworkSystem&`，只剩 `role`。

## 5. 结论

| 项 | 状态 |
| --- | --- |
| A 类（跨线程直写槽位） | **0 处**，全部改命令 / 快照 |
| B 类（跨线程直调对方对象） | 仅剩 15 处队列轮询，按设计保留 |
| 新增边界头 | `scene_commands.h` `geometry_commands.h` `network_commands.h` |
| 架构约束 | 只改 CoronaEngine；CoronaEditor / Horizon 未改动 |

## 4. 验证

### 4.1 真实增量构建（2026-11-08）

构建树 `build/conan/examples/relwithdebinfo`，RelWithDebInfo。

```bat
call "<VS2022>\VC\Auxiliary\Build\vcvars64.bat"
ninja -C build\conan\examples\relwithdebinfo -k 0
```

**必须在 vcvars 环境里跑 ninja**：编译命令不显式带 MSVC 标准库路径，靠 `INCLUDE`
继承；在裸 shell 里跑会得到 `fatal error C1083: 无法打开包括文件: "algorithm"`。
（`.workspace/tmp/build-k.bat` 已封装。）

结果：647 个 target 全部构建，**唯一失败**是既有缺陷
`corona_ui_surface_removal_race_tests` —— 它的 CMake 源列表只有
`src/shared_data_hub.cpp` + `src/systems/ui/ui_surface_lifecycle.cpp`，
两个文件都没有 `main()`，必然 `LNK2019: 无法解析的外部符号 main`。与本改造无关。

产物：`corona_engine.exe` 10:25 重新链接；`optics_system.cpp.obj` 10:22 编译通过
（此前用过期 compile db 做的 `/Zs` 检查失败是参数过期，不是代码问题）。

### 4.2 测试

`ctest -C RelWithDebInfo`，14 个相关用例全绿：

```
CommandBufferTests  SnapshotChannelTests  TickBoundaryTests  InteractionBoundaryTests
OpticsCommandTests  GeometryStorageSnapshotTests  OpticsStorageSnapshotTests
ActorSelectionRoutingTests  OpticsActorPickReadbackWorkerTests  SceneTreeActorTests
ModelNode*（3 个）…
100% tests passed, 0 tests failed out of 14
```

### 4.3 两个既有失败（已确认非本次回归）

| 用例 | 现象 | 判定依据 |
| --- | --- | --- |
| `NetworkProtocolTests` | `0xC0000409` | 把 `sync_engine.cpp` 还原到 HEAD 重建后**同样崩溃** |
| `SceneFolderTests` | `0xC0000409` | 目标只链 `test_scene_folder.cpp` + `scene_folder.cpp` + Horizon/nlohmann/assimp，不含任何引擎源码，改动不可能影响 |

### 4.4 静态检查（辅助）

语法检查用 MSVC 的 `cl /Zs`（或直接 `dev.py build-fast --target-family tests`），用于改一处验一处。
注意该 db 生成于 11-07 18:47，晚于它的源码变更会让部分 TU 的参数过期，
所以**以 4.1 的真实构建为准**。

## 5. 复现

```bash
# 语法检查（按文件名字串匹配 compile db）
node .workspace/tmp/tools-syntax.js engine_runtime_api.cpp sync_engine.cpp

# 把编辑工具写出的 CRLF 还原成 HEAD 的 LF，避免整个文件出现在 diff 里
node .workspace/tmp/normalize-eol.js

# 盘点外部线程仍在直写槽位的地方
grep -rn "acquire_write" src/engine src/systems/ui src/systems/network --include=*.cpp
```
