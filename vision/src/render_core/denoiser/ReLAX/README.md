# RELAX in Ocarina

全分辨率 RGB diffuse/specular RELAX，使用 Ocarina 的 `Kernel`、结构参数、
`device.compile` 和 `CommandBatch` 实现。分支 `feat/relax` 基于
`svgf_magic@f8a5ceb33f201e748ad69bcdfc7ab3afebc24371`。

算法固定参考 NVIDIA NRD **4.14.3**
`16a64cd4daebfb153590623f0348b36007284ba3`，对应 RTXPT
`21ef566cb37ef832438375fb3b5bd0b170e52f7c`；MathLib 使用 NRD 固定的 **v1.3**。
没有混入本机另一份 NRD 4.15.1。来源声明见 [LICENSE-NRD.txt](LICENSE-NRD.txt)
和 [LICENSE-MathLib.txt](LICENSE-MathLib.txt)。运行时无需 NRD SDK。

## 使用

在 PT 或 ReSTIR integrator 的 `param` 中显式选择：

```json
"denoiser": {
  "type": "ReLAX",
  "param": {
    "diffuse_history": 30,
    "specular_history": 30,
    "diffuse_fast_history": 6,
    "specular_fast_history": 6,
    "diffuse_prepass": 30,
    "specular_prepass": 50,
    "atrous_iterations": 5,
    "anti_firefly": false,
    "reconstruction": 0
  }
}
```

设置 `output.denoise=true`、framebuffer `accumulation=false`。`reconstruction`
取 0/1/2，分别为关闭、3×3、5×5；À-trous 支持 2–8 轮。其余默认值位于
[config.h](config.h)，遵循上述 NRD 版本。history 参数统计历史帧，所以 30
允许含当前帧共 31 帧。未配置仍使用 SVGF，SSAT 保持原规则。

ReSTIR 沿用现有 stable planes 开关，最多三层。DI/GI 的 `open` 控制开关，
新增可选 `factor` 控制非负合成权重，默认 1。权重只应用一次。raw→启用、
resize、投影改变、场景重编译和层开关均处理资源准备及历史失效。

## 信号与管线

输入是线性 RGB 和 **世界空间 hit distance**，w=0 表示未采样，不做距离归一化。
有效黑色样本保留正距离；miss 使用场景尺度的有效距离。ReSTIR 距离合并按实际
正权重归一化，全黑时保留有效端点，关闭的 DI/GI 不提供端点。材质按 RGB BSDF 拆分，
方向相关的 diffuse/specular factor 用于解调与重调制。缺失 lobe 的 guide ray
使用独立维度 1024，不向原 estimator 再贡献能量。primary sky/emission 走 bypass。

执行顺序：PrepareGuidesAndPack → 可选 HitDistReconstruction → PrePass →
TemporalAccumulation → HistoryFix → HistoryClamping（发布 illumination history）→
可选 AntiFirefly（替换发布结果）→ AtrousFirstAndGeometry → À-trous →
末轮 AtrousAndUnpack。独立 Filter 测试从原始信号运行独立 Pack。

各层保留私有几何、slow/fast RGB、二阶矩、年龄和 reflection tracking；临时
buffer 跨层顺序复用。illumination history 发布发生在空间滤波之前，geometry 在首轮 Atrous 写回；coverage 只作用于最终完整
像素，均不回流 RELAX illumination history。FrameBuffer 的中性 `denoiser_*`
原始 lobe buffer 在 Pack 冻结输入后复用为输出，避免另分配输出副本。

`position.w` 为正的轴向 viewZ；`depth_position` 单独保存世界空间 depth anchor，
当前和上一相机的深度均由该 anchor 计算，避免折射后与 virtual position 混用。
virtual position/normal 用于虚拟运动。motion 为 current−previous raster pixels。
Ocarina 插值顺序是 `lerp(t,a,b)`；同类型 swizzle 赋值可能覆盖 w，因此保留矩的
RGB 写入使用显式 float4。12-tap Catmull-Rom 按 NRD 省略四角并重新归一化。

## 验证与复现

CUDA 后端验证覆盖 sanitization、黑色距离、解调、采样、prepass/reconstruction、
surface/virtual reprojection、折射 anchor 与虚拟位置分离、极暗/黑色端点有效性、
短历史修复、亮灭突变、firefly、二阶矩及方差传播、
2/5/8 轮 À-trous、三层归属/交换/消失、重复/跳帧和真实 PT/ReSTIR 生命周期。
真实 producer 用例要求未过滤 `diffuse+specular+bypass` 与原 estimator 在 1e-4
相对/绝对容差内一致。场景测试断言实际启用且准备 ReLAX，防止误测 raw。

```powershell
$env:CORONA_EXAMPLE = 'D:/work/corona/CoronaExample'
python vision/tools/relax_eval.py --build-dir cmake-build-relwithdebinfo --manifest tests/fixtures/relax/scenarios.json --output-dir cmake-build-relwithdebinfo/relax-evidence
```

可用 `--kind quality` / `--kind performance` 分开执行，`--resume` 复用相同 renderer
二进制和 manifest 的成功结果。资产缺失会在启动渲染前报错；失败或超时保留日志，
不会作为整体成功。性能首次容量尝试失败后，其余重复标记为未运行。

产物含每次配置、命令、相机姿态、线性 PFM/PNG、两组独立种子 2048 spp PT 参考、
逐 ROI RMSE、静态尾段时间标准差、逐阶段耗时和显存采样。比较使用两张参考的
平均，并保留两参考的差异。移动误差只比较相同姿态。静态取第 63 帧，运动序列
至少 96 帧，在 31/47/63/95 帧取参考；评估器还会渲染其既有的 16 帧尾段。

性能每组预热 8 帧、测量 16 帧、重复 3 次。`VISION_RELAX_PROFILE` 指定 JSONL
日志，配合 `vision-eval --stage-profile` 使用。数字为含提交/同步的 host wall time，
不等同于 GPU timestamp。PrepareGuides 包括材质评估和额外 guide rays。buffer
逻辑字节与 device-wide 显存峰值分开报告；后者包含其他进程，不能直接相减当作
该算法的独占用量。

按仓库 CLion 技能配置、构建 `corona_engine` 后再运行模式切换回归：该回归会切换
到引擎输出目录并依赖其中已安装的 CUDA 头文件。当前固定 Horizon 的 embedded
shader generator 未清理路径里的 `-`，在含连字符的 worktree 中首次构建需要对
生成头文件的 `slang_module_*` 标识符作 `-`→`_` 修正；验证时仅修改了 build 输出，
没有改共享 Horizon。

## 支持边界与取舍

- 已验证 CUDA、RGB、普通 PT、ReSTIR 与现有最多三层 stable planes；未宣称其他
  Ocarina 后端、SH、checkerboard、透明材质物理精确虚拟运动或无限层。
- 平面镜面使用现有展开坐标，折射及曲面仍遵循 stable planes 的近似。近薄透镜
  奇点的分母作 1e-6 限制，避免非有限重投影，代价是可能丢失该处历史。
- 保留 NRD 的默认值和 4.14.3 host 参数接线，包括 luminance relaxation 来自
  roughness relaxation。后续若调整，应另作参数/画质变更。
- 在 NRD 原本标为 TODO 的短历史空间方差中加入平面深度和 branch gate，防止
  初次显露时跨边界复用；代价是尖锐边界处复用更少。重投影还要求精确 receiver/
  prefix identity，可能比原 NRD 更早拒绝历史。
- 采用完整精度和缓存 world geometry，未为降低显存引入 packed format。实际
  1080p 三层容量及性能结果单列，不能以低分辨率通过替代。
- ReLAX 并非在所有场景优于 SVGF。材质、玻璃和镜面边缘的退化与参考噪声均须
  同时查看；不能仅凭全图平均或单张降噪图判断质量。

## 初版移植实测（优化前，2026-10-16）

本节保留初版数据作为基线；容量修复见下方“636 ms 的根因与优化”，最新性能见“RELAX 逐 pass 优化”。

设备：NVIDIA GeForce RTX 3070 Ti, 8192 MiB, 610.88。17 项相关 CTest 回归通过；64 个渲染任务均完成；8 组静态/停止尾段时间噪声均低于 raw。结果已在最终审查修复后重新测量。

第 63 帧全图线性 RMSE（两组独立 2048 spp PT 参考的平均）：

| 场景 | raw | SVGF | ReLAX | 两参考差异 RMSE |
|---|---:|---:|---:|---:|
| kitchen-pt | 1.287444 | 0.714600 | 0.495288 | 0.040920 |
| kitchen-restir | 2.480845 | 0.359443 | 0.335041 | 0.040920 |
| materials-pt | 0.452840 | 0.034600 | 0.041681 | 0.017229 |
| materials-rt | 0.548848 | 0.059684 | 0.085252 | 0.017229 |
| glass-pt | 0.471450 | 0.031970 | 0.040018 | 0.017589 |
| glass-rt | 0.325003 | 0.043455 | 0.058856 | 0.017589 |

静态第 48–63 帧的逐像素 RGB 时间标准差，再对全图取平均：

| 场景 | raw | SVGF | ReLAX |
|---|---:|---:|---:|
| kitchen-pt | 0.587565 | 0.004901 | 0.005921 |
| kitchen-restir | 0.157906 | 0.005428 | 0.005293 |
| materials-pt | 0.225043 | 0.000495 | 0.000920 |
| materials-rt | 0.197549 | 0.001251 | 0.001747 |
| glass-pt | 0.247120 | 0.000576 | 0.000974 |
| glass-rt | 0.222532 | 0.001106 | 0.001605 |

移动/停止的同姿态全图线性 RMSE；第 47 帧停止，第 63/95 帧观察恢复：

| 场景/帧 | raw | SVGF | ReLAX |
|---|---:|---:|---:|
| glass-motion/31 | 0.614812 | 0.046802 | 0.075946 |
| glass-motion/47 | 0.521737 | 0.045384 | 0.072536 |
| glass-motion/63 | 0.303397 | 0.048177 | 0.057068 |
| glass-motion/95 | 0.448547 | 0.049224 | 0.061838 |
| kitchen-motion/31 | 1.016102 | 0.432686 | 0.434516 |
| kitchen-motion/47 | 1.612223 | 0.477759 | 0.453483 |
| kitchen-motion/63 | 0.936661 | 0.324559 | 0.280943 |
| kitchen-motion/95 | 1.191564 | 0.268791 | 0.224769 |

退化项：materials/glass 的静态全图及 compound/glossy/mirror-edge ROI 多数高于 SVGF；玻璃运动序列也高于 SVGF。Kitchen PT 的时间噪声略高于 SVGF，但全图误差更低。线性 RMSE 将偏差、模糊和残余噪声混合计量，不能将差值全部解释为噪声；镜面细节、轮廓模糊和停止后残影需同时看 PNG/PFM。完整逐 ROI 数据、对应姿态及不利结果均保留在 `relax-evidence/report.json`。两参考差异非零，小于此量级的画质差异不作确定性优劣结论。

### 性能与容量

同一 Kitchen，相同设置；每组 3 次平均（每次预热 8 帧、测量 16 帧）。总帧为同步测量模式，含引擎其他阶段和计时开销。

| 路径/分辨率 | 总帧 ms | RELAX+guide+coverage ms | buffer 逻辑 MiB | device-wide 峰值 MiB | RELAX CPU 编译 ms（PTX cache 已热） |
|---|---:|---:|---:|---:|---:|
| pt-1280x720 | 21.901 | 11.165 | 562.514 | 1673 | 555.187 |
| rt-1280x720 | 59.892 | 21.492 | 903.529 | 4399 | 591.689 |
| pt-1920x1080 | 46.841 | 26.933 | 1265.656 | 2493 | 608.238 |
| rt-1920x1080 | 636.019 | 54.643 | 2032.941 | 7883 | 608.752 |

逐 pass 同步耗时（ms，ReSTIR 对所有层求和）：

| 阶段 | PT 720p | 三层 720p | PT 1080p | 三层 1080p |
|---|---:|---:|---:|---:|
| PrepareGuidesIncludingRays | 0.447 | 2.759 | 0.932 | 5.584 |
| Pack | 0.295 | 0.634 | 0.640 | 2.156 |
| ClassifyTiles | 0.194 | 0.597 | 0.453 | 1.306 |
| PrePass | 2.223 | 2.803 | 5.569 | 7.224 |
| TemporalAccumulation | 1.064 | 1.580 | 2.566 | 3.683 |
| HistoryFix | 0.371 | 0.994 | 0.718 | 1.927 |
| HistoryClamping | 0.902 | 1.241 | 1.966 | 2.701 |
| PublishHistory | 0.530 | 1.631 | 1.162 | 3.839 |
| AtrousFirst | 0.676 | 1.070 | 1.414 | 2.222 |
| Atrous2 | 0.519 | 0.877 | 1.144 | 1.810 |
| Atrous4 | 0.580 | 0.922 | 1.252 | 1.973 |
| Atrous8 | 0.988 | 1.315 | 2.614 | 3.298 |
| Atrous16 | 1.394 | 1.740 | 4.402 | 5.121 |
| Unpack | 0.292 | 0.641 | 0.648 | 5.452 |
| PublishPT | 0.166 | — | 0.346 | — |
| Coverage | 0.524 | 2.688 | 1.106 | 6.347 |

buffer 账本：Filter 为共享 scratch 256 bytes/pixel + 每层 history/geometry 128 bytes/pixel，另加每 16×16 tile 的 uint。独立 depth anchor 的 16 bytes/pixel 已计入上述共享 scratch。producer 当前固定分配 192 bytes/pixel；coverage-only 为 PT 64、guided ReSTIR 196 bytes/pixel。输出与输入 alias 不重复计数。Filter 和 producer 字节从实际 buffer 大小求和；coverage 数字按已分配的字段/分辨率计算，不包含 allocator、driver、shader、基础 framebuffer 或 reservoir。

**8 GiB 三层 1080p：容量余量未获认证。** 虽然三次均完成且未报 OOM，峰值达到 7883/8192 MiB，总帧约 636.0 ms，明显慢于 720p。这个现象提示容量压力；没有采集 WDDM paging 事件，不能据此宣称无换页或适合实时使用。此项是明确的交付限制。

本次 `corona_engine` 使用仓库规定的 CLion RelWithDebInfo/MSVC 14.44.35207 配置构建成功，可执行文件存在。Horizon 固定在 `2607a3b380e526fb3c319168493119d9b93285d3`，共享 checkout 无源码改动。场景资产、随机流、分辨率、配置、命令、源码状态摘要、renderer 二进制指纹和显存时间序列均保存在本 worktree 的 `cmake-build-relwithdebinfo/relax-evidence`。

## 636 ms 的根因与优化（2026-10-16）

这次容量修复位于 `da798309`；本节性能为后续逐 pass 优化之前的基线。保持 1920×1080、三层、相同随机流、材质、RELAX 默认参数和 FP32 精度。

根因是总工作集超过这张 8 GiB 卡的可用本地显存，导致多个渲染阶段访问非本地内存；不能把 stable build 或 DI/GI 变慢直接解释为它们的计算量增加。逐 pass 同步也不是主因：优化前取消阶段同步仍为 674.444 ms。

| 同一 Kitchen 对照 | 总帧 ms | 进程提交 MiB | 进程共享/非本地峰值 MiB |
|---|---:|---:|---:|
| baseline1080 | 675.834 | 7864.7 | 842.0 |
| raw1080 | 57.212 | 6193.3 | 106.0 |
| relax900 | 86.070 | 5653.3 | 106.0 |
| batched1080 | 674.444 | 7864.7 | 810.0 |
| noPlanes1080 | 73.743 | 4095.3 | 106.0 |
| 优化后 1080p（三次平均） | 112.291 | 6929.3 | 106.0 |
| 优化后 1080p（批量提交） | 101.754 | 6929.3 | 106.0 |

`baseline1080` 为本次优化前复现；`raw1080` 关闭降噪，`relax900` 为 1600×900，`batched1080` 关闭阶段同步，`noPlanes1080` 关闭 stable planes。关闭降噪还会改变 film jitter/coverage history，所以 raw 是辅助对照；保持相同设置且图像逐位一致的优化前后对照是主要依据。106 MiB 是该运行时在这些对照中已有的共享/非本地基线，不能称为零共享内存。

原移植报告的 636.019 ms 是三次平均；本次同配置复现为 675.834 ms。以下按本次复现与优化后三次平均拆分，避免把 DI/GI 的合计再次叠加：

| 阶段 | 优化前 ms | 优化后 ms |
|---|---:|---:|
| Stable build | 280.968 | 13.193 |
| Stable fill | 45.739 | 2.344 |
| ReSTIR DI | 93.863 | 11.089 |
| ReSTIR GI | 152.092 | 26.147 |
| Combine | 24.852 | 2.042 |
| RELAX + guides + coverage | 61.157 | 47.393 |

实现参考 RTXPT 固定版本的紧凑 stable-plane 记录，以及 NRD 4.14.3 的紧凑永久池/临时池原则。首先移除我们记录中的 float3 padding：Ocarina 的 float3 占 16 字节，三元素 FP32 array 占 12 字节；读取后用 `.as_vec3()` 恢复向量运算，写入用 `.set()`。没有删除字段或改成 FP16。

| 记录 | 原字节 | 当前字节 | 每像素份数 |
|---|---:|---:|---:|
| SurfaceData | 192 | 160 | 2 |
| SurfaceExtend | 80 | 44 | 2 |
| StablePlaneData | 336 | 256 | 3 |
| StablePlaneHistory | 128 | 96 | 3 |

合计减少 472 bytes/pixel，即 1080p **933.398 MiB** 的逻辑 buffer；进程提交量实测下降约 935.4 MiB。RELAX 自身 Filter/producer/coverage 的逻辑账本不变，减少的是上游几何/transport 记录。

验证：尺寸回归先失败后通过；GPU roundtrip 对所有紧凑字段及历史检查 signed zero、1e-30 和超过 FP16 范围的 100000，逐位保留。18/18 相关回归通过；引擎按 CLion 配置构建成功；独立审查无待处理问题。64/64 评估任务重新通过，8/8 时间噪声检查低于 raw。

图像对照共 520 个 PFM，其中 377 个逐位相同，包含 Kitchen 1080p 全部性能图像。部分 ReSTIR 图像存在微小数值差异：filtered 图像最大整图 RMSE 5.61e-08，所有逐 ROI 质量 RMSE 指标变化最大 1.65e-08；raw 图像最大整图 RMSE 1.11e-05（最高亮差异像素约 112.95145→112.94916）。没有将这些图像宣称为逐位相同。同一新二进制重跑 glass-motion/raw、materials-rt/ReLAX 全部 PFM 逐位一致；原始差异和重复性记录均保留。对照门槛为 filtered RMSE/质量指标变化 <1e-6、raw RMSE <1e-4，并同时保留未通过全图逐位一致检查的记录。

计时为含提交/同步的 host wall time；Windows GPU Process Memory 的 Dedicated/Shared/Non Local/Total Committed 按渲染 PID 采样。106 MiB 基线的成因未细分，也没有采集换页事件次数。结论限本机、本场景与记录的工作负载；其他进程占用改变仍可能重新触发显存压力。

**升级需要重启渲染进程**，使用重建后的二进制重新分配 buffer；不支持把旧布局的 GPU buffer 跨二进制版本热恢复。

本次原始命令、配置、逐阶段日志、PID 内存时间序列、对照脚本及 `performance-summary.json`/`image-comparison.json` 保存在本 worktree 的 `cmake-build-relwithdebinfo/relax-performance`；新完整评估位于其 `evidence-after`，旧 `relax-evidence` 保留为基线。

## RELAX 逐 pass 优化（2026-10-17）

代码提交 `5d4d76f8`。同一 Kitchen、1920×1080、三层 stable planes，RTX 3070 Ti 8 GiB。保持默认五轮 À-trous、所有滤波参数、随机流、材质、FP32 信号和历史。

**最终无并发负载的三次性能复测待完成。** 原 HUD 版本三次平均 102.842 ms（9.72 FPS）。Pack 合并候选在外部负载出现前单次批量提交为 **85.979 ms（11.63 FPS）**，关闭两种 profiler，Kitchen PFM 逐字节一致。该候选早于最后的 stable-plane 有效性读取优化和首轮 dead-code 清理，不能将它当作最终提交的三次平均 FPS。

每次预热 8 帧、测量 16 帧。下表是原版三次平均与 Pack 合并候选单次逐 pass profile 的实验对照，均对三层求和，含提交/同步的 host wall time。候选该次整帧为 403.647 ms 的上游异常值，但下列降噪区间为 31.512 ms；其后的正常批量提交单次为 85.979 ms。表格不代表最终提交的三次复测，也不是 GPU timestamp。

| 阶段 | 原版三次平均 ms | 候选单次 ms |
|---|---:|---:|
| Guide rays + Pack | 6.146 | 5.419 |
| ClassifyTiles | 1.349 | 0.000 |
| PrePass | 7.157 | 3.202 |
| TemporalAccumulation | 3.658 | 3.310 |
| HistoryFix | 1.986 | 1.681 |
| HistoryClamping（优化后含永久 illumination history） | 2.672 | 3.196 |
| PublishHistory（已并入 Clamping/首轮 Atrous） | 3.558 | 0.000 |
| AtrousFirst（优化后含 geometry history） | 2.226 | 2.424 |
| Atrous2 | 1.825 | 1.424 |
| Atrous4 | 2.010 | 1.626 |
| Atrous8 | 3.310 | 1.660 |
| Atrous16（优化后含 Unpack） | 5.027 | 2.239 |
| Unpack（已并入末轮 Atrous） | 1.354 | 0.000 |
| Coverage | 5.288 | 5.332 |
| 合计 | 47.569 | 31.512 |

合并后的行包含额外写回，不能仅把该行的增加解释为算法变慢。JSON 同时保留原始 pass 名、每层数据和每次重复，便于重新求和。

实现依据固定 RTXPT/NRD 4.14.3 的分离 guide 资源和永久/临时池调度：

- 当前 Guide 改为七列连续 float4：同样 112 bytes/pixel，identity 位转换，motion 保留 FP32。相邻线程读取相邻同类字段；不使用 FP16 或删除 guide 字段。
- 后续 Atrous 的 diffuse/specular 共用八个邻域 tap 的 geometry/ownership 读取，保留各通道原来的算术、tap 顺序与平方权重方差传播。
- HistoryClamping 写永久 slow/fast history；启用 AntiFirefly 时由它写最终 slow 和已夹紧 fast/meta。首轮 Atrous 写 geometry，末轮直接重调制输出，省去单独 PublishHistory/Unpack。
- Guide 生成直接解调到既有 scratch，并在读取完整 stable-plane payload 前判断有效性。删除没有消费者的 ClassifyTiles。重建模式、反萤火虫和 2–8 轮路径保留。

固定 16×16 和 16×8 launch 的试验没有稳定收益，均已撤回。NCU 硬件计数器返回 ERR_NVGPUCTRPERM；没有修改系统权限，也没有据此声称实际占用率、DRAM 带宽或迁移事件。

当前 Filter 是共享 scratch 256 bytes/pixel（已含独立 depth anchor）+ 每层 geometry/history 128 bytes/pixel。1080p 三层 Filter 1265.625 MiB，连同 producer/coverage 逻辑 buffer 共 2032.910 MiB；比此前只减少未使用的 32640-byte tile buffer，主要收益来自访问和调度。

验证：CLion 配置的引擎构建成功，18/18 回归通过；独立源码审查无待处理问题。64/64 评估任务通过，8/8 时间噪声检查低于 raw。对照上一版容量优化后的 520 个 PFM，520 个文件逐字节一致，filtered 最大整图 RMSE 0，全部质量 ROI 指标变化最大 0，全部图像最大 RMSE 0。另有 5 次 Kitchen 1080p 诊断的图像比较。52 个质量任务包含 320×240 静态/运动序列，12 个渲染任务覆盖 720p/1080p PT/三层 ReSTIR；后者在 AccuRC 并发期间运行，耗时不用于正常负载的性能结论。

原有 materials/glass 相对 SVGF 的误差、镜面边缘和运动恢复限制仍保留在上方报告；本轮只验证当前画质保持，不声称消除了原有质量缺点。

异常结果同样保留：中间候选一次 profile 整帧 403.647 ms，但 RELAX+guide+coverage 为 31.512 ms，主要拖慢发生在上游；缺乏迁移事件证据，原因未定。最终首次复测时 AccuRC 2 于 01:28:37 启动，占用约 639 MiB dedicated，渲染进程 Shared/Non Local 升至 392 MiB，得到 1363.810 / 1359.600 / 673.817 ms，第四次未完成即停止。它们作为并发负载结果单列，不纳入相同显卡占用条件的三次对照。没有关闭或修改用户程序。

完整记录位于本 worktree 的 `cmake-build-relwithdebinfo/relax-performance`：`relax-passes-summary.json`（原始逐层/逐 pass、全部重复和异常）、`relax-passes-image-comparison.json`、`relax-passes-evidence/report.json`（64 个任务）、`relax-passes-diagnostics`（构建/测试/审查/计数器日志和复现脚本）。前一版 `evidence-after` 和全部中间实验保留。

**需重启渲染进程加载新二进制与 Guide 布局。** 当前仍约 7 GiB 进程工作集，其他程序占用显卡可能重新引入容量压力；本轮没有降低分辨率或层数来回避该限制。
