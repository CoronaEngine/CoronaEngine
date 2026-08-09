"""Stateless normalization of AgentRuntime result shapes."""

from __future__ import annotations

from typing import Any


def agent_runtime_graphs_from_result(result: dict[str, Any]) -> list[dict[str, Any]]:
    """Return graph records from current and legacy Runtime response shapes."""
    if not isinstance(result, dict):
        return []
    graphs = result.get("graphs")
    if isinstance(graphs, list):
        normalized = [dict(graph) for graph in graphs if isinstance(graph, dict)]
        if normalized:
            return normalized
    graph = result.get("graph")
    if isinstance(graph, dict) and graph:
        return [dict(graph)]
    queued = result.get("queued")
    if isinstance(queued, dict):
        queued_graphs = queued.get("graphs")
        if isinstance(queued_graphs, list):
            normalized = [dict(graph) for graph in queued_graphs if isinstance(graph, dict)]
            if normalized:
                return normalized
        queued_graph = queued.get("graph")
        if isinstance(queued_graph, dict) and queued_graph:
            return [dict(queued_graph)]
    return []


def agent_runtime_batches_from_result(result: dict[str, Any]) -> list[dict[str, Any]]:
    """Return batch records from current and legacy Runtime response shapes."""
    if not isinstance(result, dict):
        return []
    batches = result.get("batches")
    if isinstance(batches, list):
        normalized = [dict(batch) for batch in batches if isinstance(batch, dict)]
        if normalized:
            return normalized
    batch = result.get("batch")
    if isinstance(batch, dict) and batch:
        return [dict(batch)]
    queued = result.get("queued")
    if isinstance(queued, dict):
        queued_batches = queued.get("batches")
        if isinstance(queued_batches, list):
            normalized = [dict(batch) for batch in queued_batches if isinstance(batch, dict)]
            if normalized:
                return normalized
        queued_batch = queued.get("batch")
        if isinstance(queued_batch, dict) and queued_batch:
            return [dict(queued_batch)]
    return []
