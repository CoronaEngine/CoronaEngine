"""Stateless formatting policy for Runtime Sync replay summaries."""

from __future__ import annotations

import re
from typing import Any


def _format_transfer_bytes(bytes_transferred: int, total_bytes: int) -> str:
    def human(value: int) -> str:
        amount = max(0, int(value or 0))
        if amount >= 1024 * 1024:
            return f"{amount / (1024 * 1024):.1f}MB"
        if amount >= 1024:
            return f"{amount // 1024}KB"
        return f"{amount}B"

    if bytes_transferred <= 0 and total_bytes <= 0:
        return ""
    if total_bytes > 0:
        return f"{human(bytes_transferred)}/{human(total_bytes)}"
    return human(bytes_transferred)


def format_agent_runtime_sync_replay_report(summary: Any) -> str:
    """Format sync replay counters with bounded, redacted labels."""

    if not isinstance(summary, dict):
        return "recorded 0, failed 0"

    def safe_label(value: Any) -> str:
        text = str(value or "").strip().replace("_", "-")
        for marker in ("prompt", "provider", "url", "raw", "token", "api-key", "path", "session", "job"):
            text = re.sub(marker, "resource", text, flags=re.IGNORECASE)
        return text[:60]

    recorded_count = int(summary.get("recorded_count") or 0)
    failed_count = int(summary.get("failed_count") or 0)
    actor_transform_count = int(summary.get("actor_transform_count") or 0)
    actor_delete_count = int(summary.get("actor_delete_count") or 0)
    peer_join_count = int(summary.get("peer_join_count") or 0)
    peer_leave_count = int(summary.get("peer_leave_count") or 0)
    transfer_failed_count = int(summary.get("transfer_failed_count") or 0)
    transfer_progress_count = int(summary.get("transfer_progress_count") or 0)
    latest_transfer_progress = int(summary.get("latest_transfer_progress") or 0)
    latest_chunk_index = int(summary.get("latest_chunk_index") or 0)
    latest_chunk_count = int(summary.get("latest_chunk_count") or 0)
    latest_bytes_transferred = int(summary.get("latest_bytes_transferred") or 0)
    latest_total_bytes = int(summary.get("latest_total_bytes") or 0)
    latest_event_type = str(summary.get("latest_event_type") or "").strip().replace("_", "-")
    failure_code_counts = (
        summary.get("failure_code_counts")
        if isinstance(summary.get("failure_code_counts"), dict)
        else {}
    )
    failure_codes = ", ".join(
        f"{safe_label(code)}:{int(count or 0)}"
        for code, count in sorted(failure_code_counts.items())[:5]
        if int(count or 0) > 0 and safe_label(code)
    )
    latest_failure_code = safe_label(summary.get("latest_failure_code"))
    parts = [f"recorded {recorded_count}", f"failed {failed_count}"]
    if actor_transform_count:
        parts.append(f"actor-transform {actor_transform_count}")
    if actor_delete_count:
        parts.append(f"actor-delete {actor_delete_count}")
    if peer_join_count:
        parts.append(f"peer-join {peer_join_count}")
    if peer_leave_count:
        parts.append(f"peer-leave {peer_leave_count}")
    if transfer_failed_count:
        parts.append(f"transfer-failed {transfer_failed_count}")
    if transfer_progress_count:
        transfer_parts = [f"transfer-progress {transfer_progress_count}"]
        progress_bits: list[str] = []
        if latest_transfer_progress:
            progress_bits.append(f"{max(0, min(100, latest_transfer_progress))}%")
        if latest_chunk_index and latest_chunk_count:
            progress_bits.append(f"chunk {latest_chunk_index}/{latest_chunk_count}")
        byte_text = _format_transfer_bytes(latest_bytes_transferred, latest_total_bytes)
        if byte_text:
            progress_bits.append(byte_text.strip())
        if progress_bits:
            transfer_parts.append("latest " + " ".join(progress_bits))
        parts.append(" ".join(transfer_parts))
    if latest_event_type:
        parts.append(f"latest {latest_event_type[:48]}")
    if failure_codes:
        parts.append(f"failure codes {failure_codes}")
    if latest_failure_code:
        parts.append(f"latest failure {latest_failure_code}")
    return ", ".join(parts)
