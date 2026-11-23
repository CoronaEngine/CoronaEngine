# 用 EnTT 替换 Storage 容器：可行性分析

<!-- HISTORICAL-DOC: the feasibility analysis below was written before the migration; S3 has since landed (commit 5581680c): all 13 SharedDataHub storages use EntityStorage<T> backed by entt, and the editor API handler it mentions no longer exists (typed V8 binding). The storages were backed by EntityStorage<T> over entt (see include/corona/kernel/entity_storage.h). -->

> 本文只做分析，**不改任何代码**。
> 结论：技术上可行，但不是"删掉 13 个 Storage"这么简单——有三个硬约束必须先解决，
> 其中一个（容器在 Horizon 仓库）决定了这件事无法在 CoronaEngine 单仓库内完成。

---

## 1. 现状：`Storage` 到底是什么

**位置**：`Horizon/include/horizon/core/storage.h`（1053 行）。

注意这一点：它不是 CoronaEngine 的代码，而是 Horizon 仓库提供的通用容器，
通过 `<horizon/core/storage.h>` 引入。CoronaEngine 只是使用者。

**模板形态**

```cpp
template <typename T, std::size_t BufferCapacity = 128, std::size_t InitialBuffers = 2>
class Storage;
```

**内存布局**

```
Storage<T,128>
└── std::list<StaticBuffer<T,128>>        // list：元素地址恒定
    └── StaticBuffer
        ├── std::array<T, 128>                          // 数据本体（常驻）
        ├── std::array<std::atomic<bool>, 128>          // 占用标志
        ├── std::array<std::shared_timed_mutex, 128>    // 每槽一把读写锁
        ├── std::vector<std::size_t> free_indices       // 空闲槽
        └── std::atomic<bool> is_full
```

用 `std::list` 而不是 `vector` 是被迫的：`StaticBuffer` 内含
`std::array<std::shared_timed_mutex,128>`，既不能拷贝也不能移动，
所以只能用节点容器保证扩容时元素地址稳定。

**句柄语义（关键）**

```cpp
using ObjectId = std::uintptr_t;   ///< 存储槽位的实际内存地址
```

`ObjectId` **就是槽位的地址**。`contains()` / `seq_id()` 的实现都是拿 id 做指针运算
反推所在 buffer 和槽位索引。这意味着：

- 句柄不可跨进程、不可持久化（重启后地址全变）
- 槽位回收复用后，新对象拿到的是**同一个** ObjectId（无版本号，无 ABA 保护）
- 句柄天然是 8 字节的 `uintptr_t`

**扩容行为**：当前 buffer 全满后 `emplace_back` 追加一个新 buffer，
容量按 128 递增，**没有硬上限**。（先前口头说过"256 槽硬上限"，那是错的——
`InitialBuffers=2` 只是初始值。）

**在 CoronaEngine 里的规模**

`SharedDataHub` 持有 13 个实例，全部是 `Storage<T,128,2>`：

| # | 存储 | 元素类型 |
|---|---|---|
| 1 | `model_resource_storage` | `ModelResource` |
| 2 | `model_transform_storage` | `ModelTransform` |
| 3 | `geometry_storage` | `GeometryDevice` |
| 4 | `mechanics_storage` | `MechanicsDevice` |
| 5 | `acoustics_storage` | `AcousticsDevice` |
| 6 | `optics_storage` | `OpticsDevice` |
| 7 | `profile_storage` | `ProfileDevice` |
| 8 | `actor_storage` | `ActorDevice` |
| 9 | `camera_storage` | `CameraDevice` |
| 10 | `actor_pick_storage` | `ActorPickDevice` |
| 11 | `environment_storage` | `EnvironmentDevice` |
| 12 | `scene_storage` | `SceneDevice` |
| 13 | `image_storage` | `ImageDevice` |

调用点（`src/` 全量统计）：

- `acquire_write` / `try_acquire_write`：**76 处**
- `acquire_read` / `try_acquire_read`：**303 处**

---

## 2. EnTT 的模型差异

| 维度 | 当前 `Storage` | EnTT `registry` |
|---|---|---|
| 实体标识 | `uintptr_t`（槽位地址，8 字节） | `entt::entity`（index + version，32 位） |
| ABA 安全 | **无** | **有**（version 递增，旧句柄自动失效） |
| 实体存活检测 | `contains(id)` 查 occupied 标志 | `registry.valid(e)` |
| 删除语义 | 归还槽位，地址可复用 | swap-and-pop，**组件内存位置会变** |
| 并发 | 每槽 `shared_timed_mutex`，任意线程可读写 | **完全无锁**，写者唯一性由调用方保证 |
| 遍历 | 无内建遍历接口，各系统自己维护句柄列表 | `view<A,B>()` / `group<A,B>()` 按组件集合过滤 |
| 内存 | `T[128]` 常驻 × 13 | 组件池按需增长 |
| 引用稳定性 | 槽位地址恒定 → `T*` 稳定 | 插入/删除可能搬移组件 → **指针/引用不稳定** |

两条差异对改造影响最大：

1. **引用不稳定**。当前代码大量持有 `WriteHandle`/`ReadHandle`（本质是 `T*`）并在多条语句
   之间使用；换成 entt 后，中间只要有 insert/erase，之前的引用就可能失效。
   这是迁移中最容易被低估的风险点。
2. **无锁**。EnTT 不提供任何同步。当前有 303 处读路径，其中相当一部分目前是靠
   每槽读写锁"任何线程随时可读"来保证正确性的。

---

## 3. 三个硬约束

### C1 · 句柄语义要整体换，且已经外泄

`std::uintptr_t` 形态的句柄已经写死进了大量结构：

- **设备结构里的跨系统链接字段**（`include/corona/shared_data_hub.h`）：
  `std::uintptr_t *_handle` 形态的字段共 **68 处**，包括 `transform_handle`、
  `geometry_handle`、`optics_handle`、`acoustics_handle`、`mechanics_handle`、
  `actor_pick_handle`、`actor_handle`、`model_resource_handle` 等
- **CEF 侧**：`get_handle()` 出现 **35 次**，用于 native 侧的句柄登记与回查
- **运行时 API**：`EngineRuntime` 的 `Camera`/`Geometry`/`Actor` 等对象都以外层
  `handle_` 成员保存句柄

换成 `entt::entity` 意味着这些字段全部改成 32 位实体 ID，并且要重新定义：

- 句柄在 JSON 回包里的表示（CEF 现在把句柄当整数传）
- 句柄在存档/场景文件里的表示（地址不可持久化，这一点现在已经是隐患）

**这既是成本，也是收益**：把地址型句柄换成 index+version，顺带解决了"存档里的句柄
重启后失效"这个既有问题。

### C2 · 容器在 Horizon，不在 CoronaEngine

`Storage` 由 Horizon 提供。要"删掉 Storage 换成 entt"，只有两条路：

| 方案 | 做法 | 代价 |
|---|---|---|
| A | 改 Horizon，让它提供 entt 后端或直接替换 | 触碰第二个仓库；Horizon 是 pinned 依赖（`build_tools/source_workspaces.py`），需要同步 pin |
| B | 把容器 vendored 进 CoronaEngine（`include/corona/kernel/`），Horizon 那份保留不动 | 单仓库可完成，但会与 Horizon 产生分叉，后续 Horizon 更新需要人工对齐 |

按此前明确的"只改 Engine、别的仓库不动"约束，**只有方案 B 可行**。

### C3 · 锁模型必须先变成"单写者 + 快照读"

EnTT 无锁，所以组件必须满足：

- 每个组件只有一个写者线程（通常是它的 owner system）
- 其他线程一律通过快照读取

**这一点当前恰好已经满足了** —— 命令通道改造刚刚完成，跨线程直写已归零：

- `src/systems/network` 的 `acquire_write`：0 处
- `src/engine` 的 `acquire_write`：只剩构造期初始化与外部拥有字段
- 光学 / 几何 / 场景 / 环境 / 变换的跨线程写全部改走 `CommandBuffer`
- 读侧已有 `SnapshotChannel`（TransformSnapshot、CameraSnapshot、
  EnvironmentSnapshot、NetworkStateSnapshot、LanChatQuerySnapshot 等）

换句话说：**命令通道改造是这次 ECS 迁移的前置条件，而它刚做完**。
反过来说，如果在改造前直接换 entt，会立刻暴露几十处数据竞争。

---

## 4. 组件映射建议

13 个 Storage 大体一一对应到组件，但有三处需要拆分：

| 当前存储 | 建议 |
|---|---|
| `GeometryDevice` | 拆成 `GeometryCore`（句柄链接、mesh 句柄表）+ `ModelNodes`（逐节点状态）——后者体积大、更新频率低，混在一起会让遍历缓存命中变差 |
| `ActorDevice` | 拆成 `ActorIdentity`（guid/name/route）+ `ActorNetworkState`（版本号、所有权），网络线程只关心后者 |
| `EnvironmentDevice` | 拆成 `EnvironmentLighting` + `EnvironmentPhysics`（gravity/floor/fixed_dt），两者更新频率差一个数量级 |
| 其余 10 个 | 1:1 映射 |

拆分不是必须的，但 ECS 的价值恰恰来自"按需组合遍历"，维持 13 个巨型组件
等于只换了容器、没换用法。

---

## 5. 迁移路线（每步可编译、可回滚）

| 步骤 | 内容 | 可回滚性 |
|---|---|---|
| **S-1** | 引入依赖。当前 `build_tools/conanfile.py` 的 `requirements()` 里**没有 entt**（27 个依赖里不含），需要 `self.requires("entt/...")` 或 FetchContent。这一步会动依赖锁定，需要重新 `conan install` | 可回滚（移除 requires） |
| **S0** | 引入 `using EntityId = std::uint64_t;` 与 `to_entity()/to_handle()` 转换层，把全仓 `uintptr_t` 句柄先收口到别名。**不换容器**，行为零变化 | 完全可回滚（纯重命名 + inline 转换） |
| **S1** | 挑读写面最窄的一个存储试点（建议 `actor_pick_storage` 或 `environment_storage`）换 entt，其余照旧，双容器并存 | 可回滚（单点改动） |
| **S2** | owner 内部的手写 `for` 遍历改成 `registry.view<...>()`，验证遍历收益 | 可回滚 |
| **S3** | 命令通道里的 handle 字段改 `EntityId`，应用点仍是 owner 线程（通道结构不变） | 可回滚 |
| **S4** | 剩下 12 个存储批量替换，删掉 `Storage` 依赖 | 一次性大改，建议在分支上做 |

**S0 是必须的**：不做句柄抽象直接换容器，改动会同时爆炸在句柄语义和容器实现两个维度上，
无法定位问题。

---

## 6. 收益评估（哪些是真的）

**真实收益**

- 按组件集合过滤遍历：现在每个系统只能"自己的句柄列表 + 逐个 acquire"，没有
  "所有同时具备 A 和 B 的实体"这种查询能力
- 无锁：省掉每槽 `shared_timed_mutex` 的加锁开销（13 × 每槽一把，命中不同槽时
  还有 false sharing 风险）
- 无固定分块：现在每个缓冲固定 128 槽，实体少时 `T[128]` 仍是常驻内存
- ABA 安全：现在槽位复用后旧句柄"看起来仍然有效"

**不是收益（别指望）**

- **单实体访问会变慢**：现在是"句柄 → 直接解引用"，entt 是"registry → sparse set →
  index → 组件池"，多一层间接。以当前代码里 303 处读路径为主的使用形态，
  净性能可能是**下降**的
- 遍历性能提升要靠 `view` 才体现；如果只是"对外层循环换个写法"，不会更快
- 内存：`T[128]×13` 常驻 vs entt 按需池化，取决于实际实体数。实体多时 entt 更省，
  实体少时差不太多（稀疏集本身也有开销）

**结论**：这次改造是**架构收益**（查询能力、线程模型清晰、句柄可持久化），
不是性能优化。如果预期是"换了 entt 帧率变好"，大概率会失望。

---

## 7. 风险与验收

**风险排序**

1. **引用失效**（高）：`WriteHandle` 跨语句持有 + entt swap-and-pop
2. **句柄迁移期双 ID 并存**（高）：S1–S3 期间 `EntityId` 与 `uintptr_t` 同时存在，
   一旦有地方漏转，表现为"查不到实体"而不是崩溃，很难定位
3. **存档/协议兼容**（中）：句柄表示方式变化
4. **单实体访问性能回归**（中，见 §6）
5. **容器分叉**（中）：方案 B 后 CoronaEngine 的容器与 Horizon 不同步

**验收指标（建议）**

- 编译通过（这是硬门槛，前几轮已验证 `ninja -k 0` 是本项目的有效判据）
- 13 个存储、76 处写、303 处读全部清零或改用新接口
- 帧时间对比：主线程 / SystemsThread / CEF 线程分别记录（重点看 CEF 侧，
  它是最容易因锁模型变化而回归的）
- 编辑器实测：模型加载、逐节点编辑、多人协作、LanChat 面板

---

## 8. 与既有文档的关系

`docs/ecs-migration-design.html`（10-30）已经有一版设计，分七个 tab：
总览 / 组件映射 / 快照与命令 / 迁移路线 / 风险与验收 / 代码骨架 / 口径。

需要更新的部分：

- 它的"快照与命令"章节写在命令通道改造**之前**。现在命令通道已经落地
  （`scene_commands.h` / `geometry_commands.h` / `network_commands.h`），
  §3-C3 提到的前置条件已经满足，这是旧稿无法覆盖的新事实
- 它的组件映射没有考虑本文 §4 的三处拆分
- 它没有提到容器在 Horizon 这个跨仓库约束

建议：以本文作为**当前口径**，旧 HTML 保留作为设计细节参考；
如果要合并，优先把旧稿的"代码骨架"和"口径"两节吸收进来。
