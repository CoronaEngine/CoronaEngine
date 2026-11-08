# 合并后线程边界审计

> 审计对象：`feat/systems-thread-dag` @ `60c7ed57`（已合并远端 13 个提交）
> 审计时间：2026-11-09
> 目的：找出仍然"绕过命令通道"的跨线程写入与跨线程直接调用，给出处置计划。

---

## 1. 审计口径

**判定为绕过**（需要改）：

- 非 owner 线程对 `Storage` 槽位取写锁并修改
- 非 owner 线程直接调用 owner system 的方法（读或写）

**判定为合法**（保留，不列入债）：

| 类别 | 说明 |
| --- | --- |
| 构造期初始化 | 槽位刚 `allocate()`，尚无第二个可见者 |
| 外部拥有字段 | 字段只有外部写、DAG 侧只读（写者即 owner） |
| GPU 生产者 | UI 自己的 `ImageDevice`，receipt 协议保护 |
| 内部队列握手 | `peek_pending_*` / `ack_pending_*` 一类，本质是 mutex 保护的 MPMC 队列 |
| owner 应用命令 | system 在自己的 update 里应用别人投递的命令 |

**合规的三条通道**：写 → `CommandBuffer`；读 → `SnapshotChannel`；通知 → `EventBus` / `EventStream`。

---

## 2. 线程模型（合并后不变）

| 线程 | 承载 | 频率 |
| --- | --- | --- |
| SystemsThread | DAG：`Mechanics → {Geometry, Acoustics} → Optics → Display` | 120 Hz |
| ScriptThread | Script | 60 FPS |
| NetworkThread | Network（含对象同步） | ~55 |
| UI / CEF | UI 系统、编辑器 native API | 事件驱动 |
| main | 窗口与事件泵 | — |

DAG 内部的系统之间是**同线程**；CEF / Script / Network 对 DAG 系统是**跨线程**。

---

## 3. 结论摘要

| 分类 | 位置 | 数量 | 判定 |
| --- | --- | --- | --- |
| A1 槽位直写（构造期） | `engine_runtime_api.cpp` 各 ctor | 13 | 保留 |
| A2 槽位直写（外部拥有字段） | CEF `ik_chains` | 4 | 保留 |
| A3 槽位直写（GPU 生产者） | `vulkan_backend.cpp` | 6 | 保留 |
| B1 跨线程直调（**待改**） | CEF → `NetworkSystem` | **12** | 见 §4.1 |
| B2 跨线程直调（保留） | CEF → `NetworkSystem` 队列握手 | 11 | 保留 |
| B3 同线程兄弟系统直调 | Optics/Mechanics/Display | 5 | 见 §4.2 |
| C owner 应用命令 | acoustics / display / geometry / optics | — | 合法 |

**待改共 12 处**，全部集中在 CEF → `NetworkSystem`。

---

## 4. 逐项清单

### 4.1 CEF → NetworkSystem（23 处，其中 12 处待改）

CEF 侧对 `NetworkSystem` 的调用面（`sys->` 16 + `network->` 7）：

| 方法 | 次数 | 性质 | 处置 |
| --- | --- | --- | --- |
| `broadcast_actor_transform_update` | 1 | **写**（void） | → 命令通道（种类已存在） |
| `broadcast_actor_state_update` | 1 | **写**（void） | → 命令通道（种类已存在） |
| `broadcast_actor_delete` | 1 | **写**（void） | → 命令通道（种类已存在） |
| `session_state` | 4 | **读** | → `NetworkStateSnapshot::active` |
| `broadcast_intent` | 1 | **写**（void） | → 新命令种类 |
| `lock_object` | 1 | **写 + 回执** | → 新命令种类 + `submit_and_wait` |
| `unlock_object` | 1 | **写 + 回执** | → 新命令种类 + `submit_and_wait` |
| `locked_by` | 1 | **读** | → 新命令种类 + 回执 text（或快照） |
| `check_preview_collision` | 1 | **读** | → 新命令种类 + 回执 text |
| `peek_pending_actor_create` | 1 | 队列握手 | 保留 |
| `peek_pending_actor_delete` | 1 | 队列握手 | 保留 |
| `peek_pending_actor_state_update` | 1 | 队列握手 | 保留 |
| `peek_pending_actor_transform_update` | 1 | 队列握手 | 保留 |
| `ack_pending_actor_create` | 1 | 队列握手 | 保留 |
| `ack_pending_actor_delete` | 1 | 队列握手 | 保留 |
| `ack_pending_actor_state_update` | 2 | 队列握手 | 保留 |
| `ack_pending_actor_transform_update` | 1 | 队列握手 | 保留 |
| `pop_pending_actor_scene_snapshot` | 1 | 队列握手 | 保留 |
| `pop_pending_actor_scene_snapshot_request` | 1 | 队列握手 | 保留 |

**关键观察**：远端 `c22e74de` 删掉 LANChat 后，把对象锁能力换成了通用 API
（`lock_object` / `unlock_object` / `locked_by` / `broadcast_intent` /
`check_preview_collision`），**形状与原来的 lanchat 版本一一对应**，签名几乎相同：

```cpp
Network::CollaborationResult lock_object(const std::string& object_id,
                                         const std::string& user_id,
                                         const std::string& operation,
                                         uint64_t now_ms);
struct CollaborationResult { bool ok = false; std::string error; };
```

所以之前为 lanchat 写的那套命令方案可以**原样复用**，只需要换名字。

### 4.2 同线程兄弟系统直调（5 处）

| 调用方 | 被调方 | 方法 | 次数 |
| --- | --- | --- | --- |
| Optics | Geometry | `query_mesh_slots` | 2 |
| Optics | Geometry | `query_shadow_mesh_slots_batch` | 1 |
| Mechanics | Geometry | `get_actor_load_state` | 1 |
| Mechanics | Geometry | `get_skinning_lod_targets` | 1 |
| Display | Optics | `get_target_fps` | 1 |

**判定**：全部在 SystemsThread 上，**同线程只读查询**，不存在数据竞争。

它们仍然违反"读走快照"的教条，但：

- 换成快照意味着每 tick 为每个 camera/actor 发布一份 mesh-slot 列表，收益不明
- `query_mesh_slots` 内部会分配 `std::vector<MeshSlot>`，**这是性能项而非正确性项**
- `get_target_fps` 读的是 `std::atomic`，本身安全

**结论**：列为"记录在案，暂不改"。如果后续做性能优化，优先看
`query_mesh_slots` 每帧多次调用的分配开销。

### 4.3 槽位直写

| 位置 | 数量 | 判定依据 |
| --- | --- | --- |
| `engine_runtime_api.cpp` | 13 | 构造期（Environment/Geometry/Optics/Mechanics/Acoustics/Camera ctor、`add_profile`）+ 外部拥有字段（`remove_profile`、`set_follow_camera` —— `ActorDevice`/`ProfileDevice` 无 DAG 写者） |
| CEF `cef_editor_native_api_handlers.cpp` | 4 | 全部是 `geometry->ik_chains`；全仓只有 CEF 写，Mechanics 只读拷贝 |
| `vulkan_backend.cpp` | 6 | UI 自己的 `ImageDevice` 生产，receipt 协议 |
| `acoustics_system.cpp` | 1 | owner 应用 `command.acoustics_handle`（远端新增的命令通道） |
| `display_system.cpp` | 2 | owner 发布 `published_image` |

`geometry_system.cpp`(18) / `optics_system.cpp`(17) / `mechanics_system.cpp`(8) /
`display_system.cpp` 其余 / `acoustics_system.cpp` 其余：均为 **owner 线程写自己的槽位**，合法。

---

## 5. 远端新增代码的审计结论

| 变动 | 检查结果 |
| --- | --- |
| `ef374791` scene API 聚合（typed V8） | 写路径仍走 `EngineRuntime` API，已被命令通道覆盖；未引入新的槽位直写 |
| `c22e74de` 删 LANChat、保留对象同步 | 引入 5 处新直调（见 §4.1），形状可复用既有方案 |
| 新增 `editor_api/cef_editor_api.cpp`、`editor_dispatch.h` | **不触碰 `SharedDataHub`**，纯转发层，干净 |
| 新增 `collaboration_state.cpp` | **不触碰 `SharedDataHub`**，纯状态机，干净 |
| 新增 `acoustics_commands.h`、`display_frame_commands.h` | 远端自己也在用命令模式，与本方向的思路一致 |
| `ec4878d0` 平台 surface 注册不阻塞 UI 线程 | 涉及窗口线程，未见共享槽位写入 |

---

## 6. 修复计划

### 批次 1（下一批，12 处，纯机械改动）

1. **3 处写** → 复用现有命令种类：
   `BroadcastActorTransformUpdate` / `BroadcastActorStateUpdate` / `BroadcastActorDelete`
2. **4 处读** → `NetworkStateSnapshot::active`（快照 helper 已在位）
3. **5 处对象同步** → 新增命令种类：
   - `BroadcastIntent`（void）
   - `LockObject` / `UnlockObject`（`CollaborationResult` → 回执 ok/error）
   - `LockedBy` / `CheckPreviewCollision`（回执 text）

预估改动：`network_commands.h` +40 行、`network_system.cpp` 分发 +40 行、
CEF 5 个调用点。完成后 CEF → Network 只剩 11 处队列握手。

### 批次 2（可选，性能项）

`query_mesh_slots` 的每帧分配。不是线程安全问题。

### 批次 3（架构项）

`Storage` 容器的后续方向（见 `docs/ecs-entt-migration-analysis.md`）。

---

## 7. 与合并前的差异

| 项 | 合并前 | 合并后 | 来源 |
| --- | --- | --- | --- |
| 跨线程槽位直写 | 0（值路径） | 0（值路径） | — |
| CEF → Network 直调 | 15（全为队列握手） | 23 | 见下 |
| ├ 队列握手 | 15 | 11 | 远端重构后减少 |
| ├ 广播写 | 0 | 3 | **远端新增**（`ef374791` 的 `broadcast_network_editor_actor*` 系列 helper） |
| ├ 会话状态读 | 0 | 4 | **远端新增**（同上） |
| └ 对象同步 | 0 | 5 | **远端新增**（`c22e74de` 删 LANChat 后的通用替代 API） |
| LANChat 相关债 | 25 | 0 | 远端删除 |
| 兄弟系统直调 | 5 | 5 | 未变（同线程只读） |

**净变化**：远端删掉 25 处 lanchat 债，但新引入 12 处直调
（3 广播写 + 4 状态读 + 5 对象同步）。校验方式：`git show ef374791:...` 里这些调用已存在，
而我的合并前版本 `3634fdbc` 里为 0。

所以在"以远端为主"的合并策略下，这 12 处**不是我的回归，而是远端新代码需要补的债**。
