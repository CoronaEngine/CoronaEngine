"""Stateless formatting policy for RuntimeEvent replay summaries."""

from __future__ import annotations

import re
from typing import Any


def format_agent_runtime_event_rows(events: Any) -> list[tuple[str, dict[str, Any]]]:
    """Format event lines while preserving the source event for disclosure checks."""

    if not isinstance(events, list):
        return []
    rows: list[tuple[str, dict[str, Any]]] = []
    for event in events:
        if not isinstance(event, dict):
            continue
        title = str(event.get("title") or "").strip()
        message = str(event.get("message") or "").strip()
        if not title and not message:
            continue
        progress = event.get("progress")
        prefix = f"{title}" if title else "状态更新"
        if isinstance(progress, int):
            prefix = f"{prefix} {max(0, min(100, progress))}%"
        if message:
            rows.append((f"{prefix}: {message}", event))
        else:
            rows.append((prefix, event))
    return rows


def format_agent_runtime_replay_runtime_event_report(summary: Any) -> str:
    if not isinstance(summary, dict) or not summary:
        return "emitted 0, failed 0"

    def safe_label(value: Any) -> str:
        text = str(value or "").strip().replace("_", "-")
        for marker in ("provider", "prompt", "url", "raw", "token", "api-key"):
            text = re.sub(marker, "resource", text, flags=re.IGNORECASE)
        return text[:80]

    emitted = int(summary.get("emitted_count") or 0)
    failed = int(summary.get("emit_failed_count") or 0)
    skipped_count = int(summary.get("disclosure_skipped_count") or 0)
    parts = [f"emitted {emitted}", f"failed {failed}"]
    if skipped_count > 0:
        parts.append(f"skipped {skipped_count}")
    type_counts = summary.get("event_type_counts") if isinstance(summary.get("event_type_counts"), dict) else {}
    top_types = [
        f"{safe_label(key)}:{int(value or 0)}"
        for key, value in sorted(type_counts.items())[:4]
        if str(key).strip() and int(value or 0) > 0
    ]
    if top_types:
        parts.append("types " + ",".join(top_types))
    report_ready = int(summary.get("report_ready_count") or 0)
    report_attention = int(summary.get("report_attention_count") or 0)
    if report_ready > 0:
        report_part = f"report-ready {report_ready}"
        if report_attention > 0:
            report_part += f"/attention {report_attention}"
        status_counts = (
            summary.get("report_health_status_counts")
            if isinstance(summary.get("report_health_status_counts"), dict)
            else {}
        )
        status_parts = [
            f"{safe_label(key)}:{int(value or 0)}"
            for key, value in sorted(status_counts.items())[:3]
            if str(key).strip() and int(value or 0) > 0
        ]
        if status_parts:
            report_part += " " + ",".join(status_parts)
        parts.append(report_part)
    latest = summary.get("latest_runtime_event") if isinstance(summary.get("latest_runtime_event"), dict) else {}
    event_type = safe_label(latest.get("event_type"))
    status = safe_label(latest.get("status"))
    if event_type:
        parts.append(f"latest {event_type}:{status or 'unknown'}")
    latest_report = summary.get("latest_report_ready") if isinstance(summary.get("latest_report_ready"), dict) else {}
    report_status = safe_label(latest_report.get("status"))
    if report_status:
        parts.append(f"latest-report {report_status}")
    environment_counts = (
        latest_report.get("environment_import_failure_code_counts")
        if isinstance(latest_report.get("environment_import_failure_code_counts"), dict)
        else {}
    )
    environment_failures = [
        f"{safe_label(key)}:{int(value or 0)}"
        for key, value in sorted(environment_counts.items())[:3]
        if str(key).strip() and int(value or 0) > 0
    ]
    if environment_failures:
        parts.append("env-import-failures " + ",".join(environment_failures))
    bridge_failed = int(latest_report.get("engine_write_bridge_failed_count") or 0)
    bridge_error_counts = (
        latest_report.get("engine_write_bridge_error_code_counts")
        if isinstance(latest_report.get("engine_write_bridge_error_code_counts"), dict)
        else {}
    )
    bridge_failures = [
        f"{safe_label(key)}:{int(value or 0)}"
        for key, value in sorted(bridge_error_counts.items())[:3]
        if str(key).strip() and int(value or 0) > 0
    ]
    if bridge_failures:
        parts.append("engine-write-failures " + ",".join(bridge_failures))
    elif bridge_failed > 0:
        parts.append(f"engine-write-failures {bridge_failed}")
    mismatch_count = int(latest_report.get("engine_write_readiness_mismatch_count") or 0)
    mismatch_channels = (
        latest_report.get("engine_write_readiness_mismatch_channels")
        if isinstance(latest_report.get("engine_write_readiness_mismatch_channels"), list)
        else []
    )
    mismatch_parts = [safe_label(item) for item in mismatch_channels[:4] if safe_label(item)]
    if mismatch_count:
        if mismatch_parts:
            parts.append(f"engine-write-mismatch {mismatch_count}(" + "/".join(mismatch_parts) + ")")
        else:
            parts.append(f"engine-write-mismatch {mismatch_count}")
    latest_skip = summary.get("latest_disclosure_skip") if isinstance(summary.get("latest_disclosure_skip"), dict) else {}
    skip_type = safe_label(latest_skip.get("event_type"))
    skip_audience = safe_label(latest_skip.get("audience"))
    if skip_type:
        parts.append(f"latest-skip {skip_type}:{skip_audience or 'unknown'}")
    return ", ".join(parts)


def format_agent_runtime_gm_runtime_event_replay_digest(digest: Any) -> str:
    """Format GM RuntimeEvent replay diagnostics without event routing."""

    if not isinstance(digest, dict) or not digest:
        return "emitted 0, failed 0, skipped 0"

    def safe_label(value: Any) -> str:
        text = str(value or "").strip().replace("_", "-")
        for marker in ("provider", "prompt", "url", "raw", "token", "api-key"):
            text = re.sub(marker, "resource", text, flags=re.IGNORECASE)
        return text[:60]

    emitted = int(digest.get("emitted_count") or 0)
    failed = int(digest.get("emit_failed_count") or 0)
    skipped = int(digest.get("disclosure_skipped_count") or 0)
    parts = [f"emitted {emitted}", f"failed {failed}", f"skipped {skipped}"]
    report_ready = int(digest.get("report_ready_count") or 0)
    report_attention = int(digest.get("report_attention_count") or 0)
    if report_ready > 0:
        report_part = f"report-ready {report_ready}"
        if report_attention > 0:
            report_part += f"/attention {report_attention}"
        status_counts = (
            digest.get("report_health_status_counts")
            if isinstance(digest.get("report_health_status_counts"), dict)
            else {}
        )
        status_parts = [
            f"{safe_label(key)}:{int(value or 0)}"
            for key, value in sorted(status_counts.items())[:3]
            if str(key).strip() and int(value or 0) > 0
        ]
        if status_parts:
            report_part += " " + ",".join(status_parts)
        parts.append(report_part)
    latest_report = digest.get("latest_report_ready") if isinstance(digest.get("latest_report_ready"), dict) else {}
    environment_import_failure_code_counts = (
        latest_report.get("environment_import_failure_code_counts")
        if isinstance(latest_report.get("environment_import_failure_code_counts"), dict)
        else {}
    )
    environment_failure_parts = [
        f"{safe_label(key)}:{int(value or 0)}"
        for key, value in sorted(environment_import_failure_code_counts.items())[:3]
        if str(key).strip() and int(value or 0) > 0
    ]
    if environment_failure_parts:
        parts.append("env-import-failures " + ",".join(environment_failure_parts))
    engine_write_bridge_failed_count = int(latest_report.get("engine_write_bridge_failed_count") or 0)
    engine_write_bridge_error_code_counts = (
        latest_report.get("engine_write_bridge_error_code_counts")
        if isinstance(latest_report.get("engine_write_bridge_error_code_counts"), dict)
        else {}
    )
    engine_write_failure_parts = [
        f"{safe_label(key)}:{int(value or 0)}"
        for key, value in sorted(engine_write_bridge_error_code_counts.items())[:3]
        if str(key).strip() and int(value or 0) > 0
    ]
    if engine_write_failure_parts:
        parts.append("engine-write-failures " + ",".join(engine_write_failure_parts))
    elif engine_write_bridge_failed_count > 0:
        parts.append(f"engine-write-failures {engine_write_bridge_failed_count}")
    engine_write_readiness_mismatch_count = int(latest_report.get("engine_write_readiness_mismatch_count") or 0)
    engine_write_readiness_mismatch_channels = (
        latest_report.get("engine_write_readiness_mismatch_channels")
        if isinstance(latest_report.get("engine_write_readiness_mismatch_channels"), list)
        else []
    )
    engine_write_mismatch_parts = [
        safe_label(item)
        for item in engine_write_readiness_mismatch_channels[:4]
        if safe_label(item)
    ]
    if engine_write_readiness_mismatch_count:
        if engine_write_mismatch_parts:
            parts.append(
                "engine-write-mismatch "
                f"{engine_write_readiness_mismatch_count}(" + "/".join(engine_write_mismatch_parts) + ")"
            )
        else:
            parts.append(f"engine-write-mismatch {engine_write_readiness_mismatch_count}")
    latest_skip = digest.get("latest_disclosure_skip") if isinstance(digest.get("latest_disclosure_skip"), dict) else {}
    skip_type = safe_label(latest_skip.get("event_type"))
    skip_audience = safe_label(latest_skip.get("audience"))
    if skip_type:
        parts.append(f"latest-skip {skip_type}:{skip_audience or 'unknown'}")
    return ", ".join(parts)
