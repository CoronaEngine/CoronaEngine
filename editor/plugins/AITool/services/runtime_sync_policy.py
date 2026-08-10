"""Stateless formatting policy for Runtime Sync row previews."""

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

    transferred = max(0, int(bytes_transferred or 0))
    total = max(0, int(total_bytes or 0))
    if transferred and total:
        return f" {human(transferred)}/{human(total)}"
    if transferred:
        return f" {human(transferred)}"
    return ""


def format_agent_runtime_sync_actor_rows(rows: Any) -> str:
    """Format a bounded actor preview without reading Runtime state."""

    if not isinstance(rows, list) or not rows:
        return "none"
    formatted: list[str] = []
    for item in rows[:5]:
        if not isinstance(item, dict):
            continue
        actor = str(item.get("actor_name") or item.get("actor_id") or "actor")
        event_type = str(item.get("event_type") or "")
        lifecycle = str(item.get("lifecycle_status") or "")
        status = lifecycle or event_type or "updated"
        formatted.append(f"{actor}:{status}")
    return ", ".join(formatted) or "none"


def format_agent_runtime_sync_asset_rows(rows: Any) -> str:
    """Format a bounded asset transfer preview without exposing paths."""

    if not isinstance(rows, list) or not rows:
        return "none"
    formatted: list[str] = []
    for item in rows[:5]:
        if not isinstance(item, dict):
            continue
        asset = str(item.get("asset_id") or "asset")
        transfer_status = str(
            item.get("transfer_status") or item.get("status") or item.get("event_type") or "unknown"
        )
        progress = int(item.get("progress") or 0)
        chunk_index = int(item.get("chunk_index") or 0)
        chunk_count = int(item.get("chunk_count") or 0)
        bytes_transferred = int(item.get("bytes_transferred") or 0)
        total_bytes = int(item.get("total_bytes") or 0)
        progress_text = f" {progress}%" if progress else ""
        chunk_text = f" chunk {chunk_index}/{chunk_count}" if chunk_index and chunk_count else ""
        byte_text = _format_transfer_bytes(bytes_transferred, total_bytes)
        formatted.append(f"{asset}:{transfer_status}{progress_text}{chunk_text}{byte_text}")
    return ", ".join(formatted) or "none"


def format_agent_runtime_sync_health_report(summary: Any) -> str:
    """Format sync health counters without querying the runtime."""

    if not isinstance(summary, dict) or not summary:
        return "unknown"
    status = str(summary.get("status") or "unknown").strip() or "unknown"
    needs_attention = [
        str(item).strip().replace("_", "-")
        for item in list(summary.get("needs_attention") or [])[:4]
        if str(item).strip()
    ]
    actor_create_count = int(summary.get("actor_create_count") or 0)
    actor_transform_count = int(summary.get("actor_transform_count") or 0)
    actor_delete_count = int(summary.get("actor_delete_count") or 0)
    active_actor_count = int(summary.get("latest_active_actor_count") or 0)
    peer_join_count = int(summary.get("peer_join_count") or 0)
    peer_leave_count = int(summary.get("peer_leave_count") or 0)
    room_close_count = int(summary.get("room_close_count") or 0)
    parts = [
        status,
        f"attention {len(needs_attention)}",
        f"actors create/transform/delete {actor_create_count}/{actor_transform_count}/{actor_delete_count}",
        f"active {active_actor_count}",
    ]
    if peer_join_count or peer_leave_count:
        parts.append(f"peers join/leave {peer_join_count}/{peer_leave_count}")
    if room_close_count:
        parts.append(f"room-close {room_close_count}")
    if needs_attention:
        parts.append("needs " + ",".join(needs_attention))
    return ", ".join(parts)


def format_agent_runtime_sync_report(summary: Any) -> str:
    """Format the aggregate Runtime Sync report without reading engine state."""

    if not isinstance(summary, dict):
        return "events 0, actors 0, assets 0"
    event_count = int(summary.get("event_count") or 0)
    actor_count = int(summary.get("actor_event_count") or 0)
    asset_count = int(summary.get("asset_event_count") or 0)
    latest_actors = summary.get("latest_actors")
    actor_preview = format_agent_runtime_sync_actor_rows(
        latest_actors if isinstance(latest_actors, list) else []
    )
    latest_assets = summary.get("latest_assets")
    asset_preview = format_agent_runtime_sync_asset_rows(
        latest_assets if isinstance(latest_assets, list) else []
    )
    parts = [f"events {event_count}", f"actors {actor_count}", f"assets {asset_count}"]
    if actor_preview != "none":
        parts.append(f"latest actors {actor_preview}")
    if asset_preview != "none":
        parts.append(f"latest assets {asset_preview}")
    return ", ".join(parts)


def format_agent_runtime_gm_sync_replay_digest(digest: Any) -> str:
    """Format GM Runtime Sync replay counters with redacted labels."""

    if not isinstance(digest, dict) or not digest:
        return "recorded 0, asset progress 0, peer join/leave 0/0, reconcile 0/0"

    def safe_label(value: Any) -> str:
        text = str(value or "").strip().replace("_", "-")
        for marker in ("prompt", "provider", "url", "raw", "token", "api-key", "path", "session", "job"):
            text = re.sub(marker, "resource", text, flags=re.IGNORECASE)
        return text[:60]

    recorded_count = int(digest.get("recorded_count") or 0)
    failed_count = int(digest.get("failed_count") or 0)
    actor_transform_count = int(digest.get("actor_transform_count") or 0)
    actor_delete_count = int(digest.get("actor_delete_count") or 0)
    asset_progress_count = int(digest.get("asset_transfer_progress_count") or 0)
    asset_completed_count = int(digest.get("asset_transfer_completed_count") or 0)
    asset_failed_count = int(digest.get("asset_transfer_failed_count") or 0)
    peer_ready_count = int(digest.get("peer_asset_ready_count") or 0)
    peer_join_count = int(digest.get("peer_join_count") or 0)
    peer_leave_count = int(digest.get("peer_leave_count") or 0)
    sync_reconcile_count = int(digest.get("sync_reconcile_count") or 0)
    sync_reconcile_failed_count = int(digest.get("sync_reconcile_failed_count") or 0)
    failure_code_counts = (
        digest.get("failure_code_counts")
        if isinstance(digest.get("failure_code_counts"), dict)
        else {}
    )
    failure_codes = ", ".join(
        f"{safe_label(code)}:{int(count or 0)}"
        for code, count in sorted(failure_code_counts.items())[:5]
        if int(count or 0) > 0 and safe_label(code)
    )
    latest_failure_code = safe_label(digest.get("latest_failure_code"))
    parts = [
        f"recorded {recorded_count}/{failed_count}",
        f"actor transform/delete {actor_transform_count}/{actor_delete_count}",
        f"asset progress {asset_progress_count}",
        f"asset completed/failed {asset_completed_count}/{asset_failed_count}",
        f"peer-ready {peer_ready_count}",
        f"peer join/leave {peer_join_count}/{peer_leave_count}",
        f"reconcile {sync_reconcile_count}/{sync_reconcile_failed_count}",
        *([f"failure codes {failure_codes}"] if failure_codes else []),
        *([f"latest failure {latest_failure_code}"] if latest_failure_code else []),
    ]
    return "；".join(parts)


def format_agent_runtime_asset_transfer_report(summary: Any) -> str:
    """Format aggregate asset transfer counters without performing transfers."""

    if not isinstance(summary, dict) or not summary:
        return "none"
    asset_count = int(summary.get("asset_count") or 0)
    if asset_count <= 0:
        return "none"
    ready_count = int(summary.get("ready_count") or 0)
    failed_count = int(summary.get("failed_count") or 0)
    transferring_count = int(summary.get("transferring_count") or 0)
    completed_count = int(summary.get("completed_count") or 0)
    progress = int(summary.get("overall_progress") or 0)
    bytes_transferred = int(summary.get("bytes_transferred") or 0)
    total_bytes = int(summary.get("total_bytes") or 0)
    parts = [
        f"assets {asset_count}",
        f"ready {ready_count}",
        f"completed {completed_count}",
        f"transferring {transferring_count}",
        f"failed {failed_count}",
    ]
    if progress:
        parts.append(f"progress {max(0, min(100, progress))}%")
    if total_bytes > 0:
        parts.append(f"bytes {bytes_transferred}/{total_bytes}")
    latest_assets = summary.get("latest_assets")
    if isinstance(latest_assets, list) and latest_assets:
        latest = latest_assets[-1] if isinstance(latest_assets[-1], dict) else {}
        asset_id = str(latest.get("asset_id") or "").strip()
        status = str(latest.get("transfer_status") or "").strip()
        if asset_id or status:
            parts.append(f"latest {asset_id or 'asset'}:{status or 'unknown'}")
    return ", ".join(parts)
