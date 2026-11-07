"""Append only measured native Corona results; preserve every previous row."""
from pathlib import Path
from html import escape
import hashlib,json
R=Path(__file__).resolve().parent
d=json.loads((R/'analysis/final-native-v1.json').read_text(encoding='utf-8'))
assert d['status']=='complete'
s=d['combined_results'];s['schema']='five-way-restirgi-native-supplement.v1'
s['supplement']='Only Corona native was newly measured; four previous measured rows reused unchanged.'
s['native_evidence']=dict(path='analysis/final-native-v1.json',sha256=hashlib.sha256((R/'analysis/final-native-v1.json').read_bytes()).hexdigest())
old=json.loads((R.parent/'rtxgi-multibounce-20261107/results-final.json').read_text(encoding='utf-8'))
assert all(s['renderers'][k]==v for k,v in old['renderers'].items())
(R/'results-final.json').write_text(json.dumps(s,ensure_ascii=False,indent=2),encoding='utf-8')
labels={'corona_native':'Corona 原生 ReSTIR GI（本次新增）','corona_port':'Corona 当前移植版','original':'RestirGI 全分辨率实验版','rtxpt':'RTXPT','rtxdi':'RTXDI'}
rows=''
for k,label in labels.items():
 v=s['renderers'][k]
 rows+=f'<tr><th>{escape(label)}</th><td>{v["GI_GPU_ms"]["mean_ms"]:.2f}</td><td>{v["GI_plus_preparation_GPU_ms"]["mean_ms"]:.2f}</td><td>{v["temporal_noise_percent"]:.2f}%</td><td>{v["per_frame_L2_NRMSE_percent"]:.2f}%</td><td>{v["brightness_bias_percent"]:+.2f}%</td></tr>'
n=s['renderers']['corona_native'];p=s['renderers']['corona_port']
q=d['quality']['pooled'];assert q['frame_count']==96 and q['temporal_variance_degrees_of_freedom']==93
assert n['GI_GPU_ms']['count']==720
repeats=''.join(f'<tr><td>{i+1}</td><td>{100*x["regions"]["full_frame"]["temporal_RMS_noise_over_reference_L2"]:.3f}%</td><td>{100*x["regions"]["full_frame"]["per_frame_L2_NRMSE_rms"]:.3f}%</td><td>{100*x["regions"]["full_frame"]["relative_signed_mean_bias"]:+.3f}%</td></tr>' for i,x in enumerate(d['quality']['repeats']))
stages=''.join(f'<tr><th>{escape(k)}</th><td>{v:.4f}</td></tr>' for k,v in n['stage_means_ms'].items())
html=f'''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Corona 原生 ReSTIR GI 补测</title>
<style>body{{font:16px/1.7 system-ui,"Microsoft YaHei",sans-serif;background:#f5f7fa;color:#17212e;margin:0}}main{{max-width:1200px;margin:auto;padding:28px}}h1{{font-size:29px}}h2{{font-size:22px;margin-top:32px}}table{{border-collapse:collapse;width:100%;background:white;margin:20px 0;font-variant-numeric:tabular-nums}}th,td{{padding:10px 13px;border-bottom:1px solid #d4deea;text-align:right}}th:first-child{{text-align:left}}thead{{background:#e7edf6}}tbody tr:first-child{{background:#eef5ff}}.scroll{{overflow:auto}}a{{color:#0757b3}}.note{{padding:12px 18px;background:#e7edf6}}code{{word-break:break-all}}</style><main>
<h1>Corona 原生 ReSTIR GI 补测：五份实现列表</h1><p>2026-11-07 · Kitchen灰色漫反射 · 1920×1080 · RTX3070Ti /610.88 · 纯间接光，无去噪</p>
<p>本次只新增测量Corona既有原生ReSTIRGI。前四份实测数据和8192spp公共PT参考逐项保持不变，没有重新运行其他实现。</p>
<div class="scroll"><table><thead><tr><th>实现</th><th>GI内核 ms ↓</th><th>GI＋准备 ms ↓</th><th>时间噪声 ↓</th><th>总误差NRMSE ↓</th><th>亮度偏差</th></tr></thead><tbody>{rows}</tbody></table></div>
<p class="note">比较效率应看“GI＋准备”：原生GI的主表面BSDF候选由DI初始内核生成，不能漏算。原生这一列计入整个融合的DI初始内核（包含部分直接光工作），以及几何准备；GI内核一列只含后缀追踪、时间复用、空间复用与shading。因此原生的GI内核边界与另四份不同，不能单独据此排名。</p>
<p>原生候选生产融合内核：{n['candidate_producer_fused_GPU_ms']['mean_ms']:.4f}ms；几何准备：{n['primary_geometry_preparation_GPU_ms']['mean_ms']:.4f}ms。原生完整GPU帧范围不可用，未将CPU主机时间替代为GPU计时。</p>
<h2>新增测量口径</h2><p>最小深度5、最大深度16、RR阈值1。表面1为主表面，后缀追踪表面2–16；首次RR在表面6后。每个有效像素每帧1条完整新候选路径；原生保留2个空间邻居、历史限制5、半径12、最大年龄30、原有可见性／Jacobian／归一化／复用逻辑，DI复用关闭且M_bsdf=1。保留原生终端BSDF/RR/补充射线工作，计入实测成本。各实现复用设置和总射线工作量仍不同。</p>
<p>同一精确Lambert0.7、几何面法线、黑环境／自发光、单位硬太阳、固定相机像素中心。仅在测试开关下对齐物理材质与后缀几何法线，并集中偏移IndependentSampler随机流；历史索引仍使用逻辑帧。原生算法用于复用筛选的插值法线启发式保留。原版RestirGI仍指前次的全分辨率／AO引导固定为1实验适配版；本次没有修改其数据。</p>
<p>构建使用项目规定的CLion脚本，6项现有光学CPU测试与4项新增契约检查通过。新适配块移除后可逐文件恢复前次源码哈希；原生DI及通用Li主体未改。先进行32帧无复用校准：平均亮度相对公共PT为+0.21049%；主表面物理位置／法线在有效像素上与前次Corona一致，跨引擎差异限于已记录的边缘案例。该校准不替代正式复用后的画质统计。</p>
<h2>原生三组画质</h2><p>每组128帧，统计96–127帧，偏移0／10000／20000；共96帧、93个组内时间方差自由度。噪声是时间RMS／公共参考L2；NRMSE包含噪声和偏差；亮度按原始线性Rec.709计算，正值偏亮。没有单独调整曝光或能量。重复结果具有时间相关性，不声称独立同分布置信区间。</p>
<table><thead><tr><th>组</th><th>时间噪声</th><th>NRMSE</th><th>亮度偏差</th></tr></thead><tbody>{repeats}</tbody></table>
<p>原生存在少量极亮离群点，三组时间噪声范围123.01%–306.89%，因此RMS对随机序列敏感。独立检查96帧共199,065,600个像素样本：354个亮度大于1，18个大于10，2个大于100，均为有效主表面像素；亮度大于1的样本贡献总平方误差的85.51%。没有裁剪、剔除或降低这些样本的权重。此为观测到的离群分布，不据此断言具体代码成因。</p>
<h2>原生计时拆分</h2><p>3个独立进程，各预热120帧后测240帧，共720帧。正式计时关闭原始图像导出、读回和物理guide诊断，连续执行，无额外帧率限制。以下为不重叠的GPU时间戳均值。</p>
<table><thead><tr><th>GI内核组成</th><th>ms</th></tr></thead><tbody>{stages}</tbody></table>
<p>三项之和＝{n['GI_GPU_ms']['mean_ms']:.4f}ms；加候选生产融合内核和几何准备后＝{n['GI_plus_preparation_GPU_ms']['mean_ms']:.4f}ms。所有阶段均保留原生算法开销，没有用源码或指令数估算。</p>
<p>结果只适用于这次静态灰色漫反射Kitchen，不能直接推断运动、复杂材质和遮挡显露时的普遍排名。</p>
<h2>证据</h2><p><a href="results-final.json">五行精确结果</a> · <a href="analysis/final-native-v1.json">原生完整统计及前四行副本</a> · <a href="analysis/manifest-executed-v1.json">冻结的执行清单</a> · <a href="source-audit.json">原生源代码审计</a> · <a href="native-contract-regression.json">契约回归</a> · <a href="../rtxgi-multibounce-20261107/report.html">前四份完整报告</a> · <a href="analysis/physical-guide-domain-acceptance-v1.json">校准与物理guide验收</a> · <a href="independent-native-check.json">独立重算</a> · <a href="outlier-check.json">亮斑分布检查</a></p></main></html>'''
(R/'report.html').write_text(html,encoding='utf-8')
print(json.dumps({k:{x:v[x] for x in ['GI_GPU_ms','GI_plus_preparation_GPU_ms','temporal_noise_percent','per_frame_L2_NRMSE_percent','brightness_bias_percent']} for k,v in s['renderers'].items()},ensure_ascii=False,indent=2))
