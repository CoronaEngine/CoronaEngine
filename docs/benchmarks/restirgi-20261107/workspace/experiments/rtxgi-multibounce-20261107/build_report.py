"""Build the reviewable report from completed, audited measurement JSONs only."""
from pathlib import Path
from html import escape
import hashlib,json

R=Path(__file__).resolve().parent
A=R/'analysis'
q=json.loads((A/'final-quality-v1.json').read_text())
t=json.loads((A/'final-timing-v1.json').read_text())
assert q['status']=='quality_complete',q['status']
assert t.get('status','timing_complete')=='timing_complete',t.get('status')
assert all(q['comparison_evidence'][k] for k in ['actual_guides_reviewed','fresh_reference_agreement_reviewed','actual_route_and_transport_reviewed','source_and_runtime_freeze_reviewed'])
labels={'original':'原版 RestirGI（全分辨率实验版）','rtxpt':'RTXPT ReSTIR GI','rtxdi':'RTXDI ReSTIR GI','corona_port':'CoronaEngine 当前移植版'}
rows={}
for name,label in labels.items():
    quality=q['quality'][name];timing=t['renderers'][name]
    p=quality['pooled'];f=p['regions']['full_frame'];scopes=timing['pooled']
    assert p['frame_count']==96 and p['repeat_count']==3
    assert p['temporal_variance_degrees_of_freedom']==93
    assert scopes['GI_exclusive']['count']==720
    rows[name]=dict(label=label,GI_GPU_ms=scopes['GI_exclusive'],
        GI_plus_preparation_GPU_ms=scopes['GI_plus_primary_preparation'],full_GPU_ms=scopes.get('full_GPU_frame'),
        temporal_noise_percent=100*f['temporal_RMS_noise_over_reference_L2'],
        brightness_bias_percent=100*f['relative_signed_mean_bias'],
        per_frame_L2_NRMSE_percent=100*f['per_frame_L2_NRMSE_rms'],
        reference_noise_adjusted_NRMSE_percent=100*f['reference_noise_adjusted_frame_L2_NRMSE'],
        mean_luminance=f['mean_luminance'],time_mean_NRMSE_percent=100*f['time_mean_L2_NRMSE'],
        quality_repeat_ranges=p['repeat_ranges'],
        quality_repeats=[dict(label=x.get('label',str(i+1)),
            bias_percent=100*x['regions']['full_frame']['relative_signed_mean_bias'],
            noise_percent=100*x['regions']['full_frame']['temporal_RMS_noise_over_reference_L2'],
            NRMSE_percent=100*x['regions']['full_frame']['per_frame_L2_NRMSE_rms']) for i,x in enumerate(quality['repeats'])],
        timing_repeat_GI_means_ms=[x['scopes']['GI_exclusive']['mean_ms'] for x in timing['repeats']],
        stage_means_ms={k:scopes['component_'+k]['mean_ms'] for k in ['candidate','validation','temporal','spatial','resolve']},
        native_settings=quality['native_settings'])
    assert abs(sum(rows[name]['stage_means_ms'].values())-scopes['GI_exclusive']['mean_ms'])<1e-7
    if name=='corona_port':
        rows[name]['native_settings']=dict(rows[name]['native_settings'])
        rows[name]['native_settings'].pop('note',None)

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
summary=dict(schema='four-way-multibounce-final-results.v1',status='complete',scene='Kitchen',resolution=[1920,1080],
    GPU='NVIDIA GeForce RTX 3070 Ti',driver='610.88',transport=dict(min_depth=5,max_depth=16,rr_threshold=1,
        max_depth_includes_primary=True,first_roulette_after_surface=6),
    budget='One new full candidate path per valid pixel/frame; native reuse counts and validation schedules differ.',
    quality='3 independent offset sequences, frames96..127 each; within-repeat temporal variance,93DOF',
    timing='3 processes,120warmup+240measured each, capture disabled; GPU timestamp sums; means include periodic validation',
    reference='8192spp average of8 fresh1024spp PT halves from4producers',renderers=rows,
    source_evidence={n:sha(A/n) for n in ['final-quality-v1.json','final-timing-v1.json','reference-acceptance-v1.json']})
(R/'results-final.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding='utf-8')

main_rows=''.join(f'<tr><th>{escape(v["label"])}</th><td>{v["GI_GPU_ms"]["mean_ms"]:.2f}</td><td>{v["GI_plus_preparation_GPU_ms"]["mean_ms"]:.2f}</td><td>{v["temporal_noise_percent"]:.2f}%</td><td>{v["per_frame_L2_NRMSE_percent"]:.2f}%</td><td>{v["brightness_bias_percent"]:+.2f}%</td></tr>' for v in rows.values())
timing_rows=''.join(f'<tr><th>{escape(v["label"])}</th><td>{v["GI_GPU_ms"]["median_ms"]:.3f}</td><td>{v["GI_GPU_ms"]["p10_ms"]:.3f}–{v["GI_GPU_ms"]["p90_ms"]:.3f}</td><td>'+', '.join(f'{x:.3f}' for x in v['timing_repeat_GI_means_ms'])+'</td><td>'+(f'{v["full_GPU_ms"]["mean_ms"]:.3f}' if v['full_GPU_ms'] else '未提供完整 GPU 范围')+'</td></tr>' for v in rows.values())
repeat_rows=''.join(f'<tr><th>{escape(v["label"])}</th><td>{i+1}</td><td>{x["noise_percent"]:.2f}%</td><td>{x["NRMSE_percent"]:.2f}%</td><td>{x["bias_percent"]:+.2f}%</td></tr>' for v in rows.values() for i,x in enumerate(v['quality_repeats']))
stage_rows=''.join('<tr><th>'+escape(v['label'])+'</th>'+''.join(f'<td>{v["stage_means_ms"][k]:.3f}</td>' for k in ['candidate','validation','temporal','spatial','resolve'])+'</tr>' for v in rows.values())
pt,di,og,co=(rows[k] for k in ['rtxpt','rtxdi','original','corona_port'])
speed=100*(1-di['GI_GPU_ms']['mean_ms']/pt['GI_GPU_ms']['mean_ms'])
noise=100*(1-pt['temporal_noise_percent']/di['temporal_noise_percent'])
text=f'''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Kitchen：四份 ReSTIR GI 多反弹实测</title>
<style>body{{font:16px/1.7 system-ui,"Microsoft YaHei",sans-serif;color:#17212e;background:#f6f8fb;margin:0}}main{{max-width:1280px;margin:auto;padding:28px}}h1{{font-size:30px;line-height:1.3}}h2{{font-size:23px;margin-top:34px}}p{{max-width:1100px}}table{{border-collapse:collapse;background:white;width:100%;margin:18px 0;font-variant-numeric:tabular-nums}}th,td{{padding:11px 14px;border-bottom:1px solid #d9e0e9;text-align:right}}th:first-child{{text-align:left}}thead{{background:#e8eef7}}img{{width:100%;height:auto;background:white}}a{{color:#0757b3}}.note{{padding:14px 18px;background:#e8eef7;border-left:4px solid #5b7da8}}.scroll{{overflow:auto}}code{{word-break:break-all}}small{{color:#536273}}</style>
<main><h1>Kitchen：四份 ReSTIR GI 多反弹实测</h1>
<p>2026-11-07 · RTX 3070 Ti / 驱动610.88 · 1920×1080 · 纯间接光 · 去噪关闭</p>
<p>这次包含原版 RestirGI、RTXPT、RTXDI 和 CoronaEngine 当前移植版。最大深度16，最小深度5，RR阈值1，已用实际路径记录核验多反弹。单帧误差最低的是本次原版实验版，GPU耗时最低的是RTXDI；RTXDI比RTXPT节省{speed:.2f}%的GI耗时，RTXPT的时间噪声低{noise:.2f}%。</p>
<div class="note">每个有效像素每帧新增1条完整候选路径。原生时空复用次数、初始方向分布和验证频率保留差异，因此这是相同新增路径预算下的效果／耗时比较。采样预算并不代表相同邻域工作量或相同总射线数。</div>
<h2>主要结果</h2><div class="scroll"><table><thead><tr><th>实现</th><th>GI GPU均值 ms ↓</th><th>GI＋主表面准备 ms ↓</th><th>时间噪声 ↓</th><th>单帧 NRMSE ↓</th><th>平均亮度偏差</th></tr></thead><tbody>{main_rows}</tbody></table></div>
<p>时间噪声＝像素时间标准差的RMS／参考图L2；NRMSE＝相对公共PT参考的单帧总误差，含噪声与偏差。亮度偏差按线性Rec.709亮度计算，正值偏亮。所有数值先在原始线性HDR上计算，没有曝光补偿。</p>
<p>Corona一栏是当前移植方案的实测结果，包含本轮启用的多反弹路径；没有把RTXPT或RTXDI接入Corona。不能把这些数字归结为框架效率：原版每帧完整验证历史后缀，Corona每3帧验证一次；相关GPU工作全部计入均值。</p>
<h2>同曝光图像</h2><p>从左到右：8192spp公共PT参考、原版实验版、RTXPT、RTXDI、Corona。四份算法均为第1组的第127帧；下排为同一裁剪。统一曝光8、Reinhard与sRGB显示变换。表格使用全部三组96帧，图像只展示其中一帧。</p>
<a href="comparison-multibounce.png"><img src="comparison-multibounce.png" alt="公共PT参考与四份ReSTIR GI同曝光比较"></a>
<p>下图为同一帧相对参考的线性亮度差，统一±0.02范围，包含该帧噪声和亮度误差。</p><a href="error-multibounce.png"><img src="error-multibounce.png" alt="四份实现的同尺度有符号误差"></a>
<h2>GPU计时范围与重复性</h2><p>每份实现3次独立进程，每次先预热120帧，再测240帧，共720帧。计时关闭原始图像导出、读回与诊断计数存储。以下是GPU时间戳，未用CPU主机等待时间冒充GPU耗时。Corona周期验证使中位数低估平均成本，所以主要结论使用均值。</p>
<div class="scroll"><table><thead><tr><th>实现</th><th>GI中位数 ms</th><th>GI P10–P90 ms</th><th>三组GI均值 ms</th><th>完整GPU帧均值 ms</th></tr></thead><tbody>{timing_rows}</tbody></table></div>
<p>GI范围包含候选路径、验证、时空复用、最终resolve与相关GI内核。主表面准备另外列出。完整GPU范围还包括各程序其余绘制工作；Corona没有提供完整GPU帧范围，所以不填推测值。</p>
<div class="scroll"><table><thead><tr><th>GPU内核均值 ms</th><th>候选路径</th><th>验证</th><th>时间复用</th><th>空间复用</th><th>resolve</th></tr></thead><tbody>{stage_rows}</tbody></table></div>
<p>每行五项之和等于严格GI均值。周期验证在未执行的帧按0计入均值。原版较大的验证成本、RTX两份的空间复用成本差异，均能在实际GPU记录中直接看到；这些不是指令数估算。</p>
<h2>三组画质</h2><p>每组运行128帧，统计96–127帧；样本偏移0、10000、20000。时间方差在每组内部计算，再合并93个自由度，不把组间均值漂移算成时间噪声；相邻帧相关性不跨组。重复范围仅描述本次观测，不提供虚假的独立同分布置信区间。</p>
<div class="scroll"><table><thead><tr><th>实现</th><th>重复组</th><th>时间噪声</th><th>单帧NRMSE</th><th>平均亮度偏差</th></tr></thead><tbody>{repeat_rows}</tbody></table></div>
<h2>场景、深度和预算</h2>
<p>同一Kitchen，315个实例图元、1,474,261个三角形，固定相机和像素中心射线；精确几何面法线、Lambert反照率0.7、黑色环境、无自发光、单位硬太阳。玻璃窗作为不散射开口处理，原作者禁用的窗矩形也在四份实现中统一排除。Corona世界坐标Z反射已在几何、法线和太阳方向适配中核验。</p>
<p>表面1为主表面，最大16包含它，间接NEE在表面2–16计算。min_depth5遵循Corona零起始循环语义：首次俄罗斯轮盘在表面6之后；路径可以因未命中更早结束。终端表面16仍计算NEE，但不再继续。余弦提议的首次存活概率0.7⁵，之后0.7；原版和Corona保留均匀半球首方向，使用实际相机路径吞吐量执行同一RR规则。</p>
<div class="scroll"><table><thead><tr><th>实现组</th><th>首方向</th><th>空间复用</th><th>时间／历史</th><th>验证与resolve</th></tr></thead><tbody>
<tr><th>原版实验版</th><td>均匀半球</td><td>两遍8＋5提议</td><td>5来源，M上限20，W上限10</td><td>每帧历史完整后缀验证；4tap</td></tr>
<tr><th>Corona当前移植版</th><td>均匀半球</td><td>两遍8＋5提议</td><td>5来源，M上限20，W上限10</td><td>每3帧历史完整后缀验证；4tap</td></tr>
<tr><th>RTXPT／RTXDI</th><td>余弦半球</td><td>2邻居，半径32</td><td>历史8，年龄30</td><td>BASIC，最终可见性开；MIS关</td></tr>
</tbody></table></div>
<p>RTX两份均关闭去噪、AA、boiling、fallback、permutation。原版指全分辨率实验适配版：此前已将半分辨率坐标／采样半径／排列改为全分辨率，并将AO引导固定为1，移除AO对空间核紧度、相关性和resolve权重的影响，本轮在该基线上增加真实多反弹后缀。它不是未经改动的原仓库默认演示。RTXDI和原版原有单跳候选也扩展到同一5/16/RR1传输；不能宣称所有仓库开箱即支持此次设置。RTX的SDK核心未改；原版保留reservoir合并、Jacobian与clamp公式，但此前的分辨率适配和AO启发式中和也计入实验版本定义，详细来源见审计文件。</p>
<h2>公共参考、核验和限制</h2><p>四个独立参考生产者（原版实验版、RTXPT、RTXDI、Corona PT），各两份1024spp，共8192spp。原版SUM按1024归一化，其余已是MEAN；Corona PFM正确翻转存储行。八份参考通过亮度、split-noise、空间残差和相关性检查，生产者均值最大相差约0.036%。公共参考仍有有限采样噪声，不能等同于数学真值。</p>
<p>已检查主表面位置／面法线／有效像素、真实传输与执行路由、可执行文件及源码冻结、实际帧覆盖。匹配的全图结果保留边缘像素，另输出一致法线／位置区域诊断。相同相机的静态灰色漫反射Kitchen不能代表运动、遮挡显露、玻璃或复杂材质下的普遍优劣；图示单帧也不能代替完整的时间统计。</p>
<p>复核曾发现Corona实验种子偏移没有写入历史回放键，且只偏移候选路径、未一起偏移其他pass的随机流。修正回放键，并使用同一样本帧偏移时间、空间和resolve的随机键；历史索引、排列相位、验证调度仍使用逻辑帧。120组CPU种子检查及跨pass参数检查通过，旧错误作为负对照仍被检出。全部Corona正式画质与计时已在最终修正构建上重采，旧数据排除。加载阶段的异常退出均无测量帧，已保留并排除；没有更改系统保护策略。GPU驱动温控／热降频在开始前只读确认，采集连续满负载，无额外帧率或等待限速。</p>
<h2>原始证据</h2><p>
<a href="results-final.json">精简结果JSON</a> · <a href="scene_contract.json">场景与传输契约</a> ·
<a href="analysis/final-quality-v1.json">完整画质统计</a> · <a href="analysis/final-timing-v1.json">完整GPU统计</a> ·
<a href="analysis/reference-acceptance-v1.json">参考验收</a> · <a href="analysis/quality-run-audit-v1.json">逐画质运行审计</a> ·
<a href="analysis/corona-seed-stream-regression-v1.json">随机流与回放键回归</a> · <a href="analysis/independent-review-resolution-v1.json">分析器独立复核</a> ·
<a href="final-independent-audit.json">最终独立证据审计</a> · <a href="analysis/README.md">指标定义与重算命令</a></p>
<small>指标与表格由build_report.py读取已验收JSON生成；运行、构建日志和未采用实验保存在同目录及.workspace/tmp/rtxgi-multibounce-20261107。</small></main></html>'''
(R/'report.html').write_text(text,encoding='utf-8')
print(json.dumps({n:{k:v for k,v in row.items() if k in ['GI_GPU_ms','temporal_noise_percent','brightness_bias_percent','per_frame_L2_NRMSE_percent']} for n,row in rows.items()},indent=2))
