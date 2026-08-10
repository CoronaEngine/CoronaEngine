"""Stateless formatting policy for Runtime message-delivery summaries."""

from __future__ import annotations

import re
from typing import Any


def format_agent_runtime_message_delivery_report(
    summary: Any,
    *,
    redact_agent_reply: bool = False,
) -> str:
    """Format delivery counters while preserving the legacy redaction switch."""

    if not isinstance(summary, dict):
        return "暂无"

    def safe_delivery_label(value: Any) -> str:
        label = str(value or "").strip()
        if not label:
            return ""
        if not redact_agent_reply:
            return label
        replacements = {
            "agent_reply": "reply",
            "provider": "adapter",
            "prompt": "detail",
            "url": "link",
            "raw": "payload",
            "token": "credential",
            "api-key": "credential",
        }
        for marker, replacement in replacements.items():
            label = re.sub(marker, replacement, label, flags=re.IGNORECASE)
        return label[:80]

    def safe_failure_label(value: Any) -> str:
        return safe_delivery_label(value).replace("_", "-")

    requested = int(summary.get("requested_count") or 0)
    succeeded = int(summary.get("succeeded_count") or 0)
    failed = int(summary.get("failed_count") or 0)
    parts = [
        f"璇锋眰 {requested}",
        f"鎴愬姛 {succeeded}",
        f"澶辫触 {failed}",
    ]
    message_kind_counts = summary.get("message_kind_counts") if isinstance(summary.get("message_kind_counts"), dict) else {}
    channel_counts = summary.get("channel_counts") if isinstance(summary.get("channel_counts"), dict) else {}
    latest_kind = str(summary.get("latest_message_kind") or "").strip()
    latest_channel = str(summary.get("latest_channel") or "").strip()
    latest_stage = str(summary.get("latest_stage") or "").strip()
    latest_progress = summary.get("latest_progress")
    failure_code_counts = (
        summary.get("failure_code_counts")
        if isinstance(summary.get("failure_code_counts"), dict)
        else {}
    )
    failure_codes = ", ".join(
        f"{safe_failure_label(code)}:{int(count or 0)}"
        for code, count in sorted(failure_code_counts.items())[:5]
        if int(count or 0) > 0 and safe_failure_label(code)
    )
    latest_failure_code = safe_failure_label(summary.get("latest_failure_code"))
    if message_kind_counts:
        safe_kinds = {
            safe_delivery_label(key): int(value or 0)
            for key, value in message_kind_counts.items()
            if safe_delivery_label(key)
        }
        parts.append(f"绫诲瀷 {safe_kinds}")
    if channel_counts:
        safe_channels = {
            safe_delivery_label(key): int(value or 0)
            for key, value in channel_counts.items()
            if safe_delivery_label(key)
        }
        parts.append(f"鍑哄彛 {safe_channels}")
    if failure_codes:
        parts.append(f"failure codes {failure_codes}")
    if latest_failure_code:
        parts.append(f"latest failure {latest_failure_code}")
    if latest_kind or latest_channel or latest_stage:
        latest = safe_delivery_label(latest_kind) or "unknown"
        if latest_channel:
            latest += f"/{safe_delivery_label(latest_channel)}"
        if latest_stage:
            latest += f"@{safe_delivery_label(latest_stage)}"
        if isinstance(latest_progress, (int, float)):
            latest += f" {max(0, min(100, int(latest_progress)))}%"
        parts.append(f"鏈€杩?{latest}")
    return "；".join(parts)
