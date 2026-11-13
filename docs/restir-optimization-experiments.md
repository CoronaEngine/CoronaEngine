> 注：文中引用的 JSON 结果、`native_nrd.*` 等是当时的临时实验产物，未随仓库保存；NRD 相关代码已不在当前树中。

# Corona ReSTIR 优化实验总账

更新：2026-11-01。记录 Corona ReSTIR DI、GI、Stable Planes（SP）及相关缓冲的优化方法、独立对照、失败原因与验收状态，用于避免重复实验。

本页自包含必要的方法、数值和结论。原始图像、FP32 数据、逐帧日志与完整实验快照单独归档，未纳入本页；本文不依赖工作机路径或被忽略的分项报告。文档提交不代表下文列为未提交或待验收的渲染代码已经合入。

## 当前路线

当前保留原 DSL ReSTIR 路线，以及已接受的 DI 面积灯端点复用、零半径回退和 DI／GI／SP 压缩优化。Principled 的常量 lobe 裁剪已在 `1c9b2bc4` 提交，材质契约和调试行为见 [Principled 与 Stable Plane 说明](principled-stable-planes.md)。

独立 RTXDI 风格渲染器及 GI HLSL 实验路线未采用，专用实现已移出工程。分项实验报告、执行计划和进度文档已删除；未采用方案只在本页保留必要结论。历史性能数据仍以各节的版本和测试条件为准，不代表当前版本性能。

## 结果怎么读

- 每行A→B只比较该项自己的对照与候选，不跨轮次相减、相加或拼接最佳值。时间单位ms，容量单位MiB。
- 时间为纯渲染kernel GPU求和，排除降噪、后处理、CPU/帧开销、等待，以及单列的history/clear housekeeping。阶段下降不等于整体同比下降。
- 历史表中的“保留”表示该轮接受的实现，代码是否已提交另行注明；“撤回”表示该轮结束时不保留该候选。诊断、仅审计、待验收和条件未满足均不计作成功优化。保存实验快照不等于候选已获接受。
- 两对AB/BA通常只是筛选。没有建立收益、没有证明等效、已经证明退化是不同结论；不把区间跨零写成性能等效，也不把逐位测试失败直接写成可见画质下降。
- 常用设置：Kitchen 1920×1080、1 spp、ReSTIR SP3，最少完成5次反弹后走Russian Roulette（RR）、最多16次、RR阈值1，DI10个灯光候选+1个BSDF候选，DI空间1/GI空间4。各轮保持对应预算、MIS、可见性及随机数调用要求。后续验证排除Hero，历史覆盖记录不改写。
- raw图像指关闭降噪后导出的线性FP32 RGB；底层raw输出缓冲可以采用压缩存储。ReBLUR对照的FP32图像为降噪后的线性输出，两类证据分开记录。
- 图像相对RMSE = 两图RMSE / 基线图均方根；它衡量差异，不是相对真值的画质下降比例。PNG最大差按0～255通道码值表示，变化像素比例指任一通道码值不同的像素比例。
- 最新有损候选由用户看图确认视觉可接受性，单一数值门槛不自动决定接受或拒绝。

## DI 初始采样与时间复用（2026-10-26）

每项3A/3B，均有六套回归、12张静态/运动raw FP32精确比较及候选/reservoir/RNG证据。

| 方案 | 阶段A→B | 全渲染A→B | 结果/原因 |
|---|---:|---:|---|
| 面积灯端点上下文复用，省去辐射与PDF之间重复几何重建 | 4.451708→4.141143 | 36.110616→35.784342 | 保留；独立复现收益 |
| 时间复用历史view按需加载（lazy） | 4.126435→4.110662 | 35.769948→35.822253 | 撤回；小差异与噪声重叠，整体未受益 |
| 零半径回退快路，保留原随机数消耗 | 4.128267→3.912287 | 35.800526→35.526075 | 保留；省去重复同像素gather/validation |
| lazy+zero组合 | 3.943127→3.919514 | 35.626657→35.532968 | 撤回lazy增量；重复结果不稳定 |
| temporal target-only/F-only，保留四次目标求值与MIS | 3.907408→3.928164 | 35.542468→35.479982 | 撤回；目标阶段无稳定收益，baseline已有相关DCE |

当轮保留area+zero相对最初对照：阶段下降12.117%、全DI下降7.502%、全渲染下降1.619%；不能相加两项独立百分比。合并提交记录：`98db6133`。

## 整体渲染与早期GI尝试（2026-10-26）

| 方案 | 实际执行/证据 | 结果/原因 |
|---|---|---|
| SP确定性lobe计算延后到guide/albedo之后 | 4A/4B；SP build4.789635→4.349479；完整SP payload/RNG与12raw一致 | 该轮保留在 `stable_planes.cpp` 的未提交渲染改动中；阶段收益9.190%，整体稳定收益未证实；无新增容量 |
| 遮挡时跳过direct-light BSDF evaluate | mapped Principled的sample方向差1ULP | 撤回；未通过当轮精确门槛，未进入有效性能验收 |
| GI initial有效性判断后再重建receiver | 4A/4B；initial14.301834→14.046861；全渲染37.107781→37.216061 | 撤回；画质通过，但阶段两快两慢、整体未受益 |
| GI receiver几何共享（早期方案） | 已绑定历史测慢证据；这一轮只准备测试，没有新生产变体 | 取消重复试验；不要与后来的空间S1混为同一个实验 |
| GI temporal按需加载payload/view | 6A/6B；temporal2.231649→2.030180；全渲染37.338195→37.485336 | 撤回；阶段均值受末组A尖峰影响，整体收益未建立；全部有效样本保留 |

历史复核还包含已拒绝的GI initial 32×1→8×4、8×8布局、大范围evaluator缓存、终止循环共享、deferred visibility等方案。本总账不补造其单项耗时，也不把“历史复核”计作本轮重新实施。

## 无损缓冲布局（2026-10-26～27）

每项六对AB/BA。下列节省量为单项候选相对各自对照的容量；撤回项不能计入保留收益。

| 方案 | 候选独立节省 | 全渲染A→B | 结果/原因 |
|---|---:|---:|---|
| DI reservoir 56→32B、两个切片 | 94.921875 | 35.544→34.459 | 保留；六对均加快，约3.05%整体收益；提交`3b3a2c18` |
| GI scratch scalar布局 | 79.101563 | 35.024→35.289 | 撤回；画质/容量通过，性能不确定 |
| GI scratch bulk布局 | 79.101563 | 35.229→35.251 | 撤回；未建立性能接受条件 |
| SP256→232B | 142.382813 | 35.418→35.392 | 撤回；性能不确定 |
| SP232平铺/连续写入修正版 | 142.382813 | 35.486→35.791 | 撤回；整体慢化，配对95%区间为正 |
| Surface152 | 31.640625 | 35.660→36.846 | 撤回；整体慢化，主要增加SP build成本 |
| DI/GI共用scratch | 计划额外63.281250 | 未运行 | 未实施；前置GI scratch未被接受 |

## DI raw 输出压缩（2026-10-27）

实际完成并保留的是`rgb21v3`：signed RGB21×3加valid位，输出16→8B/像素；不是另一次独立throughput压缩。

| 范围 | 对照→候选 | 判定 |
|---|---:|---|
| 1080p DI输出allocation | 31.640625→15.820313MiB | 减少15.820313MiB（50%） |
| DI独占缓冲合计 | 158.203125→142.382813MiB | 无全屏解码暂存；降噪开启时保留float4 |
| 六对全渲染kernel | 34.550480→34.526379ms | 95%差值区间[−0.111246,+0.063043]，速度未确定 |
| 全DI | 5.695304→5.690773ms | 区间跨零，不计为已证实加速 |
| 质量 | 12raw通过量化门槛，后续432对检查点文件无新增NaN/Inf | 按容量和质量收益保留；已提交并推送 `eb4070a3` |

FP32×3、half4、half+公共指数、R11G11B10、RGB9E5只完成过CPU格式比较，未作为独立GPU候选完成验收。RTXPT throughput约7.910156MiB属于历史参考模型，不与Corona radiance输出压缩混记。

## GI 有损存储压缩（2026-10-27～28）

三项独立对照；未构造未通过候选的组合。

| 方案 | GI容量A→B | 时间和画质证据 | 结果 |
|---|---:|---|---|
| O：raw输出float4 16B→signed RGB21 uint2 8B | 316.406250→300.585938 | Kitchen全渲染36.368806→36.342766；Kitchen6对、glass/bathroom各3对区间均跨零；输出误差、sample/reservoir、NaN/Inf、完整512帧尾段和回归通过 | 保留；省15.820313MiB（5%），速度不确定 |
| N：法线压缩 | 316.406250→268.945313 | 存储/核心回归通过；三场景多seed静态画质非劣性门槛未通过 | 拒绝；节省不计入最终收益，未继续完整性能验收 |
| L：缓存Lo压缩 | 316.406250→292.675781 | 存储/核心回归通过；多seed静态画质非劣性门槛未通过 | 拒绝；节省不计入最终收益，未继续完整性能验收 |

N/L未过非劣性门槛不等于已经统计证明可见画质退化。O的5%节省适用于raw模式；启用降噪器时保留float4和方差，GI容量回到316.406250MiB。截至本文更新，O仍为保留的未提交渲染改动。

## GI 初始采样（2026-10-28）

| 方案 | 对照/实际执行 | initial A→B | 全渲染A→B | 结果 |
|---|---|---:|---:|---|
| C1：遮挡后省去NEE求值 | 实现、编译、局部oracle与单次成本调查；无有效配对 | 候选单次14.070 | — | 撤回；mapped Principled逐位门槛失败（约1ULP方向变化），未证明可见画质下降，也未完成图像验收 |
| C2：先完成直接光贡献，再采样BSDF | 两对AB/BA，对原基线 | 14.538→14.639 | 36.137→36.336 | 撤回；质量通过，无可复现收益 |
| C3：GI initial工作组32×1→64×1 | 两对AB/BA，对原基线 | 13.865→14.784 | 35.832→36.564 | 撤回；质量通过，两对目标阶段及总计均更慢 |
| R1：DI首次命中供GI复用，原点逐位相等才跳过query | 两对AB/BA，对原基线 | 14.399983→14.191807 | 36.132873→36.090221 | 撤回；质量通过，两对整体方向相反，无可复现净收益 |
| R2：直接续接DI保存的射线，省GI receiver重建和原点比较 | 两对AB/BA，**对R1r，不是原基线** | 14.231405→14.339589 | 35.893476→36.377170 | 已实施并完成增量筛选，该轮结束时撤回；质量通过，未取得性能接受 |

C2/C3/R1/R2分别有相关六项回归、三场景21张静态/运动raw FP32、372份debug、12组GI sample/reservoir及24份NaN/Inf检查记录；保留原预算和300.585938MiB GI容量。R2的A是R1源码的重新构建运行别名R1r，不可把其时间称为原始baseline。R2两对全渲染差值+0.786207/+0.181181ms，95%区间跨零；未完成相对原baseline的正式接受比较。

R2的生成代码已确认移除了预循环receiver重建；代码工作减少未转化为可接受的GPU性能收益。

## GI 空间复用与最终着色（2026-10-28）

| 方案 | 空间+着色A→B | 全渲染A→B | 验证与结果 |
|---|---:|---:|---|
| S1：共享接收点几何，复用/着色各保留独立frame | 6.702227→7.767721 | 36.003057→36.415147 | 两对AB/BA；21raw、372debug、sample/reservoir/RNG通过；未建立收益，撤回 |
| S2：只读材质的局部evaluator复用，保留全部目标/支持度求值 | 6.927296→7.572481 | 36.318195→36.676026 | 两对AB/BA；同样质量矩阵及12case冻结reference oracle通过；无可靠收益，撤回 |

S1 native代码213376→208000B、shared6912→4864B；S2 native代码213376→337152B，实有7份材质topology循环及fallback。两者registers/thread仍为128；不据静态资源推断动态occupancy/spill。GI容量不变；S2结论仅覆盖该轮材质矩阵及12个冻结reference用例，不作全材质证明。

| 诊断/审计，非接受的优化 | 已做工作与结果 | 状态 |
|---|---|---|
| D0：选择+支持度与最终着色拆成两个dispatch | fixed100精确；两次诊断选择+支持度约6.49～6.50，着色约0.49，合计约6.99ms | 仅定位大头，改变代码生成和读写；未证明整体收益，已移除 |
| D1：支持度、遮挡与零权重计数 | frame127支持度BSDF有效后遮挡约1.18%，最终着色零权重约0.11% | 64B临时计数器已移除；计数不能换算GPU时间比例 |
| RTXPT selectedTargetPdf/最终求值直接复用 | 审计发现Corona Principled构建会修改frame，最终着色使用独立新frame | 未实施无条件缓存，避免改变语义 |
| 新三路selection/support/shading拆分 | 审计发现跨dispatch重建frame不保留原Principled演化链 | 未作为新候选执行；已执行的是D0两路诊断 |

## GI空间轮次结束记录（2026-10-28）

该轮失败/诊断改动均已撤回，原有786份源码、代码基点和保留改动精确恢复。该批三对纯GPU render：PT17.307811ms、ReSTIR36.287153ms、全DI5.947804ms、全GI23.313087ms，其中GI空间+着色6.719600ms；ReSTIR/PT=2.096577×，整体1.6～1.7×未达到。旧2.040343×属于另一批次，不能与之直接相减宣称代码退化。

该轮性能采集未记录或对齐实际RNG采样帧；已检查计时绑定和原始GPU ticks，部分未改阶段的跨进程漂移仍未解释。这一限制属于该轮性能对照，不等同于后续ReBLUR固定帧配对缺失身份核验。Nsight硬件计数器不可用，没有动态occupancy或spill实测结论。

## SP端点guide延迟计算（2026-10-28，T1）

三对独立AB/BA/AB、每次120预热+220测量，只统计渲染kernel；未重测PT/RTXPT分母。

| 候选 | SP Build A→B | 全渲染A→B | 结果 |
|---|---:|---:|---|
| T1：前看下一段命中，延迟guide/albedo/motion，保留miss/无材质继承 | 4.484158→4.658614 | 35.178908→35.428776 | 无可复现收益，撤回；两对变慢、一对变快，95%配对区间跨零 |

新Kitchen fixed100 raw逐字节相同；两项真实SP oracle中，多跳→天空完全一致，另一项4条路径的累计距离及派生深度存在1ULP（最大2.384e-7）差异，RNG/分支/throughput/guide/运动向量一致。未证明可见画质退化，也未完成全场景质量接受；此次计时始终标为非接受诊断。SP/DI/GI缓冲容量不变。实际SPIR-V query静态位置3→11、native361472→432256B、shared19200→25344B，registers仍255；不能由此推断spill/occupancy。只撤回T1自身的SP源和两个测试文件改动，原有保留工作未被覆盖；T1渲染候选未提交。

## Half存储压缩独立实验（2026-10-29，H1/H2/H2b）

三项均以同一份冻结的未提交代码基线为起点：提交 `eb4070a3` 加当时保留的GI raw输出压缩、SP构建和测试改动。H1、H2、H2b分别测量，没有叠加或组合。每项三对独立进程，顺序AB/BA/AB，每个进程120帧预热、220帧测量；一个进程贡献一个均值，220帧不视作220次独立实验。表中95%区间来自三对进程差值，候选减基线。

| 候选 | 改动 | 独立容量节省 MiB | 全渲染A→B ms | 总差95%区间 ms | 状态 |
|---|---|---:|---:|---|---|
| H1：材质guide half | 两组RGB/roughness guide压成half，读取后FP32计算；Surface160→144 B、SP256→240 B | 158.203125 | 35.421818→35.030073 | [−0.882758,+0.099269] | 仅保留独立快照；已展示raw和ReBLUR图，待用户视觉验收 |
| H2：motion/flags | guide仍FP32，SP motion存half2，两个uint逻辑标志合并为flags；SP256→240 B | 94.921875 | 35.374435→35.470804 | [−0.662495,+0.855232] | 历史候选；全局motion被不必要量化的问题已由H2b修正 |
| H2b：保留全局FP32 motion | 路径结束后打包SP内部motion；既有全局motion缓冲导出原FP32结果，其他布局沿用H2 | 94.921875 | 35.371793→35.276905 | [−0.712651,+0.522874] | 工作区中的未提交候选，待用户视觉验收 |

三者总时间区间均跨零，未建立整体加速或减速，也不是等效证明。各行使用各自独立批次的对照，不跨行相减比较候选速度。容量来自原生核心缓冲分配，不是整卡驻留峰值；已有raw模式DI/GI输出8 B、GI独占300.585938 MiB保持，降噪模式GI输出仍为16 B。

画质和回归证据：

- H1三场景raw均无新增NaN/Inf；玻璃图像一致，Kitchen和Bathroom静态最终线性相对RMSE分别为0.08324%和2.99571%。这些数值不是画质下降比例。Kitchen运动最终检查点相对RMSE为0.03036%；ReBLUR结果见下一节。
- H2静态raw一致，但运动图像出现差异；H2b消除了全局motion的half往返后，Kitchen、glass_of_water、bathroom2共40张静态/运动raw图逐字节相同。
- H2b另有Kitchen的10个SVGF运动检查点：线性相对RMSE峰值0.129459%，PNG通道差峰值57/255，变化像素比例峰值0.729215%；三项峰值位于不同帧。最终pose64相对RMSE0.002138%、PNG最大差3/255。未自动接受或拒绝。
- H1与H2b选定的核心6项回归均通过。额外Embedded模式切换测试的“dolly中心射线命中天空”断言在正常重编的原始基线上也复现，未修复或跳过，不能称全部回归通过。
- 实际源码、完整207项运行文件及生成shader身份均有绑定。测试覆盖完整部署与新进程运行；任意局部热重载未验收，H1附加ABI守护草案未应用。
- H2b的DI/GI shader未改；SP build寄存器数255不变、shared19200→18672 B、native代码361472→369792 B。没有据此推断动态occupancy或spill。

H2b同批次纯渲染kernel均值：

| 阶段 | ms |
|---|---:|
| SP build | 4.275037 |
| 全DI | 6.002579 |
| GI initial | 14.023480 |
| GI temporal | 2.112047 |
| GI spatial＋最终着色 | 7.019425 |
| 全GI | 23.154952 |
| 全ReSTIR渲染 | 35.276905 |
| PT渲染 | 17.088104 |

ReSTIR/PT均值之比为 **2.064413×**，整体 **1.6～1.7×目标未达到**。本阶段确认了容量节省，候选整体性能收益仍不确定，视觉接受仍待确认；不能与旧批次2.096577×直接相减宣称优化收益。

## H1开启ReBLUR后的运动画质（2026-10-29）

本轮对比原FP32 guide基线与独立H1，不叠加H2/H2b。Bathroom和Kitchen各运行基线/H1两个新进程，每次64帧、10个同帧检查点，共20对。使用当前默认ReBLUR EDSL实现和默认参数，保持1080p、1 spp、SP3、min5/max16/RR1、DI10+1及原复用预算，无额外累积，Hero未测。

每个场景使用自己的原相机；双方应用同一相对平移/转角轨迹，固定时间步1/60秒。保存的零起算frame为0、1、2、15、16、31、32、47、48，另保存最终frame63；对应下表pose为frame+1。相机在frame15/31转向、frame47回到起点，继续观察停止后的历史收敛。轨迹关键点相对起始相机为：

| frame | 位移xyz（场景单位） | pitch增量（度） | yaw增量（度） |
|---|---|---:|---:|
| 0 | (0,0,0) | 0 | 0 |
| 15 | (0.12,0,0) | 0 | 1 |
| 31 | (0,0.04,0) | −0.5 | −1 |
| 47 | (0,0,0) | 0 | 0 |

采集保持同seed、同pose、同曝光和预算，源码/完整运行文件/shader缓存及实际dispatch均经核验。每次主要11级ReBLUR链各执行192次，即64帧×SP3；末端共用SVGF coverage-only是正常组合流程，未执行SVGF主滤波。GPU串行、原benchmark锁、启动≤55°C、70°C守护；150 ms墙钟间隔用于温控，不改变仿真时间步。

所有40张双方FP32图均有限，新增NaN/正Inf/负Inf为0。以下FP32数值属于**降噪后的线性输出**，PNG属于对应色调映射画面；本轮仅做画质验证，不作为新的渲染或降噪性能A/B结论。

| 场景与检查点 | pose | 线性相对RMSE % | PNG最大通道差 /255 | 变化像素 % |
|---|---:|---:|---:|---:|
| Bathroom运动转向；PNG最大差检查点 | 32 | 0.03750655 | 144 | 3.760272 |
| Bathroom线性相对RMSE最大检查点 | 48 | 0.08612762 | 133 | 5.566069 |
| Bathroom最终检查点 | 64 | 0.08255597 | 58 | 5.834925 |
| Kitchen运动转向后检查点 | 33 | 0.00529538 | 4 | 0.103636 |
| Kitchen最终检查点；三项指标均为序列最大 | 64 | 0.01408574 | 12 | 0.235822 |

Bathroom的最大通道差与最大相对RMSE发生在不同帧，不能拼成同一幅图的指标。Kitchen第一帧PNG/FP32一致，其余检查点有差异；Bathroom所有检查点有差异。这些结果只覆盖两场景的该段轨迹，未完成全场景或多seed视觉非劣性验收。

已提供原色完整画面、1:1局部及独立标记的×8绝对差图，包含运动转向、差异峰值和最终帧。**H1仍待用户看图确认，未因数值小而自动接受，也未因有差异而自动拒绝。** 原图和完整逐帧记录另行归档，不将缺失图片的本页当作视觉验收替代品。

## 截至2026-10-29的合入与待验收状态

| 范围 | 状态 |
|---|---|
| DI面积灯端点复用＋零半径回退 | 已提交并推送，`98db6133` |
| DI reservoir 32 B布局 | 已提交并推送，`3b3a2c18` |
| DI raw输出8 B | 已提交并推送，`eb4070a3` |
| GI raw输出8 B（O）与原有SP构建改动 | 工作区保留的未提交渲染改动，未随本文提交 |
| H1材质guide half | 独立实验快照；raw/ReBLUR图已展示，待用户视觉验收，未组合 |
| H2旧motion/flags版本 | 历史独立实验，已由H2b替代工作区部署；容量不重复计入 |
| H2b SP内部half motion＋全局FP32导出 | 工作区中的未提交候选，待用户视觉验收，整体加速未建立 |
| 整体ReSTIR/PT 1.6～1.7× | 未达到；最新H2b批次为2.064413× |

后续重试同一方案，应说明相较历史版本的实质变化，重新做独立A/B与质量验证；不把已撤回候选的容量计入收益，不削减采样或可见性预算。恢复仅使用本轮已核验的冻结文件，保护已有未提交工作。H1与H2b是否接受或组合尚无结论；组合布局需要另行验证。

## 2026-10-30～31：已结束实验结论

- 独立 RTXDI／HLSL 路线涉及不同的材质、路径、Stable Plane 和 MIS 工作量，不能把历史差距归因于语言；相关实验实现均未采用。
- GI 目标求值、PDF 支持 token 和条件合并候选未建立净收益，已撤回；借用 DI 二次选灯仅完成可行性审计，没有保留实现或速度结论。
- DI 局部求值复用、共享 evaluator 和 Diffuse 少量输入缓存均未建立完整渲染净收益，已撤回。通用初始／时间拆分使 Kitchen 退化，Bathroom 的阶段改善不足以证明整体收益，也已撤回。
- Nsight 剖析和固定输入 BSDF 回放仅用于诊断，没有产出保留的渲染优化；完整材质与预解码参考路径的回放成本差异不能直接换算为整帧加速。

上述实验没有撤销已接受的显存压缩和其他生产优化。后续若重试，应说明相对历史方案的实质变化，并在相同配置下重新验证。

## 2026-11-01：Engine 1080p REBLUR 显存容量拐点

Kitchen `scene_v3.xml`、SourceCamera、1920×1080、ReSTIR、SP3、EDSL REBLUR，RTX 3070 Ti 8 GiB、驱动 610.88。引擎界面显示的秒级耗时确实包含 GPU 执行异常，不能仅用历史 kernel 统计与当前墙钟口径不同来解释。

根因在 REBLUR 资源准备：启用 `VISION_REBLUR_NATIVE_NRD` 的构建在 `compile()` 中始终选择 native NRD 或 EDSL，但 `update_resolution()` 和 `set_stable_planes_enabled()` 仍调用 fallback `Filter::prepare()`，分配另一套永远不使用的 history/scratch。首次 dispatch 的 `prepare_native()` 才释放这些缓冲。1080p 一次额外分配约 617 MiB，本次启动经历的两个分辨率累计约 994 MiB。这是无用临时分配，不应称为持续泄漏；在接近显存上限的 Engine 进程中，它触发了驻留/换页的性能拐点。Windows 同时记录到数百 MiB 非本地 GPU 内存。

保留原上传内存策略，只在 NRD/EDSL 构建中直接准备必需的 guide，不申请 fallback history/scratch。未改 shader、采样预算、SP 层数或输出精度。使用分阶段同步计时和 Vulkan dispatch 时间戳，3 帧预热后采 6 帧，完成 A→B→A 回退复现：

| 阶段（ms） | 原版 A1 | 跳过无用分配 B | 回退 A2 |
|---|---:|---:|---:|
| Pipeline 渲染，含降噪 | 1520.94 | 91.44 | 1566.44 |
| SP 构建，含 history/clear | 949.08 | 8.20 | 997.83 |
| 合并阶段合计 | 488.11 | 2.24 | 485.16 |

GPU 时间戳独立确认原版慢在 `Stable planes build` 和 `Denoiser-MergeReSTIRLobes`，不是只有 CPU 等待统计变大。单独把上传 staging 移到系统内存仍约 692 ms，未解决问题，该实验已撤回。关闭降噪但保留 SP3 时约 39.07 ms；720p 开启降噪时约 30.36 ms，均为诊断对照，不是交付设置。

最终关闭阶段同步及 dispatch profiler，8 帧预热后采 16 帧：渲染 **89.56 ms**，其中降噪 **45.13 ms**，总帧 **95.24 ms**。`render_ms` 已包含 `denoise_ms`；总帧还包含调度、互操作和帧间等待，不把三项相加。通过相机截图 API 保存并检查了 1920×1080 Kitchen 输出；该次正常退出、GPU 清理完成，最高 63°C。此前一次采样后的额外画面等待触发 70°C 守护，未把该次结束状态记为通过。

核对 `svgf_magic`：SP 缓冲布局相同，其 Horizon 锁定版本 `448fb71a` 的 Vulkan compute 后端与当前迁移后的后端代码一致（排除 CMake 路径变化）。历史约 35 ms 排除了降噪及 housekeeping；不能直接与当前 UI 总渲染时间比较，也没有证据把本次异常归因于源码目录迁移或 Quill。

验证：CLion RelWithDebInfo `corona_engine` 构建成功；真实 GPU 分配回归检查在原版失败、修复后通过；最终场景采样和截图通过。额外 CTest 回归尝试在首个 `VisionRenderModeConfigTests` 启动时被旧测试二进制阻塞：缺少已移除的 `UpsamplerDesc::init` 入口。已停止该测试进程，未宣称 CTest 套件通过，未为此扩大修改范围。

### 同日补查：45 ms 降噪仍不是正常性能

用户要求继续核对 `svgf_magic` 的 REBLUR。该分支与当前 HEAD 的整个 ReBLUR 目录没有源码差异；工作区仅有上节跳过无用分配的修复。三层调用顺序、EDSL shader 和默认参数没有新增成本。旧分支留下的 `H1reblur_A_kitchen64` 实测整段降噪为 13.6523 ms，包含 SP3 REBLUR、合成及 SVGF coverage-only，不能把当前约 45 ms 用计时范围不同解释。

在隔离目录按 SHA256 恢复 H2bfinal 冻结运行时，当前工作区的源码及部署文件未被替换。使用原 NRD 动态库重新测得 12.2468 ms；进一步对齐当前 SourceCamera 的位置、方向、up、FOV，并临时替换为当前 Engine 的 NRD CPU 调度库，仍为 12.3918 ms。两轮均 1920×1080、16 帧预热、16 帧测量、150 ms 节流，正常退出且进程/GPU 清理通过，最高温度分别 60°C、59°C；隔离运行时的原 NRD 库已恢复。打包 kernel 的 SPIR-V 缓存键与当前 Engine 完全相同。

| 当前机器上的运行条件 | 渲染含降噪（ms） | 整段降噪（ms） |
|---|---:|---:|
| 冻结运行时，原相机/原 NRD，1080p | 51.81 | 12.25 |
| 冻结运行时，当前相机/当前 NRD，1080p | 53.58 | 12.39 |
| 当前 Engine，1080p | 89.56 | 45.13 |
| 当前 Engine，1920×1000 | 54.26 | 12.88 |

最后一行保留相机、算法和参数，只减少 7.4% 像素；3 帧预热后采 6 帧，正常退出、最高 57°C。这种非线性跳变说明上节修复仅消除了秒级 SP/合并异常，当前 1080p 仍存在显存工作集容量/驻留瓶颈，尚未恢复旧分支的正常降噪速度。Windows 计数器也记录到当前进程使用非本地 GPU 内存，但没有资源级换页事件，因此不宣称已定位哪张具体纹理被换出。

GPU dispatch 对照显示额外时间主要落在图像访问阶段：历史 Kitchen 记录的三次 Pack 合计约 0.88 ms，当前约 12.66 ms；主滤波阶段也显著变慢，PrepareGuides 和 coverage-only 没有同幅度增加。降低 Display 到 2 FPS，降噪仍 46.84 ms；保留上一节修复、重新独立测试 host-only upload staging，仍 47.24 ms。两者均不能解决剩余问题；staging 实验已再次撤回并重新构建，不交付全局内存分配策略改动，也不通过降低最终分辨率宣称修复。

### 同日最终修复：1080p REBLUR 滤波恢复到 6 ms 内

继续检查渲染中的实时分配快照，分别记录 Vision compute 与 Engine 原生 graphics/physics 的 VMA 分配。旧运行时也使用渲染中的快照，排除 teardown 后已释放 framebuffer 的记录。导入共享输出只是同一物理分配的另一个视图，不重复计算。

| 资源差异或额外占用 | 实测容量（MiB） | 处理 |
|---|---:|---|
| 未使用的 embedded base `FrameBuffer::rays` | 58.008 | 延迟到真正渲染 base 时分配 |
| 未使用的 embedded base `FrameBuffer::rt_buffer` | 19.336 | 同上 |
| 未使用的 base 1536×825 FP32 display texture | 21.000 | 去掉初始化和首次相机同步产生的分配 |
| 上述无用像素资源合计 | **98.344** | 已由实际 allocation trace 验证消失 |
| 普通源图像相对旧版的格式扩展 | 约 74.38 | 保留；当前 CPU sRGB 解码后的线性 RGBA32F，不能当作无用资源删除 |
| Vision/Engine 共享输出 | 31.641 | 保留；不重复计算原生侧 imported allocation |
| Engine 原生本地堆，扣除共享输出重复项 | 约 261.88 | 包含必要的物理、几何、显示资源和池空闲空间 |
| 改池策略前 compute 池未使用空间 | 约 244，单个空洞 188.5 | 缩小 VMA 首选大堆块 |
| 改池策略前原生池未使用空间 | 约 77，部分启动的单个空洞超过 64 | 同上 |

原生分配的具名资源包括 `mechanics.collision` 99.106 MiB、`geometry.vertices.shared` 25.960 MiB、indices/shadow indices 各 8.261 MiB、`optics.surface_final` 16.875 MiB、UI target/composite 各 10.5 MiB、CEF texture 5.25 MiB。这些是跨内存类型的具名分配总量，用于定位所有者，不能直接再加到本地堆容量上。物理、显示和渲染共享关系均保留。源图像差异指普通导入图片，不是新增 LUT。

生产改动：

- FrameBuffer 增加只准备编码数据和小型 hit buffer 的路径；嵌入式未使用 base 不申请逐像素资源。默认完整 `prepare()` 保留虚调用，兼容 LightField。base 第一次真正渲染时再准备像素缓冲、display texture 和 integrator。
- Engine 初始化/reload 不再无条件创建 base display texture；相机 resize 只重建已经存在的 output。progressive base 的 resize/UI 更新也不提前创建 accumulation；实际首次 PT 渲染仍正常准备。
- compute 和原生 graphics/physics 的 VMA `preferredLargeHeapBlockSize` 最终设为 **16 MiB**。它是池块偏好，不是资源大小限制；大资源仍可独立分配。所用 VMA 的 dedicated 偏好阈值随之降到 8 MiB，会改变池布局和独立分配数量；不修改资源内容、精度或共享所有权。两份 Horizon 工作副本保持一致。
- 现有 Engine 场景集成测试真正执行原先未调用的 base 模式检查，新增未使用 base 无像素分配、第一次 RT/PT base 渲染完成分配的断言。progressive resize 回归在修复前实际失败，修复后通过。

64 MiB 候选曾有一次 5.086 ms，但移除临时诊断后，三次新进程、各 32 帧预热/64 帧测量得到 **6.824 / 11.192 / 5.247 ms**，因此没有接受。慢轮次主要影响 PrePass、HistoryFix、Blur、TemporalStabilization、Unpack；Pack、GPU 频率、输入资源字节数和工作量基本相同。16 MiB 最终版本重新独立启动三次，保留所有测量帧，无异常帧剔除：

| 1080p SP3 REBLUR GPU 滤波（ms） | 均值 | P95 | 最大值 |
|---|---:|---:|---:|
| 最终第 1 轮，64 帧 | 5.055046 | 5.376608 | 5.602496 |
| 最终第 2 轮，64 帧 | 5.112059 | 5.400480 | 5.642016 |
| 最终第 3 轮，64 帧 | 5.137640 | 5.409792 | 5.610816 |

三轮共 **192 帧，均值 5.101582 ms，全部低于 6 ms**。最高温度分别 62/64/62°C，均正常退出，进程/GPU 清理通过。滤波统计是三层 REBLUR 各 pass 的 GPU timestamp 之和，**包含 Pack/Unpack**，不包含 PrepareGuides、coverage-only 和 composition；它是连续 GPU 区间的下界。与找到的旧 Kitchen GPU pass 合计 4.938344 ms 使用同一统计范围；旧记录本身是画质采集，不升级为正式 A/B 性能验收。修复前本次 Engine 同范围为 37.973275 ms。

保持 Kitchen `scene_v3.xml`、SourceCamera 1920×1080、ReSTIR、SP3、默认 EDSL REBLUR、关闭额外 accumulation，不减少分辨率、SP 层数、样本/深度、物理行为或源图像精度。最后关闭阶段同步和 GPU profiler，以正常模式 8 帧预热、16 帧测量：

| 正常模式统计 | 均值（ms） |
|---|---:|
| Pipeline 渲染，包含降噪 | **57.466194** |
| 整段降噪，包含 guide、REBLUR、coverage 和合成 | **12.687244** |
| 总帧 | **62.021500** |

这些数值不能相加；6 ms 目标对应上表 GPU 滤波范围，不是 UI 的整段降噪。正常模式 optics/display FPS 上限均为 30，实测约 16.1 FPS；性能诊断使用 10 FPS 节流，其 `frame_ms` 不作为正常帧耗时。正常模式最高 68°C，正常退出且清理通过。通过相机 API 保存并查看 1920×1080 Kitchen PNG，场景能够正常出图。

最终 CLion RelWithDebInfo Engine 构建成功；相关测试重新构建后，`VisionRenderModeConfigTests`、`VisionReblurTests`、`VisionEngineSceneTests`、`VisionCameraSettingsTests`、`VisionSharedDisplayTests` **5/5 通过**，GPU 守护正常结束，最高 54°C。独立代码审查发现的虚调用和 progressive resize 问题均已修复，最后无未解决的 P1/P2。临时堆 dump 已移除，既有 Quill 等改动保留；以上性能采集在提交前的工作区完成。

证据目录：`D:/work/corona/restir-investigation-20261101/`；最终汇总为 `final-reblur-result.json`，三轮原始 CSV 和全样本保存在 `pool16-profiled-1/2/3`，正常模式及截图在 `normal-fixed-final16`，测试日志在 `pool16-ctest-guard`。每轮真实 allocation trace 均通过仅活动相机持有 rays/color/output 的检查，运行记录也校验了实际生效的相机和降噪配置。

测量边界：没有资源级 Windows eviction trace，因此不指认某一张纹理必然被换出；容量/驻留压力的判断来自资源清单、分辨率容量拐点、受影响 pass 和重复实验。`REBLUR_GPU_PROFILE_MODE=filter/intervals` 在启动相机重配置时出现失败，未采用其数据，也未宣称连续区间低于 6 ms。最终性能验证使用与旧记录匹配的 dispatch 范围；最终版本不携带临时 VMA heap dump，因此不宣称最终池空闲字节数已经精确测定。

## 2026-11-01：按 ReSTIR 开关差值比较 RTXPT，首轮接入优化

本轮采用用户定义：**降噪增量 = 同一渲染器 ReBLUR ON 的 ReSTIR 渲染耗时 − OFF 耗时**。此前约 5.1 ms 的滤波 dispatch 合计、约 12.7 ms 的 `denoise_ms` 都不能代替这个指标。正式配对关闭分阶段同步与 dispatch profiler，保持 1920×1080、SP3、1 spp、NRD quality/5×5/anti-firefly/history50/prepass15/40；未降低分辨率、采样预算或输入精度。

Kitchen `scene_v3.xml` / 对应冻结 glTF，SourceCamera，关闭额外 accumulation，RTX 3070 Ti 8 GiB / 驱动 610.88。修正历史 RTXPT 包装场景 60° FOV，与当前相机 **35.9833984375°** 对齐。参考资产缺失的 scene.bin 和 11 张贴图恢复到新目录；geometry/glTF/OBJ 与冻结清单 SHA256 一致，贴图与原始资源 hash 一致。

RTXPT 原基准强制 OFF，叶级 CSV 也未包含原生 NRD。独立参考仓库增加基准开关、实际 NRD 配置、连续 `Frame.RenderGPU` 和等待 GPU 完成的 `Frame.RenderWall`。正式参考为单进程 OFF64→ON64→ON64→OFF64，每块跳过 32 帧、保留 32 帧，两次独立进程。跨进程先导对照有漂移，约 1.9 ms 的单对结果未作为最终结论。

Corona 每次 OFF→ON→ON→OFF，每块跳过 16 个新完成的计时、采 24 个；优化前一轮，最终两轮独立进程。主指标为已等待 GPU 完成的 Pipeline render_ms；RTXPT 主指标为同步 Sample::Render wall interval，均排除基准 CSV 与节流 sleep。两边保留各自传输算法，RTXPT 原生 RR 与 Corona min-depth/RR 语义不同，不能宣称跨渲染器输入逐像素相同。Corona optics/display 30 FPS 上限在两种模式下均不构成瓶颈，应用 frame_ms 另外保留。

| 1080p ReSTIR | ReBLUR OFF（ms） | ReBLUR ON（ms） | 开关差值（ms） |
|---|---:|---:|---:|
| Corona 优化前 | 38.955850 | 58.806183 | **19.850333** |
| Corona 最终，两轮均值 | 39.308331 | 55.950615 | **16.642283** |
| RTXPT，两轮均值 | 66.859308 | 71.199989 | **4.340681** |

Corona 最终两轮差值 **16.672552 / 16.612015 ms**；RTXPT **4.332389 / 4.348973 ms**，连续 GPU parent 差值均值约 **4.104264 ms**。按用户口径节省 **3.208050 ms（16.16%）**，剩余增量约为 RTXPT 的 **3.83 倍，尚未达到基本同速**。UI 整段降噪仅从约 12.89 降到约 12.46 ms，不能代替省掉 SP history copy 后的整帧差值；应用帧间隔差值也从约 19.83 降至约 16.54 ms。

保留的生产改动：

- `Denoiser::needs_stable_plane_history(frame)` 默认 true；ReBLUR/ReLAX 委托给 coverage-only SVGF，按原 history_valid、连续帧、FOV、相机矩阵条件判断 previous guide 是否会读取。静止时省掉 StablePlanes-SaveHistory，首次移动在覆盖当前 planes 前仍保存；普通 SVGF 保留拷贝，历史存储与所有权不变。
- ReSTIR lobe composition 已将总辐射放入 direct、indirect 写零。显式 `total_radiance_coverage` 契约允许 coverage-only 省去零通道计算和历史。PT / 普通 SVGF 保留双通道；PT 按需申请 indirect ping-pong，切回总辐射后释放。1080p 少两张 float4 history，逻辑容量 **63.28125 MiB**；真实 Vulkan allocation trace 验证了转换。ReLAX 的诊断字节统计同步修正。

历史 trace 诊断排名：PrepareGuides 约 2.15 ms、coverage 约 4.43 ms、SP history copy 约 2.53 ms；这些数字不与正式开关差值混加。Corona PrepareGuides 重建材质 evaluator，在 hit distance 缺失时补发 lobe guide ray；RTXPT 从路径追踪记录直接准备 NRD 输入。后续重点是重复工作和数据流；已有同输入 replay 表明 EDSL NRD 核心接近原生 HLSL，不能将所有剩余差距归给核心滤波。

最终额外诊断的 24 个完整 1080p 静止帧中，`StablePlanes-SaveHistory` dispatch **0 次**；三层 filter dispatch 合计 5.260097 ms、PrepareGuides 2.288235 ms、coverage 4.179979 ms。它验证了静止拷贝确实消失，并给出剩余成本位置；该 profiler 运行不计入上表正式性能均值。原始帧分组及各 pass 结果为 `final-profile-summary.json`。

撤回实验：临时跳过计时回调仅观察到约 0.23 ms 差异，未交付关闭计时改动。六个 edge pass 合成一个直接访问原始 SP records 的 shader 虽通过输出对照，ON 却达约 81.12 ms、差值约 42.10 ms，故已撤回。重新编译并检查部署 DLL 不含 `SVGF-CoverageClassifyAllEdges`；未用硬件计数器进一步指认带宽或 occupancy。

验证：CLion RelWithDebInfo corona_engine 构建成功。真实 GPU 对照覆盖 16×16 / 13×9 / 1×1、SP-off / composed、有效虚拟平面、歧义分支、静止→移动→停止、FOV/帧序重置、resize、RT/PT 切换，单通道与双通道 RGB 逐分量一致。candidate 仅在查询为 true 时保存历史，reference 无条件保存；故意不保存历史的第三对照确实产生不同 RGB，证明测试实际读取历史。独立审查提出的该项验证缺口已关闭，无未解决的重要发现。

`VisionRenderModeConfigTests`、`VisionReblurTests`、`VisionRelaxPT`、`VisionRelaxReSTIR`、`VisionRelaxLifecycle`、`VisionEngineSceneTests`、`VisionCameraSettingsTests`、`VisionSharedDisplayTests` **8/8 通过**。正式配对均正常退出、进程/GPU 清理通过。Corona 最终两轮最高 64 / 66°C；RTXPT 63 / 62°C；相关 CTest 47°C。

回退融合候选后再检查测试构建依赖，重新编译 Engine scene 的两个相关 object，并在 `final-engine-recheck` 复测 Engine scene / camera / shared display，**3/3 通过**。

证据根目录：`D:/work/corona/reblur-onoff-20261101/`。汇总 `onoff-results.json`；原始数据 `corona-baseline-abba1`、`corona-final-abba1/2`、`rtxpt-abba1/2`；测试 `coverage-red3`、`coverage-green1`、`coverage-fusion-green1`、`final-ctest`；最终 dispatch 诊断 `corona-final-profile`。参考源码差异、构建 hash、匹配相机、资产恢复清单同目录。守护工具固定的 backend=vulkan-compute 标签不代表 RTXPT API，实际参考命令为 `--d3d12`。当前完成首轮优化，同速目标仍待实现。

## 2026-11-02：原版 HLSL 核心、AA 对齐及最终输出数据流

本轮验收始终采用用户定义：**同一渲染器 ReBLUR ON 的完整 ReSTIR render_ms − OFF render_ms**。旧约 5.1016 ms 是三层滤波 dispatch 的 GPU timestamp 合计，包含 Pack/Unpack，排除 PrepareGuides、coverage、独立 composition 及开关导致的生产/存储变化；此前不能用它证明完整降噪增量低于 6 ms。UI denoise_ms 和下面的诊断分项也不替代本指标。

用户明确选择“对齐 RTXPT，关闭额外 AA”。ReSTIR+ReBLUR 现在默认使用居中主射线，不再准备、分配或运行额外 coverage AA，也不为它保留前帧 stable planes；显式 accumulation 仍可开启抖动。`VISION_RESTIR_EXTRA_AA=1` 保留旧 AA 对照，PT/ReLAX/普通 SVGF 的 AA 策略不变。这是经用户批准的策略变化，不能称为对旧 AA 图像逐像素无损的提速。

最终场景为 Kitchen、SourceCamera、1920×1080、FOV 35.9833984375°、SP3、1 spp、关闭 accumulation，RTX 3070 Ti 8 GiB / 驱动 610.88。保持 NRD 4.14.3 quality、5×5、anti-firefly、history50、prepass15/40，不减少层数、分辨率、采样预算或 NRD 输入精度。两边保留各自 transport/RR 语义，不宣称跨渲染器输入或最终图像逐像素相同。

最终版本两次独立进程，各 OFF→ON→ON→OFF，每块预热 24 个完成计时、保留 48 个，每进程 ON/OFF 各 96 样本；共保留 384 样本，不剔除异常帧。RTXPT 两次 OFF64→ON64→ON64→OFF64，每块跳过32帧、保留32帧，取同步 Frame.RenderWall。所有正式采样关闭 stage 同步、dispatch/core profiler 与快照。

| 完整渲染，ms | OFF | ON | ON−OFF |
|---|---:|---:|---:|
| Corona 原始历史基线，旧 AA | 38.955850 | 58.806183 | 19.850333 |
| Corona 最终第 1 轮 | 38.292720 | 43.832291 | **5.539571** |
| Corona 最终第 2 轮 | 38.401551 | 43.887568 | **5.486017** |
| Corona 最终，两轮均值 | **38.347135** | **43.859929** | **5.512794** |
| RTXPT fresh，两轮均值 | 64.801042 | 69.386940 | **4.585898** |

最终仍比 RTXPT 多 **0.926896 ms，约 20.21%**，尚未追平。历史基线同时包含旧 AA 和早期接入开销，不能把全部差值归为纯代码优化。

最终 Corona 测量给 optics/display 各设15 FPS上限，在帧间等待；render_ms 计时不含等待。RTXPT 保持100 ms节流。先前30 FPS的第二次 preclear 实验触发70°C守护，整轮无效，未采用其三个已完成块或补齐缺失数据。正式两轮统一使用15 FPS上限重测，最高64/66°C，均正常退出，进程/GPU清理通过；生产默认帧率配置未修改。frame_ms 受两处节流影响，不作为性能验收。NRD质量配置保持一致。

交付的接入与数据流改动：

- ReSTIR 默认直接运行 NRD 4.14.3 原版 HLSL 的 SPIR-V 核心，借用现有 EDSL VkImage，不再维护一套重复纹理池。每代资源持有自己的包装和 binding cache；CPU dispatch/常量深复制，owner 保留至随后引擎 fence。首次 import 显式跟踪 UA/GENERAL，由 close 恢复初始状态，避免旧首帧 UNDEFINED 和重复恢复屏障。
- Guide 与 Pack 融合，常规 remodulation cache 从80B降为32B FP32；1080p减少94.92MiB。DI/GI用FP32暂存并按原顺序合成 lobe，允许raw producer继续compact，而NRD输入始终取FP32暂存值，不经过RGB21量化；无消费者时省掉 raw stable stores、MergeDominant，并将两路 raw stable 全数组缩为共96B占位，1080p减少约189.84MiB。OFF/显式raw诊断仍恢复完整通道。
- 默认最终输出路径在每层 Pack 消费原始 D/S 后，将已退役的同址缓冲保存为精确 guide；无效 guide 保存原始 RGB。保留前两层额外两对 FP16 NRD 输出，增加63.28MiB，末层一次 Unpack+Compose，保持原层序、加法分组和FP32乘法舍入，不写无消费者的 filtered D/S/hit-distance。批处理不再写旧 guide scratch，但当前仍保留其分配；原完整输出、非别名和 PT 调用保留旧契约。
- 原版核心的第0/1层末尾将共享六张NRD输入硬件清为精确无效值，随后第1/2层无效像素跳过重复image stores；有效像素及第0层完全保持原Pack。清屏合并在已有native callback中，不新增正常路径的提交或等待；布局恢复成本计入完整渲染差值。仅 original-core+三层batch启用。`REBLUR_BATCH_COMPOSE=0`、`REBLUR_PRECLEAR_INPUTS=0` 提供各自对照。

同期 batch-only 的30 FPS两轮为5.853546/5.777589 ms，对照 batch=0 为6.227379 ms；这组只作阶段记录，不与最终15 FPS数据混算。材质缓存v1/v2（7.481296/7.062693 ms）、逐层 streaming 合成（6.322720 ms）和 GENERAL sampled-image layout（6.276061 ms）未建立净收益，均已删除生产代码及专用测试，纹理/zero-spec-tint回归保留。

剩余成本来自核心之外仍需执行的工作。独立15 FPS诊断，按完整1080p帧分组并丢弃ON前24帧，119个稳定帧的 batch-only 均值为：原版核心 pre-close 区间3.567172ms，Guide+Pack三层合计1.766688ms，最终批量Unpack+Compose为0.672212ms。核心区间不含可选清屏和close的UA/GENERAL恢复；这些数值不相加作为完整降噪增量。核心已经使用原版HLSL，剩余接入重点是Guide重建中的材质评估、必要的补充lobe射线以及数据桥接，不能再把全部差距归因于EDSL滤波核。默认关闭所有profiling；`REBLUR_ORIGINAL_CORE_PROFILE` 仅以已完成submission和非阻塞poll收集GPU时间，不新增等待。batch/original-core 明确拒绝不兼容的旧aggregate filter/nrd/intervals统计，dispatch CSV也明确不包含native core。

验证与证据：

- 最终七条路径×五个真实场景×连续三帧通过，六份NRD输入逐字节一致；batch/preclear最终RT对保留原版HLSL路径逐字节一致。retained、batch、preclear均实际执行ON/OFF、raw调试、resize和稳定平面切换；typed metadata证明只有对应路径/层启用fast path。GPU守护121.83秒，最高50°C，正常退出及清理通过。
- 独立48组retained与48组batch矩阵通过：retained对照旧末层Unpack后独立Compose，batch对照旧全层Unpack后独立Compose，覆盖HDR/抵消、1/3层、有效/无效guide、别名及非别名；检查生产Pack的原地guide/原始RGB回退及完整输出默认契约。最终VisionReblurTests再次运行相应矩阵。
- 最终CLion RelWithDebInfo corona_engine构建通过。VisionRenderModeConfigTests、VisionReblurTests、VisionRelaxPT、VisionRelaxReSTIR、VisionRelaxLifecycle、VisionEngineSceneTests、VisionCameraSettingsTests、VisionSharedDisplayTests **8/8通过，91.86秒**；最高51°C，进程/GPU清理通过。此前物理存储/coverage回归及旧layout拒绝证据保留，最终引擎需从新进程启动。

正式性能原始文件：`D:/work/corona/reblur-onoff-20261101/round2-final-default15-abba1/2`，含全部 samples、实际camera/settings、run-config、runtime DLL SHA256和守护记录；参考为`round2-reference-rtxpt-abba1/2`。本轮汇总/源码与二进制清单：`D:/work/corona/reblur-round2-20261102/` 的 `final-results.json`、`final-source-sha256.json`、`final-source.diff`。最终真实场景快照已复制为 `final-sevenway-five-scenes-evidence/`，运行记录为 `final-sevenway-five-scenes-gpu/`、`final-eight-ctest/`；分项诊断在两个`current-*-profile15-summary.json`。失败及撤回实验的原始文件全部保留，未作为最终性能或通过结论。

### 2026-11-02：默认使用自有 EDSL 核心，完整降噪增量低于 6 ms

按用户后续要求，ReSTIR + ReBLUR 已从上一阶段的默认原版 HLSL 核心切回我们维护的 Ocarina EDSL 移植。GPU 核心沿用 NRD 4.14.3 原版算法和质量配置，CPU 常量及 pass 调度仍使用官方 NRD；这轮没有完全移除 NRD 依赖。`REBLUR_ORIGINAL_CORE=1` 保留原版 HLSL 对照，正常启动无需设置此变量。额外 AA 继续关闭，精确 FP32 lobe 输入、Guide/Pack 融合和最终批量合成继续保留。

验收仍是完整 ReSTIR `render_ms(ON) - render_ms(OFF)`，不是 UI 的 `denoise_ms`、分项 GPU 时间之和或包含等待的 `frame_ms`。Kitchen 固定资产、SourceCamera、1920×1080、FOV 35.9833984375°、SP3、1 spp、关闭 accumulation；NRD quality/5×5/anti-firefly/history50/prepass15/40 不变。两轮均为独立进程、OFF/ON/ON/OFF，每段预热24帧、采样48帧，无 profiling；仅外部采样脚本设置 optics/display 15 FPS，等待不计入 render_ms，生产帧率配置没有改变。

| 最终默认 EDSL | ReBLUR OFF | ReBLUR ON | 完整差值 |
| --- | ---: | ---: | ---: |
| 第一轮 | 38.382874 ms | 43.777253 ms | 5.394379 ms |
| 第二轮 | 38.556606 ms | 43.919403 ms | 5.362797 ms |
| 两轮合并 | 38.469740 ms | 43.848328 ms | **5.378588 ms** |

两轮共384个正式样本，最高65/66°C，守护与进程/GPU清理均通过。两份103个运行时 DLL 哈希完全一致并匹配最终磁盘文件，二进制指纹一致。相同 AA0/batch 配置的本轮 EDSL 起点是6.677579 ms；移除空回调拆分后的单轮为6.270094 ms，参数内联与共享屏障候选为6.177226 ms，最后加入类型布局缓存后取得上表两轮结果。16×8 Guide 试验未建立 ON 时间净收益，默认仍为8×8。

实际优化主要在提交和 CPU 参数准备，未替换滤波数学或降低精度：

- 将 EDSL 每层仅用于保留资源的空 `native_callback` 改为 `retain_until_complete`，在既有 GPU fence 完成后释放 generation；不拆批、不增加 host completion 依赖。真实原版 HLSL 回调保留原有提交顺序。replay 同样覆盖，排队 resize、stream 析构及资源释放均验证。
- `ShaderArgumentPack` 的 POD 内联区从256扩为2048字节，并预留参数描述数组。实际 ReBLUR Dispatch 每包85项溢出分配降至0。最大成员尺寸按编码后的具体 `raw_t` 缓存整数，避免每帧重复执行 `Type::of` 对72成员常量结构的递归遍历；全局类型注册和 Kernel 构造逻辑保持原样。host 类布局改变，因此本轮统一重编译 DLL 并使用新进程。独立 CPU 对照的24包中位耗时约0.715→0.097 ms，参数完整字节 hash 一致；这些 CPU 数字不替代上表的完整渲染验收。
- Classify/HitDistReconstruction 的预载只读纹理、只写 workgroup shared，屏障对齐原版 `GroupMemoryBarrierWithGroupSync`，不再使用更强的 device/all-memory workgroup 屏障。

最终验证：

- 八条路径×五个真实场景×连续三帧通过。六份 NRD 输入逐字节一致；新增默认 EDSL batch 对 retained EDSL 最终 RGB 逐字节一致，metadata 逐条确认 `native_core=false`，包含实际 ON/OFF、raw 调试、resize 和稳定平面切换。原版 HLSL 与 EDSL 核心之间仍采用既有 `0.001 + 0.002*abs(reference)` 容限，不宣称两核心逐字节相同。运行250.82秒、最高55°C，正常退出及清理通过。
- 最终 **9/9 CTest 通过，93.41秒**：上一阶段八项及独立 `VisionReblurEdslTests`，覆盖格式转换、边界和原版 NRD 数值对照；没有只验证默认 HLSL。独立 stream-order GPU 回归及诊断进一步证明 retained A/B 两代资源在同一 `(stream_slot, submit_serial)` 内，fence 前持有、fence 后释放，析构同步正确。
- 真实 `ShaderArgumentPack` CPU 测试通过132参数逐块字节/布局、257个float4超过内联区容量后的动态回退、热缓存后新 Kernel 结构登记、f16/f32交替编码。原有用户 `test_compute_math_parity.cpp` 改动未触碰；独立 Horizon 仓库的本轮改动单独保存。

新目标“完整增量尽量在6 ms内”已达到。此前 RTXPT 原始数据重算仍为4.585898 ms，当前 DSL 增量还多0.792690 ms（17.29%），不宣称已与 RTXPT 完全追平，也不将不同源码阶段的旧 HLSL 5.51 ms 与新 DSL 作胜负或因果比较。

正式原始样本在 `D:/work/corona/reblur-onoff-20261101/dsl-final-default15-abba1/2/`。本轮证据根目录 `D:/work/corona/reblur-dsl-20261102/` 保存 `independent-final-audit.json`、`final-results.json`、`final-source-sha256.json`、`final-engine.diff`、`final-horizon.diff` 及完整 `final-source/`。场景快照为 `final-eightway-five-scenes-evidence/`，守护记录为 `final-oracle/`、`final-ctest/`、`final-runtime/`；CPU 对照为 `argument-pack-candidate/`。初次诊断的同 AST shader 名称碰撞已在测试夹具中修正并重新验证，失败证据保留，不混入通过记录。

### 2026-11-02：ReBLUR 去除外部 NRD/NVRHI 依赖

用户明确范围为“去掉本次 ReBLUR 的外部依赖”，原有 Horizon、Slang、CEF 等引擎基础依赖保留。此前5.378588 ms版本仍调用外部官方 NRD CPU 调度，本节完成生产路径的依赖移除，并重新验收性能。

ReBLUR 现在始终编译自有 EDSL 核心，CPU 调度来自仓库内 `scheduler/` 静态目标：内置 NRD4.14.3 的 ReBLUR-only 调度、常量和资源描述，以及必要的 MathLib1.3 CPU头文件，共约0.6 MB，保留许可及来源哈希。原表达式和运算顺序保留，pipeline描述只保留名称/绑定/常量布局，三种原版 GPU bytecode 均为空。生产目标不再 include `native_nrd.cmake` 或编译 `native_nrd.cpp`，无 `NativeNrd::` 链接、SDK头路径、NRD/NVRHI导入库和 DXC 生成 NRD shader 的步骤。外部参考后端的源码、HLSL、构建和部署规则、环境变量选择分支现已一并删除；运行只使用仓库内实现。

实际 PE 导入表已核对主程序、ReBLUR插件、EDSL测试与scheduler测试均无 NRD.dll；本地旧 `bin/NRD.dll` 和引擎目录的 `NRD.dll` 已按路径/SHA清单移入外部证据隔离目录，后续GPU测试、CTest及两轮正式性能采样期间均不存在于运行目录。CMake编译/链接图也不引用原 `build/recordings/.../native-nrd` 路径。历史SDK参考源码已从当前工程移除，已有离线对照数据和性能记录继续保留。

CPU调度对官方4.14.3的330次查询、3154个pass精确一致：比较了1/3identifier、历史/重置/resize、settings、相机/投影/MV、pipeline/资源池/绑定及完整832字节常量。当前验证仅使用仓库内29,620字节官方golden摘要或既有完整fixture，不再动态加载官方DLL或生成capture。golden仅从此前独立官方capture生成，原fixture SHA256为 `41e5e76ba10c5fcf1395c6348fabf9ac58a4790598ab15ede2946203e7eda5f1`，不从生产实现生成expected值。

当时 Horizon 必要的五个运行时文件及两个测试文件，通过引擎维护的补丁和 before/after SHA 清单交付，源码准备流程自动验证或重放。86 项 CPU 工作区/补丁回归通过；另对当时固定版本 `6429a2e0…` 的干净 sparse clone 重放，七份 after SHA 全部一致，二次执行 bytes/mtime 零写入，并保留了用户既有 math 测试改动。这是该阶段的历史验证记录。

后续源码管理已移除该补丁交付机制。Horizon 与 CoronaEditor 均直接使用 `build_tools/source_workspaces.py` 锁定的提交；更新锁定值时负责保证依赖包含引擎所需功能，本地版本自动对齐，远程分支差异仅提示。

去依赖后重新执行完整 ReSTIR `render_ms(ON)-render_ms(OFF)`：Kitchen固定资产、SourceCamera、1920×1080、FOV35.9833984375°、SP3、1spp、AA0、accumulation关闭，原版quality/5×5/anti-firefly/history50/prepass15/40保持。每轮独立进程按OFF/ON/ON/OFF、每段预热24/采样48，无profiling；外部15 FPS采样等待不计入render_ms，生产帧率设置未改变。

| 内置CPU调度 + DSL | ReBLUR OFF | ReBLUR ON | 完整差值 |
| --- | ---: | ---: | ---: |
| 第一轮 | 38.249507 ms | 43.713488 ms | 5.463980 ms |
| 第二轮 | 38.387474 ms | 43.967089 ms | 5.579615 ms |
| 两轮合并 | 38.318491 ms | 43.840288 ms | **5.521797 ms** |

两轮均低于6 ms；不用旧SDK版本的5.378588 ms替代本节结果，也不将不同源码阶段的微小差值归因为CPU静态链接的收益或损失。RTXPT更低的增量仍是后续性能目标，本轮完成依赖闭合及既定6 ms边界。

最终无SDK验证：五条DSL数据路径×五个场景×三帧通过，六份输入逐字节一致，默认batch最终RGB对retained DSL逐字节一致，包含ON/OFF、raw、resize、SP切换；91.86秒、最高51°C。**10/10 CTest通过，97.44秒、最高52°C**，包含新增 `VisionReblurSchedulerTests` 的仓库内golden对照。所有守护退出和进程/GPU清理通过。CLion RelWithDebInfo配置/引擎/测试构建通过。新的ReBLUR对象布局采用新进程冷启动验证，旧进程跨版本热更新不在本轮验证范围。

原始正式样本为 `D:/work/corona/reblur-onoff-20261101/self-final-default15-abba1/2/`；本轮证据根目录为 `D:/work/corona/reblur-self-contained-20261102/`，含PE导入表、SDK隔离清单、构建/测试记录、真实Horizon基线重放、性能与源码清单。完整调度oracle证据位于上一阶段证据目录的 `scheduler-oracle/`，Horizon86项回归为 `horizon-runtime-patch-validation.json`。
