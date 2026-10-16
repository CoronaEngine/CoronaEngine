# Ocarina RELAX Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** 在 `feat/relax` 上用 Ocarina DSL 实现完整 RGB RELAX，覆盖 PT、ReSTIR 与三层 stable planes，并提供可复现的正确性、画质和性能证据。

**Architecture:** 以 RTXPT 的 producer/分层语义和 NRD 4.14.3 的 RELAX pass 图为准。ReLAX 插件负责生命周期，Filter 以 Kernel/CommandBatch 实现各 pass；每层历史独立、临时资源共享，最终复用 coverage-only resolve。

**Tech Stack:** C++20、Ocarina 嵌入式 DSL、现有 CUDA backend、CLion RelWithDebInfo、CMake/CTest、vision-eval、Python 数值/图像评估。

**Spec:** [移植设计](../specs/2026-10-12-relax-port-design.md)。状态：用户于 2026-10-16 授权顺序施工；产品实现及独立整分支审查已完成。审查的两项 Important 问题已用 GPU RED→GREEN 修复；修复后 17 项相关回归、64 个场景/性能任务及引擎构建通过。

## Global Constraints

- 分支 `feat/relax` 直接基于 `svgf_magic@f8a5ceb33f201e748ad69bcdfc7ab3afebc24371`；不整体合并 `feat/reblur`。
- RTXPT `21ef566cb37ef832438375fb3b5bd0b170e52f7c`，其 NRD 4.14.3 `16a64cd4daebfb153590623f0348b36007284ba3`；不混用独立 NRD 4.15.1。
- 使用引擎 `.workspace/Horizon` 的 Ocarina；不新增 SDK 执行后端，不改独立 ocarina 仓库。
- `RELAX_DIFFUSE_SPECULAR`、全分辨率 RGB、checkerboard OFF；reconstruction OFF/3×3/5×5，默认 OFF；外部 confidence/disocclusion-mix 输入缺省。
- 慢历史 30/30、快历史 6/6、historyFixFrameNum=3、historyFixBasePixelStride=14、spatialVarianceEstimationHistoryThreshold=3、prepass 30/50、phiLuminance=2/1、colorBoxSigmaScale=2；À-trous 默认 5 轮、范围 2–8；AntiFirefly=false。
- RELAX 输入为 RGB + 非归一化世界 hit distance；FP32 运算和初始存储；保留内部 moment/variance/颜色夹取等不同数据语义。
- 保留 SVGF 默认、DI/GI 含义、stable prefix 能量归属和一次完整像素 coverage resolve。
- 任何优化必须在相同 fixture 上验证；不得把未测画质、性能或精度等价写成结论。

## Review Focus

1. 复合材质、黑色但有效的样本及稀疏 lobe：能量守恒，距离有效性不依赖亮度；归 Task 1/2/7。
2. 零 roughness、凹面镜、彩色 glass、多段 delta：虚拟运动和采样 frame 正确，局限可见；归 Task 3/6/8。
3. 1×1、127×73、边界 tile、resize 后旧 descriptor：无越界、barrier 死锁或旧历史；归 Task 2/5/7。
4. 停用后重启、帧号断裂、branch 换槽及 scene/camera cut：精确失效，不跨层读旧信号；归 Task 3/7。
5. DI/GI 非 0/1 factor、primary emission/sky、framebuffer accumulation：不重复缩放，不借全帧平均掩盖降噪结果；归 Task 6/7/8。

## 文件边界与执行顺序

| 文件组 | 责任 |
| --- | --- |
| 新建 `vision/src/render_core/denoiser/ReLAX/relax.{h,cpp}`、`CMakeLists.txt` | Denoiser 插件、配置、UI、prepare/compile/dispatch/reset |
| 新建 `ReLAX/filter.{h,cpp}`、`config.h`、`utils.h` | 层资源、共享 scratch、参数与采样/数学 helper |
| 新建 `ReLAX/modulator.cpp`、`prepass.cpp` | guide、去/恢复材质、pack、tile、reconstruction、prepass |
| 新建 `ReLAX/temporal_accumulator.cpp`、`history_fix.cpp`、`history_clamping.cpp`、`atrous.cpp` | 完整核心算法；copy/AntiFirefly 归 history_clamping |
| 修改 `base/denoiser.h`、`base/sensor/frame_buffer.{h,cpp}`、必要的 `sensor.h` | 显式 lobe 输入及资源生命周期 |
| 修改 `base/integral/integrator.cpp`、`base/scattering/{interaction.h,material.h,material.cpp,lobe.h,lobe.cpp}`、必要的 `render_core/material/{plastic.cpp,substrate.cpp}` | 按实际 BSDF 求值拆分、材质因子和 hit distance |
| 修改 `render_core/integrator/{pt.cpp,rt.cpp,ReSTIR/direct.h,ReSTIR/direct.cpp,ReSTIR/indirect.h,ReSTIR/indirect.cpp,ReSTIR/stable_planes.h,ReSTIR/stable_planes.cpp}` | producer、层调度、合成 |
| 修改 `render_core/denoiser/SVGF/{svgf.h,svgf.cpp,poisson.h,poisson.cpp}` | 必需的 coverage-only 资源与入口，保留原 SVGF 路径 |
| 新建 `tests/integration/test_vision_relax{,_passes,_producers}.cpp`、`relax_test_fixture.h` | 实际 shader、fixture 和完整渲染测试；单一测试入口 |
| 新建 `tests/fixtures/relax/scenarios.json`、`vision/tools/relax_eval.py`、`ReLAX/README.md`、`LICENSE-NRD.txt` | 场景 manifest、评估和来源/证据 |

表中 `ReLAX/`、`base/`、`render_core/` 分别相对 `vision/src/render_core/denoiser/`、`vision/src/`、`vision/src/`。其余均相对仓库根目录。只有必要文件才实际修改。

顺序：1 输入与测试基础 → 2 prepass → 3 temporal → 4 history → 5 À-trous → 6 PT → 7 ReSTIR/分层 → 8 全量验收。以当前会话顺序实现为建议执行方式；若用户选择分工，资源/输入接口仍先冻结，禁止对同一 producer 文件并行修改。

## 构建与测试命令约定

执行开始先重新读取 AGENTS.md 和两个 CLion 技能，核对工作区、分支与未提交修改；复用已建立的分支。当前缓存 `BUILD_TESTING=ON`、`CORONA_BUILD_VISION=ON`，`VISION_BUILD_TESTS=OFF` 不妨碍根目录 integration tests，不为本任务改开关。

CMake 修改后配置、最终引擎构建分别使用：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .agents/skills/clion-cmake-relwithdebinfo/scripts/configure.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .agents/skills/clion-build-corona-engine/scripts/build.ps1
```

测试专用目标沿用同一 CMake、构建目录和 MSVC 环境，单独构建；不能用这个命令代替上面的 `corona_engine` 验证。以下变量在同一个 PowerShell 会话初始化：

```powershell
$relaxCache = Get-Content -LiteralPath 'cmake-build-relwithdebinfo/CMakeCache.txt'
$relaxCmake = [string](($relaxCache | Where-Object { $_ -match '^CMAKE_COMMAND:' }) -replace '^[^=]+=', '')
$relaxCompiler = [string](($relaxCache | Where-Object { $_ -match '^CMAKE_CXX_COMPILER:' }) -replace '^[^=]+=', '')
. .agents/skills/clion-cmake-relwithdebinfo/scripts/clion-msvc.ps1
Import-ClionMsvcEnvironment -RepositoryRoot (Get-Location).Path -CMakePath $relaxCmake -CachedCompiler $relaxCompiler
$relaxCtest = Join-Path (Split-Path $relaxCmake) 'ctest.exe'
& $relaxCmake --build cmake-build-relwithdebinfo --target corona_vision_relax_tests -j 30
& $relaxCtest --test-dir cmake-build-relwithdebinfo -R '^VisionRelax' --output-on-failure
```

Task 1 注册单个可执行目标 `corona_vision_relax_tests`，支持 `helpers|prepass|temporal|history|atrous|pt|restir|lifecycle` 子命令，分别注册 `VisionRelaxHelpers` 等 CTest 项，全部 RUN_SERIAL。每阶段红/绿运行只执行已实现分组；确认失败来自预期行为缺失。记录命令、退出码、日志和对应提交。

## Task 1：输入契约、数值基础与最小插件

**Files:** 创建 ReLAX 的 relax/filter/config/utils/modulator、CMake 和 LICENSE；修改 denoiser 根 CMake、`vision/src/base/denoiser.h`；创建测试入口、fixture 和 passes 文件，登记 `tests/integration/CMakeLists.txt`。

**Interfaces:** `DenoiseLobeInput` 含 `BufferView<float4> diffuse_radiance_hitdist, specular_radiance_hitdist, output_diffuse, output_specular`；`RealTimeDenoiseInput::{has_lobe_signals,lobe_signals}`；`Denoiser::requires_lobe_signals() const noexcept -> bool`。`vision::relax::ReLAX` 实现既有 Denoiser 接口，插件名 `ReLAX`，目标 `vision-denoiser-ReLAX`。

`RelaxSettings` 对应设计默认值。`Filter::prepare(uint2 resolution, uint layer_count)`、`Filter::compile()`、`Filter::dispatch(RealTimeDenoiseInput&, const RelaxSettings&) noexcept -> CommandBatch` 管理实际算法。`FilterParam`/各 pass 参数以 BufferDesc 绑定有界 layer view；分阶段明确 hitDist/moment/variance 语义，不依赖 descriptor offset 自动生效。

- [x] 记录固定 NRD/RTXPT 文件哈希、未修改 baseline 场景配置，以及每个永久/临时字段的字节与最后消费者；把来源写入 README。
- [x] 新增 GPU helper 失败用例：pack 保持 `(1,2,3,7)`；黑色 radiance+hitT=7 保持有效；负数/NaN/Inf 及 >65504 输入遵守固定前端；二维 motion `(2,-1)` 在 `100×50` 得 `(-0.02,0.02)`；常量/线性 ramp 的 point/bilinear/bicubic 采样符合 CPU 参考，边缘按有效 tap 回退。
- [x] 运行 `VisionRelaxHelpers` 记录预期失败；实现源公式、最小 buffer 生命周期和真实 DSL compile/dispatch。复用当前项目的测试初始化，不引入新运行时。
- [x] 运行相同测试通过：helper 用 `abs+rel=1e-5` 的 FP32 容差；颜色 pack 在可精确表示输入上做精确比较。验证 subview 不改相邻层 sentinel。
- [x] 仅提交上述文件，提交信息 `feat(render): define RELAX signals and shader helpers`；更新任务证据。

## Task 2：guide、reconstruction 与 prepass

**Files:** `ReLAX/{modulator.cpp,prepass.cpp,filter.h,filter.cpp,utils.h}`、`test_vision_relax_passes.cpp`。

**Interfaces:** 在 Filter 增加 `compile_prepass() noexcept`、`dispatch_prepass(const FilterParam&) noexcept -> CommandBatch`，产出 demodulated diffuse/specular+hitT、tile mask 和 specular tracking 输入。Modulator 输入既有 visibility/stable surfaces 与 Task 1 lobe views，输出严格的 normal/roughness/viewZ/factor/identity。

- [x] 新增失败用例：恒定颜色不变、深度阶跃不跨面混合、roughness 0/1、黑色有效样本不当缺失、3×3/5×5 reconstruction 仅在有效几何邻域取样；sky/空层输出归零。
- [x] 在 1×1 与 127×73 上运行 `VisionRelaxPrepass` 观察预期失败。
- [x] 翻译 `RELAX_ClassifyTiles`、`HitDistReconstruction`、`PrePass`；按采样契约处理有效零辐射与 unsampled lobe，默认 reconstruction OFF。实现 reference clamp/sampling 行为，部分 tile 不提前越过 barrier。
- [x] GPU 与独立 CPU 小邻域 oracle 用 `abs+rel=1e-4` 比较；同尺寸 canary 检测边界写入；重复执行检查确定性。
- [x] 提交 `feat(render): port RELAX guide preparation and prepass`。

## Task 3：完整时间累积

**Files:** `ReLAX/{temporal_accumulator.cpp,filter.h,filter.cpp,utils.h}`、`test_vision_relax_passes.cpp`。

**Interfaces:** `Filter::compile_temporal() noexcept`、`dispatch_temporal(const FilterParam&) noexcept -> CommandBatch`。消费上一帧慢/快历史、reflection hitT、几何/身份；输出本帧两套颜色+moments、history length、tracking distance 和 specular confidence。

- [x] 新增失败用例：64 帧常量、亚像素 jitter 静止表面、已知一像素位移、遮挡显露、法线/roughness 变化、屏幕边缘 footprint；镜面虚拟点运动必须区别于 surface-only 结果。
- [x] 新增 identity/frame-gap/camera-cut/scene-reset 用例：不相关历史拒绝；正常相机移动不无条件 reset。无效状态污染为 NaN sentinel 时，拒绝后输出仍有限。
- [x] 运行 `VisionRelaxTemporal` 记录预期失败；移植 surface 与 virtual 两套重投影、曲率/视差、specular motion V2、bicubic 有效性和 bilinear fallback、快慢历史/moments。常量准备来自 `Source/Relax.cpp`，dt 使用仿真时间。
- [x] 验证 30/6 上限、source 规定的 reset 首帧行为和 reference 数值容差；反转像素写入顺序仍得到等价结果，证明没有原位邻域历史覆盖。
- [x] 提交 `feat(render): port RELAX temporal accumulation`。

## Task 4：历史修复、夹取与 firefly

**Files:** `ReLAX/{history_fix.cpp,history_clamping.cpp,filter.h,filter.cpp}`、`test_vision_relax_passes.cpp`。

**Interfaces:** `compile_history() noexcept`、`dispatch_history(const FilterParam&) noexcept -> CommandBatch`；包含 HistoryFix、HistoryClamping/antilag 与可选 Copy+AntiFirefly。Temporal 的上一帧读取完成后才发布新的慢/快颜色和长度历史。

- [x] 用短历史几何边界、稳定长历史、突然开灯/熄灯、单像素 firefly 与真实小亮光源建立失败 fixture。断言参考的夹取边界、history acceleration/reset，以及快历史保留位置。
- [x] 运行 `VisionRelaxHistory` 记录预期失败；按固定源码移植 historyFixFrameNum=3、stride=14、sigma=2 以及 antilag 全部默认系数。
- [x] 实现 AntiFirefly=false 的旁路和开启时的只读 copy 输入；不能把附近线程刚写的结果拿作邻居。
- [x] GPU 对照 CPU fixture 通过；恒定信号开关结果一致，孤立异常值被抑制，边界外不改变；保存明暗突变的逐帧输出与历史。
- [x] 提交 `feat(render): port RELAX history repair and clamping`。

## Task 5：À-trous 与完整单层 filter

**Files:** `ReLAX/{atrous.cpp,filter.h,filter.cpp,relax.cpp}`、`test_vision_relax_passes.cpp`。

**Interfaces:** `compile_atrous() noexcept`、`dispatch_atrous(const FilterParam&, uint iteration_count) noexcept -> CommandBatch`；首轮单独处理 moments→variance、短历史空间方差；后续每轮 `step=1<<i`，最后一轮遵守源输出规则。

- [x] 建立常量、impulse、几何边界、低/高 roughness 的 2/5/8 轮失败 fixture，逐轮比较颜色和 variance；确认最终 spatial 结果不写回 slow/fast 历史。
- [x] 运行 `VisionRelaxAtrous` 记录预期失败；移植 `RELAX_AtrousSmem` 数学含义与普通 `RELAX_Atrous`；先用可验证 buffer taps，实现所需边界、luminance/normal/depth/roughness/confidence 权重。
- [x] 串联完整 pass 图；只在所有消费者结束后 alias scratch，逐层顺序复用。shader 编译/分配留在 prepare/compile，稳态 dispatch 不重建 kernel。
- [x] GPU 与 CPU fixture 在 `1e-4` 容差内；长历史/短历史路径均覆盖；完整单层 64 帧无 NaN，1×1/127×73 无越界。
- [x] 提交 `feat(render): complete RELAX atrous filtering`。共享内存优化只在后续测量证明必要且数值对照通过时加入。

## Task 6：真实 PT producer 与配置入口

**Files:** 文件边界表中的 FrameBuffer、integrator.cpp、材质/lobe、pt.cpp；`src/systems/optics/vision/vision_render_mode_config.cpp`；`tests/systems/optics/test_vision_render_mode_config.cpp`；`test_vision_relax_producers.cpp`；ReLAX modulator/relax。

**Interfaces:** FrameBuffer 提供 `prepare_denoiser_signals()`，以及 `denoiser_diffuse`、`denoiser_specular`、`denoiser_bypass`、`denoiser_filtered_diffuse`、`denoiser_filtered_specular` 的有界 views。复用已有 HitContext 扩展传递实际 lobe 贡献和距离；SVGF/SSAT 不要求这些额外数据。

- [x] 用 diffuse、metal、混合 Plastic/Substrate、mirror/glass、primary sky/emission 建立真实 PT 失败用例：不滤波时 `diffuse+specular+bypass≈原始总辐射`，容差 `abs+rel=1e-4`；彩色 lobe 分配按 RGB BSDF 而非单个亮度比例。
- [x] 配置测试固定要求：显式 `denoiser.type=ReLAX` 被保留；未配置仍是 svgf；SSAT 仍按原规则；关闭/再启用与 resize 均正确准备资源。
- [x] 运行 `VisionRelaxPT` 和配置测试记录预期失败；参考 feat/reblur 的相关 hunk 逐处移入中性接口，补全方向相关 factor、actual hit distance、最多三段 delta continuation、missing-lobe guide ray 与 remodulation。
- [x] guide ray 使用独立维度，不改变原 raw estimator；有限黑色样本、miss 的有效距离按契约填充，追加光线不贡献第二份能量。
- [x] 完整 PT 路径通过；保存最小真实场景 raw/SVGF/ReLAX，明确关闭 framebuffer accumulation；构建/回归 `VisionRenderModeConfigTests`、`VisionEmbeddedModeSwitchTests`。
- [x] 提交 `feat(render): integrate RELAX with path tracing`。

## Task 7：ReSTIR、三层、coverage 与生命周期

**Files:** `rt.cpp`、`ReSTIR/{direct,indirect,stable_planes}.{h,cpp}`、上述 FrameBuffer/interaction、SVGF coverage 相关文件；`test_vision_relax_producers.cpp`；Filter/relax 的层资源状态。

**Interfaces:** DI/GI 产生处输出 Task 1 lobe 信号，`RealTimeDenoiseInput.layer_index/layer_count` 选择私有历史；已有 direct/indirect raw views 不改含义。参考 feat/reblur 为 SVGF 增加构造配置 `coverage_only`；ReLAX 用 `DenoiserDesc{"svgf"}`、`desc.set_value("coverage_only", true)` 创建 coverage 实例，在完整像素合成后仅 dispatch coverage 路径。

- [x] 建立失败用例：DI/GI factor=0、1、0.25/0.75；彩色反射/透射三层；layer slot 交换、消失两帧再出现；branch/receiver 改变；normal/roughness 边界；混合层含 sky/emission。
- [x] 建立生命周期用例：无 stable planes→三层→关闭、raw→启用、127×73→1×1→原尺寸、重复/跳帧、投影改变、场景重编译；历史与 coverage 同步失效。
- [x] 运行 `VisionRelaxReSTIR`、`VisionRelaxLifecycle` 记录预期失败；移植必要 producer 拆分，应用 DI/GI factor 一次，逐层 denoise 后 compose 与一次 coverage；scratch 跨层只复用当前临时数据。
- [x] 验证未过滤能量守恒及分层独立；coverage 输出不写入 RELAX history，coverage-only 不分配或执行 SVGF 光照滤波；空 tile 清零/失效验证通过。
- [x] 构建并执行 `VisionEmbeddedModeSwitchTests`、`VisionReSTIREstimatorTests`、三个 VisionReSTIR MIS/support 回归与 `VisionShaderBindingTests`；真实相机横移验证玻璃近似与 invalidation。
- [x] 提交 `feat(render): integrate layered RELAX and coverage resolve`。

## Task 8：场景评估、性能与交付证据

**Files:** 新建 `tests/fixtures/relax/scenarios.json`、`vision/tools/relax_eval.py`，完善 `ReLAX/README.md`；仅在缺诊断入口时小改 `vision/src/apps/vision-eval/main.cpp` 和 ReLAX 计时/资源统计。

**Interfaces:** `relax_eval.py --build-dir <dir> --manifest <json> --output-dir <dir>`；manifest 固定资产路径、camera、resolution、frame count、ROI、seed/维度和参考策略。报告包含代码/参考版本、实际 accumulation 状态、每张图的姿态及 timing 口径。

- [x] 选择可访问的 CoronaExample Kitchen、程序化材质/镜面与 glass fixtures；验证 manifest 失败能指出缺失资产，不能跳过失败场景后汇报整体成功。
- [x] 静态每 producer 输出 raw/SVGF/ReLAX 的 1 spp、64 帧结果，生成 2048 spp PT 参考；如参考噪声仍显著，追加独立 seed 检查并标注参考不确定性。
- [x] 移动/停止至少 96 帧，保存遮挡显露、镜面横移、停止恢复的关键帧。对选定姿态生成相同 pose 的高 spp 参考后才计算移动误差；报告 ROI 时间稳定性、细节与拖影，保留不利结果。
- [x] 计时至少预热 8 帧、测量 16 帧并重复 3 次；记录 720p/1080p、PT/三层 ReSTIR 的每个 RELAX pass、guide rays、coverage、总帧和显存。外部峰值与按字段分配账本同时保存；paging/OOM 属于未通过的容量结果，不隐去。
- [x] 运行全部 `VisionRelax*` 与 Task 7 相关回归，按 CLion 配置技能重新配置后按 build 技能构建 `corona_engine`；退出码 0 且可执行文件存在。禁止拿旧 feat/reblur 的二进制或日志当本分支验证。
- [x] 进行最终 diff/算法来源与输入归属审查；文档列出支持范围、全部实测数据、质量退化和已知局限。提交 `test(render): validate RELAX quality and resource costs`。

评估调用示例（在 Task 8 产物建立后执行）：

```powershell
$env:CORONA_EXAMPLE = 'D:/work/corona/CoronaExample'
python vision/tools/relax_eval.py --build-dir cmake-build-relwithdebinfo --manifest tests/fixtures/relax/scenarios.json --output-dir cmake-build-relwithdebinfo/relax-evidence
& $relaxCtest --test-dir cmake-build-relwithdebinfo -R 'VisionRelax|VisionEmbeddedModeSwitchTests|VisionRenderModeConfigTests|VisionReSTIR|VisionShaderBindingTests' --output-on-failure
```

## 当前证据与评审检查

- [x] 读取 AGENTS.md，确认初始工作区干净、没有需要搬运的未提交产品改动。
- [x] 核实 `svgf_magic` 与 `origin/svgf_magic` 在 `f8a5ceb3`，执行 `git switch -c feat/relax svgf_magic` 成功。
- [x] 阅读固定 NRD pass 图/常量/关键 shader、RTXPT 输入与合成；识别 4.14.3/4.15.1 差异风险。
- [x] 核实实际 Horizon/Ocarina checkout、现有 DSL 调度、producer 缺口、coverage 和 CLion 验证入口。
- [x] 自审设计覆盖、阶段接口、历史读写时机、默认值、五类易漏输入与对应验收任务。
- [x] 用户于 2026-10-16 通过“开始施工吧”授权当前会话顺序实现。
- [x] Task 1–8 的实现及证据已完成：17 项相关回归、64 个场景/性能任务与 CLion 同环境引擎构建通过。8 GiB 三层 1080p 容量余量未获认证，限制见 ReLAX README。

独立整分支审查已完成：无 Critical/Minor，两项 Important 均已在 `12dcaa2f` 修复。
静态折射历史使用独立 depth anchor，6 帧年龄由 1 恢复至 6；ReSTIR 的极暗/黑色
距离合并不再依赖 epsilon 亮度阈值。两项均观察到 GPU 回归先失败后通过，完整
17 项相关回归再次通过（75.50 s），64 个场景/性能任务全部重新测量。
最终二进制指纹、逐层 pass、每帧一次 coverage 和实际 buffer 字节均核对通过。
RTX 3070 Ti 8 GiB 三层 1080p 平均约 636 ms、device-wide 峰值 7883 MiB；
未认证容量余量或无换页。完整日志与评审处理保留在本 worktree 的
`cmake-build-relwithdebinfo/relax-evidence`。按用户要求保留分支及独立工作区。
