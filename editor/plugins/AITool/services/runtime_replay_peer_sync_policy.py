"""Stateless formatting policy for replay peer-sync summaries."""

from __future__ import annotations

from typing import Any


def format_agent_runtime_replay_peer_sync_report(summary: Any) -> str:
    """Format peer and reconciliation counters without exposing peer identity."""

    if not isinstance(summary, dict):
        return "events 0, join 0, leave 0, room-close 0, reconcile 0/0, state 0/0"
    event_count = int(summary.get("peer_event_count") or 0)
    join_count = int(summary.get("peer_join_count") or 0)
    leave_count = int(summary.get("peer_leave_count") or 0)
    room_close_count = int(summary.get("room_close_count") or 0)
    sync_reconcile_count = int(summary.get("sync_reconcile_count") or 0)
    sync_reconcile_failed_count = int(summary.get("sync_reconcile_failed_count") or 0)
    state_reconcile_count = int(summary.get("state_reconcile_count") or 0)
    state_reconcile_failed_count = int(summary.get("state_reconcile_failed_count") or 0)
    latest_peer_event_type = str(summary.get("latest_peer_event_type") or "").strip().replace("_", "-")
    latest_room_status = str(summary.get("latest_room_status") or "").strip().replace("_", "-")
    latest_reconcile_event = (
        summary.get("latest_reconcile_event")
        if isinstance(summary.get("latest_reconcile_event"), dict)
        else {}
    )
    latest_reconcile_status = str(
        latest_reconcile_event.get("status") if isinstance(latest_reconcile_event, dict) else ""
    ).strip().replace("_", "-")
    parts = [
        f"events {event_count}",
        f"join {join_count}",
        f"leave {leave_count}",
        f"room-close {room_close_count}",
        f"reconcile {sync_reconcile_count}/{sync_reconcile_failed_count}",
        f"state {state_reconcile_count}/{state_reconcile_failed_count}",
    ]
    if latest_peer_event_type:
        parts.append(f"latest-peer {latest_peer_event_type[:32]}")
    if latest_room_status:
        parts.append(f"room {latest_room_status[:24]}")
    if latest_reconcile_status:
        parts.append(f"latest-reconcile {latest_reconcile_status[:24]}")
    return ", ".join(parts)
