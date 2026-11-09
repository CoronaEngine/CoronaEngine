# Storage → EnTT 迁移：实施记录

> 本文记录**实际做了什么**。设计分析见 `entt-full-replacement-analysis.md`，
> 跨线程读计划见 `s4-cross-thread-reads-plan.md`。

基线：`feat/systems-thread-dag`，起点 `ab989237`。

---

## 1. 已完成

| 步骤 | 内容 | 提交 |
| --- | --- | --- |
| A | 审计列出的 12 处 CEF → NetworkSystem 直调改命令/快照 | `bf660173` |
| S0 | conanfile 加 `entt/3.16.0`，`conan install` 通过 | `6cf1f080` |
| S1 | 引入 `Corona::EntityId` 别名，全仓约 300 处句柄声明收口 | `6cf1f080` `0aff7f68` |
| S2 | `Ecs::World` 骨架 + 13 组件清单 + 契约测试 | `30f9f155` |
| S3 | **13 个 SharedDataHub 存储全部换成 entt 后端** | `5581680c` |
| S5 | Engine 源码不再引用 `horizon/core/storage.h` | 见下 |

### S3 的技术要点

`include/corona/kernel/entity_storage.h` 提供 `EntityStorage<T>`，
保持旧 `Storage` 的 API（`allocate/deallocate/acquire_*/try_acquire_*` /
`count/contains/seq_id/begin/end/cbegin/cend`），底层是 `entt::registry` 的组件池。
Engine 里 250+ 调用点**一行未改**。

三个必须处理的细节：

1. **实体 ID 要 +1**
   全仓大量 `if (handle == 0) return;` 用 0 表示无效，而 entt 的第一个实体就是 0。
   `id = integral(entity) + 1` 即可让所有既有判断继续成立。

2. **组件要包一层 shared_ptr**
   `GeometryDevice` 内含 `GpuMemToken`（拷贝被显式删除），
   直接把设备放进组件池会在某条拷贝路径上编译失败。
   用 `Holder { std::shared_ptr<T> }` 包一层后组件可拷贝、`T` 保持 move-only，
   语义上仍是同一个设备实例。

3. **锁改成条带锁**
   旧实现每槽一把 `shared_timed_mutex`；新实现是 4096 把按实体索引哈希。
   这是**过渡兼容层**：`s4-cross-thread-reads-plan.md` 里 47 处跨线程读改成快照后，
   这些锁可以直接删掉，那时 entt 才是真正无锁的。

### S3 验证

```
只构建 corona_engine + 11 个测试目标 → BUILD_EXIT=0
ctest → 11/11 通过
```

其中 `GeometryStorageSnapshotTests` / `OpticsStorageSnapshotTests`
原本是照着 Horizon `Storage` 的语义写的（含非阻塞 `try_acquire_*_nowait`），
它们通过说明兼容层的行为是一致的。

### Horizon 是否需要改：**不需要**

`Horizon/src/hardware_wrapper_vulkan/resource_pool.h` 自己就是
`horizon::core::Storage<Slot>` 的使用者，与 Engine 平级。
Engine 停止使用后，Horizon 的引用不受影响，`horizon/core/storage.h` 保持原样。

---

## 2. 尚未完成

### S4 · 跨线程读改快照（47 处）

容器已经换成 entt，但**锁还在**（条带锁），所以当前行为与迁移前一致。
要真正用上 entt 的无锁特性，需要按 `s4-cross-thread-reads-plan.md` 把
47 处跨线程读改成快照：

| 存储 | 处数 | 计划 |
| --- | --- | --- |
| geometry | 21 | 新建 `GeometryStatusSnapshot` |
| camera | 10 | 接 `camera_snapshot()`（通道已存在） |
| model_resource | 5 | 并入几何快照 |
| profile / actor | 7 | 大半是外部拥有字段，只需处理 Script 线程那 6 处 |
| scene / model_transform / actor_pick | 5 | 零散 |

### 遗留引用

| 位置 | 说明 |
| --- | --- |
| `tests/kernel/test_storage_lock_timeout.cpp` | **刻意保留**：它测的是 Horizon `Storage` 自身的锁超时契约，不是 Engine 的用法 |
| `tests/systems/display/test_published_image.cpp` | 仍 include `horizon/core/storage.h`，待确认是否需要跟着改 |

### 后续可选

- 把 250+ 调用点从 `acquire_read/acquire_write` 改写成原生
  `registry.get<T>()` / `view<T>()`——现在改不改都不影响正确性，
  只有需要多组件查询时才值得做
- 13 个设备类型是否拆分（Geometry / Actor / Environment 各拆两块），
  见分析文档 §4
