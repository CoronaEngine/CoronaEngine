"""Stateless formatting policy for replay resource summaries."""

from __future__ import annotations

import re
from typing import Any


def format_agent_runtime_replay_environment_report(summary: Any) -> str:
    if not isinstance(summary, dict) or not summary:
        return "ready 0, import 0"
    ready = int(summary.get("ready_event_count") or 0)
    failed = int(summary.get("failed_event_count") or 0)
    imported = int(summary.get("import_event_count") or 0)
    import_failed = int(summary.get("import_failed_event_count") or 0)
    parts = [f"ready {ready}/{failed}", f"import {imported}/{import_failed}"]
    event_counts = summary.get("event_type_counts") if isinstance(summary.get("event_type_counts"), dict) else {}
    event_text = ",".join(
        f"{str(key).strip().replace('_', '-')}:{int(value or 0)}"
        for key, value in sorted(event_counts.items())[:4]
        if str(key).strip() and int(value or 0) > 0
    )
    if event_text:
        parts.append(f"types {event_text}")
    latest = str(summary.get("latest_event_type") or "").strip().replace("_", "-")
    if latest:
        parts.append(f"latest {latest}")
    return ", ".join(parts)


def format_agent_runtime_replay_resource_readiness_report(summary: Any) -> str:
    if not isinstance(summary, dict) or not summary:
        return "queries 0, published 0, events 0"

    def safe_label(value: Any) -> str:
        text = str(value or "").strip().replace("_", "-")
        for marker in ("prompt", "provider", "url", "raw", "token", "api-key", "path", "session", "job"):
            text = re.sub(marker, "resource", text, flags=re.IGNORECASE)
        return text[:60]

    queries = int(summary.get("status_query_count") or 0)
    published = int(summary.get("published_count") or 0)
    publish_failed = int(summary.get("publish_failed_count") or 0)
    readiness_events = int(summary.get("readiness_event_count") or 0)
    parts = [f"queries {queries}", f"published {published}/{publish_failed}", f"events {readiness_events}"]
    publish_requested = int(summary.get("publish_requested_total") or 0)
    publish_enabled = int(summary.get("publish_enabled_total") or 0)
    publish_unavailable = int(summary.get("publish_unavailable_total") or 0)
    if publish_requested or publish_enabled or publish_unavailable:
        parts.append(
            f"publish-ready requested/enabled/unavailable {publish_requested}/{publish_enabled}/{publish_unavailable}"
        )
    publish_status_counts = summary.get("publish_status_counts") if isinstance(summary.get("publish_status_counts"), dict) else {}
    publish_status_text = ",".join(
        f"{safe_label(key)}:{int(value or 0)}"
        for key, value in sorted(publish_status_counts.items())[:4]
        if safe_label(key) and int(value or 0) > 0
    )
    if publish_status_text:
        parts.append(f"publish-status {publish_status_text}")
    requested_total = int(summary.get("status_query_requested_total") or 0)
    enabled_total = int(summary.get("status_query_enabled_total") or 0)
    unavailable_total = int(summary.get("status_query_unavailable_total") or 0)
    if requested_total or enabled_total or unavailable_total:
        parts.append(f"query-ready requested/enabled/unavailable {requested_total}/{enabled_total}/{unavailable_total}")
    query_status_counts = summary.get("status_query_status_counts") if isinstance(summary.get("status_query_status_counts"), dict) else {}
    query_status_text = ",".join(
        f"{safe_label(key)}:{int(value or 0)}"
        for key, value in sorted(query_status_counts.items())[:4]
        if safe_label(key) and int(value or 0) > 0
    )
    if query_status_text:
        parts.append(f"query-status {query_status_text}")
    status_counts = summary.get("status_counts") if isinstance(summary.get("status_counts"), dict) else {}
    status_text = ",".join(
        f"{safe_label(key)}:{int(value or 0)}"
        for key, value in sorted(status_counts.items())[:4]
        if safe_label(key) and int(value or 0) > 0
    )
    if status_text:
        parts.append(f"status {status_text}")
    latest = summary.get("latest_readiness_event") if isinstance(summary.get("latest_readiness_event"), dict) else {}
    latest_status = safe_label(latest.get("status"))
    if latest_status:
        parts.append(f"latest {latest_status}")
    return ", ".join(parts)
