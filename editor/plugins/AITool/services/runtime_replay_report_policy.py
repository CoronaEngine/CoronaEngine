"""Stateless formatting policy for AgentRuntime replay summaries."""

from __future__ import annotations

import re
from typing import Any


def format_agent_runtime_replay_report(
    summary: Any,
    *,
    runtime_event_text: str = "",
    worker_drain_text: str = "",
    engine_write_boundary_text: str = "",
) -> str:
    """Compose a replay report from structured counts and preformatted subreports."""

    if not isinstance(summary, dict) or not summary:
        return "entries 0"
    entry_count = int(summary.get("entry_count") or 0)
    event_counts = summary.get("event_counts") if isinstance(summary.get("event_counts"), dict) else {}
    latest_events = summary.get("latest_events") if isinstance(summary.get("latest_events"), list) else []
    environment_replay = (
        summary.get("environment_component_replay_summary")
        if isinstance(summary.get("environment_component_replay_summary"), dict)
        else {}
    )
    runtime_event_replay = (
        summary.get("runtime_event_replay_summary")
        if isinstance(summary.get("runtime_event_replay_summary"), dict)
        else {}
    )
    worker_drain_replay = (
        summary.get("worker_drain_replay_summary")
        if isinstance(summary.get("worker_drain_replay_summary"), dict)
        else {}
    )
    engine_write_boundary = (
        summary.get("engine_write_boundary_summary")
        if isinstance(summary.get("engine_write_boundary_summary"), dict)
        else {}
    )

    def safe_event(value: Any) -> str:
        event = str(value or "").strip()
        if not event:
            return ""
        for marker in ("provider", "prompt", "url", "raw"):
            event = re.sub(marker, "runtime", event, flags=re.IGNORECASE)
        return event.replace("_", "-")[:80]

    priority_events = [
        "scene_plan_created",
        "scene_plan_confirmed",
        "batch_plan_created",
        "tool_graph_queued",
        "tool_graph_completed",
        "user_report_generated",
    ]
    count_parts: list[str] = []
    for key in priority_events:
        value = int(event_counts.get(key) or 0)
        if value > 0:
            count_parts.append(f"{safe_event(key)}:{value}")
    if not count_parts:
        for key, value in sorted(event_counts.items())[:4]:
            if int(value or 0) > 0:
                count_parts.append(f"{safe_event(key)}:{int(value or 0)}")
    recent: list[str] = []
    for item in latest_events[-3:]:
        event = safe_event(item.get("event") if isinstance(item, dict) else item)
        if event:
            recent.append(event)
    parts = [f"entries {entry_count}"]
    if count_parts:
        parts.append("events " + ",".join(count_parts[:4]))
    env_import_count = int(environment_replay.get("import_event_count") or 0)
    env_import_failed = int(environment_replay.get("import_failed_event_count") or 0)
    if env_import_count or env_import_failed:
        env_bits = []
        if env_import_count:
            env_bits.append(f"env-import:{env_import_count}")
        if env_import_failed:
            env_bits.append(f"env-import-failed:{env_import_failed}")
        parts.append("environment " + ",".join(env_bits))
    if int(runtime_event_replay.get("disclosure_skipped_count") or 0) > 0 and runtime_event_text:
        parts.append("runtime-events " + runtime_event_text)
    drain_failed = int(worker_drain_replay.get("failed_count") or event_counts.get("runtime_worker_drain_failed") or 0)
    drain_exception = int(worker_drain_replay.get("exception_count") or event_counts.get("runtime_worker_drain_exception") or 0)
    drain_status_failed = int(
        worker_drain_replay.get("status_failed_count")
        or event_counts.get("runtime_worker_drain_status_failed")
        or 0
    )
    if (drain_failed or drain_exception or drain_status_failed) and worker_drain_text:
        parts.append("worker-drain " + worker_drain_text)
    if int(engine_write_boundary.get("boundary_fact_count") or 0) > 0 and engine_write_boundary_text:
        parts.append("engine_write_boundary " + engine_write_boundary_text)
    if recent:
        parts.append("recent " + ",".join(recent[:3]))
    return "；".join(parts)


def format_agent_runtime_replay_command_report(summary: Any) -> str:
    if not isinstance(summary, dict) or not summary:
        return "none"
    command_count = int(summary.get("command_count") or 0)
    if command_count <= 0:
        return "none"

    def safe_text(value: Any) -> str:
        text = str(value or "").strip()
        for marker in ("provider", "prompt", "url", "raw", "token", "api_key"):
            text = re.sub(marker, "runtime", text, flags=re.IGNORECASE)
        return text.replace("_", "-")[:80]

    cancelled_batches = int(summary.get("cancelled_batch_total") or 0)
    cancelled_graphs = int(summary.get("cancelled_graph_total") or 0)
    resumed_graphs = int(summary.get("resumed_graph_total") or 0)
    retried_graphs = int(summary.get("retried_graph_total") or 0)
    parts = [f"{command_count} command(s)"]
    if cancelled_batches or cancelled_graphs:
        parts.append(f"cancelled batch/graph {cancelled_batches}/{cancelled_graphs}")
    if resumed_graphs:
        parts.append(f"resumed graphs {resumed_graphs}")
    if retried_graphs:
        parts.append(f"retried graphs {retried_graphs}")
    latest = summary.get("latest_command") if isinstance(summary.get("latest_command"), dict) else {}
    command = safe_text(latest.get("command"))
    old_status = safe_text(latest.get("old_status"))
    new_status = safe_text(latest.get("new_status"))
    if command:
        if old_status or new_status:
            parts.append(f"latest {command}:{old_status or '?'}->{new_status or '?'}")
        else:
            parts.append(f"latest {command}")
    return ", ".join(parts)


def format_agent_runtime_replay_tool_execution_report(summary: Any) -> str:
    if not isinstance(summary, dict) or not summary:
        return "started 0, succeeded 0, failed 0"
    started = int(summary.get("started_count") or 0)
    succeeded = int(summary.get("succeeded_count") or 0)
    failed = int(summary.get("failed_count") or 0)
    blocked = int(summary.get("blocked_count") or 0)
    retry_scheduled = int(summary.get("retry_scheduled_count") or 0)
    skipped = int(summary.get("skipped_count") or 0)
    parts = [f"started {started}", f"succeeded {succeeded}", f"failed {failed}", f"blocked {blocked}"]
    if retry_scheduled:
        parts.append(f"retry {retry_scheduled}")
    if skipped:
        parts.append(f"skipped {skipped}")
    latest = summary.get("latest_tool_event") if isinstance(summary.get("latest_tool_event"), dict) else {}
    event = str(latest.get("event") or "").strip().replace("_", "-")
    status = str(latest.get("status") or "").strip().replace("_", "-")
    if event:
        parts.append(f"latest {event}:{status or 'unknown'}")
    return ", ".join(parts)


def format_agent_runtime_replay_tool_queue_report(summary: Any) -> str:
    if not isinstance(summary, dict) or not summary:
        return "queued 0, dequeued 0, completed 0"
    queued = int(summary.get("queued_count") or 0)
    dequeued = int(summary.get("dequeued_count") or 0)
    completed = int(summary.get("completed_count") or 0)
    rejected = int(summary.get("rejected_count") or 0)
    empty = int(summary.get("empty_count") or 0)
    blocked = int(summary.get("blocked_count") or 0)
    missing_graph = int(summary.get("missing_graph_count") or 0)
    parts = [f"queued {queued}", f"dequeued {dequeued}", f"completed {completed}"]
    if rejected:
        parts.append(f"rejected {rejected}")
    if empty:
        parts.append(f"empty {empty}")
    if blocked:
        parts.append(f"blocked {blocked}")
    if missing_graph:
        parts.append(f"missing {missing_graph}")
    latest = summary.get("latest_queue_event") if isinstance(summary.get("latest_queue_event"), dict) else {}
    event = str(latest.get("event") or "").strip().replace("_", "-")
    status = str(latest.get("status") or "").strip().replace("_", "-")
    if event:
        parts.append(f"latest {event}:{status or 'unknown'}")
    return ", ".join(parts)


def format_agent_runtime_replay_state_patch_report(summary: Any) -> str:
    if not isinstance(summary, dict) or not summary:
        return "applied 0, conflict 0, invalid 0"
    version_stamped = int(summary.get("version_stamped") or 0)
    applied = int(summary.get("applied") or 0)
    conflict = int(summary.get("conflict") or 0)
    invalid = int(summary.get("invalid") or 0)
    reconciled = int(summary.get("reconciled") or 0)
    reconcile_failed = int(summary.get("reconcile_failed") or 0)
    parts = [f"versioned {version_stamped}", f"applied {applied}", f"conflict {conflict}", f"invalid {invalid}"]
    if reconciled:
        parts.append(f"reconciled {reconciled}")
    if reconcile_failed:
        parts.append(f"reconcile-failed {reconcile_failed}")
    latest_events = summary.get("latest_events") if isinstance(summary.get("latest_events"), list) else []
    latest = latest_events[-1] if latest_events and isinstance(latest_events[-1], dict) else {}
    event = str(latest.get("event") or "").strip().replace("_", "-")
    applied_version = latest.get("applied_version")
    if event:
        suffix = f":v{applied_version}" if isinstance(applied_version, int) else ""
        parts.append(f"latest {event}{suffix}")
    return ", ".join(parts)


def format_agent_runtime_replay_guard_report(summary: Any) -> str:
    if not isinstance(summary, dict) or not summary:
        return "blocked 0"
    blocked = int(summary.get("blocked_count") or 0)
    high_risk = int(summary.get("high_risk_confirmation_required_count") or 0)
    write_confirm = int(summary.get("write_confirmation_required_count") or 0)
    system_actor = int(summary.get("system_actor_write_blocked_count") or 0)
    visible_blocked = int(summary.get("user_visible_blocked_event_count") or 0)
    requires_write = int(summary.get("requires_write_blocked_count") or 0)
    unconfirmed = int(summary.get("unconfirmed_blocked_count") or 0)
    confirmed = int(summary.get("confirmed_blocked_count") or 0)
    risk_counts = summary.get("risk_level_counts") if isinstance(summary.get("risk_level_counts"), dict) else {}
    risk_parts: list[str] = []
    for risk in ("high", "medium", "low", "unknown"):
        try:
            count = int(risk_counts.get(risk) or 0)
        except (TypeError, ValueError):
            count = 0
        if count:
            risk_parts.append(f"{risk}:{count}")
    parts = [f"blocked {blocked}"]
    if high_risk:
        parts.append(f"high-risk-confirm {high_risk}")
    if write_confirm:
        parts.append(f"write-confirm {write_confirm}")
    if system_actor:
        parts.append(f"system-actor {system_actor}")
    if visible_blocked:
        parts.append(f"visible-blocked {visible_blocked}")
    if requires_write:
        parts.append(f"write-blocked {requires_write}")
    if unconfirmed:
        parts.append(f"unconfirmed {unconfirmed}")
    if confirmed:
        parts.append(f"confirmed-blocked {confirmed}")
    if risk_parts:
        parts.append("risk " + "/".join(risk_parts))
    latest = summary.get("latest_block") if isinstance(summary.get("latest_block"), dict) else {}
    reason = str(latest.get("reason") or "").strip().replace("_", "-")
    if reason:
        latest_risk = str(latest.get("risk_level") or "").strip().replace("_", "-")
        suffix_parts = [f"risk:{latest_risk}"] if latest_risk else []
        if latest.get("requires_write"):
            suffix_parts.append("write")
        suffix_parts.append("confirmed" if latest.get("confirmed") else "unconfirmed")
        parts.append(f"latest {reason} {'/'.join(suffix_parts)}")
    return ", ".join(parts)


def format_agent_runtime_worker_drain_replay_report(summary: Any) -> str:
    """Format worker-drain replay counters without coordinating the drain."""

    if not isinstance(summary, dict) or not summary:
        return "requested 0, drained 0, failed 0, exception 0"
    requested = int(summary.get("requested_count") or 0)
    drained_messages = int(summary.get("message_drained_count") or 0)
    failed = int(summary.get("failed_count") or 0)
    exception = int(summary.get("exception_count") or 0)
    status_failed = int(summary.get("status_failed_count") or 0)
    plan_resolve_failed = int(summary.get("plan_resolve_failed_count") or 0)
    drained_graph_total = int(summary.get("drained_graph_total") or 0)
    parts = [
        f"requested {requested}",
        f"drained {drained_messages}/{drained_graph_total}",
        f"failed {failed}",
        f"exception {exception}",
    ]
    if status_failed:
        parts.append(f"status-failed {status_failed}")
    if plan_resolve_failed:
        parts.append(f"plan-resolve-failed {plan_resolve_failed}")
    latest = summary.get("latest_drain_event") if isinstance(summary.get("latest_drain_event"), dict) else {}
    latest_event = str(latest.get("event") or "").strip().replace("_", "-")
    if latest_event:
        parts.append(f"latest {latest_event[:48]}")
    return ", ".join(parts)


def format_agent_runtime_tool_graph_replay_report(
    batch_summary: Any,
    queue_summary: Any,
) -> str:
    """Format bounded tool graph batch and queue replay counters."""

    if not isinstance(batch_summary, dict):
        batch_summary = {}
    if not isinstance(queue_summary, dict):
        queue_summary = {}

    def count(source: dict[str, Any], name: str) -> int:
        try:
            return max(0, int(source.get(name) or 0))
        except (TypeError, ValueError):
            return 0

    return (
        "batch start/done/final "
        f"{count(batch_summary, 'started_count')}/"
        f"{count(batch_summary, 'completed_count')}/"
        f"{count(batch_summary, 'finalized_count')}, "
        "queue queued/dequeued/rejected/blocked "
        f"{count(queue_summary, 'queued_count')}/"
        f"{count(queue_summary, 'dequeued_count')}/"
        f"{count(queue_summary, 'rejected_count')}/"
        f"{count(queue_summary, 'blocked_count')}"
    )


def format_agent_runtime_gm_summary_replay_report(summary: Any) -> str:
    """Format GM summary replay export and readiness counters."""

    if not isinstance(summary, dict) or not summary:
        return "exported 0, failed 0, readiness publish/query 0/0"

    def count(name: str) -> int:
        try:
            return max(0, int(summary.get(name) or 0))
        except (TypeError, ValueError):
            return 0

    exported = count("exported_count")
    failed = count("failed_count")
    available = count("available_count")
    scene_plan = count("scene_plan_count")
    readiness_publish = count("resource_readiness_publish_total")
    readiness_query = count("resource_readiness_query_total")
    return (
        f"exported {exported}, failed {failed}, available {available}, "
        f"scene-plan {scene_plan}, readiness publish/query {readiness_publish}/{readiness_query}"
    )
