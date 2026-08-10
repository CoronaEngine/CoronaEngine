"""Stateless formatting policy for replay asset-transfer summaries."""

from __future__ import annotations

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


def format_agent_runtime_replay_asset_transfer_report(summary: Any) -> str:
    """Format replay transfer counters without exposing asset identity or paths."""

    if not isinstance(summary, dict):
        return "events 0, started 0, progress 0, completed 0, failed 0"
    event_count = int(summary.get("asset_event_count") or 0)
    started_count = int(summary.get("asset_transfer_started_count") or 0)
    progress_count = int(summary.get("asset_transfer_progress_count") or 0)
    completed_count = int(summary.get("asset_transfer_completed_count") or 0)
    failed_count = int(summary.get("asset_transfer_failed_count") or 0)
    peer_ready_count = int(summary.get("peer_asset_ready_count") or 0)
    latest_progress = int(summary.get("latest_transfer_progress") or 0)
    latest_chunk_index = int(summary.get("latest_chunk_index") or 0)
    latest_chunk_count = int(summary.get("latest_chunk_count") or 0)
    latest_bytes_transferred = int(summary.get("latest_bytes_transferred") or 0)
    latest_total_bytes = int(summary.get("latest_total_bytes") or 0)
    latest_status = str(summary.get("latest_transfer_status") or "").strip().replace("_", "-")
    parts = [
        f"events {event_count}",
        f"started {started_count}",
        f"progress {progress_count}",
        f"completed {completed_count}",
        f"failed {failed_count}",
    ]
    if peer_ready_count:
        parts.append(f"peer-ready {peer_ready_count}")
    latest_bits: list[str] = []
    if latest_progress:
        latest_bits.append(f"{max(0, min(100, latest_progress))}%")
    if latest_chunk_index and latest_chunk_count:
        latest_bits.append(f"chunk {latest_chunk_index}/{latest_chunk_count}")
    byte_text = _format_transfer_bytes(latest_bytes_transferred, latest_total_bytes)
    if byte_text:
        latest_bits.append(byte_text.strip())
    if latest_status:
        latest_bits.append(latest_status[:24])
    if latest_bits:
        parts.append("latest " + " ".join(latest_bits))
    return ", ".join(parts)
