"""Run the V3 F5 non-native quick gate.

Usage:
    python docs/probes/v3_f5_quick_gate.py
    python docs/probes/v3_f5_quick_gate.py --log build/examples/engine/RelWithDebInfo/logs/<run>_corona.log

This script intentionally avoids C++/Ninja/CEF/F5/native build steps. It runs
the non-native ultimate-plan verification suite, then runs the F5 log probe on
the specified log or the latest available engine log.
"""

from __future__ import annotations

import argparse
import importlib.util
import subprocess
import sys
from datetime import datetime
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
VERIFY_SCRIPT = REPO_ROOT / "editor" / "plugins" / "AITool" / "services" / "verify_ultimate_plan.py"
LOG_PROBE = REPO_ROOT / "docs" / "probes" / "v3_f5_log_check.py"
REPORT_DIR = REPO_ROOT / "docs" / "f5_reports"
FRESHNESS_SENTINELS = (
    REPO_ROOT / "editor" / "plugins" / "AITool" / "cai_extensions" / "flows" / "model_retrieval_workflow" / "progress.py",
    REPO_ROOT / "editor" / "plugins" / "AITool" / "cai_extensions" / "flows" / "model_retrieval_workflow" / "dispatch.py",
    REPO_ROOT / "editor" / "plugins" / "AITool" / "cai_extensions" / "flows" / "model_retrieval_workflow" / "generate.py",
    REPO_ROOT / "editor" / "plugins" / "AITool" / "cai_extensions" / "flows" / "model_retrieval_workflow" / "retrieve.py",
    REPO_ROOT / "editor" / "plugins" / "AITool" / "cai_extensions" / "agent" / "scene_composer.py",
    REPO_ROOT / "editor" / "plugins" / "AITool" / "cai_extensions" / "agent" / "model_reviewer.py",
    REPO_ROOT / "editor" / "plugins" / "AITool" / "cai_extensions" / "agent" / "vlm_review_loop.py",
    REPO_ROOT / "editor" / "plugins" / "AITool" / "services" / "generation_composer_adapter.py",
    REPO_ROOT / "docs" / "probes" / "v3_f5_log_check.py",
    REPO_ROOT / "editor" / "Frontend" / "src" / "views" / "sidebar" / "lanchat" / "RoomPanel.vue",
    REPO_ROOT / "editor" / "Frontend" / "src" / "views" / "sidebar" / "Network.vue",
)
REMEDIATION_HINTS = {
    "actor-create": "若 FAIL，优先查 host 快照补发、actor create 去重和新 peer 加入流程；同一 actor/model 不应反复广播。",
    "file-request": "若 terrain 或大模型重复请求，优先查资源缓存、路径映射和对端 file transfer 去重。",
    "progress-gap": "若 FAIL，检查资源 workflow 的 progress_sink、图片补偿心跳和模型批次心跳是否接入聊天室。",
    "resource-heartbeat": "若 WARN/FAIL，确认该轮资源阶段是否很短；长耗时图片/模型阶段必须有用户可见心跳。",
    "user-visible-leak": "若 FAIL，检查 DisclosurePolicy / progress publisher，不得向用户暴露 job_id、session_id、provider、API key、线程或 prompt。",
    "terrain-meshopt-noise": "该项主要用于解释 MeshOpt 地形分段日志；重复导入以 file-request 和 actor-create 为准。",
    "cef-crash": "若 FAIL，归类为 CEF/Chromium 实机稳定性问题；该轮 UI 面板已崩，不建议作为通过样本。",
    "completion-integrity": "若 FAIL，说明完成摘要和实际导入证据不一致；优先查 SceneComposer final report 和 incremental import。",
    "intervention-visibility": "若 FAIL，说明生成中介入缺少吸收/延后/失败说明；优先查 Coordinator pending intervention 和 FinalReview 报告。",
    "vlm-capture-write": "若 WARN，先判断本轮是否开启 VLM；若已开启，检查独立 VLM camera、截图目录、file ready 日志和截图超时降级。",
    "scene-substrate": "若 FAIL，说明 room_box / terrain / shell 基底与场景类型不一致；优先查 SceneComposer zone snapshot 和混合场景 guardrail。",
}


def _load_log_probe():
    spec = importlib.util.spec_from_file_location("v3_f5_log_check_runtime", LOG_PROBE)
    if not spec or not spec.loader:
        raise RuntimeError(f"cannot load log probe: {LOG_PROBE}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _run(label: str, command: list[str], *, fail_on_nonzero: bool = True) -> int:
    print(f"[RUN] {label}", flush=True)
    completed = subprocess.run(command, cwd=REPO_ROOT)
    if completed.returncode == 0:
        print(f"[OK]  {label}", flush=True)
        return 0
    level = "[FAIL]" if fail_on_nonzero else "[WARN]"
    print(f"{level} {label} exited with {completed.returncode}", flush=True)
    return completed.returncode


def _freshness_status(log_path: Path) -> tuple[str, str]:
    if not log_path.exists():
        return "UNKNOWN", "日志文件不存在，无法判断新鲜度。"
    existing = [path for path in FRESHNESS_SENTINELS if path.exists()]
    if not existing:
        return "UNKNOWN", "未找到关键源码哨兵文件，无法判断日志是否覆盖本轮改动。"
    newest = max(existing, key=lambda path: path.stat().st_mtime)
    if log_path.stat().st_mtime < newest.stat().st_mtime:
        try:
            newest_label = str(newest.relative_to(REPO_ROOT))
        except ValueError:
            newest_label = str(newest)
        return (
            "STALE",
            f"日志早于关键改动 `{newest_label}`，只能用于历史复盘；请重新 F5 后再签收。",
        )
    return "FRESH", "日志时间晚于本轮关键改动，可作为当前工作区 F5 证据。"


def _write_report(log_path: Path, verify_exit: int | None) -> Path:
    probe = _load_log_probe()
    resolved_log = log_path if log_path else probe._latest_log()
    checks = probe.run(resolved_log)
    summary = probe.summarize(checks)
    freshness_level, freshness_detail = _freshness_status(resolved_log)
    REPORT_DIR.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    report_path = REPORT_DIR / f"v3_f5_{stamp}.md"
    verify_text = "skipped" if verify_exit is None else ("passed" if verify_exit == 0 else f"failed({verify_exit})")
    lines = [
        "# V3 F5 运行报告",
        "",
        f"- 生成时间：{datetime.now().isoformat(timespec='seconds')}",
        f"- 日志文件：`{resolved_log}`",
        f"- 非 native 总门禁：{verify_text}",
        f"- 日志摘要：`{summary}`",
        f"- 日志新鲜度：`{freshness_level}` - {freshness_detail}",
        "",
        "## 检查结果",
        "",
        "| 级别 | 检查项 | 说明 | 处置建议 |",
        "|---|---|---|---|",
    ]
    for check in checks:
        detail = str(check.detail).replace("|", "\\|")
        hint = REMEDIATION_HINTS.get(check.name, "按检查项说明定位。").replace("|", "\\|")
        lines.append(f"| {check.level} | {check.name} | {detail} | {hint} |")
    lines.extend(
        [
            "",
            "## 判读",
            "",
            "- `F5_READY`：日志层无 WARN/FAIL。",
            "- `F5_REVIEW_WARNINGS`：无阻断，但需人工确认 WARN 是否为可解释噪声。",
            "- `F5_BLOCKED`：存在 FAIL，不建议签收。",
            "",
        ]
    )
    report_path.write_text("\n".join(lines), encoding="utf-8")
    return report_path


def _check_level(checks, name: str) -> str:
    for check in checks:
        if check.name == name:
            return str(check.level)
    return "MISSING"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--log",
        type=Path,
        help="Optional *_corona.log path. Defaults to latest log in build/examples/engine/RelWithDebInfo/logs.",
    )
    parser.add_argument(
        "--skip-verify",
        action="store_true",
        help="Only run the log probe. Useful immediately after verify_ultimate_plan already passed.",
    )
    parser.add_argument(
        "--write-report",
        action="store_true",
        help="Write a Markdown run report under docs/f5_reports after the log probe.",
    )
    parser.add_argument(
        "--require-fresh",
        action="store_true",
        help="Fail the gate when the selected log is older than key V3 changes. Use for formal F5 sign-off.",
    )
    parser.add_argument(
        "--require-vlm-capture",
        action="store_true",
        help="Fail unless the log probe proves VLM screenshot files exist and are non-empty.",
    )
    parser.add_argument(
        "--require-substrate-evidence",
        action="store_true",
        help="Fail unless the log probe proves scene substrate evidence such as room_box/terrain/shell.",
    )
    args = parser.parse_args(argv)

    failed = 0
    verify_exit: int | None = None
    if not args.skip_verify:
        verify_exit = _run("non-native ultimate-plan verification", [sys.executable, str(VERIFY_SCRIPT)])
        failed += bool(verify_exit)

    probe_cmd = [sys.executable, str(LOG_PROBE)]
    if args.log:
        probe_cmd.append(str(args.log))
    failed += bool(_run("V3 F5 log probe", probe_cmd))
    if args.write_report:
        try:
            report = _write_report(args.log, verify_exit)
            print(f"[REPORT] {report}", flush=True)
        except Exception as exc:
            print(f"[WARN] report write failed: {exc}", flush=True)

    try:
        probe = _load_log_probe()
        resolved_log = args.log if args.log else probe._latest_log()
        checks = probe.run(resolved_log)
        required_checks = []
        if args.require_vlm_capture:
            required_checks.append(("vlm-capture-write", "VLM 截图写入"))
        if args.require_substrate_evidence:
            required_checks.append(("scene-substrate", "场景基底"))
        for check_name, label in required_checks:
            level = _check_level(checks, check_name)
            if level != "PASS":
                print(f"[WARN] required-check: {label} `{check_name}` is {level}, expected PASS", flush=True)
                failed += 1
        freshness_level, freshness_detail = _freshness_status(resolved_log)
        if freshness_level != "FRESH":
            print(f"[WARN] stale-log-check: {freshness_level} - {freshness_detail}", flush=True)
            if args.require_fresh:
                failed += 1
    except Exception as exc:
        print(f"[WARN] stale-log-check failed: {exc}", flush=True)
        if args.require_fresh:
            failed += 1

    if failed:
        print("[SUMMARY] V3 F5 quick gate found blocking issue(s).", flush=True)
        return 1
    print("[SUMMARY] V3 F5 quick gate passed. Review WARN lines from the log probe before final F5 sign-off.", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
