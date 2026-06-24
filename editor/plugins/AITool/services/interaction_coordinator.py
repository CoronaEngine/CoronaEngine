from __future__ import annotations

import logging
import time
import uuid
from dataclasses import dataclass, field
import re
from typing import Any, Callable

from .disclosure_policy import DisclosureEvent, DisclosurePolicy
from .intent_understanding import get_intent_understanding_service
from .memory_scope import MemoryScope, MemoryScopeStore
from .scene_design_contract import (
    SceneDesignContract,
    build_scene_design_contract,
    close_contract,
    update_contract_from_intervention,
)
from .seed_plan import ParticipantIntent, SeedPlan, SeedPlanStatus
from .terrain_component_resolver import TerrainComponentResolver, canonical_actor_id


MAX_COORDINATOR_EVENTS = 2048
MAX_COORDINATOR_DISCLOSURE_EVENTS = 2048
MAX_PENDING_INTERVENTIONS_PER_PLAN = 256
MAX_RESOLVED_COORDINATOR_PROPOSALS = 256
MAX_GENERATION_JOB_REFS_PER_PLAN = 64
logger = logging.getLogger(__name__)
_SENSITIVE_CONTROL_PAYLOAD_KEYS = {
    "api_key",
    "auth",
    "batch_id",
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


def _coordinator_trace_preview(value: Any, limit: int = 100) -> str:
    text = str(value or "").replace("\r", "\\r").replace("\n", "\\n")
    if len(text) > limit:
        return f"{text[:limit]}..."
    return text


def _sanitize_control_payload(value: Any) -> Any:
    if isinstance(value, dict):
        safe: dict[str, Any] = {}
        for key, item in value.items():
            key_text = str(key)
            key_lower = key_text.lower()
            if key_lower in _SENSITIVE_CONTROL_PAYLOAD_KEYS or key_lower.endswith("_prompt") or "token" in key_lower:
                continue
            if item is None or callable(item):
                continue
            safe[key_text] = _sanitize_control_payload(item)
        return safe
    if isinstance(value, list):
        return [
            _sanitize_control_payload(item)
            for item in value
            if item is not None and not callable(item)
        ]
    if isinstance(value, tuple):
        return [
            _sanitize_control_payload(item)
            for item in value
            if item is not None and not callable(item)
        ]
    return value


def _coerce_protocol_bool(value: Any, *, default: bool = False) -> bool:
    """Parse bool-like values from JSON/native bridge payloads without truthy-string traps."""
    if isinstance(value, bool):
        return value
    if value is None:
        return default
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, str):
        normalized = value.strip().lower()
        if normalized in {"true", "1", "yes", "y", "on", "passed", "pass"}:
            return True
        if normalized in {"false", "0", "no", "n", "off", "failed", "fail"}:
            return False
        return default
    return bool(value)


@dataclass
class ChatMessage:
    room_id: str
    text: str
    sender_id: str = ""
    sender_name: str = ""
    is_host: bool = False
    agent_id: str = ""
    agent_name: str = ""
    metadata: dict[str, Any] = field(default_factory=dict)


@dataclass
class CoordinatorEvent:
    event_type: str
    message: str
    payload: dict[str, Any] = field(default_factory=dict)


@dataclass
class ConfirmResult:
    ok: bool
    plan_id: str
    message: str
    payload: dict[str, Any] = field(default_factory=dict)


@dataclass
class ConflictResolutionProposal:
    proposal_id: str
    room_id: str
    plan_id: str
    proposed_by: str
    conflict_items: list[str]
    recommendation: str
    status: str = "proposed"
    confirmed_by: str = ""

    def as_dict(self) -> dict[str, Any]:
        return {
            "proposal_id": self.proposal_id,
            "room_id": self.room_id,
            "plan_id": self.plan_id,
            "proposed_by": self.proposed_by,
            "conflict_items": list(self.conflict_items),
            "recommendation": self.recommendation,
            "status": self.status,
            "confirmed_by": self.confirmed_by,
        }


@dataclass
class GenerationJobRef:
    job_id: str
    plan_id: str
    status: str
    session_id: str = ""
    payload: dict[str, Any] = field(default_factory=dict)


@dataclass
class InterventionRequest:
    room_id: str
    plan_id: str
    content: str
    source_user_id: str = ""
    session_id: str = ""
    batch_id: str = ""
    actor_id: str = ""
    actor_version: int = 0
    target_hint: str = ""
    finding_details: list[dict[str, Any]] = field(default_factory=list)
    intent_type: str = "modify"
    priority: int = 0
    apply_policy: str = "next_batch"
    intervention_id: str = field(default_factory=lambda: f"iv-{uuid.uuid4().hex[:12]}")

    def as_dict(self) -> dict[str, Any]:
        return {
            "intervention_id": self.intervention_id,
            "room_id": self.room_id,
            "plan_id": self.plan_id,
            "session_id": self.session_id,
            "batch_id": self.batch_id,
            "actor_id": self.actor_id,
            "actor_version": self.actor_version,
            "target_hint": self.target_hint,
            "finding_details": [
                _sanitize_control_payload(item)
                for item in self.finding_details
                if isinstance(item, dict)
            ],
            "source_user_id": self.source_user_id,
            "intent_type": self.intent_type,
            "content": self.content,
            "priority": self.priority,
            "apply_policy": self.apply_policy,
        }


@dataclass
class InterventionDecision:
    accepted: bool
    route: str
    message: str
    payload: dict[str, Any] = field(default_factory=dict)


@dataclass
class LayoutReflowProposal:
    proposal_id: str
    room_id: str
    plan_id: str
    reason: str
    actor_hints: list[str] = field(default_factory=list)
    layout_strategy: list[str] = field(default_factory=list)
    deltas: list[dict[str, Any]] = field(default_factory=list)
    risk_level: str = "low"
    status: str = "proposed"
    confirmed_by: str = ""
    created_at: float = field(default_factory=time.time)
    updated_at: float = field(default_factory=time.time)

    def as_dict(self) -> dict[str, Any]:
        return {
            "proposal_id": self.proposal_id,
            "room_id": self.room_id,
            "plan_id": self.plan_id,
            "reason": self.reason,
            "actor_hints": list(self.actor_hints),
            "layout_strategy": list(self.layout_strategy),
            "deltas": [dict(item) for item in self.deltas if isinstance(item, dict)],
            "risk_level": self.risk_level,
            "status": self.status,
            "confirmed_by": self.confirmed_by,
            "created_at": self.created_at,
            "updated_at": self.updated_at,
        }


@dataclass
class BatchEvent:
    room_id: str
    plan_id: str
    stage: str
    session_id: str = ""
    batch_id: str = ""
    status: str = "running"
    progress: int = 0
    message: str = ""
    intervention_window_open: bool = False
    metadata: dict[str, Any] = field(default_factory=dict)

    def as_dict(self) -> dict[str, Any]:
        return {
            "room_id": self.room_id,
            "plan_id": self.plan_id,
            "session_id": self.session_id,
            "batch_id": self.batch_id,
            "stage": self.stage,
            "status": self.status,
            "progress": self.progress,
            "message": self.message,
            "intervention_window_open": self.intervention_window_open,
            "metadata": _sanitize_control_payload(self.metadata),
        }


@dataclass
class ReviewResult:
    room_id: str
    plan_id: str
    review_type: str
    passed: bool
    findings: list[str] = field(default_factory=list)
    finding_details: list[dict[str, Any]] = field(default_factory=list)
    session_id: str = ""
    batch_id: str = ""
    actor_id: str = ""
    actor_version: int = 0
    severity: str = "warn"
    metadata: dict[str, Any] = field(default_factory=dict)

    def as_dict(self) -> dict[str, Any]:
        return {
            "room_id": self.room_id,
            "plan_id": self.plan_id,
            "session_id": self.session_id,
            "batch_id": self.batch_id,
            "actor_id": self.actor_id,
            "actor_version": self.actor_version,
            "review_type": self.review_type,
            "passed": self.passed,
            "findings": list(self.findings),
            "finding_details": [
                _sanitize_control_payload(item)
                for item in self.finding_details
                if isinstance(item, dict)
            ],
            "severity": self.severity,
            "metadata": _sanitize_control_payload(self.metadata),
        }


class InteractionCoordinator:
    """Deterministic control plane for LANChat multi-user / multi-agent flow.

    This MVP deliberately stays outside native build surfaces. It converts chat
    discussion into SeedPlan state and exposes structured execution/intervention
    hooks that existing services can call without re-entering natural-language
    intent classification.
    """

    _ADD_WORDS = ("添加", "新增", "补", "加一个", "放入", "插入")
    _REMOVE_WORDS = ("删除", "移除", "去掉", "不要")
    _REPAIR_WORDS = (
        "穿模", "重叠", "悬空", "不贴地", "比例不对", "太大", "太小",
        "修", "问题", "不合理", "奇怪", "布局", "摆放", "动线", "挡路",
    )
    _WRONG_PLAN_WORDS = ("执行错", "方案错", "不是这个方案", "正儿八经", "重来")
    _STYLE_WORDS = ("风格", "统一", "更暗", "更亮", "暗黑", "集市")

    def __init__(
        self,
        *,
        scheduler: Any = None,
        gm_proposer: Callable[[ChatMessage, SeedPlan], str] | None = None,
        disclosure_policy: DisclosurePolicy | None = None,
        memory_store: MemoryScopeStore | None = None,
    ) -> None:
        self._scheduler = scheduler
        self._gm_proposer = gm_proposer
        self._disclosure_policy = disclosure_policy or DisclosurePolicy()
        self._memory_store = memory_store or MemoryScopeStore()
        self._plans: dict[str, SeedPlan] = {}
        self._room_active_plan: dict[str, str] = {}
        self._pending_interventions: dict[str, list[InterventionRequest]] = {}
        self._pending_conflict_resolutions: dict[str, ConflictResolutionProposal] = {}
        self._pending_final_adjustment_conflicts: dict[str, dict[str, Any]] = {}
        self._pending_layout_reflow_proposals: dict[str, LayoutReflowProposal] = {}
        self._generation_jobs_by_plan: dict[str, list[GenerationJobRef]] = {}
        self._active_generation_session_by_plan: dict[str, str] = {}
        self._replan_source_by_plan: dict[str, str] = {}
        self._latest_batch_by_plan: dict[str, tuple[str, int]] = {}
        self._scene_contracts_by_plan: dict[str, SceneDesignContract] = {}
        self._terrain_resolver = TerrainComponentResolver()
        self._events: list[CoordinatorEvent] = []
        self._disclosure_events: list[DisclosureEvent] = []
        self._disclosure_events_start_index = 0

    @property
    def events(self) -> list[CoordinatorEvent]:
        return list(self._events)

    @property
    def disclosure_events(self) -> list[DisclosureEvent]:
        return list(self._disclosure_events)

    def disclosure_events_since(self, start_index: int) -> tuple[list[DisclosureEvent], int]:
        cursor = max(0, int(start_index or 0))
        local_start = max(0, cursor - self._disclosure_events_start_index)
        events = list(self._disclosure_events[local_start:])
        next_cursor = self._disclosure_events_start_index + len(self._disclosure_events)
        return events, max(0, next_cursor - cursor)

    def ingest_message(self, message: ChatMessage | dict[str, Any]) -> CoordinatorEvent:
        msg = self._coerce_message(message)
        active = self.active_plan_for_room(msg.room_id)
        if self._is_status_query(msg.text):
            return self._handle_status_query(msg, active)
        if active and active.status == SeedPlanStatus.COMPLETED and self._is_layout_reflow_confirmation(msg.text):
            result = self.confirm_layout_reflow_proposal(
                self._layout_reflow_proposal_id_from_text(msg.text, msg.room_id),
                msg.sender_id,
                decision="reject" if self._is_layout_reflow_rejection(msg.text) else "confirm",
                room_id=msg.room_id,
            )
            event_type = (
                "layout_reflow_confirmation_failed"
                if not result.ok
                else "layout_reflow_rejected"
                if result.payload.get("status") == "rejected"
                else "layout_reflow_confirmed"
            )
            return self._record(event_type, result.message, result.payload)
        if active and active.status in {SeedPlanStatus.CONFIRMED, SeedPlanStatus.EXECUTING, SeedPlanStatus.PAUSED}:
            request = self._intervention_from_message(msg, active)
            decision = self.ingest_intervention(request)
            self._record_disclosures(
                room_id=msg.room_id,
                stage="intervention",
                progress=0,
                plan=active.as_dict(),
                intervention=decision.payload,
            )
            return self._record("intervention_routed", decision.message, decision.payload)
        if active and active.status == SeedPlanStatus.COMPLETED and self._intent_type(msg.text) == "add":
            request = self._intervention_from_message(msg, active)
            request.intent_type = "post_generation_add"
            request.apply_policy = "post_generation_add"
            decision = self.ingest_intervention(request)
            self._record_disclosures(
                room_id=msg.room_id,
                stage="intervention",
                progress=100,
                plan=active.as_dict(),
                intervention=decision.payload,
            )
            return self._record("post_generation_add_routed", decision.message, decision.payload)
        if active and active.status == SeedPlanStatus.COMPLETED and self._is_post_generation_adjustment(msg.text):
            request = self._intervention_from_message(msg, active)
            request.apply_policy = "final_adjustment"
            if request.intent_type == "status_query":
                return self._handle_status_query(msg, active)
            decision = self.ingest_intervention(request)
            if not self._is_layout_reflow_request(msg.text, request):
                self._record_disclosures(
                    room_id=msg.room_id,
                    stage="final_adjustment",
                    progress=100,
                    plan=active.as_dict(),
                    intervention=decision.payload,
                )
                return self._record("final_adjustment_routed", decision.message, decision.payload)
            proposal = self._create_layout_reflow_proposal(active, msg, request)
            self._record_disclosures(
                room_id=msg.room_id,
                stage="final_adjustment",
                progress=100,
                plan=active.as_dict(),
                intervention={**decision.payload, "layout_reflow_proposal": proposal.as_dict()},
            )
            return self._record("layout_reflow_proposal_created", self._layout_reflow_message(proposal), {
                **decision.payload,
                "layout_reflow_proposal": proposal.as_dict(),
            })

        plan = self.create_or_update_seed_plan(msg)
        if active is not None and active.status == SeedPlanStatus.CLARIFYING:
            self._record_clarification_answer(msg, plan)
        self._record_memory(
            room_id=msg.room_id,
            plan_id=plan.plan_id,
            agent_id=msg.agent_id,
            entry_type="discussion",
            text=msg.text,
            actor_id=msg.sender_id,
            visibility="shared",
            metadata={"sender_name": msg.sender_name, "is_host": msg.is_host},
        )
        if self._looks_ready_to_propose(msg.text):
            plan.propose()
            proposal = self._gm_proposer(msg, plan) if self._gm_proposer else self._default_proposal(plan)
            self._record_disclosures(
                room_id=msg.room_id,
                stage="proposed",
                progress=0,
                plan=plan.as_dict(),
            )
            return self._record("seed_plan_proposed", proposal, {"plan": plan.as_dict()})
        self._record_disclosures(
            room_id=msg.room_id,
            stage=plan.status.value,
            progress=0,
            plan=plan.as_dict(),
        )
        return self._record(
            "seed_plan_updated",
            "已记录讨论意图，等待 GM 整理或房主确认。",
            {"plan": plan.as_dict()},
        )

    def create_or_update_seed_plan(self, message: ChatMessage | dict[str, Any]) -> SeedPlan:
        msg = self._coerce_message(message)
        plan = self.active_plan_for_room(msg.room_id)
        if plan is None or plan.status in {SeedPlanStatus.EXECUTING, SeedPlanStatus.COMPLETED, SeedPlanStatus.CANCELLED}:
            plan = SeedPlan(room_id=msg.room_id, host_id=msg.sender_id if msg.is_host else "")
            self._plans[plan.plan_id] = plan
            self._room_active_plan[msg.room_id] = plan.plan_id
        plan.add_intent(ParticipantIntent(
            user_id=msg.sender_id,
            user_name=msg.sender_name,
            text=msg.text,
            priority=1 if msg.is_host else 0,
        ))
        self._update_plan_agent_ownership(plan, msg)
        self._infer_constraints(plan, msg.text)
        self._refresh_plan_design_brief(plan, source="coordinator_message")
        logger.info(
            "[SeedPlanTrace] phase=update room=%s plan=%s status=%s sender=%s/%s host=%s agent=%s/%s participants=%s text_len=%s design_len=%s source=%s text=%s",
            msg.room_id,
            plan.plan_id,
            str(getattr(plan.status, "value", plan.status)),
            msg.sender_id,
            msg.sender_name,
            msg.is_host,
            msg.agent_id,
            msg.agent_name,
            len(getattr(plan, "participants", {}) or {}),
            len(str(msg.text or "")),
            len(str(getattr(plan, "design_brief", "") or "")),
            str((msg.metadata or {}).get("source") or ""),
            _coordinator_trace_preview(msg.text),
        )
        return plan

    def _update_plan_agent_ownership(self, plan: SeedPlan, msg: ChatMessage) -> None:
        metadata = dict(msg.metadata or {})
        agent_id = str(msg.agent_id or metadata.get("target_agent_id") or "").strip()
        agent_name = str(msg.agent_name or metadata.get("target_agent_name") or "").strip()
        if agent_id or agent_name:
            plan.review_policy["owner_agent_id"] = agent_id or agent_name
            plan.review_policy["owner_agent_name"] = agent_name or agent_id
            plan.review_policy["plan_version"] = plan.version
        source_agents = plan.review_policy.setdefault("source_context_agents", [])
        source_name = str(metadata.get("source_context_agent") or metadata.get("source_context_agent_name") or "").strip()
        if source_name and source_name not in source_agents:
            source_agents.append(source_name)

    def propose_seed_plan(self, room_id: str) -> SeedPlan:
        plan = self.active_plan_for_room(room_id)
        if plan is None:
            plan = SeedPlan(room_id=room_id)
            self._plans[plan.plan_id] = plan
            self._room_active_plan[room_id] = plan.plan_id
        plan.propose()
        return plan

    def propose_replan_from_paused(
        self,
        plan_id: str,
        *,
        proposer_id: str = "gm",
        note: str = "",
    ) -> SeedPlan:
        paused_plan = self._require_plan(plan_id)
        if paused_plan.status != SeedPlanStatus.PAUSED:
            raise ValueError(f"SeedPlan {plan_id} must be paused before replan")
        replan = paused_plan.append_intervention_version()
        replan.host_id = paused_plan.host_id
        for intervention in self._pending_interventions.get(plan_id, []):
            if intervention.content:
                replan.add_intent(ParticipantIntent(
                    user_id=intervention.source_user_id or proposer_id,
                    user_name="",
                    text=intervention.content,
                    priority=max(1, int(intervention.priority)),
                ))
        if note:
            replan.add_intent(ParticipantIntent(
                user_id=proposer_id,
                user_name="GM",
                text=note,
                priority=2,
            ))
        replan.propose()
        self._plans[replan.plan_id] = replan
        self._room_active_plan[replan.room_id] = replan.plan_id
        self._pending_interventions[replan.plan_id] = list(self._pending_interventions.get(plan_id, []))
        self._prune_pending_interventions(replan.plan_id)
        self._replan_source_by_plan[replan.plan_id] = plan_id
        self._record_memory(
            room_id=replan.room_id,
            plan_id=replan.plan_id,
            entry_type="replan_proposed",
            text=replan.intent_summary,
            actor_id=proposer_id,
            visibility="shared",
            metadata={"source_plan_id": plan_id, "plan_version": replan.version},
        )
        self._record_disclosures(
            room_id=replan.room_id,
            stage="proposed",
            progress=0,
            plan=replan.as_dict(),
            intervention={"intent_type": "replan", "apply_policy": "host_confirmation", "priority": 2},
        )
        self._record(
            "seed_plan_replan_proposed",
            f"SeedPlan {plan_id} 已暂停，已生成重提案 {replan.plan_id}，等待房主确认。",
            {"source_plan_id": plan_id, "plan": replan.as_dict()},
        )
        return replan

    def propose_conflict_resolution(
        self,
        plan_id: str,
        *,
        proposed_by: str = "gm",
        recommendation: str = "",
    ) -> ConflictResolutionProposal:
        plan = self._require_plan(plan_id)
        if plan.status not in {SeedPlanStatus.DRAFT, SeedPlanStatus.CLARIFYING, SeedPlanStatus.PROPOSED}:
            raise ValueError(f"SeedPlan {plan_id} cannot accept conflict proposal from {plan.status}")
        conflict_items = list(plan.conflicts)
        if not conflict_items:
            raise ValueError(f"SeedPlan {plan_id} has no recorded conflicts")
        proposal = ConflictResolutionProposal(
            proposal_id=f"cr-{uuid.uuid4().hex[:12]}",
            room_id=plan.room_id,
            plan_id=plan.plan_id,
            proposed_by=proposed_by,
            conflict_items=conflict_items,
            recommendation=recommendation or self._default_conflict_recommendation(plan),
        )
        self._pending_conflict_resolutions[proposal.proposal_id] = proposal
        self._record_memory(
            room_id=plan.room_id,
            plan_id=plan.plan_id,
            entry_type="conflict_resolution_proposed",
            text=proposal.recommendation,
            actor_id=proposed_by,
            visibility="shared",
            metadata=proposal.as_dict(),
        )
        self._record_disclosures(
            room_id=plan.room_id,
            stage="proposed",
            progress=0,
            plan=plan.as_dict(),
            intervention={
                "intent_type": "conflict",
                "apply_policy": "host_confirmation",
                "priority": 2,
                "proposal_id": proposal.proposal_id,
            },
        )
        self._record(
            "conflict_resolution_proposed",
            "GM 已提出冲突决议候选，等待房主确认。",
            {"proposal": proposal.as_dict()},
        )
        return proposal

    def confirm_conflict_resolution(self, proposal_id: str, host_id: str) -> ConfirmResult:
        proposal = self._pending_conflict_resolutions.get(proposal_id)
        if proposal is None:
            return ConfirmResult(False, "", "找不到对应冲突决议候选。", {"proposal_id": proposal_id})
        host_id = str(host_id or "").strip()
        if not host_id:
            return ConfirmResult(False, proposal.plan_id, "缺少房主确认身份，不能确认冲突决议。", {"proposal": proposal.as_dict()})
        if proposal.status == "rejected":
            return ConfirmResult(False, proposal.plan_id, "该冲突决议候选已拒绝，不能再次确认。", {"proposal": proposal.as_dict()})
        plan = self._require_plan(proposal.plan_id)
        if plan.status not in {SeedPlanStatus.DRAFT, SeedPlanStatus.CLARIFYING, SeedPlanStatus.PROPOSED}:
            return ConfirmResult(
                False,
                plan.plan_id,
                "当前 SeedPlan 已冻结，不能再确认讨论阶段冲突决议。",
                {"proposal": proposal.as_dict()},
            )
        proposal.status = "confirmed"
        proposal.confirmed_by = host_id
        self._pending_conflict_resolutions.pop(proposal.proposal_id, None)
        self._pending_conflict_resolutions[proposal.proposal_id] = proposal
        resolutions = list(plan.review_policy.get("conflict_resolutions") or [])
        proposal_dict = proposal.as_dict()
        existing_index = next((
            index
            for index, item in enumerate(resolutions)
            if isinstance(item, dict) and item.get("proposal_id") == proposal.proposal_id
        ), -1)
        if existing_index >= 0:
            resolutions[existing_index] = proposal_dict
        else:
            resolutions.append(proposal_dict)
        plan.review_policy["conflict_resolutions"] = resolutions
        plan.updated_at = time.time()
        self._record_memory(
            room_id=plan.room_id,
            plan_id=plan.plan_id,
            entry_type="conflict_resolution_confirmed",
            text=proposal.recommendation,
            actor_id=host_id,
            visibility="shared",
            metadata=proposal.as_dict(),
        )
        self._record_disclosures(
            room_id=plan.room_id,
            stage=plan.status.value,
            progress=0,
            plan=plan.as_dict(),
            intervention={"intent_type": "conflict", "apply_policy": "confirmed", "priority": 2},
        )
        self._record(
            "conflict_resolution_confirmed",
            "房主已确认冲突决议，可进入方案确认。",
            {"proposal": proposal.as_dict(), "plan": plan.as_dict()},
        )
        self._prune_resolved_conflict_proposals()
        return ConfirmResult(True, plan.plan_id, "冲突决议已确认。", {"proposal": proposal.as_dict(), "plan": plan.as_dict()})

    def reject_conflict_resolution(self, proposal_id: str, host_id: str) -> ConfirmResult:
        proposal = self._pending_conflict_resolutions.get(proposal_id)
        if proposal is None:
            return ConfirmResult(False, "", "找不到对应冲突决议候选。", {"proposal_id": proposal_id})
        host_id = str(host_id or "").strip()
        if not host_id:
            return ConfirmResult(False, proposal.plan_id, "缺少房主确认身份，不能拒绝冲突决议。", {"proposal": proposal.as_dict()})
        plan = self._require_plan(proposal.plan_id)
        if plan.status not in {SeedPlanStatus.DRAFT, SeedPlanStatus.CLARIFYING, SeedPlanStatus.PROPOSED}:
            return ConfirmResult(
                False,
                plan.plan_id,
                "当前 SeedPlan 已冻结，不能再拒绝讨论阶段冲突决议。",
                {"proposal": proposal.as_dict()},
            )
        if proposal.status == "confirmed":
            return ConfirmResult(False, plan.plan_id, "该冲突决议候选已确认，不能再拒绝。", {"proposal": proposal.as_dict()})
        proposal.status = "rejected"
        proposal.confirmed_by = host_id
        self._pending_conflict_resolutions.pop(proposal.proposal_id, None)
        self._pending_conflict_resolutions[proposal.proposal_id] = proposal
        resolutions = list(plan.review_policy.get("conflict_resolutions") or [])
        proposal_dict = proposal.as_dict()
        existing_index = next((
            index
            for index, item in enumerate(resolutions)
            if isinstance(item, dict) and item.get("proposal_id") == proposal.proposal_id
        ), -1)
        if existing_index >= 0:
            resolutions[existing_index] = proposal_dict
        else:
            resolutions.append(proposal_dict)
        plan.review_policy["conflict_resolutions"] = resolutions
        plan.updated_at = time.time()
        self._record_memory(
            room_id=plan.room_id,
            plan_id=plan.plan_id,
            entry_type="conflict_resolution_rejected",
            text=proposal.recommendation,
            actor_id=host_id,
            visibility="shared",
            metadata=proposal.as_dict(),
        )
        self._record_disclosures(
            room_id=plan.room_id,
            stage=plan.status.value,
            progress=0,
            plan=plan.as_dict(),
            intervention={"intent_type": "conflict", "apply_policy": "rejected", "priority": 2},
        )
        self._record(
            "conflict_resolution_rejected",
            "房主已拒绝冲突决议候选，需要继续讨论或由 GM 重新提案。",
            {"proposal": proposal.as_dict(), "plan": plan.as_dict()},
        )
        self._prune_resolved_conflict_proposals()
        return ConfirmResult(True, plan.plan_id, "冲突决议已拒绝。", {"proposal": proposal.as_dict(), "plan": plan.as_dict()})

    def confirm_seed_plan(self, plan_id: str, host_id: str) -> ConfirmResult:
        plan = self._require_plan(plan_id)
        host_id = str(host_id or "").strip()
        if not host_id:
            return ConfirmResult(False, plan.plan_id, "缺少房主确认身份，不能确认 SeedPlan。", {"plan": plan.as_dict()})
        pending_clarifications = self._pending_clarifications(plan)
        if pending_clarifications:
            self._record_disclosures(
                room_id=plan.room_id,
                stage=plan.status.value,
                progress=0,
                plan=plan.as_dict(),
                intervention={
                    "intent_type": "clarification",
                    "apply_policy": "request_clarification",
                    "priority": 2,
                    "requires_clarification": True,
                    "status_message": pending_clarifications[-1].get("question", "仍有澄清问题未回答。"),
                },
            )
            return ConfirmResult(
                False,
                plan.plan_id,
                "SeedPlan 仍有未回答的澄清问题，需先补充意图再确认方案。",
                {
                    "plan": plan.as_dict(),
                    "pending_clarifications": [dict(item) for item in pending_clarifications],
                    "requires_clarification": True,
                },
            )
        if self._has_unresolved_conflicts(plan):
            self._record_disclosures(
                room_id=plan.room_id,
                stage=plan.status.value,
                progress=0,
                plan=plan.as_dict(),
                intervention={
                    "intent_type": "conflict",
                    "apply_policy": "host_confirmation",
                    "priority": 2,
                    "requires_conflict_resolution": True,
                },
            )
            return ConfirmResult(
                False,
                plan.plan_id,
                "SeedPlan 仍有未确认冲突决议，需先由 GM 给出候选并由房主确认。",
                {
                    "plan": plan.as_dict(),
                    "unresolved_conflicts": list(plan.conflicts),
                    "requires_conflict_resolution": True,
                },
            )
        contract_text = self._resolved_design_text(plan)
        logger.info(
            "[SeedPlanTrace] phase=confirm_attempt room=%s plan=%s status=%s host=%s participants=%s design_len=%s contract_len=%s summary=%s",
            plan.room_id,
            plan.plan_id,
            str(getattr(plan.status, "value", plan.status)),
            host_id,
            len(getattr(plan, "participants", {}) or {}),
            len(str(getattr(plan, "design_brief", "") or "")),
            len(str(contract_text or "")),
            _coordinator_trace_preview(getattr(plan, "intent_summary", "") or ""),
        )
        if not contract_text:
            logger.info(
                "[SeedPlanTrace] phase=confirm_reject_no_contract room=%s plan=%s status=%s host=%s design_len=%s",
                plan.room_id,
                plan.plan_id,
                str(getattr(plan.status, "value", plan.status)),
                host_id,
                len(str(getattr(plan, "design_brief", "") or "")),
            )
            self._record_disclosures(
                room_id=plan.room_id,
                stage=plan.status.value,
                progress=0,
                plan=plan.as_dict(),
                intervention={
                    "intent_type": "clarification",
                    "apply_policy": "request_concrete_plan",
                    "priority": 2,
                    "requires_clarification": True,
                    "status_message": "当前没有可确认的具体方案，请先整理或选择一个场景方案。",
                },
            )
            return ConfirmResult(
                False,
                plan.plan_id,
                "当前没有可确认的具体方案，请先整理或选择一个场景方案。",
                {"plan": plan.as_dict(), "requires_concrete_plan": True},
            )
        plan.confirm(host_id)
        contract = build_scene_design_contract(
            room_id=plan.room_id,
            plan_id=plan.plan_id,
            scene_type=plan.scene_type,
            text=contract_text,
        )
        terrain_profile = self._terrain_resolver.derive(contract_text, scene_type=plan.scene_type)
        contract.terrain_spec = terrain_profile.terrain_spec
        contract.boundary_spec = terrain_profile.boundary_spec
        self._scene_contracts_by_plan[plan.plan_id] = contract
        self._record_memory(
            room_id=plan.room_id,
            plan_id=plan.plan_id,
            entry_type="seed_plan_confirmed",
            text=plan.intent_summary,
            actor_id=host_id,
            visibility="shared",
            metadata={
                "plan_version": plan.version,
                "scene_type": plan.scene_type,
                "scene_design_contract": contract.as_dict(),
            },
        )
        payload = {
            "action_type": "start_generation",
            "execution": "coordinator_structured",
            "plan_id": plan.plan_id,
            "plan_version": plan.version,
            "room_id": plan.room_id,
            "source_user_id": host_id,
            "intent_text": self._execution_prompt_for_plan(plan, contract.as_dict()),
            "plan_summary": plan.intent_summary,
            "seed_plan": plan.as_dict(),
            "scene_design_contract": contract.as_dict(),
            "requires_host_confirm": False,
            "status": "confirmed",
        }
        self._record_disclosures(
            room_id=plan.room_id,
            stage="confirmed",
            progress=0,
            plan=plan.as_dict(),
        )
        logger.info(
            "[SeedPlanTrace] phase=confirm_ok room=%s plan=%s host=%s version=%s contract_len=%s scene_type=%s",
            plan.room_id,
            plan.plan_id,
            host_id,
            plan.version,
            len(str(contract_text or "")),
            plan.scene_type,
        )
        return ConfirmResult(True, plan.plan_id, f"SeedPlan {plan.plan_id} 已确认。", payload)

    def execute_confirmed_plan(self, plan_id: str) -> GenerationJobRef:
        plan = self._require_plan(plan_id)
        if plan.status != SeedPlanStatus.CONFIRMED:
            raise ValueError(f"SeedPlan {plan_id} must be confirmed before execution")
        plan.mark_executing()
        self._record_memory(
            room_id=plan.room_id,
            plan_id=plan.plan_id,
            entry_type="generation_started",
            text=f"SeedPlan {plan.plan_id} entered batch generation.",
            actor_id=plan.confirmed_by or plan.host_id,
            visibility="shared",
            metadata={"plan_version": plan.version, "source_plan_id": self._replan_source_by_plan.get(plan.plan_id, "")},
        )
        scheduler_resume = self._resume_scheduler_for_replan(plan)
        execution_session_id = f"exec-{plan.plan_id}-{uuid.uuid4().hex[:8]}"
        self._active_generation_session_by_plan[plan.plan_id] = execution_session_id
        payload = {
            "job_type": "scene_generation",
            "plan_id": plan.plan_id,
            "plan_version": plan.version,
            "room_id": plan.room_id,
            "session_id": execution_session_id,
            "prompt": self._execution_prompt_for_plan(plan, self.scene_design_contract(plan.plan_id)),
            "intent_text": self._execution_prompt_for_plan(plan, self.scene_design_contract(plan.plan_id)),
            "seed_plan": plan.as_dict(),
            "scene_design_contract": self.scene_design_contract(plan.plan_id),
            "_runtime_context": {"interaction_coordinator": self},
        }
        if scheduler_resume:
            payload["scheduler_resume"] = scheduler_resume
        if self._scheduler is not None and hasattr(self._scheduler, "submit"):
            self._start_lanchat_runtime_compose(plan)
            submitted = self._scheduler.submit(payload)
            if isinstance(submitted, GenerationJobRef):
                self._record_generation_ref_disclosure(plan, submitted)
                self._remember_generation_job_ref(submitted)
                return submitted
            if isinstance(submitted, dict):
                ref = GenerationJobRef(
                    job_id=str(submitted.get("job_id") or f"job-{uuid.uuid4().hex[:12]}"),
                    plan_id=plan.plan_id,
                    status=str(submitted.get("status") or "queued"),
                    session_id=str(submitted.get("session_id") or execution_session_id),
                    payload=submitted,
                )
                self._record_generation_ref_disclosure(plan, ref)
                self._remember_generation_job_ref(ref)
                return ref
        self._record_disclosures(
            room_id=plan.room_id,
            stage="queued",
            progress=0,
            plan=plan.as_dict(),
        )
        ref = GenerationJobRef(
            job_id=f"job-{uuid.uuid4().hex[:12]}",
            plan_id=plan.plan_id,
            status="queued",
            session_id=execution_session_id,
            payload=payload,
        )
        self._remember_generation_job_ref(ref)
        return ref

    def _record_generation_ref_disclosure(self, plan: SeedPlan, ref: GenerationJobRef) -> None:
        status = str(ref.status or "").strip().lower()
        if status in {"queued", "waiting_user", "paused"}:
            stage = "queued" if status == "queued" else "waiting_resource"
            progress = 0
        elif status in {"done", "completed"}:
            stage = "completed"
            progress = 100
        else:
            stage = "executing"
            progress = 0
        self._record_disclosures(
            room_id=plan.room_id,
            stage=stage,
            progress=progress,
            plan=plan.as_dict(),
        )

    def execute_action_payload(self, payload: dict[str, Any]) -> str:
        if str(payload.get("action_type") or "") == "post_generation_add":
            return self.execute_post_generation_add(payload)
        plan_id = str(payload.get("plan_id") or payload.get("resolved_from_plan_id") or "")
        seed_plan = payload.get("seed_plan")
        if not plan_id and isinstance(seed_plan, dict):
            plan_id = str(seed_plan.get("plan_id") or "")
        if not plan_id:
            return "no typed actor delta: confirmed action has no SeedPlan reference"
        if plan_id not in self._plans and isinstance(seed_plan, dict):
            self._plans[plan_id] = SeedPlan.from_dict(seed_plan)
            self._room_active_plan[self._plans[plan_id].room_id] = plan_id
        if str(payload.get("status") or "").lower() == "confirmed":
            plan = self._require_plan(plan_id)
            if plan.status not in {SeedPlanStatus.CONFIRMED, SeedPlanStatus.EXECUTING}:
                plan.confirm(str(payload.get("source_user_id") or payload.get("host_id") or "host"))
        ref = self.execute_confirmed_plan(plan_id)
        return f"SeedPlan {plan_id} 已进入生成队列：{ref.job_id} ({ref.status})"

    def execute_post_generation_add(self, payload: dict[str, Any]) -> str:
        room_id = str(payload.get("room_id") or "")
        plan_id = str(payload.get("plan_id") or payload.get("resolved_from_plan_id") or "")
        seed_plan = payload.get("seed_plan")
        if not plan_id and isinstance(seed_plan, dict):
            plan_id = str(seed_plan.get("plan_id") or "")
        if plan_id not in self._plans and isinstance(seed_plan, dict) and plan_id:
            self._plans[plan_id] = SeedPlan.from_dict(seed_plan)
            self._room_active_plan[self._plans[plan_id].room_id] = plan_id
        plan = self._plans.get(plan_id) if plan_id else self.active_plan_for_room(room_id)
        if plan is None:
            return "no typed actor delta: confirmed add action has no active SeedPlan"
        content = str(
            payload.get("resolved_intent_text")
            or payload.get("intent_text")
            or payload.get("content")
            or ""
        ).strip()
        if not content:
            return "no typed actor delta: confirmed add action has no object request"
        request = InterventionRequest(
            room_id=plan.room_id,
            plan_id=plan.plan_id,
            source_user_id=str(payload.get("source_user_id") or payload.get("host_id") or plan.host_id or ""),
            session_id=str(payload.get("session_id") or ""),
            content=content,
            intent_type="post_generation_add",
            priority=2,
            apply_policy="post_generation_add",
        )
        if plan.status in {SeedPlanStatus.CONFIRMED, SeedPlanStatus.EXECUTING, SeedPlanStatus.PAUSED}:
            request.intent_type = "add"
            request.apply_policy = "next_batch"
            decision = self.ingest_intervention(request)
            self._record_disclosures(
                room_id=plan.room_id,
                stage="intervention",
                progress=0,
                plan=plan.as_dict(),
                intervention={
                    **decision.payload,
                    "status_message": "生成仍在进行，新增请求已进入下一批吸收队列。",
                },
            )
            return "已记录追加请求，将在下一批前吸收。"
        decision = self.ingest_intervention(request)
        execution_session_id = f"append-{plan.plan_id}-{uuid.uuid4().hex[:8]}"
        self._active_generation_session_by_plan[plan.plan_id] = execution_session_id
        job_payload = {
            "job_type": "scene_generation_append",
            "action_type": "post_generation_add",
            "append_mode": True,
            "max_items": int(payload.get("max_items") or 2),
            "plan_id": plan.plan_id,
            "plan_version": plan.version,
            "room_id": plan.room_id,
            "session_id": execution_session_id,
            "prompt": content,
            "intent_text": content,
            "seed_plan": plan.as_dict(),
            "pending_interventions": [request.as_dict()],
            "latest_intervention": request.as_dict(),
            "scene_design_contract": self.scene_design_contract(plan.plan_id),
            "_runtime_context": {"interaction_coordinator": self},
        }
        if self._scheduler is not None and hasattr(self._scheduler, "submit"):
            submitted = self._scheduler.submit(job_payload)
            if isinstance(submitted, GenerationJobRef):
                ref = submitted
            elif isinstance(submitted, dict):
                ref = GenerationJobRef(
                    job_id=str(submitted.get("job_id") or f"job-{uuid.uuid4().hex[:12]}"),
                    plan_id=plan.plan_id,
                    status=str(submitted.get("status") or "queued"),
                    session_id=str(submitted.get("session_id") or execution_session_id),
                    payload=submitted,
                )
            else:
                ref = GenerationJobRef(
                    job_id=f"job-{uuid.uuid4().hex[:12]}",
                    plan_id=plan.plan_id,
                    status="queued",
                    session_id=execution_session_id,
                    payload=job_payload,
                )
        else:
            ref = GenerationJobRef(
                job_id=f"job-{uuid.uuid4().hex[:12]}",
                plan_id=plan.plan_id,
                status="queued",
                session_id=execution_session_id,
                payload=job_payload,
            )
        self._remember_generation_job_ref(ref)
        self._record_memory(
            room_id=plan.room_id,
            plan_id=plan.plan_id,
            entry_type="post_generation_add_started",
            text=content,
            actor_id=request.source_user_id,
            visibility="shared",
            metadata={
                "intervention": decision.payload,
                "generation_job": {
                    "job_id": ref.job_id,
                    "status": ref.status,
                    "session_id": ref.session_id,
                    "plan_id": ref.plan_id,
                },
            },
        )
        self._record_disclosures(
            room_id=plan.room_id,
            stage="executing",
            progress=100 if plan.status == SeedPlanStatus.COMPLETED else 0,
            plan=plan.as_dict(),
            intervention={
                **decision.payload,
                "status_message": "追加生成请求已进入生成队列。",
            },
        )
        return f"追加生成请求已进入生成队列：{ref.job_id} ({ref.status})"

    def control_pace(
        self,
        room_id: str,
        action: str,
        *,
        actor_id: str = "gm",
        note: str = "",
    ) -> CoordinatorEvent:
        """Route GM pacing commands through the same control plane as generation."""
        room = str(room_id or "default")
        normalized = self._normalize_pace_action(action)
        plan = self.active_plan_for_room(room)
        previous_status = plan.status.value if plan is not None else ""
        scheduler_control: dict[str, Any] = {}

        if normalized in {"pause", "discuss"}:
            if plan is not None and plan.status in {
                SeedPlanStatus.CONFIRMED,
                SeedPlanStatus.EXECUTING,
                SeedPlanStatus.PAUSED,
            }:
                if plan.status != SeedPlanStatus.PAUSED:
                    plan.review_policy["_pace_before_pause_status"] = plan.status.value
                plan.pause_execution()
            scheduler_control = self._pause_scheduler_session(room)
            message = (
                "已切到讨论模式，后续生成会在批次边界暂停。"
                if normalized == "discuss"
                else "已暂停后续生成，等待房主或 GM 确认后继续。"
            )
            disclosure_stage = "paused"
            apply_policy = "pause_after_batch"
        elif normalized == "resume":
            if plan is not None and plan.status == SeedPlanStatus.PAUSED:
                restored = str(plan.review_policy.pop("_pace_before_pause_status", "") or "executing")
                plan.status = SeedPlanStatus.CONFIRMED if restored == "confirmed" else SeedPlanStatus.EXECUTING
                plan.updated_at = time.time()
            scheduler_control = self._resume_scheduler_session(room)
            message = "已恢复生成节奏；后续批次会继续按确认方案推进。"
            disclosure_stage = "executing"
            apply_policy = "continue_generation"
        else:
            message = "已记录 GM 节奏控制指令。"
            disclosure_stage = plan.status.value if plan is not None else "draft"
            apply_policy = "control_pace"

        payload = {
            "room_id": room,
            "plan_id": plan.plan_id if plan is not None else "",
            "action": normalized,
            "actor_id": str(actor_id or "gm"),
            "note": str(note or ""),
            "previous_status": previous_status,
            "status": plan.status.value if plan is not None else "",
            "scheduler_control": scheduler_control,
        }
        self._record_memory(
            room_id=room,
            plan_id=payload["plan_id"],
            entry_type="gm_pace_control",
            text=note or message,
            actor_id=payload["actor_id"],
            visibility="shared",
            metadata=payload,
        )
        self._record_disclosures(
            room_id=room,
            stage=disclosure_stage,
            progress=0,
            plan=plan.as_dict() if plan is not None else {"room_id": room},
            intervention={
                "intent_type": "pace_control",
                "apply_policy": apply_policy,
                "priority": 2,
                "status_message": message,
            },
        )
        return self._record("gm_pace_control", message, payload)

    def request_clarification(
        self,
        room_id: str,
        question: str,
        *,
        requested_by: str = "gm",
        target_user_id: str = "",
        target_hint: str = "",
    ) -> CoordinatorEvent:
        room = str(room_id or "default")
        plan = self.active_plan_for_room(room)
        if plan is None:
            plan = SeedPlan(room_id=room)
            self._plans[plan.plan_id] = plan
            self._room_active_plan[room] = plan.plan_id
        if plan.status in {SeedPlanStatus.CONFIRMED, SeedPlanStatus.EXECUTING, SeedPlanStatus.PAUSED, SeedPlanStatus.COMPLETED}:
            return self._record(
                "clarification_rejected",
                "当前 SeedPlan 已冻结或正在执行，不能再进入讨论澄清。",
                {"plan": plan.as_dict(), "question": str(question or "")},
            )
        question_text = str(question or "").strip() or "请补充关键需求。"
        clarification = {
            "clarification_id": f"cl-{uuid.uuid4().hex[:12]}",
            "question": question_text,
            "requested_by": str(requested_by or "gm"),
            "target_user_id": str(target_user_id or ""),
            "target_hint": str(target_hint or ""),
            "status": "pending",
            "created_at": time.time(),
        }
        requests = list(plan.review_policy.get("clarification_requests") or [])
        requests.append(clarification)
        plan.review_policy["clarification_requests"] = requests
        plan.status = SeedPlanStatus.CLARIFYING
        plan.updated_at = time.time()
        self._record_memory(
            room_id=room,
            plan_id=plan.plan_id,
            entry_type="clarification_requested",
            text=question_text,
            actor_id=clarification["requested_by"],
            visibility="shared",
            metadata=clarification,
        )
        self._record_disclosures(
            room_id=room,
            stage="clarifying",
            progress=0,
            plan=plan.as_dict(),
            intervention={
                "intent_type": "clarification",
                "apply_policy": "request_clarification",
                "priority": 2,
                "target_hint": clarification["target_hint"],
                "status_message": question_text,
                "requires_clarification": True,
                "clarification_id": clarification["clarification_id"],
            },
        )
        return self._record("clarification_requested", "GM 已请求补充澄清。", {"plan": plan.as_dict(), "clarification": clarification})

    def ingest_intervention(self, intervention: InterventionRequest | dict[str, Any]) -> InterventionDecision:
        request = self._coerce_intervention(intervention)
        plan = self._plans.get(request.plan_id)
        if plan is None:
            return InterventionDecision(False, "missing_plan", "找不到对应 SeedPlan，介入未执行。", request.as_dict())
        route = request.apply_policy or self._route_for_intervention(request)
        request.apply_policy = route
        if not request.batch_id:
            request.batch_id = self._latest_batch_by_plan.get(request.plan_id, ("", 0))[0]
        self._remember_pending_intervention(request)
        message = {
            "next_batch": "已记录该介入，将在下一批次前吸收。",
            "geometry_review": "已记录为几何/摆放修复请求，将进入审查队列。",
            "pause_and_replan": "已暂停后续批次，等待 GM 重新整理方案。",
            "final_adjustment": "已记录为最终调整优先项。",
            "post_generation_add": "已记录为追加生成请求，将创建后续追加批次。",
        }.get(route, "已记录该介入。")
        self._update_scene_contract_for_intervention(plan, request, route)
        self._record_memory(
            room_id=request.room_id or plan.room_id,
            plan_id=request.plan_id,
            agent_id=request.actor_id,
            batch_id=request.batch_id,
            entry_type="intervention",
            text=request.content,
            actor_id=request.actor_id,
            visibility="shared",
            metadata={
                "intervention_id": request.intervention_id,
                "intent_type": request.intent_type,
                "apply_policy": route,
                "actor_version": request.actor_version,
                "target_hint": request.target_hint,
                "source_user_id": request.source_user_id,
            },
        )
        payload = request.as_dict()
        scheduler_updates = self._try_update_future_generation_jobs(request)
        if scheduler_updates:
            payload["scheduler_updates"] = scheduler_updates
            payload["scheduler_update_summary"] = self._summarize_scheduler_updates(scheduler_updates)
        scheduler_control = self._apply_scheduler_control_for_intervention(plan, request)
        if scheduler_control:
            payload["scheduler_control"] = scheduler_control
        return InterventionDecision(True, route, message, payload)

    def pending_interventions(self, plan_id: str) -> list[InterventionRequest]:
        return list(self._pending_interventions.get(plan_id, []))

    def _remember_pending_intervention(self, request: InterventionRequest) -> None:
        interventions = self._pending_interventions.setdefault(request.plan_id, [])
        interventions.append(request)
        self._prune_pending_interventions(request.plan_id)

    def _prune_pending_interventions(self, plan_id: str) -> None:
        interventions = self._pending_interventions.get(plan_id, [])
        if len(interventions) <= MAX_PENDING_INTERVENTIONS_PER_PLAN:
            return

        def value_score(item: InterventionRequest) -> int:
            route_weight = {
                "pause_and_replan": 100,
                "geometry_review": 85,
                "final_adjustment": 80,
                "next_batch": 20,
            }.get(item.apply_policy, 10)
            target_bonus = 15 if (item.actor_id or item.target_hint or item.finding_details) else 0
            return route_weight + max(0, int(item.priority)) * 20 + target_bonus

        ranked = sorted(
            enumerate(interventions),
            key=lambda pair: (value_score(pair[1]), pair[0]),
            reverse=True,
        )
        keep_indexes = {index for index, _ in ranked[:MAX_PENDING_INTERVENTIONS_PER_PLAN]}
        self._pending_interventions[plan_id] = [
            item for index, item in enumerate(interventions) if index in keep_indexes
        ]

    def _prune_resolved_conflict_proposals(self) -> None:
        resolved_keys = [
            key
            for key, proposal in self._pending_conflict_resolutions.items()
            if proposal.status in {"confirmed", "rejected"}
        ]
        for key in resolved_keys[:-MAX_RESOLVED_COORDINATOR_PROPOSALS]:
            self._pending_conflict_resolutions.pop(key, None)

    def _prune_resolved_final_adjustment_conflicts(self) -> None:
        resolved_keys = [
            key
            for key, proposal in self._pending_final_adjustment_conflicts.items()
            if str(proposal.get("status") or "proposed") in {"confirmed", "rejected"}
        ]
        for key in resolved_keys[:-MAX_RESOLVED_COORDINATOR_PROPOSALS]:
            self._pending_final_adjustment_conflicts.pop(key, None)

    def _remember_generation_job_ref(self, ref: GenerationJobRef) -> None:
        if ref.session_id:
            self._active_generation_session_by_plan[ref.plan_id] = ref.session_id
        refs = self._generation_jobs_by_plan.setdefault(ref.plan_id, [])
        refs.append(ref)
        if len(refs) > MAX_GENERATION_JOB_REFS_PER_PLAN:
            del refs[:len(refs) - MAX_GENERATION_JOB_REFS_PER_PLAN]

    def ingest_generation_job_status(self, job: dict[str, Any]) -> CoordinatorEvent:
        plan_id = str(job.get("plan_id") or "").strip()
        plan = self._plans.get(plan_id)
        if plan is None:
            return self._record(
                "generation_job_status_rejected",
                "生成任务状态所属方案不存在，已拒收。",
                {"job": _sanitize_control_payload(job), "reject_reason": "unknown_plan"},
            )
        if str(job.get("room_id") or plan.room_id) != str(plan.room_id):
            return self._record(
                "generation_job_status_rejected",
                "生成任务状态房间与方案不匹配，已拒收。",
                {"job": _sanitize_control_payload(job), "reject_reason": "room_mismatch"},
            )
        if not self._generation_session_matches(plan_id, str(job.get("session_id") or "")):
            return self._record(
                "generation_job_status_rejected",
                "生成任务状态所属执行会话已过期，已拒收。",
                {"job": _sanitize_control_payload(job), "reject_reason": "session_mismatch"},
            )

        status = str(job.get("status") or "").strip().lower()
        refs = self._generation_jobs_by_plan.setdefault(plan_id, [])
        job_id = str(job.get("job_id") or "").strip()
        existing = next((ref for ref in reversed(refs) if ref.job_id == job_id), None)
        if existing is not None:
            existing.status = status or existing.status
            existing.session_id = str(job.get("session_id") or existing.session_id)
            existing.payload = _sanitize_control_payload(job)
        else:
            refs.append(GenerationJobRef(
                job_id=job_id or f"job-{uuid.uuid4().hex[:12]}",
                plan_id=plan_id,
                status=status or "unknown",
                session_id=str(job.get("session_id") or ""),
                payload=_sanitize_control_payload(job),
            ))
            if len(refs) > MAX_GENERATION_JOB_REFS_PER_PLAN:
                del refs[:len(refs) - MAX_GENERATION_JOB_REFS_PER_PLAN]

        progress = 100 if status in {"done", "completed"} else 0
        if status in {"done", "completed"}:
            plan.mark_completed()
            self._active_generation_session_by_plan.pop(plan_id, None)
            self._end_lanchat_runtime_compose(plan)
            stage = "completed"
            message = "主生成任务已完成，可以继续追加生成或做最终调整。"
            event_type = "generation_completed"
        elif status in {"failed", "cancelled", "abandoned"}:
            self._end_lanchat_runtime_compose(plan)
            stage = "failed" if status == "failed" else "cancelled"
            message = "生成任务已结束但未完成，请查看资源状态或重新发起。"
            event_type = "generation_terminal"
        else:
            stage = "executing"
            message = "生成任务状态已更新。"
            event_type = "generation_status_updated"

        self._record_memory(
            room_id=plan.room_id,
            plan_id=plan_id,
            batch_id=str(job.get("batch_id") or ""),
            entry_type=event_type,
            text=message,
            visibility="shared",
            metadata={
                "job_id": job_id,
                "status": status,
                "current_stage": str(job.get("current_stage") or ""),
            },
        )
        self._record_disclosures(
            room_id=plan.room_id,
            stage=stage,
            progress=progress,
            plan=plan.as_dict(),
            intervention={
                "intent_type": "generation_job_status",
                "apply_policy": "read_only",
                "status_message": message,
            },
        )
        return self._record(event_type, message, {"job": _sanitize_control_payload(job), "plan": plan.as_dict()})

    def _generation_session_matches(self, plan_id: str, session_id: str) -> bool:
        incoming = str(session_id or "").strip()
        expected = str(self._active_generation_session_by_plan.get(plan_id) or "").strip()
        if not incoming or not expected:
            return True
        return incoming == expected

    def _start_lanchat_runtime_compose(self, plan: SeedPlan) -> None:
        try:
            from .lanchat_scene_runtime import get_lanchat_scene_runtime
            get_lanchat_scene_runtime().start_compose(
                plan.confirmed_by or plan.host_id or "host",
                plan.intent_summary,
            )
        except Exception:
            return

    def _end_lanchat_runtime_compose(self, plan: SeedPlan) -> None:
        try:
            from .lanchat_scene_runtime import get_lanchat_scene_runtime
            get_lanchat_scene_runtime().end_compose(plan.confirmed_by or plan.host_id or None)
        except Exception:
            return

    def final_adjustment_plan(
        self,
        plan_id: str,
        *,
        recent_batch_window: int = 2,
        limit: int = 8,
    ) -> dict[str, Any]:
        """Summarize pending interventions for final assembly/review.

        The final pass should prefer the latest strong interventions, keep
        geometry/VLM failures visible, and avoid letting old weak requests
        overrule later user corrections.
        """
        plan = self._require_plan(plan_id)
        latest_batch_id, latest_batch_index = self._latest_batch_by_plan.get(plan_id, ("", 0))
        candidates: list[dict[str, Any]] = []
        deferred: list[dict[str, Any]] = []
        conflicts: list[dict[str, Any]] = []
        resolved_conflicts: list[dict[str, Any]] = []
        rejected_target_keys: set[str] = set()
        actor_intents: dict[str, dict[str, Any]] = {}

        for order, intervention in enumerate(self.pending_interventions(plan_id), start=1):
            batch_index = self._batch_index(intervention.batch_id)
            is_recent = batch_index == 0 or latest_batch_index == 0 or (
                batch_index >= max(1, latest_batch_index - max(0, recent_batch_window) + 1)
            )
            score = self._intervention_score(intervention, order, is_recent)
            item = {
                **intervention.as_dict(),
                "batch_index": batch_index,
                "is_recent": is_recent,
                "score": score,
                "selection_reason": self._selection_reason(intervention, is_recent),
            }
            target_keys = self._final_adjustment_item_target_key_list(item)
            for target_key in target_keys:
                target_state = actor_intents.setdefault(
                    target_key,
                    {
                        "actor_id": intervention.actor_id,
                        "target_hint": intervention.target_hint,
                        "intents": set(),
                    },
                )
                if intervention.actor_id and not target_state.get("actor_id"):
                    target_state["actor_id"] = intervention.actor_id
                if intervention.target_hint and not target_state.get("target_hint"):
                    target_state["target_hint"] = intervention.target_hint
                target_state["intents"].add(intervention.intent_type)
            if not is_recent and intervention.apply_policy == "next_batch" and intervention.priority < 2:
                item["defer_reason"] = "early_low_priority_request_superseded_by_later_batches"
                deferred.append(item)
            else:
                candidates.append(item)

        emitted_conflict_keys: set[str] = set()
        for target_key, target_state in actor_intents.items():
            intents = target_state.get("intents", set())
            if "remove" in intents and ({"add", "modify", "style_adjust"} & intents):
                actor_id = str(target_state.get("actor_id") or "")
                target_hint = str(target_state.get("target_hint") or "")
                conflict_identity = actor_id or target_hint or str(target_key)
                if conflict_identity in emitted_conflict_keys:
                    continue
                emitted_conflict_keys.add(conflict_identity)
                proposal_id = f"fa-{plan.plan_id}-{uuid.uuid5(uuid.NAMESPACE_URL, f'{plan.plan_id}:{target_key}').hex[:12]}"
                conflict = {
                    "actor_id": actor_id,
                    "target_hint": target_hint,
                    "target_key": target_key,
                    "proposal_id": proposal_id,
                    "reason": (
                        "same_actor_has_remove_and_keep_modify_requests"
                        if actor_id
                        else "same_target_has_remove_and_keep_modify_requests"
                    ),
                    "resolution": "ask GM/host to confirm before final import",
                }
                existing = self._pending_final_adjustment_conflicts.get(proposal_id)
                if existing is not None:
                    conflict["status"] = str(existing.get("status") or "proposed")
                    conflict["confirmed_by"] = str(existing.get("confirmed_by") or "")
                else:
                    conflict["status"] = "proposed"
                    self._pending_final_adjustment_conflicts[proposal_id] = {
                        **conflict,
                        "plan_id": plan.plan_id,
                        "room_id": plan.room_id,
                        "status": "proposed",
                        "confirmed_by": "",
                        "updated_at": time.time(),
                    }
                status = str(conflict.get("status") or "proposed")
                if status == "confirmed":
                    conflict["resolution"] = "host_confirmed_final_adjustment"
                    resolved_conflicts.append(conflict)
                elif status == "rejected":
                    conflict["resolution"] = "host_rejected_final_adjustment"
                    rejected_target_keys.update({
                        str(target_key),
                        actor_id,
                        target_hint,
                    })
                    rejected_target_keys.discard("")
                    resolved_conflicts.append(conflict)
                else:
                    conflicts.append(conflict)
        if conflicts:
            self._record_disclosures(
                room_id=plan.room_id,
                stage="final_adjustment",
                progress=0,
                plan=plan.as_dict(),
                intervention={
                    "intent_type": "conflict",
                    "apply_policy": "host_confirmation",
                    "priority": 2,
                    "proposal_id": str(conflicts[0].get("proposal_id") or ""),
                },
            )
            self._record(
                "final_adjustment_conflict_requires_confirmation",
                "最终调整存在冲突项，需要 GM/房主确认后再执行。",
                {
                    "plan_id": plan.plan_id,
                    "room_id": plan.room_id,
                    "conflicts": list(conflicts),
                },
            )

        if rejected_target_keys:
            remaining_candidates: list[dict[str, Any]] = []
            for item in candidates:
                item_target_keys = self._final_adjustment_item_target_keys(item)
                if item_target_keys & rejected_target_keys:
                    item["defer_reason"] = "final_adjustment_conflict_rejected_by_host"
                    deferred.append(item)
                else:
                    remaining_candidates.append(item)
            candidates = remaining_candidates

        candidates.sort(key=lambda item: (-int(item["score"]), -int(item["batch_index"]), item["intervention_id"]))
        selected = candidates[:max(0, limit)]
        overflow = candidates[max(0, limit):]
        for item in overflow:
            item["defer_reason"] = "lower_score_overflow"
        deferred.extend(overflow)

        summary_text = "；".join(item["content"] for item in selected if item.get("content"))
        if summary_text:
            self._record_memory(
                room_id=plan.room_id,
                plan_id=plan.plan_id,
                batch_id=latest_batch_id,
                entry_type="final_adjustment_plan",
                text=summary_text,
                visibility="shared",
                metadata={
                    "latest_batch_id": latest_batch_id,
                    "latest_batch_index": latest_batch_index,
                    "selected_count": len(selected),
                    "deferred_count": len(deferred),
                    "conflict_count": len(conflicts),
                    "resolved_conflict_count": len(resolved_conflicts),
                },
            )

        return {
            "plan_id": plan.plan_id,
            "room_id": plan.room_id,
            "latest_batch_id": latest_batch_id,
            "latest_batch_index": latest_batch_index,
            "recent_batch_window": recent_batch_window,
            "selected": selected,
            "deferred": deferred,
            "conflicts": conflicts,
            "resolved_conflicts": resolved_conflicts,
            "summary_text": summary_text,
        }

    def _create_layout_reflow_proposal(
        self,
        plan: SeedPlan,
        message: ChatMessage,
        request: InterventionRequest,
    ) -> LayoutReflowProposal:
        proposal = LayoutReflowProposal(
            proposal_id=f"layout-{uuid.uuid4().hex[:12]}",
            room_id=plan.room_id,
            plan_id=plan.plan_id,
            reason=request.content or message.text,
            actor_hints=self._layout_reflow_actor_hints(request.content or message.text),
            layout_strategy=[
                "清出中央通道，优先降低模型互相遮挡和位置冲突",
                "大型或功能性物体靠边分区摆放，小件围绕功能区聚合",
                "焦点物体放到后方或中轴，不删除、不替换、不重新生成模型",
            ],
            deltas=[
                {"op": "clear_path", "path": "center"},
                {"op": "group_align", "group": "large_or_functional", "zone": "side"},
                {"op": "move", "target": "focal_or_latest", "zone": "back_center"},
            ],
        )
        self._pending_layout_reflow_proposals[proposal.proposal_id] = proposal
        self._record_memory(
            room_id=plan.room_id,
            plan_id=plan.plan_id,
            entry_type="layout_reflow_proposed",
            text=proposal.reason,
            actor_id=message.sender_id,
            visibility="shared",
            metadata=proposal.as_dict(),
        )
        return proposal

    @staticmethod
    def _is_layout_reflow_request(text: str, request: InterventionRequest) -> bool:
        raw = str(text or "")
        is_broad_layout_request = any(word in raw for word in (
            "调整一下布局", "布局不合理", "布局不是很合理", "位置冲突",
            "模型位置冲突", "摆放有问题", "通道被挡", "清出通道", "动线不合理",
            "浮空", "飘起来", "飘起", "没贴地", "不贴地", "悬空", "离地",
        )) or (
            any(word in raw for word in ("布局", "摆放", "动线", "位置"))
            and any(word in raw for word in ("调整", "冲突", "不合理", "有问题", "挡路"))
        )
        if not is_broad_layout_request:
            return False
        if request.actor_id:
            return False
        generic_layout_hints = {
            "一下布局",
            "布局",
            "摆放",
            "动线",
            "位置",
            "模型位置冲突",
        }
        if request.target_hint and request.target_hint not in generic_layout_hints:
            return False
        return True

    @staticmethod
    def _layout_reflow_actor_hints(text: str) -> list[str]:
        raw = str(text or "")
        hints: list[str] = []
        for token in re.split(r"[，,。；;\s]+", raw):
            token = token.strip("“”\"'：:、")
            if not token or token in {"调整", "布局", "一下", "我看", "有些", "模型", "位置", "冲突", "不合理"}:
                continue
            if any(word in token for word in ("布局", "调整", "冲突", "不合理", "摆放", "通道", "挡路")):
                continue
            if 1 <= len(token) <= 16:
                hints.append(token)
        seen: set[str] = set()
        out: list[str] = []
        for hint in hints:
            if hint in seen:
                continue
            seen.add(hint)
            out.append(hint)
        return out[:8]

    @staticmethod
    def _layout_reflow_message(proposal: LayoutReflowProposal) -> str:
        return (
            f"【布局调整建议 {proposal.proposal_id}】建议做一次低风险重排："
            "清出中央通道、按功能分区摆放，并把焦点物体放到更稳定的位置。"
            "不会删除、替换或重新生成模型。房主可回复："
            f"确认调整 {proposal.proposal_id} / 拒绝调整 {proposal.proposal_id}。"
        )

    def _layout_reflow_proposal_id_from_text(self, text: str, room_id: str = "") -> str:
        match = re.search(r"\b(layout-[A-Za-z0-9_-]+)\b", str(text or ""))
        if match:
            return match.group(1)
        for proposal in reversed(list(self._pending_layout_reflow_proposals.values())):
            if proposal.status == "proposed" and (not room_id or proposal.room_id == room_id):
                return proposal.proposal_id
        return ""

    @staticmethod
    def _is_layout_reflow_confirmation(text: str) -> bool:
        raw = str(text or "").strip()
        return (
            bool(re.search(r"\b(layout-[A-Za-z0-9_-]+)\b", raw))
            and any(word in raw for word in ("确认", "拒绝", "取消"))
        ) or any(word in raw for word in ("确认调整", "执行调整", "应用调整", "拒绝调整", "取消调整"))

    @staticmethod
    def _is_layout_reflow_rejection(text: str) -> bool:
        return any(word in str(text or "") for word in ("拒绝", "取消", "不要调整", "不调整"))

    def confirm_layout_reflow_proposal(
        self,
        proposal_id: str,
        host_id: str,
        *,
        decision: str = "confirm",
        room_id: str = "",
    ) -> ConfirmResult:
        proposal_key = str(proposal_id or "").strip()
        if not proposal_key:
            proposal_key = self._layout_reflow_proposal_id_from_text("", room_id)
        proposal = self._pending_layout_reflow_proposals.get(proposal_key)
        if proposal is None:
            return ConfirmResult(False, "", "找不到对应布局调整建议。", {"proposal_id": proposal_key, "status": "missing"})
        host = str(host_id or "").strip()
        if not host:
            return ConfirmResult(False, proposal.plan_id, "缺少房主确认身份，不能执行布局调整。", {"proposal": proposal.as_dict()})
        normalized = "rejected" if str(decision or "").lower() in {"reject", "rejected", "no", "cancel"} else "confirmed"
        proposal.status = normalized
        proposal.confirmed_by = host
        proposal.updated_at = time.time()
        payload = proposal.as_dict()
        self._record_memory(
            room_id=proposal.room_id,
            plan_id=proposal.plan_id,
            entry_type="layout_reflow_confirmed" if normalized == "confirmed" else "layout_reflow_rejected",
            text=proposal.reason,
            actor_id=host,
            visibility="shared",
            metadata=payload,
        )
        self._record_disclosures(
            room_id=proposal.room_id,
            stage="final_adjustment",
            progress=100,
            plan=self._plans.get(proposal.plan_id).as_dict() if proposal.plan_id in self._plans else {"plan_id": proposal.plan_id, "room_id": proposal.room_id},
            intervention={
                "intent_type": "final_adjustment_request",
                "apply_policy": normalized,
                "priority": 2,
                "proposal_id": proposal.proposal_id,
                "status_message": "布局调整建议已确认。" if normalized == "confirmed" else "布局调整建议已取消。",
                "layout_reflow_proposal": payload,
            },
        )
        return ConfirmResult(
            True,
            proposal.plan_id,
            "布局调整建议已确认，准备执行低风险重排。" if normalized == "confirmed" else "布局调整建议已取消。",
            payload,
        )

    @staticmethod
    def _final_adjustment_item_target_keys(item: dict[str, Any]) -> set[str]:
        return set(InteractionCoordinator._final_adjustment_item_target_key_list(item))

    @staticmethod
    def _final_adjustment_item_target_key_list(item: dict[str, Any]) -> list[str]:
        keys: list[str] = []

        def add_key(value: Any) -> None:
            key = str(value or "").strip()
            if key and key not in keys:
                keys.append(key)

        add_key(item.get("target_hint"))
        add_key(item.get("target_key"))
        add_key(item.get("actor_id"))
        details = item.get("finding_details")
        if isinstance(details, list):
            for detail in details:
                if not isinstance(detail, dict):
                    continue
                add_key(detail.get("target_hint"))
                add_key(detail.get("target_key"))
                add_key(detail.get("actor_id"))
                add_key(detail.get("target_actor_id"))
        return keys

    def confirm_final_adjustment_conflict(
        self,
        proposal_id: str,
        host_id: str,
        *,
        decision: str = "confirm",
    ) -> ConfirmResult:
        proposal_key = str(proposal_id or "").strip()
        if not proposal_key:
            return ConfirmResult(False, "", "缺少最终调整冲突确认编号。", {"proposal_id": proposal_key})
        proposal = self._pending_final_adjustment_conflicts.get(proposal_key)
        if proposal is None:
            return ConfirmResult(False, "", "找不到对应最终调整冲突候选。", {"proposal_id": proposal_key})
        host_id = str(host_id or "").strip()
        if not host_id:
            return ConfirmResult(False, str(proposal.get("plan_id") or ""), "缺少房主确认身份，不能处理最终调整冲突。", {"proposal": dict(proposal)})
        normalized = "rejected" if str(decision or "").lower() in {"reject", "rejected", "no", "cancel"} else "confirmed"
        proposal["status"] = normalized
        proposal["confirmed_by"] = host_id
        proposal["updated_at"] = time.time()
        self._pending_final_adjustment_conflicts.pop(proposal_key, None)
        self._pending_final_adjustment_conflicts[proposal_key] = proposal
        plan_id = str(proposal.get("plan_id") or "")
        room_id = str(proposal.get("room_id") or "")
        event_type = (
            "final_adjustment_conflict_confirmed"
            if normalized == "confirmed"
            else "final_adjustment_conflict_rejected"
        )
        self._record_memory(
            room_id=room_id,
            plan_id=plan_id,
            entry_type=event_type,
            text=str(proposal.get("target_hint") or proposal.get("actor_id") or proposal_key),
            actor_id=host_id,
            visibility="shared",
            metadata=dict(proposal),
        )
        self._record_disclosures(
            room_id=room_id,
            stage="final_adjustment",
            progress=0,
            plan=self._plans.get(plan_id).as_dict() if plan_id in self._plans else {"plan_id": plan_id, "room_id": room_id},
            intervention={
                "intent_type": "conflict",
                "apply_policy": normalized,
                "priority": 2,
                "proposal_id": proposal_key,
            },
        )
        self._record(
            event_type,
            (
                f"房主已确认最终调整冲突 {proposal_key}。"
                if normalized == "confirmed"
                else f"房主已拒绝最终调整冲突 {proposal_key}。"
            ),
            {"proposal": dict(proposal)},
        )
        self._prune_resolved_final_adjustment_conflicts()
        return ConfirmResult(
            True,
            plan_id,
            "最终调整冲突确认已记录。" if normalized == "confirmed" else "最终调整冲突拒绝已记录。",
            {"proposal": dict(proposal)},
        )

    def bind_scene_session_progress(
        self,
        session: Any,
        *,
        room_id: str,
        plan_id: str,
        session_id: str = "",
    ) -> None:
        setter = getattr(session, "set_progress_event_sink", None)
        if not callable(setter):
            raise TypeError("session does not support structured progress events")

        def _sink(event: dict[str, Any]) -> None:
            self.ingest_batch_event(self.batch_event_from_progress(
                event,
                room_id=room_id,
                plan_id=plan_id,
                session_id=session_id,
            ))

        setter(_sink)

    def batch_event_from_progress(
        self,
        event: dict[str, Any],
        *,
        room_id: str,
        plan_id: str,
        session_id: str = "",
    ) -> BatchEvent:
        status = str(event.get("status") or "running")
        phase = str(event.get("phase") or "batch")
        batch_id = str(event.get("batch_id") or "")
        if not batch_id:
            batch_id = f"progress-{phase.replace('#', '_')}"
        return BatchEvent(
            room_id=room_id,
            plan_id=plan_id,
            session_id=session_id,
            batch_id=batch_id,
            stage="batch_boundary" if status in {"done", "paused"} else "batch",
            status=status,
            progress=int(event.get("percent") or 0),
            message=str(event.get("user_message") or event.get("message") or ""),
            intervention_window_open=status in {"done", "paused"},
            metadata={
                "phase": phase,
                "asset_count": int(event.get("asset_count") or 0),
                "imported_count": int(event.get("imported_count") or 0),
                "cumulative_imported": int(event.get("cumulative_imported") or 0),
                "total_assets": int(event.get("total_assets") or 0),
                "batch_index": int(event.get("batch_index") or 0),
                "batch_total": int(event.get("batch_total") or 0),
            },
        )

    def ingest_batch_event(self, event: BatchEvent | dict[str, Any]) -> CoordinatorEvent:
        batch_event = self._coerce_batch_event(event)
        plan = self._plans.get(batch_event.plan_id)
        if plan is None:
            return self._record(
                "batch_event_rejected",
                "批次事件所属方案不存在，已拒收。",
                {**batch_event.as_dict(), "reject_reason": "unknown_plan"},
            )
        if str(plan.room_id or "") != str(batch_event.room_id or ""):
            return self._record(
                "batch_event_rejected",
                "批次事件房间与方案不匹配，已拒收。",
                {**batch_event.as_dict(), "reject_reason": "room_mismatch", "expected_room_id": plan.room_id},
            )
        if not self._generation_session_matches(batch_event.plan_id, batch_event.session_id):
            return self._record(
                "batch_event_rejected",
                "批次事件所属执行会话已过期，已拒收。",
                {
                    **batch_event.as_dict(),
                    "reject_reason": "session_mismatch",
                    "expected_session_id": self._active_generation_session_by_plan.get(batch_event.plan_id, ""),
                },
            )
        plan_payload = plan.as_dict() if plan else {"plan_id": batch_event.plan_id, "room_id": batch_event.room_id}
        batch_index = int(batch_event.metadata.get("batch_index") or 0) or self._batch_index(batch_event.batch_id)
        if batch_index:
            current = self._latest_batch_by_plan.get(batch_event.plan_id, ("", 0))
            if batch_index >= current[1]:
                self._latest_batch_by_plan[batch_event.plan_id] = (batch_event.batch_id, batch_index)
        self._record_memory(
            room_id=batch_event.room_id,
            plan_id=batch_event.plan_id,
            batch_id=batch_event.batch_id,
            entry_type="batch_event",
            text=batch_event.message or f"{batch_event.stage}:{batch_event.status}",
            visibility="shared",
            metadata=batch_event.as_dict(),
        )
        disclosure_stage = "intervention" if batch_event.intervention_window_open else (
            "batch" if "batch" in batch_event.stage else batch_event.stage
        )
        self._record_disclosures(
            room_id=batch_event.room_id,
            stage=disclosure_stage,
            progress=batch_event.progress,
            plan=plan_payload,
            intervention={
                "batch_id": batch_event.batch_id,
                "intervention_window_open": batch_event.intervention_window_open,
                "intent_type": "batch_boundary",
                "apply_policy": "next_batch",
                "status": batch_event.status,
                "status_message": batch_event.message,
            },
        )
        if batch_event.intervention_window_open:
            return self._record(
                "batch_intervention_window_open",
                batch_event.message or "当前批次边界已打开，可吸收后续介入。",
                batch_event.as_dict(),
            )
        return self._record("batch_event_ingested", batch_event.message or "批次事件已记录。", batch_event.as_dict())

    def ingest_review_result(self, result: ReviewResult | dict[str, Any]) -> list[CoordinatorEvent]:
        review = self._coerce_review_result(result)
        plan = self._plans.get(review.plan_id)
        if plan is None:
            return [self._record(
                "review_result_rejected",
                "审查结果所属方案不存在，已拒收。",
                {**review.as_dict(), "reject_reason": "unknown_plan"},
            )]
        if str(plan.room_id or "") != str(review.room_id or ""):
            return [self._record(
                "review_result_rejected",
                "审查结果房间与方案不匹配，已拒收。",
                {**review.as_dict(), "reject_reason": "room_mismatch", "expected_room_id": plan.room_id},
            )]
        if not self._generation_session_matches(review.plan_id, review.session_id):
            return [self._record(
                "review_result_rejected",
                "审查结果所属执行会话已过期，已拒收。",
                {
                    **review.as_dict(),
                    "reject_reason": "session_mismatch",
                    "expected_session_id": self._active_generation_session_by_plan.get(review.plan_id, ""),
                },
            )]
        plan_payload = plan.as_dict() if plan else {"plan_id": review.plan_id, "room_id": review.room_id}
        self._record_memory(
            room_id=review.room_id,
            plan_id=review.plan_id,
            batch_id=review.batch_id,
            agent_id=review.actor_id,
            entry_type=f"{review.review_type}_review",
            text="；".join(review.findings) or ("passed" if review.passed else "review failed"),
            visibility="shared",
            metadata=review.as_dict(),
        )
        self._record_disclosures(
            room_id=review.room_id,
            stage="review",
            progress=0,
            plan=plan_payload,
            review=review.as_dict(),
        )
        if review.passed:
            return [self._record(
                "review_passed",
                f"{review.review_type} 审查通过。",
                review.as_dict(),
            )]

        content = "；".join(review.findings) or f"{review.review_type} review failed"
        route = "geometry_review" if review.review_type == "geometry" else "final_adjustment"
        actor_id = self._actor_id_from_review(review)
        target_hint = self._target_hint_from_review(review)
        finding_details = [
            _sanitize_control_payload(item)
            for item in review.finding_details
            if isinstance(item, dict)
        ]
        decision = self.ingest_intervention(InterventionRequest(
            room_id=review.room_id,
            plan_id=review.plan_id,
            session_id=review.session_id,
            batch_id=review.batch_id,
            actor_id=actor_id,
            actor_version=review.actor_version,
            target_hint=target_hint,
            finding_details=finding_details,
            intent_type=f"{review.review_type}_review",
            content=content,
            priority=2 if review.severity in {"error", "fail", "critical"} else 1,
            apply_policy=route,
        ))
        return [
            self._record("review_failed", f"{review.review_type} 审查发现问题。", review.as_dict()),
            self._record("review_intervention_routed", decision.message, decision.payload),
        ]

    def memory_summary(
        self,
        *,
        room_id: str,
        plan_id: str = "",
        agent_id: str = "",
        batch_id: str = "",
        actor_id: str = "",
        visibility: str | None = None,
        limit: int = 8,
    ) -> dict[str, Any]:
        return self._memory_store.summarize(
            room_id=room_id,
            plan_id=plan_id,
            agent_id=agent_id,
            batch_id=batch_id,
            actor_id=actor_id,
            visibility=visibility,
            limit=limit,
        )

    def scene_design_contract(self, plan_id: str) -> dict[str, Any]:
        contract = self._scene_contracts_by_plan.get(plan_id)
        return contract.as_dict() if contract is not None else {}

    def close_room(self, room_id: str) -> None:
        room = str(room_id or "default")
        for plan in list(self._plans.values()):
            if plan.room_id != room:
                continue
            contract = self._scene_contracts_by_plan.get(plan.plan_id)
            if contract is not None:
                close_contract(contract)
        self._memory_store.clear_room(room)

    def disclose(
        self,
        *,
        room_id: str,
        stage: str,
        audience: str,
        progress: int = 0,
        plan_id: str = "",
        intervention: dict[str, Any] | None = None,
        review: dict[str, Any] | None = None,
    ) -> DisclosureEvent:
        plan = self._plans.get(plan_id) if plan_id else self.active_plan_for_room(room_id)
        return self._disclosure_policy.disclose(
            room_id=room_id,
            stage=stage,
            audience=audience,
            progress=progress,
            plan=plan.as_dict() if plan else {},
            intervention=intervention,
            review=review,
        )

    def active_plan_for_room(self, room_id: str) -> SeedPlan | None:
        plan_id = self._room_active_plan.get(room_id)
        return self._plans.get(plan_id or "")

    def get_plan(self, plan_id: str) -> SeedPlan | None:
        return self._plans.get(plan_id)

    def _intervention_from_message(self, message: ChatMessage, plan: SeedPlan) -> InterventionRequest:
        intent_type = self._intent_type(message.text)
        latest_batch_id = self._latest_batch_by_plan.get(plan.plan_id, ("", 0))[0]
        actor_id = canonical_actor_id(self._actor_id_from_message(message))
        target_hint = str(message.metadata.get("target_hint") or "").strip()
        if target_hint:
            target_hint = canonical_actor_id(target_hint)
        return InterventionRequest(
            room_id=message.room_id,
            plan_id=plan.plan_id,
            source_user_id=message.sender_id,
            batch_id=latest_batch_id,
            actor_id=actor_id,
            actor_version=int(message.metadata.get("actor_version") or 0),
            target_hint=target_hint or canonical_actor_id(self._target_hint(message.text, actor_id)),
            content=message.text,
            intent_type=intent_type,
            priority=2 if message.is_host else 1,
            apply_policy=self._apply_policy(message.text, intent_type),
        )

    def _actor_id_from_message(self, message: ChatMessage) -> str:
        for key in ("actor_id", "target_actor_id", "object_id", "target_object_id"):
            value = message.metadata.get(key)
            if value:
                return canonical_actor_id(str(value))
        match = re.search(r"(?:actor|object|模型|物体)[:：#]([A-Za-z0-9_.-]+)", message.text)
        if match:
            return canonical_actor_id(match.group(1))
        hash_match = re.search(r"#([A-Za-z][A-Za-z0-9_.-]{2,})", message.text)
        if hash_match:
            return canonical_actor_id(hash_match.group(1))
        for alias in ("_terrain_boundary", "__terrain_boundary", "terrain_boundary", "地形边界", "栅栏", "边界"):
            if alias in message.text:
                return canonical_actor_id(alias)
        return ""

    def _target_hint(self, text: str, actor_id: str = "") -> str:
        if actor_id:
            return canonical_actor_id(actor_id)
        cleaned = self._clean_target_text(str(text or ""))
        patterns = (
            r"(?:删除|移除|去掉|不要)\s*[:：]?\s*(?P<target>[^，。；,.]{1,24})",
            r"(?:后面|后续|接下来|之后)?\s*(?:再)?(?:新增|增加|添加|补|加入|加)\s*[:：]?(?P<target>[^，。；,.]{1,24})",
            r"(?:调整|修改|缩小|放大|移动)\s*[:：]?\s*(?P<target>[^，。；,.]{1,24})",
            r"(?:问题|报错|异常)\s*[:：]?\s*(?P<target>[^，。；,.]{1,24})",
            r"(?:说明|备注)\s*[:：]?\s*(?P<target>[^，。；,.]{1,24})",
        )
        for pattern in patterns:
            match = re.search(pattern, cleaned)
            if match:
                target = self._normalize_target_hint(match.group("target"))
                return canonical_actor_id(target[:24])
        return ""

    def _target_hint_from_review(self, review: ReviewResult) -> str:
        actor_id = self._actor_id_from_review(review)
        if actor_id:
            return actor_id
        for detail in review.finding_details:
            if not isinstance(detail, dict):
                continue
            for key in ("actor_id", "target_actor_id", "object_id", "target", "target_hint"):
                value = detail.get(key)
                if value:
                    return str(value)
        return self._target_hint("；".join(review.findings))

    @staticmethod
    def _actor_id_from_review(review: ReviewResult) -> str:
        if review.actor_id:
            return review.actor_id
        for detail in review.finding_details:
            if not isinstance(detail, dict):
                continue
            for key in ("actor_id", "target_actor_id", "object_id", "target_object_id"):
                value = detail.get(key)
                if value:
                    return str(value)
        return ""

    def _intent_type(self, text: str) -> str:
        service = get_intent_understanding_service()
        decision = service.classify(text, allow_llm=False)
        if decision.intent == "status_query" or self._is_status_query(text):
            return "status_query"
        if any(word in text for word in self._WRONG_PLAN_WORDS):
            return "wrong_plan"
        if any(word in text for word in self._REPAIR_WORDS):
            return "repair"
        if decision.intent == "intervention_delete":
            if any(word in text for word in ("换成", "替换", "改成", "调整")):
                return "modify"
            return "remove"
        if decision.intent in {"intervention_add", "post_generation_add"}:
            return "add"
        if decision.intent == "final_adjustment_request":
            return "modify"
        if decision.intent == "intervention_modify":
            if any(word in text for word in self._STYLE_WORDS):
                return "style_adjust"
            return "modify"
        if any(word in text for word in self._ADD_WORDS):
            return "add"
        if any(word in text for word in self._REMOVE_WORDS):
            return "remove"
        if any(word in text for word in self._STYLE_WORDS):
            return "style_adjust"
        return "modify"

    def _apply_policy(self, text: str, intent_type: str) -> str:
        if intent_type == "status_query":
            return "status_query"
        if intent_type == "post_generation_add":
            return "post_generation_add"
        if intent_type == "wrong_plan":
            return "pause_and_replan"
        if intent_type == "repair":
            return "geometry_review"
        if any(word in text for word in ("最后", "最终", "收尾")):
            return "final_adjustment"
        return "next_batch"

    def _is_post_generation_adjustment(self, text: str) -> bool:
        service = get_intent_understanding_service()
        decision = service.classify(text, allow_llm=False)
        if decision.intent == "status_query" or self._is_status_query(text):
            return False
        if decision.intent in {
            "intervention_modify",
            "intervention_delete",
            "final_adjustment_request",
        }:
            return True
        if decision.intent in {"intervention_add", "post_generation_add"}:
            return False
        intent_type = self._intent_type(text)
        if intent_type in {"remove", "repair", "style_adjust", "wrong_plan"}:
            return True
        return any(word in str(text or "") for word in (
            "调整", "修改", "移动", "挪", "换成", "替换", "缩小", "放大",
            "布局", "摆放", "动线", "不合理", "奇怪", "挡路", "贴地",
        ))

    def _route_for_intervention(self, request: InterventionRequest) -> str:
        return self._apply_policy(request.content, request.intent_type)

    def _intervention_score(self, intervention: InterventionRequest, order: int, is_recent: bool) -> int:
        route_weight = {
            "pause_and_replan": 90,
            "geometry_review": 75,
            "final_adjustment": 70,
            "next_batch": 35,
        }.get(intervention.apply_policy, 25)
        intent_weight = {
            "vlm_review": 30,
            "geometry_review": 25,
            "repair": 20,
            "remove": 15,
            "style_adjust": 12,
            "add": 10,
            "modify": 8,
        }.get(intervention.intent_type, 5)
        recency_weight = 30 if is_recent else -20
        return route_weight + intent_weight + (intervention.priority * 12) + recency_weight + min(order, 20)

    def _selection_reason(self, intervention: InterventionRequest, is_recent: bool) -> str:
        if intervention.apply_policy == "pause_and_replan":
            return "blocking_plan_conflict"
        if intervention.apply_policy == "geometry_review":
            return "geometry_or_placement_must_be_fixed"
        if intervention.apply_policy == "final_adjustment":
            return "explicit_final_adjustment_or_vlm_issue"
        if is_recent:
            return "recent_batch_intervention"
        return "older_context_kept_for_reference"

    def _try_update_future_generation_jobs(self, request: InterventionRequest) -> list[dict[str, Any]]:
        if request.apply_policy != "next_batch":
            return []
        updater = getattr(self._scheduler, "update_job", None)
        if not callable(updater):
            return []
        job_refs = self._generation_jobs_by_plan.get(request.plan_id, [])
        if not job_refs:
            return []
        pending_next_batch = [
            item.as_dict()
            for item in self._pending_interventions.get(request.plan_id, [])
            if item.apply_policy == "next_batch"
        ]
        payload_updates = {
            "latest_intervention": request.as_dict(),
            "pending_interventions": pending_next_batch,
            "intervention_revision": len(pending_next_batch),
        }
        next_batch_priority = max(
            [int(item.get("priority") or 0) for item in pending_next_batch] or [request.priority]
        )
        updates: list[dict[str, Any]] = []
        for ref in job_refs:
            result = updater(ref.job_id, priority=next_batch_priority, payload_updates=payload_updates)
            if isinstance(result, dict):
                ref.status = str(result.get("status") or ref.status)
                ref.payload = result
                updates.append({
                    "job_id": ref.job_id,
                    "status": str(result.get("status") or ""),
                    "success": bool(result.get("success")),
                    "error": str(result.get("error") or ""),
                })
        return updates

    @staticmethod
    def _summarize_scheduler_updates(updates: list[dict[str, Any]]) -> dict[str, Any]:
        attempted = len(updates)
        updated = sum(1 for item in updates if item.get("success"))
        failed = attempted - updated
        return {
            "attempted_count": attempted,
            "updated_count": updated,
            "failed_count": failed,
            "deferred_to_pending": attempted > 0 and updated == 0,
            "reason": (
                "queued future generation job accepted the intervention update"
                if updated
                else "no queued future generation job accepted the intervention update"
            ),
        }

    def _apply_scheduler_control_for_intervention(
        self,
        plan: SeedPlan,
        request: InterventionRequest,
    ) -> dict[str, Any]:
        if request.apply_policy != "pause_and_replan":
            return {}
        try:
            plan.pause_execution()
        except ValueError as exc:
            return {"action": "pause_session", "success": False, "error": str(exc)}
        pauser = getattr(self._scheduler, "pause_session", None)
        if not callable(pauser):
            return {"action": "pause_session", "success": False, "error": "scheduler has no pause_session"}
        result = pauser(plan.room_id)
        if isinstance(result, dict):
            return {
                "action": "pause_session",
                "session_id": str(result.get("session_id") or plan.room_id),
                "status": str(result.get("status") or ""),
                "success": bool(result.get("success")),
                "error": str(result.get("error") or ""),
            }
        return {"action": "pause_session", "session_id": plan.room_id, "success": True, "status": "paused", "error": ""}

    def _resume_scheduler_for_replan(self, plan: SeedPlan) -> dict[str, Any]:
        source_plan_id = self._replan_source_by_plan.get(plan.plan_id, "")
        if not source_plan_id:
            return {}
        resumer = getattr(self._scheduler, "resume_session", None)
        if not callable(resumer):
            return {"action": "resume_session", "success": False, "error": "scheduler has no resume_session"}
        result = resumer(plan.room_id)
        if isinstance(result, dict):
            return {
                "action": "resume_session",
                "source_plan_id": source_plan_id,
                "session_id": str(result.get("session_id") or plan.room_id),
                "status": str(result.get("status") or ""),
                "success": bool(result.get("success")),
                "error": str(result.get("error") or ""),
            }
        return {
            "action": "resume_session",
            "source_plan_id": source_plan_id,
            "session_id": plan.room_id,
            "status": "running",
            "success": True,
            "error": "",
        }

    @staticmethod
    def _normalize_pace_action(action: str) -> str:
        text = str(action or "").strip().lower()
        if text in {"pause", "paused", "暂停", "先停", "等一下", "stop"}:
            return "pause"
        if text in {"resume", "continue", "继续", "恢复", "run"}:
            return "resume"
        if text in {"discuss", "discussion", "先讨论", "不要生成", "别生成", "先规划"}:
            return "discuss"
        return text or "control"

    def _pause_scheduler_session(self, room_id: str) -> dict[str, Any]:
        if self._scheduler is None:
            return {"available": False, "reason": "generation scheduler has not been initialized"}
        pause = getattr(self._scheduler, "pause_session", None)
        if not callable(pause):
            return {"available": False, "reason": "generation scheduler does not expose pause_session"}
        try:
            result = pause(room_id)
        except Exception as exc:  # noqa: BLE001
            return {"available": True, "success": False, "error": str(exc), "session_id": room_id}
        if isinstance(result, dict):
            return {"available": True, **result}
        return {"available": True, "success": True, "result": result, "session_id": room_id}

    def _resume_scheduler_session(self, room_id: str) -> dict[str, Any]:
        if self._scheduler is None:
            return {"available": False, "reason": "generation scheduler has not been initialized"}
        resume = getattr(self._scheduler, "resume_session", None)
        if not callable(resume):
            return {"available": False, "reason": "generation scheduler does not expose resume_session"}
        try:
            result = resume(room_id)
        except Exception as exc:  # noqa: BLE001
            return {"available": True, "success": False, "error": str(exc), "session_id": room_id}
        if isinstance(result, dict):
            return {"available": True, **result}
        return {"available": True, "success": True, "result": result, "session_id": room_id}

    def _batch_index(self, batch_id: str) -> int:
        if not batch_id:
            return 0
        match = re.search(r"(?:batch[-_]?|_b|#)(\d+)(?!.*\d)", str(batch_id), flags=re.IGNORECASE)
        if match:
            return int(match.group(1))
        numbers = re.findall(r"\d+", str(batch_id))
        return int(numbers[-1]) if numbers else 0

    def _infer_constraints(self, plan: SeedPlan, text: str) -> None:
        if any(word in text for word in ("室内", "房间", "大厅")):
            plan.scene_type = "indoor"
        elif any(word in text for word in ("室外", "广场", "地形", "山坡")):
            plan.scene_type = "outdoor"
        elif any(word in text for word in ("混合", "内外", "室内外")):
            plan.scene_type = "mixed"
        if any(word in text for word in self._STYLE_WORDS) and text not in plan.style_constraints:
            plan.style_constraints.append(text)
        if any(word in text for word in ("穿模", "接地", "比例", "摆放", "地形")) and text not in plan.placement_constraints:
            plan.placement_constraints.append(text)
        negative_preference_only = any(word in text for word in ("不要太恐怖", "不太恐怖", "不要全是暗黑风"))
        if any(word in text for word in ("冲突", "不同意", "但是")) and text not in plan.conflicts:
            plan.conflicts.append(text)
        elif "不要" in text and not negative_preference_only and text not in plan.conflicts:
            plan.conflicts.append(text)
        plan.updated_at = time.time()

    def _refresh_plan_design_brief(self, plan: SeedPlan, *, source: str = "") -> None:
        if plan.status in {SeedPlanStatus.CONFIRMED, SeedPlanStatus.EXECUTING, SeedPlanStatus.COMPLETED}:
            return
        texts = [
            str(item.text or "").strip()
            for item in plan.participants
            if self._is_meaningful_design_text(str(item.text or ""))
        ]
        if not texts:
            return
        brief = "；".join(texts[-6:])
        items = self._design_items_from_text(brief)
        plan.set_design_brief(brief, items, source=source)

    def _resolved_design_text(self, plan: SeedPlan) -> str:
        brief = str(getattr(plan, "design_brief", "") or "").strip()
        if brief and self._is_meaningful_design_text(brief):
            return brief
        texts = [
            str(item.text or "").strip()
            for item in plan.participants
            if self._is_meaningful_design_text(str(item.text or ""))
        ]
        return "；".join(texts[-6:]).strip()

    @staticmethod
    def _is_meaningful_design_text(text: str) -> bool:
        raw = re.sub(r"@\S+\s*", "", str(text or "")).strip()
        if not raw:
            return False
        if InteractionCoordinator._is_status_query(raw):
            return False
        if InteractionCoordinator._is_confirmation_only_text(raw):
            return False
        placeholders = (
            "写明要改的风格",
            "物件、布局或限制",
            "新增一个...",
            "调整...",
            "问题...",
            "请输入",
        )
        if any(item in raw for item in placeholders):
            return False
        strong_update_words = ("采用", "按", "确认", "补充", "新增", "调整", "修改", "生成", "开始", "做一个")
        opinion_patterns = ("怎么看", "你觉得", "大家觉得", "评价", "看法")
        if any(word in raw for word in opinion_patterns) and not any(word in raw for word in strong_update_words):
            return False
        elaboration_prefixes = (
            "继续输出",
            "继续说",
            "展开说明",
            "详细说明",
            "说说",
            "讲讲",
        )
        if raw.startswith(elaboration_prefixes) and not any(word in raw for word in strong_update_words):
            return False
        return True

    @staticmethod
    def _is_confirmation_only_text(text: str) -> bool:
        raw = str(text or "").strip()
        if not raw:
            return True
        normalized = re.sub(r"[\s:：，,。.!！?？、；;\"'“”‘’（）()【】\[\]-]+", "", raw)
        control_words = (
            "确认开始生成",
            "确认开始",
            "确认生成",
            "确认方案",
            "开始生成",
            "开始执行",
            "直接生成",
            "执行生成",
            "按当前方案",
            "按这个方案",
            "生成",
            "确认",
            "开始",
        )
        if normalized in control_words:
            return True
        reduced = normalized
        for word in control_words:
            reduced = reduced.replace(word, "")
        reduced = reduced.replace("方案", "").replace("当前", "").replace("这个", "")
        return not reduced.strip()

    @staticmethod
    def _design_items_from_text(text: str) -> list[str]:
        raw = str(text or "")
        candidates: list[str] = []
        for token in re.split(r"[、,，；;\n]+", raw):
            value = token.strip(" ：:。.!！?？")
            if not value:
                continue
            if len(value) > 24:
                continue
            if any(word in value for word in ("摊", "灯", "门", "牌", "桌", "椅", "柜", "箱", "墙", "路", "棚", "台")):
                candidates.append(value)
        seen: set[str] = set()
        out: list[str] = []
        for item in candidates:
            if item in seen:
                continue
            seen.add(item)
            out.append(item)
        return out[:12]

    @staticmethod
    def _is_status_query(text: str) -> bool:
        raw = str(text or "")
        return any(word in raw for word in (
            "到哪步", "到哪一步", "到哪里", "生成到哪里", "生成情况", "查看生成情况",
            "现在的生成计划", "当前生成计划", "进度", "为什么不执行", "怎么还不执行",
            "执行了吗", "开始了吗", "现在的生成方案", "当前生成方案", "生成方案是什么",
            "了解现在的生成方案", "我们开始生成了吗", "现在情况", "什么情况",
            "情况是什么", "生成计划是什么", "为什么执行生成计划", "现在生成到哪里",
        ))

    def _handle_status_query(self, message: ChatMessage, plan: SeedPlan | None) -> CoordinatorEvent:
        if plan is None:
            return self._record(
                "status_query",
                "当前还没有已整理的生成方案。",
                {"room_id": message.room_id, "intent_type": "status_query", "status": "no_plan"},
            )
        jobs = [ref.payload for ref in self._generation_jobs_by_plan.get(plan.plan_id, [])]
        latest_batch_id, latest_batch_index = self._latest_batch_by_plan.get(plan.plan_id, ("", 0))
        payload = {
            "room_id": message.room_id,
            "plan_id": plan.plan_id,
            "intent_type": "status_query",
            "status": plan.status.value,
            "latest_batch_id": latest_batch_id,
            "latest_batch_index": latest_batch_index,
            "generation_jobs": _sanitize_control_payload(jobs),
            "scene_design_contract": self.scene_design_contract(plan.plan_id),
        }
        self._record_disclosures(
            room_id=message.room_id,
            stage=plan.status.value,
            progress=0,
            plan=plan.as_dict(),
            intervention={"intent_type": "status_query", "apply_policy": "read_only", "status_message": "已查询当前生成状态。"},
        )
        latest_job_status = self._latest_generation_job_status(plan.plan_id)
        return self._record("status_query", self._status_query_message(plan, latest_batch_id, latest_job_status), payload)

    def _latest_generation_job_status(self, plan_id: str) -> str:
        refs = self._generation_jobs_by_plan.get(plan_id, [])
        for ref in reversed(refs):
            status = str(ref.status or "").strip().lower()
            if status:
                return status
        return ""

    @staticmethod
    def _status_query_message(plan: SeedPlan, latest_batch_id: str, latest_job_status: str = "") -> str:
        if plan.status == SeedPlanStatus.EXECUTING:
            if latest_job_status in {"queued", "waiting_user", "paused"}:
                return "当前方案已进入生成队列，正在等待资源调度；你可以继续补充要求，我会在后续批次前吸收。"
            suffix = f"，最近批次：{latest_batch_id}" if latest_batch_id else ""
            return f"当前方案正在生成中{suffix}。"
        if plan.status == SeedPlanStatus.CONFIRMED:
            return "当前方案已确认，等待进入生成队列。"
        if plan.status == SeedPlanStatus.COMPLETED:
            return "当前主生成已完成，可以继续追加生成或做最终调整。"
        return f"当前方案状态：{plan.status.value}。"

    @staticmethod
    def _clean_target_text(text: str) -> str:
        cleaned = re.sub(r"@\S+\s*", "", str(text or "")).strip()
        return cleaned.strip(" “‘”\"'，。；;,.")

    @classmethod
    def _normalize_target_hint(cls, value: str) -> str:
        target = cls._clean_target_text(value)
        target = re.sub(
            r"^(?:后面|后续|接下来|之后|再|新增|增加|添加|加入|加|补|一个|一只|一座|一盏|一张|一把)+",
            "",
            target,
        ).strip()
        target = re.sub(r"^(?:个|只|座|盏|张|把)", "", target).strip()
        target = re.sub(r"(?:要|得|需要|应该|放在|摆在).*$", "", target).strip()
        if target in {"一下", "一下吧", "一点", "一些", "一下子", "这个", "它"}:
            return ""
        return target or cls._clean_target_text(value)

    @staticmethod
    def _execution_prompt_for_plan(plan: SeedPlan, contract: dict[str, Any] | None = None) -> str:
        contract = dict(contract or {})
        parts: list[str] = []
        design_brief = str(getattr(plan, "design_brief", "") or "").strip()
        if design_brief:
            parts.append("## 已确认 SeedPlan 方案")
            parts.append(design_brief)
        elif plan.intent_summary:
            parts.append("## 已确认多人方案")
            parts.append(plan.intent_summary)
        if plan.style_constraints:
            parts.append("## 风格约束")
            parts.extend(f"- {item}" for item in plan.style_constraints[-8:])
        if plan.spatial_constraints:
            parts.append("## 空间约束")
            parts.extend(f"- {item}" for item in plan.spatial_constraints[-8:])
        if plan.asset_constraints:
            parts.append("## 资产约束")
            parts.extend(f"- {item}" for item in plan.asset_constraints[-8:])
        if plan.placement_constraints:
            parts.append("## 摆放/地形约束")
            parts.extend(f"- {item}" for item in plan.placement_constraints[-8:])
        style_prompt = str(contract.get("asset_style_prompt") or "").strip()
        if style_prompt:
            parts.append("## 长周期场景风格合同")
            parts.append(style_prompt)
        terrain_spec = contract.get("terrain_spec")
        boundary_spec = contract.get("boundary_spec")
        if terrain_spec or boundary_spec:
            parts.append("## 地形与边界约束")
            if terrain_spec:
                parts.append(f"terrain_spec={terrain_spec}")
            if boundary_spec:
                parts.append(f"boundary_spec={boundary_spec}")
        return "\n".join(parts).strip() or plan.intent_summary

    def _update_scene_contract_for_intervention(
        self,
        plan: SeedPlan,
        request: InterventionRequest,
        route: str,
    ) -> None:
        contract = self._scene_contracts_by_plan.get(plan.plan_id)
        if contract is None:
            return
        update_contract_from_intervention(
            contract,
            text=request.content,
            accepted=True,
            deferred=route in {"next_batch", "post_generation_add"},
            rejected=False,
            actor_id=request.actor_id,
            batch_id=request.batch_id,
            updated_by=request.source_user_id,
        )
        terrain_profile = self._terrain_resolver.derive(
            plan.intent_summary + " " + request.content,
            scene_type=plan.scene_type,
        )
        if terrain_profile.scene_key != (plan.scene_type or "mixed"):
            contract.terrain_spec = terrain_profile.terrain_spec
            contract.boundary_spec = terrain_profile.boundary_spec

    def _looks_ready_to_propose(self, text: str) -> bool:
        decision = get_intent_understanding_service().classify(text, allow_llm=False)
        return decision.intent == "generation_start" or any(
            word in text
            for word in ("整理方案", "形成方案", "确认方案", "开始执行", "开始生成", "按这个方案")
        )

    def _default_proposal(self, plan: SeedPlan) -> str:
        return (
            f"【SeedPlan 草案 {plan.plan_id}】\n"
            f"场景类型：{plan.scene_type}\n"
            f"意图摘要：{plan.intent_summary or '待补充'}\n"
            "请房主确认后再进入批次生成。"
        )

    def _default_conflict_recommendation(self, plan: SeedPlan) -> str:
        conflict_text = "；".join(plan.conflicts[-3:]) if plan.conflicts else "无显式冲突"
        return f"建议房主确认折中方案：保留已确认主题，冲突项作为约束处理：{conflict_text}"

    @staticmethod
    def _has_unresolved_conflicts(plan: SeedPlan) -> bool:
        if not plan.conflicts:
            return False
        resolved_items: set[str] = set()
        for item in [
            item
            for item in plan.review_policy.get("conflict_resolutions") or []
            if isinstance(item, dict) and item.get("status") == "confirmed"
        ]:
            resolved_items.update(str(conflict) for conflict in item.get("conflict_items") or [])
        return any(conflict not in resolved_items for conflict in plan.conflicts)

    @staticmethod
    def _pending_clarifications(plan: SeedPlan) -> list[dict[str, Any]]:
        return [
            dict(item)
            for item in plan.review_policy.get("clarification_requests") or []
            if isinstance(item, dict) and str(item.get("status") or "pending") == "pending"
        ]

    def _record_clarification_answer(self, message: ChatMessage, plan: SeedPlan) -> None:
        requests = [
            dict(item)
            for item in plan.review_policy.get("clarification_requests") or []
            if isinstance(item, dict)
        ]
        if not requests:
            return
        answered_index = -1
        for index in range(len(requests) - 1, -1, -1):
            item = requests[index]
            if str(item.get("status") or "pending") != "pending":
                continue
            target_user_id = str(item.get("target_user_id") or "")
            if target_user_id and target_user_id != message.sender_id:
                continue
            answered_index = index
            break
        if answered_index < 0:
            return
        requests[answered_index].update({
            "status": "answered",
            "answered_by": message.sender_id,
            "answer_text": message.text,
            "answered_at": time.time(),
        })
        answers = list(plan.review_policy.get("clarification_answers") or [])
        answers.append({
            "clarification_id": requests[answered_index].get("clarification_id", ""),
            "answered_by": message.sender_id,
            "answer_text": message.text,
            "answered_at": requests[answered_index]["answered_at"],
        })
        plan.review_policy["clarification_requests"] = requests
        plan.review_policy["clarification_answers"] = answers
        if not self._pending_clarifications(plan):
            plan.status = SeedPlanStatus.DRAFT
        plan.updated_at = time.time()
        self._record_memory(
            room_id=message.room_id,
            plan_id=plan.plan_id,
            entry_type="clarification_answered",
            text=message.text,
            actor_id=message.sender_id,
            visibility="shared",
            metadata={
                "clarification_id": requests[answered_index].get("clarification_id", ""),
                "sender_name": message.sender_name,
            },
        )

    def _record(self, event_type: str, message: str, payload: dict[str, Any] | None = None) -> CoordinatorEvent:
        event = CoordinatorEvent(event_type, message, payload or {})
        self._events.append(event)
        if len(self._events) > MAX_COORDINATOR_EVENTS:
            del self._events[:len(self._events) - MAX_COORDINATOR_EVENTS]
        return event

    def _record_disclosures(
        self,
        *,
        room_id: str,
        stage: str,
        progress: int,
        plan: dict[str, Any],
        intervention: dict[str, Any] | None = None,
        review: dict[str, Any] | None = None,
    ) -> None:
        self._disclosure_events.extend(self._disclosure_policy.disclose_all(
            room_id=room_id,
            stage=stage,
            progress=progress,
            plan=plan,
            intervention=intervention,
            review=review,
        ))
        if len(self._disclosure_events) > MAX_COORDINATOR_DISCLOSURE_EVENTS:
            removed = len(self._disclosure_events) - MAX_COORDINATOR_DISCLOSURE_EVENTS
            del self._disclosure_events[:removed]
            self._disclosure_events_start_index += removed

    def _record_memory(
        self,
        *,
        room_id: str,
        entry_type: str,
        text: str,
        plan_id: str = "",
        agent_id: str = "",
        batch_id: str = "",
        actor_id: str = "",
        visibility: str = "private",
        metadata: dict[str, Any] | None = None,
    ) -> None:
        self._memory_store.record(
            scope=MemoryScope(
                room_id=room_id or "default",
                plan_id=plan_id,
                agent_id=agent_id,
                batch_id=batch_id,
            ),
            entry_type=entry_type,
            text=text,
            actor_id=actor_id,
            visibility=visibility,
            metadata=metadata,
        )

    def _require_plan(self, plan_id: str) -> SeedPlan:
        plan = self._plans.get(plan_id)
        if plan is None:
            raise KeyError(f"SeedPlan not found: {plan_id}")
        return plan

    def _coerce_message(self, message: ChatMessage | dict[str, Any]) -> ChatMessage:
        if isinstance(message, ChatMessage):
            return message
        metadata = message.get("metadata") if isinstance(message.get("metadata"), dict) else {}
        return ChatMessage(
            room_id=str(message.get("room_id") or "default"),
            text=str(message.get("text") or ""),
            sender_id=str(message.get("sender_id") or ""),
            sender_name=str(message.get("sender_name") or ""),
            is_host=bool(message.get("is_host") or metadata.get("is_host")),
            agent_id=str(message.get("agent_id") or metadata.get("target_agent_id") or ""),
            agent_name=str(message.get("agent_name") or metadata.get("target_agent_name") or ""),
            metadata=dict(metadata),
        )

    def _coerce_intervention(self, intervention: InterventionRequest | dict[str, Any]) -> InterventionRequest:
        if isinstance(intervention, InterventionRequest):
            return intervention
        return InterventionRequest(
            room_id=str(intervention.get("room_id") or ""),
            plan_id=str(intervention.get("plan_id") or ""),
            session_id=str(intervention.get("session_id") or ""),
            batch_id=str(intervention.get("batch_id") or ""),
            actor_id=str(intervention.get("actor_id") or ""),
            actor_version=int(intervention.get("actor_version") or 0),
            target_hint=str(intervention.get("target_hint") or intervention.get("target") or ""),
            finding_details=[
                dict(item) for item in (intervention.get("finding_details") or [])
                if isinstance(item, dict)
            ],
            source_user_id=str(intervention.get("source_user_id") or ""),
            intent_type=str(intervention.get("intent_type") or "modify"),
            content=str(intervention.get("content") or ""),
            priority=int(intervention.get("priority") or 0),
            apply_policy=str(intervention.get("apply_policy") or "next_batch"),
            intervention_id=str(intervention.get("intervention_id") or f"iv-{uuid.uuid4().hex[:12]}"),
        )

    def _coerce_batch_event(self, event: BatchEvent | dict[str, Any]) -> BatchEvent:
        if isinstance(event, BatchEvent):
            return event
        metadata = event.get("metadata") if isinstance(event.get("metadata"), dict) else {}
        return BatchEvent(
            room_id=str(event.get("room_id") or "default"),
            plan_id=str(event.get("plan_id") or ""),
            session_id=str(event.get("session_id") or ""),
            batch_id=str(event.get("batch_id") or ""),
            stage=str(event.get("stage") or "batch"),
            status=str(event.get("status") or "running"),
            progress=int(event.get("progress") or 0),
            message=str(event.get("message") or ""),
            intervention_window_open=bool(event.get("intervention_window_open")),
            metadata=dict(metadata),
        )

    def _coerce_review_result(self, result: ReviewResult | dict[str, Any]) -> ReviewResult:
        if isinstance(result, ReviewResult):
            return result
        metadata = result.get("metadata") if isinstance(result.get("metadata"), dict) else {}
        return ReviewResult(
            room_id=str(result.get("room_id") or "default"),
            plan_id=str(result.get("plan_id") or ""),
            session_id=str(result.get("session_id") or ""),
            batch_id=str(result.get("batch_id") or ""),
            actor_id=str(result.get("actor_id") or ""),
            actor_version=int(result.get("actor_version") or 0),
            review_type=str(result.get("review_type") or "geometry"),
            passed=_coerce_protocol_bool(result.get("passed")),
            findings=[str(item) for item in result.get("findings") or []],
            finding_details=[
                dict(item)
                for item in (result.get("finding_details") or [])
                if isinstance(item, dict)
            ],
            severity=str(result.get("severity") or "warn"),
            metadata=dict(metadata),
        )
