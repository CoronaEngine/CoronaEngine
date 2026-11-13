# Engine 全量用 EnTT 替换 Storage：分析

<!-- HISTORICAL-DOC: this page describes the tree as it was when it was written. The editor API handler has since been split; the scene service now lives in src/systems/ui/cef/editor_scene_service.cpp and the Python bridge in src/systems/ui/editor_api/python_service_bridge.{h,cpp}. -->

> 只分析，不改代码。基线：`feat/systems-thread-dag` @ `ab989237`（已合并远端 13 提交）
> 本文取代 `docs/ecs-entt-migration-analysis.md`（那份写在合并前）

---

## 0. 三句话结论

1. **不需要动 Horizon** —— Horizon 自己也在用这个模板（Vulkan 资源池），Engine 停用不影响它。
2. **全量替换的机械量约 260 处**（13 个存储 × 6 类 API 调用点），这部分是体力活。
3. **真正的成本不在这 260 处，而在 47 处跨线程读** —— EnTT 无锁，
   这些读点要么改快照、要么必须证明 owner 不会并发写。这是唯一有设计含量的部分。

---

## 1. Horizon 是否需要改：**不需要**

直接证据：

```
Horizon/src/hardware_wrapper_vulkan/resource_pool.h:320
    mutable horizon::core::Storage<Slot> storage_ {};

同文件 48/65/74/91 行：Storage<Slot>::ReadHandle / WriteHandle
```

Horizon 的 Vulkan 资源池**自己就是 Storage 的使用者**，和 Engine 是平级关系，不是依赖关系。

唯一的耦合是命名空间别名：

```cpp
// Horizon/tests/core/test_public_headers.cpp:8
static_assert(std::is_same_v<horizon::core::Storage<int>,
                             Corona::Kernel::Utils::Storage<int>>);
```

`horizon/core/storage.h` 把模板定义在 `Corona::Kernel::Utils` 里，Horizon 再用别名引用。
这只是"同一个模板两个名字"，不构成"Engine 改了 Horizon 就得改"。

**结论**：
- Engine 停止使用 `Storage` → **Horizon 无感**，只要不删 `horizon/core/storage.h`
- 反过来说，如果将来想从 Horizon 删除 `Storage` 模板本身 → 那才需要改 Horizon
  （`resource_pool.h` 要重写）。**本次不需要**。

---

## 2. 全量规模（精确统计）

### 2.1 容器本体

`SharedDataHub` 里 13 个 `Kernel::Utils::Storage<T,128,2>`。

### 2.2 API 调用点（`src/` 内，排除 Horizon）

| API | 调用点 | entt 对应 |
| --- | --- | --- |
| `try_acquire_read` | 147 | `registry.try_get<C>(e)` |
| `try_acquire_write` | 16 | `registry.get<C>(e)` |
| `acquire_write` | 25 | 同上 |
| `acquire_read` | 12 | 同上 |
| `allocate` | 18 | `registry.create()` |
| `deallocate` | 34 | `registry.destroy(e)` |
| `try_acquire_read_nowait` | 2 | 同上 |
| 范围 for（`cbegin`/`cend`） | 3 | `registry.view<C>()` |
| **合计** | **~257** | |

### 2.3 句柄外泄面

| 项 | 数量 |
| --- | --- |
| `std::uintptr_t *_handle` 字段（`shared_data_hub.h`） | 68 |
| CEF 侧 `get_handle()` | 35 |
| EngineRuntime API 外层对象的 `handle_` | 各组件类各一个 |

---

## 3. 读写点分布（决定工作量）

### 3.1 读点（161 处，仅统计 `*_storage()` 上的读）

| 文件 | 数量 | 线程归属 |
| --- | --- | --- |
| `geometry_system.cpp` | 49 | DAG owner，安全 |
| **`engine_runtime_api.cpp`** | **31** | **外部线程（CEF/Script/Python），风险** |
| `optics_system.cpp` | 22 | DAG owner + 同线程读兄弟，安全 |
| **`editor_scene_service.cpp`**（原 `cef_editor_native_api_handlers.cpp`，已删除） | **10** | **CEF 线程，风险** |
| `mechanics_system.cpp` | 10 | DAG owner + 同线程读兄弟，安全 |
| **`camera_follow_controller.cpp`** | **6** | **Script 线程，风险** |
| 其余 30+ 文件 | 33 | 多数在 DAG 内部 |

**跨线程读 = 47 处**（31 + 10 + 6），加上零散点量级在 50 左右。
这是全量替换的**真正阻塞点**。

> 注：`src/` 里还有一批 `.acquire_read(...)` 是调在**别的对象**上的
> （例如 `HardwareImage::acquire_write/acquire_read`、ResourceManager 的句柄），
> 那些与 `Storage` 无关，不在此列。

现在的正确性靠"每槽一把 `shared_timed_mutex`"兜底。EnTT 完全没有锁，
所以这 47 处必须：

- **改快照**（设计上正确，但要为每个被读的组件设计快照结构），或
- **证明 owner 不会并发写**（例如只在加载期读），并加注释和断言。

### 3.2 写点（76 处）

| 文件 | 数量 | 性质 |
| --- | --- | --- |
| `geometry_system.cpp` | 18 | owner 自己写 |
| `optics_system.cpp` | 17 | owner 自己写 |
| `engine_runtime_api.cpp` | 13 | 构造期 + 外部拥有字段 |
| `mechanics_system.cpp` | 8 | owner |
| `vulkan_backend.cpp` | 6 | GPU 生产者 |
| CEF | 4 | `ik_chains`（外部拥有） |
| 其余 | 10 | owner |

写侧在上一轮命令通道改造后**已经收敛**：值路径的跨线程写为 0。
这也是为什么现在做 EnTT 比以前容易。

---

## 4. 映射方案

| 现结构 | entt 组件 | 备注 |
| --- | --- | --- |
| `Storage<ModelResource,128,2>` | `ModelResource` | 1:1 |
| `Storage<ModelTransform,128,2>` | `ModelTransform` | 1:1 |
| `Storage<GeometryDevice,128,2>` | `GeometryCore` + `ModelNodes` | 建议拆：后者大且低频 |
| `Storage<MechanicsDevice,128,2>` | `MechanicsDevice` | 1:1 |
| `Storage<AcousticsDevice,128,2>` | `AcousticsDevice` | 1:1 |
| `Storage<OpticsDevice,128,2>` | `OpticsDevice` | 1:1 |
| `Storage<ProfileDevice,128,2>` | `ProfileDevice` | 1:1 |
| `Storage<ActorDevice,128,2>` | `ActorIdentity` + `ActorNetworkState` | 建议拆：网络只关心后者 |
| `Storage<CameraDevice,128,2>` | `CameraDevice` | 1:1 |
| `Storage<ActorPickDevice,128,2>` | `ActorPickDevice` | 1:1 |
| `Storage<EnvironmentDevice,128,2>` | `EnvironmentLighting` + `EnvironmentPhysics` | 建议拆：更新频率差一个量级 |
| `Storage<SceneDevice,128,2>` | `SceneDevice` | 1:1 |
| `Storage<ImageDevice,128,2>` | `ImageDevice` | 生产者是 UI |

---

## 5. 三个工作量来源（都不是"换个容器"）

### W1 · 句柄类型（机械，但面广）

`ObjectId = std::uintptr_t`（槽位地址）→ `entt::entity`（32 位 index+version）。

- 68 个 `*_handle` 字段、35 处 `get_handle()`、各 API 类的 `handle_`
- 句柄在 JSON 回包里的表示（CEF 当整数传）
- 句柄在存档里的表示

**副作用是好的**：现在句柄是地址，重启即失效、无 ABA 保护；换 entt 后 index+version
天然解决这两点。

### W2 · 引用稳定性（最容易被低估）

现在 `WriteHandle`/`ReadHandle` 本质是 `T*`，可以跨语句持有：

```cpp
if (auto w = storage.try_acquire_write(h)) {
    w->a = 1;
    w->b = w->a + 1;   // 安全：同一个 T*
}
```

EnTT 的 `registry.destroy()` 是 swap-and-pop，**会移动最后一个元素到被删位置**，
所以任何跨语句持有的 `T&` 都可能悬空。

对策：禁止跨语句持有组件引用（改成每次 `registry.get<T>(e)`），编译器不会帮你抓，
只能靠 review + 单测。

### W3 · 无锁读（真正的设计工作，47 处）

见 §3.1。这部分要么补快照，要么逐个论证。

---

## 6. 依赖问题

`build_tools/conanfile.py` 的 27 个依赖里**没有 entt**，本机 conan 缓存里也没有：

```
ls ~/.conan2/p | grep -i entt   →  空
```

仓库也没有 vendored 第三方目录（`third_party/` 不存在），策略是**全部走 conan**。

所以第一步需要：

```python
self.requires("entt/3.15.0", transitive_headers=True)
```

然后重新 `conan install` —— **这一步需要网络**。当前你的代理（`127.0.0.1:7897`）是挂的，
我上次是靠直连绕过代理完成的 git 操作；conan 大概率也能直连，但需要实测。

---

## 7. 分批计划

| 步骤 | 内容 | 改动量 | 可回滚 |
| --- | --- | --- | --- |
| **S0** | 加 entt 依赖 + `conan install` | 1 行 + 依赖锁定 | 是 |
| **S1** | 引入 `EntityId` 别名与转换层，`uintptr_t` 句柄先收口（**不换容器**） | ~100 处机械 | 是（纯重命名） |
| **S2** | 建 `entt::registry`，13 个组件注册，但**仍走 Storage**（并行期） | 新增文件 | 是 |
| **S3** | 按组件逐个切换：`allocate/deallocate` → `create/destroy`，读写 → `get/try_get` | ~257 处 | 单组件可回滚 |
| **S4** | 处理 47 处跨线程读：补快照或论证 | ~47 处（设计） | 视方案 |
| **S5** | 删掉 `Storage` 用法与 13 个 typedef | 清理 | — |

**建议顺序调整**：先做 S4 的设计（决定哪些读要快照），再做 S3。
否则 S3 做完会卡在无锁读上，等于把风险堆到最后。

---

## 8. 风险与验收

**风险排序**

1. 跨线程读失去锁保护（47 处）→ 数据竞争，且是**概率性崩溃**，难复现
2. 跨语句持有组件引用（W2）→ 悬空引用
3. 句柄迁移期双 ID 并存 → 漏转时表现为"查不到实体"而非崩溃
4. 存档/协议里的句柄表示变化
5. 单实体访问性能：`registry.get` 比 `T*` 多一层间接，当前 171 处读以单实体为主

**验收**

- 编译通过（`ninja -k 0`，唯一允许失败的是既有的 `corona_ui_surface_removal_race_tests`）
- 现有测试全绿（19 个核心用例）
- 13 个 typedef、257 处调用点、47 处跨线程读全部处理完毕
- 编辑器实测：模型加载、逐节点编辑、多人协作、场景切换

---

## 9. 值得做吗（诚实判断）

**收益**：跨组件查询能力（`view<A,B>`）、无锁、ABA 安全、句柄可持久化。

**代价**：257 处机械改动 + 55 处跨线程读的设计工作 + 一个尚未验证的新依赖。

**我的判断**：
- 如果目标是"架构更清晰、以后要加大量按组件组合的查询" → 值得
- 如果目标是"性能更好" → 大概率失望（单实体访问变慢）
- 如果只是想"去掉 Storage 的锁开销" → 有更小的方案：
  先只做 S1（句柄收口）+ S4（跨线程读改快照），把锁从热路径挪走，
  容器本身可以晚点换

最后一条值得单独强调：**命令通道 + 快照改造已经在做同样的事**
（把跨线程访问挪到通道上）。如果 S4 做完，Storage 的锁其实已经很少被跨线程命中，
那时换不换 entt 就是个纯收益/成本权衡了。
