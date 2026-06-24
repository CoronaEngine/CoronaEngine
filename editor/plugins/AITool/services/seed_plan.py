from __future__ import annotations

import time
import uuid
from dataclasses import dataclass, field
from enum import Enum
from typing import Any


_SENSITIVE_SEED_PLAN_KEYS = {
    "api_key",
    "auth",
    "chain",
    "debug",
    "debug_trace",
    "error_trace",
    "finding_details",
    "hidden_debug_ref",
    "internal",
    "job_id",
    "llm_request",
    "llm_response",
    "messages",
    "model_config",
    "model_provider",
    "prompt",
    "provider",
    "raw_prompt",
    "raw_response",
    "request",
    "response",
    "runtime_context",
    "scheduler_updates",
    "session_id",
    "stage_handlers",
    "stack",
    "trace",
    "tool",
    "tool_call",
    "tool_calls",
    "tool_name",
    "token",
    "vlm_raw",
}


def _sanitize_seed_plan_payload(value: Any) -> Any:
    if isinstance(value, dict):
        safe: dict[str, Any] = {}
        for key, item in value.items():
            key_text = str(key)
            key_lower = key_text.lower()
            if key_lower in _SENSITIVE_SEED_PLAN_KEYS or key_lower.endswith("_prompt") or "token" in key_lower:
                continue
            if item is None or callable(item):
                continue
            safe[key_text] = _sanitize_seed_plan_payload(item)
        return safe
    if isinstance(value, list):
        return [
            _sanitize_seed_plan_payload(item)
            for item in value
            if item is not None and not callable(item)
        ]
    if isinstance(value, tuple):
        return [
            _sanitize_seed_plan_payload(item)
            for item in value
            if item is not None and not callable(item)
        ]
    return value


class SeedPlanStatus(str, Enum):
    DRAFT = "draft"
    CLARIFYING = "clarifying"
    PROPOSED = "proposed"
    CONFIRMED = "confirmed"
    EXECUTING = "executing"
    PAUSED = "paused"
    COMPLETED = "completed"
    CANCELLED = "cancelled"


@dataclass
class ParticipantIntent:
    user_id: str
    user_name: str = ""
    text: str = ""
    priority: int = 0
    timestamp: float = field(default_factory=time.time)

    def as_dict(self) -> dict[str, Any]:
        return {
            "user_id": self.user_id,
            "user_name": self.user_name,
            "text": self.text,
            "priority": self.priority,
            "timestamp": self.timestamp,
        }


@dataclass
class BatchGoal:
    batch_id: str
    title: str = ""
    goals: list[str] = field(default_factory=list)
    asset_hints: list[str] = field(default_factory=list)

    def as_dict(self) -> dict[str, Any]:
        return {
            "batch_id": self.batch_id,
            "title": self.title,
            "goals": list(self.goals),
            "asset_hints": list(self.asset_hints),
        }


@dataclass
class SeedPlan:
    room_id: str
    host_id: str = ""
    plan_id: str = field(default_factory=lambda: f"seed-{uuid.uuid4().hex[:12]}")
    status: SeedPlanStatus = SeedPlanStatus.DRAFT
    scene_type: str = "mixed"
    participants: list[ParticipantIntent] = field(default_factory=list)
    intent_summary: str = ""
    conflicts: list[str] = field(default_factory=list)
    style_constraints: list[str] = field(default_factory=list)
    spatial_constraints: list[str] = field(default_factory=list)
    asset_constraints: list[str] = field(default_factory=list)
    placement_constraints: list[str] = field(default_factory=list)
    batch_goals: list[BatchGoal] = field(default_factory=list)
    review_policy: dict[str, Any] = field(default_factory=dict)
    design_brief: str = ""
    design_items: list[str] = field(default_factory=list)
    design_source: str = ""
    confirmed_by: str = ""
    version: int = 1
    created_at: float = field(default_factory=time.time)
    updated_at: float = field(default_factory=time.time)

    def add_intent(self, intent: ParticipantIntent) -> None:
        self._assert_mutable()
        self.participants.append(intent)
        if intent.text:
            self.intent_summary = self._summarize_intents()
        self.updated_at = time.time()

    def set_design_brief(
        self,
        text: str,
        items: list[str] | None = None,
        source: str = "",
    ) -> None:
        self._assert_mutable()
        brief = str(text or "").strip()
        self.design_brief = brief
        if items is not None:
            seen: set[str] = set()
            cleaned: list[str] = []
            for item in items:
                value = str(item or "").strip()
                if not value or value in seen:
                    continue
                seen.add(value)
                cleaned.append(value)
            self.design_items = cleaned
        self.design_source = str(source or "").strip()
        if brief:
            self.intent_summary = brief
        self.updated_at = time.time()

    def propose(self) -> None:
        self._assert_mutable()
        self.status = SeedPlanStatus.PROPOSED
        self.updated_at = time.time()

    def confirm(self, host_id: str) -> None:
        if self.status not in {SeedPlanStatus.DRAFT, SeedPlanStatus.CLARIFYING, SeedPlanStatus.PROPOSED}:
            raise ValueError(f"SeedPlan {self.plan_id} cannot be confirmed from {self.status}")
        confirmer = str(host_id or "").strip()
        if not confirmer:
            raise ValueError(f"SeedPlan {self.plan_id} requires a non-empty host_id for confirmation")
        self.host_id = self.host_id or confirmer
        self.confirmed_by = confirmer
        self.status = SeedPlanStatus.CONFIRMED
        self.updated_at = time.time()

    def mark_executing(self) -> None:
        if self.status != SeedPlanStatus.CONFIRMED:
            raise ValueError(f"SeedPlan {self.plan_id} must be confirmed before execution")
        self.status = SeedPlanStatus.EXECUTING
        self.updated_at = time.time()

    def mark_completed(self) -> None:
        if self.status not in {SeedPlanStatus.CONFIRMED, SeedPlanStatus.EXECUTING, SeedPlanStatus.COMPLETED}:
            raise ValueError(f"SeedPlan {self.plan_id} cannot complete from {self.status}")
        self.status = SeedPlanStatus.COMPLETED
        self.updated_at = time.time()

    def pause_execution(self) -> None:
        if self.status not in {SeedPlanStatus.CONFIRMED, SeedPlanStatus.EXECUTING, SeedPlanStatus.PAUSED}:
            raise ValueError(f"SeedPlan {self.plan_id} cannot pause from {self.status}")
        self.status = SeedPlanStatus.PAUSED
        self.updated_at = time.time()

    def append_intervention_version(self) -> "SeedPlan":
        clone = SeedPlan.from_dict(self.as_dict())
        clone.plan_id = f"{self.plan_id}-v{self.version + 1}"
        clone.version = self.version + 1
        clone.status = SeedPlanStatus.DRAFT
        clone.confirmed_by = ""
        clone.updated_at = time.time()
        return clone

    def as_dict(self) -> dict[str, Any]:
        return {
            "plan_id": self.plan_id,
            "room_id": self.room_id,
            "host_id": self.host_id,
            "status": self.status.value,
            "scene_type": self.scene_type,
            "participants": [item.as_dict() for item in self.participants],
            "intent_summary": self.intent_summary,
            "conflicts": list(self.conflicts),
            "style_constraints": list(self.style_constraints),
            "spatial_constraints": list(self.spatial_constraints),
            "asset_constraints": list(self.asset_constraints),
            "placement_constraints": list(self.placement_constraints),
            "batch_goals": [item.as_dict() for item in self.batch_goals],
            "review_policy": _sanitize_seed_plan_payload(self.review_policy),
            "design_brief": self.design_brief,
            "design_items": list(self.design_items),
            "design_source": self.design_source,
            "confirmed_by": self.confirmed_by,
            "version": self.version,
            "created_at": self.created_at,
            "updated_at": self.updated_at,
        }

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> "SeedPlan":
        plan = cls(
            room_id=str(data.get("room_id") or ""),
            host_id=str(data.get("host_id") or ""),
            plan_id=str(data.get("plan_id") or f"seed-{uuid.uuid4().hex[:12]}"),
            status=SeedPlanStatus(str(data.get("status") or SeedPlanStatus.DRAFT.value)),
            scene_type=str(data.get("scene_type") or "mixed"),
            participants=[
                ParticipantIntent(
                    user_id=str(item.get("user_id") or ""),
                    user_name=str(item.get("user_name") or ""),
                    text=str(item.get("text") or ""),
                    priority=int(item.get("priority") or 0),
                    timestamp=float(item.get("timestamp") or time.time()),
                )
                for item in data.get("participants") or []
                if isinstance(item, dict)
            ],
            intent_summary=str(data.get("intent_summary") or ""),
            conflicts=[str(item) for item in data.get("conflicts") or []],
            style_constraints=[str(item) for item in data.get("style_constraints") or []],
            spatial_constraints=[str(item) for item in data.get("spatial_constraints") or []],
            asset_constraints=[str(item) for item in data.get("asset_constraints") or []],
            placement_constraints=[str(item) for item in data.get("placement_constraints") or []],
            batch_goals=[
                BatchGoal(
                    batch_id=str(item.get("batch_id") or ""),
                    title=str(item.get("title") or ""),
                    goals=[str(value) for value in item.get("goals") or []],
                    asset_hints=[str(value) for value in item.get("asset_hints") or []],
                )
                for item in data.get("batch_goals") or []
                if isinstance(item, dict)
            ],
            review_policy=dict(data.get("review_policy") or {}),
            design_brief=str(data.get("design_brief") or ""),
            design_items=[str(item or "").strip() for item in data.get("design_items") or [] if str(item or "").strip()],
            design_source=str(data.get("design_source") or ""),
            confirmed_by=str(data.get("confirmed_by") or ""),
            version=int(data.get("version") or 1),
            created_at=float(data.get("created_at") or time.time()),
            updated_at=float(data.get("updated_at") or time.time()),
        )
        return plan

    def _assert_mutable(self) -> None:
        if self.status in {SeedPlanStatus.CONFIRMED, SeedPlanStatus.EXECUTING, SeedPlanStatus.COMPLETED}:
            raise ValueError(f"SeedPlan {self.plan_id} is frozen at {self.status}")

    def _summarize_intents(self) -> str:
        texts = [item.text.strip() for item in self.participants if item.text.strip()]
        return "；".join(texts[-5:])
