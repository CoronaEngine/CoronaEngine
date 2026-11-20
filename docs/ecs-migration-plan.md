# DAG 数据层 ECS 化：迁移计划与现状

> 目标：把"entt 当容器用"的现状，改成**真 ECS**——细粒度组件、系统按组件视图工作、
> DAG 依赖由组件访问声明推导。
>
> 线程契约（全程不变）：registry 只在 DAG 线程写；外部写走 `CommandBuffer`，
> 外部读走 `SnapshotChannel`。这条契约是整件事成立的前提。

---

## 1. 现状

| 层 | 现在 | 差在哪 |
| --- | --- | --- |
| 实体 | `EntityId = entt integral + 1` | 同一 actor 在 12 张 device 表里各有一条记录，靠 handle 手工缝合 |
| 组件 | 12 个 `*Device` 聚合体 | 一个 device 同时装状态机 + GPU 资源 + LOD + 物理参数 |
| 系统 | 各扫各的 storage，再 `acquire_read(别人.handle)` | 没有 `view<A,B>()`，跨组件查询靠人脑 |
| 调度 | 手写 `dependencies` | 无法从数据访问推导顺序 |

已就位的基础设施：`EntityStorage`（entt 承载）、命令通道、快照通道、`Ecs::World`。

---

## 2. 目标形态

- **实体 = actor**；profile 是挂在 actor 下的**子实体**（`ProfileOf`），场景归属是
  `SceneMember` 组件。组件清单见 `include/corona/ecs/components.h`。
- **系统 = 视图**：热路径 `entt::group<...>`，冷路径 `view<...>`。
- **调度 = 数据**：系统声明 `ComponentAccess`（读写哪些组件、是否结构变更），
  `FrameGraph::compile()` 推导额外边并与手写 `after` 合并。

---

## 3. 阶段与状态

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| **E0** | `ComponentAccess` + FrameGraph 由访问推导依赖边（含环检测、去重、诊断接口） | ✅ 已完成 |
| **E1** | `Ecs::ActorWorld`：actor/profile/scene 实体与关系；tick 边界随场景命令同步（双写期） | ✅ 已完成 |
| **E2** | `Transform` / `Bounds` 成为权威：每 tick 由 device 镜像到组件，快照改为从组件构建 | ⏳ 下一步 |
| **E3** | Geometry 系统按组件重写（import 状态机、LOD、流式驻留） | ⏳ |
| **E4** | Mechanics / Optics / Acoustics 切组件，删 `ProfileDevice` 指针链 | ⏳ |
| **E5** | 五个系统声明 `ComponentAccess`，DAG 由数据推导（手写边退为兜底）；评估同 tick 并行 | ⏳ |
| **E6** | 删除 12 个 device 与 `EntityStorage` 兼容层，句柄直接是 `entt::entity` | ⏳ |

每阶段独立提交，验收口径：编译 + 相关单测 + 编辑器冒烟（加载/编辑/协作/切场景）。

---

## 4. 已落地的接口

```cpp
// 声明访问（内核层，只依赖 STL）
Corona::Kernel::ComponentAccess access;
access.write<Corona::Ecs::Transform>().read<Corona::Ecs::Bounds>();

// 装图
FrameNodeDesc desc;
desc.name = "Mechanics";
desc.access = access;            // 与手写 dependencies 合并
graph.add_node(desc);
graph.compile();
graph.derived_edges();           // 诊断：哪些边是数据推导出来的
```

```cpp
// 实体模型（DAG 线程）
auto& actors = Corona::Ecs::ActorWorld::instance();
const auto scene = actors.ensure_scene(scene_handle);
const auto actor = actors.ensure_actor(actor_handle);
const auto profile = actors.create_profile(actor, profile_handle);
actors.attach_to_scene(actor, scene, /*order=*/0);
actors.actors_in_scene(scene);   // 按 order 稳定排序
actors.destroy_actor(actor);     // 级联销毁 profile
```

---

## 5. 已知风险

1. **引用稳定性**：entt `destroy()` 是 swap-and-pop，禁止跨语句持有组件引用；
   旧 `WriteHandle` 是 `T*` 语义，迁移到 `get<>()` 时必须逐条检查。
2. **双写不一致**：E1 起 device 与新组件并行存在，E2 必须明确"谁是权威"并加一致性断言。
3. **句柄语义**：`EntityId` 仍是 `uintptr_t` 槽位地址，直到 E6 才换成 `entt::entity`；
   过渡期两套 id 并存，漏转会表现为"查不到实体"而不是崩溃，需要测试兜底。
4. **性能**：组件拆分可能牺牲局部性；E2/E3 后必须用 `group` 与实测数据说话。
