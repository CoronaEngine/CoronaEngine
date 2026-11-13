# S4：跨线程读的处理计划

> **状态（2026-11）**：S3（Storage → EnTT）已落地（`5581680c`，见
> `storage-entt-migration-status.md`），所以本文"先改读、再换容器"的前提已不成立。
> 几何场景快照、CEF 侧的几何/变换/IK 读、脚本侧 `camera_follow_controller` 已改为读快照
> （第 12 轮）；剩下的读点按本文分批处理：S4-1（camera 读接 `camera_snapshot`）与
> S4-4（scene/model_transform/actor_pick 零散点）。

基线：`feat/systems-thread-dag` @ `30f9f155`

---

## 1. 总量与分布

| 存储 | engine_api | CEF | script | 合计 |
| --- | --- | --- | --- | --- |
| geometry | 14 | 5 | 2 | **21** |
| camera | 8 | 2 | 0 | **10** |
| model_resource | 5 | 0 | 0 | 5 |
| profile | 0 | 2 | 2 | 4 |
| actor | 1 | 1 | 1 | 3 |
| scene | 2 | 0 | 0 | 2 |
| model_transform | 0 | 1 | 1 | 2 |
| actor_pick | 1 | 0 | 0 | 1 |
| **合计** | **31** | **10** | **6** | **47** |

`engine_runtime_api.cpp` 是外部线程的统一入口（CEF / Python / Script 都从这里进来），
所以它占 31 处是合理的——它本来就是"跨线程 API 面"。

---

## 2. 按存储的处置决策

### 2.1 camera（10 处）—— 最便宜，先做

**现状**：`SharedDataHub` 已经有 `publish_camera_snapshot()` / `camera_snapshot()`
（`CameraSnapshot` 带 `CameraSnapshotRow`），但 `engine_runtime_api` 的相机读
仍然直接摸 `camera_storage()`。

**处置**：把 10 处读改成读 `camera_snapshot()`。需要先确认 `CameraSnapshotRow`
是否覆盖了这些站点读的字段（位置/朝向/投影参数/尺寸），缺字段就补。

**为什么先做**：通道已经在跑，只差把读点接过去。

### 2.2 geometry（21 处）—— 最大，需要新建快照

> **进展（第 12 轮）**：已新建 `GeometrySceneSnapshot`（`shared_data_hub.h`，字段：
> 节点元数据/构建状态/计数/`transform_handle`/`ik_chains`/聚合 bounds），由
> `GeometrySystem::update()` 末尾发布，未变化的模型复用上一 tick 的列表与 IK 链。
> `engine_runtime_api` 的 14 处读已切换；CEF 的 5 处读（投影的 world AABB / IK 配置、
> gizmo 的位置读写、rig 读取）也已切换。剩余：脚本线程 2 处
> （`camera_follow_controller`）与 `AnimationPlayer`（经讨论保留为活对象，不是场景状态）。

**现状**：没有几何状态快照。读的主要是 `mesh_handles.size()`、
`collision_model_id`、`is_skinned` / `skinned_aabb_valid`、`model_node_bounds` 一类标量。

**处置**：新建 `GeometryStatusSnapshot`（每几何一行，字段走"小而稳"的标量），
由 `GeometrySystem::update()` 发布。**不要**把 mesh 句柄数组塞进快照——
那样每帧复制上千个句柄，收益为负。

已有一处先例：`Geometry::get_render_status()` 在上一轮改造里已经改成
"按 slot 统计 + 读锁"，如果这里也走快照，那个实现可以一起收编。

### 2.3 model_resource（5 处）

只读元数据（`model_id`、导入选项、骨骼信息）。写者是 Geometry 的加载流程。
可以并进 `GeometryStatusSnapshot`（同一发布点），或者单独一个
`ModelResourceSnapshot`。

### 2.4 profile / actor（7 处）

这两类**大部分是外部拥有字段**：`ActorDevice` 和 `ProfileDevice` 的写者主要在 CEF。
所以 CEF 线程内的读**不是跨线程**，不需要改。

真正跨线程的是 `camera_follow_controller.cpp`（Script 线程）里的 6 处：
它读 `profile`(2) / `geometry`(2) / `model_transform`(1) / `actor`(1)。
脚本每帧跑，这 6 处需要快照或论证。

**处置（第 12 轮已做）**：`CameraFollowController` 的 geometry 读改成 `geometry_scene_row()`、
transform 读改成 `transform_view()`，并把 actor -> profile -> geometry -> transform 的解析结果
**缓存**成 `follow_transform_`：每帧只读 transform 快照，actor/profile 只在缓存失效时读一次。
写回仍走 `TransformUpdateCommand`。剩余：缓存刷新时仍会读 editor-owned 的 actor/profile 槽位；
彻底移除需要编辑器发布一个 follow-target 快照（记录为后续项，当前无 UB 风险——槽位有 try 锁）。

### 2.5 scene / model_transform / actor_pick（5 处）

零散点。`scene` 的写者是 DAG（`apply_pending_scene_commands` 在 tick 边界），
所以 CEF 读需要快照。`model_transform` 与 `actor_pick` 各 1-2 处，随所属系统一起处理。

---

## 3. 处理顺序

| 批次 | 内容 | 处数 | 前置 |
| --- | --- | --- | --- |
| S4-1 | camera 读改 `camera_snapshot()`，缺字段补齐 | 10 | 无（通道已存在） |
| S4-2 | 新建 `GeometryStatusSnapshot`，geometry + model_resource 读接过去 | 26 | 需要确定字段集 |
| S4-3 | script 线程的 6 处：先断言，再决定快照 | 6 | 需要确认 actor/profile 写者 |
| S4-4 | scene / model_transform / actor_pick 零散点 | 5 | 随所属系统 |

做完 S4-1、S4-2 后，`Storage` 的 `shared_timed_mutex` 就不再被跨线程读命中，
S3 换容器的前提才算真正具备。

---

## 4. 判定口径（避免越做越大）

不是所有读都需要快照。只有满足下面任意一条才必须改：

1. 读发生在 **非 owner 线程**
2. 且 owner **可能并发写**同一槽位

如果某字段"只在加载期写、加载完成后只读"，正确做法是**加断言 + 注释**，
而不是为它造一套快照。这类字段应该被明确标注出来，而不是混在 47 处里。

建议在 S4-1 动手时顺手给 `SharedDataHub` 的每个存储补一行
"owner 线程 / 写入时机"的注释，后面 S3 就能直接照抄。
