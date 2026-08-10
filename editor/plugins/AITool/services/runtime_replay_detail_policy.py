"""Stateless formatting policy for detailed Runtime replay summaries."""

from __future__ import annotations

from typing import Any


def format_agent_runtime_replay_failure_strategy_report(summary: Any) -> str:
    if not isinstance(summary, dict) or not summary:
        return "retry 0, skipped 0, abandoned 0"
    retry = int(summary.get("retry_scheduled_count") or 0)
    skipped = int(summary.get("dependency_skipped_count") or 0)
    abandoned = int(summary.get("abandoned_late_result_count") or 0)
    handler_failed = int(summary.get("handler_failed_count") or 0)
    invalid_result = int(summary.get("invalid_result_count") or 0)
    invalid_patch = int(summary.get("invalid_state_patch_count") or 0)
    state_conflict = int(summary.get("state_patch_conflict_count") or 0)
    stopped = int(summary.get("stopped_by_runtime_command_count") or 0)
    parts = [f"retry {retry}", f"skipped {skipped}", f"abandoned {abandoned}"]
    if handler_failed:
        parts.append(f"handler-failed {handler_failed}")
    if invalid_result:
        parts.append(f"invalid-result {invalid_result}")
    if invalid_patch:
        parts.append(f"invalid-patch {invalid_patch}")
    if state_conflict:
        parts.append(f"state-conflict {state_conflict}")
    if stopped:
        parts.append(f"stopped {stopped}")
    latest = summary.get("latest_strategy_event") if isinstance(summary.get("latest_strategy_event"), dict) else {}
    strategy = str(latest.get("strategy") or "").strip().replace("_", "-")
    status = str(latest.get("status") or "").strip().replace("_", "-")
    if strategy:
        parts.append(f"latest {strategy}:{status or 'unknown'}")
    return ", ".join(parts)


def format_agent_runtime_replay_layout_report(summary: Any) -> str:
    if not isinstance(summary, dict) or not summary:
        return "requests 0, confirmations 0, applied 0"
    request_count = int(summary.get("request_count") or 0)
    request_failed = int(summary.get("request_failed_count") or 0)
    confirmation_count = int(summary.get("confirmation_count") or 0)
    confirmation_failed = int(summary.get("confirmation_failed_count") or 0)
    applied = int(summary.get("applied_count") or 0)
    skipped = int(summary.get("skipped_count") or 0)
    transform_success = int(summary.get("transform_success_count") or 0)
    transform_failed = int(summary.get("transform_failed_count") or 0)
    ground_snapped = int(summary.get("ground_snapped_count") or 0)
    overlap_resolved = int(summary.get("overlap_resolved_count") or 0)
    delta_count = int(summary.get("delta_count") or 0)
    parts = [
        f"requests {request_count}/{request_failed}",
        f"confirmations {confirmation_count}/{confirmation_failed}",
        f"applied {applied}",
        f"transforms {transform_success}/{transform_failed}",
    ]
    if skipped:
        parts.append(f"skipped {skipped}")
    if ground_snapped:
        parts.append(f"ground {ground_snapped}")
    if overlap_resolved:
        parts.append(f"overlap {overlap_resolved}")
    if delta_count:
        parts.append(f"deltas {delta_count}")
    latest_status = str(summary.get("latest_graph_status") or "").strip().replace("_", "-")
    if latest_status:
        parts.append(f"latest {latest_status}")
    return ", ".join(parts)


def format_agent_runtime_replay_vlm_report(summary: Any) -> str:
    if not isinstance(summary, dict) or not summary:
        return "checkpoints 0, advisory 0"
    checkpoint_count = int(summary.get("checkpoint_count") or 0)
    advisory_count = int(summary.get("advisory_count") or 0)
    parts = [f"checkpoints {checkpoint_count}", f"advisory {advisory_count}"]
    status_counts = summary.get("status_counts") if isinstance(summary.get("status_counts"), dict) else {}
    status_text = ",".join(
        f"{str(key).strip().replace('_', '-')}:{int(value or 0)}"
        for key, value in sorted(status_counts.items())[:4]
        if str(key).strip() and int(value or 0) > 0
    )
    if status_text:
        parts.append(f"status {status_text}")
    checkpoint_counts = summary.get("checkpoint_counts") if isinstance(summary.get("checkpoint_counts"), dict) else {}
    checkpoint_text = ",".join(
        f"{str(key).strip().replace('_', '-')}:{int(value or 0)}"
        for key, value in sorted(checkpoint_counts.items())[:4]
        if str(key).strip() and int(value or 0) > 0
    )
    if checkpoint_text:
        parts.append(f"types {checkpoint_text}")
    latest = summary.get("latest_checkpoints") if isinstance(summary.get("latest_checkpoints"), list) else []
    latest_item = latest[-1] if latest and isinstance(latest[-1], dict) else {}
    checkpoint_type = str(latest_item.get("checkpoint_type") or "").strip().replace("_", "-")
    status = str(latest_item.get("status") or "").strip().replace("_", "-")
    if checkpoint_type:
        parts.append(f"latest {checkpoint_type}:{status or 'unknown'}")
    return ", ".join(parts)


def format_agent_runtime_replay_review_advisory_report(summary: Any) -> str:
    if not isinstance(summary, dict) or not summary:
        return "proposals 0, confirmations 0"
    proposals = int(summary.get("proposal_created_count") or 0)
    confirmations = int(summary.get("confirmation_count") or 0)
    pending = int(summary.get("pending_proposal_count") or 0)
    confirmed = int(summary.get("confirmed_proposal_count") or 0)
    rejected = int(summary.get("rejected_proposal_count") or 0)
    advisory_items = int(summary.get("advisory_item_count") or 0)
    parts = [f"proposals {proposals}", f"confirmations {confirmations}"]
    status_parts: list[str] = []
    if pending:
        status_parts.append(f"pending:{pending}")
    if confirmed:
        status_parts.append(f"confirmed:{confirmed}")
    if rejected:
        status_parts.append(f"rejected:{rejected}")
    if status_parts:
        parts.append("status " + ",".join(status_parts))
    if advisory_items:
        parts.append(f"items {advisory_items}")
    latest_decision = str(summary.get("latest_decision") or "").strip().replace("_", "-")
    if latest_decision:
        parts.append(f"latest {latest_decision}")
    return ", ".join(parts)


def format_agent_runtime_replay_final_adjustment_report(summary: Any) -> str:
    if not isinstance(summary, dict) or not summary:
        return "confirmations 0"
    confirmations = int(summary.get("confirmation_count") or 0)
    failed = int(summary.get("confirmation_failed_count") or 0)
    skipped = int(summary.get("confirmation_skipped_count") or 0)
    parts = [f"confirmations {confirmations}"]
    if failed:
        parts.append(f"failed {failed}")
    if skipped:
        parts.append(f"skipped {skipped}")
    decision_counts = summary.get("decision_counts") if isinstance(summary.get("decision_counts"), dict) else {}
    decision_text = ",".join(
        f"{str(key).strip().replace('_', '-')}:{int(value or 0)}"
        for key, value in sorted(decision_counts.items())[:4]
        if str(key).strip() and int(value or 0) > 0
    )
    if decision_text:
        parts.append(f"decisions {decision_text}")
    latest = summary.get("latest_confirmation") if isinstance(summary.get("latest_confirmation"), dict) else {}
    latest_decision = str(latest.get("decision") or "").strip().replace("_", "-")
    latest_proposal = str(latest.get("proposal_id") or "").strip()
    conflict_count = int(latest.get("conflict_item_count") or 0)
    if latest_decision:
        latest_text = f"latest {latest_decision}"
        if latest_proposal:
            latest_text += f" {latest_proposal[:48]}"
        if conflict_count:
            latest_text += f" conflicts {conflict_count}"
        parts.append(latest_text)
    return ", ".join(parts)
