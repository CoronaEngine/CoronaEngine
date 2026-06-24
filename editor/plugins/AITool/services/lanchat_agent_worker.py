from __future__ import annotations

import logging
import json
import os
import re
import threading
import time
from collections import deque
from typing import Any, Callable

from .interaction_coordinator import ChatMessage, InteractionCoordinator
from .seed_plan import SeedPlanStatus
from .lanchat_agent_orchestrator import LanChatAgentOrchestrator
from .lanchat_host_action_executor import LanChatHostActionExecutor
from .generation_scheduler import GenerationScheduler
from .generation_composer_adapter import SceneComposerJobRunner


MAX_COORDINATOR_SYNC_MESSAGES_PER_TICK = 4
MAX_ROOM_EVENTS_PER_TICK = 4
MAX_COORDINATOR_SEEN_MESSAGE_IDS = 2048
MAX_ACTIVE_ROOM_IDS = 256
_SENSITIVE_WORKER_PAYLOAD_KEYS = {
    "prompt",
    "raw_prompt",
    "provider",
    "model_provider",
    "runtime_context",
    "scheduler_updates",
    "vlm_raw",
    "hidden_debug_ref",
    "debug",
    "job_id",
    "session_id",
    "token",
    "api_key",
}
_SENSITIVE_WORKER_TEXT_MARKERS = tuple(sorted(_SENSITIVE_WORKER_PAYLOAD_KEYS))


def _trace_preview(value: Any, limit: int = 80) -> str:
    text = str(value or "").replace("\r", "\\r").replace("\n", "\\n")
    if len(text) > limit:
        return f"{text[:limit]}..."
    return text


class LANChatAgentWorker:
    """Poll C++ LANChat agent triggers and return replies through C++."""

    def __init__(
        self,
        corona_engine: Any = None,
        agent_factory: Callable[[], Any] | None = None,
        host_action_executor: Any = None,
        interaction_coordinator: InteractionCoordinator | None = None,
        generation_scheduler: Any = None,
        composer_factory: Callable[[], Any] | None = None,
        sleep_seconds: float = 0.1,
        async_agent_execution: bool | None = None,
    ) -> None:
        self._corona_engine = corona_engine
        self._agent_factory = agent_factory
        self._host_action_executor = host_action_executor
        self._interaction_coordinator = interaction_coordinator
        self._generation_scheduler = generation_scheduler
        self._composer_factory = composer_factory
        self._owns_generation_scheduler = generation_scheduler is None and interaction_coordinator is None
        self._sleep_seconds = sleep_seconds
        self._async_agent_execution = (
            os.getenv("LANCHAT_AGENT_ASYNC", "1") == "1"
            if async_agent_execution is None
            else bool(async_agent_execution)
        )
        self._stop_event = threading.Event()
        self._thread: threading.Thread | None = None
        self._orchestrator: LanChatAgentOrchestrator | None = None
        self._agent_call_lock = threading.RLock()
        self._coordinator_seen_message_ids: set[str] = set()
        self._coordinator_seen_message_order: deque[str] = deque()
        self._active_room_ids: set[str] = set()
        self._active_room_order: deque[str] = deque()
        self._progress_disclosure_lock = threading.RLock()
        self._progress_disclosure_last_by_room: dict[str, tuple[str, float]] = {}
        self._logger = logging.getLogger(__name__)
        if self._generation_scheduler is not None:
            self._install_generation_scheduler_hooks(self._generation_scheduler)

    def start(self) -> None:
        if self._thread is not None and self._thread.is_alive():
            return
        if not self._has_engine_api():
            return

        self._stop_event.clear()
        self._thread = threading.Thread(
            target=self._run,
            name="LANChatAgentWorker",
            daemon=True,
        )
        self._thread.start()

    def stop(self, timeout: float = 1.0) -> None:
        self._stop_event.set()
        if self._thread is not None and self._thread.is_alive():
            self._thread.join(timeout=timeout)
        if self._generation_scheduler is not None:
            self._clear_generation_scheduler_hooks(self._generation_scheduler)
        if self._owns_generation_scheduler and self._generation_scheduler is not None:
            shutdown = getattr(self._generation_scheduler, "shutdown", None)
            if callable(shutdown):
                shutdown()

    def generation_scheduler_snapshot(self) -> dict[str, Any]:
        scheduler = self._generation_scheduler
        if scheduler is None:
            return {"available": False, "reason": "generation scheduler has not been initialized"}
        snapshot = getattr(scheduler, "public_snapshot", None)
        if not callable(snapshot):
            snapshot = getattr(scheduler, "snapshot", None)
        if not callable(snapshot):
            return {"available": False, "reason": "generation scheduler does not expose snapshot"}
        data = snapshot()
        if isinstance(data, dict):
            return {"available": True, **data}
        return {"available": False, "reason": "generation scheduler snapshot returned non-dict"}

    def generation_scheduler_session_snapshot(self, session_id: str) -> dict[str, Any]:
        scheduler = self._generation_scheduler
        if scheduler is None:
            return {"available": False, "reason": "generation scheduler has not been initialized"}
        session_snapshot = getattr(scheduler, "public_session_snapshot", None)
        if not callable(session_snapshot):
            session_snapshot = getattr(scheduler, "session_snapshot", None)
        if not callable(session_snapshot):
            return {"available": False, "reason": "generation scheduler does not expose session_snapshot"}
        data = session_snapshot(session_id)
        if isinstance(data, dict):
            return {"available": True, **data}
        return {"available": False, "reason": "generation scheduler session_snapshot returned non-dict"}

    def cancel_generation_session(self, session_id: str, *, abandon_remote: bool = False) -> dict[str, Any]:
        scheduler = self._generation_scheduler
        if scheduler is None:
            return {"available": False, "reason": "generation scheduler has not been initialized"}
        cancel_session = getattr(scheduler, "cancel_session", None)
        if not callable(cancel_session):
            return {"available": False, "reason": "generation scheduler does not expose cancel_session"}
        result = cancel_session(session_id, abandon_remote=abandon_remote)
        if isinstance(result, dict):
            return {"available": True, **result}
        return {"available": False, "reason": "generation scheduler cancel_session returned non-dict"}

    def handle_lanchat_room_event(self, event: dict[str, Any]) -> dict[str, Any]:
        if not isinstance(event, dict):
            return {"handled": False, "reason": "event is not a dict"}
        event_type = str(event.get("event") or event.get("type") or "").strip().lower()
        room_id = str(event.get("room_id") or event.get("room") or "").strip()
        if room_id:
            self._remember_room_id(room_id)
        if event_type not in {"room_closed", "leave_room", "left", "stop_room", "stopped", "closed"}:
            return {"handled": False, "reason": "event does not close a room"}
        target_rooms = [room_id] if room_id else sorted(self._active_room_ids)
        if not target_rooms:
            return {"handled": True, "cancelled": [], "reason": "no active room id known"}
        cancelled = []
        for target_room in target_rooms:
            cancelled.append(self.cancel_generation_session(target_room, abandon_remote=True))
            self._forget_room_id(target_room)
        return {"handled": True, "cancelled": cancelled}

    def sync_chat_message_to_coordinator(
        self,
        message: dict[str, Any],
        *,
        source: str = "lanchat_direct",
        emit_disclosure: bool = True,
    ) -> bool:
        """Sync one ordinary LANChat user/host message into InteractionCoordinator.

        This is the Python bridge point for non-@Agent chat messages. It does
        not run role agents or execute generation; Coordinator decides whether
        the message updates a SeedPlan draft or becomes a batch intervention.
        """
        if not isinstance(message, dict):
            return False
        self._apply_generation_options_from_message(message)
        message_kind = str(message.get("message_kind") or "chat").lower()
        sender_type = str(message.get("sender_type") or "user").lower()
        dedupe_key = self._coordinator_sync_dedupe_key(message, source=source)
        if not dedupe_key:
            self._logger.info(
                "[LANChatSyncTrace] phase=skip_no_dedupe source=%s message_id=%s room=%s sender=%s/%s text=%s",
                source,
                message.get("message_id") or "",
                message.get("room_id") or "",
                message.get("sender_type") or "",
                message.get("sender_id") or message.get("from") or "",
                _trace_preview(message.get("text")),
            )
            return False
        if dedupe_key in self._coordinator_seen_message_ids:
            self._logger.info(
                "[LANChatSyncTrace] phase=dedupe_skip source=%s dedupe=%s message_id=%s room=%s sender=%s/%s text=%s",
                source,
                dedupe_key,
                message.get("message_id") or "",
                message.get("room_id") or "",
                message.get("sender_type") or "",
                message.get("sender_id") or message.get("from") or "",
                _trace_preview(message.get("text")),
            )
            return False
        self._logger.info(
            "[LANChatSyncTrace] phase=received source=%s dedupe=%s message_id=%s correlation=%s room=%s kind=%s sender=%s/%s/%s target=%s/%s text=%s",
            source,
            dedupe_key,
            message.get("message_id") or "",
            message.get("correlation_id") or "",
            message.get("room_id") or "",
            message_kind,
            sender_type,
            message.get("sender_id") or message.get("from") or "",
            message.get("sender_name") or "",
            message.get("target_agent_id") or message.get("agent_id") or "",
            message.get("target_agent_name") or message.get("agent_name") or "",
            _trace_preview(message.get("text")),
        )
        if message_kind != "chat" or sender_type not in {"user", "host"}:
            self._logger.info(
                "[LANChatSyncTrace] phase=skip_non_chat source=%s dedupe=%s kind=%s sender_type=%s",
                source,
                dedupe_key,
                message_kind,
                sender_type,
            )
            self._remember_coordinator_seen_message_id(dedupe_key)
            return False
        text = str(message.get("text") or "").strip()
        if not text:
            self._logger.info(
                "[LANChatSyncTrace] phase=skip_empty_text source=%s dedupe=%s message_id=%s",
                source,
                dedupe_key,
                message.get("message_id") or "",
            )
            self._remember_coordinator_seen_message_id(dedupe_key)
            return False
        try:
            coordinator = self._get_interaction_coordinator()
            disclosure_start = len(coordinator.disclosure_events)
            room_id = str(message.get("room_id") or "default")
            self._remember_room_id(room_id)
            metadata = self._coordinator_sync_metadata(message, source=source)
            metadata = self._normalize_coordinator_target_metadata(message, text, metadata)
            active = coordinator.active_plan_for_room(room_id)
            self._logger.info(
                "[LANChatSyncTrace] phase=route_start source=%s dedupe=%s room=%s active=%s plan=%s draft_action=%s target_scope=%s target_agent=%s/%s metadata_keys=%s",
                source,
                dedupe_key,
                room_id,
                str(active.status.value if active is not None else "none"),
                str(getattr(active, "plan_id", "") or ""),
                metadata.get("draft_action") or "",
                metadata.get("target_scope") or "",
                metadata.get("target_agent_id") or "",
                metadata.get("target_agent_name") or "",
                ",".join(sorted(str(key) for key in metadata.keys())),
            )
            authoritative_synced = False
            if self._should_sync_metadata_scene_message_to_seed_plan(coordinator, room_id, text, metadata):
                self._logger.info(
                    "[LANChatSyncTrace] phase=authoritative_ingest source=%s dedupe=%s room=%s sender=%s host=%s text=%s",
                    source,
                    dedupe_key,
                    room_id,
                    message.get("sender_id") or message.get("from") or "",
                    bool(message.get("is_host") or sender_type == "host"),
                    _trace_preview(text),
                )
                coordinator.ingest_message(ChatMessage(
                    room_id=room_id,
                    sender_id=str(message.get("sender_id") or message.get("from") or ""),
                    sender_name=str(message.get("sender_name") or message.get("from") or ""),
                    text=text,
                    is_host=bool(message.get("is_host") or sender_type == "host"),
                    agent_id=str(metadata.get("target_agent_id") or ""),
                    agent_name=str(metadata.get("target_agent_name") or ""),
                    metadata=metadata,
                ))
                authoritative_synced = True
                active = coordinator.active_plan_for_room(room_id)
            structured_handled = self._handle_structured_chat_route(message, text, metadata)
            if structured_handled:
                self._logger.info(
                    "[LANChatSyncTrace] phase=structured_handled source=%s dedupe=%s room=%s action=%s authoritative=%s active=%s plan=%s",
                    source,
                    dedupe_key,
                    room_id,
                    structured_handled,
                    authoritative_synced,
                    str(active.status.value if active is not None else "none"),
                    str(getattr(active, "plan_id", "") or ""),
                )
                self._log_scene_route(
                    room_id=room_id,
                    sender=str(message.get("sender_name") or message.get("sender_id") or ""),
                    target_agent=str(
                        metadata.get("target_agent_name")
                        or metadata.get("target_agent_id")
                        or message.get("target_agent_name")
                        or message.get("agent_name")
                        or ""
                    ),
                    room_state=str(active.status.value if active is not None else "structured"),
                    intent=str(metadata.get("draft_action") or "structured"),
                    action=structured_handled,
                    reason="metadata route",
                )
                return True
            if (
                source == "lanchat_history_snapshot"
                and active is not None
                and active.status == SeedPlanStatus.COMPLETED
                and not coordinator._is_status_query(text)
                and coordinator._intent_type(text) != "add"
                and not coordinator._is_post_generation_adjustment(text)
            ):
                return False
            planning_gate_handled = ""
            if source != "lanchat_history_snapshot":
                planning_gate_handled = self._handle_plain_chat_planning_gate(message, text)
            if planning_gate_handled in {"reply", "compose"}:
                self._logger.info(
                    "[LANChatSyncTrace] phase=planning_gate_handled source=%s dedupe=%s room=%s action=%s authoritative=%s",
                    source,
                    dedupe_key,
                    room_id,
                    planning_gate_handled,
                    authoritative_synced,
                )
                self._log_scene_route(
                    room_id=room_id,
                    sender=str(message.get("sender_name") or message.get("sender_id") or ""),
                    target_agent=str(message.get("target_agent_name") or message.get("agent_name") or ""),
                    room_state="planning",
                    intent="planning_gate",
                    action=planning_gate_handled,
                    reason="pending planning message",
                )
                return True
            if self._is_generation_start_text(text):
                generation_reply = self._start_active_coordinator_generation(
                    coordinator,
                    room_id=room_id,
                    host_id=str(message.get("sender_id") or message.get("from") or ""),
                )
                if generation_reply is not None:
                    self._send_coordinator_sync_system_reply(message, generation_reply)
                    self._log_scene_route(
                        room_id=room_id,
                        sender=str(message.get("sender_name") or message.get("sender_id") or ""),
                        target_agent=str(message.get("target_agent_name") or message.get("agent_name") or ""),
                        room_state=str(coordinator.active_plan_for_room(room_id).status.value if coordinator.active_plan_for_room(room_id) is not None else "none"),
                        intent="generation_start",
                        action="confirm_and_execute",
                        reason=f"source={source}",
                    )
                    return True
            if authoritative_synced:
                self._logger.info(
                    "[LANChatSyncTrace] phase=authoritative_only_done source=%s dedupe=%s room=%s plan=%s",
                    source,
                    dedupe_key,
                    room_id,
                    str(getattr(active, "plan_id", "") or ""),
                )
                if emit_disclosure:
                    self._emit_new_disclosure_events(coordinator, disclosure_start)
                return True
            if not planning_gate_handled and not self._should_sync_chat_to_coordinator(coordinator, room_id, text, source=source):
                self._logger.info(
                    "[LANChatSyncTrace] phase=skip_not_scene_write source=%s dedupe=%s room=%s active=%s text=%s",
                    source,
                    dedupe_key,
                    room_id,
                    str(active.status.value if active is not None else "none"),
                    _trace_preview(text),
                )
                self._log_scene_route(
                    room_id=room_id,
                    sender=str(message.get("sender_name") or message.get("sender_id") or ""),
                    target_agent=str(message.get("target_agent_name") or message.get("agent_name") or ""),
                    room_state=str(active.status.value if active is not None else "none"),
                    intent="chat",
                    action="skip_coordinator",
                    reason="not scene-write intent",
                )
                return False
            event = coordinator.ingest_message(ChatMessage(
                room_id=room_id,
                sender_id=str(message.get("sender_id") or message.get("from") or ""),
                sender_name=str(message.get("sender_name") or message.get("from") or ""),
                text=text,
                is_host=bool(message.get("is_host") or sender_type == "host"),
                agent_id=str(metadata.get("target_agent_id") or ""),
                agent_name=str(metadata.get("target_agent_name") or ""),
                metadata=metadata,
            ))
            event_type = str(getattr(event, "event_type", "") or "")
            if event_type in {"layout_reflow_proposal_created", "layout_reflow_confirmed", "layout_reflow_rejected", "layout_reflow_confirmation_failed"}:
                reply = str(getattr(event, "message", "") or "")
                if event_type == "layout_reflow_confirmed":
                    executed = self._execute_layout_reflow_confirmation(getattr(event, "payload", {}) or {})
                    if executed:
                        reply = f"{reply}\n{executed}" if reply else executed
                if reply:
                    self._send_coordinator_sync_system_reply(message, reply)
            updated = coordinator.active_plan_for_room(room_id)
            self._logger.info(
                "[LANChatSyncTrace] phase=coordinator_ingested source=%s dedupe=%s room=%s before=%s after=%s plan=%s design_len=%s",
                source,
                dedupe_key,
                room_id,
                str(active.status.value if active is not None else "none"),
                str(updated.status.value if updated is not None else "none"),
                str(getattr(updated, "plan_id", "") or ""),
                len(str(getattr(updated, "design_brief", "") or "")) if updated is not None else 0,
            )
            self._log_scene_route(
                room_id=room_id,
                sender=str(message.get("sender_name") or message.get("sender_id") or ""),
                target_agent=str(message.get("target_agent_name") or message.get("agent_name") or ""),
                room_state=str(active.status.value if active is not None else "draft"),
                intent="scene_write",
                action="coordinator_ingest",
                reason=f"source={source}",
            )
            if emit_disclosure:
                self._emit_new_disclosure_events(coordinator, disclosure_start)
            return True
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Failed to sync LANChat chat message to Coordinator: %s", exc)
            return False
        finally:
            self._remember_coordinator_seen_message_id(dedupe_key)

    def _should_sync_metadata_scene_message_to_seed_plan(
        self,
        coordinator: InteractionCoordinator,
        room_id: str,
        text: str,
        metadata: dict[str, Any],
    ) -> bool:
        if not metadata:
            return False
        draft_action = str(metadata.get("draft_action") or "").strip().lower()
        target_scope = str(metadata.get("target_scope") or "").strip().lower()
        if draft_action in {"supplement", "generate"} or target_scope == "plan":
            return True
        if draft_action != "chat":
            return False
        active = coordinator.active_plan_for_room(room_id)
        if active is not None and active.status in {SeedPlanStatus.CONFIRMED, SeedPlanStatus.EXECUTING, SeedPlanStatus.PAUSED}:
            return False
        return self._looks_like_seedplan_design_message(text)

    @staticmethod
    def _looks_like_seedplan_design_message(text: str) -> bool:
        raw = str(text or "").strip()
        if not raw:
            return False
        opinion_patterns = ("怎么看", "你觉得", "大家觉得", "对于", "评价", "看法")
        strong_update_words = ("采用", "按", "确认", "补充", "新增", "调整", "修改", "生成", "开始")
        if any(word in raw for word in opinion_patterns) and not any(word in raw for word in strong_update_words):
            return False
        scene_words = (
            "方案", "场景", "主题", "设计", "布局", "风格", "物品", "清单",
            "集市", "鬼市", "卧室", "客厅", "房间", "展厅", "商业空间",
            "草原", "电竞房", "庭院", "街区", "摊位",
        )
        action_words = (
            "围绕", "讨论", "设计", "优化", "简化", "采用", "生成", "做",
            "帮我", "补充", "调整", "新增", "改成", "还是", "整理",
        )
        return any(word in raw for word in scene_words) and any(word in raw for word in action_words)

    def _handle_structured_chat_route(
        self,
        message: dict[str, Any],
        text: str,
        metadata: dict[str, Any],
    ) -> str:
        metadata = self._normalize_coordinator_target_metadata(message, text, metadata)
        draft_action = str(metadata.get("draft_action") or "").strip().lower()
        target_scope = str(metadata.get("target_scope") or "").strip().lower()
        target_agent_id = str(metadata.get("target_agent_id") or "").strip()
        target_agent_name = str(metadata.get("target_agent_name") or "").strip()
        target_plan_id = str(metadata.get("target_plan_id") or "").strip()
        if not any((draft_action, target_scope, target_agent_id, target_agent_name, target_plan_id)):
            return ""
        if not self._can_execute_agent_locally():
            self._logger.info(
                "[LANChatAgentTrace] phase=blocked_non_host_agent route=structured_chat role=%s message_id=%s room=%s action=%s target_scope=%s target_agent=%s/%s text=%s",
                self._network_session_role_name(),
                message.get("message_id") or "",
                message.get("room_id") or "",
                draft_action,
                target_scope,
                target_agent_id,
                target_agent_name,
                _trace_preview(text),
            )
            return "blocked_non_host_agent"
        if draft_action == "chat" and target_scope == "group":
            group_agents = self._structured_group_agents(metadata)
            if not group_agents:
                return ""
            for agent_id, agent_name in group_agents:
                trigger = self._structured_trigger(message, metadata, agent_id=agent_id, agent_name=agent_name)
                self._process_trigger(trigger)
            return "group_chat"
        if draft_action == "chat" and (target_agent_id or target_agent_name or target_scope == "agent"):
            agent_id = target_agent_id or target_agent_name or "agent"
            agent_name = target_agent_name or target_agent_id or "Agent"
            trigger = self._structured_trigger(message, metadata, agent_id=agent_id, agent_name=agent_name)
            self._process_trigger(trigger)
            return "agent_chat"
        if draft_action in {"plan", "supplement", "generate"} or target_scope == "plan" or target_plan_id:
            return self._handle_structured_planning_gate(message, text, metadata)
        if draft_action == "gm_control" or target_scope == "gm":
            trigger = self._structured_trigger(
                message,
                metadata,
                agent_id=target_agent_id or "gm",
                agent_name=target_agent_name or "GM",
            )
            self._process_trigger(trigger)
            return "gm_control"
        return ""

    def _handle_structured_planning_gate(
        self,
        message: dict[str, Any],
        text: str,
        metadata: dict[str, Any],
    ) -> str:
        try:
            from .lanchat_scene_runtime import get_lanchat_scene_runtime
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Failed to import LANChat scene runtime for metadata planning route: %s", exc)
            return ""
        draft_action = str(metadata.get("draft_action") or "").strip().lower()
        target = (
            str(metadata.get("target_plan_id") or "").strip()
            or str(metadata.get("target_agent_name") or "").strip()
            or str(metadata.get("target_agent_id") or "").strip()
        )
        try:
            runtime = get_lanchat_scene_runtime()
            if draft_action == "plan":
                agent_name = (
                    str(metadata.get("target_agent_name") or "").strip()
                    or str(metadata.get("target_agent_id") or "").strip()
                    or "设计助手"
                )
                action, payload = runtime.handle_planning_gate(agent_name, text)
                if action == "pass":
                    return ""
            elif target:
                action, payload, agent_name = runtime.handle_targeted_planning_message(
                    target,
                    text,
                    draft_action=draft_action,
                    source_context_agent=str(metadata.get("source_context_agent") or ""),
                )
            else:
                agent_name = str(metadata.get("target_agent_name") or metadata.get("target_agent_id") or "").strip()
                action, payload = runtime.handle_planning_gate(agent_name or "设计助手", text)
                if action == "pass":
                    return ""
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Failed to handle metadata planning route: %s", exc)
            return ""
        if action not in {"reply", "compose"} or not agent_name:
            return ""
        trigger = self._structured_trigger(
            message,
            metadata,
            agent_id=str(metadata.get("target_agent_id") or agent_name),
            agent_name=str(agent_name),
        )
        if action == "reply":
            self._send_final_reply(str(trigger.get("agent_id") or agent_name), str(agent_name), str(payload or ""), trigger)
            return "planning_reply"
        if self._execute_runtime_planning_compose(trigger, str(payload or text), str(agent_name)):
            return "planning_compose"
        trigger["text"] = str(payload or text)
        self._process_trigger(trigger)
        return "planning_compose"

    def _structured_trigger(
        self,
        message: dict[str, Any],
        metadata: dict[str, Any],
        *,
        agent_id: str,
        agent_name: str,
    ) -> dict[str, Any]:
        trigger = dict(message)
        trigger["agent_id"] = str(agent_id or "agent")
        trigger["agent_name"] = str(agent_name or "Agent")
        trigger["target_agent_id"] = str(agent_id or "")
        trigger["target_agent_name"] = str(agent_name or "")
        trigger["metadata"] = dict(metadata or {})
        trigger["metadata_json"] = json.dumps(trigger["metadata"], ensure_ascii=False)
        return trigger

    @staticmethod
    def _structured_group_agents(metadata: dict[str, Any]) -> list[tuple[str, str]]:
        names_raw = metadata.get("target_agent_names")
        ids_raw = metadata.get("target_agent_ids")
        names = names_raw if isinstance(names_raw, list) else []
        ids = ids_raw if isinstance(ids_raw, list) else []
        out: list[tuple[str, str]] = []
        for index, raw_name in enumerate(names):
            name = str(raw_name or "").strip()
            if not name:
                continue
            agent_id = str(ids[index] if index < len(ids) else name).strip() or name
            out.append((agent_id, name))
        return out

    def _handle_plain_chat_planning_gate(self, message: dict[str, Any], text: str) -> str:
        if not self._can_execute_agent_locally():
            self._logger.info(
                "[LANChatAgentTrace] phase=blocked_non_host_agent route=plain_planning_gate role=%s message_id=%s room=%s text=%s",
                self._network_session_role_name(),
                message.get("message_id") or "",
                message.get("room_id") or "",
                _trace_preview(text),
            )
            return ""
        try:
            from .lanchat_scene_runtime import get_lanchat_scene_runtime
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Failed to import LANChat scene runtime for plain planning gate: %s", exc)
            return ""
        try:
            action, payload, agent_name = get_lanchat_scene_runtime().handle_pending_planning_message(text)
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Failed to handle plain chat planning gate: %s", exc)
            return ""
        if action not in {"reply", "compose"} or not agent_name:
            return ""
        trigger = dict(message)
        trigger.setdefault("agent_id", str(agent_name))
        trigger.setdefault("agent_name", str(agent_name))
        trigger.setdefault("target_agent_id", str(agent_name))
        trigger.setdefault("target_agent_name", str(agent_name))
        if action == "reply":
            self._send_final_reply(str(agent_name), str(agent_name), str(payload or ""), trigger)
            return "reply"
        if self._execute_runtime_planning_compose(trigger, str(payload or text), str(agent_name)):
            return "compose"
        trigger["text"] = str(payload or text)
        self._process_trigger(trigger)
        return "compose"

    def _handle_agent_trigger_planning_gate(self, trigger: dict[str, Any]) -> bool:
        text = str(trigger.get("text") or "").strip()
        if not text:
            return False
        message_kind = str(trigger.get("message_kind") or "chat").strip().lower()
        if message_kind not in {"", "chat"}:
            return False
        is_gm_target = (
            str(trigger.get("agent_id") or trigger.get("target_agent_id") or "").strip().lower() == "gm"
            or str(trigger.get("agent_name") or "").strip().lower() in {"gm", "主持人", "裁判", "game master"}
        )
        if is_gm_target:
            return False
        try:
            from .lanchat_scene_runtime import get_lanchat_scene_runtime
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Failed to import LANChat scene runtime for agent planning gate: %s", exc)
            return False

        metadata = self._metadata_from_trigger(trigger)
        draft_action = str(metadata.get("draft_action") or "").strip().lower()
        targets = [
            str(metadata.get("target_plan_id") or "").strip(),
            str(metadata.get("target_agent_name") or "").strip(),
            str(metadata.get("target_agent_id") or "").strip(),
            str(trigger.get("target_agent_name") or "").strip(),
            str(trigger.get("agent_name") or "").strip(),
            str(trigger.get("target_agent_id") or "").strip(),
            str(trigger.get("agent_id") or "").strip(),
        ]
        try:
            runtime = get_lanchat_scene_runtime()
            for target in targets:
                if not target:
                    continue
                action, payload, agent_name = runtime.handle_targeted_planning_message(
                    target,
                    text,
                    draft_action=draft_action,
                    source_context_agent=str(metadata.get("source_context_agent") or ""),
                )
                if action in {"reply", "compose"} and agent_name:
                    return self._send_runtime_planning_action(trigger, action, payload, agent_name)
            action, payload, agent_name = runtime.handle_pending_planning_message(text)
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Failed to handle agent planning gate: %s", exc)
            return False
        if action in {"reply", "compose"} and agent_name:
            return self._send_runtime_planning_action(trigger, action, payload, agent_name)
        return False

    def _send_runtime_planning_action(
        self,
        trigger: dict[str, Any],
        action: str,
        payload: str | None,
        agent_name: str,
    ) -> bool:
        agent_id = str(trigger.get("agent_id") or trigger.get("target_agent_id") or agent_name)
        visible_name = str(agent_name or trigger.get("agent_name") or "设计助手")
        if action == "reply":
            return bool(self._send_final_reply(agent_id, visible_name, str(payload or ""), trigger))
        if action == "compose":
            return self._execute_runtime_planning_compose(trigger, str(payload or ""), visible_name)
        return False

    def _execute_runtime_planning_compose(
        self,
        trigger: dict[str, Any],
        compose_text: str,
        agent_name: str,
    ) -> bool:
        text = str(compose_text or "").strip()
        if not text:
            return False
        try:
            coordinator = self._get_interaction_coordinator()
            room_id = str(trigger.get("room_id") or "default")
            host_id = str(trigger.get("sender_id") or trigger.get("from") or "host")
            self._remember_room_id(room_id)
            coordinator.create_or_update_seed_plan(ChatMessage(
                room_id=room_id,
                sender_id=host_id,
                sender_name=str(trigger.get("sender_name") or trigger.get("from") or "房主"),
                text=text,
                is_host=True,
                agent_id=str(trigger.get("agent_id") or trigger.get("target_agent_id") or agent_name or ""),
                agent_name=str(agent_name or trigger.get("agent_name") or ""),
                metadata=self._coordinator_sync_metadata(trigger, source="lanchat_runtime_planning_gate"),
            ))
            reply = self._start_active_coordinator_generation(
                coordinator,
                room_id=room_id,
                host_id=host_id,
            )
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Failed to execute runtime planning compose: %s", exc)
            return False
        if reply is None:
            return False
        return bool(self._send_final_reply("gm-system", "系统", reply, trigger))

    def _log_scene_route(
        self,
        *,
        room_id: str,
        sender: str,
        target_agent: str,
        room_state: str,
        intent: str,
        action: str,
        reason: str,
    ) -> None:
        self._logger.info(
            "[LANChatIntentRoute] room=%s sender=%s target=%s state=%s intent=%s action=%s reason=%s",
            room_id or "default",
            sender or "",
            target_agent or "",
            room_state or "",
            intent or "",
            action or "",
            reason or "",
        )

    def _should_sync_chat_to_coordinator(
        self,
        coordinator: InteractionCoordinator,
        room_id: str,
        text: str,
        *,
        source: str,
    ) -> bool:
        active = coordinator.active_plan_for_room(room_id)
        if active is not None and active.status in {
            SeedPlanStatus.CONFIRMED,
            SeedPlanStatus.EXECUTING,
            SeedPlanStatus.PAUSED,
        }:
            return True
        if active is not None and coordinator._is_status_query(text):
            return True
        if active is not None and active.status == SeedPlanStatus.COMPLETED:
            return (
                coordinator._intent_type(text) == "add"
                or coordinator._is_post_generation_adjustment(text)
            )
        try:
            from plugins.AITool.cai_extensions.agent.agent_adapter import classify_intent
        except Exception:  # noqa: BLE001
            try:
                from cai_extensions.agent.agent_adapter import classify_intent  # type: ignore
            except Exception as exc:  # noqa: BLE001
                self._logger.debug("Failed to import scene intent classifier for %s: %s", source, exc)
                return False
        try:
            intent = classify_intent(text)
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Failed to classify LANChat chat message for Coordinator sync: %s", exc)
            return False
        return intent in {"compose", "edit"}

    def process_once(self) -> bool:
        if not self._has_engine_api():
            return False

        processed_room_event = self._process_room_events(
            max_events=MAX_ROOM_EVENTS_PER_TICK,
        )
        processed_coordinator_sync = self._process_coordinator_sync_messages(
            max_messages=MAX_COORDINATOR_SYNC_MESSAGES_PER_TICK,
        )

        try:
            trigger = self._corona_engine.network_pop_lanchat_agent_trigger()
        except Exception as exc:
            self._logger.debug("Failed to poll LANChat agent trigger: %s", exc)
            return processed_room_event or processed_coordinator_sync

        if not trigger:
            return processed_room_event or processed_coordinator_sync

        self._logger.info(
            "[LANChatAgentTrace] phase=trigger_pop message_id=%s correlation=%s room=%s sender=%s/%s target=%s/%s kind=%s text=%s",
            trigger.get("message_id") or "",
            trigger.get("correlation_id") or "",
            trigger.get("room_id") or "",
            trigger.get("sender_type") or "",
            trigger.get("sender_id") or trigger.get("from") or "",
            trigger.get("target_agent_id") or trigger.get("agent_id") or "",
            trigger.get("target_agent_name") or trigger.get("agent_name") or "",
            trigger.get("message_kind") or "",
            _trace_preview(trigger.get("text")),
        )
        self._sync_trigger_history_to_coordinator(trigger)

        if self._async_agent_execution:
            threading.Thread(
                target=self._process_trigger,
                args=(trigger,),
                name="LANChatAgentTask",
                daemon=True,
            ).start()
            return True

        return self._process_trigger(trigger)

    def _process_room_events(self, *, max_events: int) -> bool:
        if not hasattr(self._corona_engine, "network_pop_lanchat_room_event"):
            return False
        processed = False
        limit = max(1, int(max_events or 1))
        for _ in range(limit):
            try:
                event = self._corona_engine.network_pop_lanchat_room_event()
            except Exception as exc:
                self._logger.debug("Failed to poll LANChat room event: %s", exc)
                break
            if not event:
                break
            processed = True
            try:
                self.handle_lanchat_room_event(dict(event))
            except Exception as exc:  # noqa: BLE001
                self._logger.debug("Failed to handle LANChat room event: %s", exc)
        return processed

    def _process_coordinator_sync_messages(self, *, max_messages: int) -> bool:
        if not hasattr(self._corona_engine, "network_pop_lanchat_coordinator_sync_message"):
            return False
        processed = False
        limit = max(1, int(max_messages or 1))
        for _ in range(limit):
            try:
                message = self._corona_engine.network_pop_lanchat_coordinator_sync_message()
            except Exception as exc:
                self._logger.debug("Failed to poll LANChat Coordinator sync message: %s", exc)
                break
            if not message:
                break
            processed = True
            self._logger.info(
                "[LANChatSyncTrace] phase=native_queue_pop message_id=%s correlation=%s room=%s sender=%s/%s target=%s/%s text=%s",
                message.get("message_id") or "",
                message.get("correlation_id") or "",
                message.get("room_id") or "",
                message.get("sender_type") or "",
                message.get("sender_id") or message.get("from") or "",
                message.get("target_agent_id") or message.get("agent_id") or "",
                message.get("target_agent_name") or message.get("agent_name") or "",
                _trace_preview(message.get("text")),
            )
            self.sync_chat_message_to_coordinator(
                dict(message),
                source="lanchat_native_queue",
                emit_disclosure=True,
            )
        return processed

    def _process_trigger(self, trigger: dict[str, Any]) -> bool:
        self._apply_generation_options_from_message(trigger)
        agent_id = str(trigger.get("agent_id") or "agent")
        agent_name = str(trigger.get("agent_name") or "Agent")
        action_payload = None
        if not self._can_execute_agent_locally():
            self._logger.info(
                "[LANChatAgentTrace] phase=blocked_non_host_agent route=process_trigger role=%s message_id=%s correlation=%s room=%s agent=%s/%s sender=%s/%s kind=%s text=%s",
                self._network_session_role_name(),
                trigger.get("message_id") or "",
                self._correlation_id(trigger),
                trigger.get("room_id") or "",
                agent_id,
                agent_name,
                trigger.get("sender_type") or "",
                trigger.get("sender_id") or trigger.get("from") or "",
                trigger.get("message_kind") or "",
                _trace_preview(trigger.get("text")),
            )
            return False
        self._logger.info(
            "[LANChatAgentTrace] phase=process_start message_id=%s correlation=%s room=%s agent=%s/%s sender=%s/%s kind=%s text=%s",
            trigger.get("message_id") or "",
            self._correlation_id(trigger),
            trigger.get("room_id") or "",
            agent_id,
            agent_name,
            trigger.get("sender_type") or "",
            trigger.get("sender_id") or trigger.get("from") or "",
            trigger.get("message_kind") or "",
            _trace_preview(trigger.get("text")),
        )

        def _send_progress(message: str) -> None:
            text = str(message or "").strip()
            if not text:
                return
            try:
                if hasattr(self._corona_engine, "network_send_agent_reply_ex"):
                    self._corona_engine.network_send_agent_reply_ex(
                        agent_id,
                        agent_name,
                        text,
                        "progress",
                        agent_id,
                        self._correlation_id(trigger),
                        json.dumps({"phase": "progress"}, ensure_ascii=False),
                    )
                else:
                    self._corona_engine.network_send_agent_reply(
                        agent_id,
                        agent_name,
                        text,
                    )
            except Exception as exc:
                self._logger.debug("Failed to send LANChat progress reply: %s", exc)

        control_reply = self._handle_coordinator_gm_control(trigger)
        if control_reply is not None:
            return bool(self._send_final_reply("gm-system", "GM", control_reply, trigger))
        clarification_reply = self._handle_coordinator_gm_clarification(trigger)
        if clarification_reply is not None:
            return bool(self._send_final_reply("gm-system", "GM", clarification_reply, trigger))
        status_reply = self._handle_coordinator_status_query(trigger)
        if status_reply is not None:
            return bool(self._send_final_reply(agent_id, agent_name, status_reply, trigger))
        generation_start_reply = self._handle_coordinator_generation_start(trigger)
        if generation_start_reply is not None:
            return bool(self._send_final_reply("gm-system", "GM", generation_start_reply, trigger))
        completed_intervention_reply = self._handle_coordinator_completed_intervention(trigger)
        if completed_intervention_reply is not None:
            return bool(self._send_final_reply(agent_id, agent_name, completed_intervention_reply, trigger))
        if self._handle_agent_trigger_planning_gate(trigger):
            return True

        try:
            from .agent_progress_context import agent_progress_sink
            from .lanchat_scene_runtime import get_lanchat_scene_runtime

            is_gm_target = (
                str(trigger.get("agent_id") or trigger.get("target_agent_id") or "").strip().lower() == "gm"
                or str(trigger.get("agent_name") or "").strip().lower() in {"gm", "主持人", "裁判", "game master"}
            )
            if not is_gm_target and str(trigger.get("message_kind") or "chat").strip().lower() in {"", "chat"}:
                quick_reply = get_lanchat_scene_runtime().record_busy_message(
                    agent_name=agent_name,
                    text=str(trigger.get("text") or ""),
                    source_user_id=str(trigger.get("sender_id") or ""),
                )
                if quick_reply:
                    return bool(self._send_final_reply(agent_id, agent_name, quick_reply, trigger))

            if self._async_agent_execution and self._should_send_fast_ack(trigger):
                _send_progress("已收到，我正在整理你的请求。生成尚未开始；需要确认后才会进入生成队列。")

            with agent_progress_sink(_send_progress):
                with self._agent_call_lock:
                    result = self._run_agent(trigger)
        except Exception as exc:
            self._logger.debug("LANChat AI agent failed: %s", exc)
            reply = f"AI agent failed: {exc}"
        else:
            agent_id = result.sender_id
            agent_name = result.sender_name
            reply = result.text
            action_payload = getattr(result, "action_payload", None)
            action_payload = self._prepare_confirmed_action_payload(action_payload, trigger)

        try:
            self._broadcast_confirmed_action(action_payload)
            self._logger.info(
                "[LANChatAgentTrace] phase=process_reply message_id=%s correlation=%s room=%s agent=%s/%s reply_len=%s action=%s status=%s",
                trigger.get("message_id") or "",
                self._correlation_id(trigger),
                trigger.get("room_id") or "",
                agent_id,
                agent_name,
                len(str(reply or "")),
                str((action_payload or {}).get("action_type") or ""),
                str((action_payload or {}).get("status") or ""),
            )
            return bool(
                self._send_final_reply(agent_id, agent_name, str(reply or ""), trigger, action_payload)
            )
        except Exception as exc:
            self._logger.debug("Failed to send LANChat agent reply: %s", exc)
            return False

    def _run(self) -> None:
        while not self._stop_event.is_set():
            processed = self.process_once()
            if not processed:
                self._stop_event.wait(self._sleep_seconds)

    def _send_final_reply(
        self,
        agent_id: str,
        agent_name: str,
        text: str,
        trigger: dict[str, Any],
        action_payload: dict[str, Any] | None = None,
    ) -> bool:
        if action_payload and (
            action_payload.get("status") in {"pending_host_confirmation", "pending"}
            or action_payload.get("requires_host_confirm")
        ):
            proposal_id = str(action_payload.get("proposal_id") or self._correlation_id(trigger))
            metadata = self._sanitize_control_payload(action_payload)
            metadata.setdefault("requires_host_confirm", True)
            if hasattr(self._corona_engine, "network_send_system_message_ex"):
                self._logger.info(
                    "[LANChatReplyTrace] phase=send_system_message_ex message_id=%s correlation=%s proposal=%s agent=%s/%s text_len=%s action=%s status=%s text=%s",
                    trigger.get("message_id") or "",
                    self._correlation_id(trigger),
                    proposal_id,
                    agent_id,
                    agent_name,
                    len(str(text or "")),
                    str((action_payload or {}).get("action_type") or ""),
                    str((action_payload or {}).get("status") or ""),
                    _trace_preview(text),
                )
                return bool(self._corona_engine.network_send_system_message_ex(
                    agent_id,
                    agent_name,
                    self._safe_control_text(text),
                    "gm_proposal",
                    proposal_id,
                    json.dumps(metadata, ensure_ascii=False),
                ))
        if hasattr(self._corona_engine, "network_send_agent_reply_ex"):
            self._logger.info(
                "[LANChatReplyTrace] phase=send_agent_reply_ex message_id=%s correlation=%s reply_to=%s agent=%s/%s text_len=%s text=%s",
                trigger.get("message_id") or "",
                self._correlation_id(trigger),
                trigger.get("message_id") or "",
                agent_id,
                agent_name,
                len(str(text or "")),
                _trace_preview(text),
            )
            return bool(self._corona_engine.network_send_agent_reply_ex(
                agent_id,
                agent_name,
                text,
                "agent_reply",
                agent_id,
                self._correlation_id(trigger),
                json.dumps({"reply_to": str(trigger.get("message_id") or "")}, ensure_ascii=False),
            ))
        self._logger.info(
            "[LANChatReplyTrace] phase=send_agent_reply message_id=%s correlation=%s agent=%s/%s text_len=%s text=%s",
            trigger.get("message_id") or "",
            self._correlation_id(trigger),
            agent_id,
            agent_name,
            len(str(text or "")),
            _trace_preview(text),
        )
        return bool(self._corona_engine.network_send_agent_reply(agent_id, agent_name, text))

    def _remember_room_id(self, room_id: str) -> None:
        room = str(room_id or "").strip()
        if not room:
            return
        if room in self._active_room_ids:
            try:
                self._active_room_order.remove(room)
            except ValueError:
                pass
        self._active_room_ids.add(room)
        self._active_room_order.append(room)
        while len(self._active_room_order) > MAX_ACTIVE_ROOM_IDS:
            oldest = self._active_room_order.popleft()
            self._active_room_ids.discard(oldest)

    def _forget_room_id(self, room_id: str) -> None:
        room = str(room_id or "").strip()
        if not room:
            return
        self._active_room_ids.discard(room)
        try:
            self._active_room_order.remove(room)
        except ValueError:
            pass

    def _remember_coordinator_seen_message_id(self, key: str) -> None:
        normalized = str(key or "").strip()
        if not normalized or normalized in self._coordinator_seen_message_ids:
            return
        self._coordinator_seen_message_ids.add(normalized)
        self._coordinator_seen_message_order.append(normalized)
        while len(self._coordinator_seen_message_order) > MAX_COORDINATOR_SEEN_MESSAGE_IDS:
            oldest = self._coordinator_seen_message_order.popleft()
            self._coordinator_seen_message_ids.discard(oldest)

    def _has_engine_api(self) -> bool:
        return (
            self._corona_engine is not None
            and hasattr(self._corona_engine, "network_pop_lanchat_agent_trigger")
            and hasattr(self._corona_engine, "network_send_agent_reply")
        )

    def _network_session_role_name(self) -> str:
        if self._corona_engine is None:
            return "none"
        session_role_name = getattr(self._corona_engine, "network_session_role_name", None)
        if not callable(session_role_name):
            return "none"
        try:
            return str(session_role_name() or "none").strip().lower()
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("LANChat network role check skipped: %s", exc)
            return "none"

    def _can_execute_agent_locally(self) -> bool:
        return self._network_session_role_name() != "client"

    def _can_execute_generation_locally(self) -> bool:
        return self._can_execute_agent_locally()

    def _get_orchestrator(self) -> LanChatAgentOrchestrator:
        if self._orchestrator is None:
            self._orchestrator = LanChatAgentOrchestrator(
                agent_factory=self._agent_factory or self._default_agent_factory,
            )
        return self._orchestrator

    def _run_agent(self, trigger: dict[str, Any]):
        return self._get_orchestrator().handle_trigger(trigger)

    def _handle_coordinator_gm_control(self, trigger: dict[str, Any]) -> str | None:
        action = self._gm_pace_action_from_trigger(trigger)
        if not action:
            return None
        if self._trusted_host_control(trigger) is False:
            return "【GM】只有房主可以控制生成节奏；该请求没有进入 Coordinator。"
        room_id = str(trigger.get("room_id") or "default")
        self._remember_room_id(room_id)
        try:
            coordinator = self._get_interaction_coordinator()
            disclosure_start = len(coordinator.disclosure_events)
            event = coordinator.control_pace(
                room_id,
                action,
                actor_id=str(trigger.get("sender_id") or trigger.get("agent_id") or "gm"),
                note=str(trigger.get("text") or ""),
            )
            emitted = self._emit_new_disclosure_events(coordinator, disclosure_start)
            self._start_coordinator_disclosure_watch(coordinator, disclosure_start + emitted)
            self._set_runtime_mode_for_pace(action)
            self._emit_generation_scheduler_disclosure()
            return f"【GM】{event.message}"
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Coordinator GM pace control skipped: %s", exc)
            return None

    def _handle_coordinator_gm_clarification(self, trigger: dict[str, Any]) -> str | None:
        question = self._gm_clarification_question_from_trigger(trigger)
        if not question:
            return None
        if self._trusted_host_control(trigger) is False:
            return "【GM】只有房主可以发起强制澄清；该请求没有进入 Coordinator。"
        room_id = str(trigger.get("room_id") or "default")
        self._remember_room_id(room_id)
        try:
            coordinator = self._get_interaction_coordinator()
            disclosure_start = len(coordinator.disclosure_events)
            event = coordinator.request_clarification(
                room_id,
                question,
                requested_by=str(trigger.get("sender_id") or trigger.get("agent_id") or "gm"),
            )
            emitted = self._emit_new_disclosure_events(coordinator, disclosure_start)
            self._start_coordinator_disclosure_watch(coordinator, disclosure_start + emitted)
            return f"【GM】{event.message} {question}"
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Coordinator GM clarification skipped: %s", exc)
            return None

    def _handle_coordinator_status_query(self, trigger: dict[str, Any]) -> str | None:
        text = str(trigger.get("text") or "").strip()
        if not text:
            return None
        message_kind = str(trigger.get("message_kind") or "chat").strip().lower()
        if message_kind not in {"", "chat"}:
            return None
        try:
            coordinator = self._get_interaction_coordinator()
            is_status_query = getattr(coordinator, "_is_status_query", None)
            if not callable(is_status_query) or not is_status_query(text):
                return None
            room_id = str(trigger.get("room_id") or "default")
            self._remember_room_id(room_id)
            event = coordinator.ingest_message(ChatMessage(
                room_id=room_id,
                sender_id=str(trigger.get("sender_id") or trigger.get("from") or ""),
                sender_name=str(trigger.get("sender_name") or trigger.get("from") or ""),
                text=text,
                is_host=bool(trigger.get("is_host") or str(trigger.get("sender_type") or "").lower() == "host"),
                metadata=self._coordinator_sync_metadata(trigger, source="lanchat_agent_trigger"),
            ))
            if getattr(event, "event_type", "") != "status_query":
                return None
            return str(getattr(event, "message", "") or "当前状态暂不可用，请稍后再试。")
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Coordinator status query skipped: %s", exc)
            return None

    def _handle_coordinator_generation_start(self, trigger: dict[str, Any]) -> str | None:
        text = str(trigger.get("text") or "").strip()
        if not text:
            return None
        message_kind = str(trigger.get("message_kind") or "chat").strip().lower()
        if message_kind not in {"", "chat"}:
            return None
        if not self._is_generation_start_text(text):
            return None
        try:
            coordinator = self._get_interaction_coordinator()
            room_id = str(trigger.get("room_id") or "default")
            plan = coordinator.active_plan_for_room(room_id)
            self._logger.info(
                "[LANChatGenerationTrace] phase=trigger_generation_start room=%s sender=%s/%s plan=%s status=%s text=%s",
                room_id,
                trigger.get("sender_id") or trigger.get("from") or "",
                trigger.get("sender_name") or trigger.get("from") or "",
                str(getattr(plan, "plan_id", "") or ""),
                str(getattr(getattr(plan, "status", ""), "value", getattr(plan, "status", "")) or ""),
                _trace_preview(text),
            )
            if plan is None:
                return None
            if plan.status == SeedPlanStatus.CONFIRMED:
                disclosure_start = len(coordinator.disclosure_events)
                self._logger.info(
                    "[LANChatGenerationTrace] phase=execute_confirmed room=%s plan=%s design_len=%s",
                    room_id,
                    plan.plan_id,
                    len(str(getattr(plan, "design_brief", "") or "")),
                )
                ref = coordinator.execute_confirmed_plan(plan.plan_id)
                self._logger.info(
                    "[LANChatGenerationTrace] phase=execute_result room=%s plan=%s job=%s status=%s",
                    room_id,
                    plan.plan_id,
                    getattr(ref, "job_id", ""),
                    getattr(ref, "status", ""),
                )
                emitted = self._emit_new_disclosure_events(coordinator, disclosure_start)
                self._start_coordinator_disclosure_watch(coordinator, disclosure_start + emitted)
                self._emit_generation_scheduler_disclosure()
                return f"【执行结果】SeedPlan {plan.plan_id} 已进入生成队列：{ref.job_id} ({ref.status})"
            if plan.status == SeedPlanStatus.EXECUTING:
                latest_status = coordinator._latest_generation_job_status(plan.plan_id)
                return coordinator._status_query_message(plan, "", latest_status)
            return None
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Coordinator generation start skipped: %s", exc)
            return None

    def _start_active_coordinator_generation(
        self,
        coordinator: InteractionCoordinator,
        *,
        room_id: str,
        host_id: str,
    ) -> str | None:
        if not self._can_execute_generation_locally():
            self._logger.info(
                "[LANChatGenerationTrace] phase=blocked_non_host room=%s host=%s",
                room_id,
                host_id,
            )
            return None
        plan = coordinator.active_plan_for_room(room_id)
        if plan is None:
            self._logger.info(
                "[LANChatGenerationTrace] phase=start_request_no_plan room=%s host=%s",
                room_id,
                host_id,
            )
            return None
        self._logger.info(
            "[LANChatGenerationTrace] phase=start_request room=%s host=%s plan=%s status=%s design_len=%s summary=%s",
            room_id,
            host_id,
            plan.plan_id,
            str(getattr(plan.status, "value", plan.status)),
            len(str(getattr(plan, "design_brief", "") or "")),
            _trace_preview(getattr(plan, "intent_summary", "") or "", 100),
        )
        if plan.status == SeedPlanStatus.EXECUTING:
            latest_status = coordinator._latest_generation_job_status(plan.plan_id)
            return coordinator._status_query_message(plan, "", latest_status)
        if plan.status != SeedPlanStatus.CONFIRMED:
            if plan.status not in {SeedPlanStatus.DRAFT, SeedPlanStatus.CLARIFYING, SeedPlanStatus.PROPOSED}:
                return None
            disclosure_start = len(coordinator.disclosure_events)
            confirmed = coordinator.confirm_seed_plan(plan.plan_id, str(host_id or ""))
            self._logger.info(
                "[LANChatGenerationTrace] phase=confirm_result room=%s host=%s plan=%s ok=%s message=%s payload_plan=%s design_len=%s",
                room_id,
                host_id,
                plan.plan_id,
                bool(getattr(confirmed, "ok", False)),
                _trace_preview(getattr(confirmed, "message", "") or ""),
                str((getattr(confirmed, "payload", {}) or {}).get("plan_id") or ""),
                len(str(getattr(plan, "design_brief", "") or "")),
            )
            emitted = self._emit_new_disclosure_events(coordinator, disclosure_start)
            self._start_coordinator_disclosure_watch(coordinator, disclosure_start + emitted)
            if not getattr(confirmed, "ok", False):
                return str(getattr(confirmed, "message", "") or "当前方案还不能确认生成，请先补充必要信息。")
            plan = coordinator.active_plan_for_room(room_id) or plan
        if plan.status == SeedPlanStatus.CONFIRMED:
            disclosure_start = len(coordinator.disclosure_events)
            self._logger.info(
                "[LANChatGenerationTrace] phase=execute_confirmed room=%s plan=%s design_len=%s",
                room_id,
                plan.plan_id,
                len(str(getattr(plan, "design_brief", "") or "")),
            )
            ref = coordinator.execute_confirmed_plan(plan.plan_id)
            self._logger.info(
                "[LANChatGenerationTrace] phase=execute_result room=%s plan=%s job=%s status=%s",
                room_id,
                plan.plan_id,
                getattr(ref, "job_id", ""),
                getattr(ref, "status", ""),
            )
            emitted = self._emit_new_disclosure_events(coordinator, disclosure_start)
            self._start_coordinator_disclosure_watch(coordinator, disclosure_start + emitted)
            self._emit_generation_scheduler_disclosure()
            return f"【执行结果】SeedPlan {plan.plan_id} 已进入生成队列：{ref.job_id} ({ref.status})"
        return None

    def _send_coordinator_sync_system_reply(self, message: dict[str, Any], text: str) -> bool:
        if self._corona_engine is None:
            return False
        safe_text = self._safe_control_text(text)
        metadata = {
            "reply_to": str(message.get("message_id") or ""),
            "phase": "generation_start",
        }
        try:
            if hasattr(self._corona_engine, "network_send_system_message_ex"):
                return bool(self._corona_engine.network_send_system_message_ex(
                    "system",
                    "系统",
                    safe_text,
                    "action_status",
                    str(message.get("message_id") or ""),
                    json.dumps(metadata, ensure_ascii=False),
                ))
            if hasattr(self._corona_engine, "network_send_system_message"):
                return bool(self._corona_engine.network_send_system_message("system", "系统", safe_text))
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Failed to send Coordinator sync system reply: %s", exc)
        return False

    @staticmethod
    def _is_generation_start_text(text: str) -> bool:
        raw = str(text or "")
        return any(word in raw for word in (
            "确认开始", "确认生成", "确认执行", "直接生成", "开始生成", "开始执行", "执行生成",
            "按照方案执行生成", "按方案执行生成", "就按方案生成", "按这个方案生成",
            "按照方案生成", "就按照这个方案生成", "就按照方案生成", "开始搭建", "开始布置",
        ))

    def _handle_coordinator_completed_intervention(self, trigger: dict[str, Any]) -> str | None:
        text = str(trigger.get("text") or "").strip()
        if not text:
            return None
        message_kind = str(trigger.get("message_kind") or "chat").strip().lower()
        if message_kind not in {"", "chat"}:
            return None
        try:
            coordinator = self._get_interaction_coordinator()
            room_id = str(trigger.get("room_id") or "default")
            plan = coordinator.active_plan_for_room(room_id)
            if plan is None or plan.status != SeedPlanStatus.COMPLETED:
                return None
            if self._is_generation_start_text(text):
                return "当前方案已生成完成；如需继续，请说“添加生成...”或“调整一下布局”。"
            is_status_query = getattr(coordinator, "_is_status_query", None)
            if callable(is_status_query) and is_status_query(text):
                return None
            is_post_adjustment = getattr(coordinator, "_is_post_generation_adjustment", None)
            intent_type = coordinator._intent_type(text)
            if intent_type != "add" and (not callable(is_post_adjustment) or not is_post_adjustment(text)):
                return None
            disclosure_start = len(coordinator.disclosure_events)
            event = coordinator.ingest_message(ChatMessage(
                room_id=room_id,
                sender_id=str(trigger.get("sender_id") or trigger.get("from") or ""),
                sender_name=str(trigger.get("sender_name") or trigger.get("from") or ""),
                text=text,
                is_host=bool(trigger.get("is_host") or str(trigger.get("sender_type") or "").lower() == "host"),
                agent_id=str(trigger.get("agent_id") or ""),
                agent_name=str(trigger.get("agent_name") or ""),
                metadata=self._coordinator_sync_metadata(trigger, source="lanchat_agent_completed_intervention"),
            ))
            self._emit_new_disclosure_events(coordinator, disclosure_start)
            if getattr(event, "event_type", "") in {
                "post_generation_add_routed",
                "final_adjustment_routed",
                "layout_reflow_proposal_created",
                "layout_reflow_confirmed",
                "layout_reflow_rejected",
                "layout_reflow_confirmation_failed",
            }:
                if getattr(event, "event_type", "") == "layout_reflow_proposal_created":
                    return str(getattr(event, "message", "") or "已生成布局调整建议。")
                if getattr(event, "event_type", "") == "layout_reflow_confirmed":
                    executed = self._execute_layout_reflow_confirmation(getattr(event, "payload", {}) or {})
                    base = str(getattr(event, "message", "") or "布局调整建议已确认。").strip()
                    return f"{base}\n{executed}" if executed else base
                if getattr(event, "event_type", "") == "layout_reflow_rejected":
                    return str(getattr(event, "message", "") or "布局调整建议已取消。")
                if getattr(event, "event_type", "") == "layout_reflow_confirmation_failed":
                    return str(getattr(event, "message", "") or "找不到对应布局调整建议。")
                executed = self._try_execute_completed_final_adjustment(event, trigger)
                if executed:
                    base = str(getattr(event, "message", "") or "已记录该调整。").strip()
                    return f"{base}\n{executed}" if base else executed
                return str(getattr(event, "message", "") or "已记录该调整。")
            return None
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Coordinator completed intervention skipped: %s", exc)
            return None

    def _try_execute_completed_final_adjustment(self, event: Any, trigger: dict[str, Any]) -> str:
        if getattr(event, "event_type", "") != "final_adjustment_routed":
            return ""
        text = str(trigger.get("text") or "").strip()
        if not text:
            return ""
        payload = getattr(event, "payload", None)
        payload = payload if isinstance(payload, dict) else {}
        target_hint = str(
            payload.get("actor_id")
            or payload.get("target_actor_id")
            or payload.get("target_hint")
            or ""
        ).strip()
        actor = self._pick_completed_adjustment_actor(text, target_hint)
        if actor is not None:
            changes = self._apply_completed_adjustment_to_actor(actor, text)
            if changes:
                name = str(getattr(actor, "name", "") or target_hint or "目标物体")
                return f"已执行低风险最终调整：{name}，{'；'.join(changes)}。"
        review_changes = self._apply_completed_review_adjustments(event, trigger, text)
        if review_changes:
            return f"已执行低风险最终调整：{'；'.join(review_changes)}。"
        if self._looks_like_review_result_application(text):
            return "已检查最近的审查建议，但没有找到可自动执行的低风险缩放/贴地调整；已保留为人工最终调整记录。"
        return ""

    def _pick_completed_adjustment_actor(self, text: str, target_hint: str = "") -> Any | None:
        actors = self._current_scene_actors()
        if not actors:
            return None
        try:
            from .terrain_component_resolver import canonical_actor_id
        except Exception:  # noqa: BLE001
            canonical_actor_id = lambda value: str(value or "").strip()  # type: ignore
        text_value = str(text or "")
        canonical_target = str(canonical_actor_id(target_hint) or "").strip()
        if canonical_target == "__terrain_boundary" or self._looks_like_boundary_adjustment(text_value):
            for actor in actors:
                name = str(getattr(actor, "name", "") or "")
                if str(canonical_actor_id(name) or "") == "__terrain_boundary":
                    return actor
        target_values = {target_hint, canonical_target}
        target_values = {str(item).strip() for item in target_values if str(item or "").strip()}
        for actor in actors:
            name = str(getattr(actor, "name", "") or "")
            display = self._completed_adjustment_display_name(name)
            canonical = str(canonical_actor_id(name) or "").strip()
            candidates = {name, display, canonical}
            if target_values & {item for item in candidates if item}:
                return actor
            if any(item and item in text_value for item in candidates):
                return actor
        return None

    def _current_scene_actors(self) -> list[Any]:
        getter = getattr(self._corona_engine, "get_scene_actors", None)
        if callable(getter):
            try:
                actors = getter()
                return list(actors or [])
            except Exception as exc:  # noqa: BLE001
                self._logger.debug("Failed to read scene actors from engine helper: %s", exc)
        try:
            from CoronaCore.core.managers import scene_manager
            scene = scene_manager.get("")
            if scene is None:
                routes = scene_manager.list_all()
                scene = scene_manager.get(routes[0]) if routes else None
            if scene is None:
                return []
            get_actors = getattr(scene, "get_actors", None)
            return list(get_actors() or []) if callable(get_actors) else []
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Failed to read scene actors for completed adjustment: %s", exc)
            return []

    def _execute_layout_reflow_confirmation(self, payload: dict[str, Any]) -> str:
        if not isinstance(payload, dict) or str(payload.get("status") or "") != "confirmed":
            return ""
        actors = [actor for actor in self._current_scene_actors() if self._is_layout_reflow_actor(actor)]
        if not actors:
            return "已确认布局调整建议，但当前没有可执行的普通场景物体坐标；请刷新场景物体列表后再试。"
        applied: list[str] = []
        grounded: list[str] = []
        skipped_ground: list[str] = []
        max_targets = min(len(actors), 8)
        for index, actor in enumerate(actors[:max_targets]):
            name = str(getattr(actor, "name", "") or f"物体{index + 1}")
            try:
                current = [float(value) for value in actor.get_position()]
                while len(current) < 3:
                    current.append(0.0)
                side = -1.0 if index % 2 == 0 else 1.0
                row = index // 2
                if index == max_targets - 1 and max_targets >= 4:
                    target = [0.0, current[1], round(2.2 + 0.35 * row, 3)]
                    label = "后方焦点区"
                else:
                    target = [
                        round(side * (1.8 + 0.25 * row), 3),
                        current[1],
                        round(-1.2 + 0.7 * row, 3),
                    ]
                    label = "侧边分区"
                target = self._clamp_layout_reflow_to_room(target, actor)
                if [round(v, 3) for v in current[:3]] != target:
                    actor.set_position(target)
                snapped, reason = self._selective_ground_actor_if_floor_supported(actor)
                if snapped:
                    grounded.append(name)
                elif reason:
                    skipped_ground.append(f"{name}: {reason}")
                final_pos = [round(float(value), 3) for value in actor.get_position()[:3]]
                if [round(v, 3) for v in current[:3]] != final_pos:
                    applied.append(f"{name} -> {label} {final_pos}")
            except Exception as exc:  # noqa: BLE001
                self._logger.debug("Layout reflow actor move skipped for %s: %s", name, exc)
        if not applied:
            return "已确认布局调整建议，但当前没有可安全移动的物体；已保留为最终调整记录。"
        suffix = ""
        if grounded:
            suffix = f" 并已贴地修正地面物体：{'、'.join(grounded[:8])}。"
        elif skipped_ground:
            suffix = " 未发现需要自动贴地的地面物体。"
        return "布局调整完成：" + "；".join(applied[:8]) + "。" + suffix

    def _ground_layout_reflow_position(self, actor: Any, target: list[float]) -> list[float]:
        grounded = [float(value) for value in target[:3]]
        while len(grounded) < 3:
            grounded.append(0.0)
        aabb = self._safe_actor_aabb(actor)
        if aabb and len(aabb) >= 6:
            try:
                current = [float(value) for value in actor.get_position()]
            except Exception:
                current = [grounded[0], grounded[1], grounded[2]]
            while len(current) < 3:
                current.append(0.0)
            min_y = float(aabb[1])
            max_y = float(aabb[4])
            is_world_aabb = min_y - 1e-4 <= current[1] <= max_y + 1e-4
            grounded[1] = current[1] - min_y if is_world_aabb else -min_y
        else:
            grounded[1] = max(0.0, grounded[1])
        grounded[1] = max(0.0, grounded[1])
        return [round(value, 3) for value in grounded[:3]]

    def _clamp_layout_reflow_to_room(self, target: list[float], actor: Any) -> list[float]:
        room_size = self._current_room_box_size()
        if len(room_size) < 3:
            return [round(float(value), 3) for value in target[:3]]
        aabb = self._safe_actor_aabb(actor)
        if aabb and len(aabb) >= 6:
            half_x = max(0.0, (float(aabb[3]) - float(aabb[0])) / 2.0)
            half_z = max(0.0, (float(aabb[5]) - float(aabb[2])) / 2.0)
        else:
            half_x = half_z = 0.25
        margin = 0.18
        width, depth = float(room_size[0]), float(room_size[1])
        min_x = -width / 2.0 + margin + half_x
        max_x = width / 2.0 - margin - half_x
        min_z = -depth / 2.0 + margin + half_z
        max_z = depth / 2.0 - margin - half_z
        out = [float(value) for value in target[:3]]
        if min_x <= max_x:
            out[0] = min(max(out[0], min_x), max_x)
        if min_z <= max_z:
            out[2] = min(max(out[2], min_z), max_z)
        return [round(value, 3) for value in out[:3]]

    def _selective_ground_actor_if_floor_supported(self, actor: Any) -> tuple[bool, str]:
        support_type = self._layout_support_type(actor)
        if support_type == "floor_supported":
            return self._snap_actor_bottom_to_ground(actor)
        if support_type in {"system", "wall_mounted", "ceiling_hung"}:
            return False, f"跳过{support_type}"
        return False, "未知支撑类型，未自动贴地"

    def _snap_actor_bottom_to_ground(
        self,
        actor: Any,
        *,
        ground_y: float = 0.0,
        epsilon: float = 0.03,
    ) -> tuple[bool, str]:
        aabb = self._safe_actor_aabb(actor)
        if not aabb or len(aabb) < 6:
            return False, "AABB不可读"
        try:
            current = [float(value) for value in actor.get_position()]
        except Exception:
            return False, "位置不可读"
        while len(current) < 3:
            current.append(0.0)
        bottom_y = float(aabb[1])
        delta = bottom_y - float(ground_y)
        if abs(delta) <= float(epsilon):
            return False, "已贴地"
        current[1] = current[1] - delta
        actor.set_position([round(value, 3) for value in current[:3]])
        return True, "已贴地"

    @staticmethod
    def _layout_support_type(actor: Any) -> str:
        name = str(getattr(actor, "name", "") or "").strip()
        lowered = name.lower()
        if not name:
            return "unknown"
        if (
            lowered.startswith("__room")
            or lowered.startswith("__terrain")
            or lowered.startswith("_terrain")
            or lowered in {"terrain", "ground", "sky", "room_box", "__room_box", "__room_terrain"}
            or any(term in name for term in ("地形", "天空", "边界"))
        ):
            return "system"

        ceiling_terms = (
            "吊灯", "吊旗", "吊笼", "悬挂", "铁链", "天花", "ceiling", "chandelier", "hanging",
        )
        if any(term in lowered or term in name for term in ceiling_terms):
            return "ceiling_hung"

        wall_terms = (
            "火把", "壁灯", "墙灯", "墙饰", "地图", "旗帜", "窗", "门", "招牌", "武器架",
            "torch", "sconce", "wall", "map", "flag", "window", "door", "sign", "weapon rack",
        )
        if any(term in lowered or term in name for term in wall_terms):
            return "wall_mounted"

        floor_terms = (
            "桌", "椅", "箱", "宝箱", "金币", "木桶", "酒桶", "麻袋", "床", "柜", "地毯",
            "雕像", "动物", "长椅", "沙发", "桶", "袋",
            "table", "chair", "box", "chest", "coin", "barrel", "sack", "bed", "cabinet",
            "rug", "carpet", "statue", "animal", "bench", "sofa",
        )
        if any(term in lowered or term in name for term in floor_terms):
            return "floor_supported"
        return "unknown"

    def _current_room_box_size(self) -> list[float]:
        for actor in self._current_scene_actors():
            name = str(getattr(actor, "name", "") or "").lower()
            if name not in {"__room_box", "room_box"}:
                continue
            try:
                scale = [float(value) for value in actor.get_scale()]
                if len(scale) >= 3:
                    return [abs(scale[0]), abs(scale[2]), abs(scale[1])]
            except Exception:
                pass
        return []

    @staticmethod
    def _safe_actor_aabb(actor: Any) -> list[float]:
        getter = getattr(actor, "get_aabb", None)
        if not callable(getter):
            getter = getattr(actor, "get_bounding_box", None)
        if not callable(getter):
            return []
        try:
            raw = getter()
        except Exception:
            return []
        if isinstance(raw, dict):
            values = raw.get("aabb") or raw.get("bounds") or raw.get("box")
        else:
            values = raw
        if not isinstance(values, (list, tuple)) or len(values) < 6:
            return []
        try:
            return [float(value) for value in values[:6]]
        except Exception:
            return []

    @staticmethod
    def _is_layout_reflow_actor(actor: Any) -> bool:
        name = str(getattr(actor, "name", "") or "")
        if not name:
            return False
        lowered = name.lower()
        if lowered.startswith("__room") or lowered.startswith("__terrain") or lowered.startswith("_terrain"):
            return False
        if lowered in {"terrain", "ground", "sky", "room_box"}:
            return False
        return callable(getattr(actor, "get_position", None)) and callable(getattr(actor, "set_position", None))

    def _apply_completed_review_adjustments(
        self,
        event: Any,
        trigger: dict[str, Any],
        text: str,
    ) -> list[str]:
        if not self._looks_like_review_result_application(text):
            return []
        payload = getattr(event, "payload", None)
        payload = payload if isinstance(payload, dict) else {}
        plan_id = str(payload.get("plan_id") or "").strip()
        try:
            coordinator = self._get_interaction_coordinator()
            if not plan_id:
                room_id = str(trigger.get("room_id") or payload.get("room_id") or "default")
                plan = coordinator.active_plan_for_room(room_id)
                plan_id = str(getattr(plan, "plan_id", "") or "").strip()
            if not plan_id:
                return []
            pending = list(coordinator.pending_interventions(plan_id))
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Completed review adjustment lookup failed: %s", exc)
            return []
        changes: list[str] = []
        for intervention in reversed(pending):
            details = getattr(intervention, "finding_details", None)
            if not isinstance(details, list) or not details:
                continue
            route = str(getattr(intervention, "apply_policy", "") or "")
            intent = str(getattr(intervention, "intent_type", "") or "")
            if route != "final_adjustment" and "review" not in intent:
                continue
            for detail in details[:8]:
                if not isinstance(detail, dict):
                    continue
                actor_hint = self._review_detail_actor_hint(detail)
                advice_text = self._review_detail_adjustment_text(detail)
                if not actor_hint and not advice_text:
                    continue
                actor = self._pick_completed_adjustment_actor(advice_text or text, actor_hint)
                if actor is None:
                    continue
                actor_changes = self._apply_completed_review_detail_to_actor(actor, detail, advice_text)
                if actor_changes:
                    name = str(getattr(actor, "name", "") or actor_hint or "目标物体")
                    changes.append(f"{name}，{'、'.join(actor_changes)}")
            if changes:
                break
        return changes

    @staticmethod
    def _looks_like_review_result_application(text: str) -> bool:
        raw = str(text or "")
        review_words = ("审查", "检查", "外观", "VLM", "vlm", "建议", "结果", "参考", "参照", "参茶")
        action_words = ("按", "根据", "应用", "执行", "处理", "调整", "摆放", "修正", "优化")
        if any(word in raw for word in review_words) and any(word in raw for word in action_words):
            return True
        return (
            any(word in raw for word in ("摆放", "布局", "大小", "尺寸", "比例"))
            and any(word in raw for word in ("问题", "不对", "不合理", "有问题"))
        )

    @staticmethod
    def _review_detail_actor_hint(detail: dict[str, Any]) -> str:
        for key in ("actor_id", "target_actor_id", "object_id", "target_object_id", "target", "target_hint"):
            value = detail.get(key)
            if value:
                return str(value).strip()
        return ""

    @staticmethod
    def _review_detail_adjustment_text(detail: dict[str, Any]) -> str:
        parts: list[str] = []
        for key in ("fix_suggestion", "suggestion", "message", "overall"):
            value = str(detail.get(key) or "").strip()
            if value:
                parts.append(value)
        issues = detail.get("issues")
        if isinstance(issues, list):
            parts.extend(str(item or "").strip() for item in issues if str(item or "").strip())
        return "；".join(parts)

    def _apply_completed_review_detail_to_actor(
        self,
        actor: Any,
        detail: dict[str, Any],
        advice_text: str,
    ) -> list[str]:
        changes: list[str] = []
        scale_vector = detail.get("scale_correction")
        if isinstance(scale_vector, list) and len(scale_vector) >= 3:
            try:
                factors = [float(value) for value in scale_vector[:3]]
                if any(abs(value - 1.0) > 1e-3 for value in factors):
                    current = [float(v) for v in actor.get_scale()]
                    while len(current) < 3:
                        current.append(1.0)
                    new_scale = [
                        round(max(0.02, min(20.0, current[index] * factors[index])), 4)
                        for index in range(3)
                    ]
                    actor.set_scale(new_scale)
                    changes.append(f"缩放调整为 {new_scale}")
            except Exception as exc:  # noqa: BLE001
                self._logger.debug("Completed VLM review scale vector adjustment failed: %s", exc)
        text_changes = self._apply_completed_adjustment_to_actor(actor, advice_text)
        for item in text_changes:
            if item not in changes:
                changes.append(item)
        return changes

    @staticmethod
    def _completed_adjustment_display_name(name: str) -> str:
        display = str(name or "")
        for prefix in ("__shell_", "__asset_"):
            if display.startswith(prefix):
                return display[len(prefix):]
        return display

    @staticmethod
    def _looks_like_boundary_adjustment(text: str) -> bool:
        return any(token in str(text or "") for token in (
            "_terrain_boundary",
            "__terrain_boundary",
            "terrain_boundary",
            "地形边界",
            "场地边界",
            "边界",
            "栅栏",
            "围栏",
        ))

    def _apply_completed_adjustment_to_actor(self, actor: Any, text: str) -> list[str]:
        try:
            from .terrain_component_resolver import canonical_actor_id
        except Exception:  # noqa: BLE001
            canonical_actor_id = lambda value: str(value or "").strip()  # type: ignore
        name = str(getattr(actor, "name", "") or "")
        canonical = str(canonical_actor_id(name) or "").strip()
        changes: list[str] = []
        raw = str(text or "")
        if canonical == "__terrain_boundary":
            changes.extend(self._apply_completed_boundary_adjustment(actor, raw))
        scale_factor = self._completed_adjustment_scale_factor(raw)
        if scale_factor is not None and canonical != "__terrain_boundary":
            try:
                current = [float(v) for v in actor.get_scale()]
                while len(current) < 3:
                    current.append(1.0)
                new_scale = [round(max(0.02, value * scale_factor), 4) for value in current[:3]]
                actor.set_scale(new_scale)
                changes.append(f"缩放调整为 {new_scale}")
            except Exception as exc:  # noqa: BLE001
                self._logger.debug("Completed final adjustment scale failed: %s", exc)
        if any(word in raw for word in ("贴地", "落地", "悬空", "浮空", "飘起", "飘起来", "离地", "没贴地", "穿模", "接地")):
            try:
                current = [float(v) for v in actor.get_position()]
                while len(current) < 3:
                    current.append(0.0)
                grounded = self._ground_layout_reflow_position(actor, current)
                if [round(v, 3) for v in current[:3]] != grounded:
                    actor.set_position(grounded)
                    changes.append("已校正贴地高度")
            except Exception as exc:  # noqa: BLE001
                self._logger.debug("Completed final adjustment grounding failed: %s", exc)
        return changes

    def _apply_completed_boundary_adjustment(self, actor: Any, text: str) -> list[str]:
        changes: list[str] = []
        if any(word in text for word in ("低矮", "矮一点", "太高", "别太高", "奇怪", "不自然", "藤蔓", "木栏", "围栏", "栅栏")):
            try:
                current = [float(v) for v in actor.get_scale()]
                while len(current) < 3:
                    current.append(1.0)
                new_scale = [
                    round(max(0.02, current[0]), 4),
                    round(min(max(0.02, current[1]), 0.55), 4),
                    round(max(0.02, current[2]), 4),
                ]
                actor.set_scale(new_scale)
                changes.append(f"边界高度调整为 {new_scale}")
            except Exception as exc:  # noqa: BLE001
                self._logger.debug("Completed boundary scale adjustment failed: %s", exc)
        if any(word in text for word in ("藤蔓", "木栏", "木质", "温暖", "自然")):
            rgb = [0.34, 0.45, 0.18] if "藤蔓" in text else [0.42, 0.25, 0.12]
            if self._try_completed_actor_color(actor, rgb):
                changes.append("边界颜色调整为自然木藤色")
        return changes

    @staticmethod
    def _completed_adjustment_scale_factor(text: str) -> float | None:
        numeric = re.search(r"([0-9]+(?:\.[0-9]+)?)\s*倍", str(text or ""))
        if numeric and any(word in text for word in ("放大", "变大", "扩大")):
            return max(0.05, float(numeric.group(1)))
        if numeric and any(word in text for word in ("缩小", "变小")):
            return max(0.05, 1.0 / max(0.05, float(numeric.group(1))))
        if "一半" in text and any(word in text for word in ("缩小", "变小")):
            return 0.5
        if any(word in text for word in ("太大", "过大", "偏大", "尺寸大", "比例大")):
            return 0.8
        if any(word in text for word in ("太小", "过小", "偏小", "尺寸小", "比例小")):
            return 1.2
        if any(word in text for word in ("大一点", "变大", "放大")):
            return 1.35
        if any(word in text for word in ("小一点", "变小", "缩小")):
            return 0.75
        return None

    @staticmethod
    def _try_completed_actor_color(actor: Any, rgb: list[float]) -> bool:
        candidates = [
            getattr(actor, "set_color", None),
            getattr(actor, "set_diffuse", None),
        ]
        optics = getattr(actor, "_optics", None)
        if optics is not None:
            candidates.extend([
                getattr(optics, "set_color", None),
                getattr(optics, "set_diffuse", None),
                getattr(optics, "set_base_color", None),
            ])
        for setter in candidates:
            if not callable(setter):
                continue
            try:
                setter(rgb)
                return True
            except TypeError:
                try:
                    setter(float(rgb[0]), float(rgb[1]), float(rgb[2]))
                    return True
                except Exception:
                    continue
            except Exception:
                continue
        return False

    @staticmethod
    def _gm_pace_action_from_trigger(trigger: dict[str, Any]) -> str:
        agent_id = str(trigger.get("agent_id") or trigger.get("target_agent_id") or "").lower()
        agent_name = str(trigger.get("agent_name") or "").lower()
        text = str(trigger.get("text") or "").strip()
        if not (agent_id == "gm" or agent_name in {"gm", "主持人", "裁判", "game master"} or text.startswith("@GM")):
            return ""
        if re.search(r"\b(?:gm-\d+|fa-[\w.-]+|cr-[\w.-]+)\b", text, flags=re.I):
            return ""
        if any(word in text for word in ("暂停", "先停", "等一下")):
            return "pause"
        if any(word in text for word in ("继续", "恢复")):
            return "resume"
        if any(word in text for word in ("先讨论", "不要生成", "别生成", "先规划")):
            return "discuss"
        return ""

    @staticmethod
    def _gm_clarification_question_from_trigger(trigger: dict[str, Any]) -> str:
        agent_id = str(trigger.get("agent_id") or trigger.get("target_agent_id") or "").lower()
        agent_name = str(trigger.get("agent_name") or "").lower()
        text = str(trigger.get("text") or "").strip()
        if not (agent_id == "gm" or agent_name in {"gm", "主持人", "裁判", "game master"} or text.startswith("@GM")):
            return ""
        if re.search(r"\b(?:gm-\d+|fa-[\w.-]+|cr-[\w.-]+)\b", text, flags=re.I):
            return ""
        if not any(word in text for word in ("澄清", "问清楚", "问一下", "不明确", "需要补充", "补充需求")):
            return ""
        question = re.sub(r"^@GM\s*", "", text, flags=re.I).strip()
        return question or "请补充关键需求。"

    @classmethod
    def _trusted_host_control(cls, trigger: dict[str, Any]) -> bool | None:
        metadata = cls._metadata_from_trigger(trigger)
        view = {**metadata, **(trigger or {})}
        for key in ("sender_role", "room_role", "role"):
            if key not in view:
                continue
            role = str(view.get(key) or "").strip().lower()
            if role:
                return role in {"host", "owner", "room_host", "房主"}
        for key in ("is_host", "is_room_host", "sender_is_host"):
            if key in view:
                return bool(view.get(key))
        return None

    @staticmethod
    def _metadata_from_trigger(trigger: dict[str, Any]) -> dict[str, Any]:
        metadata = (trigger or {}).get("metadata")
        if isinstance(metadata, dict):
            return metadata
        raw = (trigger or {}).get("metadata_json")
        if not raw:
            return {}
        if isinstance(raw, dict):
            return raw
        try:
            parsed = json.loads(str(raw))
        except Exception:
            return {}
        return parsed if isinstance(parsed, dict) else {}

    def _coordinator_sync_metadata(self, message: dict[str, Any], *, source: str) -> dict[str, Any]:
        metadata: dict[str, Any] = {
            "message_id": str(message.get("message_id") or ""),
            "source": source,
        }
        raw_metadata = self._metadata_from_trigger(message)
        for key in (
            "actor_id",
            "target_actor_id",
            "object_id",
            "target_object_id",
            "actor_version",
            "target_hint",
            "workspace_mode",
            "draft_action",
            "target_agent_id",
            "target_agent_name",
            "target_agent_ids",
            "target_agent_names",
            "target_plan_id",
            "target_scope",
        ):
            value = raw_metadata.get(key)
            if value is not None and value != "":
                metadata[key] = value
        for key in ("source_user_id", "correlation_id"):
            value = message.get(key)
            if value:
                metadata[key] = str(value)
        return metadata

    def _normalize_coordinator_target_metadata(
        self,
        message: dict[str, Any],
        text: str,
        metadata: dict[str, Any],
    ) -> dict[str, Any]:
        normalized = dict(metadata or {})
        mention = self._explicit_agent_mention(text)
        if mention:
            agent_id, agent_name = self._resolve_lanchat_agent_mention(mention)
            normalized["target_scope"] = "agent"
            normalized["target_agent_name"] = agent_name or mention
            normalized["target_agent_id"] = agent_id or str(message.get("target_agent_id") or message.get("agent_id") or agent_name or mention)
        elif str(message.get("target_agent_id") or "").strip() or str(message.get("target_agent_name") or "").strip():
            normalized.setdefault("target_scope", "agent")
            normalized["target_agent_id"] = str(message.get("target_agent_id") or "").strip()
            normalized["target_agent_name"] = str(message.get("target_agent_name") or "").strip()
        source_context = self._source_context_agent_from_text(text)
        if source_context and source_context != str(normalized.get("target_agent_name") or ""):
            normalized["source_context_agent"] = source_context
        return normalized

    @staticmethod
    def _explicit_agent_mention(text: str) -> str:
        match = re.search(r"@([^\s，,。；;：:]+)", str(text or ""))
        return match.group(1).strip() if match else ""

    @staticmethod
    def _source_context_agent_from_text(text: str) -> str:
        raw = str(text or "")
        match = re.search(r"在\s*([^，,。；;\s@]+)\s*方案基础上", raw)
        if match:
            return match.group(1).strip()
        match = re.search(r"基于\s*([^，,。；;\s@]+)\s*方案", raw)
        return match.group(1).strip() if match else ""

    def _resolve_lanchat_agent_mention(self, mention: str) -> tuple[str, str]:
        wanted = str(mention or "").strip()
        if not wanted:
            return "", ""
        roster = []
        getter = getattr(self._corona_engine, "network_lanchat_agents_snapshot", None)
        if callable(getter):
            try:
                roster = list(getter() or [])
            except Exception as exc:  # noqa: BLE001
                self._logger.debug("Failed to read LANChat agent roster: %s", exc)
        for item in roster:
            if not isinstance(item, dict):
                continue
            agent_id = str(item.get("agent_id") or item.get("id") or "").strip()
            agent_name = str(item.get("name") or item.get("agent_name") or "").strip()
            if wanted in {agent_id, agent_name}:
                return agent_id, agent_name
        return "", wanted

    def _apply_generation_options_from_message(self, message: dict[str, Any]) -> None:
        metadata = self._metadata_from_trigger(message)
        options = metadata.get("generation_options") if isinstance(metadata, dict) else None
        if not isinstance(options, dict):
            return
        is_host = bool(
            message.get("is_host")
            or str(message.get("sender_type") or "").lower() == "host"
            or metadata.get("is_host")
            or str(metadata.get("sender_role") or "").lower() == "host"
        )
        if not is_host:
            return
        enabled = bool(options.get("vlm_enabled"))
        raw_targets = options.get("vlm_max_targets", 1 if enabled else 0)
        try:
            targets = int(raw_targets)
        except Exception:
            targets = 1 if enabled else 0
        targets = max(0, min(4, targets))
        if enabled and targets <= 0:
            targets = 1
        os.environ["PROGRESSIVE_VLM_MAX_TARGETS"] = str(targets if enabled else 0)
        self._logger.info(
            "LANChat generation option updated: PROGRESSIVE_VLM_MAX_TARGETS=%s",
            os.environ["PROGRESSIVE_VLM_MAX_TARGETS"],
        )

    @staticmethod
    def _coordinator_sync_dedupe_key(message: dict[str, Any], *, source: str) -> str:
        message_id = str(message.get("message_id") or "").strip()
        if message_id:
            return f"id:{message_id}"
        text = str(message.get("text") or "").strip()
        if not text:
            return ""
        parts = (
            "fallback",
            str(source or "lanchat_direct").strip(),
            str(message.get("room_id") or "default").strip(),
            str(message.get("sender_id") or message.get("from") or "").strip(),
            str(message.get("sender_type") or "user").strip().lower(),
            str(message.get("message_kind") or "chat").strip().lower(),
            text,
        )
        return "|".join(parts)

    def _set_runtime_mode_for_pace(self, action: str) -> None:
        mode = {"pause": "PAUSED", "resume": "EXECUTING", "discuss": "DISCUSSING"}.get(action)
        if not mode:
            return
        try:
            from .lanchat_scene_runtime import get_lanchat_scene_runtime
            get_lanchat_scene_runtime().set_mode(mode)
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("LANChat scene runtime pace update skipped: %s", exc)

    def _sync_trigger_history_to_coordinator(self, trigger: dict[str, Any]) -> None:
        history = trigger.get("history") or []
        if not isinstance(history, list):
            return
        room_id = str(trigger.get("room_id") or "default")
        self._remember_room_id(room_id)
        current_message_id = str(trigger.get("message_id") or "")
        for item in history:
            if not isinstance(item, dict):
                continue
            message_id = str(item.get("message_id") or "")
            if not message_id or message_id == current_message_id:
                continue
            if message_id in self._coordinator_seen_message_ids:
                continue
            payload = dict(item)
            payload["room_id"] = str(payload.get("room_id") or room_id)
            self.sync_chat_message_to_coordinator(
                payload,
                source="lanchat_history_snapshot",
                emit_disclosure=False,
            )

    def _broadcast_confirmed_action(self, payload: dict[str, Any] | None) -> None:
        if not payload:
            return
        if str(payload.get("action_type") or "") == "final_adjustment_confirmation":
            self._record_final_adjustment_confirmation(payload)
            return
        if str(payload.get("action_type") or "") == "conflict_resolution_confirmation":
            self._record_conflict_resolution_confirmation(payload)
            return
        if payload.get("status") != "confirmed":
            return
        if str(payload.get("action_type") or "") == "discussion_only":
            return
        if hasattr(self._corona_engine, "network_broadcast_intent"):
            source_user_id = str(payload.get("source_user_id") or "unknown")
            tooltip = self._safe_control_text(payload.get("intent_text") or payload.get("proposal_id") or "")
            try:
                self._corona_engine.network_broadcast_intent(
                    source_user_id,
                    tooltip,
                    [0.0, 0.0, 0.0],
                    "confirmed_gm_action",
                )
            except Exception as exc:
                self._logger.debug("Failed to broadcast confirmed GM action: %s", exc)

        self._execute_confirmed_action(payload)

    @classmethod
    def _sanitize_control_payload(cls, value: Any) -> Any:
        if isinstance(value, dict):
            sanitized: dict[str, Any] = {}
            for key, item in value.items():
                normalized = str(key or "").lower()
                if any(marker in normalized for marker in _SENSITIVE_WORKER_PAYLOAD_KEYS):
                    continue
                sanitized[key] = cls._sanitize_control_payload(item)
            return sanitized
        if isinstance(value, list):
            return [cls._sanitize_control_payload(item) for item in value]
        if isinstance(value, tuple):
            return [cls._sanitize_control_payload(item) for item in value]
        if isinstance(value, str):
            return cls._safe_control_text(value)
        return value

    @staticmethod
    def _safe_control_text(value: Any) -> str:
        text = str(value or "")
        lower = text.lower()
        cut_points = [
            lower.find(marker)
            for marker in _SENSITIVE_WORKER_TEXT_MARKERS
            if lower.find(marker) >= 0
        ]
        if not cut_points:
            return text
        first = min(cut_points)
        keep = text[:first].strip(" \t\r\n,;；。")
        return keep or "已收到控制动作，内部执行细节已隐藏。"

    def _record_final_adjustment_confirmation(self, payload: dict[str, Any]) -> None:
        coordinator = self._interaction_coordinator
        if coordinator is None:
            return
        proposal_id = str(payload.get("proposal_id") or "").strip()
        decision = str(payload.get("decision") or "confirm").strip().lower()
        host_id = str(payload.get("source_user_id") or payload.get("confirmed_by") or "").strip()
        disclosure_start = len(coordinator.disclosure_events)
        confirm = getattr(coordinator, "confirm_final_adjustment_conflict", None)
        if not callable(confirm):
            return
        try:
            confirm(proposal_id, host_id, decision=decision)
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Failed to record final adjustment confirmation: %s", exc)
            return
        emitted = self._emit_new_disclosure_events(coordinator, disclosure_start)
        self._start_coordinator_disclosure_watch(coordinator, disclosure_start + emitted)

    def _record_conflict_resolution_confirmation(self, payload: dict[str, Any]) -> None:
        coordinator = self._interaction_coordinator
        if coordinator is None:
            return
        proposal_id = str(payload.get("proposal_id") or "").strip()
        decision = str(payload.get("decision") or "confirm").strip().lower()
        host_id = str(payload.get("source_user_id") or payload.get("confirmed_by") or "").strip()
        disclosure_start = len(coordinator.disclosure_events)
        handler_name = "reject_conflict_resolution" if decision in {"reject", "rejected", "no", "cancel"} else "confirm_conflict_resolution"
        handler = getattr(coordinator, handler_name, None)
        if not callable(handler):
            return
        try:
            handler(proposal_id, host_id)
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Failed to record conflict resolution confirmation: %s", exc)
            return
        emitted = self._emit_new_disclosure_events(coordinator, disclosure_start)
        self._start_coordinator_disclosure_watch(coordinator, disclosure_start + emitted)

    def _execute_confirmed_action(self, payload: dict[str, Any]) -> None:
        executor = self._get_host_action_executor()
        if executor is None or not hasattr(executor, "enqueue_and_process"):
            return
        coordinator = self._interaction_coordinator
        disclosure_start = len(coordinator.disclosure_events) if coordinator is not None else 0
        try:
            executor.enqueue_and_process(payload)
        except Exception as exc:
            self._logger.debug("Failed to execute confirmed GM action: %s", exc)
        finally:
            if coordinator is not None:
                emitted = self._emit_new_disclosure_events(coordinator, disclosure_start)
                self._start_coordinator_disclosure_watch(coordinator, disclosure_start + emitted)
            self._emit_generation_scheduler_disclosure()

    def _emit_new_disclosure_events(self, coordinator: InteractionCoordinator, start_index: int) -> int:
        if self._corona_engine is None:
            return 0
        if hasattr(coordinator, "disclosure_events_since"):
            events, cursor_advance = coordinator.disclosure_events_since(start_index)
        else:
            events = coordinator.disclosure_events[start_index:]
            cursor_advance = len(events)
        if not events:
            return cursor_advance
        for event in events:
            if getattr(event, "audience", "") not in {"participant", "host"}:
                continue
            payload = event.as_dict()
            text = self._broadcast_text_for_disclosure(payload)
            if not text:
                continue
            if self._try_send_targeted_host_disclosure(payload, text):
                continue
            metadata_payload = payload
            metadata_envelope = {"disclosure": metadata_payload}
            if str(payload.get("audience") or "") == "host":
                metadata_payload = self._host_disclosure_broadcast_payload(payload, text)
                metadata_envelope = {
                    "disclosure": metadata_payload,
                    "host_disclosure": self._host_disclosure_fallback_payload(payload, text),
                }
            metadata = json.dumps(metadata_envelope, ensure_ascii=False)
            try:
                if hasattr(self._corona_engine, "network_send_system_message_ex"):
                    self._corona_engine.network_send_system_message_ex(
                        "system",
                        "系统",
                        text,
                        "action_status",
                        str(payload.get("event_id") or ""),
                        metadata,
                    )
                elif hasattr(self._corona_engine, "network_send_system_message"):
                    self._corona_engine.network_send_system_message("system", "系统", text)
            except Exception as exc:  # noqa: BLE001
                self._logger.debug("Failed to emit LANChat disclosure event: %s", exc)
        return cursor_advance

    def _try_send_targeted_host_disclosure(self, payload: dict[str, Any], text: str) -> bool:
        if str(payload.get("audience") or "") != "host":
            return False
        target_sender_id = str(
            payload.get("target_user_id")
            or (payload.get("metadata") or {}).get("target_user_id")
            or "host"
        )
        metadata = json.dumps({"disclosure": payload}, ensure_ascii=False)
        for method_name in (
            "network_send_system_message_to_host_ex",
            "network_send_system_message_to_user_ex",
        ):
            sender = getattr(self._corona_engine, method_name, None)
            if not callable(sender):
                continue
            try:
                if method_name.endswith("_to_user_ex"):
                    sender(
                        target_sender_id,
                        "system",
                        "系统",
                        text,
                        "action_status",
                        str(payload.get("event_id") or ""),
                        metadata,
                    )
                else:
                    sender(
                        "system",
                        "系统",
                        text,
                        "action_status",
                        str(payload.get("event_id") or ""),
                        metadata,
                    )
                return True
            except Exception as exc:  # noqa: BLE001
                self._logger.debug("Failed to emit targeted host disclosure via %s: %s", method_name, exc)
        return False

    @staticmethod
    def _host_disclosure_broadcast_payload(payload: dict[str, Any], text: str) -> dict[str, Any]:
        metadata = payload.get("metadata") if isinstance(payload.get("metadata"), dict) else {}
        safe_metadata = {
            key: metadata.get(key)
            for key in ("proposal_id", "requires_conflict_resolution", "requires_confirmation")
            if key in metadata
        }
        return {
            "event_id": payload.get("event_id"),
            "room_id": payload.get("room_id"),
            "audience": "participant",
            "stage": payload.get("stage"),
            "progress": payload.get("progress"),
            "public_message": text,
            "available_actions": [],
            "requires_confirmation": False,
            "metadata": safe_metadata,
        }

    @staticmethod
    def _host_disclosure_fallback_payload(payload: dict[str, Any], text: str) -> dict[str, Any]:
        metadata = payload.get("metadata") if isinstance(payload.get("metadata"), dict) else {}
        intervention = metadata.get("intervention") if isinstance(metadata.get("intervention"), dict) else {}
        proposal_id = (
            payload.get("proposal_id")
            or metadata.get("proposal_id")
            or intervention.get("proposal_id")
            or ""
        )
        safe_metadata = {
            key: metadata.get(key)
            for key in ("proposal_id", "requires_conflict_resolution", "requires_confirmation", "apply_policy")
            if key in metadata
        }
        if intervention:
            safe_metadata["intervention"] = {
                key: intervention.get(key)
                for key in ("proposal_id", "requires_conflict_resolution", "apply_policy", "intent_type")
                if key in intervention
            }
        available_actions = payload.get("available_actions")
        return {
            "event_id": payload.get("event_id"),
            "room_id": payload.get("room_id"),
            "audience": "host",
            "stage": payload.get("stage"),
            "progress": payload.get("progress"),
            "public_message": payload.get("public_message") or text,
            "available_actions": list(available_actions) if isinstance(available_actions, list) else [],
            "requires_confirmation": bool(payload.get("requires_confirmation")),
            "requires_conflict_resolution": bool(
                payload.get("requires_conflict_resolution")
                or metadata.get("requires_conflict_resolution")
                or intervention.get("requires_conflict_resolution")
            ),
            "proposal_id": proposal_id,
            "metadata": safe_metadata,
            "created_at": payload.get("created_at"),
        }

    def _start_coordinator_disclosure_watch(
        self,
        coordinator: InteractionCoordinator,
        start_index: int,
        *,
        duration_seconds: float = 30.0,
        interval_seconds: float = 0.05,
    ) -> None:
        if self._corona_engine is None:
            return

        def _watch() -> None:
            cursor = int(start_index)
            deadline = time.time() + max(0.1, float(duration_seconds))
            while not self._stop_event.is_set() and time.time() < deadline:
                emitted = self._emit_new_disclosure_events(coordinator, cursor)
                if emitted:
                    cursor += emitted
                time.sleep(max(0.01, float(interval_seconds)))

        threading.Thread(
            target=_watch,
            name="LANChatDisclosureWatch",
            daemon=True,
        ).start()

    @staticmethod
    def _broadcast_text_for_disclosure(payload: dict[str, Any]) -> str:
        """Return text safe for a room-wide system message."""
        audience = str(payload.get("audience") or "")
        if audience == "host":
            if payload.get("requires_confirmation"):
                return "有一项需要房主确认的事项。"
            return "有一项仅房主可见的进度更新。"
        return str(payload.get("public_message") or "")

    def _emit_generation_scheduler_disclosure(self) -> None:
        if self._corona_engine is None:
            return
        room_ids = sorted(self._active_room_ids)
        snapshots: list[tuple[str, dict[str, Any]]] = []
        if room_ids:
            for room_id in room_ids:
                snapshot = self.generation_scheduler_session_snapshot(room_id)
                if snapshot.get("available"):
                    snapshots.append((room_id, snapshot))
        else:
            snapshot = self.generation_scheduler_snapshot()
            if snapshot.get("available"):
                snapshots.append(("", snapshot))
        for room_id, snapshot in snapshots:
            self._emit_generation_scheduler_snapshot_disclosure(room_id, snapshot)

    def _emit_generation_scheduler_snapshot_disclosure(self, room_id: str, snapshot: dict[str, Any]) -> None:
        queued_count = int(snapshot.get("queued_count") or 0)
        total_jobs = int(snapshot.get("total_jobs") or 0)
        active_count = int(snapshot.get("active_count") or len(snapshot.get("active_jobs") or []))
        paused_sessions = list(snapshot.get("paused_sessions") or [])
        paused_session_count = int(snapshot.get("paused_session_count") or len(paused_sessions))
        queue_pressure = float(snapshot.get("queue_pressure") or 0.0)
        diagnosis = snapshot.get("diagnosis") if isinstance(snapshot.get("diagnosis"), dict) else {}
        if queued_count <= 0 and active_count <= 0 and paused_session_count <= 0:
            return
        progress = max(0, min(100, int(round(queue_pressure * 100))))
        if paused_session_count > 0:
            public_message = "后续生成已暂停，等待房主或 GM 确认后继续。"
            available_actions = ["continue_generation", "add_note"]
        elif queue_pressure >= 1.0:
            public_message = "生成队列已满，新的生成请求会先等待当前批次释放资源。"
            available_actions = ["pause_after_batch", "add_note"]
        elif queued_count > 0:
            public_message = "生成任务已进入队列，系统会按批次和优先级继续处理。"
            available_actions = ["add_note", "pause_after_batch"]
        else:
            public_message = "生成任务正在执行，当前阶段会持续更新。"
            available_actions = ["add_note"]
        metadata = {
            "disclosure": {
                "event_id": f"scheduler-{room_id or 'global'}-{int(time.time() * 1000)}",
                "room_id": room_id,
                "audience": "participant",
                "stage": "资源调度",
                "progress": progress,
                "public_message": public_message,
                "available_actions": available_actions,
                "requires_confirmation": False,
                "metadata": {
                    "queue_pressure": queue_pressure,
                    "queued_count": queued_count,
                    "active_count": active_count,
                    "paused_session_count": paused_session_count,
                    "total_jobs": total_jobs,
                    "diagnosis": {
                        "state": str(diagnosis.get("state") or ""),
                        "reasons": [
                            str(item) for item in list(diagnosis.get("reasons") or [])[:6]
                            if str(item)
                        ],
                        "recommended_actions": [
                            str(item) for item in list(diagnosis.get("recommended_actions") or [])[:6]
                            if str(item)
                        ],
                    },
                    "recent_event_types": [
                        str(event.get("event_type") or "")
                        for event in (snapshot.get("recent_events") or [])[-5:]
                        if isinstance(event, dict)
                    ],
                },
            },
        }
        text = public_message
        try:
            if hasattr(self._corona_engine, "network_send_system_message_ex"):
                self._corona_engine.network_send_system_message_ex(
                    "system",
                    "系统",
                    text,
                    "action_status",
                    metadata["disclosure"]["event_id"],
                    json.dumps(metadata, ensure_ascii=False),
                )
            elif hasattr(self._corona_engine, "network_send_system_message"):
                self._corona_engine.network_send_system_message("system", "系统", text)
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Failed to emit generation scheduler disclosure: %s", exc)

    def _get_host_action_executor(self) -> Any:
        if self._host_action_executor is None:
            self._host_action_executor = LanChatHostActionExecutor(
                corona_engine=self._corona_engine,
                agent_factory=self._agent_factory or self._default_agent_factory,
                structured_action_handler=self._get_interaction_coordinator().execute_action_payload,
            )
        return self._host_action_executor

    def _get_interaction_coordinator(self) -> InteractionCoordinator:
        if self._interaction_coordinator is None:
            self._interaction_coordinator = InteractionCoordinator(
                scheduler=self._get_generation_scheduler(),
            )
        return self._interaction_coordinator

    def _get_generation_scheduler(self) -> Any:
        if self._generation_scheduler is None:
            if self._composer_factory is not None:
                runner = SceneComposerJobRunner(self._composer_factory)
                self._generation_scheduler = GenerationScheduler(
                    stage_handlers=runner.stage_handlers(),
                    stage_order=("compose",),
                )
            else:
                self._generation_scheduler = GenerationScheduler()
            self._install_generation_scheduler_hooks(self._generation_scheduler)
        return self._generation_scheduler

    def _install_generation_scheduler_hooks(self, scheduler: Any) -> None:
        self._install_deferred_download_scheduler(scheduler)
        self._install_media_task_scheduler(scheduler)
        self._install_progress_disclosure_scheduler(scheduler)

    def _clear_generation_scheduler_hooks(self, scheduler: Any) -> None:
        self._clear_deferred_download_scheduler(scheduler)
        self._clear_media_task_scheduler(scheduler)
        self._clear_progress_disclosure_scheduler(scheduler)

    def _install_progress_disclosure_scheduler(self, scheduler: Any) -> None:
        submit = getattr(scheduler, "submit", None)
        if not callable(submit):
            return
        if getattr(scheduler, "_lanchat_progress_disclosure_installed", False):
            return
        worker = self

        def submit_with_progress(payload: dict[str, Any]) -> Any:
            job_payload = dict(payload or {})
            job_type = str(job_payload.get("job_type") or "")
            if job_type.startswith("scene_generation"):
                runtime_context = dict(job_payload.get("_runtime_context") or {})
                if not callable(runtime_context.get("progress_sink")):
                    runtime_context["progress_sink"] = worker._make_generation_progress_sink(
                        room_id=str(job_payload.get("room_id") or job_payload.get("session_id") or ""),
                        plan_id=str(job_payload.get("plan_id") or ""),
                    )
                    job_payload["_runtime_context"] = runtime_context
            return submit(job_payload)

        try:
            setattr(scheduler, "_lanchat_progress_disclosure_original_submit", submit)
            setattr(scheduler, "_lanchat_progress_disclosure_installed", True)
            setattr(scheduler, "submit", submit_with_progress)
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Failed to install LANChat progress disclosure scheduler hook: %s", exc)

    def _clear_progress_disclosure_scheduler(self, scheduler: Any) -> None:
        if not getattr(scheduler, "_lanchat_progress_disclosure_installed", False):
            return
        original = getattr(scheduler, "_lanchat_progress_disclosure_original_submit", None)
        try:
            if callable(original):
                setattr(scheduler, "submit", original)
            setattr(scheduler, "_lanchat_progress_disclosure_installed", False)
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Failed to clear LANChat progress disclosure scheduler hook: %s", exc)

    def _make_generation_progress_sink(self, *, room_id: str, plan_id: str) -> Callable[[str], None]:
        def sink(message: str) -> None:
            self._emit_generation_progress_disclosure(
                message,
                room_id=room_id,
                plan_id=plan_id,
            )
        return sink

    def _emit_generation_progress_disclosure(self, message: str, *, room_id: str, plan_id: str) -> None:
        text = self._safe_control_text(str(message or "").strip())
        if not text or self._corona_engine is None:
            return
        room = str(room_id or "default")
        now = time.time()
        with self._progress_disclosure_lock:
            last_text, last_at = self._progress_disclosure_last_by_room.get(room, ("", 0.0))
            if text == last_text and now - float(last_at or 0.0) < 1.0:
                return
            self._progress_disclosure_last_by_room[room] = (text, now)
        stage, progress = self._generation_progress_stage_and_percent(text)
        event_id = f"generation-progress-{room}-{int(now * 1000)}"
        disclosure = {
            "event_id": event_id,
            "room_id": room,
            "audience": "participant",
            "stage": stage,
            "progress": progress,
            "public_message": text,
            "available_actions": ["add_note", "pause_after_batch"],
            "requires_confirmation": False,
            "metadata": {
                "plan_id": str(plan_id or ""),
                "source": "generation_progress_sink",
            },
        }
        metadata = json.dumps({"disclosure": disclosure}, ensure_ascii=False)
        try:
            if hasattr(self._corona_engine, "network_send_system_message_ex"):
                self._corona_engine.network_send_system_message_ex(
                    "system",
                    "系统",
                    text,
                    "action_status",
                    event_id,
                    metadata,
                )
            elif hasattr(self._corona_engine, "network_send_system_message"):
                self._corona_engine.network_send_system_message("system", "系统", text)
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Failed to emit generation progress disclosure: %s", exc)

    @staticmethod
    def _generation_progress_stage_and_percent(text: str) -> tuple[str, int]:
        progress = 0
        match = re.search(r"生成进度\s*(\d{1,3})\s*%", str(text or ""))
        if match:
            progress = max(0, min(100, int(match.group(1))))
        if "排队" in text:
            return "排队中", progress
        if "准备所需模型" in text or "图片" in text or "模型" in text:
            return "资源准备", progress
        if "开始组装" in text or "导入" in text or "放入" in text or "摆放" in text:
            return "分批组装", progress
        if "自动检查" in text or "检查" in text:
            return "最终检查", progress
        if "完成空间" in text or "理解场景" in text:
            return "理解方案", progress
        return "生成中", progress

    def _install_deferred_download_scheduler(self, scheduler: Any) -> None:
        try:
            from plugins.AITool.Quasar.ai_modules.three_d_generate.tools import model_tools
        except Exception:
            return
        setter = getattr(model_tools, "set_deferred_download_scheduler", None)
        if callable(setter):
            try:
                setter(scheduler)
            except Exception as exc:  # noqa: BLE001
                self._logger.debug("Failed to install deferred download scheduler: %s", exc)

    def _clear_deferred_download_scheduler(self, scheduler: Any) -> None:
        try:
            from plugins.AITool.Quasar.ai_modules.three_d_generate.tools import model_tools
        except Exception:
            return
        getter = getattr(model_tools, "get_deferred_download_scheduler", None)
        setter = getattr(model_tools, "set_deferred_download_scheduler", None)
        if not callable(getter) or not callable(setter):
            return
        try:
            if getter() is scheduler:
                setter(None)
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Failed to clear deferred download scheduler: %s", exc)

    def _install_media_task_scheduler(self, scheduler: Any) -> None:
        try:
            from plugins.AITool.Quasar.ai_media_resource import registry
        except Exception:
            return
        setter = getattr(registry, "set_media_task_scheduler", None)
        if callable(setter):
            try:
                setter(scheduler)
            except Exception as exc:  # noqa: BLE001
                self._logger.debug("Failed to install media task scheduler: %s", exc)

    def _clear_media_task_scheduler(self, scheduler: Any) -> None:
        try:
            from plugins.AITool.Quasar.ai_media_resource import registry
        except Exception:
            return
        getter = getattr(registry, "get_media_task_scheduler", None)
        setter = getattr(registry, "set_media_task_scheduler", None)
        if not callable(getter) or not callable(setter):
            return
        try:
            if getter() is scheduler:
                setter(None)
        except Exception as exc:  # noqa: BLE001
            self._logger.debug("Failed to clear media task scheduler: %s", exc)

    def _prepare_confirmed_action_payload(
        self,
        payload: dict[str, Any] | None,
        trigger: dict[str, Any],
    ) -> dict[str, Any] | None:
        if not payload or payload.get("status") != "confirmed":
            return payload
        if str(payload.get("action_type") or "") != "start_generation":
            return payload
        if payload.get("seed_plan") and payload.get("plan_id"):
            return payload

        coordinator = self._get_interaction_coordinator()
        room_id = str(trigger.get("room_id") or payload.get("room_id") or "default")
        host_id = str(trigger.get("sender_id") or payload.get("source_user_id") or "host")
        intent_text = str(
            payload.get("resolved_intent_text")
            or payload.get("intent_text")
            or trigger.get("text")
            or ""
        )
        plan = coordinator.create_or_update_seed_plan(ChatMessage(
            room_id=room_id,
            sender_id=host_id,
            sender_name=str(trigger.get("sender_name") or ""),
            text=intent_text,
            is_host=True,
            agent_id=str(trigger.get("agent_id") or ""),
            agent_name=str(trigger.get("agent_name") or ""),
        ))
        if plan.status.value == "draft":
            plan.propose()
        confirmed = coordinator.confirm_seed_plan(plan.plan_id, host_id)
        confirmed_payload = confirmed.payload if isinstance(getattr(confirmed, "payload", None), dict) else {}
        seed_plan = confirmed_payload.get("seed_plan")
        if not getattr(confirmed, "ok", False) or not confirmed_payload.get("plan_id") or not seed_plan:
            structured = dict(payload)
            structured.update({
                "action_type": "discussion_only",
                "execution": "coordinator_confirmation_blocked",
                "room_id": room_id,
                "plan_id": plan.plan_id,
                "requires_host_confirm": False,
                "status": "confirmed",
                "coordinator_blocked": True,
                "reason": str(getattr(confirmed, "message", "") or "SeedPlan 暂不能确认执行。"),
                "seed_plan_status": str(getattr(plan.status, "value", plan.status)),
            })
            structured.setdefault("intent_text", intent_text)
            return structured
        structured = dict(payload)
        structured.update({
            "action_type": "start_generation",
            "execution": "coordinator_structured",
            "plan_id": confirmed_payload["plan_id"],
            "plan_version": confirmed_payload["plan_version"],
            "room_id": room_id,
            "seed_plan": seed_plan,
            "requires_host_confirm": False,
            "status": "confirmed",
        })
        structured.setdefault("intent_text", intent_text)
        return structured

    @staticmethod
    def _correlation_id(trigger: dict[str, Any]) -> str:
        return str(trigger.get("correlation_id") or trigger.get("message_id") or "")

    @staticmethod
    def _should_send_fast_ack(trigger: dict[str, Any]) -> bool:
        kind = str(trigger.get("message_kind") or "chat").lower()
        if kind and kind != "chat":
            return False
        text = str(trigger.get("text") or "")
        if not text.strip():
            return False
        keywords = (
            "生成", "设计", "场景", "房间", "卧室", "广场", "教堂",
            "添加", "放大", "缩小", "移动", "移", "删", "删除", "调整",
            "靠左", "靠右", "远一点", "近一点", "不要太拥挤",
            "generate", "create", "move", "scale", "delete", "adjust",
        )
        return any(keyword in text for keyword in keywords)

    @staticmethod
    def _messages_from_trigger(trigger: dict[str, Any]) -> list[str]:
        messages: list[str] = []
        history = trigger.get("history") or []
        if isinstance(history, list):
            for item in history:
                if not isinstance(item, dict):
                    continue
                sender = str(item.get("from") or item.get("sender_name") or "")
                text = str(item.get("text") or "")
                if text:
                    messages.append(f"{sender}: {text}" if sender else text)

        text = str(trigger.get("text") or "")
        if text and text not in messages:
            messages.append(text)
        return messages

    @staticmethod
    def _default_agent_factory() -> Any:
        from plugins.AITool.cai_extensions.agent.agent_adapter import create_master_agent

        return create_master_agent()
