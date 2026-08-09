"""Stateless formatting policy for replay lifecycle summaries."""

from __future__ import annotations

from typing import Any


def format_agent_runtime_replay_plan_lifecycle_report(summary: Any) -> str:
    if not isinstance(summary, dict) or not summary:
        return "created 0, confirmed 0"
    created = int(summary.get("created_count") or 0)
    confirmed = int(summary.get("confirmed_count") or 0)
    state_persisted = int(summary.get("state_persisted_count") or 0)
    state_failed = int(summary.get("state_persist_failed_count") or 0)
    status_persisted = int(summary.get("status_persisted_count") or 0)
    status_failed = int(summary.get("status_persist_failed_count") or 0)
    extracted = int(summary.get("extracted_count") or 0)
    parts = [
        f"created {created}",
        f"confirmed {confirmed}",
        f"state {state_persisted}/{state_failed}",
        f"status {status_persisted}/{status_failed}",
    ]
    if extracted:
        parts.append(f"extracted {extracted}")
    latest = summary.get("latest_plan_event") if isinstance(summary.get("latest_plan_event"), dict) else {}
    event = str(latest.get("event") or "").strip().replace("_", "-")
    status = str(latest.get("status") or "").strip().replace("_", "-")
    if event:
        parts.append(f"latest {event}:{status or 'unknown'}")
    return ", ".join(parts)


def format_agent_runtime_replay_intervention_report(summary: Any) -> str:
    if not isinstance(summary, dict) or not summary:
        return "routed 0, queued 0, absorbed 0"
    routed = int(summary.get("routed_count") or 0)
    queued = int(summary.get("queued_count") or 0)
    persisted = int(summary.get("persisted_count") or 0)
    persist_failed = int(summary.get("persist_failed_count") or 0)
    skipped = int(summary.get("skipped_count") or 0)
    enqueue_failed = int(summary.get("enqueue_failed_count") or 0)
    absorbed = int(summary.get("absorbed_count") or 0)
    route_absorbable = int(summary.get("route_absorbable_count") or 0)
    route_non_absorbable = int(summary.get("route_non_absorbable_count") or 0)
    route_requested_items = int(summary.get("route_requested_item_count") or 0)
    merge_events = int(summary.get("merge_event_count") or 0)
    merged_items = int(summary.get("merged_item_count") or 0)
    merge_absorbed = int(summary.get("merge_absorbed_count") or 0)
    parts = [
        f"routed {routed}",
        f"queued {queued}",
        f"persisted {persisted}/{persist_failed}",
        f"absorbed {absorbed}",
    ]
    if route_absorbable or route_non_absorbable or route_requested_items:
        parts.append(f"route {route_absorbable}/{route_non_absorbable} items {route_requested_items}")
    if merge_events or merged_items or merge_absorbed:
        parts.append(f"merge {merge_events} items {merged_items} absorbed {merge_absorbed}")
    if skipped:
        parts.append(f"skipped {skipped}")
    if enqueue_failed:
        parts.append(f"enqueue-failed {enqueue_failed}")
    latest = summary.get("latest_intervention_batch") if isinstance(summary.get("latest_intervention_batch"), dict) else {}
    event = str(latest.get("event") or "").strip().replace("_", "-")
    status = str(latest.get("status") or "").strip().replace("_", "-")
    item_count = int(latest.get("item_count") or 0)
    if event:
        parts.append(f"latest {event}:{status or 'unknown'} items {item_count}")
    return ", ".join(parts)


def format_agent_runtime_replay_geometry_report(summary: Any) -> str:
    if not isinstance(summary, dict) or not summary:
        return "facts 0, overlap 0"
    patch_events = int(summary.get("patch_event_count") or 0)
    fact_count = int(summary.get("fact_count") or 0)
    aabb_actor_count = int(summary.get("aabb_actor_count") or 0)
    aabb_skipped_count = int(summary.get("aabb_skipped_count") or 0)
    overlap_issue_count = int(summary.get("overlap_issue_count") or 0)
    parts = [
        f"patches {patch_events}",
        f"facts {fact_count}",
        f"aabb {aabb_actor_count}/{aabb_skipped_count}",
        f"overlap {overlap_issue_count}",
    ]
    status_counts = summary.get("status_counts") if isinstance(summary.get("status_counts"), dict) else {}
    if status_counts:
        status_text = ", ".join(
            f"{str(status).strip().replace('_', '-')}:{int(count or 0)}"
            for status, count in sorted(status_counts.items())
            if str(status).strip()
        )
        if status_text:
            parts.append(f"status {status_text}")
    fact_type_counts = summary.get("fact_type_counts") if isinstance(summary.get("fact_type_counts"), dict) else {}
    if fact_type_counts:
        type_text = ", ".join(
            f"{str(fact_type).strip().replace('_', '-')}:{int(count or 0)}"
            for fact_type, count in sorted(fact_type_counts.items())
            if str(fact_type).strip()
        )
        if type_text:
            parts.append(f"types {type_text}")
    latest = summary.get("latest_geometry_event") if isinstance(summary.get("latest_geometry_event"), dict) else {}
    latest_type = str(latest.get("fact_type") or "").strip().replace("_", "-")
    latest_status = str(latest.get("status") or "").strip().replace("_", "-")
    latest_actor_count = int(latest.get("actor_count") or 0)
    latest_issue_count = int(latest.get("issue_count") or 0)
    latest_skipped_count = int(latest.get("skipped_count") or 0)
    if latest_type:
        parts.append(
            f"latest {latest_type}:{latest_status or 'unknown'} "
            f"actors {latest_actor_count} issues {latest_issue_count} skipped {latest_skipped_count}"
        )
    return ", ".join(parts)
