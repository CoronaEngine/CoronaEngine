"""V3 F5 log probe for tonight's non-native acceptance checks.

Usage:
    python docs/probes/v3_f5_log_check.py [path/to/*_corona.log]

The probe reads an engine log and reports PASS/WARN/FAIL for the issues that
recent F5 runs exposed: long user-visible progress silence, repeated actor
create broadcasts, repeated terrain file requests, and internal leakage in
chat/system messages. It does not compile or start the engine.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Iterable


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LOG_DIR = REPO_ROOT / "build" / "examples" / "engine" / "RelWithDebInfo" / "logs"

TIMESTAMP_RE = re.compile(r"\[(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d+)\]")
ACTOR_CREATE_RE = re.compile(r"Broadcast actor create .*?actor='([^']+)'.*?model='([^']+)'")
FILE_REQUEST_RE = re.compile(r"FILE_REQUEST .*?path='([^']+)'")

PROGRESS_KEYWORDS = (
    "生成进度",
    "资源准备",
    "参考图片",
    "模型仍在生成",
    "图片仍在准备",
    "正在分批生成",
    "排队中",
    "可介入窗口",
)
RESOURCE_HEARTBEAT_KEYWORDS = (
    "参考图片仍在准备中",
    "批模型仍在生成",
    "资源准备-图片",
    "资源准备-检索",
    "资源准备-模型",
)
USER_VISIBLE_HINTS = (
    "network_send_system_message",
    "network_send_agent_reply",
    "lanchat_send_system_message",
    "lanchat_send_system_message_to_host",
    "lanchat_send_system_message_to_user",
    "message_event_json",
)
FORBIDDEN_USER_VISIBLE_TOKENS = (
    "job_id",
    "session_id",
    "provider",
    "api_key",
    "runtime_context",
    "ThreadPool",
    "worker",
    "function name",
    "prompt=",
)
CEF_FATAL_KEYWORDS = (
    "[FATAL:",
    "DCHECK failed",
    "render_input_router.cc",
    "gpu_channel_manager.cc",
)
FINAL_SUCCESS_KEYWORDS = (
    "场景组合完成",
    "生成完成",
    "组合完成",
    "已放入场景",
)
IMPORT_EVIDENCE_KEYWORDS = (
    "导入引擎",
    "added to Scene",
    "Broadcast actor create",
    "IncrementalImport",
    "本批已放入",
)
ZERO_IMPORT_RE = re.compile(r"(?:导入引擎|导入)[:：]\s*0(?:\s*个|\b)")
INTERVENTION_RECORDED_KEYWORDS = (
    "可介入窗口：已记录",
    "已记录后续补充",
    "生成中新增",
    "生成中介入",
    "pending intervention",
)
INTERVENTION_RESULT_KEYWORDS = (
    "生成中吸收",
    "已吸收",
    "延后",
    "未处理原因",
    "失败",
    "待补",
    "failed resource requests",
    "deferred",
)
VLM_MARKERS = (
    "VlmReviewLoop",
    "ModelReviewer][VLMCapture",
    "VLM 外审",
    "VLM 审查",
    "独立摄像头截图",
)
VLM_SUCCESS_MARKERS = (
    "file ready=True",
    "VLM 审查未发现明显语义问题",
    "VLM 审查发现",
)
VLM_SKIP_MARKERS = (
    "VLM 外审未执行",
    "VLM 外审未完成",
    "截图失败",
    "截图超时",
    "无截图",
    "跳过审查",
)
VLM_READY_PATH_RE = re.compile(r"file ready=True .*?path=(.+?\.png)(?:\s|$)")
ZONE_SNAPSHOT_RE = re.compile(r"Zone decompose snapshot saved:\s*(.+?\.json)")
ROOM_MARKERS = (
    "__room_box",
    "room_box",
    "房间尺寸",
    "室内空间预算",
)
TERRAIN_MARKERS = (
    "__room_terrain",
    "__terrain_boundary",
    "terrain created",
    "Resource/terrain.obj",
)


@dataclass
class Check:
    level: str
    name: str
    detail: str


def _latest_log() -> Path:
    candidates = sorted(DEFAULT_LOG_DIR.glob("*_corona.log"), key=lambda p: p.stat().st_mtime)
    if not candidates:
        raise FileNotFoundError(f"no *_corona.log found under {DEFAULT_LOG_DIR}")
    return candidates[-1]


def _parse_timestamp(line: str) -> datetime | None:
    match = TIMESTAMP_RE.search(line)
    if not match:
        return None
    raw = match.group(1)
    if "." in raw:
        head, tail = raw.split(".", 1)
        raw = f"{head}.{tail[:6].ljust(6, '0')}"
    try:
        return datetime.fromisoformat(raw)
    except ValueError:
        return None


def _read_lines(path: Path) -> list[str]:
    return path.read_text(encoding="utf-8", errors="ignore").splitlines()


def _line_has_any(line: str, needles: Iterable[str]) -> bool:
    return any(needle in line for needle in needles)


def _check_actor_create(lines: list[str]) -> Check:
    counts: Counter[tuple[str, str]] = Counter()
    for line in lines:
        match = ACTOR_CREATE_RE.search(line)
        if match:
            counts[(match.group(1), match.group(2))] += 1
    if not counts:
        return Check("WARN", "actor-create", "no Broadcast actor create lines found")
    repeated = [(key, count) for key, count in counts.items() if count > 1]
    worst = max(counts.values())
    if worst > 2:
        offender = max(repeated, key=lambda item: item[1])
        actor, model = offender[0]
        return Check(
            "FAIL",
            "actor-create",
            f"same actor/model broadcast {offender[1]} times: actor={actor} model={model}",
        )
    if repeated:
        return Check(
            "WARN",
            "actor-create",
            f"{len(repeated)} actor/model pair(s) repeated once; acceptable only for explicit snapshot catch-up",
        )
    return Check("PASS", "actor-create", f"{len(counts)} actor create broadcasts, no duplicate actor/model pair")


def _check_file_requests(lines: list[str]) -> Check:
    requests: Counter[str] = Counter()
    for line in lines:
        match = FILE_REQUEST_RE.search(line)
        if match:
            requests[match.group(1)] += 1
    terrain_count = requests.get("Resource/terrain.obj", 0)
    repeated = [(path, count) for path, count in requests.items() if count > 1]
    if terrain_count > 1:
        return Check("FAIL", "file-request", f"Resource/terrain.obj requested {terrain_count} times")
    if repeated:
        sample = ", ".join(f"{path} x{count}" for path, count in repeated[:5])
        return Check("WARN", "file-request", f"repeated file requests detected: {sample}")
    if requests:
        return Check("PASS", "file-request", f"{len(requests)} file requests, terrain request count={terrain_count}")
    return Check("WARN", "file-request", "no FILE_REQUEST lines found")


def _check_progress_gaps(lines: list[str]) -> Check:
    events: list[datetime] = []
    for line in lines:
        if not _line_has_any(line, PROGRESS_KEYWORDS):
            continue
        ts = _parse_timestamp(line)
        if ts:
            events.append(ts)
    if len(events) < 2:
        return Check("WARN", "progress-gap", f"only {len(events)} user-progress-like event(s) found")
    gaps = [
        (events[idx] - events[idx - 1]).total_seconds()
        for idx in range(1, len(events))
    ]
    worst = max(gaps)
    if worst > 180:
        return Check("FAIL", "progress-gap", f"max user-progress gap {worst:.1f}s exceeds 180s")
    if worst > 75:
        return Check("WARN", "progress-gap", f"max user-progress gap {worst:.1f}s; watch F5 user anxiety")
    return Check("PASS", "progress-gap", f"max user-progress gap {worst:.1f}s across {len(events)} events")


def _check_resource_heartbeat(lines: list[str]) -> Check:
    count = sum(1 for line in lines if _line_has_any(line, RESOURCE_HEARTBEAT_KEYWORDS))
    if count:
        return Check("PASS", "resource-heartbeat", f"{count} resource progress/heartbeat line(s) found")
    return Check(
        "WARN",
        "resource-heartbeat",
        "no resource heartbeat found; acceptable only if resource stages finished quickly",
    )


def _check_user_visible_leak(lines: list[str]) -> Check:
    suspect: list[str] = []
    for line in lines:
        if not _line_has_any(line, USER_VISIBLE_HINTS):
            continue
        if _line_has_any(line, FORBIDDEN_USER_VISIBLE_TOKENS):
            suspect.append(line.strip())
    if suspect:
        return Check(
            "FAIL",
            "user-visible-leak",
            f"{len(suspect)} suspected user-visible line(s) contain internal fields; first={suspect[0][:220]}",
        )
    return Check("PASS", "user-visible-leak", "no suspicious internal fields in user-visible message lines")


def _check_meshopt_noise(lines: list[str]) -> Check:
    terrain_mesh = sum(1 for line in lines if "Mesh 'terrain'" in line)
    terrain_detail = sum(1 for line in lines if "Mesh 'terrain_detail'" in line)
    if terrain_mesh or terrain_detail:
        return Check(
            "PASS",
            "terrain-meshopt-noise",
            "MeshOpt terrain logs: "
            f"terrain={terrain_mesh}, terrain_detail={terrain_detail}; "
            "these are mesh segments, not actor imports. "
            "Use file-request and actor-create checks to judge real repeated import/sync.",
        )
    return Check("WARN", "terrain-meshopt-noise", "no terrain MeshOpt lines found")


def _check_cef_crash(lines: list[str]) -> Check:
    suspect = [line.strip() for line in lines if _line_has_any(line, CEF_FATAL_KEYWORDS)]
    if suspect:
        return Check(
            "FAIL",
            "cef-crash",
            f"CEF/Chromium fatal crash detected; first={suspect[0][:220]}",
        )
    return Check("PASS", "cef-crash", "no CEF/Chromium fatal crash markers found")


def _check_completion_integrity(lines: list[str]) -> Check:
    final_lines = [line.strip() for line in lines if _line_has_any(line, FINAL_SUCCESS_KEYWORDS)]
    if not final_lines:
        return Check("WARN", "completion-integrity", "no final scene completion summary found")
    for line in final_lines:
        if ZERO_IMPORT_RE.search(line):
            return Check("FAIL", "completion-integrity", f"completion summary reports zero imports; line={line[:220]}")
    has_import_evidence = any(_line_has_any(line, IMPORT_EVIDENCE_KEYWORDS) for line in lines)
    if not has_import_evidence:
        return Check(
            "FAIL",
            "completion-integrity",
            "completion summary found but no import/actor-create evidence exists in log",
        )
    return Check("PASS", "completion-integrity", f"{len(final_lines)} completion line(s) with import evidence")


def _check_intervention_visibility(lines: list[str]) -> Check:
    recorded_count = sum(1 for line in lines if _line_has_any(line, INTERVENTION_RECORDED_KEYWORDS))
    if not recorded_count:
        return Check("WARN", "intervention-visibility", "no generation-time intervention markers found")
    result_count = sum(1 for line in lines if _line_has_any(line, INTERVENTION_RESULT_KEYWORDS))
    if result_count:
        return Check(
            "PASS",
            "intervention-visibility",
            f"{recorded_count} intervention marker(s), {result_count} result/disposition marker(s)",
        )
    return Check(
        "FAIL",
        "intervention-visibility",
        f"{recorded_count} intervention marker(s) found but no absorbed/deferred/failed disposition in log",
    )


def _check_vlm_capture_write(lines: list[str]) -> Check:
    vlm_lines = [line.strip() for line in lines if _line_has_any(line, VLM_MARKERS)]
    if not vlm_lines:
        return Check("WARN", "vlm-capture-write", "no VLM review/capture markers found; acceptable only if VLM was disabled")
    ready_paths = [
        Path(match.group(1).strip())
        for line in vlm_lines
        for match in [VLM_READY_PATH_RE.search(line)]
        if match
    ]
    if ready_paths:
        existing = [path for path in ready_paths if path.exists() and path.stat().st_size > 0]
        if existing:
            return Check(
                "PASS",
                "vlm-capture-write",
                f"VLM screenshot file(s) confirmed on disk: {len(existing)}/{len(ready_paths)}; first={existing[0]}",
            )
        return Check(
            "WARN",
            "vlm-capture-write",
            f"VLM log reports file ready but screenshot file is missing/empty now; ready line(s)={len(ready_paths)}",
        )
    if any(_line_has_any(line, VLM_SUCCESS_MARKERS) for line in vlm_lines):
        return Check("PASS", "vlm-capture-write", "VLM review success marker found without screenshot file path")
    if any(_line_has_any(line, VLM_SKIP_MARKERS) for line in vlm_lines):
        sample = next((line for line in vlm_lines if _line_has_any(line, VLM_SKIP_MARKERS)), vlm_lines[0])
        return Check("WARN", "vlm-capture-write", f"VLM ran or was configured but did not produce confirmed screenshots; first={sample[:220]}")
    return Check("WARN", "vlm-capture-write", f"{len(vlm_lines)} VLM marker(s) found but no confirmed screenshot-ready evidence")


def _looks_mixed_scene_prompt(prompt: str) -> bool:
    text = str(prompt or "").lower()
    outdoor = any(token in text for token in ("室外", "户外", "草原", "森林", "营地", "山坡", "野外", "outdoor", "grassland", "forest", "camp"))
    structure = any(token in text for token in ("蒙古包", "帐篷", "木屋", "小屋", "房子", "建筑", "商铺", "屋子", "yurt", "tent", "cabin", "hut", "building"))
    inside = any(token in text for token in ("里面", "内部", "里有", "内有", "可进入", "inside", "interior"))
    return outdoor and structure and inside


def _looks_indoor_prompt(prompt: str) -> bool:
    text = str(prompt or "").lower()
    return any(token in text for token in ("卧室", "客厅", "室内", "藏宝室", "宝库", "密室", "库房", "地下室", "room", "vault", "chamber", "indoor"))


def _looks_outdoor_prompt(prompt: str) -> bool:
    text = str(prompt or "").lower()
    return any(token in text for token in ("室外", "户外", "草原", "森林", "营地", "夜市", "集市", "outdoor", "grassland", "forest", "camp", "market"))


def _zone_enclosures(payload: dict) -> set[str]:
    zones = payload.get("normalized_zones")
    if not isinstance(zones, list):
        return set()
    return {str(zone.get("enclosure") or "").strip().lower() for zone in zones if isinstance(zone, dict)}


def _latest_zone_snapshot(lines: list[str]) -> Path | None:
    paths: list[Path] = []
    for line in lines:
        match = ZONE_SNAPSHOT_RE.search(line)
        if match:
            paths.append(Path(match.group(1).strip()))
    return paths[-1] if paths else None


def _check_scene_substrate_evidence(lines: list[str]) -> Check:
    snapshot = _latest_zone_snapshot(lines)
    if snapshot:
        if not snapshot.exists():
            return Check("WARN", "scene-substrate", f"zone snapshot referenced but missing: {snapshot}")
        try:
            payload = json.loads(snapshot.read_text(encoding="utf-8"))
        except Exception as exc:  # noqa: BLE001
            return Check("WARN", "scene-substrate", f"zone snapshot could not be read: {exc}")
        prompt = str(payload.get("prompt") or "")
        enclosures = _zone_enclosures(payload)
        has_room = bool(enclosures & {"box", "shell"})
        has_terrain = "terrain" in enclosures
        if _looks_mixed_scene_prompt(prompt):
            if has_room and has_terrain:
                return Check("PASS", "scene-substrate", f"mixed scene snapshot has terrain + room/shell: {snapshot}")
            return Check("FAIL", "scene-substrate", f"mixed scene snapshot missing terrain or room/shell; enclosures={sorted(enclosures)} path={snapshot}")
        if _looks_indoor_prompt(prompt) and not has_room:
            return Check("FAIL", "scene-substrate", f"indoor-like snapshot missing room/shell; enclosures={sorted(enclosures)} path={snapshot}")
        if _looks_outdoor_prompt(prompt) and not has_terrain and not has_room:
            return Check("FAIL", "scene-substrate", f"outdoor-like snapshot missing terrain evidence; enclosures={sorted(enclosures)} path={snapshot}")
        return Check("PASS", "scene-substrate", f"zone snapshot present; enclosures={sorted(enclosures)}")

    has_room_marker = any(_line_has_any(line, ROOM_MARKERS) for line in lines)
    has_terrain_marker = any(_line_has_any(line, TERRAIN_MARKERS) for line in lines)
    if has_room_marker or has_terrain_marker:
        return Check("PASS", "scene-substrate", f"log substrate markers: room={has_room_marker}, terrain={has_terrain_marker}")
    return Check("WARN", "scene-substrate", "no zone snapshot or room/terrain markers found")


def run(path: Path) -> list[Check]:
    lines = _read_lines(path)
    return [
        _check_actor_create(lines),
        _check_file_requests(lines),
        _check_progress_gaps(lines),
        _check_resource_heartbeat(lines),
        _check_user_visible_leak(lines),
        _check_meshopt_noise(lines),
        _check_cef_crash(lines),
        _check_completion_integrity(lines),
        _check_intervention_visibility(lines),
        _check_vlm_capture_write(lines),
        _check_scene_substrate_evidence(lines),
    ]


def summarize(checks: list[Check]) -> str:
    counts = Counter(check.level for check in checks)
    if counts.get("FAIL", 0):
        status = "F5_BLOCKED"
    elif counts.get("WARN", 0):
        status = "F5_REVIEW_WARNINGS"
    else:
        status = "F5_READY"
    return (
        f"{status}: PASS={counts.get('PASS', 0)} "
        f"WARN={counts.get('WARN', 0)} FAIL={counts.get('FAIL', 0)}"
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", nargs="?", type=Path, help="Path to *_corona.log; defaults to latest")
    args = parser.parse_args(argv)

    path = args.log or _latest_log()
    if not path.exists():
        print(f"[FAIL] log not found: {path}", file=sys.stderr)
        return 2

    print(f"[INFO] log={path}")
    checks = run(path)
    for check in checks:
        print(f"[{check.level}] {check.name}: {check.detail}")
    print(f"[SUMMARY] {summarize(checks)}")
    return 1 if any(check.level == "FAIL" for check in checks) else 0


if __name__ == "__main__":
    raise SystemExit(main())
