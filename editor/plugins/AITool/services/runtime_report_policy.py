"""Stateless formatting policy for AgentRuntime scene reports.

The worker owns report retrieval and routing.  This module only converts
already-structured summaries into bounded, redacted display text.
"""

from __future__ import annotations

import re
from typing import Any


def format_agent_runtime_short_list(
    value: Any,
    *,
    fallback: str = "none",
    limit: int = 6,
) -> str:
    if not isinstance(value, list):
        return fallback
    items = [str(item).strip() for item in value if str(item).strip()]
    if not items:
        return fallback
    item_limit = max(1, int(limit or 1))
    preview = "、".join(items[:item_limit])
    if len(items) > item_limit:
        preview += f" 等 {len(items)} 项"
    return preview


def format_agent_runtime_resource_flow_report(summary: Any) -> str:
    """Format batch resource-flow counters and bounded failure labels."""

    if not isinstance(summary, dict) or not summary:
        return "batches 0"

    def safe_label(value: Any) -> str:
        text = str(value or "").strip().replace("_", "-")
        for marker in ("prompt", "provider", "url", "raw", "token", "api-key", "path", "session", "job"):
            text = re.sub(marker, "resource", text, flags=re.IGNORECASE)
        return text[:60]

    batch_count = int(summary.get("batch_count") or 0)
    completed_count = int(summary.get("completed_count") or 0)
    partial_count = int(summary.get("partial_count") or 0)
    failed_count = int(summary.get("failed_count") or 0)
    waiting_count = int(summary.get("waiting_count") or 0)
    parts = [
        f"batches {batch_count}",
        f"completed {completed_count}",
        f"partial {partial_count}",
        f"failed {failed_count}",
        f"waiting {waiting_count}",
    ]
    latest = summary.get("latest_batch") if isinstance(summary.get("latest_batch"), dict) else {}
    latest_status = str(latest.get("status") or "").strip()
    latest_index = int(latest.get("batch_index") or 0)
    latest_total = int(latest.get("total_batches") or 0)
    requested_count = int(latest.get("requested_count") or 0)
    image_ready_count = int(latest.get("image_ready_count") or 0)
    model_ready_count = int(latest.get("model_ready_count") or 0)
    import_ready_count = int(latest.get("import_ready_count") or 0)
    import_failure_code_counts = (
        latest.get("import_failure_code_counts")
        if isinstance(latest.get("import_failure_code_counts"), dict)
        else {}
    )
    review_status = str(latest.get("review_status") or "").strip()
    if latest_status or latest_index or requested_count:
        batch_label = (
            f"{latest_index}/{latest_total}"
            if latest_index and latest_total
            else str(latest_index or "?")
        )
        parts.append(
            "latest "
            f"{batch_label}:{latest_status or 'unknown'} "
            f"img/model/import {image_ready_count}/{model_ready_count}/{import_ready_count}"
            f" of {requested_count}"
        )
    if review_status:
        parts.append(f"review {review_status.replace('_', '-')[:40]}")
    import_failure_codes = ",".join(
        f"{safe_label(code)}:{int(count or 0)}"
        for code, count in sorted(import_failure_code_counts.items())[:4]
        if safe_label(code) and int(count or 0) > 0
    )
    if import_failure_codes:
        parts.append(f"import-failures {import_failure_codes}")
    needs_attention = [
        str(item).strip().replace("_", "-")[:40]
        for item in list(summary.get("needs_attention") or [])[:4]
        if str(item).strip()
    ]
    if needs_attention:
        parts.append("needs " + ",".join(needs_attention))
    return ", ".join(parts)


def format_agent_runtime_scene_snapshot_report(summary: Any) -> str:
    """Format bounded scene snapshot counters."""

    if not isinstance(summary, dict) or not summary:
        return "snapshots 0, observed 0"
    snapshot_count = int(summary.get("scoped_snapshot_count") or summary.get("snapshot_count") or 0)
    observed_count = int(summary.get("observed_actor_count") or 0)
    observed_total = int(summary.get("observed_actor_total_count") or observed_count or 0)
    latest_source = str(summary.get("latest_source") or "").strip().replace("_", "-")[:40]
    parts = [
        f"snapshots {snapshot_count}",
        f"observed {observed_count}/{observed_total}",
    ]
    if latest_source:
        parts.append(f"source {latest_source}")
    return ", ".join(parts)


def format_agent_runtime_resource_stage_report(summary: Any) -> str:
    """Format resource stage counters and latest stage status."""

    if not isinstance(summary, dict) or not summary:
        return "events 0"
    by_phase = summary.get("by_phase") if isinstance(summary.get("by_phase"), dict) else {}
    parts = [f"events {int(summary.get('event_count') or 0)}"]
    ordered_phases = ["image", "model", "import", "review"]
    extra_phases = [
        str(phase)
        for phase in by_phase.keys()
        if str(phase) not in set(ordered_phases)
    ]
    for phase in ordered_phases + sorted(extra_phases):
        row = by_phase.get(phase) if isinstance(by_phase.get(phase), dict) else {}
        if not row:
            if phase in {"image", "model"}:
                parts.append(f"{phase} 0/0 failed 0")
            continue
        parts.append(
            f"{phase} {int(row.get('item_count') or 0)}/"
            f"{int(row.get('requested_count') or 0)} failed {int(row.get('failed_count') or 0)}"
        )
    latest = summary.get("latest_events") if isinstance(summary.get("latest_events"), list) else []
    latest_row = latest[-1] if latest and isinstance(latest[-1], dict) else {}
    latest_phase = str(latest_row.get("phase") or "").strip()
    latest_status = str(latest_row.get("status") or "").strip().replace("_", "-")
    if latest_phase or latest_status:
        parts.append(f"latest {latest_phase or 'resource'}:{latest_status or 'unknown'}")
    needs_attention = [
        str(item).strip().replace("_", "-")[:40]
        for item in list(summary.get("needs_attention") or [])[:4]
        if str(item).strip()
    ]
    if needs_attention:
        parts.append("needs " + ",".join(needs_attention))
    return ", ".join(parts)


def format_agent_runtime_report_health_report(summary: Any) -> str:
    """Format aggregate report health counters with bounded safe labels."""

    if not isinstance(summary, dict) or not summary:
        return "unknown"

    def safe_label(value: Any) -> str:
        text = str(value or "").strip().replace("_", "-")
        for marker in ("prompt", "provider", "url", "raw", "token", "api-key", "path", "session", "job"):
            text = re.sub(marker, "resource", text, flags=re.IGNORECASE)
        return text[:60]

    status = safe_label(summary.get("status")) or "unknown"
    attention = "yes" if bool(summary.get("attention_required")) else "no"
    batch_failed = int(summary.get("batch_failed_count") or 0)
    batch_partial = int(summary.get("batch_partial_count") or 0)
    batch_waiting = int(summary.get("batch_waiting_count") or 0)
    import_failed = int(summary.get("import_failed_count") or 0)
    resource_failed = int(summary.get("resource_phase_failed_count") or 0)
    resource_partial = int(summary.get("resource_phase_partial_count") or 0)
    resource_waiting = int(summary.get("resource_phase_waiting_count") or 0)
    asset_failed = int(summary.get("asset_failed_count") or 0)
    asset_incomplete = int(summary.get("asset_incomplete_count") or 0)
    sync_health = safe_label(summary.get("sync_health_status")) or "unknown"
    import_failure_code_counts = (
        summary.get("import_failure_code_counts")
        if isinstance(summary.get("import_failure_code_counts"), dict)
        else {}
    )
    import_failure_codes = ", ".join(
        safe_label(code)
        for code, count in sorted(import_failure_code_counts.items())
        if int(count or 0) > 0 and safe_label(code)
    ) or "none"
    sync_failure_code_counts = (
        summary.get("sync_failure_code_counts")
        if isinstance(summary.get("sync_failure_code_counts"), dict)
        else {}
    )
    sync_failure_codes = ", ".join(
        safe_label(code)
        for code, count in sorted(sync_failure_code_counts.items())
        if int(count or 0) > 0 and safe_label(code)
    ) or "none"
    latest_sync_failure_code = safe_label(summary.get("latest_sync_failure_code"))
    engine_write_readiness_mismatch_count = int(summary.get("engine_write_readiness_mismatch_count") or 0)
    raw_engine_write_readiness_mismatch_channels = (
        summary.get("engine_write_readiness_mismatch_channels")
        if isinstance(summary.get("engine_write_readiness_mismatch_channels"), list)
        else []
    )
    engine_write_readiness_mismatch_channels = "/".join(
        safe_label(item)
        for item in raw_engine_write_readiness_mismatch_channels[:4]
        if safe_label(item)
    )
    engine_write_runtime_state_only_count = int(summary.get("engine_write_runtime_state_only_count") or 0)
    raw_engine_write_runtime_state_only_channels = (
        summary.get("engine_write_runtime_state_only_channels")
        if isinstance(summary.get("engine_write_runtime_state_only_channels"), list)
        else []
    )
    engine_write_runtime_state_only_channels = "/".join(
        safe_label(item)
        for item in raw_engine_write_runtime_state_only_channels[:4]
        if safe_label(item)
    )
    worker_drain_failed = int(summary.get("worker_drain_failed_count") or 0)
    worker_drain_exception = int(summary.get("worker_drain_exception_count") or 0)
    worker_drain_status_failed = int(summary.get("worker_drain_status_failed_count") or 0)
    worker_drain_plan_resolve_failed = int(summary.get("worker_drain_plan_resolve_failed_count") or 0)
    raw_reasons = summary.get("reasons") if isinstance(summary.get("reasons"), list) else []
    reasons = ", ".join(safe_label(reason) for reason in raw_reasons[:5] if safe_label(reason)) or "none"
    parts = [
        status,
        f"attention {attention}",
        f"batch failed/partial/waiting {batch_failed}/{batch_partial}/{batch_waiting}",
        f"import failed {import_failed}",
        f"resource phase failed/partial/waiting {resource_failed}/{resource_partial}/{resource_waiting}",
        f"asset failed/incomplete {asset_failed}/{asset_incomplete}",
        f"sync {sync_health}",
    ]
    if import_failure_codes != "none":
        parts.append(f"import failures {import_failure_codes}")
    if sync_failure_codes != "none":
        parts.append(f"sync failures {sync_failure_codes}")
    if latest_sync_failure_code:
        parts.append(f"latest sync failure {latest_sync_failure_code}")
    if engine_write_readiness_mismatch_count:
        if engine_write_readiness_mismatch_channels:
            parts.append(
                f"engine-write mismatch {engine_write_readiness_mismatch_count}"
                f"({engine_write_readiness_mismatch_channels})"
            )
        else:
            parts.append(f"engine-write mismatch {engine_write_readiness_mismatch_count}")
    if engine_write_runtime_state_only_count:
        if engine_write_runtime_state_only_channels:
            parts.append(
                f"engine-write runtime-state-only {engine_write_runtime_state_only_count}"
                f"({engine_write_runtime_state_only_channels})"
            )
        else:
            parts.append(f"engine-write runtime-state-only {engine_write_runtime_state_only_count}")
    if worker_drain_failed or worker_drain_exception or worker_drain_status_failed or worker_drain_plan_resolve_failed:
        parts.append(
            "worker-drain failed/status-failed/exception/plan-resolve "
            f"{worker_drain_failed}/{worker_drain_status_failed}/"
            f"{worker_drain_exception}/{worker_drain_plan_resolve_failed}"
        )
    if reasons != "none":
        parts.append(f"reasons {reasons}")
    return ", ".join(parts)


def format_agent_runtime_fact_source_boundary_report(summary: Any) -> str:
    """Format Runtime versus external fact-source counters."""

    if not isinstance(summary, dict) or not summary:
        return "runtime 0, external 0, external unavailable"
    runtime_count = int(summary.get("runtime_business_fact_count") or 0)
    external_count = int(summary.get("mirrored_external_fact_count") or 0)
    plan_count = int(summary.get("runtime_plan_fact_count") or 0)
    batch_count = int(summary.get("runtime_batch_fact_count") or 0)
    resource_count = int(summary.get("runtime_resource_event_count") or 0)
    import_count = int(summary.get("runtime_import_event_count") or 0)
    sync_count = int(summary.get("sync_event_count") or 0)
    engine_write_count = int(summary.get("engine_write_result_count") or 0)
    engine_write_boundary_count = int(summary.get("engine_write_boundary_fact_count") or 0)
    snapshot_count = int(summary.get("scene_snapshot_count") or 0)
    external_available = bool(summary.get("external_authoritative_available"))
    parts = [
        f"runtime {runtime_count}",
        f"external {external_count}",
        f"plan/batch {plan_count}/{batch_count}",
        f"resource/import {resource_count}/{import_count}",
        f"sync/write/snapshot {sync_count}/{engine_write_count}/{snapshot_count}",
        f"write-boundary {engine_write_boundary_count}",
    ]
    parts.append("external available" if external_available else "external unavailable")
    notes = [
        str(item).strip().replace("_", "-")[:48]
        for item in list(summary.get("boundary_notes") or [])[:3]
        if str(item).strip()
    ]
    if notes:
        parts.append("notes " + ",".join(notes))
    return ", ".join(parts)


def format_agent_runtime_closure_report(
    fact_source: Any,
    state_patch: Any,
    *,
    operation_count: Any = 0,
    operation_total_count: Any = 0,
) -> str:
    """Format closure counters from structured Runtime summaries."""

    fact_data = fact_source if isinstance(fact_source, dict) else {}
    patch_data = state_patch if isinstance(state_patch, dict) else {}
    source = str(fact_data.get("runtime_state_source") or "unknown").strip() or "unknown"
    write_boundary = int(fact_data.get("engine_write_boundary_fact_count") or 0)
    try:
        operations = int(operation_count or 0)
    except (TypeError, ValueError):
        operations = 0
    try:
        operation_total = int(operation_total_count or 0)
    except (TypeError, ValueError):
        operation_total = 0
    return (
        f"state {source}, operation {operations}/{operation_total}, "
        f"patch applied/conflict/invalid "
        f"{int(patch_data.get('applied') or 0)}/"
        f"{int(patch_data.get('conflict') or 0)}/"
        f"{int(patch_data.get('invalid') or 0)}, "
        f"write-boundary {write_boundary}"
    )


def format_agent_runtime_import_stage_report(summary: Any) -> str:
    """Format import stage event counters and latest status."""

    if not isinstance(summary, dict) or not summary:
        return "events 0, imported 0/0, failed 0"
    parts = [
        f"events {int(summary.get('event_count') or 0)}",
        f"imported {int(summary.get('imported_count') or 0)}/"
        f"{int(summary.get('requested_count') or 0)}",
        f"failed {int(summary.get('failed_count') or 0)}",
    ]
    latest = summary.get("latest_events") if isinstance(summary.get("latest_events"), list) else []
    latest_row = latest[-1] if latest and isinstance(latest[-1], dict) else {}
    latest_status = str(latest_row.get("status") or "").strip().replace("_", "-")
    if latest_status:
        parts.append(f"latest {latest_status}")
    return ", ".join(parts)


def format_agent_runtime_actor_import_boundary_report(
    import_summary: Any,
    scene_registry: Any,
    engine_write_boundary: Any,
) -> str:
    """Format actor import and native bridge boundary counters."""

    import_data = import_summary if isinstance(import_summary, dict) else {}
    registry_data = scene_registry if isinstance(scene_registry, dict) else {}
    boundary_data = engine_write_boundary if isinstance(engine_write_boundary, dict) else {}
    entity_type_counts = dict(registry_data.get("entity_type_counts") or {})
    requested = int(import_data.get("requested_count") or 0)
    imported = int(import_data.get("imported_count") or 0)
    failed = int(import_data.get("failed_count") or 0)
    actor_count = int(registry_data.get("actor_count") or entity_type_counts.get("actor") or 0)
    bridge_calls = int(boundary_data.get("bridge_call_count") or 0)
    bridge_success = int(boundary_data.get("bridge_success_count") or 0)
    bridge_failed = int(boundary_data.get("bridge_failed_count") or 0)
    status_counts = boundary_data.get("status_counts") if isinstance(boundary_data.get("status_counts"), dict) else {}
    runtime_state_only = int(status_counts.get("runtime_state_only") or 0)
    if bridge_calls > 0:
        native_state = f"bridge {bridge_success}/{bridge_calls}"
        if bridge_failed:
            native_state += f", failed {bridge_failed}"
    elif runtime_state_only > 0:
        native_state = f"RuntimeState-only {runtime_state_only}, native pending F5"
    else:
        native_state = "native not-observed"
    return (
        f"requested/imported/failed {requested}/{imported}/{failed}, "
        f"registered actor {actor_count}, {native_state}"
    )


def format_agent_runtime_tool_queue_health_report(summary: Any) -> str:
    """Format tool queue health counters without inspecting queue state."""

    if not isinstance(summary, dict) or not summary:
        return "queue 0, active 0, blocked 0, pressure 0%"
    queue_count = int(summary.get("queue_count") or 0)
    queued_count = int(summary.get("queued_count") or 0)
    running_count = int(summary.get("running_count") or 0)
    blocked_count = int(summary.get("blocked_count") or 0)
    terminal_count = int(summary.get("terminal_count") or 0)
    active_count = int(summary.get("active_count") or 0)
    queue_pressure = float(summary.get("queue_pressure") or 0.0)
    queue_pressure = max(0.0, min(1.0, queue_pressure))
    return (
        f"queue {queue_count}, active {active_count}, "
        f"queued/running {queued_count}/{running_count}, "
        f"blocked {blocked_count}, terminal {terminal_count}, "
        f"pressure {int(queue_pressure * 100)}%"
    )


def format_agent_runtime_batch_tooling_report(summary: Any) -> str:
    """Format batch tooling fact and intervention counters."""

    if not isinstance(summary, dict) or not summary:
        return "facts 0, created-batches 0, priorities 0, merged 0, absorbed 0"
    fact_count = int(summary.get("fact_count") or 0)
    created_batch_fact_count = int(summary.get("created_batch_fact_count") or 0)
    created_batch_count = int(summary.get("created_batch_count") or 0)
    prioritized_item_count = int(summary.get("prioritized_item_count") or 0)
    merged_intervention_fact_count = int(summary.get("merged_intervention_fact_count") or 0)
    merged_intervention_item_count = int(summary.get("merged_intervention_item_count") or 0)
    absorbed_intervention_count = int(summary.get("absorbed_intervention_count") or 0)
    latest_types = [
        str(item).strip().replace("_", "-")[:40]
        for item in list(summary.get("latest_fact_types") or [])[:5]
        if str(item).strip()
    ]
    parts = [
        f"facts {fact_count}",
        f"created-batches {created_batch_count}/{created_batch_fact_count}",
        f"priorities {prioritized_item_count}",
        f"merged {merged_intervention_item_count}/{merged_intervention_fact_count}",
        f"absorbed {absorbed_intervention_count}",
    ]
    if latest_types:
        parts.append("latest " + ",".join(latest_types))
    return ", ".join(parts)


def format_agent_runtime_batch_resource_lifecycle_report(summary: Any) -> str:
    """Format batch resource lifecycle event counters."""

    if not isinstance(summary, dict) or not summary:
        return "events 0"
    resource_event_count = int(summary.get("resource_event_count") or 0)
    image_ready_count = int(summary.get("image_ready_count") or 0)
    image_failed_count = int(summary.get("image_failed_count") or 0)
    model_ready_count = int(summary.get("model_ready_count") or 0)
    model_failed_count = int(summary.get("model_failed_count") or 0)
    import_ready_count = int(summary.get("import_ready_count") or 0)
    import_failed_count = int(summary.get("import_failed_count") or 0)
    environment_ready_count = int(summary.get("environment_ready_count") or 0)
    environment_failed_count = int(summary.get("environment_failed_count") or 0)
    emit_failed_count = int(summary.get("emit_failed_count") or 0)
    parts = [
        f"events {resource_event_count}",
        f"image {image_ready_count}/{image_failed_count}",
        f"model {model_ready_count}/{model_failed_count}",
        f"import {import_ready_count}/{import_failed_count}",
        f"env {environment_ready_count}/{environment_failed_count}",
    ]
    if emit_failed_count:
        parts.append(f"emit-failed {emit_failed_count}")
    latest = summary.get("latest_resource_event") if isinstance(summary.get("latest_resource_event"), dict) else {}
    latest_stage = str(latest.get("stage") or "").strip().replace("_", "-")
    latest_status = "persisted" if bool(latest.get("persisted")) else "not-persisted"
    if latest_stage:
        parts.append(f"latest {latest_stage}:{latest_status}")
    return ", ".join(parts)


def format_agent_runtime_geometry_fact_report(summary: Any) -> str:
    """Format geometry fact counters and bounded fact types."""

    if not isinstance(summary, dict) or not summary:
        return "none"
    fact_count = int(summary.get("fact_count") or 0)
    aabb_count = int(summary.get("aabb_actor_count") or 0)
    skipped_count = int(summary.get("aabb_skipped_count") or 0)
    overlap_count = int(summary.get("overlap_issue_count") or 0)
    if fact_count <= 0 and aabb_count <= 0 and skipped_count <= 0 and overlap_count <= 0:
        return "none"
    status_counts = summary.get("status_counts") if isinstance(summary.get("status_counts"), dict) else {}
    fact_type_counts = summary.get("fact_type_counts") if isinstance(summary.get("fact_type_counts"), dict) else {}
    statuses = ",".join(
        f"{str(key).replace('_', '-')}:{int(value or 0)}"
        for key, value in sorted(status_counts.items())
        if int(value or 0) > 0
    )
    fact_types = ",".join(
        f"{str(key).replace('_', '-')}:{int(value or 0)}"
        for key, value in sorted(fact_type_counts.items())
        if int(value or 0) > 0
    )
    parts = [f"{fact_count} fact(s)", f"AABB actors {aabb_count}", f"overlap issues {overlap_count}"]
    if skipped_count:
        parts.append(f"skipped {skipped_count}")
    if statuses:
        parts.append(f"status {statuses}")
    if fact_types:
        parts.append(f"type {fact_types}")
    return "；".join(parts)


def format_agent_runtime_command_report(summary: Any) -> str:
    """Format command counters and redacted latest command transitions."""

    if not isinstance(summary, dict) or not summary:
        return "none"
    command_count = int(summary.get("command_count") or 0)
    commands = summary.get("latest_commands") if isinstance(summary.get("latest_commands"), list) else []
    if command_count <= 0 and not commands:
        return "none"

    def safe_text(value: Any) -> str:
        text = str(value or "").strip()
        for marker in ("provider", "prompt", "url", "raw", "token", "api_key"):
            text = re.sub(marker, "runtime", text, flags=re.IGNORECASE)
        return text.replace("_", "-")[:80]

    latest_parts: list[str] = []
    for item in commands[-3:]:
        if not isinstance(item, dict):
            continue
        command = safe_text(item.get("command"))
        old_status = safe_text(item.get("old_status"))
        new_status = safe_text(item.get("new_status"))
        if not command:
            continue
        latest_parts.append(f"{command}:{old_status or '?'}->{new_status or '?'}" if old_status or new_status else command)
    parts = [f"{command_count} command(s)"]
    if latest_parts:
        parts.append("latest " + ",".join(latest_parts))
    return "；".join(parts)


def format_agent_runtime_review_proposal_report(summary: Any) -> str:
    """Format review proposal counts and host decision state."""

    if not isinstance(summary, dict) or not summary:
        return "none"
    proposal_count = int(summary.get("proposal_count") or 0)
    if proposal_count <= 0:
        return "none"
    item_count = int(summary.get("item_count") or 0)
    status_counts = summary.get("status_counts") if isinstance(summary.get("status_counts"), dict) else {}
    statuses = ",".join(
        f"{str(key).replace('_', '-')}:{int(value or 0)}"
        for key, value in sorted(status_counts.items())
        if int(value or 0) > 0
    )
    pending_count = int(status_counts.get("proposed") or 0)
    confirmed_count = int(status_counts.get("confirmed") or 0)
    rejected_count = int(status_counts.get("rejected") or 0)
    if pending_count > 0:
        decision_state = "waiting host confirmation"
    elif confirmed_count > 0 or rejected_count > 0:
        decision_state = "host decision recorded"
    else:
        decision_state = "decision state unknown"
    parts = [f"{proposal_count} proposal(s)", f"items {item_count}", decision_state]
    if statuses:
        parts.append(f"status {statuses}")
    return "；".join(parts)


def format_agent_runtime_review_confirmation_report(summary: Any) -> str:
    """Format review confirmation counts and decision totals."""

    if not isinstance(summary, dict) or not summary:
        return "none"
    confirmation_count = int(summary.get("confirmation_count") or 0)
    if confirmation_count <= 0:
        return "none"
    decision_counts = summary.get("decision_counts") if isinstance(summary.get("decision_counts"), dict) else {}
    decisions = ",".join(
        f"{str(key).replace('_', '-')}:{int(value or 0)}"
        for key, value in sorted(decision_counts.items())
        if int(value or 0) > 0
    )
    return f"{confirmation_count} confirmation(s)" + (f"；decision {decisions}" if decisions else "")


def format_agent_runtime_review_report(summary: Any) -> str:
    """Format the aggregate review summary."""

    if not isinstance(summary, dict) or not summary:
        return "none"
    review_count = int(summary.get("review_count") or 0)
    if review_count <= 0:
        return "none"
    issue_count = int(summary.get("issue_count") or 0)
    advisory_count = int(summary.get("advisory_count") or 0)
    status_counts = summary.get("status_counts") if isinstance(summary.get("status_counts"), dict) else {}
    checkpoint_counts = summary.get("checkpoint_counts") if isinstance(summary.get("checkpoint_counts"), dict) else {}
    statuses = ",".join(
        f"{str(key).replace('_', '-')}:{int(value or 0)}"
        for key, value in sorted(status_counts.items())
        if int(value or 0) > 0
    )
    checkpoints = ",".join(
        f"{str(key).replace('_', '-')}:{int(value or 0)}"
        for key, value in sorted(checkpoint_counts.items())
        if int(value or 0) > 0
    )
    parts = [f"{review_count} review(s)", f"issues {issue_count}", f"advisory {advisory_count}"]
    if statuses:
        parts.append(f"status {statuses}")
    if checkpoints:
        parts.append(f"checkpoint {checkpoints}")
    return "；".join(parts)


def format_agent_runtime_layout_report(summary: Any, confirmation_summary: Any = None) -> str:
    """Format layout proposals, transforms and confirmation counts."""

    if not isinstance(summary, dict) or not summary:
        proposal_count = 0
        proposals: list[Any] = []
    else:
        proposal_count = int(summary.get("proposal_count") or 0)
        proposals = summary.get("proposals") if isinstance(summary.get("proposals"), list) else []
    confirmation_count = 0
    if isinstance(confirmation_summary, dict):
        confirmation_count = int(confirmation_summary.get("confirmation_count") or 0)
    if proposal_count <= 0 and confirmation_count <= 0:
        return "none"
    status_counts: dict[str, int] = {}
    delta_count = 0
    applied_delta_count = int(summary.get("applied_delta_count") or 0) if isinstance(summary, dict) else 0
    skipped_delta_count = int(summary.get("skipped_delta_count") or 0) if isinstance(summary, dict) else 0
    transform_result_count = int(summary.get("transform_result_count") or 0) if isinstance(summary, dict) else 0
    ground_snapped_count = int(summary.get("ground_snapped_count") or 0) if isinstance(summary, dict) else 0
    overlap_resolved_count = int(summary.get("overlap_resolved_count") or 0) if isinstance(summary, dict) else 0
    transform_failure_code_counts = (
        summary.get("layout_transform_failure_code_counts")
        if isinstance(summary, dict) and isinstance(summary.get("layout_transform_failure_code_counts"), dict)
        else {}
    )
    risk_levels: list[str] = []
    for proposal in proposals:
        if not isinstance(proposal, dict):
            continue
        status = str(proposal.get("status") or "").strip()
        if status:
            status_counts[status] = status_counts.get(status, 0) + 1
        delta_count += int(proposal.get("delta_count") or 0)
        risk = str(proposal.get("risk_level") or "").strip().replace("_", "-")
        if risk and risk not in risk_levels:
            risk_levels.append(risk)
    parts = [f"{proposal_count} proposal(s)", f"deltas {delta_count}", f"applied {applied_delta_count}", f"skipped {skipped_delta_count}", f"transforms {transform_result_count}"]
    if ground_snapped_count:
        parts.append(f"ground-snapped {ground_snapped_count}")
    if overlap_resolved_count:
        parts.append(f"overlap-resolved {overlap_resolved_count}")
    if transform_failure_code_counts:
        def safe_layout_failure_label(value: Any) -> str:
            label = str(value or "").strip().lower().replace("_", "-").replace(" ", "-")
            blocked = ("provider", "url", "http", "prompt", "raw", "api-key", "apikey", "secret", "token")
            if any(marker in label for marker in blocked):
                return "redacted"
            return label[:64] or "unknown"

        failure_items = ",".join(
            f"{safe_layout_failure_label(key)}:{int(value or 0)}"
            for key, value in sorted(transform_failure_code_counts.items())
            if int(value or 0) > 0
        )
        if failure_items:
            parts.append(f"transform-failures {failure_items}")
    if confirmation_count:
        parts.append(f"confirmations {confirmation_count}")
    if risk_levels:
        parts.append("risk " + ",".join(risk_levels[:3]))
    if status_counts:
        statuses = ",".join(
            f"{str(key).replace('_', '-')}:{int(value or 0)}"
            for key, value in sorted(status_counts.items())
            if int(value or 0) > 0
        )
        if statuses:
            parts.append(f"status {statuses}")
    return "；".join(parts)


def format_agent_runtime_layout_confirmation_reply(result: dict[str, Any]) -> str:
    """Format a layout confirmation result without applying layout changes."""

    if not isinstance(result, dict):
        return "【AgentRuntime 布局结果】Runtime 未返回布局确认结果。"
    graph = result.get("graph") if isinstance(result.get("graph"), dict) else {}
    proposal = result.get("proposal") if isinstance(result.get("proposal"), dict) else {}
    if not proposal:
        reason = str(result.get("reason") or "未找到布局调整建议").strip()
        return f"【AgentRuntime 布局结果】{reason}。"
    plan_id = str(proposal.get("plan_id") or graph.get("plan_id") or "").strip()
    proposal_id = str(proposal.get("proposal_id") or proposal.get("id") or "").strip()
    graph_status = str(graph.get("status") or "unknown").strip().replace("_", "-")
    applied = proposal.get("applied_deltas") if isinstance(proposal.get("applied_deltas"), list) else []
    skipped = proposal.get("skipped_deltas") if isinstance(proposal.get("skipped_deltas"), list) else []
    transform_results = (
        proposal.get("engine_transform_results")
        if isinstance(proposal.get("engine_transform_results"), list)
        else []
    )
    transform_success = 0
    transform_failed = 0
    ground_snapped = 0
    overlap_resolved = 0
    for item in transform_results:
        if not isinstance(item, dict):
            continue
        status = str(item.get("status") or "").strip().lower()
        if status in {"success", "succeeded", "applied", "ok"}:
            transform_success += 1
        elif status in {"failed", "failure", "error", "rejected"}:
            transform_failed += 1
        if bool(item.get("ground_snapped")):
            ground_snapped += 1
        if bool(item.get("overlap_resolved")):
            overlap_resolved += 1
    prefix = f"ScenePlan {plan_id} " if plan_id else ""
    proposal_part = f"建议 {proposal_id} " if proposal_id else ""
    return (
        f"【AgentRuntime 布局结果】{prefix}{proposal_part}已通过 ToolCallGraph 确认，"
        f"graph {graph_status}，应用 {len(applied)} 项，跳过 {len(skipped)} 项，"
        f"引擎写入成功 {transform_success} 项、失败 {transform_failed} 项，"
        f"贴地 {ground_snapped} 项，重叠修正 {overlap_resolved} 项。"
    )


def format_agent_runtime_engine_write_report(summary: Any) -> str:
    """Format engine-write result counts and readiness export diagnostics."""

    if not isinstance(summary, dict) or not summary:
        return "import 0, transform 0, env-import 0, actor-delete 0"
    import_count = int(summary.get("import_result_count") or 0)
    transform_count = int(summary.get("transform_result_count") or 0)
    environment_import_count = int(summary.get("environment_import_result_count") or 0)
    delete_count = int(summary.get("delete_result_count") or 0)

    def status_text(counts: Any) -> str:
        if not isinstance(counts, dict) or not counts:
            return ""
        rows = [
            f"{str(key).replace('_', '-')}:{int(value or 0)}"
            for key, value in sorted(counts.items())
            if int(value or 0) > 0
            and "provider" not in str(key).lower()
            and "secret" not in str(key).lower()
            and "raw" not in str(key).lower()
        ]
        return "(" + ",".join(rows[:4]) + ")" if rows else ""

    parts = [
        f"import {import_count}{status_text(summary.get('import_status_counts'))}",
        f"transform {transform_count}{status_text(summary.get('transform_status_counts'))}",
        f"env-import {environment_import_count}{status_text(summary.get('environment_import_status_counts'))}",
        f"actor-delete {delete_count}{status_text(summary.get('delete_status_counts'))}",
    ]
    mismatch_count = int(summary.get("readiness_mismatch_count") or 0)
    mismatch_channels = summary.get("readiness_mismatch_channels")
    if mismatch_count and isinstance(mismatch_channels, list):
        names = [
            str(item or "").replace("_", "-")[:32]
            for item in mismatch_channels[:4]
            if str(item or "").strip()
            and "provider" not in str(item).lower()
            and "secret" not in str(item).lower()
        ]
        if names:
            parts.append(f"readiness-mismatch {mismatch_count}(" + "/".join(names) + ")")
    status_export_count = int(summary.get("status_export_count") or 0)
    latest_status_export = summary.get("latest_status_export") if isinstance(summary.get("latest_status_export"), dict) else {}
    if status_export_count > 0:
        export_bits = ["recorded" if latest_status_export.get("recorded") else "not-recorded"]
        bridge_failed = int(latest_status_export.get("engine_write_bridge_failed_count") or 0)
        if bridge_failed:
            export_bits.append(f"bridge-failed:{bridge_failed}")
        readiness_bits = []
        for label, key in (
            ("native", "engine_write_readiness_native_enabled_count"),
            ("runtime-state", "engine_write_readiness_runtime_state_only_count"),
            ("fallback", "engine_write_readiness_fallback_count"),
            ("disabled", "engine_write_readiness_disabled_count"),
            ("unavailable", "engine_write_readiness_unavailable_count"),
        ):
            value = int(latest_status_export.get(key) or 0)
            if value:
                readiness_bits.append(f"{label}:{value}")
        if readiness_bits:
            export_bits.append("readiness " + ",".join(readiness_bits[:5]))
        channel_bits = []
        for label, key in (
            ("native", "engine_write_readiness_native_enabled_channels"),
            ("runtime-state", "engine_write_readiness_runtime_state_only_channels"),
            ("fallback", "engine_write_readiness_fallback_channels"),
            ("disabled", "engine_write_readiness_disabled_channels"),
            ("unavailable", "engine_write_readiness_unavailable_channels"),
        ):
            values = latest_status_export.get(key)
            if not isinstance(values, list) or not values:
                continue
            names = [
                str(item or "").replace("_", "-")[:32]
                for item in values[:3]
                if str(item or "").strip()
                and "provider" not in str(item).lower()
                and "secret" not in str(item).lower()
            ]
            if names:
                channel_bits.append(f"{label} " + "/".join(names))
        if channel_bits:
            export_bits.append("channels " + "; ".join(channel_bits[:5]))
        error_counts = latest_status_export.get("engine_write_bridge_error_code_counts")
        if isinstance(error_counts, dict) and error_counts:
            safe_errors = [
                f"{str(key).replace('_', '-')}:{int(value or 0)}"
                for key, value in sorted(error_counts.items())
                if int(value or 0) > 0
                and "provider" not in str(key).lower()
                and "secret" not in str(key).lower()
            ]
            if safe_errors:
                export_bits.append("errors " + ",".join(safe_errors[:3]))
        parts.append(f"status-export {status_export_count}(" + ", ".join(export_bits) + ")")
    return ", ".join(parts)


def format_agent_runtime_engine_write_readiness_report(summary: Any) -> str:
    """Format engine-write readiness channels."""

    if not isinstance(summary, dict) or not summary:
        return "channels 0, native 0, runtime-state 0, fallback 0, disabled 0"

    def count(name: str) -> int:
        try:
            return max(0, int(summary.get(name) or 0))
        except (TypeError, ValueError):
            return 0

    def channel_list(name: str) -> str:
        values = summary.get(name)
        if not isinstance(values, list) or not values:
            return ""
        names = [
            str(item or "")[:32]
            for item in values[:3]
            if str(item or "").strip()
        ]
        return "(" + "?".join(names) + ")" if names else ""

    parts = [
        f"channels {count('channel_count')}",
        f"native {count('native_enabled_count')}{channel_list('native_enabled_channels')}",
        f"runtime-state {count('runtime_state_only_count')}{channel_list('runtime_state_only_channels')}",
        f"fallback {count('fallback_count')}{channel_list('fallback_channels')}",
        f"disabled {count('disabled_count')}{channel_list('disabled_channels')}",
    ]
    unavailable_count = count("unavailable_count")
    if unavailable_count:
        parts.append(f"unavailable {unavailable_count}{channel_list('unavailable_channels')}")
    return ", ".join(parts)


def format_agent_runtime_engine_write_boundary_report(summary: Any) -> str:
    """Format engine-write boundary facts and bridge diagnostics."""

    if not isinstance(summary, dict) or not summary:
        return "boundary 0, import/transform/delete 0/0/0"
    boundary_count = int(summary.get("boundary_fact_count") or 0)
    import_count = int(summary.get("import_boundary_count") or 0)
    transform_count = int(summary.get("transform_boundary_count") or 0)
    delete_count = int(summary.get("delete_boundary_count") or 0)

    def safe_label(value: Any) -> str:
        text = str(value or "").strip().lower()
        if not text:
            return ""
        text = re.sub(r"provider|prompt|raw|url|api[_-]?key|token", "runtime", text)
        text = re.sub(r"[^a-z0-9_.:-]+", "-", text)
        return text[:48].strip("-")

    def count_rows(value: Any) -> str:
        if not isinstance(value, dict) or not value:
            return "none"
        rows: list[str] = []
        for key, count in sorted(value.items()):
            label = safe_label(key)
            if not label:
                continue
            try:
                numeric_count = int(count or 0)
            except (TypeError, ValueError):
                continue
            if numeric_count > 0:
                rows.append(f"{label}:{numeric_count}")
        return ",".join(rows[:4]) if rows else "none"

    source_text = count_rows(summary.get("write_source_counts"))
    status_text = count_rows(summary.get("status_counts"))
    bridge_calls = int(summary.get("bridge_call_count") or 0)
    bridge_success = int(summary.get("bridge_success_count") or 0)
    bridge_failed = int(summary.get("bridge_failed_count") or 0)
    bridge_skipped = int(summary.get("bridge_skipped_count") or 0)
    bridge_error_text = count_rows(summary.get("bridge_error_code_counts"))
    bridge_skip_text = count_rows(summary.get("bridge_skip_reason_counts"))
    raw_status_counts = summary.get("status_counts") if isinstance(summary.get("status_counts"), dict) else {}
    runtime_state_only_count = int(raw_status_counts.get("runtime_state_only") or 0)
    if bridge_calls > 0:
        native_text = "native verified" if bridge_success > 0 and bridge_failed <= 0 else "native needs-attention"
    elif runtime_state_only_count > 0:
        native_text = "native pending F5"
    else:
        native_text = "native not-observed"
    return (
        f"boundary {boundary_count}, "
        f"import/transform/delete {import_count}/{transform_count}/{delete_count}, "
        f"sources {source_text}, statuses {status_text}, "
        f"bridge {bridge_calls}/{bridge_success}/{bridge_failed}, skipped {bridge_skipped}({bridge_skip_text}), "
        f"errors {bridge_error_text}, "
        f"{native_text}"
    )


def format_agent_runtime_resource_report(summary: Any) -> str:
    """Format resource adapter status by channel."""

    if not isinstance(summary, dict) or not summary:
        return "default runtime adapters"
    parts: list[str] = []

    def safe_value(value: Any) -> str:
        text = str(value or "").strip()[:80]
        return text.replace("provider", "adapter").replace("_", "-")

    for key in (
        "scene_snapshot",
        "image_resource",
        "model_resource",
        "actor_import",
        "environment_component",
        "environment_import",
        "review",
        "layout_transform",
    ):
        value = summary.get(key)
        if not isinstance(value, dict):
            continue
        status = safe_value(value.get("status") or value.get("mode") or "")
        reason = safe_value(value.get("reason") or "")
        if not status:
            continue
        label = key.replace("_", "-")
        if reason and status != "enabled":
            parts.append(f"{label}:{status}({reason[:40]})")
        else:
            parts.append(f"{label}:{status}")
    return "、".join(parts[:7]) if parts else "default runtime adapters"


def format_agent_runtime_resource_readiness_report(summary: Any) -> str:
    """Format resource channel readiness counts."""

    if not isinstance(summary, dict) or not summary:
        return "channels 0, enabled 0, unavailable 0"
    channel_count = int(summary.get("channel_count") or 0)
    requested_count = int(summary.get("requested_count") or 0)
    enabled_count = int(summary.get("enabled_count") or 0)
    unavailable_count = int(summary.get("unavailable_count") or 0)
    unavailable = summary.get("unavailable_channels")
    unavailable_text = ""
    if isinstance(unavailable, list) and unavailable:
        names = [
            str(item or "").replace("_", "-")[:32]
            for item in unavailable[:3]
            if str(item or "").strip()
        ]
        if names:
            unavailable_text = ", unavailable " + "、".join(names)
    return (
        f"channels {channel_count}, requested {requested_count}, "
        f"enabled {enabled_count}, unavailable {unavailable_count}{unavailable_text}"
    )


def format_agent_runtime_context_report(summary: Any) -> str:
    """Format bounded context counts and a redacted latest context preview."""

    if not isinstance(summary, dict) or not summary:
        return "0 context(s)"
    context_count = int(summary.get("context_count") or 0)
    context_type_counts = (
        summary.get("context_type_counts")
        if isinstance(summary.get("context_type_counts"), dict)
        else {}
    )
    speaker_type_counts = (
        summary.get("speaker_type_counts")
        if isinstance(summary.get("speaker_type_counts"), dict)
        else {}
    )
    latest_context = summary.get("latest_context") if isinstance(summary.get("latest_context"), list) else []

    def safe_label(value: Any) -> str:
        text = str(value or "").strip().replace("_", "-")
        for marker in ("provider", "prompt", "url", "raw", "metadata", "message-id"):
            text = re.sub(marker, "runtime", text, flags=re.IGNORECASE)
        return text[:48]

    type_rows = [
        f"{safe_label(key)}:{int(value or 0)}"
        for key, value in sorted(context_type_counts.items())
        if int(value or 0) > 0
    ]
    speaker_rows = [
        f"{safe_label(key)}:{int(value or 0)}"
        for key, value in sorted(speaker_type_counts.items())
        if int(value or 0) > 0
    ]
    latest_preview = ""
    if latest_context:
        latest = latest_context[-1] if isinstance(latest_context[-1], dict) else {}
        latest_type = safe_label(latest.get("context_type"))
        latest_speaker = safe_label(latest.get("speaker_type"))
        latest_message = safe_label(latest.get("message") or latest.get("text_preview"))
        if latest_message:
            latest_preview = f"{latest_type or 'context'}/{latest_speaker or 'speaker'}:{latest_message}"
        elif latest_type or latest_speaker:
            latest_preview = f"{latest_type or 'context'}/{latest_speaker or 'speaker'}"
    parts = [f"{context_count} context(s)"]
    if type_rows:
        parts.append("types " + ",".join(type_rows[:4]))
    if speaker_rows:
        parts.append("speakers " + ",".join(speaker_rows[:4]))
    if latest_preview:
        parts.append("latest " + latest_preview)
    return "；".join(parts)


def format_agent_runtime_intervention_digest(digest: Any) -> str:
    """Format intervention counters without exposing intervention contents."""

    if not isinstance(digest, dict) or not digest:
        return "pending 0, accepted 0, deferred 0"
    pending_count = int(digest.get("pending_count") or 0)
    accepted_count = int(digest.get("accepted_count") or 0)
    deferred_count = int(digest.get("deferred_count") or 0)
    absorbable_count = int(digest.get("absorbable_pending_count") or 0)
    non_absorbable_count = int(digest.get("non_absorbable_pending_count") or 0)
    parts = [
        f"pending {pending_count}",
        f"accepted {accepted_count}",
        f"deferred {deferred_count}",
    ]
    if absorbable_count or non_absorbable_count:
        parts.append(f"absorbable {absorbable_count}")
        parts.append(f"needs-confirmation {non_absorbable_count}")
    return ", ".join(parts)


def format_agent_runtime_intervention_reply(result: dict[str, Any]) -> str:
    """Format the intervention command result without routing or persistence."""

    if not isinstance(result, dict):
        return "【AgentRuntime 介入结果】Runtime 未返回介入结果。"
    plan = result.get("plan") if isinstance(result.get("plan"), dict) else {}
    patch = result.get("patch") if isinstance(result.get("patch"), dict) else {}
    runtime_plan_id = str(plan.get("plan_id") or patch.get("plan_id") or "")
    if not patch:
        message = str(result.get("message") or "AgentRuntime 未记录介入。")
        return f"【AgentRuntime 介入结果】{message}"
    patch_type = str(patch.get("patch_type") or result.get("action") or "intervention").strip().replace("_", "-")
    status = str(
        patch.get("status")
        or ("recorded" if result.get("recorded") else "not-recorded")
    ).strip().replace("_", "-")
    raw_items = patch.get("items") if isinstance(patch.get("items"), list) else []
    item_count = len([item for item in raw_items if str(item or "").strip()])
    return (
        f"【AgentRuntime 介入结果】ScenePlan {runtime_plan_id} 已记录 {patch_type}，"
        f"状态 {status}，对象 {item_count} 个。"
    )


def format_agent_runtime_intervention_summary(interventions: Any) -> str:
    """Format intervention status counts and a bounded pending preview."""

    if not isinstance(interventions, dict):
        return "暂无"
    pending_count = int(interventions.get("pending_count") or 0)
    accepted_count = int(interventions.get("accepted_count") or 0)
    deferred_count = int(interventions.get("deferred_count") or 0)
    parts = [
        f"待处理 {pending_count}",
        f"已吸收 {accepted_count}",
        f"延后 {deferred_count}",
    ]
    latest_pending = interventions.get("latest_pending")
    if isinstance(latest_pending, list) and latest_pending:
        latest = latest_pending[-1] if isinstance(latest_pending[-1], dict) else {}
        items = [str(item) for item in (latest.get("items") or []) if str(item)]
        preview = "、".join(items[:3]) if items else str(latest.get("text") or "").strip()
        if len(preview) > 48:
            preview = preview[:48] + "..."
        if preview:
            parts.append(f"最近待处理：{preview}")
    return "；".join(parts)


def format_agent_runtime_intervention_batch_summary(summary: Any) -> str:
    """Format intervention batch counts and a bounded latest-batch preview."""

    if not isinstance(summary, dict):
        return "暂无"
    batch_count = int(summary.get("batch_count") or 0)
    status_counts = summary.get("status_counts") if isinstance(summary.get("status_counts"), dict) else {}
    parts = [f"{batch_count} batch(es)"]
    if status_counts:
        parts.append(f"状态 {status_counts}")
    latest = summary.get("latest_batches")
    if isinstance(latest, list) and latest:
        batch = latest[-1] if isinstance(latest[-1], dict) else {}
        items = [str(item) for item in (batch.get("requested_items") or []) if str(item)]
        preview = "、".join(items[:3])
        if len(items) > 3:
            preview += f" 等 {len(items)} 项"
        if preview:
            parts.append(
                f"最近第 {batch.get('batch_index') or 0}/{batch.get('total_batches') or 0} 批：{preview}"
            )
    return "；".join(parts)


def format_agent_runtime_execution_reply(summary: dict[str, Any]) -> str:
    """Compose an execution reply from worker-prepared presentation fields."""

    if not isinstance(summary, dict):
        return "【AgentRuntime 执行结果】Runtime 未返回执行结果。"
    plan_id = str(summary.get("plan_id") or "")
    batch_count = int(summary.get("batch_count") or 0)
    graph_status_text = str(summary.get("graph_status_text") or "none")
    health_text = str(summary.get("health_text") or "unknown")
    details = "；".join(
        str(summary.get(key) or "")
        for key in (
            "registry_text",
            "classification_text",
            "flow_text",
            "tool_state_text",
            "guard_text",
            "queue_text",
            "drain_text",
            "batch_tooling_text",
            "report_source_text",
            "engine_text",
        )
        if str(summary.get(key) or "")
    )
    state = str(summary.get("state") or "completed").strip().lower()
    if state == "failed":
        lead = f"ScenePlan {plan_id} 执行未完成"
    elif state == "queued":
        lead = f"ScenePlan {plan_id} 已进入 Runtime 执行队列，待 worker drain 执行"
    elif state == "running":
        lead = f"ScenePlan {plan_id} 正在 Runtime 执行中"
    else:
        lead = f"ScenePlan {plan_id} 已执行 Runtime 批次 {batch_count} 个"
    batch_suffix = "" if state == "completed" else f"，批次 {batch_count} 个"
    return (
        f"【AgentRuntime 执行结果】{lead}{batch_suffix}，"
        f"执行图 {graph_status_text}，报告健康：{health_text}。{details}。"
    )


def format_agent_runtime_scene_registry_report(summary: Any) -> str:
    if not isinstance(summary, dict) or not summary:
        return "none"
    entity_type_counts = dict(summary.get("entity_type_counts") or {})
    entity_count = int(summary.get("entity_count") or 0)
    actor_count = int(summary.get("actor_count") or entity_type_counts.get("actor") or 0)
    terrain_count = int(summary.get("terrain_count") or entity_type_counts.get("terrain") or 0)
    skybox_count = int(summary.get("skybox_count") or entity_type_counts.get("skybox") or 0)
    entities = summary.get("entities") if isinstance(summary.get("entities"), list) else []
    roles: list[str] = []
    for entity in entities:
        if not isinstance(entity, dict):
            continue
        role = str(entity.get("semantic_role") or entity.get("name") or "").strip()
        if role and role not in roles:
            roles.append(role)
        if len(roles) >= 4:
            break
    if entity_count <= 0 and actor_count <= 0 and terrain_count <= 0 and skybox_count <= 0:
        return "none"
    parts = [
        f"entities {entity_count}",
        f"actor {actor_count}",
        f"terrain {terrain_count}",
        f"skybox {skybox_count}",
    ]
    if roles:
        parts.append("roles " + "、".join(roles))
    return ", ".join(parts)


def format_agent_runtime_scene_world_consistency_report(summary: Any) -> str:
    if not isinstance(summary, dict) or not summary:
        return "尚无 Engine/Runtime 对账结果"
    status = str(summary.get("status") or "blocked").strip().lower()
    expected = int(summary.get("expected_entity_count") or 0)
    actual = int(summary.get("engine_actor_count") or 0)
    matched = int(summary.get("matched_entity_count") or 0)
    issues = int(summary.get("issue_count") or 0)
    counts = f"匹配 {matched}/{expected}，Engine 实体 {actual}"
    if status == "consistent":
        return f"对账通过，{counts}"
    if status == "needs_review":
        return f"需要复核，{counts}，问题 {issues} 项"
    reason = str(summary.get("reason") or "engine_snapshot_unavailable").strip().lower()
    if reason == "engine_scene_snapshot_unavailable":
        return f"对账尚未完成，等待 Engine 场景快照；{counts}"
    return f"对账尚未完成，{counts}"


def format_agent_runtime_environment_report(summary: Any) -> str:
    if not isinstance(summary, dict) or not summary:
        return "none"
    count = int(summary.get("component_count") or 0)
    requested = int(summary.get("requested_count") or 0)
    ready = int(summary.get("ready_count") or count or 0)
    failed = int(summary.get("failed_count") or 0)
    imported = int(summary.get("imported_count") or 0)
    import_failed = int(summary.get("import_failed_count") or 0)
    event_count = int(summary.get("event_count") or 0)
    if count <= 0 and requested <= 0 and failed <= 0 and imported <= 0 and import_failed <= 0 and event_count <= 0:
        return "none"
    type_counts = summary.get("type_counts") if isinstance(summary.get("type_counts"), dict) else {}
    parts = [
        f"{str(key).replace('_', '-')}: {int(value or 0)}"
        for key, value in sorted(type_counts.items())
        if int(value or 0) > 0
    ]
    detail = "、".join(parts[:4]) if parts else "components tracked"
    counters = [f"{count} component(s)", f"ready {ready}"]
    if imported:
        counters.append(f"imported {imported}")
    if requested:
        counters.append(f"requested {requested}")
    if failed:
        counters.append(f"failed {failed}")
    if import_failed:
        counters.append(f"import-failed {import_failed}")
    return f"{'；'.join(counters)}, {detail}"


def _safe_label(value: Any, markers: tuple[str, ...]) -> str:
    text = str(value or "").strip().replace("_", "-")
    for marker in markers:
        text = re.sub(marker, "resource", text, flags=re.IGNORECASE)
    return text[:80]


def format_agent_runtime_scene_contract_report(summary: Any) -> str:
    if not isinstance(summary, dict) or not summary or not summary.get("available"):
        return "none"
    markers = ("prompt", "provider", "url", "raw", "token", "api-key", "path")
    scene_type = _safe_label(summary.get("scene_type"), markers) or "unknown-scene"
    environment_type = _safe_label(summary.get("environment_type"), markers) or "unknown-env"
    terrain_type = _safe_label(summary.get("terrain_type"), markers) or "unknown-terrain"
    boundary_type = _safe_label(summary.get("boundary_type"), markers) or "unknown-boundary"
    mood = format_agent_runtime_short_list(summary.get("mood"), fallback="none", limit=3)
    style = format_agent_runtime_short_list(summary.get("style_keywords"), fallback="none", limit=3)
    avoid = format_agent_runtime_short_list(summary.get("avoid_keywords"), fallback="none", limit=3)
    version = int(summary.get("version") or 0)
    return (
        f"{scene_type}/{environment_type}, terrain {terrain_type}, boundary {boundary_type}, "
        f"mood {mood}, style {style}, avoid {avoid}, v{version}"
    )


def format_agent_runtime_semantic_arbitration_report(summary: Any) -> str:
    if not isinstance(summary, dict) or not summary or not summary.get("available"):
        return "none"
    markers = ("prompt", "provider", "url", "raw", "token", "api-key", "path")
    state = _safe_label(summary.get("arbitration_state"), markers) or "unknown"
    readiness = _safe_label(summary.get("execution_readiness"), markers) or "unknown"
    owner = _safe_label(summary.get("owner_agent"), markers) or "none"
    agents = format_agent_runtime_short_list(summary.get("contributing_agents"), fallback="none", limit=4)
    flags = format_agent_runtime_short_list(summary.get("risk_flags"), fallback="none", limit=4)
    confirm = "yes" if bool(summary.get("requires_host_confirmation")) else "no"
    clarify = "yes" if bool(summary.get("needs_clarification")) else "no"
    multi_agent = "yes" if bool(summary.get("multi_agent_discussion")) else "no"
    return (
        f"{state}, readiness {readiness}, owner {owner}, agents {agents}, "
        f"multi-agent {multi_agent}, confirm {confirm}, clarify {clarify}, flags {flags}"
    )


def format_agent_runtime_tool_execution_digest_report(summary: Any) -> str:
    if not isinstance(summary, dict) or not summary or not summary.get("available"):
        return "none"
    markers = (
        "prompt", "provider", "url", "raw", "token", "api-key", "path", "tool-call", "tool-name",
    )
    graph_count = int(summary.get("graph_count") or 0)
    queue_count = int(summary.get("queue_count") or 0)
    node_count = int(summary.get("node_count") or 0)
    succeeded = int(summary.get("succeeded_count") or 0)
    failed = int(summary.get("failed_count") or 0)
    blocked = int(summary.get("blocked_count") or 0)
    skipped = int(summary.get("skipped_count") or 0)
    running = int(summary.get("running_count") or 0)
    planned = int(summary.get("planned_count") or 0)
    ready = int(summary.get("ready_count") or 0)
    attention = "yes" if bool(summary.get("attention_required")) else "no"
    reasons = format_agent_runtime_short_list(summary.get("attention_reasons"), fallback="none", limit=4)
    latest = summary.get("latest_attention") if isinstance(summary.get("latest_attention"), dict) else {}
    latest_status = _safe_label(latest.get("status"), markers)
    latest_reason = _safe_label(latest.get("reason"), markers) if latest_status else ""
    parts = [
        f"graphs {graph_count}", f"queue {queue_count}", f"nodes {node_count}",
        f"ok {succeeded}", f"failed {failed}", f"blocked {blocked}", f"skipped {skipped}",
    ]
    if running or planned or ready:
        parts.append(f"active r/p/ready {running}/{planned}/{ready}")
    parts.append(f"attention {attention}")
    if reasons != "none":
        parts.append(f"reasons {reasons}")
    if latest_status:
        parts.append(f"latest {latest_status}" + (f": {latest_reason}" if latest_reason else ""))
    return ", ".join(parts)
