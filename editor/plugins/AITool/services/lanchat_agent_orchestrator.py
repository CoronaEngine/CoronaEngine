from __future__ import annotations

import logging
import json
import re
import time
from dataclasses import dataclass
from typing import Any, Callable

from .lanchat_summary_service import DiscussionState, LANChatSummaryService


@dataclass
class AgentOrchestrationResult:
    """Result sent back through C++ LANChat."""

    text: str
    sender_id: str
    sender_name: str
    discussion_state: DiscussionState
    proposal: bool = False
    action_payload: dict[str, Any] | None = None


class LanChatAgentOrchestrator:
    """Python AI/GM layer for C++ LANChat agent triggers.

    C++ owns room state and reliable transport. This class only performs
    semantic orchestration and returns a reply payload for the worker to send
    through C++.
    """

    _GM_NAMES = {"gm", "主持人", "裁判", "gm agent", "game master"}
    _CONFLICT_WORDS = ("冲突", "同时", "覆盖", "不同意", "反对", "抢", "都要")
    _CONFIRM_WORDS = ("确认", "同意", "按方案", "执行", "可以")
    _REJECT_WORDS = ("拒绝", "取消", "不要")
    _MAJOR_ACTION_WORDS = ("删除", "重置", "清空", "整体", "大件", "核心家具", "覆盖")
    _EXECUTION_WORDS = ("开始执行", "开始生成", "按方案", "执行", "生成", "应用方案")
    _DISCUSSION_WORDS = (
        "讨论", "介绍", "建议", "idea", "想法", "你是谁", "你会干什么",
        "其他人都可以提出", "大家都可以提出",
    )
    _PLAN_WORDS = ("方案", "建议", "设计", "布局", "清单", "步骤", "动线", "区域", "主题")
    _PLAN_OPINION_WORDS = (
        "怎么看", "怎么评价", "如何评价", "看法", "观点",
        "觉得", "认为", "是否合理", "合不合理", "评价一下",
    )
    _PLAN_REFERENCE_WORDS = (
        "按你的方案", "按照你的方案", "按你说的", "按刚才那个方案",
        "按上面的方案", "就这样生成", "执行你的方案", "开始执行你的方案",
        "就按照你的方案", "照你的方案", "按这个方案",
        "按照你说的这个方案", "按照你说的方案", "就按照你说的",
        "按照刚才的方案", "按刚才商人的方案", "按这个方案生成",
        "按照这个方案进行场景建筑生成", "按照这个方案进行场景生成",
    )
    _STALE_PLAN_WORDS = ("换方案", "不要刚才", "不要这个方案", "重新讨论", "重来", "作废")
    _PROPOSAL_ID_PATTERN = re.compile(r"\b(?:gm-\d+|fa-[\w.-]+|cr-[\w.-]+)\b", re.I)

    def __init__(
        self,
        agent_factory: Callable[[], Any] | None = None,
        summary_service: LANChatSummaryService | None = None,
        system_sender_id: str = "gm-system",
        system_sender_name: str = "GM",
    ) -> None:
        self._agent_factory = agent_factory or self._default_agent_factory
        self._summary_service = summary_service or LANChatSummaryService()
        self._system_sender_id = system_sender_id
        self._system_sender_name = system_sender_name
        self._agent: Any = None
        self._pending_proposal: dict[str, Any] | None = None
        self._proposals: dict[str, dict[str, Any]] = {}
        self._trigger_to_proposal: dict[str, str] = {}
        self._last_confirmed_action: dict[str, Any] | None = None
        self._processed_proposals: dict[str, str] = {}
        self._latest_agent_plans: dict[str, dict[str, Any]] = {}
        self._session_mode = "DISCUSSING"
        self._logger = logging.getLogger(__name__)

    @property
    def summary_state(self) -> DiscussionState:
        return self._summary_service.state

    def handle_trigger(self, trigger: dict[str, Any]) -> AgentOrchestrationResult:
        history = self._history_from_trigger(trigger)
        self._remember_plans_from_history(history, trigger)
        state = self._summary_service.monitor(history)
        text = str(trigger.get("text") or "")

        if self._marks_plan_stale(text):
            self._mark_agent_plan_stale(trigger)

        plan_resolution = self._resolve_plan_reference(trigger)
        if plan_resolution is False:
            return AgentOrchestrationResult(
                text=self._missing_plan_text(trigger),
                sender_id=self._system_sender_id,
                sender_name=self._system_sender_name,
                discussion_state=state,
                proposal=False,
            )
        if isinstance(plan_resolution, dict):
            trigger = self._with_resolved_plan(trigger, plan_resolution)
            text = str(trigger.get("text") or "")
        else:
            opinion_plan = self._resolve_opinion_plan_context(trigger)
            if isinstance(opinion_plan, dict):
                trigger = self._with_opinion_plan_context(trigger, opinion_plan)

        if self._is_gm_trigger(trigger) and self._is_control_without_proposal_id(text):
            control = self._gm_control_response(trigger, state, history)
            if control is not None:
                return AgentOrchestrationResult(
                    text=control,
                    sender_id=self._system_sender_id,
                    sender_name=self._system_sender_name,
                    discussion_state=state,
                    proposal=False,
                )

        confirmation = self._consume_confirmation(text, trigger)
        if confirmation is not None:
            return AgentOrchestrationResult(
                text=confirmation,
                sender_id=self._system_sender_id,
                sender_name=self._system_sender_name,
                discussion_state=state,
                proposal=False,
                action_payload=self._last_confirmed_action,
            )

        if self._is_gm_trigger(trigger):
            control = self._gm_control_response(trigger, state, history)
            if control is not None:
                return AgentOrchestrationResult(
                    text=control,
                    sender_id=self._system_sender_id,
                    sender_name=self._system_sender_name,
                    discussion_state=state,
                    proposal=False,
                )

        if self._needs_gm_proposal(trigger, state):
            proposal_text = self._build_gm_proposal(trigger, state)
            return AgentOrchestrationResult(
                text=proposal_text,
                sender_id=self._system_sender_id,
                sender_name=self._system_sender_name,
                discussion_state=state,
                proposal=True,
                action_payload=dict(self._pending_proposal or {}),
            )

        role_reply = self._run_role_agent(trigger, state)
        self._record_agent_plan_from_reply(trigger, role_reply)
        return AgentOrchestrationResult(
            text=role_reply,
            sender_id=str(trigger.get("agent_id") or "agent"),
            sender_name=str(trigger.get("agent_name") or "Agent"),
            discussion_state=state,
            proposal=False,
        )

    def _needs_gm_proposal(self, trigger: dict[str, Any], state: DiscussionState) -> bool:
        if isinstance(trigger.get("_resolved_plan"), dict):
            return True
        text = str(trigger.get("text") or "")
        if not self._is_gm_trigger(trigger) and self._is_discussion_intent(text):
            return False
        if self._is_gm_trigger(trigger):
            return (
                state.conflicts
                or any(word in text for word in self._CONFLICT_WORDS)
                or any(word in text for word in self._MAJOR_ACTION_WORDS)
                or any(word in text for word in self._EXECUTION_WORDS)
            )
        if state.conflicts:
            return True
        if any(word in text for word in self._CONFLICT_WORDS):
            return True
        # 单人/无冲突的明确增删改移应回到普通 agentic 工具通道，避免
        # GM 只发 proposal 但没有执行队列时吞掉用户操作。多人重大操作仍需 GM。
        if any(word in text for word in self._MAJOR_ACTION_WORDS) and self._is_multi_user(trigger):
            return True
        if any(word in text for word in self._EXECUTION_WORDS) and self._is_multi_user(trigger):
            return True
        return False

    def _is_discussion_intent(self, text: str) -> bool:
        return any(word in str(text or "") for word in self._DISCUSSION_WORDS)

    def _gm_control_response(
        self,
        trigger: dict[str, Any],
        state: DiscussionState,
        history: list[dict[str, Any]] | None = None,
    ) -> str | None:
        text = str(trigger.get("text") or "").strip()
        if any(word in text for word in ("暂停", "先停", "等一下")):
            self._session_mode = "PAUSED"
            self._set_runtime_mode("PAUSED")
            return "【GM】已进入暂停状态。我会在当前批次边界停止继续推进；你们可以继续补充调整。"
        if any(word in text for word in ("继续", "恢复")):
            self._session_mode = "PLANNING"
            self._set_runtime_mode("EXECUTING")
            return "【GM】已恢复到规划状态。需要执行时请由房主确认具体方案。"
        if any(word in text for word in ("先讨论", "不要生成", "别生成", "先规划")):
            self._session_mode = "DISCUSSING"
            self._set_runtime_mode("DISCUSSING")
            return "【GM】已切到讨论模式。当前只整理方案和约束，不会写入场景。"
        if any(word in text for word in ("整理", "总结", "大家的想法", "当前想法")):
            gm_state = self._summary_service.monitor_for_gm_summary(history or self._history_from_trigger(trigger))
            return self._build_gm_summary(gm_state)
        return None

    def _set_runtime_mode(self, mode: str) -> None:
        try:
            from .lanchat_scene_runtime import get_lanchat_scene_runtime
            get_lanchat_scene_runtime().set_mode(mode)
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("LANChat scene runtime mode update skipped: %s", exc)

    def _is_control_without_proposal_id(self, text: str) -> bool:
        if self._PROPOSAL_ID_PATTERN.search(str(text or "")):
            return False
        return any(word in text for word in (
            "暂停", "先停", "等一下", "继续", "恢复",
            "先讨论", "不要生成", "别生成", "先规划",
            "整理", "总结", "大家的想法", "当前想法",
        ))

    def _build_gm_summary(self, state: DiscussionState) -> str:
        lines = ["【GM 总结】"]
        if state.summary:
            lines.append(f"当前讨论：{state.summary}")
        if state.pending_intents:
            lines.append(f"待整理意图：{self._join_lines(state.pending_intents)}")
        if state.conflicts:
            lines.append(f"潜在冲突：{self._join_lines(state.conflicts)}")
        if not state.summary and not state.pending_intents and not state.conflicts:
            lines.append("目前还没有足够明确的多人共识。可以继续讨论，或由房主指定一个方向。")
        lines.append("如需执行，请让房主明确确认方案。")
        return "\n".join(lines)

    def _build_gm_proposal(self, trigger: dict[str, Any], state: DiscussionState) -> str:
        requester = str(trigger.get("sender_name") or trigger.get("sender_id") or "用户")
        text = str(trigger.get("text") or "").strip()
        dedupe_key = self._proposal_dedupe_key(trigger)
        existing_id = self._trigger_to_proposal.get(dedupe_key)
        if existing_id and existing_id in self._proposals:
            existing = self._proposals[existing_id]
            self._pending_proposal = existing if existing.get("status") == "pending" else self._pending_proposal
            return self._format_gm_proposal(existing)

        proposal_id = f"gm-{int(time.time() * 1000)}"
        resolved_plan = trigger.get("_resolved_plan")
        if isinstance(resolved_plan, dict):
            pending = [str(resolved_plan.get("resolved_intent_text") or text)]
        else:
            pending = state.pending_intents or [f"{requester}: {text}"]
        conflicts = state.conflicts or self._infer_pair_conflicts(self._history_from_trigger(trigger))
        if not conflicts:
            conflicts = ["暂无明确对象冲突，但该操作可能影响多人共识或核心布局。"]

        action_type = self._infer_action_type(trigger, state)
        self._pending_proposal = {
            "proposal_id": proposal_id,
            "correlation_id": proposal_id,
            "status": "pending",
            "action_type": action_type,
            "source_user_id": str(trigger.get("sender_id") or ""),
            "target_agent_id": str(trigger.get("agent_id") or ""),
            "requester": requester,
            "intent_text": text,
            "pending": pending,
            "conflicts": conflicts,
            "requires_host_confirm": True,
            "execution": "host_single_writer",
        }
        if action_type == "start_generation":
            self._pending_proposal["plan_summary"] = self._build_generation_plan_summary(text, pending)
        if isinstance(resolved_plan, dict):
            resolved_intent_text = str(resolved_plan.get("resolved_intent_text") or text)
            original_user_text = text
            text = resolved_intent_text
            self._pending_proposal.update({
                "resolved_from_plan_id": resolved_plan.get("plan_id"),
                "source_agent_id": resolved_plan.get("agent_id"),
                "source_agent_name": resolved_plan.get("agent_name"),
                "resolved_intent_text": resolved_intent_text,
                "original_user_text": original_user_text,
                "intent_text": resolved_intent_text,
                "plan_summary": resolved_plan.get("compact_summary"),
            })
        self._proposals[proposal_id.lower()] = self._pending_proposal
        self._trigger_to_proposal[dedupe_key] = proposal_id.lower()

        return self._format_gm_proposal(self._pending_proposal)

    def _format_gm_proposal(self, proposal: dict[str, Any]) -> str:
        proposal_id = str(proposal.get("proposal_id") or "")
        requester = str(proposal.get("requester") or "用户")
        text = str(proposal.get("intent_text") or "")
        pending = proposal.get("pending") or []
        conflicts = proposal.get("conflicts") or []
        plan_summary = str(proposal.get("plan_summary") or "").strip()
        action_type = str(proposal.get("action_type") or "")
        if plan_summary:
            confirmation_line = self._proposal_confirmation_line(proposal_id, action_type)
            return (
                f"【GM 提案 {proposal_id}】\n"
                f"方案摘要：\n{plan_summary}\n"
                f"潜在冲突：{self._join_lines(conflicts)}\n"
                f"确认后动作：{self._proposal_confirmation_effect(action_type)}\n"
                f"房主可回复：{confirmation_line}"
            )
        return (
            f"【GM 提案 {proposal_id}】\n"
            f"我理解当前请求来自 {requester}：{text}\n"
            f"待处理意图：{self._join_lines(pending)}\n"
            f"潜在冲突：{self._join_lines(conflicts)}\n"
            "建议：先保留最近用户明确操作，Agent 物体让位；涉及删除、重置或覆盖多人意见时由房主确认。\n"
            f"确认后动作：{self._proposal_confirmation_effect(action_type)}\n"
            f"房主可回复：{self._proposal_confirmation_line(proposal_id, action_type)}"
        )

    def _build_generation_plan_summary(self, text: str, pending: list[str]) -> str:
        cleaned_text = self._strip_leading_mention(text)
        basis = self._join_lines(pending) if pending else cleaned_text
        lines = [
            f"- 设计目标：{cleaned_text or basis or '按当前讨论生成场景'}",
            "- 生成范围：完整 3D 场景，包括空间布局、主体家具和基础装饰。",
            "- 执行方式：确认后进入生成队列；未确认前只作为方案草案保留。",
        ]
        return "\n".join(lines)

    @staticmethod
    def _strip_leading_mention(text: str) -> str:
        return re.sub(r"^\s*@\S+\s*", "", str(text or "").strip()).strip()

    @staticmethod
    def _proposal_confirmation_effect(action_type: str) -> str:
        if action_type == "start_generation":
            return "开始生成 3D 场景"
        if action_type == "discussion_only":
            return "确认讨论方案，但不会自动生成"
        return "提交给房主单写入执行队列"

    @staticmethod
    def _proposal_confirmation_line(proposal_id: str, action_type: str) -> str:
        if action_type == "start_generation":
            return f"@GM 确认 {proposal_id}（确认该方案并开始生成） / @GM 拒绝 {proposal_id}（继续讨论，不生成）。"
        return f"@GM 确认 {proposal_id} / @GM 拒绝 {proposal_id}。"

    def _proposal_dedupe_key(self, trigger: dict[str, Any]) -> str:
        message_id = str(trigger.get("message_id") or trigger.get("correlation_id") or "")
        sender_id = str(trigger.get("sender_id") or trigger.get("sender_name") or "")
        text = str(trigger.get("text") or "").strip()
        return f"{sender_id}|{message_id}|{text}"

    def _infer_action_type(self, trigger: dict[str, Any], state: DiscussionState) -> str:
        if isinstance(trigger.get("_resolved_plan"), dict):
            return "start_generation"
        text = str(trigger.get("text") or "")
        if any(word in text for word in self._CONFLICT_WORDS) or state.conflicts:
            return "conflict_resolution"
        if any(word in text for word in ("删除", "移除", "清空")):
            return "actor_delete"
        if any(word in text for word in ("移动", "旋转", "缩放", "放大", "缩小", "调整")):
            return "actor_transform"
        if any(word in text for word in ("添加", "新增", "放入")):
            return "actor_add"
        if any(word in text for word in ("开始生成", "生成", "开始执行", "执行", "应用方案")):
            return "start_generation"
        return "discussion_only"

    def _consume_confirmation(self, text: str, trigger: dict[str, Any] | None = None) -> str | None:
        self._last_confirmed_action = None
        trigger = trigger or {}
        metadata = self._metadata_from_trigger(trigger)
        kind = str(trigger.get("message_kind") or "").strip().lower()
        decision = str(metadata.get("decision") or "").strip().lower()
        is_structured_confirmation = kind == "confirmation"
        is_confirm = (
            decision in {"confirm", "confirmed", "yes", "accept"}
            or any(word in text for word in self._CONFIRM_WORDS)
        )
        is_reject = (
            decision in {"reject", "rejected", "no", "cancel"}
            or any(word in text for word in self._REJECT_WORDS)
        )
        if is_structured_confirmation and not is_confirm and not is_reject:
            return "【GM】结构化确认缺少 decision=confirm|reject，未进入执行队列。"
        if not is_confirm and not is_reject:
            return None
        mentioned_ids = {match.group(0).lower() for match in self._PROPOSAL_ID_PATTERN.finditer(text)}
        correlation_id = str(trigger.get("correlation_id") or metadata.get("proposal_id") or "").strip().lower()
        if correlation_id:
            mentioned_ids.add(correlation_id)
        external_confirmation = self._consume_external_confirmation(
            mentioned_ids,
            is_confirm=is_confirm,
            is_reject=is_reject,
            is_structured_confirmation=is_structured_confirmation,
            trigger=trigger,
        )
        if external_confirmation is not None:
            return external_confirmation
        processed_matches = [item for item in sorted(mentioned_ids) if item in self._processed_proposals]
        if processed_matches:
            replay_id = processed_matches[0]
            status = self._processed_proposals[replay_id]
            return f"【GM】提案 {replay_id} 已处理（{status}），不会重复入队。"
        proposal_status_matches = [
            (item, self._proposals[item].get("status"))
            for item in sorted(mentioned_ids)
            if item in self._proposals and self._proposals[item].get("status") != "pending"
        ]
        if proposal_status_matches:
            replay_id, status = proposal_status_matches[0]
            self._processed_proposals[replay_id] = str(status or "processed")
            return f"【GM】提案 {replay_id} 已处理（{status}），不会重复入队。"
        proposal = self._find_confirmation_proposal(mentioned_ids)
        if proposal is None:
            return None
        pid = str(proposal.get("proposal_id") or "")
        if mentioned_ids and pid.lower() not in mentioned_ids:
            for item in mentioned_ids:
                self._processed_proposals.setdefault(item, "mismatched")
            current = self._current_pending_proposal_id() or pid
            return f"【GM】确认编号不匹配，当前待确认提案是 {current}。请回复：@GM 确认 {current} 或 @GM 拒绝 {current}。"
        host_check = self._trusted_host_confirmation({**metadata, **trigger})
        if host_check is False:
            return f"【GM】只有房主可以确认 {pid}；该请求没有进入 host 执行队列。"
        if is_confirm:
            self._last_confirmed_action = dict(proposal)
            self._last_confirmed_action["status"] = "confirmed"
            if not mentioned_ids:
                self._last_confirmed_action["confirmation_mode"] = "bare_text_fallback"
            elif is_structured_confirmation:
                self._last_confirmed_action["confirmation_mode"] = "structured_confirmation"
            elif host_check is None:
                self._last_confirmed_action["confirmation_mode"] = "proposal_id_without_verified_host"
            else:
                self._last_confirmed_action["confirmation_mode"] = "verified_host"
            self._last_confirmed_action["requires_host_confirm"] = False
            proposal["status"] = "confirmed"
            if self._pending_proposal is proposal:
                self._pending_proposal = self._latest_pending_proposal()
            self._processed_proposals[pid.lower()] = "confirmed"
            return f"【GM】已确认 {pid}。"
        if is_reject:
            self._last_confirmed_action = dict(proposal)
            self._last_confirmed_action["status"] = "rejected"
            proposal["status"] = "rejected"
            if self._pending_proposal is proposal:
                self._pending_proposal = self._latest_pending_proposal()
            self._processed_proposals[pid.lower()] = "rejected"
            return f"【GM】已取消 {pid}，不会执行该提案。"
        return None

    def _consume_external_confirmation(
        self,
        mentioned_ids: set[str],
        *,
        is_confirm: bool,
        is_reject: bool,
        is_structured_confirmation: bool,
        trigger: dict[str, Any] | None = None,
    ) -> str | None:
        """Acknowledge Coordinator-owned proposal ids without host-executing them."""
        if not is_structured_confirmation:
            return None
        external_ids = [
            item for item in sorted(mentioned_ids)
            if item.startswith("fa-") or item.startswith("cr-")
        ]
        if not external_ids:
            return None
        proposal_id = external_ids[0]
        is_conflict_resolution = proposal_id.startswith("cr-")
        label = "冲突决议候选" if is_conflict_resolution else "最终调整提案"
        action_type = (
            "conflict_resolution_confirmation"
            if is_conflict_resolution
            else "final_adjustment_confirmation"
        )
        trigger = trigger or {}
        metadata = self._metadata_from_trigger(trigger)
        source_user_id = str(
            trigger.get("source_user_id")
            or trigger.get("sender_id")
            or metadata.get("source_user_id")
            or metadata.get("sender_id")
            or metadata.get("confirmed_by")
            or ""
        ).strip()
        host_check = self._trusted_host_confirmation({**metadata, **trigger})
        if host_check is False:
            return f"【GM】只有房主可以处理{label} {proposal_id}；该请求没有进入确认流程。"
        if not source_user_id:
            return f"【GM】缺少房主确认身份，不能处理{label} {proposal_id}。"
        if proposal_id in self._processed_proposals:
            status = self._processed_proposals[proposal_id]
            return f"【GM】{label} {proposal_id} 已处理（{status}），不会重复确认。"
        if is_confirm:
            self._processed_proposals[proposal_id] = "confirmed"
            self._last_confirmed_action = {
                "action_type": action_type,
                "execution": "coordinator_only",
                "proposal_id": proposal_id,
                "decision": "confirm",
                "status": "confirmed",
                "requires_host_confirm": False,
                "source_user_id": source_user_id,
            }
            if is_conflict_resolution:
                return f"【GM】已确认冲突决议候选 {proposal_id}，将交由方案确认流程处理。"
            return f"【GM】已确认最终调整提案 {proposal_id}，将交由最终调整流程处理。"
        if is_reject:
            self._processed_proposals[proposal_id] = "rejected"
            self._last_confirmed_action = {
                "action_type": action_type,
                "execution": "coordinator_only",
                "proposal_id": proposal_id,
                "decision": "reject",
                "status": "rejected",
                "requires_host_confirm": False,
                "source_user_id": source_user_id,
            }
            if is_conflict_resolution:
                return f"【GM】已拒绝冲突决议候选 {proposal_id}，方案仍需继续讨论或重新提案。"
            return f"【GM】已拒绝最终调整提案 {proposal_id}，不会自动应用该冲突调整。"
        return None

    def _find_confirmation_proposal(self, mentioned_ids: set[str]) -> dict[str, Any] | None:
        if mentioned_ids:
            for item in mentioned_ids:
                proposal = self._proposals.get(item.lower())
                if proposal is not None:
                    return proposal
            return self._pending_proposal
        return self._pending_proposal

    def _latest_pending_proposal(self) -> dict[str, Any] | None:
        for proposal in reversed(list(self._proposals.values())):
            if proposal.get("status") == "pending":
                return proposal
        return None

    def _current_pending_proposal_id(self) -> str:
        proposal = self._pending_proposal or self._latest_pending_proposal()
        return str((proposal or {}).get("proposal_id") or "")

    @staticmethod
    def _metadata_from_trigger(trigger: dict[str, Any]) -> dict[str, Any]:
        metadata = trigger.get("metadata")
        if isinstance(metadata, dict):
            return metadata
        raw = trigger.get("metadata_json")
        if not raw:
            return {}
        if isinstance(raw, dict):
            return raw
        try:
            parsed = json.loads(str(raw))
        except Exception:
            return {}
        return parsed if isinstance(parsed, dict) else {}

    def _run_role_agent(self, trigger: dict[str, Any], state: DiscussionState) -> str:
        try:
            agent = self._get_agent()
            persona = str(trigger.get("persona") or "")
            messages = self._messages_from_trigger(trigger)
            agent_name = str(trigger.get("agent_name") or "Agent")
            latest = str(trigger.get("text") or "")
            messages = [
                "【当前点名上下文】\n"
                "【链路上下文】"
                f"room_id={trigger.get('room_id') or ''} "
                f"agent_id={trigger.get('agent_id') or ''} "
                f"agent_name={trigger.get('agent_name') or ''}\n"
                f"本轮明确被 @ 的 AI 助手是：{agent_name}。\n"
                f"最新用户消息是发给你的：{latest}\n"
                "请以该助手身份回应，不要因为历史中出现其他 @对象 而拒绝执行或越位判断。"
            ] + messages
            referenced_plan = trigger.get("_referenced_plan_context")
            if isinstance(referenced_plan, dict):
                messages = [self._format_referenced_plan_context(referenced_plan)] + messages
            context = state.to_prompt_context()
            if context:
                messages = [f"【静默监听摘要】\n{context}"] + messages
            return str(agent(persona, messages))
        except Exception as exc:  # noqa: BLE001
            self._logger.warning("LANChat role agent failed", exc_info=True)
            return self._safe_agent_error_text(exc)

    def _remember_plans_from_history(
        self,
        history: list[dict[str, Any]],
        trigger: dict[str, Any] | None = None,
    ) -> None:
        trigger = trigger or {}
        current_agent_id = str(trigger.get("agent_id") or "").strip().lower()
        current_agent_name = str(trigger.get("agent_name") or "").strip().lower()
        for item in history[-24:]:
            kind = str(item.get("message_kind") or "").strip().lower()
            sender_type = str(item.get("sender_type") or "").strip().lower()
            text = str(item.get("text") or "").strip()
            sender_id = str(item.get("sender_id") or item.get("agent_id") or "").strip()
            agent_name = str(item.get("from") or item.get("sender_name") or sender_id or "Agent").strip()
            sender_id_key = sender_id.lower()
            agent_name_key = agent_name.lower()
            looks_like_current_agent = bool(
                current_agent_id
                and sender_id_key == current_agent_id
                or current_agent_name
                and agent_name_key == current_agent_name
            )
            if kind != "agent_reply" and sender_type != "agent" and not looks_like_current_agent:
                continue
            if not self._looks_like_plan(text):
                continue
            self._record_agent_plan(
                agent_id=sender_id,
                agent_name=agent_name,
                raw_text=text,
                source_message_id=str(item.get("message_id") or ""),
                source_user_id=str(item.get("source_user_id") or ""),
            )

    def _record_agent_plan_from_reply(self, trigger: dict[str, Any], reply: str) -> None:
        if not self._looks_like_plan(reply):
            return
        self._record_agent_plan(
            agent_id=str(trigger.get("agent_id") or ""),
            agent_name=str(trigger.get("agent_name") or "Agent"),
            raw_text=reply,
            source_message_id=str(trigger.get("message_id") or ""),
            source_user_id=str(trigger.get("sender_id") or ""),
        )

    def _record_agent_plan(
        self,
        *,
        agent_id: str,
        agent_name: str,
        raw_text: str,
        source_message_id: str,
        source_user_id: str,
    ) -> None:
        agent_id = str(agent_id or "").strip()
        agent_name = str(agent_name or "Agent").strip()
        raw_text = str(raw_text or "").strip()
        if not raw_text:
            return
        plan = {
            "plan_id": f"plan-{agent_id or agent_name}-{abs(hash(raw_text)) % 100000000}",
            "agent_id": agent_id,
            "agent_name": agent_name,
            "source_message_id": source_message_id,
            "source_user_id": source_user_id,
            "raw_text": raw_text,
            "compact_summary": self._compact_plan(raw_text),
            "status": "active",
            "created_at": time.time(),
        }
        for key in self._agent_plan_keys(agent_id, agent_name):
            self._latest_agent_plans[key] = plan

    def _resolve_plan_reference(self, trigger: dict[str, Any]) -> dict[str, Any] | bool | None:
        text = str(trigger.get("text") or "")
        if self._is_gm_trigger(trigger) or not self._is_plan_reference(text):
            return None
        agent_id = str(trigger.get("agent_id") or "")
        agent_name = str(trigger.get("agent_name") or "")
        plan = self._find_agent_plan(agent_id, agent_name)
        if not plan:
            return False
        resolved = dict(plan)
        resolved["resolved_intent_text"] = (
            f"原始用户请求：{text}\n"
            f"用户确认执行 @{agent_name or agent_id} 最近方案。请严格围绕下列方案生成开放场景，不要退化成通用现代建筑：\n"
            f"{plan.get('raw_text') or plan.get('compact_summary') or ''}"
        )
        return resolved

    def _resolve_opinion_plan_context(self, trigger: dict[str, Any]) -> dict[str, Any] | None:
        text = str(trigger.get("text") or "")
        if self._is_gm_trigger(trigger) or not self._is_plan_opinion_reference(text):
            return None
        current_agent_id = str(trigger.get("agent_id") or "").strip().lower()
        current_agent_name = str(trigger.get("agent_name") or "").strip().lower()
        candidates = []
        seen: set[int] = set()
        for plan in self._latest_agent_plans.values():
            if not isinstance(plan, dict) or id(plan) in seen:
                continue
            seen.add(id(plan))
            if plan.get("status") != "active":
                continue
            agent_id = str(plan.get("agent_id") or "").strip()
            agent_name = str(plan.get("agent_name") or "").strip()
            if (
                agent_id
                and agent_id.lower() == current_agent_id
                or agent_name
                and agent_name.lower() == current_agent_name
            ):
                continue
            score = 0
            if agent_name and agent_name in text:
                score += 100 + len(agent_name)
            if agent_id and agent_id in text:
                score += 80 + len(agent_id)
            if score:
                candidates.append((score, plan))
        if not candidates:
            return None
        candidates.sort(key=lambda item: item[0], reverse=True)
        return candidates[0][1]

    @staticmethod
    def _with_opinion_plan_context(trigger: dict[str, Any], plan: dict[str, Any]) -> dict[str, Any]:
        updated = dict(trigger)
        updated["_referenced_plan_context"] = dict(plan)
        return updated

    @staticmethod
    def _format_referenced_plan_context(plan: dict[str, Any]) -> str:
        agent_name = str(plan.get("agent_name") or plan.get("agent_id") or "该助手").strip()
        raw_text = str(plan.get("raw_text") or plan.get("compact_summary") or "").strip()
        if len(raw_text) > 1600:
            raw_text = raw_text[:1600].rstrip() + "..."
        return (
            "【被评价方案上下文】\n"
            f"用户正在询问你评价 {agent_name} 最近的方案。请基于下面方案内容回应，"
            "不要说对方尚未给出具体方案；可以指出优点、风险和可调整建议。\n"
            f"方案来源：{agent_name}\n"
            f"方案内容：\n{raw_text}"
        )

    def _with_resolved_plan(self, trigger: dict[str, Any], plan: dict[str, Any]) -> dict[str, Any]:
        updated = dict(trigger)
        agent_name = str(updated.get("agent_name") or plan.get("agent_name") or "该助手")
        summary = str(plan.get("compact_summary") or plan.get("raw_text") or "")
        updated["text"] = (
            f"@GM 开始执行 {agent_name} 刚才的方案：\n{summary}\n"
            "执行时必须使用完整 resolved_intent_text。"
        )
        updated["_resolved_plan"] = plan
        return updated

    def _find_agent_plan(self, agent_id: str, agent_name: str) -> dict[str, Any] | None:
        for key in self._agent_plan_keys(agent_id, agent_name):
            plan = self._latest_agent_plans.get(key)
            if plan and plan.get("status") == "active":
                return plan
        return None

    def _mark_agent_plan_stale(self, trigger: dict[str, Any]) -> None:
        for key in self._agent_plan_keys(
            str(trigger.get("agent_id") or ""),
            str(trigger.get("agent_name") or ""),
        ):
            plan = self._latest_agent_plans.get(key)
            if plan:
                plan["status"] = "stale"

    @classmethod
    def _marks_plan_stale(cls, text: str) -> bool:
        return any(word in str(text or "") for word in cls._STALE_PLAN_WORDS)

    @classmethod
    def _is_plan_reference(cls, text: str) -> bool:
        return any(word in str(text or "") for word in cls._PLAN_REFERENCE_WORDS)

    @classmethod
    def _is_plan_opinion_reference(cls, text: str) -> bool:
        value = str(text or "")
        return (
            any(word in value for word in cls._PLAN_WORDS)
            and any(word in value for word in cls._PLAN_OPINION_WORDS)
        )

    @classmethod
    def _looks_like_plan(cls, text: str) -> bool:
        normalized = str(text or "").strip()
        if len(normalized) < 16:
            return False
        return any(word in normalized for word in cls._PLAN_WORDS)

    @staticmethod
    def _agent_plan_keys(agent_id: str, agent_name: str) -> list[str]:
        keys = []
        for value in (agent_id, agent_name):
            key = str(value or "").strip().lower()
            if key and key not in keys:
                keys.append(key)
        return keys

    @staticmethod
    def _compact_plan(text: str, limit: int = 260) -> str:
        lines = [line.strip(" -\t") for line in str(text or "").splitlines() if line.strip()]
        compact = "\n".join(f"- {line}" for line in lines[:6])
        if not compact:
            compact = str(text or "").strip()
        if len(compact) > limit:
            compact = compact[:limit].rstrip() + "..."
        return compact

    @staticmethod
    def _missing_plan_text(trigger: dict[str, Any]) -> str:
        agent_name = str(trigger.get("agent_name") or "该助手")
        return f"【GM】我没有找到{agent_name}刚才的可执行方案。请先让{agent_name}给出明确方案，或重新描述要执行的内容。"

    @staticmethod
    def _safe_agent_error_text(exc: Exception) -> str:
        text = str(exc)
        if any(marker in text for marker in ("Invalid Token", "request id", "rix_api_error", "stack trace")):
            return "当前模型服务不可用，已跳过该助手回复。"
        return "该助手暂时无法响应，请稍后重试或换一个助手。"

    def _get_agent(self) -> Any:
        if self._agent is None:
            self._agent = self._agent_factory()
        return self._agent

    @staticmethod
    def _history_from_trigger(trigger: dict[str, Any]) -> list[dict[str, Any]]:
        history = trigger.get("history") or []
        return [item for item in history if isinstance(item, dict)]

    @staticmethod
    def _messages_from_trigger(trigger: dict[str, Any]) -> list[str]:
        messages: list[str] = []
        for item in LanChatAgentOrchestrator._history_from_trigger(trigger):
            sender = str(item.get("from") or item.get("sender_name") or "")
            text = str(item.get("text") or "")
            if text:
                messages.append(f"{sender}: {text}" if sender else text)

        text = str(trigger.get("text") or "")
        if text and text not in messages:
            messages.append(text)
        return messages

    @staticmethod
    def _infer_pair_conflicts(history: list[dict[str, Any]]) -> list[str]:
        object_mentions: dict[str, list[str]] = {}
        pattern = re.compile(r"(桌子|椅子|门|墙|蒙古包|篝火|灯|床|沙发|table|chair|door|wall|fire)", re.I)
        for item in LanChatAgentOrchestrator._user_chat_history(history)[-8:]:
            sender = str(item.get("from") or item.get("sender_name") or item.get("sender_id") or "")
            text = str(item.get("text") or "")
            for match in pattern.findall(text):
                key = match.lower()
                object_mentions.setdefault(key, []).append(sender or text[:12])
        conflicts = []
        for key, speakers in object_mentions.items():
            unique = [s for i, s in enumerate(speakers) if s and s not in speakers[:i]]
            if len(unique) >= 2:
                conflicts.append(f"{key}: " + " / ".join(unique[:3]))
        return conflicts

    @staticmethod
    def _user_chat_history(history: list[dict[str, Any]]) -> list[dict[str, Any]]:
        out: list[dict[str, Any]] = []
        for item in history:
            kind = str(item.get("message_kind") or "chat").strip().lower()
            sender_type = str(item.get("sender_type") or "user").strip().lower()
            if kind == "chat" and sender_type == "user":
                out.append(item)
        return out

    def _is_gm_trigger(self, trigger: dict[str, Any]) -> bool:
        agent_name = str(trigger.get("agent_name") or "").strip().lower()
        agent_id = str(trigger.get("agent_id") or "").strip().lower()
        target_agent_id = str(trigger.get("target_agent_id") or "").strip().lower()
        return agent_name in self._GM_NAMES or agent_id == "gm" or target_agent_id == "gm"

    @staticmethod
    def _is_multi_user(trigger: dict[str, Any]) -> bool:
        speakers: set[str] = set()
        sender = str(trigger.get("sender_id") or trigger.get("sender_name") or "")
        sender_name = str(trigger.get("sender_name") or "")
        if sender:
            speakers.add(sender)
        for item in LanChatAgentOrchestrator._history_from_trigger(trigger):
            speaker = str(
                item.get("sender_id")
                or item.get("from")
                or item.get("sender_name")
                or ""
            )
            if speaker and sender_name and speaker == sender_name and sender:
                speaker = sender
            if speaker:
                speakers.add(speaker)
        return len(speakers) >= 2

    @staticmethod
    def _trusted_host_confirmation(trigger: dict[str, Any]) -> bool | None:
        """Return True/False only when trigger carries an explicit room role."""
        for key in ("sender_role", "room_role", "role"):
            if key not in trigger:
                continue
            role = str(trigger.get(key) or "").strip().lower()
            if role:
                return role in {"host", "owner", "room_host", "房主"}
        for key in ("is_host", "is_room_host", "sender_is_host"):
            if key in trigger:
                return bool(trigger.get(key))
        return None

    @staticmethod
    def _join_lines(items: list[str]) -> str:
        return "；".join(items) if items else "无"

    @staticmethod
    def _default_agent_factory() -> Any:
        from plugins.AITool.cai_extensions.agent.agent_adapter import create_master_agent

        return create_master_agent()


__all__ = ["AgentOrchestrationResult", "LanChatAgentOrchestrator"]
