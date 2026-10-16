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
有效黑色样本保留正距离；miss 使用场景尺度的有效距离。材质按 RGB BSDF 拆分，
方向相关的 diffuse/specular factor 用于解调与重调制。缺失 lobe 的 guide ray
使用独立维度 1024，不向原 estimator 再贡献能量。primary sky/emission 走 bypass。

执行顺序：Pack → ClassifyTiles → 可选 HitDistReconstruction → PrePass →
TemporalAccumulation → HistoryFix → HistoryClamping → 可选 AntiFirefly →
PublishHistory → AtrousFirst → À-trous → Unpack。

各层保留私有几何、slow/fast RGB、二阶矩、年龄和 reflection tracking；临时
buffer 跨层顺序复用。发布历史发生在空间滤波之前，coverage 只作用于最终完整
像素，均不回流 RELAX illumination history。FrameBuffer 的中性 `denoiser_*`
原始 lobe buffer 在 Pack 冻结输入后复用为输出，避免另分配输出副本。

`position.w` 为正的轴向 viewZ，stable plane 的 viewZ 来自 `depth_position`，
virtual position/normal 用于虚拟运动。motion 为 current−previous raster pixels。
Ocarina 插值顺序是 `lerp(t,a,b)`；同类型 swizzle 赋值可能覆盖 w，因此保留矩的
RGB 写入使用显式 float4。12-tap Catmull-Rom 按 NRD 省略四角并重新归一化。

## 验证与复现

CUDA 后端验证覆盖 sanitization、黑色距离、解调、采样、prepass/reconstruction、
surface/virtual reprojection、短历史修复、亮灭突变、firefly、二阶矩及方差传播、
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

## 实测结果（2026-10-16）

设备：NVIDIA GeForce RTX 3070 Ti, 8192 MiB, 610.88。17 项相关 CTest 回归通过；64 个渲染任务均完成；8 组静态/停止尾段时间噪声均低于 raw。

第 63 帧全图线性 RMSE（两组独立 2048 spp PT 参考的平均）：

| 场景 | raw | SVGF | ReLAX | 两参考差异 RMSE |
|---|---:|---:|---:|---:|
| kitchen-pt | 1.287444 | 0.714600 | 0.495288 | 0.040920 |
| kitchen-restir | 2.480845 | 0.359443 | 0.335028 | 0.040920 |
| materials-pt | 0.452840 | 0.034600 | 0.041681 | 0.017229 |
| materials-rt | 0.548848 | 0.059684 | 0.085181 | 0.017229 |
| glass-pt | 0.471450 | 0.031970 | 0.040018 | 0.017589 |
| glass-rt | 0.325003 | 0.043455 | 0.058431 | 0.017589 |

静态第 48–63 帧的逐像素 RGB 时间标准差，再对全图取平均：

| 场景 | raw | SVGF | ReLAX |
|---|---:|---:|---:|
| kitchen-pt | 0.587565 | 0.004901 | 0.005921 |
| kitchen-restir | 0.157906 | 0.005428 | 0.005303 |
| materials-pt | 0.225043 | 0.000495 | 0.000920 |
| materials-rt | 0.197549 | 0.001251 | 0.001748 |
| glass-pt | 0.247120 | 0.000576 | 0.000974 |
| glass-rt | 0.222532 | 0.001106 | 0.001634 |

移动/停止的同姿态全图线性 RMSE；第 47 帧停止，第 63/95 帧观察恢复：

| 场景/帧 | raw | SVGF | ReLAX |
|---|---:|---:|---:|
| glass-motion/31 | 0.614812 | 0.046802 | 0.081756 |
| glass-motion/47 | 0.521737 | 0.045384 | 0.073404 |
| glass-motion/63 | 0.303397 | 0.048177 | 0.057485 |
| glass-motion/95 | 0.448547 | 0.049224 | 0.061751 |
| kitchen-motion/31 | 1.016102 | 0.432686 | 0.435141 |
| kitchen-motion/47 | 1.612223 | 0.477759 | 0.438490 |
| kitchen-motion/63 | 0.936661 | 0.324559 | 0.279734 |
| kitchen-motion/95 | 1.191564 | 0.268791 | 0.224801 |

退化项：materials/glass 的静态全图及 compound/glossy/mirror-edge ROI 多数高于 SVGF；玻璃运动序列也高于 SVGF。Kitchen PT 的时间噪声略高于 SVGF，但全图误差更低。线性 RMSE 将偏差、模糊和残余噪声混合计量，不能将差值全部解释为噪声；镜面细节、轮廓模糊和停止后残影需同时看 PNG/PFM。完整逐 ROI 数据、对应姿态及不利结果均保留在 `relax-evidence/report.json`。两参考差异非零，小于此量级的画质差异不作确定性优劣结论。

### 性能与容量

同一 Kitchen，相同设置；每组 3 次平均（每次预热 8 帧、测量 16 帧）。总帧为同步测量模式，含引擎其他阶段和计时开销。

| 路径/分辨率 | 总帧 ms | RELAX+guide+coverage ms | buffer 逻辑 MiB | device-wide 峰值 MiB | RELAX CPU 编译 ms（PTX cache 已热） |
|---|---:|---:|---:|---:|---:|
| pt-1280x720 | 20.440 | 9.661 | 548.451 | 1318 | 529.323 |
| rt-1280x720 | 57.994 | 20.126 | 889.467 | 4044 | 519.809 |
| pt-1920x1080 | 41.975 | 22.349 | 1234.016 | 2120 | 530.233 |
| rt-1920x1080 | 523.504 | 45.471 | 2001.301 | 7890 | 526.277 |

逐 pass 同步耗时（ms，ReSTIR 对所有层求和）：

| 阶段 | PT 720p | 三层 720p | PT 1080p | 三层 1080p |
|---|---:|---:|---:|---:|
| PrepareGuidesIncludingRays | 0.418 | 2.610 | 0.842 | 5.082 |
| Pack | 0.307 | 0.691 | 0.628 | 1.496 |
| ClassifyTiles | 0.237 | 0.727 | 0.607 | 1.892 |
| PrePass | 1.660 | 2.071 | 4.245 | 5.325 |
| TemporalAccumulation | 0.990 | 1.536 | 2.396 | 3.587 |
| HistoryFix | 0.354 | 0.964 | 0.684 | 2.032 |
| HistoryClamping | 0.921 | 1.314 | 1.980 | 2.889 |
| PublishHistory | 0.522 | 1.539 | 1.114 | 3.376 |
| AtrousFirst | 0.534 | 0.991 | 1.110 | 2.009 |
| Atrous2 | 0.392 | 0.788 | 0.856 | 1.685 |
| Atrous4 | 0.512 | 0.885 | 1.074 | 1.886 |
| Atrous8 | 0.740 | 1.124 | 1.890 | 2.760 |
| Atrous16 | 1.081 | 1.479 | 2.848 | 3.792 |
| Unpack | 0.302 | 0.693 | 0.637 | 1.450 |
| PublishPT | 0.172 | — | 0.339 | — |
| Coverage | 0.520 | 2.714 | 1.100 | 6.210 |

buffer 账本：Filter 为共享 scratch 240 bytes/pixel + 每层 history/geometry 128 bytes/pixel，另加每 16×16 tile 的 uint。producer 当前固定分配 192 bytes/pixel；coverage-only 为 PT 64、guided ReSTIR 196 bytes/pixel。输出与输入 alias 不重复计数。Filter 和 producer 字节从实际 buffer 大小求和；coverage 数字按已分配的字段/分辨率计算，不包含 allocator、driver、shader、基础 framebuffer 或 reservoir。

**8 GiB 三层 1080p：容量余量未获认证。** 虽然三次均完成且未报 OOM，峰值达到 7890/8192 MiB，总帧约 523.5 ms，明显慢于 720p。这个现象提示容量压力；没有采集 WDDM paging 事件，不能据此宣称无换页或适合实时使用。此项是明确的交付限制。

本次 `corona_engine` 使用仓库规定的 CLion RelWithDebInfo/MSVC 14.44.35207 配置构建成功，可执行文件存在。Horizon 固定在 `2607a3b380e526fb3c319168493119d9b93285d3`，共享 checkout 无源码改动。场景资产、随机流、分辨率、配置、命令、源码状态摘要、renderer 二进制指纹和显存时间序列均保存在本 worktree 的 `cmake-build-relwithdebinfo/relax-evidence`。
