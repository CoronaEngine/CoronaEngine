"""Offline self-checks for docs/probes/v3_f5_log_check.py."""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
from pathlib import Path


_PROBE_PATH = Path(__file__).with_name("v3_f5_log_check.py")
_SPEC = importlib.util.spec_from_file_location("v3_f5_log_check_under_test", _PROBE_PATH)
assert _SPEC and _SPEC.loader
_MODULE = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = _MODULE
_SPEC.loader.exec_module(_MODULE)


def _write_log(text: str) -> Path:
    handle = tempfile.NamedTemporaryFile("w", encoding="utf-8", suffix="_corona.log", delete=False)
    with handle:
        handle.write(text)
    return Path(handle.name)


def _write_zone_snapshot(prompt: str, enclosures: list[str]) -> Path:
    handle = tempfile.NamedTemporaryFile("w", encoding="utf-8", suffix=".json", delete=False)
    payload = {
        "prompt": prompt,
        "raw_zones": [],
        "normalized_zones": [
            {"zone_id": f"z{idx}", "name": enclosure, "enclosure": enclosure}
            for idx, enclosure in enumerate(enclosures)
        ],
    }
    with handle:
        json.dump(payload, handle, ensure_ascii=False)
    return Path(handle.name)


def _write_png_stub() -> Path:
    handle = tempfile.NamedTemporaryFile("wb", suffix=".png", delete=False)
    with handle:
        handle.write(b"\x89PNG\r\n\x1a\nstub")
    return Path(handle.name)


def _levels(checks):
    return {check.name: check.level for check in checks}


def _details(checks):
    return {check.name: check.detail for check in checks}


def test_probe_accepts_healthy_resource_log():
    snapshot = _write_zone_snapshot("草原上的蒙古包，里面有毯子和桌子", ["terrain", "shell"])
    screenshot = _write_png_stub()
    path = _write_log(
        "\n".join(
            [
                "[2026-06-19T03:42:00.000000][1][INFO] network_send_system_message 生成进度 10%：资源准备-图片：参考图片仍在准备中",
                "[2026-06-19T03:42:40.000000][1][INFO] network_send_system_message 生成进度 55%：资源准备-模型：第 1/2 批模型仍在生成",
                "[2026-06-19T03:42:50.000000][1][INFO] NetworkSystem: Broadcast actor create — actor='actor-a' scene='Scene/场景1.scene' model='Resource/terrain.obj' deps=1",
                "[2026-06-19T03:42:51.000000][1][INFO] NetworkSystem: Received FILE_REQUEST from peer — id=2 path='Resource/terrain.obj'",
                "[2026-06-19T03:42:52.000000][1][DEBUG] [MeshOpt] Mesh 'terrain': starting phase1 simplification",
                "[2026-06-19T03:42:53.000000][1][DEBUG] [MeshOpt] Mesh 'terrain_detail' indexed: 4 -> 4 unique vertices",
                "[2026-06-19T03:42:53.500000][1][INFO] network_send_system_message 可介入窗口：已记录“新增一只小狗”，会优先进入下一批或最终调整。",
                f"[2026-06-19T03:42:53.600000][1][INFO] [ModelReviewer][VLMCapture] file ready=True path={screenshot} elapsed=0.20s",
                f"[2026-06-19T03:42:53.700000][1][INFO] [SceneComposer] Zone decompose snapshot saved: {snapshot}",
                "[2026-06-19T03:42:54.000000][1][INFO] network_send_agent_reply [场景设计大师] 场景组合完成 • 导入引擎：1 个 ✅ 已放入场景：terrain",
                "[2026-06-19T03:42:55.000000][1][INFO] network_send_agent_reply • 生成中吸收：1 条后续要求",
            ]
        )
    )
    checks = _MODULE.run(path)
    levels = _levels(checks)

    assert levels["actor-create"] == "PASS"
    assert levels["file-request"] == "PASS"
    assert levels["progress-gap"] == "PASS"
    assert levels["resource-heartbeat"] == "PASS"
    assert levels["user-visible-leak"] == "PASS"
    assert levels["terrain-meshopt-noise"] == "PASS"
    assert "not actor imports" in _details(checks)["terrain-meshopt-noise"]
    assert "file-request and actor-create" in _details(checks)["terrain-meshopt-noise"]
    assert levels["cef-crash"] == "PASS"
    assert levels["completion-integrity"] == "PASS"
    assert levels["intervention-visibility"] == "PASS"
    assert levels["vlm-capture-write"] == "PASS"
    assert levels["scene-substrate"] == "PASS"
    assert _MODULE.summarize(checks) == "F5_READY: PASS=11 WARN=0 FAIL=0"
    print("[OK] V3 F5 log probe accepts healthy resource log")


def test_probe_warns_when_vlm_ready_file_is_missing_now():
    path = _write_log(
        "[2026-06-19T03:42:53.600000][1][INFO] "
        "[ModelReviewer][VLMCapture] file ready=True path=C:\\Temp\\missing_vlm_file.png elapsed=0.20s"
    )
    checks = _MODULE.run(path)
    levels = _levels(checks)
    details = _details(checks)
    assert levels["vlm-capture-write"] == "WARN"
    assert "missing/empty" in details["vlm-capture-write"]
    print("[OK] V3 F5 log probe warns when VLM ready file is missing now")


def test_probe_flags_repeated_sync_and_user_visible_leak():
    path = _write_log(
        "\n".join(
            [
                "[2026-06-19T03:42:00.000000][1][INFO] network_send_system_message 生成进度 10%：资源准备-图片：开始",
                "[2026-06-19T03:46:30.000000][1][INFO] network_send_system_message 生成进度 20%：资源准备-模型：继续",
                "[2026-06-19T03:46:31.000000][1][INFO] network_send_system_message job_id=gen-secret provider=hunyuan",
                "[2026-06-19T03:46:32.000000][1][INFO] NetworkSystem: Broadcast actor create — actor='actor-a' scene='Scene/场景1.scene' model='Resource/terrain.obj' deps=1",
                "[2026-06-19T03:46:33.000000][1][INFO] NetworkSystem: Broadcast actor create — actor='actor-a' scene='Scene/场景1.scene' model='Resource/terrain.obj' deps=1",
                "[2026-06-19T03:46:34.000000][1][INFO] NetworkSystem: Broadcast actor create — actor='actor-a' scene='Scene/场景1.scene' model='Resource/terrain.obj' deps=1",
                "[2026-06-19T03:46:35.000000][1][INFO] NetworkSystem: Received FILE_REQUEST from peer — id=2 path='Resource/terrain.obj'",
                "[2026-06-19T03:46:36.000000][1][INFO] NetworkSystem: Received FILE_REQUEST from peer — id=3 path='Resource/terrain.obj'",
                "[2026-06-19T03:46:36.500000][1][INFO] network_send_system_message 可介入窗口：已记录“新增一只小狗”，会优先进入下一批或最终调整。",
                "[2026-06-19T03:46:37.000000][1][INFO] network_send_agent_reply [场景设计大师] 场景组合完成 • 导入引擎：0 个",
                "[144560:50020:0618/214244.213:FATAL:components\\input\\render_input_router.cc:655] DCHECK failed: is_in_gesture_scroll_",
            ]
        )
    )
    checks = _MODULE.run(path)
    levels = _levels(checks)
    details = _details(checks)

    assert levels["actor-create"] == "FAIL"
    assert levels["file-request"] == "FAIL"
    assert levels["progress-gap"] == "FAIL"
    assert levels["user-visible-leak"] == "FAIL"
    assert levels["cef-crash"] == "FAIL"
    assert levels["completion-integrity"] == "FAIL"
    assert levels["intervention-visibility"] == "FAIL"
    assert "terrain.obj" in details["file-request"]
    summary = _MODULE.summarize(checks)
    assert summary.startswith("F5_BLOCKED:")
    assert "FAIL=7" in summary
    print("[OK] V3 F5 log probe flags repeated sync and user-visible leakage")


if __name__ == "__main__":
    test_probe_accepts_healthy_resource_log()
    test_probe_warns_when_vlm_ready_file_is_missing_now()
    test_probe_flags_repeated_sync_and_user_visible_leak()
    print("\n=== V3 F5 log probe ALL PASS ===")
