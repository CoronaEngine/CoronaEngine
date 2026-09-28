# Vision 路径规范化优化 Review 指南

本指南用于审查本次“加载/绑定时生成路径键”的实现。需求和实测结果见 [Kitchen CPU 场景同步优化方案](kitchen-scene-sync-optimization.md) 第 3、7 节。

## 1. 先确认审查范围

本次应实现：绑定来源路径在首次绑定、来源/基目录变化或显式重载时规范化；稳态逐物体同步直接比较已有键。原始路径继续承担项目持久化语义。

本次不包含 AABB 优化、PT/SVGF 调整或渲染参数变化。方案第 5 节包含两项优化的完整验收表，其中“静止物体 AABB 零重算”等条件属于后续工作，不能作为本次已经完成的结果。

在 GitHub 打开本次提交的 Files changed，或在仓库根目录执行以下命令。将 `<本次提交SHA>` 替换为提交结果中的 SHA，避免以后 `HEAD` 移动导致审查范围扩大。

```powershell
git show --stat <本次提交SHA>
git show --format=fuller <本次提交SHA>
git diff <本次提交SHA>^ <本次提交SHA> -- src/systems/optics/optics_system.cpp
```

优先寻找以下三类缺陷：来源改变后复用旧键导致串场景；相对路径随工作目录变化而错绑；无关字段修改重新访问文件系统。

## 2. 按数据流阅读代码

| 顺序 | 文件与符号 | 要回答的问题 |
|---|---|---|
| 1 | `include/corona/shared_data_hub.h`：`ExternalVisionBindingDevice` | 原始 `source_path` 与运行时 `source_base_dir`、`source_path_key` 是否分开？ |
| 2 | `include/corona/utils/scene_path_key.h`：`normalize_scene_path_key()` | 绑定与场景是否使用相同规则？相对路径是否有确定的绝对基目录？失败回退是否仍保持绝对身份？ |
| 3 | `src/shared_data_hub.cpp`：`set_external_vision_binding()` | 何时复用、何时重建？是否忽略调用方复制来的旧键？解析是否在元数据锁外？ |
| 4 | 同文件：`refresh_external_vision_binding_paths()` | 解析期间绑定被修改或移除时，是否避免覆盖新来源或复活旧绑定？刷新是否只写回派生键、保留无关字段？ |
| 5 | `include/corona/engine/engine_runtime_api.h`、`src/engine/engine_runtime_api.cpp`、`src/systems/script/python/engine_bindings.cpp` | C++ 和 Python 的参数顺序、默认值、基目录传递是否一致？ |
| 6 | `src/systems/ui/cef/cef_editor_native_api_handlers.cpp`：`register_embedded_vision_actor_binding()` | 编辑器是否传入当前项目根目录？重载是否清除并重建绑定？ |
| 7 | `src/systems/optics/optics_system.cpp`：加载事件回调、两个 `load_external_vision_scene*()` | 延迟加载前是否固定路径上下文？显式重载是否刷新保留的绑定？ |
| 8 | 同文件：`has_external_live_bindings_for_scene()`、`sync_external_live_vision_transforms()` | 逐绑定比较是否只读 `source_path_key`？有没有退回逐帧补算？AABB 和渲染逻辑是否保持原样？ |
| 9 | `src/systems/network/tests/test_network_protocol.cpp`：`test_external_vision_binding_path_identity()` | 测试是否覆盖身份改变、复用和拒绝无效输入，而非只检查字段存在？ |

可用以下搜索核实所有入口与消费位置：

```powershell
rg -n 'set_external_vision_binding|refresh_external_vision_binding_paths' include src
rg -n 'normalize_scene_path_key|source_path_key|source_base_dir' include src
rg -n 'external_vision_bindings_' include src
```

搜索结果应表明：绑定表由 Hub 管理；编辑器可见性更新和 Optics 形状索引回写仍经过统一入口。关注规范化调用所在的循环，不能只凭函数调用总数判断是否存在逐物体热点。场景目标键的计算与绑定来源的逐物体计算应分别核查。

### 需要确认的 API 行为

- 绝对来源路径的既有调用可保持原样。
- 相对来源路径现在要求传入绝对 `source_base_dir`。这是有意的兼容性变化；例如 `source_path="scenes/kitchen.json"` 应配合 `source_base_dir="D:/projects/demo"`。
- 相对来源缺少基目录、使用相对基目录或盘符相对形式（如 `D:scene.json`）会抛出 `invalid_argument`，旧绑定保持不变。
- 文件加载 API 的旧相对路径在接收事件时按当时工作目录固定；嵌入场景的相对身份按资源基目录解释。两者与 Actor 绑定的显式基目录要求有所区别。
- Windows 键沿用 ASCII 大小写不敏感约定，并保留 UTF-8 字节；这不是完整的 Unicode 大小写折叠实现。
- 项目迁移应传入新项目基目录并重新绑定。运行中修改目录链接后，需要显式重载、Hub 强制刷新，或清除再绑定。

## 3. 复核正确性

先检查当前引擎进程和本地改动；运行场景测试前保存测试项目文件，结束后恢复。不要同时运行 CLion 构建和命令行构建。

引擎构建按 [CLion 构建技能](../../.agents/skills/clion-build-corona-engine/SKILL.md) 执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .agents/skills/clion-build-corona-engine/scripts/build.ps1
```

要求退出码为 0，且 `cmake-build-relwithdebinfo/examples/engine/corona_engine.exe` 存在。只有配置缺失、过期或确实变更时，才按项目 CMake 技能重配。

在 CLion 的相同工具链和 RelWithDebInfo 配置中构建现有 `corona_network_protocol_tests` target，然后从仓库根目录运行：

```powershell
& ./cmake-build-relwithdebinfo/src/systems/network/corona_network_protocol_tests.exe
if ($LASTEXITCODE -ne 0) { throw 'Network protocol regression failed' }
```

测试 target 依赖 `BUILD_CORONA_TESTING` 或 `BUILD_TESTING`。本次验证使用已有的 `BUILD_TESTING=ON` 配置，没有为本次改动重配 CMake。

| 用例 | 应观察到的结果 |
|---|---|
| 首次绑定时故意携带错误派生键 | Hub 生成正确键，不信任调用方的值 |
| 绝对路径与明确基目录下的相对路径、`..`、Windows 分隔符/大小写变体 | 等价路径得到同一个键；UTF-8 来源保持正确 |
| 不同来源或同一相对路径配不同项目基目录 | 键不同，不跨场景匹配 |
| 修改来源但携带旧键 | 重新生成键 |
| 仅修改可见性/形状索引，或改变进程工作目录 | 来源身份不变；真实解析计数不增加 |
| 缺少有效基目录的相对来源 | 抛出异常，旧绑定不被部分覆盖 |
| 清除重建、基目录迁移、显式刷新 | 生成当前上下文的键，保留可见性/形状索引 |
| 目录联接从 A 改向 B | 普通无关修改继续复用旧键；显式刷新后切换到 B |
| kitchen 隐藏/恢复、物体移动/还原、相机移动/还原 | 快照状态正确，场景继续出帧，绑定路径解析计数不增加 |

永久测试覆盖身份和字段行为；“确实没有访问文件系统”还需入口计数验证。目录联接及计数断言使用了本机临时测试补丁，未并入永久跨平台测试。实际运行的动作和结果见方案第 7 节，完整项目资产搬迁和逐像素图像比较尚未验证。

## 4. 复核性能证据

先看方案第 7 节的测试条件和表格。关键验收是**稳态绑定来源规范化 0 次/帧**，而不是仅编译成功、画面能出现或 FPS 上升。

已有实测：预热 20 秒后采样 25 秒，163 帧，299 个物体，实际 framebuffer 1536×825；绑定解析累计计数全程保持 598。598 来自加载阶段的初始化和刷新，不代表每帧调用数。同步均值 133.543 ms，新场景帧间隔均值 153.737 ms；AABB 仍是 299 次/帧、131.394 ms。

原始证据位于本机 `cmake-build-relwithdebinfo/kitchen-frame-profile/`：

| 文件 | 检查内容 |
|---|---|
| `path-optimized.summary.json` / `.frames.csv` | 每帧 `path_calls=0`、`path_ms=0`、`changed=false`；累计 `binding_path_total` 最小/最大都为 598 |
| `path-optimized.stdout.log` / `.window.json` | 汇总是否仅选取规定采样窗口，帧数与逐帧数据是否一致 |
| `path-optimized.snapshot.json`、`baseline2.snapshot.json` | 场景、相机是否一致；实际 framebuffer 尺寸还需对照渲染日志 |
| `path-optimized-instrumentation.patch` | 计数是否覆盖真实绑定初始化/刷新入口及所有线程，而不是打印常量零 |
| `path-count-tests.patch` / `.log` | 可见性、形状索引、强制刷新及目录联接的额外验证 |
| `path-regression.actions.json`、`path-camera.actions.json` | API 响应和动作后快照是否支持通过结论，失败请求是否被排除 |

这些文件位于构建目录，**不会随本次 Git 提交推送**。远端可查看已提交的方案与结果记录，但没有原始日志时，不能声称自己重新验证了性能统计。

需要重新采样时：

1. 保持同一场景、相机、尺寸、采样数和最大深度，确认当前 PT 实际启用 SVGF。
2. 在绑定 setter 的重建分支和显式刷新入口统计真实调用；记录全局累计值，同时计时同步、AABB 和新场景帧间隔。不能只计同步内部，遗漏其他线程或帧间解析。
3. 预热至少 20 秒、采样至少 25 秒，保存原始日志、窗口、状态快照、均值、P95 和样本数。本机既有 `capture.py` / `analyze.py` 及临时计时补丁可供参考，使用前检查其中的固定路径。
4. 另测物体及相机动作，验证状态确实改变且绑定解析计数不增加。编辑器相机动作使用 `scene.ini` 路由；遇到 API 错误必须检查快照，不能当作成功。
5. 撤回临时插桩、恢复测试项目、重新构建正式版本，再核对 diff。

旧整帧、旧同步细分和优化后数据来自不同轮次，不应组合计算精确阶段加速比。UI、Display 与 Optics 并行，耗时不能直接相加。约 131 ms 的 AABB 瓶颈仍在，不能将本次结果描述为“同步已降至数毫秒”。

## 5. 给出 Review 结论

建议按以下格式记录：

```text
审查提交：<SHA>
结论：通过 / 需要修改
已检查：路径身份、基目录、缓存失效、锁范围、加载/刷新入口、API 兼容性
已运行：<命令、退出码、场景动作>
性能证据：<自行复测 / 查看原始日志 / 仅阅读已提交结果>
问题：<文件、函数、触发条件、错误结果、建议修正>
未验证：<完整项目迁移、图像比较等实际未覆盖内容>
```

阻塞问题包括：不同来源得到错误的同一身份、来源改变后键不刷新、稳态重复解析、锁内执行文件系统操作、无效绑定被部分发布、项目保存写入派生绝对键。单独记录已有编辑器持久化问题和待实施的 AABB 优化，避免扩大本次修改范围。
