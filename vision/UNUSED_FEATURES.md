# Vision 未使用功能审计

审计日期：2026-10-31。代码基线：`781907e59b6f6449082e5801c7e3a2d4f45b22b6`。

后续状态：F02 双边上采样、F03 SHARC、F04 BVH 灯光采样器已从源码和构建依赖中移除；其余清单及数量保留上述审计基线口径。当前插件目标为 47 个、17 类。SHARC 的构建和回归结果见 [SHARC 移除记录](E:/CoronaEngine/vision/SHARC_ANALYSIS.md)。

Vision 的闲置部分主要来自独立渲染器向 Corona 集成时遗留的入口、未完成的实验功能，以及仍会构造但不再参与输出的对象。最值得优先处理的是 **SSAT 模式未接入引擎场景创建、空实现插件、无消费者的 Postprocessor 着色器、保存链路断开**。这些问题不能通过删除所有“非默认插件”解决。

本清单覆盖 `vision/` 全部 359 个已跟踪文件，包括 19 类、50 个插件目标；交叉核查 Corona 的 optics 适配层、显示路径、测试、场景夹具和构建配置。排除 `vision/src/ext/` 后共 69,350 行，这个数字包含源码、生成表、脚本和构建文件，不是有效代码行数。第三方 ImGui、GLAD、ImGuizmo 仅检查依赖，不做供应商内部死代码清理。

这里的“未使用”指**当前仓库调用链**。动态插件由 `Node::create_shared()` 根据 `desc.plugin_name()` 和 `desc.construct_name` 加载，公共 API 也可能被仓库外程序使用，因此不把静态无引用直接等同于可安全删除。没有进行完整构建、GPU 运行覆盖或链接器可达性证明；宏和 EDSL 也使普通 C++ 语法分析不足以证明每个函数的全局可达性。

## 判定标准

| 标记 | 含义 | 处理原则 |
| --- | --- | --- |
| 未完成 | 空实现、缺少构造导出，或声明的能力未实现 | 补全或明确禁用 |
| 未接入 | 有实现，但生产入口、执行入口或输出消费者缺失 | 先决定恢复还是移除 |
| 无仓库调用 | 仅有声明、定义或闲置子系统内部引用 | 可作为清理候选，核对公共 API 契约 |
| 仅测试使用 | 测试明确创建或调用，生产主流程不用 | 保留测试能力或移到测试构建 |
| 可选未选择 | 可通过描述符选择，当前代码和夹具未选择 | 不直接判定为废代码 |
| 条件关闭 | 明确构建开关或编译期条件控制 | 单独管理，不混入死代码 |

动态加载证据：[Node::create_shared](E:/CoronaEngine/vision/src/base/node.h:116)、[插件名生成](E:/CoronaEngine/vision/src/base/import/node_desc.h:138)。引擎入口证据：[默认描述符](E:/CoronaEngine/src/systems/optics/optics_system.cpp:1585)、[创建管线](E:/CoronaEngine/src/systems/optics/optics_system.cpp:1627)、[每相机上下文](E:/CoronaEngine/vision/src/base/mgr/pipeline.cpp:52)。

## 优先处理的功能

### F01 光场和 SSAT 没有接入当前引擎场景入口

**判定：生产入口未接入，算法实现不能删除。**

`make_default_vision_project_desc()` 默认生成 `fixed + normal + pt + svgf`。`create_vision_pipeline()` 仅在 ReSTIR 模式调用 `configure_vision_scene_for_mode()`；SSAT 分支只是跳过一次普通 framebuffer 覆盖，没有将默认值改成 `lightfield` 和 `SSAT`。相机上下文沿用这份描述符，SSAT 又强制使用 `Scene` 降噪器选择。模式切换函数也只调整降噪开关，并明确警告“不改变 framebuffer 或 denoiser type”。

因此，当前引擎界面选择 SSAT 不代表执行光场 SSAT。Vision 内部 `PT → prepass_sampling_mask → spatial filter → temporal accumulator → lightfield encode` 的代码确实存在，配置单元测试也存在，但它们不能证明引擎入口接通。

证据：[管线创建](E:/CoronaEngine/src/systems/optics/optics_system.cpp:1630)、[相机选择](E:/CoronaEngine/src/systems/optics/optics_system.cpp:2261)、[模式切换限制](E:/CoronaEngine/src/systems/optics/optics_system.cpp:6429)、[配置测试](E:/CoronaEngine/tests/systems/optics/test_vision_render_mode_config.cpp:299)、[PT 内部调用](E:/CoronaEngine/vision/src/render_core/integrator/pt.cpp:306)。

建议：若保留光场产品能力，应在引擎场景创建时应用 SSAT 描述符，并增加穿过真实引擎入口的 framebuffer 类型和降噪器类型验证；在此之前明确标识模式限制。

### F02 双边上采样 已移除

**状态：已删除插件、抽象接口、默认构造和配置解析。**

移除前每个 `FrameBuffer` 都创建默认 `bilateral` 插件，但其 kernel 为空、`apply()` 返回空命令，渲染链没有调用其 `compile/apply`。

现已移除 `Upsampler`、`UpsamplingParam`、`UpsamplerDesc`、Framebuffer 持有关系、getter、热更新与 GUI 状态引用、ReSTIR 的无用 include，以及薄透镜测试中的旧配置。构建不再生成 `vision-upsampler-bilateral`。

`FrameBuffer` 和 `FrameBufferDesc` 的 C++ 布局发生变化，使用它们的 DLL 必须一起重建。旧 JSON 的 `param.upsampler` 不再由专用接口解析，也不会触发插件加载；其他降噪器内部的双边滤波和上采样逻辑不属于本次移除范围。

### F03 SHARC 辐射缓存 已移除

**状态：已删除实现、抽象接口、配置解析和生产依赖。**

移除前 `rt` 会构造默认 `sharc` 缓存，但只绑定 integrator、参与热更新和旧 UI，没有准备或执行缓存的调用。插件的 `update/resolve/compaction/query` 本身也为空。

现已移除 SHARC 目录及 `RadianceCache` 基类，清理 ReSTIR 持有关系、`RadianceCacheDesc`、`IntegratorDesc::cache_desc` 和 CMake 目标。ReSTIR 的算法目录和 `rt.cpp` 的编译、渲染函数保持不变。

此前四块缓冲区合计 176 MiB 的分配已取消，因此本次移除不重复计入显存优化收益。具体修改和回归记录见 [SHARC 移除记录](E:/CoronaEngine/vision/SHARC_ANALYSIS.md)。

### F04 BVH 灯光采样器 已移除

**状态：已删除占位实现和插件构建目标。**

移除前 `LightBVHNode` 和 `build_bvh()` 为空，`bvh.cpp` 也没有插件构造导出，但 CMake 仍生成 `vision-lightsampler-bvh`。当前已删除该文件及目标；引擎使用的 `uniform` 和测试使用的 `power` 均保留。

剩余构建目标见 [lightsampler/CMakeLists.txt](E:/CoronaEngine/vision/src/render_core/lightsampler/CMakeLists.txt)。相关的 `Light::bound()` 也只返回空值且没有调用者，本次未改动这一公共接口。

根集成构建检查已将两个插件及上采样分类目标列为 retired target，防止重新引入占位构建依赖。

F02/F04 初次移除验证：`examples-relwithdebinfo` 引擎及三个相关测试目标构建成功；47 个保留插件在构建运行目录与引擎部署目录中的 SHA256 一致，两个旧插件 DLL 均已清除。五项 CTest 中，模式配置、引擎场景（含 PT/ReSTIR）、相机设置和共享显示四项通过；`VisionWorldBoundsGpuTests` 失败。该测试此前六次半径/概率检查通过，最终 `clear_shapes()` 后灯光表仍报告两个灯，PMF 为 0.2，预期为 1。对应场景清理、几何更新、power 采样器和测试代码在初次移除时均未修改；未进行移除前二进制对照，不将其定性为已复现的既有失败。初次失败记录保留在 [验证汇总](E:/CoronaEngine/build/plugin-removal/verification.json)、[测试日志](E:/CoronaEngine/build/plugin-removal/tests.log)和 [JUnit 结果](E:/CoronaEngine/build/plugin-removal/tests.xml)。

后续回归已复现并修复 WorldBounds 失败：`Pipeline::rebuild_geometry_gpu()` 原先没有同步变更后的实例与灯光表，清空几何后仍有孤立面积光参与功率采样。现在重建入口先激活上下文、等待旧提交完成，再调用 `Scene::fill_instances()` 重绑定存活发光实例并清除孤立面积光，最后上传和重建几何。原有概率断言保留，新增单视图和双视图清空后的灯光数量与 GPU 边界检查；方向光和环境光继续保留。

当前配置下的 28 项 Vision/LightField 测试最新结果全部通过，包括完整 ReSTIR 估计器、ReBLUR、ReLAX、引擎场景、共享显示和资源生命周期。整轮为 27 项通过、1 项因性能分析 Python 启动器依赖的 EXE 漏构建而启动失败；补建 `corona_switch_profile_scope_tests` 后该项重跑通过，没有修改其逻辑或断言。引擎已重建部署，`vision-base.dll` 和 47 个插件的构建/部署哈希一致。验证环境为 RTX 2080/Vulkan/RelWithDebInfo，Native NRD 未启用。详见 [后续验证汇总](E:/CoronaEngine/build/plugin-removal/regression-verification.json)、[整轮结果](E:/CoronaEngine/build/plugin-removal/regression-tests.xml)与 [补充重跑](E:/CoronaEngine/build/plugin-removal/profile-scope-tests.xml)。

### F05 真实镜头相机

**判定：插件可构造，真实镜头功能未实现。**

`RealisticCamera` 只有镜片数据结构和 `lens_element()`。静态 `lens_config` 没有装载入口，`simple_weighting_` 没有使用，没有覆盖 `generate_ray_in_local_space()`，实际继承的是基础针孔射线生成逻辑。

证据：[镜片数据和相机](E:/CoronaEngine/vision/src/render_core/sensor/realistic.cpp:23)、[基础射线生成](E:/CoronaEngine/vision/src/base/sensor/sensor.cpp:58)。

建议：标记为未实现或移入实验目标；不要以 `realistic` 名义向用户宣称真实镜头追迹。

### F06 PT 收敛自适应采样

**判定：存在参数和对象，但采样决策未使用。**

`ConvergenceInspector` 会创建，参数可以显示；`prepare()`、`reset()` 调用被注释，PT 不调用 `add_sample()` 和 `is_convergence()`。其 `settings()` 已明确返回 `available: false`。这与 **SSAT 的 AdaptiveSampler 是不同功能**；后者在光场 PT 内部有实际采样 mask 消费者。

证据：[PT prepare](E:/CoronaEngine/vision/src/render_core/integrator/pt.cpp:89)、[重置残留](E:/CoronaEngine/vision/src/render_core/integrator/pt.cpp:257)、[能力标记](E:/CoronaEngine/vision/src/render_core/integrator/adaptive/inspector.h:65)。

建议：保留明确的不可用状态，取消无效 UI，或恢复完整收敛判断；不能只打开参数开关。

### F07 PT 的旧 TAA

**判定：注释实现加遗留成员。**

`taa_history_` 仍创建 CPU 包装对象，`taa_shader_` 仍声明，但 history 分配、分辨率更新和整段 TAA kernel 编译代码均被注释，没有 dispatch。不要将其与正在使用的降噪时间累积或 progressive accumulation 混同。

证据：[遗留成员](E:/CoronaEngine/vision/src/render_core/integrator/pt.cpp:34)、[注释实现](E:/CoronaEngine/vision/src/render_core/integrator/pt.cpp:149)。

建议：清理成员和注释代码；若要保留实验记录，使用版本历史。

### F08 旧 Postprocessor 色调映射

**判定：着色器会编译，但没有执行消费者。**

fixed/customized 的 `init_postprocessor()` 调用 `set_tone_mapper()`，后者立即 `compile_tone_mapping()`；全仓库没有 `Postprocessor::tone_mapping()` 的调用。实际 framebuffer 渲染使用自己的 `tone_mapping_`，Corona 显示又通过零拷贝桥取得线性源并执行 `vision_resolve`。

证据：[初始化](E:/CoronaEngine/vision/src/pipelines/fixed/pipeline.cpp:38)、[触发编译](E:/CoronaEngine/vision/src/base/mgr/postprocessor.h:24)、[未调用的 dispatch 包装](E:/CoronaEngine/vision/src/base/mgr/postprocessor.cpp:26)、[显示源](E:/CoronaEngine/src/systems/optics/vision/vision_zero_copy_bridge.cpp:130)。

建议：优先去除这套孤立着色器编译及持有关系。这里能确认存在无消费者的编译工作，但没有测量耗时，不给出性能收益数值。

### F09 保存和离线输出入口

**判定：保存链路断开。**

`request_save()` 只设置 `need_save_`，`output.spp` 由 `check_and_save()` 检查，`save_exit` 在其下游 `save_result()` 中消费；`check_and_save()` 在仓库中只有声明和定义。`save_result()` 的唯一内部调用正是它。`display()` 没有检查保存请求，`offline_rendering()` 只调用 `invalidate()`。

证据：[完整保存链](E:/CoronaEngine/vision/src/base/mgr/pipeline.cpp:207)、[帧循环](E:/CoronaEngine/vision/src/base/mgr/pipeline.cpp:400)、[旧保存按钮](E:/CoronaEngine/vision/src/base/mgr/pipeline_ui.cpp:119)。

建议：如果保留 Vision 输出 API，补上明确的消费点；否则移除失效入口。不要删除 Corona 自身的截图、诊断导出或测试 readback，它们是其他路径。

### F10 IES 灯光分布导入

**判定：整组实现无仓库消费者。**

`IESFile` 只在 `ies.h/ies.cpp` 中出现，解析、B/C 类型处理、打包函数均没有外部调用；灯光插件和 Corona 适配层没有读取或应用 IES 的路径。`ies.cpp` 仍因 `vision-base` 的递归 glob 参与编译。

证据：[IESFile](E:/CoronaEngine/vision/src/base/illumination/ies.h:14)、[实现](E:/CoronaEngine/vision/src/base/illumination/ies.cpp:57)、[base glob](E:/CoronaEngine/vision/src/base/CMakeLists.txt:2)。

建议：可整体隔离，或通过具体灯光类型接入；目前不能声称支持 IES 配光。

### F11 金属光谱 IOR 表

**判定：孤立头文件和数据表。**

`metal_ior.h` 没有 include 消费者，`metal_ior.inl.h` 仅由它 include；`ComplexIorTable` 和 `ComplexIor` 没有材质消费者。表中的 Ag、Al、Au、Cu 等数据不参与当前 principled 材质计算。

证据：[孤立表入口](E:/CoronaEngine/vision/src/render_core/material/metal_ior.h:12)。

建议：作为整组清理候选。不要连带删除正在使用的 `precomputed_table.h`、`ltc_sheen_table.h`、`srgb2spec.h` 或 `math/complex.h`。

### F12 材质预计算工具入口

**判定：表生成入口未使用，生成结果仍在使用。**

`MaterialRegistry::precompute_albedo()` 没有调用者，当前构建也没有它注明的 `vision-precompute.exe` 目标。它下面的 `Material::precompute()`、principled 预计算和输出序列化构成一条失去外部入口的工具链。但现成的预计算表由 principled 和 lobe 代码使用。

证据：[生成入口](E:/CoronaEngine/vision/src/base/mgr/registries.cpp:144)、[principled 预计算](E:/CoronaEngine/vision/src/render_core/material/principled_bsdf.cpp:379)、[表消费者](E:/CoronaEngine/vision/src/render_core/material/principled_bsdf.cpp:12)。

建议：将生成器恢复为单独工具目标，或明确只保留已生成表；不要把表生成函数和表运行时消费者一起删除。

### F13 Customized 管线和 RenderGraph

**判定：可动态创建，但当前生产和测试没有执行这条管线。**

引擎固定使用 `fixed`，没有描述符选择 `customized`。`integrate/accumulate/tonemapping/gamma` 四个 pass 只由 customized 创建。它的最终 `render_graph_.output_buffer()` 写回 framebuffer 代码已被注释。`test_render_graph.cpp` 的 `main()` 只构造一个空图，不构建、不编译、不执行图。

证据：[定制管线](E:/CoronaEngine/vision/src/pipelines/customized/pipeline.cpp:27)、[输出断点](E:/CoronaEngine/vision/src/pipelines/customized/pipeline.cpp:48)、[空测试](E:/CoronaEngine/vision/src/tests/test_render_graph.cpp:81)。

建议：作为完整实验子系统单独管理。RenderGraph 没有生产消费者，不等于图算法已通过测试；也不能只删 pass DLL 而留下 customized 工厂入口。

### F14 Vision 独立窗口和旧参数面板

**判定：Corona 主流程不用；窗口仍有手动热更新测试消费者。**

生产链没有调用 `PipelineUI::render/render_perf_panel`，没有创建 Vision 的 GLFW/OpenGL 窗口，也没有连接 `Pipeline::on_touch()` 的旧选取入口。当前相机、UI、拾取和显示由 Corona 管理。旧面板下面的场景、framebuffer、统计、热更新和输出编辑器因此没有生产入口。

证据：[PipelineUI](E:/CoronaEngine/vision/src/base/mgr/pipeline_ui.cpp:35)、[旧拾取](E:/CoronaEngine/vision/src/base/mgr/pipeline.cpp:174)、[仍使用窗口的测试](E:/CoronaEngine/vision/src/tests/test_hotfix.cpp:53)、[window 依赖](E:/CoronaEngine/vision/src/base/CMakeLists.txt:8)。

`WindowLibrary::SDL` 也不是第二套窗口实现，它明确警告后回退 GLFW，见 [window.cpp](E:/CoronaEngine/vision/src/window/window.cpp:14)。

建议：将独立 viewer 和热更新演示拆为可选目标。不能直接删除 `vision-window` 或全部 GUI 基类，当前 `vision-base` 仍依赖它们，宏也会注册运行时类型。

### F15 CPU 调试可视化和 screen buffer 预览

**判定：输出消费者断开，部分资源和采集逻辑仍存在。**

`fill_window_buffer()` 没有有效调用，`FrameBuffer::after_render()` 中调用被注释。`Visualizer::draw()` 只能从这条链路触发，但 framebuffer 构造仍执行 `visualizer_->init()` 并分配三组列表。材质和积分器中还保留条件采集射线、法线、局部坐标架的逻辑。`cur_screen_buffer()` 和旧预览选择也没有消费者。

证据：[资源初始化](E:/CoronaEngine/vision/src/base/sensor/frame_buffer.cpp:25)、[列表分配](E:/CoronaEngine/vision/src/base/sensor/visualizer.cpp:24)、[CPU 下载和绘制](E:/CoronaEngine/vision/src/base/sensor/frame_buffer.cpp:363)、[断开的 after_render](E:/CoronaEngine/vision/src/base/sensor/frame_buffer.cpp:433)。

建议：将分配、采集、绘制一起置于明确调试能力后面；不要只移除绘制而留下资源初始化。现有 Corona diagnostic adapter 不属于该闲置链路。

### F16 Beckmann 微表面分布

**判定：实现存在，当前没有实例化消费者。**

`BeckmannMicrofacet` 的引用只在自己的声明和实现中。当前材质使用 GGX，没有选择 Beckmann 的描述符或材质工厂入口。底层共享公式不能一并删除，因为 GGX 仍使用 microfacet 基础代码。

证据：[Beckmann 类](E:/CoronaEngine/vision/src/base/scattering/microfacet.h:354)、[实现](E:/CoronaEngine/vision/src/base/scattering/microfacet.cpp:270)。

### F17 SVGF 未接入的辅助算法和旧配置

**判定：局部无调用，不是整个 SVGF 未使用。**

`AnisotropicUtils` 的掠射角、边缘方向、组合各向异性和 offset 变换没有外部消费者。`SVGFConfig::Anisotropic::kEnabled = true` 并不能启用它。`compute_full_geometry_weight()`、`should_skip()`、`is_instance_boundary()` 也没有调用。`Disocclusion` 和 `AdaptiveRadius` 两组配置没有消费者，另有单独闲置常量，列入机器清单。

证据：[辅助算法](E:/CoronaEngine/vision/src/render_core/denoiser/SVGF/utils.h:627)、[配置](E:/CoronaEngine/vision/src/render_core/denoiser/SVGF/svgf_config.h:101)。

`Poisson::kFeedbackEnabled/kFeedbackSpecular = false` 则属于**有执行分支但刻意在编译期关闭**，代码说明了历史反馈产生模糊和拖影的原因；它们与无消费者常量分开处理，见 [反馈分支](E:/CoronaEngine/vision/src/render_core/denoiser/SVGF/poisson.cpp:359)。

### F18 SSAT 的通用稀疏 gather 和辅助接口

**判定：SSAT 内部部分算法未使用。**

`SparseGather::gather()` 只有 `gather_channel()` 调用，而后者及 `gather_with_geometry()` 没有执行消费者。`phase_space_to_buffer_index()` 同样无调用。实际滤波器只使用该类的坐标转换部分，并实现自己的采样逻辑；不能删除整个 `SparseGather`。

此外，phase space 的 setter、offset、distance/helper，以及若干 luminance/depth/B-spline 辅助函数没有调用，详见接口清单。

证据：[通用 gather](E:/CoronaEngine/vision/src/render_core/denoiser/SSAT/sparse_gather.h:166)、[闲置入口](E:/CoronaEngine/vision/src/render_core/denoiser/SSAT/sparse_gather.h:254)、[仍使用的转换](E:/CoronaEngine/vision/src/render_core/denoiser/SSAT/spatial_angular_filter.h:141)。

### F19 旧 JSON 和 BSON 文件读取器

**判定：无仓库调用。**

`create_json_from_file()` 没有调用者，`remove_cxx_comment()` 仅被它使用。生产场景现在由 Corona 导入，测试通过内存描述符构造；这不影响仍被大量使用的 `ParameterSet`、`DataWrap`、`NodeDesc` 和材质图解析。

证据：[旧文件读取器](E:/CoronaEngine/vision/src/base/import/json_util.h:98)、[测试入口说明](E:/CoronaEngine/tests/support/vision_test_scene.h:3)。

### F20 解析或写入后不生效的配置

| 配置或字段 | 当前情况 | 证据 |
| --- | --- | --- |
| `Sampler::spp_` / `sample_per_pixel()` | `spp` 被解析、保存，但没有采样循环读取；不能用它控制当前每帧样本数 | [sampler.h](E:/CoronaEngine/vision/src/base/sampler.h:30) |
| `MediumsDesc::process` | JSON 会写入，`MediumRegistry::load_mediums()` 不读取；实际判定使用自身的 `process_mediums_ && !elements_.empty()` | [解析](E:/CoronaEngine/vision/src/base/import/project_desc.cpp:25)、[消费缺口](E:/CoronaEngine/vision/src/base/mgr/registries.cpp:189) |
| `RendererDesc::denoiser_desc` | 仅传给 `init_postprocessor()`，两个实现都忽略形参；有效降噪描述符在 `IntegratorDesc::denoiser_desc` | [fixed](E:/CoronaEngine/vision/src/pipelines/fixed/pipeline.cpp:38)、[积分器](E:/CoronaEngine/vision/src/base/integral/integrator.cpp:88) |
| `ProjectDesc::scene_path` | 测试有赋值，没有读取；测试还需单独 `Global::set_scene_path()` | [测试赋值](E:/CoronaEngine/tests/support/vision_test_scene.h:21) |
| `output.spp/save_exit/fn` 的自动输出行为 | 保存消费点缺失，见 F09 | [pipeline.cpp](E:/CoronaEngine/vision/src/base/mgr/pipeline.cpp:225) |

建议：区分“应该删的兼容字段”和“应该修的失效配置”。尤其 `mediums.process=false` 被忽略属于行为缺口，不宜仅靠删字段掩盖。

### F21 旧场景相机列表管理

**判定：公共接口无仓库调用。**

`SensorRegistry` 没有实例化；`Scene::add_sensor/remove_sensor/cur_sensor_index/set_cur_sensor_index` 没有调用者。场景仍需要默认 sensor，但当前多相机通过 `Pipeline::ViewContext` 和 `sensor_override_` 工作，不靠旧列表切换 API。

证据：[旧接口](E:/CoronaEngine/vision/src/base/mgr/scene.h:148)、[空闲 registry](E:/CoronaEngine/vision/src/base/mgr/registries.h:82)、[现有 view](E:/CoronaEngine/vision/src/base/mgr/pipeline.cpp:52)。

### F22 旧全局 ImagePool 路径

**判定：全局池入口无当前调用，场景池仍活跃。**

`Global::image_pool()` 指向 `ImagePool::instance()`，但当前引擎和 renderer 都使用 `Pipeline::image_pool() → SceneData::image_pool_`；旧单例仅剩入口和析构清理关系。`set_source_cache()` 也没有调用者，但 `register_source()` 确实会创建并使用 `source_cache_`，所以不能把 source cache 整体删除。

证据：[旧入口](E:/CoronaEngine/vision/src/base/mgr/global.h:47)、[场景池](E:/CoronaEngine/vision/src/base/mgr/scene.h:164)、[有效 source cache](E:/CoronaEngine/vision/src/base/mgr/image_pool.cpp:15)。

## 构建和工具遗留

| 项目 | 判定和证据 | 建议 |
| --- | --- | --- |
| `vision-test_hotfix` 链接 `vision-hotfix-all` | [tests CMake](E:/CoronaEngine/vision/src/tests/CMakeLists.txt:34) 使用旧目标名；当前 Horizon 定义的是 [horizon-hotfix-all](E:/CoronaEngine/Horizon/src/hotfix/CMakeLists.txt:33)。没有找到旧名 alias | 修正可选测试依赖；未实际构建，不把预期链接问题写成已复现 |
| 热更新测试请求 `vision-hotfix-test.dll` | [test_hotfix.cpp](E:/CoronaEngine/vision/src/tests/test_hotfix.cpp:73) 与 Horizon 的 `horizon-hotfix-test.dll` 注册名不一致 | 同步迁移名称 |
| Vision C++ 测试未注册 CTest | [tests CMake](E:/CoronaEngine/vision/src/tests/CMakeLists.txt:1) 只建可执行文件，没有 `add_test()`；默认 `VISION_BUILD_TESTS=OFF` | 区分手动测试和自动回归覆盖 |
| `test_svgf_poisson.cpp` | 未列入任一 CMake 源或测试命令，文件注明手动编译 | 补入 CPU 测试，不作为算法废代码删除 |
| 两个 `run_all_*tests.bat` | 仍定位 `vision/src/ocarina/src/tests/CMakeLists.txt`，该目录已不存在 | 改接当前 Horizon/根构建 |
| 独立构建脚本 | `build_clion_target.ps1`、`build_standalone.cmd`、`compile_tu.ps1`、`invoke_msvc_build.ps1`、`start_cmake_config.ps1`、`start_msvc_build.ps1` 默认指向旧 `D:\yzy\code\cpp\Vision` 及个人工具链 | 合并到根 `build_tools` 或重新参数化 |
| `dev_verify.py` / `run_tests.bat` | 推导 Vision 子目录为工程根，默认使用旧 `cmake-build-*` 布局；Vision 现在是根工程的子项目 | 适配集成后的构建入口 |
| `stats_line_num.py` | 仍统计 `src/ocarina/src` | 修正统计范围 |

`test_restir_visibility_mis.py`、`test_restir_pairwise_mis.py`、`test_restir_gi_support.py` 已由根 [integration CMake](E:/CoronaEngine/tests/integration/CMakeLists.txt:149) 注册，不能因 Vision 自己的 CMake 没提到它们而误删。图像检查脚本、timeout/watch 辅助脚本和格式工具可供人手动运行，仅凭没有源码调用不能判为未使用。

## 全部插件状态

共 50 个插件目标。主流程使用包含按材质、光源或降噪器选择而触发的路径，不意味着每帧都会实例化所有插件。

| 类别 | 插件 | 当前状态 | 证据与说明 |
| --- | --- | --- | --- |
| pipeline | `customized` | 未接入 | [CMake](E:/CoronaEngine/vision/src/pipelines/customized/CMakeLists.txt:2)；见 F13；图输出写回被注释 |
| pipeline | `fixed` | 主流程使用 | [CMake](E:/CoronaEngine/vision/src/pipelines/fixed/CMakeLists.txt:2)；引擎默认管线 |
| denoiser | `ReBLUR` | 主流程可选 | [CMake](E:/CoronaEngine/vision/src/render_core/denoiser/ReBLUR/CMakeLists.txt:4)；每相机上下文可选择；有集成测试 |
| denoiser | `ReLAX` | 主流程可选 | [CMake](E:/CoronaEngine/vision/src/render_core/denoiser/ReLAX/CMakeLists.txt:5)；每相机上下文可选择；有集成测试 |
| denoiser | `SSAT` | 引擎入口未接入 | [CMake](E:/CoronaEngine/vision/src/render_core/denoiser/SSAT/CMakeLists.txt:2)；内部算法调用链存在，配置测试不等于引擎运行覆盖；见 F01 |
| denoiser | `SVGF` | 主流程可选 | [CMake](E:/CoronaEngine/vision/src/render_core/denoiser/SVGF/CMakeLists.txt:2)；每相机上下文可选择；有集成测试 |
| filter | `box` | 仅测试显式选择 | [CMake](E:/CoronaEngine/vision/src/render_core/filter/CMakeLists.txt:2)；测试和夹具使用；主流程默认 gaussian |
| filter | `triangle` | 仅测试显式选择 | [CMake](E:/CoronaEngine/vision/src/render_core/filter/CMakeLists.txt:3)；测试和夹具使用；主流程默认 gaussian |
| filter | `gaussian` | 主流程使用 | [CMake](E:/CoronaEngine/vision/src/render_core/filter/CMakeLists.txt:4)；SensorDesc 默认值；不能删除 FittedCurveFilter 和 alias2d |
| filter | `sinc` | 可选未选择 | [CMake](E:/CoronaEngine/vision/src/render_core/filter/CMakeLists.txt:5)；当前仓库没有选择该滤波器的场景 |
| filter | `mitchell` | 可选未选择 | [CMake](E:/CoronaEngine/vision/src/render_core/filter/CMakeLists.txt:6)；当前仓库没有选择该滤波器的场景 |
| framebuffer | `normal` | 主流程使用 | [CMake](E:/CoronaEngine/vision/src/render_core/framebuffer/CMakeLists.txt:2)；普通每相机 framebuffer |
| framebuffer | `lightfield` | 引擎入口未接入 | [CMake](E:/CoronaEngine/vision/src/render_core/framebuffer/CMakeLists.txt:3)；见 F01；内部 PT/SSAT 支持存在 |
| integrator | `pt` | 主流程使用 | [CMake](E:/CoronaEngine/vision/src/render_core/integrator/CMakeLists.txt:7)；PT 和 ReSTIR |
| integrator | `rt` | 主流程使用 | [CMake](E:/CoronaEngine/vision/src/render_core/integrator/CMakeLists.txt:8)；PT 和 ReSTIR |
| light | `area` | 主流程使用 | [CMake](E:/CoronaEngine/vision/src/render_core/light/CMakeLists.txt:2)；发光网格、方向光和环境光适配器 |
| light | `directional` | 主流程使用 | [CMake](E:/CoronaEngine/vision/src/render_core/light/CMakeLists.txt:3)；发光网格、方向光和环境光适配器 |
| light | `spherical` | 主流程使用 | [CMake](E:/CoronaEngine/vision/src/render_core/light/CMakeLists.txt:4)；发光网格、方向光和环境光适配器 |
| lightsampler | `uniform` | 主流程使用 | [CMake](E:/CoronaEngine/vision/src/render_core/lightsampler/CMakeLists.txt:2)；引擎默认 uniform，env_separate=false |
| lightsampler | `power` | 仅测试显式选择 | [CMake](E:/CoronaEngine/vision/src/render_core/lightsampler/CMakeLists.txt:3)；world_bounds GPU 测试使用 |
| lightsampler | `bvh` | 已移除 | 占位源码及插件目标已删除；见 F04 |
| material | `black_body` | 主流程使用 | [CMake](E:/CoronaEngine/vision/src/render_core/material/CMakeLists.txt:2)；principled 和黑体回退都是真实消费者 |
| material | `principled_bsdf` | 主流程使用 | [CMake](E:/CoronaEngine/vision/src/render_core/material/CMakeLists.txt:3)；principled 和黑体回退都是真实消费者 |
| medium | `homogeneous` | 可选未接入生产场景 | [CMake](E:/CoronaEngine/vision/src/render_core/medium/CMakeLists.txt:2)；Corona 当前不填充 mediums_desc；仅保留通用描述符加载路径 |
| pass | `accumulate` | 未接入 | [CMake](E:/CoronaEngine/vision/src/render_core/pass/CMakeLists.txt:2)；由 customized 使用，当前无管线执行入口；见 F13 |
| pass | `integrate` | 未接入 | [CMake](E:/CoronaEngine/vision/src/render_core/pass/CMakeLists.txt:3)；由 customized 使用，当前无管线执行入口；见 F13 |
| pass | `tonemapping` | 未接入 | [CMake](E:/CoronaEngine/vision/src/render_core/pass/CMakeLists.txt:4)；由 customized 使用，当前无管线执行入口；见 F13 |
| pass | `gamma` | 未接入 | [CMake](E:/CoronaEngine/vision/src/render_core/pass/CMakeLists.txt:5)；由 customized 使用，当前无管线执行入口；见 F13 |
| radiancecache | `sharc` | 已移除 | [移除记录](E:/CoronaEngine/vision/SHARC_ANALYSIS.md)；此表保留审计时的 50 个目标，当前剩余 47 个 |
| sampler | `independent` | 主流程使用 | [CMake](E:/CoronaEngine/vision/src/render_core/sampler/CMakeLists.txt:2)；默认随机采样器；spp 参数另见 F20 |
| sensor | `thin_lens` | 主流程使用 | [CMake](E:/CoronaEngine/vision/src/render_core/sensor/CMakeLists.txt:2)；默认 SensorDesc 和 Corona 相机适配 |
| sensor | `pinhole` | 可选未选择 | [CMake](E:/CoronaEngine/vision/src/render_core/sensor/CMakeLists.txt:3)；生产使用 thin_lens；半径为零不等于加载 pinhole 插件 |
| sensor | `realistic` | 未完成 | [CMake](E:/CoronaEngine/vision/src/render_core/sensor/CMakeLists.txt:4)；继承基础射线生成，没有真实镜片追迹；见 F05 |
| shadernode | `constant` | 可选兼容路径 | [CMake](E:/CoronaEngine/vision/src/render_core/shadernode/CMakeLists.txt:2)；保留在部分 ShaderNodeDesc 默认值中；常规数值适配使用 number，不能仅按显式字符串选择断言全局死代码 |
| shadernode | `image` | 主流程使用 | [CMake](E:/CoronaEngine/vision/src/render_core/shadernode/CMakeLists.txt:3)；材质表达式适配器创建；converter 的部分工厂闲置 |
| shadernode | `mix` | 主流程使用 | [CMake](E:/CoronaEngine/vision/src/render_core/shadernode/CMakeLists.txt:4)；材质表达式适配器创建；converter 的部分工厂闲置 |
| shadernode | `number` | 主流程使用 | [CMake](E:/CoronaEngine/vision/src/render_core/shadernode/CMakeLists.txt:5)；材质表达式适配器创建；converter 的部分工厂闲置 |
| shadernode | `math` | 主流程使用 | [CMake](E:/CoronaEngine/vision/src/render_core/shadernode/CMakeLists.txt:6)；材质表达式适配器创建；converter 的部分工厂闲置 |
| shadernode | `spd` | 可选未选择 | [CMake](E:/CoronaEngine/vision/src/render_core/shadernode/CMakeLists.txt:7)；有动态工厂；当前适配层和夹具没有选择 |
| shadernode | `src_node` | 可选未选择 | [CMake](E:/CoronaEngine/vision/src/render_core/shadernode/CMakeLists.txt:8)；有动态工厂；当前适配层和夹具没有选择 |
| shadernode | `converter` | 主流程使用 | [CMake](E:/CoronaEngine/vision/src/render_core/shadernode/CMakeLists.txt:9)；材质表达式适配器创建；converter 的部分工厂闲置 |
| shape | `quad` | 间接依赖和测试使用 | [CMake](E:/CoronaEngine/vision/src/render_core/shape/CMakeLists.txt:2)；AreaLight::init_geometry 会创建 quad；测试也显式使用 |
| shape | `cube` | 可选未选择 | [CMake](E:/CoronaEngine/vision/src/render_core/shape/CMakeLists.txt:3)；引擎通过资源导入三角网格；编辑器创建 cube/sphere 并不证明用了 Vision shape 插件 |
| shape | `sphere` | 可选未选择 | [CMake](E:/CoronaEngine/vision/src/render_core/shape/CMakeLists.txt:4)；引擎通过资源导入三角网格；编辑器创建 cube/sphere 并不证明用了 Vision shape 插件 |
| spectrum | `hero` | 仅测试显式选择 | [CMake](E:/CoronaEngine/vision/src/render_core/spectrum/CMakeLists.txt:2)；ReSTIR estimator 测试选择 hero |
| spectrum | `srgb` | 主流程使用 | [CMake](E:/CoronaEngine/vision/src/render_core/spectrum/CMakeLists.txt:3)；引擎默认光谱模型 |
| tonemapper | `impl` | 主流程使用 | [CMake](E:/CoronaEngine/vision/src/render_core/tonemapper/CMakeLists.txt:2)；默认 linear；aces/reinhard 工厂未被当前 Vision 描述符选择 |
| upsampler | `bilateral` | 已移除 | 插件、抽象接口、配置和默认构造均已删除；见 F02 |
| warper | `alias` | 主流程间接使用 | [CMake](E:/CoronaEngine/vision/src/render_core/warper/CMakeLists.txt:2)；面积光、环境贴图和滤波器的概率分布采样依赖 |
| warper | `alias2d` | 主流程间接使用 | [CMake](E:/CoronaEngine/vision/src/render_core/warper/CMakeLists.txt:3)；面积光、环境贴图和滤波器的概率分布采样依赖 |

同一插件还可能导出多个构造器，不能只按 DLL 数量判断能力：

| 插件内部功能 | 状态 |
| --- | --- |
| `converter`: `normal_map`、`gamma` | Corona 材质与纹理表达式适配器实际创建 |
| `converter`: `fresnel`、`vector_mapping`、`combine_xyz`、`combine_color`、`separate_color`、`separate_xyz`、`clamp` | 有工厂导出，当前适配器和夹具没有选择；属于通用图可选功能 |
| `src_node`: `tex_coord`、`geometry`、`camera` | 有命名工厂，当前没有描述符选择；使用时需正确的 `construct_name` |
| `math`: `Multiply` | 表达式适配器实际使用 |
| `math`: `Add`、`Subtract`、`Divide` | 分支已实现，当前适配器未产生这些操作；不要因此删除整个 math 插件 |
| `tonemapper/impl`: `linear` | Vision 默认工厂 |
| `tonemapper/impl`: `aces`、`reinhard` | Vision 描述符当前未选择；Corona 的 `vision_resolve` 自己执行 ACES，并不调用这里的 `aces` 工厂 |

## 接口级清单

以下 127 项按方法名或类型名计，不把每个重载重复计数；包含前面功能的入口、无调用兼容包装和未接入算法的内部依赖。它们是当前源码清理候选，不是自动删除名单。公共虚函数和模板应另核对 ABI、宏生成调用及仓库外扩展。

| 区域 | 无调用或仅在闲置链内调用的符号 | 位置 |
| --- | --- | --- |
| 描述符与旧解析 | `fill_id`、`set_type` | [node_desc.h](E:/CoronaEngine/vision/src/base/import/node_desc.h:28) |
| 描述符与旧解析 | `set_value_if_null` | [parameter_set.h](E:/CoronaEngine/vision/src/base/import/parameter_set.h:175) |
| Spectrum 工具 | `to_list` | [spd.h](E:/CoronaEngine/vision/src/base/color/spd.h:41) |
| Spectrum 工具 | `array2vector` | [spd.cpp](E:/CoronaEngine/vision/src/base/color/spd.cpp:34) |
| Spectrum 工具 | `zero_if_any_nan`、`zero_if_any_nan_inf` | [spectrum.h](E:/CoronaEngine/vision/src/base/color/spectrum.h:218) |
| 渲染环境辅助 | `reset_frame_index`、`emplace_frame_index`、`emplace_sampled_wavelengths`、`has_denoiser` | [integrator.h](E:/CoronaEngine/vision/src/base/integral/integrator.h:108) |
| 诊断辅助 | `denoiser_disabled` | [evaluation_debug.h](E:/CoronaEngine/vision/src/base/mgr/evaluation_debug.h:12) |
| 几何辅助 | `remove_mesh`、`build_count` | [geometry.h](E:/CoronaEngine/vision/src/base/mgr/geometry.h:51) |
| Pipeline 旧接口 | `rebuild_view_context_renderers`、`on_touch`、`check_and_save`、`clear_geometry`、`deregister_buffer`、`deregister_texture3d`、`deregister_texture2d`、`output_spp`、`flip_debugger` | [pipeline.h](E:/CoronaEngine/vision/src/base/mgr/pipeline.h:104) |
| 旧 UI | `render_perf_panel`、`filp_show_fps`、`cur_node` | [pipeline_ui.h](E:/CoronaEngine/vision/src/base/mgr/pipeline_ui.h:29) |
| Registry | `get_element`、`precompute_albedo`、`SensorRegistry` | [registries.h](E:/CoronaEngine/vision/src/base/mgr/registries.h:33) |
| 旧相机列表 | `add_sensor`、`remove_sensor`、`cur_sensor_index`、`set_cur_sensor_index` | [scene.h](E:/CoronaEngine/vision/src/base/mgr/scene.h:154) |
| 节点构造辅助 | `construct`、`construct_unique`、`static_object_cast` | [node.h](E:/CoronaEngine/vision/src/base/node.h:80) |
| 灯光辅助 | `bound`、`is`、`add_emission_reference` | [light.h](E:/CoronaEngine/vision/src/base/illumination/light.h:118) |
| 材质和介质反向引用 | `add_material_reference`、`combine_flag` | [material.h](E:/CoronaEngine/vision/src/base/scattering/material.h:210) |
| 材质和介质反向引用 | `add_inside_reference`、`add_outside_reference` | [medium.h](E:/CoronaEngine/vision/src/base/scattering/medium.h:45) |
| 交互和采样辅助 | `material_inst_id`、`material_type_id`、`is_transition` | [interaction.h](E:/CoronaEngine/vision/src/base/scattering/interaction.h:309) |
| 交互和采样辅助 | `match_flag` | [bxdf.h](E:/CoronaEngine/vision/src/base/scattering/bxdf.h:70) |
| 交互和采样辅助 | `RaySample` | [sample.h](E:/CoronaEngine/vision/src/base/scattering/sample.h:67) |
| 交互和采样辅助 | `to_ratio_y` | [lobe.h](E:/CoronaEngine/vision/src/base/scattering/lobe.h:118) |
| 微表面辅助 | `roughness_to_alpha`、`alpha_to_roughness`、`calculate_alpha` | [microfacet.h](E:/CoronaEngine/vision/src/base/scattering/microfacet.h:24) |
| 旧滤波离散化 | `discretize` | [filter.h](E:/CoronaEngine/vision/src/base/sensor/filter.h:49) |
| Framebuffer 旧接口 | `foreach_neighbor`、`deregister_callback`、`fill_window_buffer`、`cur_screen_buffer`、`compute_motion_vector`、`has_custom_post_processing`、`post_processed_buffer` | [frame_buffer.h](E:/CoronaEngine/vision/src/base/sensor/frame_buffer.h:31) |
| Sensor 辅助接口 | `set_mat`、`device_forward`、`device_up`、`device_right`、`set_sensitivity` | [sensor.h](E:/CoronaEngine/vision/src/base/sensor/sensor.h:59) |
| ShaderGraph 兼容包装 | `build_graph_blueprint`、`resolve_graph`、`set_graph_and_resolve` | [shader_graph.h](E:/CoronaEngine/vision/src/base/shader_graph/shader_graph.h:47) |
| 几何批量包装 | `add_instances` | [shape.h](E:/CoronaEngine/vision/src/base/shape.h:206) |
| ImagePool 旧注入接口 | `set_source_cache` | [image_pool.h](E:/CoronaEngine/vision/src/base/mgr/image_pool.h:30) |
| Denoise 辅助接口 | `subpixel_resolution` | [denoiser.h](E:/CoronaEngine/vision/src/base/denoiser.h:86) |
| 数学辅助 | `gaussian_integral` | [util.h](E:/CoronaEngine/vision/src/math/util.h:45) |
| 数学辅助 | `uniform_disk_PDF`、`uniform_sphere_PDF`、`uniform_hemisphere_PDF`、`square_to_cone`、`uniform_cone_PDF`、`square_to_sphere`、`square_to_hemisphere`、`linear_PDF`、`power_heuristic`、`robust_MIS_weight_n` | [warp.h](E:/CoronaEngine/vision/src/math/warp.h:29) |
| 黑体辐射公式 | `black_body_impl` | [optics.h](E:/CoronaEngine/vision/src/math/optics.h:81) |
| ReBLUR EDSL 辅助 | `AtanApprox`、`GetStdDev`、`ComputeNonExponentialWeightWithSigma`、`IsInScreenBilinear`、`GetDisocclusionWeight`、`ComputeParallaxInPixels`、`GetColorCompressionExposureForSpatialPasses`、`GetFadeBasedOnAccumulatedFrames` | [edsl_nrd_common.h](E:/CoronaEngine/vision/src/render_core/denoiser/ReBLUR/edsl_nrd_common.h:52) |
| SSAT phase space 辅助 | `set_spatial`、`set_angular`、`offset_spatial`、`offset_angular`、`phase_distance_sq`、`make_phase_coord`、`make_phase_coord_zero` | [phase_space.h](E:/CoronaEngine/vision/src/render_core/denoiser/SSAT/phase_space.h:45) |
| SSAT 通用 gather | `phase_space_to_buffer_index`、`gather`、`gather_channel`、`gather_with_geometry` | [sparse_gather.h](E:/CoronaEngine/vision/src/render_core/denoiser/SSAT/sparse_gather.h:117) |
| SSAT getter | `adaptive_sampler`、`temporal_accumulator` | [ssat.h](E:/CoronaEngine/vision/src/render_core/denoiser/SSAT/ssat.h:19) |
| SSAT getter | `history_buffer` | [temporal_accumulator.h](E:/CoronaEngine/vision/src/render_core/denoiser/SSAT/temporal_accumulator.h:95) |
| SSAT 数学辅助 | `subpixel_linear_index`、`encode_subpixel`、`compute_luminance`、`weight_luminance`、`weight_depth`、`bspline_weight` | [utils.h](E:/CoronaEngine/vision/src/render_core/denoiser/SSAT/utils.h:70) |
| SVGF 辅助 | `compute_full_geometry_weight`、`should_skip`、`is_instance_boundary`、`compute_grazing_anisotropy`、`compute_edge_anisotropy`、`compute_combined_anisotropy`、`apply_edge_anisotropic_transform` | [utils.h](E:/CoronaEngine/vision/src/render_core/denoiser/SVGF/utils.h:387) |
| LightField 旧接口 | `tone_mapping_encoded`、`post_render`、`set_lenticular_params`、`set_geometry`、`output_resolution` | [light_field.cpp](E:/CoronaEngine/vision/src/render_core/framebuffer/light_field.cpp:626) |
| 旧窗口辅助 | `get_window_handle`、`background_visible`、`WindowLoop` | [window.h](E:/CoronaEngine/vision/src/window/GUI/window.h:64) |
| 旧 GL shader 辅助 | `create_gl_program` | [gl_helper.h](E:/CoronaEngine/vision/src/window/GUI_impl/imGui/gl_helper.h:59) |

`create_gl_shader()` 仅由无调用的 `create_gl_program()` 使用；`remove_cxx_comment()` 仅由 F19 的文件读取器使用。`RaySample` 没有消费者，但 `RayState`、`ScatterSample`、`BSDFSample` 正在使用。ShaderGraph 三个兼容包装无调用，不代表 `ensure_graph_ready()`、`set_graph_blueprint()` 或整个材质图可移除。

`ReLAX::motion_to_uv()` 和 `sample_bicubic()` 在集成测试中使用，未列入无调用名单；同样保留测试覆盖价值。

## 无消费者配置常量

以下 31 个常量在去除注释和字符串后的仓库 C/C++ 文本中仅出现于定义。这里列的是没有消费者的常量；F17 的整条闲置算法链还包含内部互相引用的常量。

| 文件 | 常量 |
| --- | --- |
| [svgf_config.h](E:/CoronaEngine/vision/src/render_core/denoiser/SVGF/svgf_config.h:11) | `kMinPositive`、`kPoissonNormalPowerDefault`、`kAlbedoThreshold`、`kExtremeValueThreshold`、`kExtremeValueScale`、`kDecayRate`、`kMaxVarianceMultiplier`、`kMinVarianceFloor`、`kMaxBlendWeight`、`kHistoryBlendThreshold`、`kDisocclusionBlendBoost`、`kMinScale`、`kMaxScale`、`kVarianceScale`、`kDirectAdaptiveStrength`、`kIndirectAdaptiveStrength`、`kEnabled`、`kEdgeAwareEnabled`、`kMinEdgeWeight` |
| [ssat_config.h](E:/CoronaEngine/vision/src/render_core/denoiser/SSAT/ssat_config.h:17) | `kDefaultRhoBase`、`kDefaultAlpha`、`kDepthWeightScale`、`kDefaultSpatialRadius`、`kDefaultAngularSamples`、`kDefaultSigmaLum`、`kDefaultSigmaDepthAngular`、`kAngularBandwidth`、`kDefaultSigmaX`、`kDefaultSigmaU`、`kMaxNeighborRadius` |
| [edsl_nrd_common.h](E:/CoronaEngine/vision/src/render_core/denoiser/ReBLUR/edsl_nrd_common.h:40) | `REBLUR_MAX_ACCUM_FRAME_NUM` |

SHARC 的 `constant.h` 已随 F03 移除；枚举和通用旗标没有直接消费者通常没有运行时成本，不宜作为优先优化目标。

## 明确保留的功能

- PT、ReSTIR direct/indirect/stable planes、SVGF、ReBLUR、ReLAX、每相机 view context、共享场景资源、零拷贝显示桥、相机同步和 actor picking 都有真实调用。
- 黑体材质是 fallback；它与未使用的 Planck 黑体辐射公式 `black_body_impl()` 是两回事。
- `GUI/decl.h`、`GUI/widgets.h`、`GUI/window.h` 是仍被 include 的兼容转发头，不是三套重复实现。
- `vision/src/render_core/denoiser/ReBLUR/native_nrd.cmake` 明确用 `VISION_REBLUR_NATIVE_NRD` 控制原生 NRD 和配套 EDSL 源；默认 OFF，但运行选择和测试存在，属于条件能力。
- `precomputed_table.h`、`dielectric_precompute.h`、`ltc_sheen_table.h`、`srgb2spec.h`、slot swizzle 生成头均有有效消费者。
- `VISION_INTERACTIVE_AUX_WORK` 的根默认值是 ON，其打印回收有条件调用；不能因为头文件 fallback 为 0 就判为关闭或废弃。

机器清单：[unused_features_inventory.json](E:/CoronaEngine/vision/unused_features_inventory.json)，提供每个插件、接口和配置常量的精确文件与行号。

## 清理顺序

1. **先修行为缺口**：F01 的 SSAT 入口、F09 保存请求、F20 失效配置；对空壳插件明确报不可用。
2. **减少确定的无效工作**：F08 Postprocessor 编译、F15 调试资源默认初始化（F02、F03 已移除）。移除前同步调用者和插件部署列表。
3. **清理独立闲置代码**：IES、金属 IOR 表、旧 TAA、无调用兼容包装；保持改动小并检查 ABI 使用范围。
4. **拆分实验和独立 viewer**：customized/render graph、真实镜头、旧窗口与面板（SHARC、BVH 已移除），不牵连当前主渲染链。
5. **保留测试与可选能力**：Hero spectrum、PowerLightSampler、非默认滤波器、Native NRD，以及预计算表。先决定产品支持范围，再裁剪构建。

初次审计仅添加文档和机器清单；后续已执行 F02、F03、F04 移除，状态与验证记录见机器清单的 `resolutions`。
