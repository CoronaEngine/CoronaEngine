"""Read flushed scope events; incomplete scopes never become measured durations."""
from collections import defaultdict
import html
import json
from pathlib import Path
import re

MARKER = "CORONA_PROFILE "


def parse_log(text):
    roots, events, warnings, shaders = [], [], [], []
    stacks = defaultdict(list)
    for line in text.splitlines():
        if MARKER not in line:
            cache = re.search(r"shader PTX cache (hit|miss): (.+)", line)
            timing = re.search(r"task NVRTC compile (.+)\.cu is take ([\d.]+) (ms|s)\b", line)
            if cache:
                active = [node["name"] for stack in stacks.values() for node in stack
                          if node["category"] == "switch"]
                shaders.append(dict(name=cache[2], cache=cache[1],
                                    phase=active[0] if len(active) == 1 else None,
                                    nvrtc_ms=0 if cache[1] == "hit" else None))
            elif timing:
                for shader in reversed(shaders):
                    if shader["name"] == timing[1] + ".ptx":
                        shader["nvrtc_ms"] = float(timing[2]) * (1000 if timing[3] == "s" else 1)
                        break
            continue
        try:
            event = json.loads(line.split(MARKER, 1)[1])
            ph, name, ts, tid = (event[key] for key in ("ph", "name", "ts", "tid"))
            if ph not in ("B", "E") or not isinstance(ts, (int, float)):
                raise ValueError("invalid phase/timestamp")
            stack = stacks[tid]
            events.append(event)
            if ph == "B":
                node = dict(name=name, category=event["cat"], start_us=ts, thread=tid,
                            duration_ms=None, self_ms=None, children=[])
                (stack[-1]["children"] if stack else roots).append(node)
                stack.append(node)
            elif stack and stack[-1]["name"] == name and ts >= stack[-1]["start_us"]:
                node = stack.pop()
                node["duration_ms"] = (ts - node["start_us"]) / 1000
                node["self_ms"] = max(0, node["duration_ms"] - sum(c["duration_ms"] or 0 for c in node["children"]))
            else:
                warnings.append(f"unmatched end event: {name}")
        except (ValueError, KeyError, TypeError) as error:
            warnings.append(f"invalid profile event: {error}")
    for stack in stacks.values():
        warnings.extend(f"incomplete stage: {node['name']}" for node in stack)

    def breakdown(node, totals):
        if node["self_ms"] is not None:
            totals[node["category"]] += node["self_ms"]
        for child in node["children"]:
            breakdown(child, totals)

    phases = []
    def function_summary(node, result):
        name = node["name"]
        if "::" in name or name.startswith(("optix", "cuModule", "nvrtc", "emit_cuda")):
            row = result.setdefault(name, dict(name=name, calls=0, incomplete=0, total_ms=0, self_ms=0))
            if node["duration_ms"] is None:
                row["incomplete"] += 1
            else:
                row["calls"] += 1
                row["total_ms"] += node["duration_ms"]
                row["self_ms"] += node["self_ms"]
        for child in node["children"]:
            function_summary(child, result)

    def collect(node):
        if node["category"] == "switch":
            totals = defaultdict(float)
            breakdown(node, totals)
            node["breakdown_ms"] = dict(totals)
            functions = {}
            function_summary(node, functions)
            node["functions"] = sorted(functions.values(), key=lambda row: -row["self_ms"])
            phases.append(node)
        else:
            for child in node["children"]:
                collect(child)
    for root in roots:
        collect(root)
    if not phases:
        warnings.append("No switch profile events; rebuild the native benchmark target.")
    return dict(phases=phases, scopes=roots, events=events, warnings=warnings, shaders=shaders,
                cache=dict(ptx_hits=len(re.findall(r"PTX cache hit", text, re.I)),
                           ptx_misses=len(re.findall(r"PTX cache miss", text, re.I))))


def _ms(value):
    return "未完成" if value is None else f"{value:,.3f} ms"


def _tree(node):
    title = f"{html.escape(node['name'])} <b>{_ms(node['duration_ms'])}</b>"
    if not node["children"]:
        return f"<li>{title}</li>"
    children = "".join(_tree(child) for child in node["children"])
    return f"<li><details open><summary>{title} <small>自身 {_ms(node['self_ms'])}</small></summary><ul>{children}</ul></details></li>"


def write_report(output: Path, report):
    output.mkdir(parents=True, exist_ok=True)
    (output / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    body = [f"<h1>PT → ReSTIR 切换耗时</h1><p>{html.escape(report.get('scene', ''))}</p>",
            "<p>墙钟计时；准备阶段包含 GPU 提交，首帧阶段显式等待 GPU 完成。分布使用各阶段自身耗时，父子阶段不会重复累计。未完成阶段不推算总时长。</p>",
            "<p>第 1 次运行使用现有缓存；第 2 次运行复用前一次留下的磁盘缓存。同一进程再次切换测量内存复用。工具不清缓存。</p>"]
    backend_scopes = any(event.get("name") == "CUDADevice::create_shader"
                         for run in report.get("runs", []) for event in run["profile"]["events"])
    body.append("<p>编译阶段包含 DSL、NVRTC 和驱动调用。NVRTC 明细读取已有计时日志；" +
                ("函数表单独记录 PTX 读写、CUDA/OptiX 模块、Program Group、Pipeline 和 SBT。" if backend_scopes else "本次日志未包含独立的 CUDA/OptiX 后端函数计时。") +
                "PTX 命中不代表 OptiX 驱动缓存命中，后者不做推断。</p>")
    for index, run in enumerate(report.get("runs", []), 1):
        process, profile = run["process"], run["profile"]
        body.append(f"<h2>运行 {index} · {html.escape(process['status'])} · 进程总计 {_ms(process['elapsed_ms'])}</h2>")
        body.append(f"<p>退出码 {process.get('exit_code')} · PID {process.get('pid')} · 工作目录 {html.escape(process.get('cwd', ''))}</p>")
        body.append(f"<p>PTX 缓存命中 {profile['cache']['ptx_hits']} / 未命中 {profile['cache']['ptx_misses']}（按日志统计，整次运行）</p>")
        body.extend(f"<p class='warning'>{html.escape(w)}</p>" for w in profile["warnings"])
        for phase in profile["phases"]:
            body.append(f"<section><h3>{html.escape(phase['name'])} · {_ms(phase['duration_ms'])}</h3>")
            children = {child["name"]: child for child in phase["children"]}
            prepare = children.get("switch.prepare", {}).get("duration_ms")
            frame = children.get("first_frame.complete", {}).get("duration_ms")
            body.append(f"<p>切换准备 <b>{_ms(prepare)}</b> · 首帧完成 <b>{_ms(frame)}</b></p>")
            total = phase["duration_ms"] or sum(phase["breakdown_ms"].values()) or 1
            for category, duration in sorted(phase["breakdown_ms"].items(), key=lambda pair: -pair[1]):
                body.append(f"<div class='row'><span>{html.escape(category)}</span><div class='track'><i style='width:{min(100, duration / total * 100):.3f}%'></i></div><b>{_ms(duration)}</b></div>")
            body.append("<ul>" + _tree(phase) + "</ul></section>")
            if phase.get("functions"):
                body.append("<details open><summary>函数累计耗时（按自身耗时排序；包含子调用列不可相加）</summary><table><tr><th>函数</th><th>完成次数</th><th>包含子调用</th><th>自身</th></tr>")
                for fn in phase["functions"]:
                    status = str(fn["calls"]) + (f"，{fn['incomplete']} 未完成" if fn["incomplete"] else "")
                    body.append(f"<tr><td>{html.escape(fn['name'])}</td><td>{status}</td><td>{_ms(fn['total_ms'])}</td><td>{_ms(fn['self_ms'])}</td></tr>")
                body.append("</table></details>")
            shaders = [shader for shader in profile.get("shaders", []) if shader["phase"] == phase["name"]]
            if shaders:
                body.append("<details><summary>着色器 PTX 缓存 / NVRTC 明细（不与上方总时间重复相加）</summary><table><tr><th>着色器</th><th>PTX</th><th>NVRTC</th></tr>")
                for shader in shaders:
                    body.append(f"<tr><td>{html.escape(shader['name'])}</td><td>{shader['cache']}</td><td>{_ms(shader['nvrtc_ms'])}</td></tr>")
                body.append("</table></details>")
        relative = f"run-{index}"
        body.append(f"<p><a href='{relative}/stdout.log'>stdout</a> · <a href='{relative}/stderr.log'>stderr</a> · <a href='{relative}/process.json'>启动与退出信息</a> · <a href='{relative}/trace.json'>Chrome/Perfetto trace</a></p>")
        if process.get("error"):
            body.append(f"<pre>{html.escape(process['error'])}</pre>")
    css = "body{max-width:1120px;margin:40px auto;padding:0 24px;font:15px/1.6 system-ui;color:#dfe9f4;background:#111925}h1,h2,h3{color:#fff}p,small{color:#aebed0}section{background:#1b2736;padding:20px;margin:20px 0;border-radius:12px}ul{padding-left:24px}li{margin:7px 0}b{font-variant-numeric:tabular-nums;margin-left:12px}summary{cursor:pointer}.row{display:grid;grid-template-columns:160px 1fr 145px;align-items:center;gap:12px}.track{background:#2b3849;height:14px;border-radius:4px}i{display:block;background:#58c6cb;height:100%;border-radius:4px}.warning{color:#ffca80}a{color:#6dd5ff}pre{white-space:pre-wrap}"
    css += "table{width:100%;border-collapse:collapse;margin:12px 0}td,th{text-align:left;padding:8px;border-bottom:1px solid #344255;overflow-wrap:anywhere}td:first-child{max-width:650px}"
    path = output / "report.html"
    path.write_text("<!doctype html><html lang='zh-CN'><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>PT → ReSTIR profile</title><style>" + css + "</style><body>" + "\n".join(body) + "</body></html>", encoding="utf-8")
    return path
