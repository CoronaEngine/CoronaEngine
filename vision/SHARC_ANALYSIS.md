# SHARC 移除记录与验证

日期：2026-10-31。移除前基线：`781907e59b6f6449082e5801c7e3a2d4f45b22b6`。

SHARC 实现及仅为它服务的 RadianceCache 抽象、描述符和构建依赖已完整移除。ReSTIR 的采样、时空复用、StablePlanes、合成和降噪算法没有改动。引擎及选定测试目标构建成功，完整 ReSTIR GPU 回归通过；保存的 60 份光照缓冲区与删除前逐字节一致。

初次移除后运行 12 个不同的测试项：9 项通过，3 项 ReLAX 专用测试被 CTest 归类为 SEGFAULT。后者使用删除前保存的完整二进制也全部复现。初次验证汇总保存在 [verification.json](E:/CoronaEngine/build/sharc-removal/verification.json)。后续已定位并修复其延迟编译上下文问题：**18 项回归全部通过，0 失败、0 跳过**，ReSTIR 采样输出仍与移除前逐字节一致。最新汇总见 [ReLAX 修复验证](E:/CoronaEngine/build/sharc-removal/relax-fix-verification.json)。

## 原本用途与移除原因

SHARC 将光照估计缓存在世界空间，通过 Update、Resolve、Query 三阶段复用结果，并在合适的间接反弹处提前结束后续追踪，以减少路径追踪工作。[NVIDIA 集成说明](https://github.com/NVIDIA-RTX/SHARC/blob/main/docs/Integration.md)

移除前，ReSTIR 会强制构造 SHARC 插件，但仅用于 integrator 绑定、热更新和旧 UI；没有准备、更新或查询缓存的调用。插件的 update、resolve、compaction、query 四个执行方法也都是空实现。头文件中的哈希网格和缓存算法从未接入实际渲染输出。

四块缓存原本合计 176 MiB，但此前已经停止分配；本次移除不重复计入显存优化收益。已有优化历史保留在 [ReBLUR 记录](E:/CoronaEngine/vision/src/render_core/denoiser/ReBLUR/README.md:564)。

## 已完成的修改

| 范围 | 修改 |
| --- | --- |
| [ReSTIR integrator](E:/CoronaEngine/vision/src/render_core/integrator/rt.cpp:80) | 删除缓存 include、成员、构造、两处 integrator 绑定、热更新和 UI 引用；清掉随缓存遗留的无用局部变量 |
| [描述符定义](E:/CoronaEngine/vision/src/base/import/node_desc.h:384) | 删除 RadianceCacheDesc 和 IntegratorDesc::cache_desc |
| [描述符解析](E:/CoronaEngine/vision/src/base/import/node_desc.cpp:98) | 删除 param.cache 的专用解析及默认 SHARC 工厂选择 |
| [插件集合](E:/CoronaEngine/vision/src/render_core/CMakeLists.txt:20) | 删除 radiancecache 子目录与接口目标依赖；取消注册、构建和自动部署该插件 |
| [积分器构建](E:/CoronaEngine/vision/src/render_core/integrator/CMakeLists.txt:1) | 删除旧的 SHARC 子目录注释 |
| [构建回归检查](E:/CoronaEngine/tests/integration/test_vision_required.cmake:11) | 将 vision-radiancecache 和 vision-radiancecache-sharc 加入禁止注册的旧目标列表 |
| [资源回归断言](E:/CoronaEngine/tests/integration/test_vision_restir_estimators.cpp:345) | 保留无 SHARC 缓冲区分配检查，标明该缓存已移除 |

整组删除 8 个文件，共 965 行：

- `vision/src/base/integral/radiance_cache.h`
- `vision/src/base/integral/radiance_cache.cpp`
- `vision/src/render_core/radiancecache/CMakeLists.txt`
- `vision/src/render_core/radiancecache/sharc/CMakeLists.txt`
- `vision/src/render_core/radiancecache/sharc/constant.h`
- `vision/src/render_core/radiancecache/sharc/hash_grid.h`
- `vision/src/render_core/radiancecache/sharc/sharc.h`
- `vision/src/render_core/radiancecache/sharc/sharc.cpp`

这些文件可通过基线提交和 Git 历史找回。当前插件集合剩余 49 个目标、18 个类别。

## 验证条件

复用 `build/conan/examples/relwithdebinfo`，通过 `BUILD_TESTING=ON` 在现有 examples 配置中启用根测试。使用仓库 Conan/MSVC 环境、RelWithDebInfo 配置和 Vulkan 后端；GPU 基线实际选择 NVIDIA GeForce RTX 2080。Native NRD 选项保持关闭。

删除前先编译测试目标并采集 GPU 基线。原始输出使用现有 `VISION_EVAL_DEBUG_DIR` 和 `VISION_EVAL_DEBUG_FRAMES=0,1,7` 保存；同一输出名会被后续夹具覆盖，因此它们是测试结束时保留的采样输出，不代表所有场景和所有帧的完整录像。全部夹具的行为覆盖仍由回归断言提供。

`rt.cpp` 从 `compile()` 到文件结尾与基线逐字一致，ReSTIR 算法目录没有差异。证据见 [算法一致性检查](E:/CoronaEngine/build/sharc-removal/algorithm-equivalence.json)。这项源码检查不能替代运行验证。

## 初次移除的构建与回归结果

| 验证项 | 结果 | 证据 |
| --- | --- | --- |
| 基线构建 | 通过；使用删除前源码生成测试程序 | [构建记录](E:/CoronaEngine/build/sharc-removal/baseline-build.json) |
| 删除后构建 | 通过；corona_engine、全部 Vision 插件依赖及 5 个选定测试目标 | [构建记录](E:/CoronaEngine/build/sharc-removal/removed-build.json) |
| 构建图 | SHARC 目标为 0；插件 49 个、18 类 | [目标检查](E:/CoronaEngine/build/sharc-removal/target-graph.json) |
| 运行产物 | 已清理 6 个旧产物；活动构建树中没有 SHARC DLL | [清理清单](E:/CoronaEngine/build/sharc-removal/removed-stale-artifacts.json) |
| 完整 ReSTIR GPU 回归 | 基线及删除后均通过 | [基线日志](E:/CoronaEngine/build/sharc-removal/baseline-restir.log)、[删除后日志](E:/CoronaEngine/build/sharc-removal/removed-restir.log) |
| 输出比较 | 60 份缓冲区、61,440 个浮点值全部有限且逐字节一致，最大绝对差为 0 | [逐文件 SHA256 和数值比较](E:/CoronaEngine/build/sharc-removal/frame-comparison.json) |
| 测试诊断值 | 41 行诊断输出按日志显示精度逐行一致；不包含环境变量和临时路径 | [诊断值比较](E:/CoronaEngine/build/sharc-removal/estimator-metrics-comparison.json) |
| 补充兼容性 | 8 项通过，3 项 ReLAX 异常终止（CTest: SEGFAULT） | [CTest 日志](E:/CoronaEngine/build/sharc-removal/removed-compat.log)、[JUnit 结果](E:/CoronaEngine/build/sharc-removal/removed-compat.xml) |
| ReLAX 基线对照 | 删除前二进制同样复现全部 3 项异常终止 | [对照日志](E:/CoronaEngine/build/sharc-removal/baseline-relax.log)、[二进制完整性与加载路径](E:/CoronaEngine/build/sharc-removal/baseline-relax-provenance.json) |

完整 ReSTIR 测试覆盖 DI/GI 能量分离、时间与空间复用、采样支持域、材质引导、玻璃传输、镜面、StablePlanes、SVGF 分层合成与历史、相机隔离和运行时开关。输出比较包含原始、过滤后、各层及合成阶段的 DI/GI 缓冲区；比较范围受上述采样帧和覆盖规则约束。

基线 GPU 测试耗时 874.13 秒，删除后为 314.81 秒。两轮着色器缓存状态不同，不能将耗时差解释为移除 SHARC 带来的性能提升。

补充测试明细：

| 测试 | 删除后 | 删除前失败对照 |
| --- | --- | --- |
| VisionRenderModeConfigTests | 通过 | — |
| VisionReblurTests | 通过 | — |
| VisionEngineSceneTests | 通过 | — |
| VisionCameraSettingsTests | 通过 | — |
| VisionSharedDisplayTests | 通过 | — |
| VisionReSTIRVisibilityMIS | 通过 | 通过 |
| VisionReSTIRPairwiseMIS | 通过 | 通过 |
| VisionReSTIRGISupport | 通过 | 通过 |
| VisionRelaxPT | SEGFAULT | SEGFAULT |
| VisionRelaxReSTIR | SEGFAULT | SEGFAULT |
| VisionRelaxLifecycle | SEGFAULT | SEGFAULT |

对照使用移除前保存的 EXE/DLL 快照，83 个文件的 SHA256 均与原始清单一致；日志中 19 个动态模块路径均来自该快照，包括旧 SHARC 插件。对照与删除后运行共享外部 Conan 依赖和着色器缓存。快照独立保存在 `build/sharc-removal/baseline-runtime`，不属于活动运行目录。

以上是后续 ReLAX 修复前的结果，保留失败记录及对照证据。Native NRD、其他 GPU/后端、Debug 配置和 GUI 人工检查未纳入本次验证。

## 后续：ReLAX 延迟编译崩溃修复

三项原生调用栈均为 `ReLAX::dispatch → ReLAX::compile → Material::create_evaluator → SheenLTC::fetch_ltc → std::bad_variant_access → terminate`。CTest 将其归类为 SEGFAULT；原生栈表明主机端构造着色器时的异常穿过 `noexcept`，导致进程终止，异常码为 `0xC0000409`。完整证据见 [三项原生调用栈](E:/CoronaEngine/build/sharc-removal/relax-crash-stacks.json)。

正常 `FixedRenderPipeline::compile()` 在场景 GPU 上下文中编译。测试先执行 raw 帧，再在运行中开启 ReLAX；`dispatch()` 会延迟调用 `compile()`，此时没有外层 `Global::SceneGpuContextScope`。`MaterialLut` 按场景 bindless array 查找纹理，缺少上下文使该查找失效，随后在 Principled 材质的 sheen LUT 读取处终止。

修复在 [ReLAX::compile](E:/CoronaEngine/vision/src/render_core/denoiser/ReLAX/relax.cpp:144) 内部建立当前场景的上下文，并通过 RAII 恢复调用者状态；普通编译和延迟编译入口均受保护。运行代码只新增这 4 行，滤波公式、参数、历史处理和 ReSTIR 算法均未改动。

[现有场景回归](E:/CoronaEngine/tests/integration/test_vision_relax_producers.cpp:292) 增加三类前提/后置检查：启动时资源尚未准备、运行时开启发生在场景编译作用域外、每帧结束后上下文恢复。原有能量、有限颜色、真实插件启用、缩放及层切换断言全部保留。

- 引擎及 6 个选定测试目标构建成功：[构建记录](E:/CoronaEngine/build/sharc-removal/relax-fix-build.json)。
- `VisionRelaxPT`、`VisionRelaxReSTIR`、`VisionRelaxLifecycle` 全部通过：[JUnit 结果](E:/CoronaEngine/build/sharc-removal/relax-fix-targeted.xml)。
- 完整 ReSTIR GPU 回归通过：[JUnit 结果](E:/CoronaEngine/build/sharc-removal/removed-restir-relaxfix.xml)。与移除前基线相比，60 份缓冲区、61,440 个浮点值全部有限且逐字节一致，最大绝对差为 0：[输出对照](E:/CoronaEngine/build/sharc-removal/relax-fix-frame-comparison.json)。
- 其余 14 项兼容性回归全部通过，包含 ReLAX 的 6 个滤波阶段测试，以及 ReBLUR、引擎场景、相机、共享显示、模式配置和 MIS 检查：[JUnit 结果](E:/CoronaEngine/build/sharc-removal/relax-fix-compat.xml)。
- 41 行 ReSTIR 数值诊断按日志显示精度逐行一致：[诊断值对照](E:/CoronaEngine/build/sharc-removal/relax-fix-metrics-comparison.json)。

合计 18 项测试全部通过，没有跳过或放宽原有断言；引擎部署目录与测试运行目录中的 ReLAX 插件 SHA256 一致。最新构建命令、测试明细、输出范围和限制统一保存在 [修复验证汇总](E:/CoronaEngine/build/sharc-removal/relax-fix-verification.json)。本轮使用 RTX 2080/Vulkan/RelWithDebInfo；采样输出的覆盖限制与上文一致，未重复高分辨率质量/性能基准，也未验证其他后端。

## 兼容性

IntegratorDesc 的 C++ 布局发生变化，需要同步重建使用它的模块，不能混用新旧 DLL。旧 JSON 中的 param.cache 不再被专用接口消费；它原先也不影响本仓库的光照结果。仓库外若有 RadianceCache 派生插件，需要独立迁移。

当前保留的 SHARC 文本仅用于审计历史和防止重新引入的测试，不是运行依赖。未来若重新开发世界空间辐射缓存，应作为完整功能重新接入更新、解析、查询、资源同步和相机/场景生命周期，而非恢复空插件入口。
