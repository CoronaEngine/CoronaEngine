from __future__ import annotations

import ast
import importlib.util
import json
import threading
from pathlib import Path
import sys
import tempfile
import types
import unittest
from typing import Any
from unittest.mock import patch


REPO_ROOT = Path(__file__).resolve().parents[5]
EDITOR_ROOT = REPO_ROOT / "editor"
if str(EDITOR_ROOT) not in sys.path:
    sys.path.insert(0, str(EDITOR_ROOT))

from plugins.AITool.services.agent_runtime import (  # noqa: E402
    AgentRuntime,
    AgentRuntimeFlags,
    StatePatch,
    install_f5_runtime_provider_env_defaults,
)
from plugins.AITool.services.agent_runtime import adapters as runtime_adapters  # noqa: E402
from plugins.AITool.services.agent_runtime.adapters import RuntimeCppBridge  # noqa: E402

runtime_adapters.ENGINE_READY_TIMEOUT_DEFAULT_S = 0.2
runtime_adapters.ENGINE_READY_POLL_DEFAULT_S = 0.01
from plugins.AITool.services.disclosure_policy import DisclosureEvent  # noqa: E402
from plugins.AITool.services.intent_understanding import IntentUnderstandingService  # noqa: E402
from plugins.AITool.services.lanchat_agent_worker import LANChatAgentWorker  # noqa: E402
from plugins.AITool.services.lanchat_scene_runtime import get_lanchat_scene_runtime  # noqa: E402
from plugins.AITool.services.seed_plan import SeedPlanStatus  # noqa: E402
from plugins.AITool.services.workflow_command_policy import (  # noqa: E402
    DEPRECATED_WORKFLOW_COMMAND_MESSAGE,
    DEPRECATED_USER_WORKFLOW_COMMANDS,
    INTERNAL_DEBUG_WORKFLOW_COMMANDS,
    classify_workflow_command_exposure,
    classify_workflow_function_exposure,
    install_workflow_command_policy,
    install_workflow_function_policy,
    is_deprecated_user_workflow_command,
    should_execute_workflow_function,
    should_register_workflow_command,
)


def _load_agent_adapter_module():
    module_path = REPO_ROOT / "editor" / "plugins" / "AITool" / "cai_extensions" / "agent" / "agent_adapter.py"
    spec = importlib.util.spec_from_file_location("_runtime_guard_agent_adapter", module_path)
    if spec is None or spec.loader is None:
        raise RuntimeError("unable to load agent_adapter module for runtime guard test")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class _FakePlan:
    plan_id = "seed-test"
    status = SeedPlanStatus.CONFIRMED
    design_brief = "测试方案"
    intent_summary = "测试"
    metadata = {"scene_name": "Scene/场景1.scene"}


class _FakeDraftPlan:
    plan_id = "seed-draft"
    status = SeedPlanStatus.PROPOSED
    design_brief = "强盗藏宝室方案：中央宝箱，两侧武器架，入口火把"
    intent_summary = "强盗藏宝室"
    owner_agent_name = "山贼"
    source_context_agents = ["长者"]
    metadata = {}


class _FakeRef:
    job_id = "gen-test"
    status = "queued"


class _FakeCoordinator:
    def __init__(self) -> None:
        self.disclosure_events: list = []
        self.execute_calls: list[str] = []
        self.action_payload_calls: list[dict] = []
        self.ingest_calls: list = []
        self.seed_plan_calls: list = []
        self.plan = _FakePlan()

    def active_plan_for_room(self, room_id: str):
        return self.plan

    def _is_status_query(self, text: str) -> bool:
        return "状态" in str(text or "") or "到哪里" in str(text or "")

    def ingest_message(self, message):
        self.ingest_calls.append(message)
        return type("StatusEvent", (), {"event_type": "status_query", "message": "coordinator status"})()

    def execute_confirmed_plan(self, plan_id: str):
        self.execute_calls.append(plan_id)
        return _FakeRef()

    def execute_action_payload(self, payload: dict):
        self.action_payload_calls.append(dict(payload or {}))
        return "legacy action executed"

    def create_or_update_seed_plan(self, message):
        self.seed_plan_calls.append(message)
        return self.plan


class _FakeNoActiveCoordinator(_FakeCoordinator):
    def active_plan_for_room(self, room_id: str):
        return None


class _ExplodingCoordinator(_FakeCoordinator):
    def active_plan_for_room(self, room_id: str):  # noqa: ANN001
        raise AssertionError("Runtime explicit queries must not touch Coordinator active plan by default")


class _FakeReplyEngine:
    def __init__(self) -> None:
        self.replies: list[dict] = []

    def network_send_agent_reply_ex(
        self,
        agent_id: str,
        agent_name: str,
        text: str,
        message_kind: str,
        target_agent_id: str,
        correlation_id: str,
        metadata_json: str,
    ) -> bool:
        self.replies.append({
            "agent_id": agent_id,
            "agent_name": agent_name,
            "text": text,
            "message_kind": message_kind,
            "target_agent_id": target_agent_id,
            "correlation_id": correlation_id,
            "metadata_json": metadata_json,
        })
        return True


class _FakeFailingReplyEngine(_FakeReplyEngine):
    def network_send_agent_reply_ex(
        self,
        agent_id: str,
        agent_name: str,
        text: str,
        message_kind: str,
        target_agent_id: str,
        correlation_id: str,
        metadata_json: str,
    ) -> bool:
        self.replies.append({
            "agent_id": agent_id,
            "agent_name": agent_name,
            "text": text,
            "message_kind": message_kind,
            "target_agent_id": target_agent_id,
            "correlation_id": correlation_id,
            "metadata_json": metadata_json,
        })
        return False


class _FakeFlakyReplyEngine(_FakeReplyEngine):
    def __init__(self) -> None:
        super().__init__()
        self.attempts = 0

    def network_send_agent_reply_ex(
        self,
        agent_id: str,
        agent_name: str,
        text: str,
        message_kind: str,
        target_agent_id: str,
        correlation_id: str,
        metadata_json: str,
    ) -> bool:
        self.attempts += 1
        super().network_send_agent_reply_ex(
            agent_id,
            agent_name,
            text,
            message_kind,
            target_agent_id,
            correlation_id,
            metadata_json,
        )
        return self.attempts > 1


class _FakeIdleEngine:
    def __init__(self) -> None:
        self.system_messages: list[dict[str, str]] = []
        self.intent_broadcasts: list[dict[str, Any]] = []

    def network_pop_lanchat_agent_trigger(self):  # noqa: ANN201
        return None

    def network_send_agent_reply(self, *args, **kwargs) -> bool:  # noqa: ANN002, ANN003
        return True

    def network_send_system_message_ex(
        self,
        agent_id: str,
        agent_name: str,
        text: str,
        message_kind: str,
        correlation_id: str,
        metadata_json: str,
    ) -> bool:
        self.system_messages.append({
            "agent_id": agent_id,
            "agent_name": agent_name,
            "text": text,
            "message_kind": message_kind,
            "correlation_id": correlation_id,
            "metadata_json": metadata_json,
        })
        return True

    def network_send_system_message(self, agent_id: str, agent_name: str, text: str) -> bool:
        self.system_messages.append({
            "agent_id": agent_id,
            "agent_name": agent_name,
            "text": text,
            "message_kind": "runtime_status",
            "correlation_id": "",
            "metadata_json": "{}",
        })
        return True

    def network_broadcast_intent(
        self,
        source_user_id: str,
        tooltip: str,
        position: list[float],
        status: str,
    ) -> bool:
        self.intent_broadcasts.append({
            "source_user_id": source_user_id,
            "tooltip": tooltip,
            "position": list(position),
            "status": status,
        })
        return True


class _FakeFailingSystemMessageEngine(_FakeIdleEngine):
    def network_send_system_message_ex(
        self,
        agent_id: str,
        agent_name: str,
        text: str,
        message_kind: str,
        correlation_id: str,
        metadata_json: str,
    ) -> bool:
        self.system_messages.append({
            "agent_id": agent_id,
            "agent_name": agent_name,
            "text": text,
            "message_kind": message_kind,
            "correlation_id": correlation_id,
            "metadata_json": metadata_json,
        })
        return False


class _FakeTargetedHostDisclosureEngine(_FakeIdleEngine):
    def __init__(self) -> None:
        super().__init__()
        self.host_messages: list[dict[str, str]] = []

    def network_send_system_message_to_host_ex(
        self,
        agent_id: str,
        agent_name: str,
        text: str,
        message_kind: str,
        correlation_id: str,
        metadata_json: str,
    ) -> bool:
        self.host_messages.append({
            "agent_id": agent_id,
            "agent_name": agent_name,
            "text": text,
            "message_kind": message_kind,
            "correlation_id": correlation_id,
            "metadata_json": metadata_json,
        })
        return True


class _FakeCompletedPlan:
    plan_id = "seed-completed"
    status = SeedPlanStatus.COMPLETED
    design_brief = "做一个强盗藏宝室，包含宝箱、金币和火把"
    intent_summary = "强盗藏宝室"
    owner_agent_name = "商人"
    source_context_agents = ["山贼"]


class _FakeCompletedEvent:
    event_type = "layout_reflow_proposal_created"
    message = "已生成布局调整建议。"
    payload = {"proposal_id": "layout-test", "status": "proposed"}


class _FakeCompletedConfirmedEvent:
    event_type = "layout_reflow_confirmed"
    message = "布局调整建议已确认。"
    payload = {"proposal_id": "layout-test", "status": "confirmed"}


class _FakeFinalAdjustmentEvent:
    event_type = "final_adjustment_routed"
    message = "已记录最终调整。"
    payload = {
        "intent_type": "modify",
        "content": "把藏宝箱放大一点",
        "target_hint": "藏宝箱",
    }


class _FakeExecutingPlan:
    plan_id = "seed-executing"
    status = SeedPlanStatus.EXECUTING
    design_brief = "做一个强盗藏宝室，包含宝箱、金币和火把"
    intent_summary = "强盗藏宝室"
    owner_agent_name = "山贼"
    source_context_agents = []


class _FakeInterventionEvent:
    event_type = "intervention_routed"
    message = "已记录该介入，将在下一批次前吸收。"
    payload = {
        "intervention": {
            "intervention_id": "iv-test",
            "intent_type": "add",
            "content": "再添加一个天使雕像",
            "apply_policy": "next_batch",
        }
    }


class _FakeCompletedCoordinator:
    def __init__(self) -> None:
        self.disclosure_events: list = []
        self.plan = _FakeCompletedPlan()
        self.ingested: list = []

    def active_plan_for_room(self, room_id: str):
        return self.plan

    def _is_status_query(self, text: str) -> bool:
        return False

    def _is_post_generation_adjustment(self, text: str) -> bool:
        return True

    def _intent_type(self, text: str) -> str:
        return "modify"

    def ingest_message(self, message):
        self.ingested.append(message)
        return _FakeCompletedEvent()


class _FakeCompletedConfirmedCoordinator(_FakeCompletedCoordinator):
    def ingest_message(self, message):
        self.ingested.append(message)
        return _FakeCompletedConfirmedEvent()


class _FakeFinalAdjustmentCoordinator(_FakeCompletedCoordinator):
    def ingest_message(self, message):
        self.ingested.append(message)
        return _FakeFinalAdjustmentEvent()


class _FakeFinalAdjustmentConfirmCoordinator(_FakeCompletedCoordinator):
    def __init__(self) -> None:
        super().__init__()
        self.confirm_calls: list[dict] = []

    def confirm_final_adjustment_conflict(self, proposal_id: str, host_id: str, *, decision: str = "confirm"):
        self.confirm_calls.append({
            "proposal_id": proposal_id,
            "host_id": host_id,
            "decision": decision,
        })
        normalized = "rejected" if str(decision).lower() in {"reject", "rejected", "cancel"} else "confirmed"
        return type(
            "ConfirmResult",
            (),
            {
                "ok": True,
                "plan_id": "seed-completed",
                "message": "最终调整冲突确认已记录。",
                "payload": {
                    "proposal": {
                        "proposal_id": proposal_id,
                        "room_id": "room-final-confirm-worker",
                        "plan_id": "seed-completed",
                        "target_hint": "藏宝箱",
                        "conflict_items": ["放大藏宝箱", "保持通道"],
                        "status": normalized,
                        "confirmed_by": host_id,
                    }
                },
            },
        )()


class _FakeExecutingCoordinator(_FakeCompletedCoordinator):
    def __init__(self) -> None:
        super().__init__()
        self.plan = _FakeExecutingPlan()

    def _intent_type(self, text: str) -> str:
        return "add"

    def _is_post_generation_adjustment(self, text: str) -> bool:
        return False

    def ingest_message(self, message):
        self.ingested.append(message)
        return _FakeInterventionEvent()


class _FakePaceCoordinator(_FakeCoordinator):
    def __init__(self) -> None:
        super().__init__()
        self.control_calls: list[dict[str, Any]] = []

    def control_pace(self, room_id: str, action: str, *, actor_id: str = "", note: str = ""):
        self.control_calls.append({
            "room_id": room_id,
            "action": action,
            "actor_id": actor_id,
            "note": note,
        })
        return type("PaceEvent", (), {"message": f"已暂停后续生成: {action}"})()


class _TestWorker(LANChatAgentWorker):
    def _can_execute_generation_locally(self) -> bool:
        return True

    def _emit_new_disclosure_events(self, coordinator, start_index: int) -> int:
        return 0

    def _start_coordinator_disclosure_watch(self, coordinator, start_index: int) -> None:
        return None

    def _emit_generation_scheduler_disclosure(self) -> None:
        return None


class _DispatchProbeWorker(_TestWorker):
    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self.run_calls = 0
        self.run_started = threading.Event()
        self.run_release = threading.Event()

    def _seed_agent_trigger_planning_context_in_runtime(self, trigger: dict):  # noqa: ANN201
        return None

    def _handle_agent_trigger_planning_gate(self, trigger: dict) -> bool:
        return False

    def _handle_agent_trigger_runtime_write_gate(self, trigger: dict, *, planning_seed=None) -> bool:  # noqa: ANN001
        return False

    def _mirror_agent_reply_context_in_agent_runtime(self, **kwargs) -> None:  # noqa: ANN003
        return None

    def _run_agent(self, trigger: dict):  # noqa: ANN201
        self.run_calls += 1
        self.run_started.set()
        self.run_release.wait(timeout=1.0)
        return type(
            "Result",
            (),
            {
                "sender_id": str(trigger.get("agent_id") or "agent"),
                "sender_name": str(trigger.get("agent_name") or "Agent"),
                "text": "authoritative reply",
                "action_payload": None,
            },
        )()


class _RemoteGenerationWorker(_TestWorker):
    def _can_execute_generation_locally(self) -> bool:
        return False


class _RemoteClientWorker(_TestWorker):
    def _can_execute_agent_locally(self) -> bool:
        return False


class _LayoutDirectExecutionTrackingWorker(_TestWorker):
    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self.direct_layout_reflow_calls = 0
        self.direct_final_adjustment_calls = 0
        self.coordinator_system_replies: list[str] = []

    def _execute_layout_reflow_confirmation(self, payload: dict) -> str:
        self.direct_layout_reflow_calls += 1
        return "legacy layout reflow executed"

    def _try_execute_completed_final_adjustment(self, event, trigger: dict) -> str:
        self.direct_final_adjustment_calls += 1
        return "legacy final adjustment executed"

    def _send_coordinator_sync_system_reply(self, message: dict, text: str) -> bool:
        self.coordinator_system_replies.append(str(text or ""))
        return True


class LANChatRuntimeGuardTests(unittest.TestCase):
    def test_model_call_summary_records_zero_for_deterministic_reply(self) -> None:
        engine = _FakeReplyEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        trigger = {
            "room_id": "room-model-zero",
            "message_id": "msg-model-zero",
            "correlation_id": "corr-model-zero",
        }

        self.assertTrue(worker._send_final_reply("gm-system", "GM", "status unchanged", trigger))

        summary = worker._model_call_ledger.summary(
            room_id="room-model-zero",
            message_id="msg-model-zero",
        )
        self.assertEqual(summary["call_count"], 0)
        entries = worker._agent_runtime.operation_log.query(
            event="model_call_summary",
            room_id="room-model-zero",
        )
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0].payload["call_count"], 0)

    def test_visible_agent_reasoning_has_one_call_budget(self) -> None:
        worker = _TestWorker(
            agent_factory=lambda: (lambda persona, messages: "structured agent reply"),
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        trigger = {
            "room_id": "room-model-one",
            "message_id": "msg-model-one",
            "correlation_id": "corr-model-one",
            "text": "Give one structured proposal.",
            "agent_id": "planning-agent",
            "agent_name": "planning-agent",
        }

        result = worker._run_agent(trigger)

        self.assertEqual(result.text, "structured agent reply")
        summary = worker._model_call_ledger.summary(
            room_id="room-model-one",
            message_id="msg-model-one",
        )
        self.assertEqual(summary["call_count"], 1)
        self.assertEqual(summary["purposes"], ["agent_visible_reasoning"])
        with self.assertRaisesRegex(RuntimeError, "model call budget exhausted"):
            worker._run_agent(trigger)

    def test_member_native_chat_does_not_run_authoritative_agent_routes(self) -> None:
        worker = _RemoteClientWorker(
            interaction_coordinator=_ExplodingCoordinator(),
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        operation_count_before = len(worker._agent_runtime.operation_log.entries())
        message = {
            "room_id": "room-member-authority",
            "message_id": "msg-member-authority",
            "text": "@GM 当前进度",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "is_host": True,
        }

        handled = worker.sync_chat_message_to_coordinator(
            message,
            source="lanchat_native_queue",
            emit_disclosure=False,
        )
        repeated = worker.sync_chat_message_to_coordinator(
            message,
            source="lanchat_native_queue",
            emit_disclosure=False,
        )

        self.assertFalse(handled)
        self.assertFalse(repeated)
        self.assertEqual(len(worker._agent_runtime.operation_log.entries()), operation_count_before)
        self.assertEqual(len(worker._coordinator_seen_message_ids), 1)

    def test_runtime_planning_external_id_reuses_active_room_plan_link(self) -> None:
        worker = _TestWorker()
        plan = worker._agent_runtime.sync_external_plan_context(
            room_id="room-plan-link",
            external_plan_id="planning:first-message",
            text="children bedroom plan",
            owner_agent="elder",
        )

        resolved = worker._runtime_planning_external_id(
            {
                "room_id": "room-plan-link",
                "correlation_id": "second-message",
                "agent_name": "girl",
            },
            "girl",
        )

        self.assertEqual(resolved, "planning:first-message")
        room = worker._agent_runtime.query_state("room-plan-link")["room"]
        self.assertEqual(room["active_plan_id"], plan.plan_id)

    def test_worker_logger_exists_before_runtime_provider_initialization(self) -> None:
        class ProbeWorker(LANChatAgentWorker):
            logger_seen_during_runtime_create = False

            def _create_agent_runtime(self) -> AgentRuntime:
                self.logger_seen_during_runtime_create = hasattr(self, "_logger")
                return AgentRuntime()

        worker = ProbeWorker()

        self.assertTrue(worker.logger_seen_during_runtime_create)

    def test_f5_worker_marks_engine_actor_import_as_required(self) -> None:
        env: dict[str, str] = {}
        install_f5_runtime_provider_env_defaults(env)
        flags = AgentRuntimeFlags.from_env(env)

        # This assertion only covers the flag-to-runtime contract.  Avoid
        # discovering production Quasar tools here, which can construct real
        # HTTP clients during a unit test and make the suite order-dependent.
        with patch.object(LANChatAgentWorker, "_get_runtime_tool", lambda self, name: None):
            worker = _TestWorker(corona_engine=object(), agent_runtime_flags=flags)

        self.assertTrue(worker._agent_runtime._require_engine_actor_import)

    def test_worker_runtime_quasar_import_path_points_to_aitool_root(self) -> None:
        aitool_root = str((REPO_ROOT / "editor" / "plugins" / "AITool").resolve())
        original_path = list(sys.path)
        try:
            sys.path = [
                entry
                for entry in sys.path
                if str(Path(entry).resolve()) != aitool_root
            ]

            LANChatAgentWorker._ensure_runtime_quasar_import_path()

            self.assertEqual(str(Path(sys.path[0]).resolve()), aitool_root)
        finally:
            sys.path = original_path

    @staticmethod
    def _non_planning_tool_graphs(room_state: dict) -> list[dict]:
        planning_tools = {
            "runtime.plan.extract",
            "runtime.elements.classify",
            "scene.extract_objects",
            "scene.classify_type",
            "scene.extract_constraints",
            "scene.extract_environment",
            "room.estimate_bounds",
            "zone.decompose",
            "asset.route_item",
            "placement.prepare_items",
        }
        graphs: list[dict] = []
        for graph in dict(room_state.get("tool_graphs") or {}).values():
            nodes = dict(graph.get("nodes") or {})
            tool_names = {
                str(node.get("tool_name") or "")
                for node in nodes.values()
                if isinstance(node, dict)
            }
            if tool_names and tool_names <= planning_tools:
                continue
            if tool_names and tool_names <= {"runtime.scene_plan.persist"}:
                continue
            if tool_names and tool_names <= {"runtime.external_plan.link"}:
                continue
            if tool_names and tool_names <= {"runtime.scene_plan.status.persist"}:
                continue
            if tool_names and tool_names <= {"runtime.planning_context.persist"}:
                continue
            if tool_names and tool_names <= {"runtime.event.emit"}:
                continue
            if tool_names and tool_names <= {"runtime.audit_event.record"}:
                continue
            if tool_names and tool_names <= {"runtime.provider_readiness.publish"}:
                continue
            graphs.append(dict(graph))
        return graphs

    def test_runtime_resource_and_fact_source_formatters_surface_attention(self) -> None:
        resource_text = LANChatAgentWorker._format_agent_runtime_resource_stage_report({
            "event_count": 3,
            "by_phase": {
                "image": {"item_count": 1, "requested_count": 2, "failed_count": 1},
                "model": {"item_count": 0, "requested_count": 1, "failed_count": 0},
                "import": {"item_count": 1, "requested_count": 2, "failed_count": 1},
                "review": {"item_count": 1, "requested_count": 1, "failed_count": 0},
            },
            "latest_events": [{"phase": "image", "status": "failed"}],
            "needs_attention": ["image_resource_failed", "model_resource_partial"],
        })
        resource_flow_text = LANChatAgentWorker._format_agent_runtime_resource_flow_report({
            "batch_count": 1,
            "completed_count": 0,
            "partial_count": 1,
            "failed_count": 0,
            "waiting_count": 0,
            "latest_batch": {
                "batch_index": 1,
                "total_batches": 1,
                "status": "partial",
                "requested_count": 3,
                "image_ready_count": 3,
                "model_ready_count": 1,
                "import_ready_count": 1,
                "import_failure_code_counts": {
                    "missing_ready_model_resource": 2,
                    "provider_url_hidden": 1,
                },
            },
        })
        fact_source_text = LANChatAgentWorker._format_agent_runtime_fact_source_boundary_report({
            "runtime_business_fact_count": 4,
            "mirrored_external_fact_count": 2,
            "runtime_plan_fact_count": 1,
            "runtime_batch_fact_count": 1,
            "runtime_resource_event_count": 1,
            "runtime_import_event_count": 1,
            "sync_event_count": 1,
            "engine_write_result_count": 1,
            "engine_write_boundary_fact_count": 1,
            "scene_snapshot_count": 0,
            "external_authoritative_available": True,
            "boundary_notes": ["runtime-state-is-business-truth"],
        })

        self.assertIn("needs image-resource-failed,model-resource-partial", resource_text)
        self.assertIn("import 1/2 failed 1", resource_text)
        self.assertIn("review 1/1 failed 0", resource_text)
        self.assertIn("import-failures", resource_flow_text)
        self.assertIn("missing-ready-model-resource:2", resource_flow_text)
        self.assertIn("resource-resource-hidden:1", resource_flow_text)
        self.assertNotIn("provider", resource_flow_text)
        self.assertNotIn("url", resource_flow_text)
        self.assertIn("runtime 4", fact_source_text)
        self.assertIn("external 2", fact_source_text)
        self.assertIn("write-boundary 1", fact_source_text)
        self.assertIn("external available", fact_source_text)

        layout_text = LANChatAgentWorker._format_agent_runtime_layout_report(
            {
                "proposal_count": 1,
                "delta_count": 3,
                "applied_delta_count": 2,
                "skipped_delta_count": 1,
                "transform_result_count": 2,
                "ground_snapped_count": 1,
                "overlap_resolved_count": 1,
                "layout_transform_failure_code_counts": {
                    "cpp_actor_transform_failed": 1,
                    "provider_raw_url_hidden": 2,
                },
                "proposals": [
                    {
                        "proposal_id": "layout-demo",
                        "status": "confirmed",
                        "risk_level": "low",
                        "delta_count": 3,
                    }
                ],
            },
            {"confirmation_count": 1},
        )
        self.assertIn("applied 2", layout_text)
        self.assertIn("skipped 1", layout_text)
        self.assertIn("transforms 2", layout_text)
        self.assertIn("ground-snapped 1", layout_text)
        self.assertIn("overlap-resolved 1", layout_text)
        self.assertIn("transform-failures cpp-actor-transform-failed:1,redacted:2", layout_text)
        self.assertIn("confirmations 1", layout_text)
        self.assertNotIn("provider", layout_text)
        self.assertNotIn("url", layout_text)
        self.assertNotIn("锛", layout_text)

        report_health_text = LANChatAgentWorker._format_agent_runtime_report_health_report({
            "status": "failed",
            "attention_required": True,
            "batch_failed_count": 1,
            "batch_partial_count": 2,
            "batch_waiting_count": 3,
            "import_failed_count": 4,
            "import_failure_code_counts": {
                "missing_ready_model_resource": 2,
                "provider_url_hidden": 1,
            },
            "resource_phase_failed_count": 5,
            "resource_phase_partial_count": 6,
            "resource_phase_waiting_count": 7,
            "asset_failed_count": 8,
            "asset_incomplete_count": 9,
            "sync_health_status": "needs_attention",
            "sync_failure_code_counts": {
                "sync_event_record_failed": 2,
                "provider_raw_url_hidden": 1,
            },
            "latest_sync_failure_code": "sync_event_record_failed",
            "engine_write_readiness_mismatch_count": 1,
            "engine_write_readiness_mismatch_channels": ["layout_transform"],
            "engine_write_runtime_state_only_count": 2,
            "engine_write_runtime_state_only_channels": ["actor_import", "environment_import"],
            "worker_drain_failed_count": 10,
            "worker_drain_status_failed_count": 11,
            "worker_drain_exception_count": 12,
            "worker_drain_plan_resolve_failed_count": 13,
            "reasons": [
                "resource_phase_failed",
                "engine_write_runtime_state_only",
                "worker_drain_failed",
                "provider_raw_url_hidden",
            ],
        })
        self.assertIn("failed", report_health_text)
        self.assertIn("attention yes", report_health_text)
        self.assertIn("batch failed/partial/waiting 1/2/3", report_health_text)
        self.assertIn("import failed 4", report_health_text)
        self.assertIn("import failures missing-ready-model-resource, resource-resource-hidden", report_health_text)
        self.assertIn("resource phase failed/partial/waiting 5/6/7", report_health_text)
        self.assertIn("asset failed/incomplete 8/9", report_health_text)
        self.assertIn("sync needs-attention", report_health_text)
        self.assertIn("sync failures", report_health_text)
        self.assertIn("sync-event-record-failed", report_health_text)
        self.assertIn("latest sync failure sync-event-record-failed", report_health_text)
        self.assertIn("engine-write mismatch 1(layout-transform)", report_health_text)
        self.assertIn(
            "engine-write runtime-state-only 2(actor-import/environment-import)",
            report_health_text,
        )
        self.assertIn("worker-drain failed/status-failed/exception/plan-resolve 10/11/12/13", report_health_text)
        self.assertIn("reasons resource-phase-failed", report_health_text)
        self.assertIn("engine-write-runtime-state-only", report_health_text)
        self.assertIn("worker-drain-failed", report_health_text)
        self.assertNotIn("provider", report_health_text)
        self.assertNotIn("url", report_health_text)

        runtime_event_text = LANChatAgentWorker._format_agent_runtime_replay_runtime_event_report({
            "emitted_count": 4,
            "emit_failed_count": 0,
            "disclosure_skipped_count": 1,
            "event_type_counts": {"report_ready": 1, "image_resources_ready": 2},
            "report_ready_count": 1,
            "report_attention_count": 1,
            "report_health_status_counts": {"partial": 1},
            "latest_report_ready": {
                "status": "partial",
                "attention_required": True,
                "environment_import_failure_code_counts": {
                    "cpp_environment_component_import_failed": 1,
                    "provider_raw_url_hidden": 2,
                },
                "engine_write_bridge_failed_count": 1,
                "engine_write_bridge_error_code_counts": {
                    "cpp_actor_import_failed": 1,
                    "provider_raw_url_hidden": 2,
                },
                "engine_write_readiness_mismatch_count": 1,
                "engine_write_readiness_mismatch_channels": ["layout_transform"],
            },
        })
        runtime_event_gm_text = LANChatAgentWorker._format_agent_runtime_gm_runtime_event_replay_digest({
            "emitted_count": 4,
            "emit_failed_count": 0,
            "disclosure_skipped_count": 1,
            "report_ready_count": 1,
            "report_attention_count": 1,
            "report_health_status_counts": {"partial": 1},
            "latest_report_ready": {
                "environment_import_failure_code_counts": {
                    "cpp_environment_component_import_failed": 1,
                },
                "engine_write_bridge_failed_count": 1,
                "engine_write_bridge_error_code_counts": {
                    "cpp_actor_import_failed": 1,
                },
                "engine_write_readiness_mismatch_count": 1,
                "engine_write_readiness_mismatch_channels": ["layout_transform"],
            },
        })
        self.assertIn("report-ready 1/attention 1 partial:1", runtime_event_text)
        self.assertIn("latest-report partial", runtime_event_text)
        self.assertIn("env-import-failures cpp-environment-component-import-failed:1", runtime_event_text)
        self.assertIn("engine-write-failures cpp-actor-import-failed:1", runtime_event_text)
        self.assertIn("engine-write-mismatch 1(layout-transform)", runtime_event_text)
        self.assertIn("report-ready 1/attention 1 partial:1", runtime_event_gm_text)
        self.assertIn("env-import-failures cpp-environment-component-import-failed:1", runtime_event_gm_text)
        self.assertIn("engine-write-failures cpp-actor-import-failed:1", runtime_event_gm_text)
        self.assertIn("engine-write-mismatch 1(layout-transform)", runtime_event_gm_text)
        self.assertNotIn("provider", runtime_event_text)
        self.assertNotIn("url", runtime_event_text)
        self.assertNotIn("provider", runtime_event_gm_text)
        self.assertNotIn("url", runtime_event_gm_text)

    def test_runtime_cpp_bridge_success_payload_is_narrow_and_sanitized(self) -> None:
        class FakeEngineGate:
            def invoke_tool(self, tool: Any, payload: dict[str, Any]) -> dict[str, Any]:
                return {
                    "success": True,
                    "status": "ok",
                    "actor_id": "actor-box",
                    "actor_name": "藏宝箱",
                    "position": [1.0, 0.0, 2.0],
                    "model_path": "E:/private/model.glb",
                    "provider": "internal-provider",
                    "prompt": "hidden prompt",
                    "metadata": {"api_key": "secret"},
                    "actor": {
                        "actor_guid": "actor-box",
                        "name": "藏宝箱",
                        "url": "https://example.invalid/private",
                    },
                    "debug_payload": {"raw": "private"},
                }

        bridge = RuntimeCppBridge(engine_gate=FakeEngineGate())
        result = bridge.invoke_tool(object(), {"actor_name": "藏宝箱"})

        self.assertTrue(result.success)
        self.assertEqual(result.payload["actor_id"], "actor-box")
        self.assertEqual(result.payload["actor_name"], "藏宝箱")
        self.assertIn("position", result.payload)
        self.assertEqual(result.boundary_fact["bridge_call_count"], 1)
        self.assertEqual(result.boundary_fact["bridge_success_count"], 1)
        self.assertEqual(result.boundary_fact["bridge_failed_count"], 0)
        self.assertEqual(result.boundary_fact["bridge_method_counts"], {"invoke_tool": 1})
        payload_text = json.dumps(result.payload, ensure_ascii=False)
        self.assertNotIn("model_path", payload_text)
        self.assertNotIn("internal-provider", payload_text)
        self.assertNotIn("hidden prompt", payload_text)
        self.assertNotIn("api_key", payload_text)
        self.assertNotIn("https://example.invalid", payload_text)
        self.assertNotIn("debug_payload", payload_text)

    def test_runtime_cpp_bridge_failure_message_is_sanitized(self) -> None:
        class FakeEngineGate:
            def set_transform(self, tool: Any, payload: dict[str, Any]) -> dict[str, Any]:
                return {
                    "success": False,
                    "status": "failed",
                    "error_code": "native_transform_failed",
                    "message": "provider=hunyuan prompt=secret url=https://example.invalid E:\\private\\x.glb",
                }

        bridge = RuntimeCppBridge(engine_gate=FakeEngineGate())
        result = bridge.set_transform(object(), {"actor_id": "actor-box"})

        self.assertFalse(result.success)
        self.assertEqual(result.error_code, "native_transform_failed")
        self.assertEqual(result.message, "tool error native_transform_failed")
        self.assertEqual(result.payload["error"], "tool error native_transform_failed")
        self.assertEqual(result.boundary_fact["bridge_call_count"], 1)
        self.assertEqual(result.boundary_fact["bridge_success_count"], 0)
        self.assertEqual(result.boundary_fact["bridge_failed_count"], 1)
        self.assertEqual(result.boundary_fact["bridge_method_counts"], {"set_transform": 1})
        self.assertEqual(result.boundary_fact["bridge_error_code_counts"], {"native_transform_failed": 1})
        payload_text = json.dumps(result.payload, ensure_ascii=False)
        self.assertNotIn("hunyuan", payload_text)
        self.assertNotIn("prompt", payload_text)
        self.assertNotIn("https://example.invalid", payload_text)
        self.assertNotIn("E:\\private", payload_text)

    def test_runtime_cpp_bridge_missing_gate_method_is_stable(self) -> None:
        bridge = RuntimeCppBridge(engine_gate=object())

        result = bridge.remove_actor(object(), {"actor_id": "actor-box"})

        self.assertFalse(result.success)
        self.assertEqual(result.error_code, "cpp_gate_method_missing")
        self.assertEqual(result.payload, {})
        self.assertIn("method is missing", result.message)

    def test_intent_understanding_handles_agent_basis_revision_and_opinion(self) -> None:
        service = IntentUnderstandingService()

        revision = service.classify("在长者基础上改进", allow_llm=False)
        opinion = service.classify("评价一下长者方案", allow_llm=False)

        self.assertEqual(revision.intent, "plan_revision")
        self.assertEqual(revision.route, "plan_revision")
        self.assertEqual(opinion.intent, "discussion")
        self.assertEqual(opinion.reason, "protocol/design opinion discussion")

    def test_structured_chat_execution_text_does_not_direct_agent_chat(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        process_calls: list[dict] = []
        worker._process_trigger = lambda trigger: process_calls.append(dict(trigger))  # type: ignore[method-assign]

        for text in ("确认生成", "帮我设计一个藏宝室", "调整一下布局", "新增一个天使雕像"):
            handled = worker._handle_structured_chat_route(
                {"room_id": "room-structured-start", "message_id": f"msg-{text}"},
                text,
                {
                    "draft_action": "chat",
                    "target_scope": "agent",
                    "target_agent_id": "merchant",
                    "target_agent_name": "商人",
                },
            )
            self.assertEqual(handled, "")

        self.assertEqual(process_calls, [])

    def test_structured_chat_discussion_text_can_still_route_to_agent_chat(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        process_calls: list[dict] = []
        worker._process_trigger = lambda trigger: process_calls.append(dict(trigger))  # type: ignore[method-assign]

        handled = worker._handle_structured_chat_route(
            {"room_id": "room-structured-discuss", "message_id": "msg-discuss"},
            "评价一下长者方案",
            {
                "draft_action": "chat",
                "target_scope": "agent",
                "target_agent_id": "merchant",
                "target_agent_name": "商人",
            },
        )

        self.assertEqual(handled, "agent_chat")
        self.assertEqual(len(process_calls), 1)

    def test_native_queue_defers_explicit_agent_plan_request_to_agent_trigger(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        process_calls: list[dict] = []
        worker._process_trigger = lambda trigger: process_calls.append(dict(trigger))  # type: ignore[method-assign]

        handled = worker._handle_structured_chat_route(
            {
                "room_id": "room-explicit-agent",
                "message_id": "msg-explicit-agent",
                "target_agent_id": "elder-id",
            },
            "@长者 请你给出一个方案",
            {
                "draft_action": "chat",
                "target_scope": "agent",
                "target_agent_id": "elder-id",
                "target_agent_name": "长者",
                "source": "lanchat_native_queue",
            },
        )

        self.assertEqual(handled, "agent_chat")
        self.assertEqual(process_calls, [])

    def test_native_queue_defers_explicit_agent_before_any_business_mutation(self) -> None:
        class _NoMutationWorker(_TestWorker):
            def _record_conversation_turn_context(self, payload, text):  # noqa: ANN001, ANN201
                raise AssertionError("native observer must not record conversation context")

            def _apply_generation_options_from_message(self, message):  # noqa: ANN001, ANN201
                raise AssertionError("native observer must not apply generation options")

            def _get_interaction_coordinator(self):  # noqa: ANN201
                raise AssertionError("native observer must not resolve the coordinator")

        worker = _NoMutationWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        message = {
            "room_id": "room-native-defer",
            "message_id": "msg-native-defer",
            "text": "@长者 请你给出一个方案",
            "sender_type": "user",
            "sender_id": "host",
            "message_kind": "chat",
            "target_agent_id": "elder-id",
            "target_agent_name": "长者",
            "metadata": {
                "draft_action": "chat",
                "target_scope": "agent",
                "target_agent_id": "elder-id",
                "target_agent_name": "长者",
            },
        }

        handled = worker.sync_chat_message_to_coordinator(
            message,
            source="lanchat_native_queue",
        )

        self.assertTrue(handled)
        self.assertEqual(
            worker._message_dispatch_ledger.entry("room-native-defer", "msg-native-defer"),
            {},
        )

    def test_concurrent_agent_trigger_replay_executes_and_replies_once(self) -> None:
        engine = _FakeReplyEngine()
        worker = _DispatchProbeWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
            async_agent_execution=False,
        )
        trigger = {
            "room_id": "room-dispatch",
            "message_id": "msg-dispatch",
            "correlation_id": "corr-dispatch",
            "text": "请说明你的方案",
            "sender_type": "user",
            "sender_id": "host",
            "message_kind": "chat",
            "agent_id": "elder-id",
            "agent_name": "长者",
            "target_agent_id": "elder-id",
            "target_agent_name": "长者",
        }
        discussion_started = threading.Event()
        discussion_release = threading.Event()

        def handle_discussion(current_trigger: dict[str, Any]) -> bool:
            discussion_started.set()
            discussion_release.wait(timeout=1.0)
            return worker._send_final_reply(
                "elder-id",
                "长者",
                "authoritative reply",
                current_trigger,
            )

        # Plain agent chat is now handled by the canonical discussion route;
        # the legacy _run_agent path must not be needed for replay protection.
        worker._handle_tool_free_discussion = handle_discussion  # type: ignore[method-assign]
        results: list[bool] = []
        first = threading.Thread(target=lambda: results.append(worker._process_trigger(dict(trigger))))
        second = threading.Thread(target=lambda: results.append(worker._process_trigger(dict(trigger))))

        first.start()
        self.assertTrue(discussion_started.wait(timeout=1.0))
        second.start()
        second.join(timeout=1.0)
        discussion_release.set()
        first.join(timeout=1.0)

        self.assertEqual(worker.run_calls, 0)
        self.assertEqual(len(engine.replies), 1)
        self.assertEqual(engine.replies[0]["agent_name"], "长者")
        self.assertEqual(results.count(True), 2)
        entry = worker._message_dispatch_ledger.entry("room-dispatch", "msg-dispatch")
        self.assertEqual(entry["execution_owner"], "agent_trigger")
        self.assertTrue(entry["final_reply_sent"])

    def test_final_reply_rejects_wrong_agent_and_uses_correlation_as_identity(self) -> None:
        engine = _FakeReplyEngine()
        worker = _DispatchProbeWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
            async_agent_execution=False,
        )
        trigger = {
            "room_id": "room-target",
            "correlation_id": "corr-without-message-id",
            "text": "@长者 请你给出一个方案",
            "target_agent_id": "elder-id",
            "target_agent_name": "长者",
        }

        self.assertFalse(worker._send_final_reply("girl-id", "小女孩", "wrong", trigger))
        self.assertTrue(worker._send_final_reply("elder-id", "长者", "correct", trigger))
        self.assertFalse(worker._send_final_reply("elder-id", "长者", "duplicate", trigger))

        self.assertEqual(len(engine.replies), 1)
        self.assertEqual(engine.replies[0]["text"], "correct")
        self.assertEqual(trigger["message_id"], "corr-without-message-id")

    def test_failed_final_reply_can_retry_without_reexecuting(self) -> None:
        engine = _FakeFlakyReplyEngine()
        worker = _DispatchProbeWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
            async_agent_execution=False,
        )
        trigger = {
            "room_id": "room-retry",
            "message_id": "msg-retry",
            "correlation_id": "corr-retry",
            "text": "reply",
            "target_agent_id": "elder-id",
            "target_agent_name": "长者",
        }

        self.assertFalse(worker._send_final_reply("elder-id", "长者", "first", trigger))
        self.assertTrue(worker._send_final_reply("elder-id", "长者", "second", trigger))
        self.assertFalse(worker._send_final_reply("elder-id", "长者", "third", trigger))

        self.assertEqual(engine.attempts, 2)
        self.assertEqual(len(engine.replies), 2)
        entry = worker._message_dispatch_ledger.entry("room-retry", "msg-retry")
        self.assertEqual(entry["reply"], "second")
        self.assertTrue(entry["final_reply_sent"])

    def test_planning_followup_uses_accumulated_goal_and_emits_stable_refs(self) -> None:
        scene_runtime = get_lanchat_scene_runtime()
        scene_runtime.clear_pending_planning()
        engine = _FakeReplyEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
            async_agent_execution=False,
        )
        try:
            worker._record_conversation_turn_context(
                {
                    "room_id": "room-context",
                    "message_id": "msg-context-1",
                    "sender_type": "user",
                    "message_kind": "chat",
                    "target_agent_name": "小女孩",
                },
                "@小女孩 围绕迪士尼乐园主题讨论一下",
            )
            worker._record_conversation_turn_context(
                {
                    "room_id": "room-context",
                    "message_id": "msg-context-2",
                    "sender_type": "user",
                    "message_kind": "chat",
                    "target_agent_name": "小女孩",
                },
                "@小女孩 按照迪士尼风格的卧室来设计呢",
            )
            trigger = {
                "room_id": "room-context",
                "message_id": "msg-context-3",
                "correlation_id": "corr-context-3",
                "sender_type": "user",
                "message_kind": "chat",
                "text": "@长者 请你给出一个方案",
                "agent_id": "elder-id",
                "agent_name": "长者",
                "target_agent_id": "elder-id",
                "target_agent_name": "长者",
                "metadata": {
                    "draft_action": "chat",
                    "target_scope": "agent",
                    "target_agent_id": "elder-id",
                    "target_agent_name": "长者",
                },
            }
            worker._record_conversation_turn_context(trigger, trigger["text"])

            handled = worker._handle_agent_trigger_planning_gate(trigger)

            self.assertTrue(handled)
            self.assertEqual(len(engine.replies), 1)
            self.assertEqual(engine.replies[0]["agent_name"], "长者")
            self.assertIn("迪士尼风格的卧室", engine.replies[0]["text"])
            self.assertNotIn("迪士尼乐园主题讨论", engine.replies[0]["text"])
            self.assertNotIn("我理解你的目标是：你给出一个方案", engine.replies[0]["text"])
            metadata = json.loads(engine.replies[0]["metadata_json"])
            self.assertTrue(metadata["agent_plan_id"].startswith("plan-"))
            self.assertEqual(metadata["artifact_ref"], f"legacy-plan:{metadata['agent_plan_id']}")
            context = worker._conversation_turn_contexts.get("room-context")
            self.assertEqual(context.active_agent_plan_id, metadata["agent_plan_id"])
            self.assertEqual(context.artifact_ref, metadata["artifact_ref"])
        finally:
            scene_runtime.clear_pending_planning()

    def test_confirmation_never_falls_back_to_another_agents_pending_plan(self) -> None:
        scene_runtime = get_lanchat_scene_runtime()
        scene_runtime.clear_pending_planning()
        engine = _FakeReplyEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
            async_agent_execution=False,
        )
        try:
            action, _, agent_name = scene_runtime.handle_targeted_planning_message(
                "小女孩",
                "设计一个迪士尼风格卧室",
                draft_action="chat",
            )
            self.assertEqual((action, agent_name), ("reply", "小女孩"))
            trigger = {
                "room_id": "room-plan-owner",
                "message_id": "msg-confirm-elder",
                "correlation_id": "corr-confirm-elder",
                "sender_type": "user",
                "message_kind": "chat",
                "text": "@长者 确认开始",
                "agent_id": "elder-id",
                "agent_name": "长者",
                "target_agent_id": "elder-id",
                "target_agent_name": "长者",
                "metadata": {
                    "draft_action": "chat",
                    "target_scope": "agent",
                    "target_agent_id": "elder-id",
                    "target_agent_name": "长者",
                },
            }

            handled = worker._handle_agent_trigger_planning_gate(trigger)

            self.assertTrue(handled)
            self.assertEqual(len(engine.replies), 1)
            self.assertEqual(engine.replies[0]["agent_name"], "GM")
            self.assertIn("未找到 长者 可确认的方案", engine.replies[0]["text"])
            pending = scene_runtime.pending_planning_snapshot()
            self.assertEqual(len(pending), 1)
            self.assertEqual(pending[0]["target_agent"], "小女孩")
        finally:
            scene_runtime.clear_pending_planning()

    def test_planning_confirmation_is_transactional_until_runtime_enqueue_finishes(self) -> None:
        scene_runtime = get_lanchat_scene_runtime()
        scene_runtime.clear_pending_planning()
        try:
            action, _, agent_name = scene_runtime.handle_targeted_planning_message(
                "长者",
                "设计一个迪士尼风格卧室，包含床、衣柜和书桌",
                draft_action="plan",
            )
            self.assertEqual((action, agent_name), ("reply", "长者"))
            reference = scene_runtime.pending_planning_reference("长者")

            action, _, agent_name = scene_runtime.handle_targeted_planning_message(
                reference["artifact_ref"],
                "确认开始",
                draft_action="generate",
            )
            self.assertEqual((action, agent_name), ("compose", "长者"))
            self.assertEqual(scene_runtime.pending_planning_snapshot(), [])
            self.assertTrue(scene_runtime.pending_planning_reference(reference["artifact_ref"]))

            self.assertTrue(scene_runtime.finalize_planning_confirmation(
                reference["artifact_ref"],
                succeeded=False,
            ))
            self.assertEqual(len(scene_runtime.pending_planning_snapshot()), 1)

            scene_runtime.handle_targeted_planning_message(
                reference["artifact_ref"],
                "确认开始",
                draft_action="generate",
            )
            self.assertTrue(scene_runtime.finalize_planning_confirmation(
                reference["artifact_ref"],
                succeeded=True,
            ))
            self.assertEqual(scene_runtime.pending_planning_snapshot(), [])
        finally:
            scene_runtime.clear_pending_planning()

    def test_gm_confirmation_uses_unique_pending_artifact_and_replies_as_gm(self) -> None:
        scene_runtime = get_lanchat_scene_runtime()
        scene_runtime.clear_pending_planning()
        engine = _FakeReplyEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
            async_agent_execution=False,
        )
        try:
            action, _, agent_name = scene_runtime.handle_targeted_planning_message(
                "小女孩",
                "设计一个迪士尼风格卧室，包含床、衣柜和书桌",
                draft_action="plan",
            )
            self.assertEqual((action, agent_name), ("reply", "小女孩"))
            reference = scene_runtime.pending_planning_reference("小女孩")
            trigger = {
                "room_id": "room-gm-confirm",
                "message_id": "msg-gm-confirm",
                "correlation_id": "corr-gm-confirm",
                "sender_type": "host",
                "sender_id": "host",
                "message_kind": "chat",
                "text": "@GM 确认生成",
                "agent_id": "gm",
                "agent_name": "GM",
                "target_agent_id": "gm",
                "target_agent_name": "GM",
                "legacy_route": True,
            }

            self.assertTrue(worker._handle_gm_pending_planning_confirmation(trigger))

            self.assertEqual(len(engine.replies), 1)
            self.assertEqual(engine.replies[0]["agent_name"], "GM")
            metadata = json.loads(engine.replies[0]["metadata_json"])
            self.assertEqual(metadata["proposal_id"], reference["agent_plan_id"])
            self.assertEqual(metadata["artifact_ref"], reference["artifact_ref"])
            self.assertEqual(metadata["reply_contract"], "runtime_write_blocked")
            self.assertNotIn("runtime_plan_id", metadata)
            state = worker._agent_runtime.query_state("room-gm-confirm")["room"]
            execution_ref = (
                f"{reference['agent_plan_id']}@{reference['proposal_version']}:"
                f"{reference['proposal_hash'].removeprefix('sha256:')}"
            )
            self.assertNotIn(execution_ref, state["external_plan_links"])
            self.assertEqual(len(scene_runtime.pending_planning_snapshot()), 1)
        finally:
            scene_runtime.clear_pending_planning()

    def test_native_gm_confirmation_replay_is_deduped_without_canonical_proposal(self) -> None:
        scene_runtime = get_lanchat_scene_runtime()
        scene_runtime.clear_pending_planning()
        engine = _FakeReplyEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
            async_agent_execution=False,
        )
        try:
            scene_runtime.handle_targeted_planning_message(
                "小女孩",
                "设计一个迪士尼风格卧室，包含床、衣柜和书桌",
                draft_action="plan",
            )
            message = {
                "room_id": "room-native-gm-confirm",
                "message_id": "msg-native-gm-confirm",
                "correlation_id": "corr-native-gm-confirm",
                "sender_type": "host",
                "sender_id": "host",
                "sender_name": "房主",
                "message_kind": "chat",
                "text": "@GM 确认生成",
                "agent_id": "gm",
                "agent_name": "GM",
                "target_agent_id": "gm",
                "target_agent_name": "GM",
                "metadata": {
                    "draft_action": "gm_control",
                    "target_scope": "gm",
                    "target_agent_id": "gm",
                    "target_agent_name": "GM",
                },
            }

            self.assertTrue(worker.sync_chat_message_to_coordinator(
                dict(message),
                source="lanchat_native_queue",
            ))
            batch_count_after_native = len(
                worker._agent_runtime.query_state("room-native-gm-confirm")["room"]["batch_plans"]
            )
            self.assertTrue(worker._process_trigger(dict(message)))

            self.assertEqual(len(engine.replies), 1)
            self.assertEqual(engine.replies[0]["agent_name"], "GM")
            metadata = json.loads(engine.replies[0]["metadata_json"])
            self.assertEqual(metadata["reply_contract"], "collaboration_blocked")
            entry = worker._message_dispatch_ledger.entry(
                "room-native-gm-confirm",
                "msg-native-gm-confirm",
            )
            self.assertEqual(entry["execution_owner"], "native_queue")
            self.assertTrue(entry["final_reply_sent"])
            state = worker._agent_runtime.query_state("room-native-gm-confirm")["room"]
            self.assertEqual(batch_count_after_native, 0)
            self.assertEqual(len(state["batch_plans"]), batch_count_after_native)
        finally:
            scene_runtime.clear_pending_planning()

    def test_explicit_agent_confirmation_is_blocked_by_runtime_write_boundary(self) -> None:
        scene_runtime = get_lanchat_scene_runtime()
        scene_runtime.clear_pending_planning()
        engine = _FakeReplyEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
            async_agent_execution=False,
        )
        try:
            scene_runtime.handle_targeted_planning_message(
                "长者",
                "设计一个迪士尼风格卧室，包含床、衣柜和书桌",
                draft_action="plan",
            )
            trigger = {
                "room_id": "room-agent-confirm",
                "message_id": "msg-agent-confirm",
                "correlation_id": "corr-agent-confirm",
                "sender_type": "host",
                "sender_id": "host",
                "message_kind": "chat",
                "text": "@长者 确认开始",
                "agent_id": "elder-id",
                "agent_name": "长者",
                "target_agent_id": "elder-id",
                "target_agent_name": "长者",
                "metadata": {
                    "draft_action": "chat",
                    "target_scope": "agent",
                    "target_agent_id": "elder-id",
                    "target_agent_name": "长者",
                },
            }

            self.assertTrue(worker._handle_agent_trigger_planning_gate(trigger))

            self.assertEqual(len(engine.replies), 1)
            self.assertEqual(engine.replies[0]["agent_name"], "GM")
            metadata = json.loads(engine.replies[0]["metadata_json"])
            self.assertEqual(metadata["reply_contract"], "runtime_write_blocked")
        finally:
            scene_runtime.clear_pending_planning()

    def test_source_context_agent_extracts_natural_basis_phrasing(self) -> None:
        self.assertEqual(
            LANChatAgentWorker._source_context_agent_from_text("在长者基础上改进"),
            "长者",
        )
        self.assertEqual(
            LANChatAgentWorker._source_context_agent_from_text("基于商人设计继续修改"),
            "商人",
        )

    def test_worker_can_build_runtime_with_scene_snapshot_provider_flag_without_main_workflow(self) -> None:
        class FakeSnapshotTool:
            def __init__(self) -> None:
                self.calls: list[dict] = []

            def invoke(self, payload: dict) -> dict:
                self.calls.append(dict(payload))
                return {
                    "scene_name": payload.get("scene_name") or "Scene/场景1.scene",
                    "actors": [
                        {
                            "actor_id": "actor-001",
                            "actor_guid": "actor-001",
                            "name": "藏宝箱",
                        }
                    ],
                    "bounds_ready": True,
                }

        snapshot_tool = FakeSnapshotTool()
        flags = AgentRuntimeFlags.from_env({
            "AGENT_RUNTIME_USE_SCENE_SNAPSHOT_PROVIDER": "1",
        })
        with patch.object(
            LANChatAgentWorker,
            "_get_runtime_tool",
            lambda self, name: snapshot_tool if name == "get_scene_snapshot" else None,
        ):
            worker = _TestWorker(
                corona_engine=object(),
                agent_runtime_flags=flags,
            )

        snapshot = worker._agent_runtime.refresh_scene_snapshot("room-snapshot-provider")
        room = worker._agent_runtime.query_state("room-snapshot-provider")["room"]

        self.assertEqual(snapshot["graph"]["status"], "completed")
        self.assertEqual(snapshot["snapshot_summary"]["observed_actor_count"], 1)
        self.assertTrue(snapshot_tool.calls)
        self.assertIn("actor-001", room["observed_actors"])
        self.assertEqual(
            worker._agent_runtime.status_summary("room-snapshot-provider")["provider_summary"]["scene_snapshot"]["mode"],
            "adapter",
        )
        self.assertEqual(
            worker._agent_runtime.status_summary("room-snapshot-provider")["provider_summary"]["scene_snapshot"]["status"],
            "enabled",
        )
        self.assertFalse(flags.can_call_legacy_main_workflow())

    def test_worker_records_requested_provider_unavailable_without_engine(self) -> None:
        flags = AgentRuntimeFlags.from_env({
            "AGENT_RUNTIME_USE_SCENE_SNAPSHOT_PROVIDER": "1",
            "AGENT_RUNTIME_USE_ENGINE_ENVIRONMENT_IMPORT_PROVIDER": "1",
            "AGENT_RUNTIME_USE_ENGINE_IMPORT_PROVIDER": "1",
            "AGENT_RUNTIME_USE_ENGINE_DELETE_PROVIDER": "1",
            "AGENT_RUNTIME_USE_ENGINE_TRANSFORM_PROVIDER": "1",
        })

        worker = _TestWorker(agent_runtime_flags=flags)

        provider_summary = worker._agent_runtime.status_summary("room-provider-missing-engine")["provider_summary"]
        self.assertEqual(provider_summary["scene_snapshot"]["mode"], "mock_empty")
        self.assertTrue(provider_summary["scene_snapshot"]["requested"])
        self.assertEqual(provider_summary["scene_snapshot"]["status"], "unavailable")
        self.assertEqual(provider_summary["scene_snapshot"]["reason"], "missing_engine")
        self.assertEqual(provider_summary["actor_import"]["mode"], "engine_actor_import_required_unavailable")
        self.assertEqual(provider_summary["actor_import"]["status"], "unavailable")
        self.assertEqual(provider_summary["actor_import"]["reason"], "missing_engine")
        self.assertEqual(provider_summary["actor_delete"]["mode"], "runtime_state_only")
        self.assertEqual(provider_summary["actor_delete"]["reason"], "missing_engine")
        self.assertEqual(
            provider_summary["environment_import"]["mode"],
            "engine_environment_import_required_unavailable",
        )
        self.assertEqual(provider_summary["environment_import"]["reason"], "missing_engine")
        self.assertEqual(provider_summary["layout_transform"]["mode"], "runtime_state_only")
        self.assertEqual(provider_summary["layout_transform"]["reason"], "missing_engine")

    def test_worker_records_requested_provider_unavailable_when_tool_missing(self) -> None:
        flags = AgentRuntimeFlags.from_env({
            "AGENT_RUNTIME_USE_IMAGE_PROVIDER": "1",
            "AGENT_RUNTIME_USE_MODEL_PROVIDER": "1",
            "AGENT_RUNTIME_USE_SCENE_REVIEW_PROVIDER": "1",
            "AGENT_RUNTIME_USE_ENVIRONMENT_PROVIDER": "1",
            "AGENT_RUNTIME_USE_ENGINE_ENVIRONMENT_IMPORT_PROVIDER": "1",
            "AGENT_RUNTIME_USE_ENGINE_DELETE_PROVIDER": "1",
        })
        gate_module = types.ModuleType("plugins.AITool.cai_extensions.agent.engine_write_gate")
        gate_module.get_engine_write_gate = lambda: object()
        with patch.dict(
            sys.modules,
            {
                "plugins.AITool.cai_extensions.agent.engine_write_gate": gate_module,
            },
        ), patch.object(
            LANChatAgentWorker,
            "_get_runtime_tool",
            lambda self, name: None,
        ):
            worker = _TestWorker(corona_engine=object(), agent_runtime_flags=flags)

        provider_summary = worker._agent_runtime.status_summary("room-provider-missing-tool")["provider_summary"]
        self.assertEqual(provider_summary["image_resource"]["mode"], "mock_planned")
        self.assertTrue(provider_summary["image_resource"]["requested"])
        self.assertEqual(provider_summary["image_resource"]["status"], "unavailable")
        self.assertEqual(provider_summary["image_resource"]["reason"], "missing_tool:generate_image")
        self.assertEqual(provider_summary["model_resource"]["mode"], "mock_adapter_model")
        self.assertTrue(provider_summary["model_resource"]["requested"])
        self.assertEqual(provider_summary["model_resource"]["status"], "unavailable")
        self.assertEqual(provider_summary["model_resource"]["reason"], "missing_tool:hunyuan_generate_3d")
        self.assertEqual(provider_summary["review"]["mode"], "runtime_geometry_rules")
        self.assertEqual(provider_summary["review"]["reason"], "missing_tool:scene_rationality_review")
        self.assertEqual(provider_summary["vlm_review"]["mode"], "disabled")
        self.assertTrue(provider_summary["vlm_review"]["requested"])
        self.assertEqual(provider_summary["vlm_review"]["status"], "unavailable")
        self.assertEqual(provider_summary["vlm_review"]["reason"], "missing_tool:scene_rationality_review")
        self.assertEqual(provider_summary["environment_component"]["mode"], "runtime_component_facts")
        self.assertEqual(
            provider_summary["environment_component"]["reason"],
            "missing_tool:create_environment_component",
        )
        self.assertEqual(
            provider_summary["environment_import"]["mode"],
            "engine_environment_import_required_unavailable",
        )
        self.assertTrue(provider_summary["environment_import"]["requested"])
        self.assertEqual(provider_summary["environment_import"]["status"], "unavailable")
        self.assertEqual(
            provider_summary["environment_import"]["reason"],
            "missing_tool:import_environment_component",
        )
        self.assertEqual(provider_summary["actor_delete"]["mode"], "runtime_state_only")
        self.assertTrue(provider_summary["actor_delete"]["requested"])
        self.assertEqual(provider_summary["actor_delete"]["status"], "unavailable")
        self.assertEqual(provider_summary["actor_delete"]["reason"], "missing_tool:remove_actor")

    def test_runtime_direct_tool_loader_registers_p0_model_and_import_tools(self) -> None:
        class FakeTool:
            def __init__(self, name: str) -> None:
                self.name = name

        class FakeRegistry:
            def __init__(self) -> None:
                self.tools: dict[str, Any] = {}

            def get(self, name: str):
                return self.tools.get(name)

            def register(self, tool, *, overwrite: bool = False) -> None:  # noqa: ANN001
                if not overwrite and tool.name in self.tools:
                    raise ValueError(tool.name)
                self.tools[tool.name] = tool

        registry = FakeRegistry()
        worker = object.__new__(LANChatAgentWorker)
        worker._logger = __import__("logging").getLogger("test-runtime-direct-tool-loader")
        config = types.SimpleNamespace(
            hunyuan3d=types.SimpleNamespace(enable=True, api_keys=["test-key"])
        )
        model_import_module = types.ModuleType(
            "plugins.AITool.cai_extensions.mcp.tools.model_import_tools"
        )
        model_import_module.load_model_import_tools = lambda: [
            FakeTool("import_model"),
            FakeTool("import_environment_component"),
            FakeTool("remove_model"),
        ]
        hunyuan_module = types.ModuleType(
            "Quasar.ai_modules.three_d_generate.tools.model_tools"
        )
        hunyuan_module.load_hunyuan3d_tools = lambda cfg: [
            FakeTool("hunyuan_generate_3d")
        ] if getattr(getattr(cfg, "hunyuan3d", None), "enable", False) else []

        with patch.dict(
            sys.modules,
            {
                "plugins.AITool.cai_extensions.mcp.tools.model_import_tools": model_import_module,
                "Quasar.ai_modules.three_d_generate.tools.model_tools": hunyuan_module,
            },
        ):
            self.assertEqual(
                worker._load_runtime_tool_direct(registry, config, "import_environment_component").name,
                "import_environment_component",
            )
            self.assertEqual(
                worker._load_runtime_tool_direct(registry, config, "import_model").name,
                "import_model",
            )
            self.assertEqual(
                worker._load_runtime_tool_direct(registry, config, "hunyuan_generate_3d").name,
                "hunyuan_generate_3d",
            )

    def test_hunyuan3d_tool_loader_accepts_dict_settings(self) -> None:
        from plugins.AITool.Quasar.ai_modules.three_d_generate.tools import model_tools

        class FakeClient:
            def __init__(self, **kwargs) -> None:
                self.kwargs = dict(kwargs)

        class FakeRegistry:
            def resolve(self, file_id: str) -> str:
                return f"mock://{file_id}"

            def register(self, **kwargs) -> str:
                return "fileid://mock"

        config = types.SimpleNamespace(
            hunyuan3d={
                "enable": True,
                "api_keys": ["runtime-test-key-123"],
                "max_concurrent_generations": 1,
            }
        )
        with patch.object(model_tools, "Hunyuan3DClient", FakeClient), patch.object(
            model_tools,
            "get_media_registry",
            lambda: FakeRegistry(),
        ):
            tools = model_tools.load_hunyuan3d_tools(config)

        self.assertTrue(tools)
        self.assertIn("hunyuan_generate_3d", {getattr(tool, "name", "") for tool in tools})

    def test_f5_runtime_provider_env_defaults_enable_minimum_engine_bridge_flags(self) -> None:
        env: dict[str, str] = {
            "AGENT_RUNTIME_USE_ENGINE_TRANSFORM_PROVIDER": "0",
        }

        install_f5_runtime_provider_env_defaults(env)

        self.assertEqual(env["AGENT_RUNTIME_ENABLED"], "1")
        self.assertEqual(env["OLD_WORKFLOW_DIRECT_ENTRY_DISABLED"], "1")
        self.assertEqual(env["ALLOW_LEGACY_FUNCTION_ADAPTER"], "1")
        self.assertEqual(env["ALLOW_LEGACY_MAIN_WORKFLOW"], "0")
        self.assertEqual(env["AGENT_RUNTIME_USE_MODEL_PROVIDER"], "1")
        self.assertEqual(env["AGENT_RUNTIME_USE_ENGINE_IMPORT_PROVIDER"], "1")
        self.assertEqual(env["AGENT_RUNTIME_USE_ENGINE_ENVIRONMENT_IMPORT_PROVIDER"], "1")
        self.assertEqual(env["AGENT_RUNTIME_USE_SCENE_SNAPSHOT_PROVIDER"], "1")
        self.assertEqual(env["AGENT_RUNTIME_USE_ENGINE_TRANSFORM_PROVIDER"], "0")

    def test_aitool_installs_f5_runtime_provider_defaults_before_worker_creation(self) -> None:
        main_path = REPO_ROOT / "editor" / "plugins" / "AITool" / "main.py"
        source = main_path.read_text(encoding="utf-8")
        module = ast.parse(source)
        runtime_builder = next(
            node
            for node in module.body
            if isinstance(node, ast.ClassDef) and node.name == "AITool"
            for node in node.body
            if isinstance(node, ast.FunctionDef) and node.name == "_build_runtime"
        )
        install_line = 0
        worker_line = 0
        imported_install = False
        for node in ast.walk(runtime_builder):
            if isinstance(node, ast.ImportFrom) and node.module == "services.agent_runtime.flags":
                imported_install = any(
                    alias.name == "install_f5_runtime_provider_env_defaults"
                    for alias in node.names
                )
            if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call):
                func = node.value.func
                if isinstance(func, ast.Name) and func.id == "install_f5_runtime_provider_env_defaults":
                    install_line = int(getattr(node, "lineno", 0) or 0)
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "LANChatAgentWorker":
                worker_line = int(getattr(node, "lineno", 0) or 0)

        self.assertTrue(imported_install)
        self.assertGreater(install_line, 0)
        self.assertGreater(worker_line, 0)
        self.assertLess(install_line, worker_line)

    def test_runtime_engine_tool_loader_does_not_stop_after_model_import_only(self) -> None:
        class FakeRegistry:
            def __init__(self) -> None:
                self._loaders = [types.SimpleNamespace(source="cai_extensions.mcp.model_import")]
                self._discovered = True

        class FakeLogger:
            def debug(self, *args, **kwargs) -> None:  # noqa: ANN002, ANN003
                return None

        worker = LANChatAgentWorker.__new__(LANChatAgentWorker)
        worker._logger = FakeLogger()
        registry = FakeRegistry()
        calls: list[str] = []

        def fake_register_extra(registrar) -> None:
            calls.append("extra")

        def fake_register_engine_loaders(target_registry) -> None:
            calls.append("engine")
            target_registry._loaders.extend([
                types.SimpleNamespace(source="cai_extensions.mcp.scene_review"),
                types.SimpleNamespace(source="cai_extensions.mcp.scene_snapshot"),
                types.SimpleNamespace(source="cai_extensions.mcp.set_actor_transform"),
            ])

        quasar_module = types.ModuleType("Quasar")
        ai_tools_module = types.ModuleType("Quasar.ai_tools")
        load_tools_module = types.ModuleType("Quasar.ai_tools.load_tools")
        load_tools_module.register_extra_builtin_registrar = fake_register_extra
        engine_tools_module = types.ModuleType("plugins.AITool.cai_extensions.engine_tools")
        engine_tools_module.register_engine_loaders = fake_register_engine_loaders
        with patch.dict(
            sys.modules,
            {
                "Quasar": quasar_module,
                "Quasar.ai_tools": ai_tools_module,
                "Quasar.ai_tools.load_tools": load_tools_module,
                "plugins.AITool.cai_extensions.engine_tools": engine_tools_module,
            },
        ):
            worker._ensure_runtime_engine_tool_loaders(registry)

        self.assertEqual(calls, ["extra", "engine"])
        self.assertFalse(registry._discovered)
        self.assertIn(
            "cai_extensions.mcp.set_actor_transform",
            {spec.source for spec in registry._loaders},
        )

    def test_runtime_hunyuan_direct_loader_prefers_canonical_namespace(self) -> None:
        class FakeTool:
            def __init__(self, name: str) -> None:
                self.name = name

        class FakeRegistry:
            def __init__(self) -> None:
                self.tools: dict[str, Any] = {}

            def get(self, name: str):
                return self.tools.get(name)

            def register(self, tool, overwrite: bool = False) -> None:
                if overwrite or tool.name not in self.tools:
                    self.tools[tool.name] = tool

        class FakeLogger:
            def debug(self, *args, **kwargs) -> None:  # noqa: ANN002, ANN003
                return None

        worker = LANChatAgentWorker.__new__(LANChatAgentWorker)
        worker._logger = FakeLogger()
        registry = FakeRegistry()
        config = types.SimpleNamespace(hunyuan3d=types.SimpleNamespace(enable=True))
        plugin_module = types.ModuleType(
            "plugins.AITool.Quasar.ai_modules.three_d_generate.tools.model_tools"
        )
        top_level_module = types.ModuleType(
            "Quasar.ai_modules.three_d_generate.tools.model_tools"
        )
        plugin_module.load_hunyuan3d_tools = lambda cfg: []
        top_level_module.load_hunyuan3d_tools = lambda cfg: [FakeTool("hunyuan_generate_3d")]

        with patch.dict(
            sys.modules,
            {
                "plugins.AITool.Quasar.ai_modules.three_d_generate.tools.model_tools": plugin_module,
                "Quasar.ai_modules.three_d_generate.tools.model_tools": top_level_module,
            },
        ):
            tool = worker._load_runtime_tool_direct(registry, config, "hunyuan_generate_3d")

        self.assertIsNotNone(tool)
        self.assertEqual(tool.name, "hunyuan_generate_3d")

    def test_runtime_ai_config_binds_canonical_quasar_namespace(self) -> None:
        worker = LANChatAgentWorker.__new__(LANChatAgentWorker)
        worker._logger = __import__("logging").getLogger("test-runtime-ai-config")
        expected = object()
        quasar = types.ModuleType("Quasar")
        quasar.__path__ = []
        ai_config_package = types.ModuleType("Quasar.ai_config")
        ai_config_package.__path__ = []
        ai_config_module = types.ModuleType("Quasar.ai_config.ai_config")
        ai_config_module.get_ai_config = lambda: expected

        with patch.dict(
            sys.modules,
            {
                "Quasar": quasar,
                "Quasar.ai_config": ai_config_package,
                "Quasar.ai_config.ai_config": ai_config_module,
            },
        ):
            worker._bind_runtime_ai_config()

        self.assertIs(worker._runtime_ai_config_override, expected)

    def test_runtime_ai_config_loads_canonical_namespace_once_per_process(self) -> None:
        first = LANChatAgentWorker.__new__(LANChatAgentWorker)
        second = LANChatAgentWorker.__new__(LANChatAgentWorker)
        first._logger = __import__("logging").getLogger("test-runtime-ai-config-first")
        second._logger = __import__("logging").getLogger("test-runtime-ai-config-second")
        imported_names: list[str] = []
        original_import = __import__

        def tracking_import(name, globals=None, locals=None, fromlist=(), level=0):  # noqa: ANN001
            imported_names.append(str(name))
            return original_import(name, globals, locals, fromlist, level)

        quasar = types.ModuleType("Quasar")
        quasar.__path__ = []
        ai_modules = types.ModuleType("Quasar.ai_modules")
        ai_modules.__path__ = []
        three_d_generate = types.ModuleType("Quasar.ai_modules.three_d_generate")
        three_d_generate.__path__ = []
        tools = types.ModuleType("Quasar.ai_modules.three_d_generate.tools")
        tools.__path__ = []
        loader = types.ModuleType("Quasar.ai_modules.three_d_generate.tools.loader")
        tools.loader = loader
        aitool_utils = types.ModuleType("plugins.AITool.utils")
        aitool_utils.__path__ = []
        ai_setting = types.ModuleType("plugins.AITool.utils.ai_setting")
        aitool_utils.ai_setting = ai_setting
        aitool_package = sys.modules["plugins.AITool"]

        LANChatAgentWorker._quasar_config_process_loaded = False
        try:
            with (
                patch.dict(
                    sys.modules,
                    {
                        "Quasar": quasar,
                        "Quasar.ai_modules": ai_modules,
                        "Quasar.ai_modules.three_d_generate": three_d_generate,
                        "Quasar.ai_modules.three_d_generate.tools": tools,
                        "Quasar.ai_modules.three_d_generate.tools.loader": loader,
                        "plugins.AITool.utils": aitool_utils,
                        "plugins.AITool.utils.ai_setting": ai_setting,
                    },
                ),
                patch.object(aitool_package, "utils", aitool_utils, create=True),
                patch("builtins.__import__", side_effect=tracking_import),
                patch.object(first, "_ensure_runtime_quasar_import_path") as first_path,
                patch.object(first, "_bind_runtime_ai_config") as first_bind,
                patch.object(second, "_ensure_runtime_quasar_import_path") as second_path,
                patch.object(second, "_bind_runtime_ai_config") as second_bind,
            ):
                first._ensure_runtime_ai_config_loaded()
                second._ensure_runtime_ai_config_loaded()
        finally:
            LANChatAgentWorker._quasar_config_process_loaded = False

        first_path.assert_called_once_with()
        first_bind.assert_called_once_with()
        second_path.assert_not_called()
        second_bind.assert_not_called()
        self.assertTrue(first._runtime_ai_config_loaded)
        self.assertTrue(second._runtime_ai_config_loaded)
        self.assertFalse(
            any(name.startswith("plugins.AITool.Quasar") for name in imported_names)
        )

    def test_production_sources_use_only_canonical_quasar_import_root(self) -> None:
        production_paths = [
            REPO_ROOT / "editor" / "main.py",
            REPO_ROOT / "editor" / "plugins" / "AITool" / "main.py",
            REPO_ROOT / "editor" / "plugins" / "AITool" / "services" / "lanchat_agent_worker.py",
        ]

        for path in production_paths:
            source = path.read_text(encoding="utf-8")
            self.assertNotIn("plugins.AITool.Quasar", source, path)
        editor_source = production_paths[0].read_text(encoding="utf-8")
        aitool_source = production_paths[1].read_text(encoding="utf-8")
        self.assertNotIn("warmup_all", editor_source)
        self.assertIn("from Quasar.ai_tools import warmup as warmup_module", aitool_source)
        self.assertIn("warmup_module.warmup_all", aitool_source)

    def test_runtime_direct_engine_tool_overrides_stale_import_model(self) -> None:
        class FakeTool:
            def __init__(self, name: str, source: str) -> None:
                self.name = name
                self.source = source

        class FakeRegistry:
            def __init__(self) -> None:
                self.tools: dict[str, Any] = {
                    "import_model": FakeTool("import_model", "stale-mainview"),
                }

            def get(self, name: str):
                return self.tools.get(name)

            def register(self, tool, overwrite: bool = False) -> None:
                if overwrite or tool.name not in self.tools:
                    self.tools[tool.name] = tool

        class FakeLogger:
            def debug(self, *args, **kwargs) -> None:  # noqa: ANN002, ANN003
                return None

        worker = LANChatAgentWorker.__new__(LANChatAgentWorker)
        worker._logger = FakeLogger()
        registry = FakeRegistry()
        mcp_module = types.ModuleType(
            "plugins.AITool.cai_extensions.mcp.tools.model_import_tools"
        )
        mcp_module.load_model_import_tools = lambda: [
            FakeTool("import_model", "runtime-mcp-native"),
            FakeTool("remove_model", "runtime-mcp-native"),
        ]

        with patch.dict(
            sys.modules,
            {
                "plugins.AITool.cai_extensions.mcp.tools.model_import_tools": mcp_module,
            },
        ):
            tool = worker._load_runtime_tool_direct(registry, None, "import_model")

        self.assertIsNotNone(tool)
        self.assertEqual(tool.name, "import_model")
        self.assertEqual(tool.source, "runtime-mcp-native")

    def test_f5_runtime_provider_env_defaults_reach_worker_provider_summary(self) -> None:
        class FakeTool:
            def __init__(self, name: str) -> None:
                self.name = name
                self.calls: list[dict[str, Any]] = []

            def invoke(self, payload: dict) -> dict:
                self.calls.append(dict(payload))
                if self.name == "get_scene_snapshot":
                    return {"scene_name": "Scene/provider.scene", "actors": []}
                if self.name == "hunyuan_generate_3d":
                    return {"local_path": f"E:/models/{payload.get('prompt') or 'object'}.glb"}
                if self.name == "import_environment_component":
                    return {
                        "status": "success",
                        "component_id": payload.get("component_id") or "env-1",
                        "component_type": payload.get("component_type") or "terrain",
                    }
                if self.name == "set_actor_transform":
                    return {"status": "success", "position": list(payload.get("position") or [0.0, 0.0, 0.0])}
                return {
                    "status": "success",
                    "actor": {
                        "actor_guid": "actor-f5",
                        "name": payload.get("actor_name") or payload.get("name") or "actor",
                    },
                }

        class FakeEngineWriteGate:
            def invoke_tool(self, tool, payload: dict) -> dict:
                return tool.invoke(payload)

            def set_transform(self, tool, payload: dict) -> dict:
                return tool.invoke(payload)

        env: dict[str, str] = {}
        install_f5_runtime_provider_env_defaults(env)
        flags = AgentRuntimeFlags.from_env(env)
        tools = {
            name: FakeTool(name)
            for name in (
                "get_scene_snapshot",
                "hunyuan_generate_3d",
                "import_model",
                "import_environment_component",
                "set_actor_transform",
            )
        }
        gate_module = types.ModuleType("plugins.AITool.cai_extensions.agent.engine_write_gate")
        gate_module.get_engine_write_gate = lambda: FakeEngineWriteGate()
        with patch.dict(
            sys.modules,
            {
                "plugins.AITool.cai_extensions.agent.engine_write_gate": gate_module,
            },
        ), patch.object(
            LANChatAgentWorker,
            "_get_runtime_tool",
            lambda self, name: tools.get(name),
        ):
            worker = _TestWorker(corona_engine=object(), agent_runtime_flags=flags)

        provider_summary = worker._agent_runtime.status_summary("room-f5-defaults")["provider_summary"]
        self.assertEqual(provider_summary["scene_snapshot"]["mode"], "adapter")
        self.assertEqual(provider_summary["model_resource"]["mode"], "adapter")
        self.assertEqual(provider_summary["environment_component"]["mode"], "runtime_component_facts")
        self.assertEqual(provider_summary["actor_import"]["mode"], "adapter")
        self.assertEqual(provider_summary["environment_import"]["mode"], "adapter")
        self.assertEqual(provider_summary["layout_transform"]["mode"], "adapter")
        self.assertEqual(provider_summary["actor_import"]["status"], "enabled")
        self.assertEqual(provider_summary["actor_import"]["reason"], "import_model")

    def test_f5_runtime_provider_env_defaults_execute_graph_through_engine_bridge(self) -> None:
        class FakeTool:
            def __init__(self, name: str) -> None:
                self.name = name
                self.calls: list[dict[str, Any]] = []

            def invoke(self, payload: dict) -> dict:
                self.calls.append(dict(payload))
                if self.name == "get_scene_snapshot":
                    actors: list[dict[str, Any]] = []
                    for index, import_call in enumerate(tools["import_model"].calls, start=1):
                        actor_name = import_call.get("actor_name") or import_call.get("name") or f"actor-{index}"
                        actors.append({
                            "actor_id": f"actor-{index}",
                            "actor_guid": f"actor-{index}",
                            "name": actor_name,
                            "position": list(import_call.get("position") or [0.0, 0.0, 0.0]),
                            "rotation": list(import_call.get("rotation") or [0.0, 0.0, 0.0]),
                            "scale": list(import_call.get("scale") or [1.0, 1.0, 1.0]),
                            "world_aabb": {"min": [-0.5, 0.0, -0.5], "max": [0.5, 1.0, 0.5]},
                            "bounds_ready": True,
                            "engine_lifecycle_status": "bounds_ready",
                            "render_status_observed": True,
                            "render_ready": True,
                            "render_failed": False,
                            "gpu_build_state": "Ready",
                            "mesh_count": 1,
                            "renderable_mesh_count": 1,
                            "invalid_mesh_count": 0,
                            "sync_status": "engine_imported",
                        })
                    for index, import_call in enumerate(
                        tools["import_environment_component"].calls,
                        start=1,
                    ):
                        actors.append({
                            "actor_id": f"environment-actor-{index}",
                            "actor_guid": f"environment-actor-{index}",
                            "name": import_call.get("name") or f"environment-{index}",
                            "position": list(import_call.get("position") or [0.0, 0.0, 0.0]),
                            "rotation": list(import_call.get("rotation") or [0.0, 0.0, 0.0]),
                            "scale": list(import_call.get("scale") or [1.0, 1.0, 1.0]),
                            "world_aabb": {"min": [-8.0, 0.0, -8.0], "max": [8.0, 0.05, 8.0]},
                            "bounds_ready": True,
                            "engine_lifecycle_status": "bounds_ready",
                            "render_status_observed": True,
                            "render_ready": True,
                            "render_failed": False,
                            "gpu_build_state": "Ready",
                            "mesh_count": 1,
                            "renderable_mesh_count": 1,
                            "invalid_mesh_count": 0,
                            "sync_status": "engine_imported",
                        })
                    return {"scene_name": "Scene/f5.scene", "actors": actors}
                if self.name == "generate_image":
                    return {
                        "image_url": (
                            "data:image/png;base64,"
                            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk"
                            "YAAAAAYAAjCB0C8AAAAASUVORK5CYII="
                        )
                    }
                if self.name == "hunyuan_generate_3d":
                    return {
                        "local_path": f"E:/models/{payload.get('prompt') or payload.get('object_name') or 'object'}.glb",
                        "model_id": f"model-{len(self.calls)}",
                    }
                if self.name == "import_environment_component":
                    return {
                        "status": "success",
                        "component_id": payload.get("component_id") or payload.get("object_id") or "env-1",
                        "actor_id": f"environment-actor-{len(self.calls)}",
                        "name": payload.get("name") or "environment",
                        "component_type": payload.get("component_type") or "environment",
                        "position": list(payload.get("position") or [0.0, 0.0, 0.0]),
                        "rotation": list(payload.get("rotation") or [0.0, 0.0, 0.0]),
                        "scale": list(payload.get("scale") or [1.0, 1.0, 1.0]),
                        "aabb": {"min": [-8.0, 0.0, -8.0], "max": [8.0, 0.05, 8.0]},
                        "sync_status": "engine_imported",
                    }
                if self.name == "set_actor_transform":
                    return {
                        "status": "success",
                        "actor_id": payload.get("actor_id") or payload.get("target") or "actor-transform",
                        "position": list(payload.get("position") or [0.0, 0.0, 0.0]),
                        "rotation": list(payload.get("rotation") or [0.0, 0.0, 0.0]),
                        "scale": list(payload.get("scale") or [1.0, 1.0, 1.0]),
                    }
                return {
                    "status": "success",
                    "actor": {
                        "actor_guid": f"actor-{len(self.calls)}",
                        "name": payload.get("actor_name") or payload.get("name") or "actor",
                        "position": list(payload.get("position") or [0.0, 0.0, 0.0]),
                        "rotation": list(payload.get("rotation") or [0.0, 0.0, 0.0]),
                        "scale": list(payload.get("scale") or [1.0, 1.0, 1.0]),
                        "world_aabb": {"min": [-0.5, 0.0, -0.5], "max": [0.5, 1.0, 0.5]},
                        "sync_status": "created",
                    },
                }

        class FakeEngineWriteGate:
            def invoke_tool(self, tool, payload: dict) -> dict:
                return tool.invoke(payload)

            def set_transform(self, tool, payload: dict) -> dict:
                return tool.invoke(payload)

        env: dict[str, str] = {}
        install_f5_runtime_provider_env_defaults(env)
        flags = AgentRuntimeFlags.from_env(env)
        tools = {
            name: FakeTool(name)
            for name in (
                "get_scene_snapshot",
                "generate_image",
                "hunyuan_generate_3d",
                "import_model",
                "import_environment_component",
                "set_actor_transform",
            )
        }
        gate_module = types.ModuleType("plugins.AITool.cai_extensions.agent.engine_write_gate")
        gate_module.get_engine_write_gate = lambda: FakeEngineWriteGate()
        with patch.dict(
            sys.modules,
            {
                "plugins.AITool.cai_extensions.agent.engine_write_gate": gate_module,
            },
        ), patch.object(
            LANChatAgentWorker,
            "_get_runtime_tool",
            lambda self, name: tools.get(name),
        ):
            worker = _TestWorker(corona_engine=object(), agent_runtime_flags=flags)

        plan = worker._agent_runtime.propose_scene_plan(
            room_id="room-f5-execute",
            text="生成一个小营地，有草地、天空、帐篷、小木桌。",
            owner_agent="小女孩",
        )
        worker._agent_runtime.confirm_scene_plan(plan.plan_id, confirmed_by="房主")
        queued = worker._agent_runtime.enqueue_planned_batches(plan.plan_id, max_items_per_batch=8)
        self.assertTrue(queued["graphs"])
        result = worker._agent_runtime.handle_message(
            room_id="room-f5-execute",
            text="runtime worker drain",
            action="worker_drain",
            external_plan_id=plan.plan_id,
            max_graphs=1,
        )
        report = result["report"]

        self.assertTrue(tools["hunyuan_generate_3d"].calls)
        self.assertTrue(tools["import_model"].calls)
        self.assertTrue(tools["import_environment_component"].calls)
        self.assertTrue(result["drain"]["graphs"])
        self.assertEqual(result["drain"]["graphs"][0]["status"], "completed")
        self.assertEqual(result["plan"]["status"], "completed")
        bridge_summary = report["engine_write_boundary_summary"]
        adapter_summary = report["engine_write_adapter_summary"]
        self.assertGreaterEqual(bridge_summary["bridge_call_count"], 2)
        self.assertGreaterEqual(bridge_summary["bridge_success_count"], 2)
        self.assertEqual(bridge_summary["bridge_failed_count"], 0)
        self.assertIn("engine_actor_import", bridge_summary["write_source_counts"])
        self.assertIn("engine_environment_import", bridge_summary["write_source_counts"])
        registry = report["scene_entity_registry"]
        self.assertEqual(
            adapter_summary["readiness_mismatch_count"],
            0,
            {"adapter": adapter_summary, "registry": registry},
        )
        self.assertEqual(
            set(adapter_summary["resolved_readiness_mismatch_channels"]),
            {"actor_import", "environment_import"},
        )
        self.assertGreaterEqual(registry["actor_count"], 1)
        self.assertEqual(registry["missing_transform_count"], 0)
        self.assertEqual(registry["missing_aabb_count"], 0)
        self.assertGreaterEqual(registry["actor_aabb_available_count"], 1)
        self.assertGreaterEqual(registry["aabb_available_count"], 2)
        self.assertGreaterEqual(registry["entity_type_counts"].get("environment", 0), 1)
        self.assertGreaterEqual(registry["game_ready_entity_count"], 2, registry["entities"])
        self.assertGreaterEqual(registry["materialization_status_counts"].get("engine_ready", 0), 2)
        self.assertEqual(
            registry["environment_count"],
            registry["entity_type_counts"].get("environment", 0),
        )
        self.assertGreaterEqual(registry["sync_status_counts"].get("created", 0), 1)
        self.assertGreaterEqual(registry["sync_status_counts"].get("engine_imported", 0), 1)
        evidence = LANChatAgentWorker._agent_runtime_evidence_summary(result)
        self.assertGreaterEqual(evidence["game_ready_entity_count"], 2)
        self.assertGreaterEqual(evidence["environment_count"], 1)
        self.assertEqual(evidence["engine_loading_entity_count"], 0)
        report_event = next(
            event
            for event in reversed(
                worker._agent_runtime.user_visible_events(
                    "room-f5-execute",
                    plan_id=plan.plan_id,
                )
            )
            if event["event_type"] == "report_ready"
        )
        self.assertIn("game_ready_entity_count", report_event["payload"], report_event)
        self.assertGreaterEqual(report_event["payload"]["game_ready_entity_count"], 2)
        self.assertGreaterEqual(report_event["payload"]["environment_entity_count"], 1)
        self.assertEqual(report_event["payload"]["engine_write_readiness_mismatch_count"], 0)
        self.assertEqual(report_event["payload"]["report_health_status"], "ok")

    def test_worker_preflight_all_runtime_provider_flags_use_narrow_adapters(self) -> None:
        class FakeTool:
            def __init__(self, name: str) -> None:
                self.name = name
                self.calls: list[dict] = []

            def invoke(self, payload: dict) -> dict:
                self.calls.append(dict(payload))
                if self.name == "get_scene_snapshot":
                    return {"scene_name": "Scene/provider.scene", "actors": []}
                if self.name == "generate_image":
                    return {"image_url": "mock://provider.png"}
                if self.name == "hunyuan_generate_3d":
                    return {"local_path": f"E:/models/{payload.get('prompt')}.glb"}
                if self.name == "scene_rationality_review":
                    return {"overall": "PASS", "issues": []}
                if self.name == "create_environment_component":
                    return {"component_id": "component-1", "status": "ready"}
                if self.name == "import_environment_component":
                    return {
                        "component_id": payload.get("component_id"),
                        "actor_id": f"actor-{payload.get('component_id')}",
                        "status": "success",
                    }
                if self.name == "import_model":
                    return {"actor": {"actor_guid": "actor-1", "name": payload.get("actor_name")}}
                if self.name == "remove_actor":
                    return {"status": "deleted", "actor_id": payload.get("actor_id")}
                if self.name == "set_actor_transform":
                    return {"position": list(payload.get("position") or [0.0, 0.0, 0.0])}
                return {}

        class FakeEngineWriteGate:
            def invoke_tool(self, tool, payload: dict) -> dict:
                return tool.invoke(payload)

            def set_transform(self, tool, payload: dict) -> dict:
                return tool.invoke(payload)

            def remove_actor(self, tool, payload: dict) -> dict:
                return tool.invoke(payload)

        tools = {
            name: FakeTool(name)
            for name in (
                "get_scene_snapshot",
                "generate_image",
                "hunyuan_generate_3d",
                "scene_rationality_review",
                "create_environment_component",
                "import_environment_component",
                "import_model",
                "remove_actor",
                "set_actor_transform",
            )
        }
        flags = AgentRuntimeFlags.from_env({
            "AGENT_RUNTIME_USE_SCENE_SNAPSHOT_PROVIDER": "1",
            "AGENT_RUNTIME_USE_IMAGE_PROVIDER": "1",
            "AGENT_RUNTIME_USE_MODEL_PROVIDER": "1",
            "AGENT_RUNTIME_USE_SCENE_REVIEW_PROVIDER": "1",
            "AGENT_RUNTIME_USE_ENVIRONMENT_PROVIDER": "1",
            "AGENT_RUNTIME_USE_ENGINE_ENVIRONMENT_IMPORT_PROVIDER": "1",
            "AGENT_RUNTIME_USE_ENGINE_IMPORT_PROVIDER": "1",
            "AGENT_RUNTIME_USE_ENGINE_DELETE_PROVIDER": "1",
            "AGENT_RUNTIME_USE_ENGINE_TRANSFORM_PROVIDER": "1",
            "ALLOW_LEGACY_MAIN_WORKFLOW": "1",
            "OLD_WORKFLOW_DIRECT_ENTRY_DISABLED": "1",
        })
        gate_module = types.ModuleType("plugins.AITool.cai_extensions.agent.engine_write_gate")
        gate_module.get_engine_write_gate = lambda: FakeEngineWriteGate()
        with patch.dict(
            sys.modules,
            {
                "plugins.AITool.cai_extensions.agent.engine_write_gate": gate_module,
            },
        ), patch.object(
            LANChatAgentWorker,
            "_get_runtime_tool",
            lambda self, name: tools.get(name),
        ):
            worker = _TestWorker(
                corona_engine=object(),
                agent_runtime_flags=flags,
            )

        provider_summary = worker._agent_runtime.status_summary("room-all-runtime-providers")["provider_summary"]

        self.assertFalse(flags.can_call_legacy_main_workflow())
        self.assertEqual(provider_summary["scene_snapshot"]["mode"], "adapter")
        self.assertEqual(provider_summary["image_resource"]["mode"], "adapter")
        self.assertEqual(provider_summary["model_resource"]["mode"], "adapter")
        self.assertEqual(provider_summary["review"]["mode"], "adapter")
        self.assertEqual(provider_summary["vlm_review"]["mode"], "adapter")
        self.assertEqual(provider_summary["environment_component"]["mode"], "adapter")
        self.assertEqual(provider_summary["environment_import"]["mode"], "adapter")
        self.assertEqual(provider_summary["actor_import"]["mode"], "adapter")
        self.assertEqual(provider_summary["actor_delete"]["mode"], "adapter")
        self.assertEqual(provider_summary["layout_transform"]["mode"], "adapter")
        for key in (
            "scene_snapshot",
            "image_resource",
            "model_resource",
            "review",
            "vlm_review",
            "environment_component",
            "environment_import",
            "actor_import",
            "actor_delete",
            "layout_transform",
        ):
            self.assertTrue(provider_summary[key]["requested"], key)
            self.assertEqual(provider_summary[key]["status"], "enabled", key)
        self.assertNotIn("legacy_workflow", str(provider_summary))

    def test_worker_enables_engine_actor_import_provider_by_default_when_tool_exists(self) -> None:
        class FakeTool:
            def __init__(self, name: str) -> None:
                self.name = name
                self.calls: list[dict] = []

            def invoke(self, payload: dict) -> dict:
                self.calls.append(dict(payload))
                if self.name == "hunyuan_generate_3d":
                    return {"local_path": f"E:/models/{payload.get('prompt') or 'object'}.glb"}
                if self.name == "import_environment_component":
                    return {
                        "status": "success",
                        "component_id": payload.get("component_id") or payload.get("object_id") or "env-1",
                        "name": payload.get("name") or "environment",
                        "component_type": payload.get("component_type") or "environment",
                    }
                return {
                    "status": "success",
                    "actor": {
                        "actor_guid": f"actor-{len(self.calls)}",
                        "name": payload.get("actor_name") or payload.get("name") or "actor",
                        "position": list(payload.get("position") or [0.0, 0.0, 0.0]),
                        "rotation": list(payload.get("rotation") or [0.0, 0.0, 0.0]),
                        "scale": list(payload.get("scale") or [1.0, 1.0, 1.0]),
                    },
                }

        class FakeEngineWriteGate:
            def invoke_tool(self, tool, payload: dict) -> dict:
                return tool.invoke(payload)

        import_tool = FakeTool("import_model")
        environment_import_tool = FakeTool("import_environment_component")
        model_tool = FakeTool("hunyuan_generate_3d")
        runtime_tools = {
            "import_model": import_tool,
            "import_environment_component": environment_import_tool,
            "hunyuan_generate_3d": model_tool,
        }
        gate_module = types.ModuleType("plugins.AITool.cai_extensions.agent.engine_write_gate")
        gate_module.get_engine_write_gate = lambda: FakeEngineWriteGate()

        with patch.dict(
            sys.modules,
            {
                "plugins.AITool.cai_extensions.agent.engine_write_gate": gate_module,
            },
        ), patch.object(
            LANChatAgentWorker,
            "_get_runtime_tool",
            lambda self, name: runtime_tools.get(name),
        ):
            worker = _TestWorker(
                corona_engine=object(),
                agent_runtime_flags=AgentRuntimeFlags.from_env({
                    "AGENT_RUNTIME_USE_MODEL_PROVIDER": "1",
                }),
            )

        provider_summary = worker._agent_runtime.status_summary("room-default-engine-import")["provider_summary"]
        self.assertEqual(provider_summary["actor_import"]["mode"], "adapter")
        self.assertTrue(provider_summary["actor_import"]["requested"])
        self.assertEqual(provider_summary["actor_import"]["status"], "enabled")
        self.assertEqual(provider_summary["actor_import"]["reason"], "import_model")
        self.assertEqual(provider_summary["environment_import"]["mode"], "runtime_state_only")
        self.assertNotIn("requested", provider_summary["environment_import"])
        self.assertEqual(provider_summary["model_resource"]["mode"], "adapter")

        plan = worker._agent_runtime.propose_scene_plan(
            room_id="room-default-engine-import",
            text="做一个简单森林营地，有草地、天空、帐篷、小木桌",
            owner_agent="小女孩",
        )
        worker._agent_runtime.confirm_scene_plan(plan.plan_id, confirmed_by="房主")
        result = worker._agent_runtime.execute_scene_plan(plan.plan_id)

        self.assertTrue(import_tool.calls)
        self.assertFalse(environment_import_tool.calls)
        self.assertTrue(result["graphs"])
        self.assertEqual(result["graphs"][0]["status"], "completed")
        report = worker._agent_runtime.generate_report("room-default-engine-import", plan_id=plan.plan_id)
        bridge_summary = report["engine_write_boundary_summary"]
        self.assertGreaterEqual(bridge_summary["bridge_call_count"], 1)
        self.assertGreaterEqual(bridge_summary["bridge_success_count"], 1)
        self.assertEqual(bridge_summary["bridge_failed_count"], 0)
        self.assertIn("engine_actor_import", bridge_summary["write_source_counts"])
        self.assertGreaterEqual(bridge_summary["environment_import_boundary_count"], 1)

    def test_worker_does_not_enable_engine_actor_import_without_model_resource_provider(self) -> None:
        class FakeImportTool:
            def __init__(self) -> None:
                self.calls: list[dict] = []

            def invoke(self, payload: dict) -> dict:
                self.calls.append(dict(payload))
                return {
                    "status": "success",
                    "actor": {
                        "actor_guid": f"actor-{len(self.calls)}",
                        "name": payload.get("actor_name") or "actor",
                    },
                }

        import_tool = FakeImportTool()
        flags = AgentRuntimeFlags.from_env({
            "AGENT_RUNTIME_ENABLED": "1",
            "AGENT_RUNTIME_USE_MODEL_PROVIDER": "1",
        })
        with patch.object(
            LANChatAgentWorker,
            "_get_runtime_tool",
            lambda self, name: import_tool if name == "import_model" else None,
        ):
            worker = _TestWorker(
                corona_engine=object(),
                agent_runtime_flags=flags,
            )

        provider_summary = worker._agent_runtime.status_summary("room-import-without-model-provider")["provider_summary"]
        self.assertNotEqual(provider_summary["actor_import"]["mode"], "adapter")
        self.assertFalse(import_tool.calls)

        plan = worker._agent_runtime.propose_scene_plan(
            room_id="room-import-without-model-provider",
            text="做一个简单营地，有帐篷、小木桌",
            owner_agent="小女孩",
        )
        worker._agent_runtime.confirm_scene_plan(plan.plan_id, confirmed_by="房主")
        result = worker._agent_runtime.execute_scene_plan(plan.plan_id)

        self.assertFalse(import_tool.calls)
        self.assertTrue(result["graphs"])
        self.assertEqual(result["graphs"][0]["status"], "completed")
        report = worker._agent_runtime.generate_report(
            "room-import-without-model-provider",
            plan_id=plan.plan_id,
        )
        bridge_summary = report["engine_write_boundary_summary"]
        self.assertEqual(bridge_summary["bridge_call_count"], 0)
        self.assertGreaterEqual(bridge_summary["status_counts"].get("runtime_state_only", 0), 1)
        self.assertGreaterEqual(report["scene_entity_registry"]["actor_count"], 1)

    def test_worker_can_build_runtime_with_image_provider_flag_without_main_workflow(self) -> None:
        class FakeImageTool:
            def __init__(self) -> None:
                self.calls: list[dict] = []

            def invoke(self, payload: dict) -> dict:
                self.calls.append(dict(payload))
                return {"image_url": f"mock://{payload['object_name']}.png"}

        image_tool = FakeImageTool()
        flags = AgentRuntimeFlags.from_env({
            "AGENT_RUNTIME_USE_IMAGE_PROVIDER": "1",
        })
        with patch.object(
            LANChatAgentWorker,
            "_get_runtime_tool",
            lambda self, name: image_tool if name == "generate_image" else None,
        ):
            worker = _TestWorker(agent_runtime_flags=flags)

        reply = worker._start_active_coordinator_generation(
            _FakeCoordinator(),
            room_id="room-image-provider",
            host_id="房主",
        )
        worker._agent_runtime.handle_message(
            room_id="room-image-provider",
            text="worker drain",
            action="worker_drain",
            max_graphs=16,
        )
        state = worker._agent_runtime.query_state("room-image-provider")["room"]
        image_plans = state["image_resource_plans"]

        self.assertIn("【AgentRuntime 执行结果】ScenePlan", reply or "")
        self.assertTrue(image_tool.calls)
        self.assertTrue(image_plans)
        self.assertFalse(flags.can_call_legacy_main_workflow())

    def test_worker_can_build_runtime_with_scene_review_provider_flag_without_main_workflow(self) -> None:
        class FakeReviewTool:
            def __init__(self) -> None:
                self.calls: list[dict] = []

            def invoke(self, payload: dict) -> dict:
                self.calls.append(dict(payload))
                return {"overall": "PASS", "issues": []}

        review_tool = FakeReviewTool()
        flags = AgentRuntimeFlags.from_env({
            "AGENT_RUNTIME_USE_SCENE_REVIEW_PROVIDER": "1",
        })
        with patch.object(
            LANChatAgentWorker,
            "_get_runtime_tool",
            lambda self, name: review_tool if name == "scene_rationality_review" else None,
        ):
            worker = _TestWorker(agent_runtime_flags=flags)

        reply = worker._start_active_coordinator_generation(
            _FakeCoordinator(),
            room_id="room-review-provider",
            host_id="房主",
        )
        worker._agent_runtime.handle_message(
            room_id="room-review-provider",
            text="worker drain",
            action="worker_drain",
            max_graphs=16,
        )
        state = worker._agent_runtime.query_state("room-review-provider")["room"]
        reviews = state["geometry_reviews"]

        self.assertIn("【AgentRuntime 执行结果】ScenePlan", reply or "")
        self.assertTrue(reviews)
        self.assertFalse(review_tool.calls)
        self.assertEqual(
            worker._agent_runtime.status_summary("room-review-provider")["provider_summary"]["review"]["mode"],
            "adapter",
        )
        self.assertEqual(
            worker._agent_runtime.status_summary("room-review-provider")["provider_summary"]["review"]["status"],
            "enabled",
        )
        self.assertFalse(flags.can_call_legacy_main_workflow())

    def test_worker_can_build_runtime_with_legacy_model_adapter_flag_without_main_workflow(self) -> None:
        flags = AgentRuntimeFlags.from_env({
            "AGENT_RUNTIME_USE_LEGACY_MODEL_PROVIDER": "1",
        })
        worker = _TestWorker(agent_runtime_flags=flags)

        self.assertIsNotNone(worker._agent_runtime._model_resource_provider)
        self.assertFalse(flags.can_call_legacy_main_workflow())

    def test_default_agent_runtime_flags_route_generation_to_agent_runtime(self) -> None:
        coordinator = _FakeCoordinator()
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))

        reply = worker._start_active_coordinator_generation(
            coordinator,
            room_id="room-1",
            host_id="房主",
        )

        self.assertIn("【AgentRuntime 执行结果】ScenePlan", reply or "")
        self.assertEqual(coordinator.execute_calls, [])
        runtime_state = worker._agent_runtime.query_state("room-1")["room"]
        self.assertEqual(runtime_state["external_plan_links"]["seed-test"], runtime_state["active_plan_id"])
        import_nodes = [
            node
            for graph in runtime_state["tool_graphs"].values()
            for node in graph["nodes"].values()
            if node["tool_name"] == "runtime.actor.import_batch"
        ]
        self.assertTrue(import_nodes)
        self.assertEqual(import_nodes[0]["args"]["scene_name"], "Scene/场景1.scene")

    def test_default_agent_runtime_generation_does_not_touch_legacy_scheduler(self) -> None:
        class _ForbiddenLegacyWorker(_TestWorker):
            def _get_generation_scheduler(self):  # noqa: ANN001
                raise AssertionError("legacy scheduler must not be touched by default AgentRuntime path")

        coordinator = _FakeCoordinator()
        worker = _ForbiddenLegacyWorker(
            composer_factory=lambda: object(),
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        reply = worker._start_active_coordinator_generation(
            coordinator,
            room_id="room-no-legacy-scheduler",
            host_id="房主",
        )

        self.assertIn("【AgentRuntime 执行结果】ScenePlan", reply or "")
        self.assertIsNone(worker._generation_scheduler)
        self.assertEqual(coordinator.execute_calls, [])

    def test_default_interaction_coordinator_does_not_construct_legacy_scheduler(self) -> None:
        composer_factory_calls = 0

        def composer_factory():
            nonlocal composer_factory_calls
            composer_factory_calls += 1
            return object()

        worker = _TestWorker(
            composer_factory=composer_factory,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        coordinator = worker._get_interaction_coordinator()

        self.assertIsNotNone(coordinator)
        self.assertIsNone(worker._generation_scheduler)
        self.assertEqual(composer_factory_calls, 0)

    def test_default_lazy_generation_scheduler_boundary_returns_none(self) -> None:
        composer_factory_calls = 0

        def composer_factory():
            nonlocal composer_factory_calls
            composer_factory_calls += 1
            return object()

        worker = _TestWorker(
            composer_factory=composer_factory,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        scheduler = worker._get_generation_scheduler()

        self.assertIsNone(scheduler)
        self.assertIsNone(worker._generation_scheduler)
        self.assertEqual(composer_factory_calls, 0)

    def test_default_runtime_mode_blocks_injected_legacy_scheduler_state_and_cancel(self) -> None:
        class _InjectedLegacyScheduler:
            def public_snapshot(self) -> dict:
                return {"queue_size": 99}

            def public_session_snapshot(self, session_id: str) -> dict:
                return {"session_id": session_id, "status": "running"}

            def cancel_session(self, session_id: str, *, abandon_remote: bool = False) -> dict:
                return {"session_id": session_id, "cancelled": True}

        worker = _TestWorker(
            generation_scheduler=_InjectedLegacyScheduler(),
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        snapshot = worker.generation_scheduler_snapshot()
        session_snapshot = worker.generation_scheduler_session_snapshot("legacy-session")
        cancel_result = worker.cancel_generation_session("legacy-session")

        self.assertFalse(snapshot["available"])
        self.assertFalse(session_snapshot["available"])
        self.assertFalse(cancel_result["available"])
        self.assertIn("legacy generation scheduler is disabled", snapshot["reason"])
        self.assertIn("legacy generation scheduler is disabled", session_snapshot["reason"])
        self.assertIn("legacy generation scheduler is disabled", cancel_result["reason"])
        self.assertNotIn("queue_size", snapshot)
        self.assertNotIn("status", session_snapshot)
        self.assertNotIn("cancelled", cancel_result)

    def test_legacy_main_workflow_flag_alone_does_not_enable_scheduler(self) -> None:
        flags = AgentRuntimeFlags.from_env(
            {
                "ALLOW_LEGACY_FUNCTION_ADAPTER": "1",
                "ALLOW_LEGACY_MAIN_WORKFLOW": "1",
                "OLD_WORKFLOW_DIRECT_ENTRY_DISABLED": "1",
            }
        )
        worker = _TestWorker(
            composer_factory=lambda: object(),
            agent_runtime_flags=flags,
        )

        scheduler = worker._get_generation_scheduler()

        self.assertTrue(flags.can_call_legacy_function_adapter())
        self.assertFalse(flags.can_call_legacy_main_workflow())
        self.assertIsNone(scheduler)
        self.assertIsNone(worker._generation_scheduler)

    def test_lanchat_worker_does_not_import_legacy_scheduler_at_module_top_level(self) -> None:
        worker_source = (REPO_ROOT / "editor/plugins/AITool/services/lanchat_agent_worker.py").read_text(encoding="utf-8")
        top_level_source = worker_source.split("class LANChatAgentWorker", 1)[0]

        self.assertNotIn("from .generation_scheduler import GenerationScheduler", top_level_source)
        self.assertNotIn("from .generation_composer_adapter import SceneComposerJobRunner", top_level_source)

    def test_agent_runtime_package_does_not_import_legacy_coordinator_or_seed_plan(self) -> None:
        runtime_dir = REPO_ROOT / "editor/plugins/AITool/services/agent_runtime"
        forbidden_modules = {
            "interaction_coordinator",
            "plugins.AITool.services.interaction_coordinator",
            "seed_plan",
            "plugins.AITool.services.seed_plan",
        }
        forbidden_names = {"InteractionCoordinator", "SeedPlan", "SeedPlanStatus"}

        violations: list[str] = []
        for path in sorted(runtime_dir.glob("*.py")):
            module = ast.parse(path.read_text(encoding="utf-8-sig"))
            for node in ast.walk(module):
                if isinstance(node, ast.Import):
                    for alias in node.names:
                        if alias.name in forbidden_modules:
                            violations.append(f"{path.name}: import {alias.name}")
                elif isinstance(node, ast.ImportFrom):
                    module_name = (node.module or "").lstrip(".")
                    imported_names = {alias.name for alias in node.names}
                    if module_name in forbidden_modules:
                        violations.append(f"{path.name}: from {node.module} import ...")
                    if imported_names & forbidden_names:
                        violations.append(
                            f"{path.name}: imports legacy control names {sorted(imported_names & forbidden_names)}"
                        )

        self.assertFalse(
            violations,
            "AgentRuntime package must treat Coordinator/SeedPlan as external bridge payloads, "
            f"not direct dependencies: {violations}",
        )

    def test_legacy_generation_scheduler_lazy_import_is_guarded(self) -> None:
        worker_source = (REPO_ROOT / "editor/plugins/AITool/services/lanchat_agent_worker.py").read_text(encoding="utf-8")
        getter_index = worker_source.index("def _get_generation_scheduler(")
        scheduler_import_index = worker_source.index("from .generation_scheduler import GenerationScheduler", getter_index)
        runner_import_index = worker_source.index("from .generation_composer_adapter import SceneComposerJobRunner", getter_index)
        guarded_prefix = worker_source[getter_index:scheduler_import_index]

        self.assertIn("can_call_legacy_main_workflow()", guarded_prefix)
        self.assertIn("return None", guarded_prefix)
        self.assertLess(scheduler_import_index, runner_import_index)

    def test_direct_actor_adjustment_helpers_are_only_called_behind_legacy_flag(self) -> None:
        worker_source = (REPO_ROOT / "editor/plugins/AITool/services/lanchat_agent_worker.py").read_text(encoding="utf-8")
        guarded_helpers = (
            "_execute_layout_reflow_confirmation(",
            "_try_execute_completed_final_adjustment(",
        )
        for helper in guarded_helpers:
            search_from = 0
            while True:
                index = worker_source.find(helper, search_from)
                if index < 0:
                    break
                line_start = worker_source.rfind("\n", 0, index) + 1
                line = worker_source[line_start: worker_source.find("\n", index)]
                search_from = index + len(helper)
                if line.lstrip().startswith("def "):
                    continue
                context = worker_source[max(0, index - 220): index]
                self.assertIn(
                    "can_call_legacy_main_workflow()",
                    context,
                    msg=f"{helper} call must stay behind explicit legacy workflow flags",
                )

    def test_legacy_coordinator_execution_calls_are_only_behind_legacy_flag(self) -> None:
        worker_source = (REPO_ROOT / "editor/plugins/AITool/services/lanchat_agent_worker.py").read_text(encoding="utf-8")
        legacy_calls = (
            "execute_confirmed_plan(",
            "execute_action_payload",
            "create_or_update_seed_plan(",
        )
        for legacy_call in legacy_calls:
            search_from = 0
            while True:
                index = worker_source.find(legacy_call, search_from)
                if index < 0:
                    break
                line_start = worker_source.rfind("\n", 0, index) + 1
                line = worker_source[line_start: worker_source.find("\n", index)]
                search_from = index + len(legacy_call)
                if line.strip().startswith("def "):
                    continue
                context = worker_source[max(0, index - 1600): min(len(worker_source), index + 300)]
                self.assertIn(
                    "can_call_legacy_main_workflow()",
                    context,
                    msg=f"{legacy_call} must stay behind explicit legacy workflow flags",
                )

    def test_agent_reply_context_uses_runtime_mapping_without_coordinator_fallback(self) -> None:
        worker_source = (REPO_ROOT / "editor/plugins/AITool/services/lanchat_agent_worker.py").read_text(encoding="utf-8")
        start = worker_source.index("def _mirror_agent_reply_context_in_agent_runtime(")
        end = worker_source.index("def _should_promote_agent_reply_to_runtime_plan(", start)
        body = worker_source[start:end]

        self.assertIn("_mapped_runtime_context_plan_ref(room, external_plan_id)", body)
        self.assertNotIn("active_plan_for_room(", body)
        self.assertNotIn("can_call_legacy_main_workflow()", body)

    def test_active_runtime_external_plan_id_resolves_external_link_from_runtime_state(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        plan = worker._agent_runtime.sync_external_plan_context(
            room_id="room-active-runtime-ref",
            external_plan_id="seed-active-runtime-ref",
            text="围绕强盗藏宝室做一个方案，包含宝箱和火把。",
            owner_agent="山贼",
        )

        active_ref = worker._active_runtime_external_plan_id("room-active-runtime-ref")

        self.assertEqual(active_ref, "seed-active-runtime-ref")
        room = worker._agent_runtime.query_state("room-active-runtime-ref")["room"]
        self.assertEqual(room["external_plan_links"][active_ref], plan.plan_id)

    def test_generation_scheduler_hook_adds_runtime_status_provider(self) -> None:
        class FakeScheduler:
            def __init__(self) -> None:
                self.submitted_payload: dict[str, Any] = {}

            def submit(self, payload: dict[str, Any]) -> dict[str, Any]:
                self.submitted_payload = dict(payload or {})
                return self.submitted_payload

        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        worker._agent_runtime.sync_external_plan_context(
            room_id="room-generation-status-provider",
            external_plan_id="seed-generation-status-provider",
            text="强盗藏宝室方案：中央宝箱，两侧武器架，入口火把",
            owner_agent="山贼",
        )
        scheduler = FakeScheduler()
        worker._install_progress_disclosure_scheduler(scheduler)  # noqa: SLF001

        submitted = scheduler.submit({
            "job_type": "scene_generation",
            "room_id": "room-generation-status-provider",
            "plan_id": "seed-generation-status-provider",
        })

        runtime_context = submitted.get("_runtime_context")
        self.assertIsInstance(runtime_context, dict)
        provider = runtime_context.get("runtime_status_provider")
        self.assertTrue(callable(provider))

        with patch.object(
            worker._agent_runtime,
            "query_state",
            side_effect=AssertionError("Generation status provider must use runtime_status action"),
        ):
            status = provider()

        self.assertIsInstance(status, dict)
        routed = worker._agent_runtime.operation_log.snapshot(
            room_id="room-generation-status-provider",
            event="runtime_message_action_routed",
        )
        self.assertGreaterEqual(routed["entry_count"], 1)
        self.assertEqual(routed["entries"][-1]["payload"]["action"], "runtime_status")

    def test_scene_composer_job_runner_injects_runtime_status_provider(self) -> None:
        from plugins.AITool.services.generation_composer_adapter import SceneComposerJobRunner
        from plugins.AITool.services.generation_scheduler import GenerationJob

        class FakeComposer:
            def __init__(self) -> None:
                self.observed_provider = None

            def compose(self, prompt: str, **kwargs) -> dict[str, Any]:  # noqa: ANN001
                self.observed_provider = getattr(self, "_runtime_status_provider", None)
                return {"prompt": prompt, "kwargs": kwargs}

        composer = FakeComposer()
        provider = lambda: {"status": {"plan_summary": {"status": "paused"}}}
        flags = AgentRuntimeFlags.from_env(
            {
                "AGENT_RUNTIME_ENABLED": "1",
                "ALLOW_LEGACY_MAIN_WORKFLOW": "1",
                "OLD_WORKFLOW_DIRECT_ENTRY_DISABLED": "0",
            }
        )
        runner = SceneComposerJobRunner(lambda: composer, agent_runtime_flags=flags)
        job = GenerationJob(
            payload={"prompt": "生成一个强盗藏宝室", "do_import": False},
            runtime_context={"runtime_status_provider": provider},
            room_id="room-runner-provider",
            plan_id="seed-runner-provider",
            session_id="room-runner-provider",
        )

        result = runner.compose(job)

        self.assertIs(composer.observed_provider, provider)
        self.assertEqual(result["compose_result"]["prompt"], "生成一个强盗藏宝室")

    def test_scene_composer_job_runner_blocks_default_legacy_main_workflow(self) -> None:
        from plugins.AITool.services.generation_composer_adapter import SceneComposerJobRunner
        from plugins.AITool.services.generation_scheduler import GenerationJob

        composer_factory_calls = 0

        def composer_factory():  # noqa: ANN001
            nonlocal composer_factory_calls
            composer_factory_calls += 1
            raise AssertionError("legacy composer factory must not be called")

        runner = SceneComposerJobRunner(
            composer_factory,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        job = GenerationJob(
            payload={"prompt": "生成一个强盗藏宝室", "do_import": False},
            room_id="room-runner-blocked",
            plan_id="seed-runner-blocked",
            session_id="room-runner-blocked",
        )

        with self.assertRaisesRegex(RuntimeError, "legacy SceneComposer main workflow is disabled"):
            runner.compose(job)

        self.assertEqual(composer_factory_calls, 0)

    def test_master_agent_direct_scene_compose_blocks_default_legacy_main_workflow(self) -> None:
        module = _load_agent_adapter_module()
        agent = module.MasterAgent(fallback_chat=lambda _system, _messages: "chat")

        with patch.object(module, "_legacy_main_workflow_allowed", return_value=False):
            reply = agent._handle_scene_compose(
                "按照方案生成一个强盗藏宝室",
                ["房主: 按照方案生成一个强盗藏宝室"],
                object(),
                "商人",
            )

        self.assertIn("AgentRuntime 主控", reply)
        self.assertIn("确认方案", reply)

    def test_master_agent_direct_file_import_blocks_default_legacy_main_workflow(self) -> None:
        module = _load_agent_adapter_module()
        agent = module.MasterAgent(fallback_chat=lambda _system, _messages: "chat")

        with patch.object(module, "_legacy_main_workflow_allowed", return_value=False):
            reply = agent._handle_direct_import("E:/models/treasure_chest.obj")

        self.assertIn("AgentRuntime 主控", reply)
        self.assertIn("确认方案", reply)

    def test_generation_scheduler_events_are_mirrored_to_runtime_audit(self) -> None:
        class FakeScheduler:
            def __init__(self) -> None:
                self.events: list[dict[str, Any]] = []

            def _record_event_locked(self, event_type: str, **payload: Any) -> None:
                self.events.append({"event_type": event_type, **dict(payload)})

        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        worker._agent_runtime.sync_external_plan_context(
            room_id="room-scheduler-audit",
            external_plan_id="seed-scheduler-audit",
            text="强盗藏宝室方案：中央宝箱，两侧武器架，入口火把",
            owner_agent="山贼",
        )
        scheduler = FakeScheduler()
        worker._install_generation_scheduler_runtime_audit(scheduler)  # noqa: SLF001

        scheduler._record_event_locked(
            "status_change",
            room_id="room-scheduler-audit",
            session_id="room-scheduler-audit",
            plan_id="seed-scheduler-audit",
            batch_id="batch-1",
            job_id="gen-internal-should-not-leak",
            status="composing",
            current_stage="compose",
            priority=2,
        )

        entries = worker._agent_runtime.operation_log.query(
            room_id="room-scheduler-audit",
            event="generation_scheduler_status_change",
        )
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0].payload["status"], "composing")
        self.assertEqual(entries[0].payload["event_type"], "status_change")
        self.assertNotIn("job_id", entries[0].payload)
        self.assertNotIn("gen-internal-should-not-leak", str(entries[0].payload))

        worker._clear_generation_scheduler_runtime_audit(scheduler)  # noqa: SLF001
        scheduler._record_event_locked("status_change", room_id="room-scheduler-audit", status="done")
        entries_after_clear = worker._agent_runtime.operation_log.query(
            room_id="room-scheduler-audit",
            event="generation_scheduler_status_change",
        )
        self.assertEqual(len(entries_after_clear), 1)

    def test_explicit_legacy_flags_allow_lazy_generation_scheduler_for_transition(self) -> None:
        flags = AgentRuntimeFlags.from_env(
            {
                "AGENT_RUNTIME_ENABLED": "1",
                "OLD_WORKFLOW_DIRECT_ENTRY_DISABLED": "0",
                "ALLOW_LEGACY_MAIN_WORKFLOW": "1",
            }
        )
        worker = _TestWorker(
            composer_factory=lambda: object(),
            agent_runtime_flags=flags,
        )

        scheduler = worker._get_generation_scheduler()

        self.assertIsNotNone(scheduler)
        self.assertIs(scheduler, worker._generation_scheduler)

    def test_master_agent_scene_write_entries_are_guarded_by_runtime_flags(self) -> None:
        source = (
            REPO_ROOT
            / "editor"
            / "plugins"
            / "AITool"
            / "cai_extensions"
            / "agent"
            / "agent_adapter.py"
        ).read_text(encoding="utf-8")
        handle_scene_index = source.index("def _handle_scene(")
        direct_compose_index = source.index("return self._handle_scene_compose", handle_scene_index)
        scene_guarded_prefix = source[handle_scene_index:direct_compose_index]
        direct_import_index = source.index("def _handle_direct_import(")
        direct_import_tool_index = source.index('get_tool("import_model")', direct_import_index)
        import_guarded_prefix = source[direct_import_index:direct_import_tool_index]
        edit_index = source.index("def _handle_edit(")
        edit_guarded_prefix = source[edit_index:]

        for guarded_prefix in (
            scene_guarded_prefix,
            import_guarded_prefix,
            edit_guarded_prefix,
        ):
            self.assertIn("_legacy_main_workflow_allowed()", guarded_prefix)
            self.assertIn("AGENT_RUNTIME_REQUIRED_MESSAGE", guarded_prefix)

    def test_master_agent_direct_scene_compose_entry_is_self_guarded(self) -> None:
        source = (
            REPO_ROOT
            / "editor"
            / "plugins"
            / "AITool"
            / "cai_extensions"
            / "agent"
            / "agent_adapter.py"
        ).read_text(encoding="utf-8")
        compose_entry_index = source.index("def _handle_scene_compose(")
        composer_import_index = source.index("from .scene_composer import SceneComposer", compose_entry_index)
        guarded_prefix = source[compose_entry_index:composer_import_index]

        self.assertIn("_legacy_main_workflow_allowed()", guarded_prefix)
        self.assertIn("AGENT_RUNTIME_REQUIRED_MESSAGE", guarded_prefix)

    def test_direct_scene_composer_compose_call_sites_are_flag_guarded(self) -> None:
        agent_source = (
            REPO_ROOT
            / "editor"
            / "plugins"
            / "AITool"
            / "cai_extensions"
            / "agent"
            / "agent_adapter.py"
        ).read_text(encoding="utf-8")
        adapter_source = (
            REPO_ROOT
            / "editor"
            / "plugins"
            / "AITool"
            / "services"
            / "generation_composer_adapter.py"
        ).read_text(encoding="utf-8")

        compose_entry_index = agent_source.index("def _handle_scene_compose(")
        scene_composer_import_index = agent_source.index("from .scene_composer import SceneComposer", compose_entry_index)
        direct_compose_index = agent_source.index("composer.compose(", compose_entry_index)
        master_agent_guarded_prefix = agent_source[compose_entry_index:scene_composer_import_index]
        self.assertLess(scene_composer_import_index, direct_compose_index)
        self.assertIn("_legacy_main_workflow_allowed()", master_agent_guarded_prefix)
        self.assertIn("AGENT_RUNTIME_REQUIRED_MESSAGE", master_agent_guarded_prefix)

        runner_compose_index = adapter_source.index("def compose(self, job:")
        factory_index = adapter_source.index("composer = self._composer_factory()", runner_compose_index)
        runner_direct_compose_index = adapter_source.index("composer.compose(", runner_compose_index)
        runner_guarded_prefix = adapter_source[runner_compose_index:factory_index]
        self.assertLess(factory_index, runner_direct_compose_index)
        self.assertIn("can_call_legacy_main_workflow()", runner_guarded_prefix)
        self.assertIn("legacy SceneComposer main workflow is disabled", runner_guarded_prefix)

    def test_master_agent_scene_compose_entries_return_runtime_required_message_by_default(self) -> None:
        agent_adapter = _load_agent_adapter_module()
        agent = agent_adapter.MasterAgent(fallback_chat=lambda system, messages: "fallback")

        import_reply = agent._handle_direct_import("E:/tmp/demo_model.glb")  # noqa: SLF001
        edit_reply = agent._handle_edit("把宝箱往左移动一点", [])  # noqa: SLF001
        scene_reply = agent._handle_scene(  # noqa: SLF001
            "生成一个强盗藏宝室，有宝箱、金币和火把",
            "山贼",
            [],
            force_compose=True,
        )
        specialist = agent._router.route("山贼")  # noqa: SLF001
        scene_state = {
            "metadata": {"room_size": [5, 3, 3], "scene_name": "Scene/default.scene"},
            "intermediate": {},
        }
        single_reply = agent._handle_scene_single(  # noqa: SLF001
            "把宝箱往左移动一点",
            dict(scene_state),
            {},
            specialist,
            [],
        )
        multistep_reply = agent._handle_scene_multistep(  # noqa: SLF001
            "设计并布置一个复杂强盗藏宝室",
            dict(scene_state),
            {},
            specialist,
        )
        compose_reply = agent._handle_scene_compose(  # noqa: SLF001
            "生成一个强盗藏宝室，有宝箱、金币和火把",
            [],
            specialist=None,
            persona="山贼",
        )

        self.assertEqual(import_reply, agent_adapter.AGENT_RUNTIME_REQUIRED_MESSAGE)
        self.assertEqual(edit_reply, agent_adapter.AGENT_RUNTIME_REQUIRED_MESSAGE)
        self.assertEqual(scene_reply, agent_adapter.AGENT_RUNTIME_REQUIRED_MESSAGE)
        self.assertEqual(single_reply, agent_adapter.AGENT_RUNTIME_REQUIRED_MESSAGE)
        self.assertEqual(multistep_reply, agent_adapter.AGENT_RUNTIME_REQUIRED_MESSAGE)
        self.assertEqual(compose_reply, agent_adapter.AGENT_RUNTIME_REQUIRED_MESSAGE)

    def test_master_agent_call_write_routes_return_runtime_required_message_by_default(self) -> None:
        agent_adapter = _load_agent_adapter_module()
        agent = agent_adapter.MasterAgent(fallback_chat=lambda system, messages: "fallback")

        original_classifier = agent_adapter.classify_intent
        original_runtime_getter = agent_adapter.get_lanchat_scene_runtime

        class _NoPlanningGate:
            def handle_planning_gate(self, agent_name: str, trigger: str):  # noqa: ANN001
                return "", ""

        class _ComposePlanningGate:
            def handle_planning_gate(self, agent_name: str, trigger: str):  # noqa: ANN001
                return "compose", "生成一个强盗藏宝室，有宝箱、金币和火把"

        try:
            agent_adapter.get_lanchat_scene_runtime = lambda: _NoPlanningGate()

            with tempfile.NamedTemporaryFile(suffix=".glb") as model_file:
                direct_import_reply = agent("山贼", [f"房主: {model_file.name}"])

            agent_adapter.classify_intent = lambda text: "compose"
            compose_reply = agent("山贼", ["房主: 生成一个强盗藏宝室，有宝箱、金币和火把"])

            agent_adapter.classify_intent = lambda text: "edit"
            edit_reply = agent("山贼", ["房主: 把宝箱往左移动一点"])

            agent_adapter.get_lanchat_scene_runtime = lambda: _ComposePlanningGate()
            planning_gate_compose_reply = agent("山贼", ["房主: 确认生成"])
        finally:
            agent_adapter.classify_intent = original_classifier
            agent_adapter.get_lanchat_scene_runtime = original_runtime_getter

        self.assertEqual(direct_import_reply, agent_adapter.AGENT_RUNTIME_REQUIRED_MESSAGE)
        self.assertEqual(compose_reply, agent_adapter.AGENT_RUNTIME_REQUIRED_MESSAGE)
        self.assertEqual(edit_reply, agent_adapter.AGENT_RUNTIME_REQUIRED_MESSAGE)
        self.assertEqual(planning_gate_compose_reply, agent_adapter.AGENT_RUNTIME_REQUIRED_MESSAGE)

    def test_master_agent_call_compose_routes_do_not_enter_legacy_scene_handler_by_default(self) -> None:
        agent_adapter = _load_agent_adapter_module()
        agent = agent_adapter.MasterAgent(fallback_chat=lambda system, messages: "fallback")

        original_classifier = agent_adapter.classify_intent
        original_runtime_getter = agent_adapter.get_lanchat_scene_runtime

        class _NoPlanningGate:
            def handle_planning_gate(self, agent_name: str, trigger: str):  # noqa: ANN001
                return "", ""

        class _ComposePlanningGate:
            def handle_planning_gate(self, agent_name: str, trigger: str):  # noqa: ANN001
                return "compose", "生成一个强盗藏宝室，有宝箱、金币和火把"

        try:
            with patch.object(
                agent,
                "_handle_scene",
                side_effect=AssertionError("default AgentRuntime mode must block before legacy scene handler"),
            ):
                agent_adapter.get_lanchat_scene_runtime = lambda: _ComposePlanningGate()
                planning_reply = agent("山贼", ["房主: 确认生成"])

                agent_adapter.get_lanchat_scene_runtime = lambda: _NoPlanningGate()
                agent_adapter.classify_intent = lambda text: "compose"
                semantic_reply = agent("山贼", ["房主: 生成一个强盗藏宝室，有宝箱、金币和火把"])
        finally:
            agent_adapter.classify_intent = original_classifier
            agent_adapter.get_lanchat_scene_runtime = original_runtime_getter

        self.assertEqual(planning_reply, agent_adapter.AGENT_RUNTIME_REQUIRED_MESSAGE)
        self.assertEqual(semantic_reply, agent_adapter.AGENT_RUNTIME_REQUIRED_MESSAGE)

    def test_master_agent_lanchat_progress_context_blocks_role_agent_direct_compose(self) -> None:
        source = (
            REPO_ROOT
            / "editor"
            / "plugins"
            / "AITool"
            / "cai_extensions"
            / "agent"
            / "agent_adapter.py"
        ).read_text(encoding="utf-8")
        handle_scene_index = source.index("def _handle_scene(")
        direct_compose_index = source.index("return self._handle_scene_compose", handle_scene_index)
        guarded_prefix = source[handle_scene_index:direct_compose_index]

        self.assertIn("get_current_progress_sink() is not None", guarded_prefix)
        self.assertIn("LANChat compose request blocked from direct RoleAgent compose", guarded_prefix)
        self.assertIn("请先由房主确认当前方案", guarded_prefix)

    def test_master_agent_lanchat_progress_context_blocks_compose_even_when_legacy_enabled(self) -> None:
        agent_adapter = _load_agent_adapter_module()
        agent = agent_adapter.MasterAgent(fallback_chat=lambda system, messages: "fallback")
        original_runtime_getter = agent_adapter.get_lanchat_scene_runtime

        try:
            class _ComposePlanningGate:
                def handle_planning_gate(self, agent_name: str, trigger: str):  # noqa: ANN001
                    return "compose", "生成一个强盗藏宝室，有宝箱、金币和火把"

            agent_adapter.get_lanchat_scene_runtime = lambda: _ComposePlanningGate()
            with patch.dict(
                "os.environ",
                {
                    "AGENT_RUNTIME_ENABLED": "1",
                    "OLD_WORKFLOW_DIRECT_ENTRY_DISABLED": "0",
                    "ALLOW_LEGACY_MAIN_WORKFLOW": "1",
                },
            ):
                with agent_adapter.agent_progress_sink(lambda message: None):
                    with patch.object(
                        agent,
                        "_handle_scene",
                        side_effect=AssertionError("LANChat RoleAgent must not enter legacy scene handler in progress context"),
                    ):
                        reply = agent("山贼", ["房主: 确认生成"])
        finally:
            agent_adapter.get_lanchat_scene_runtime = original_runtime_getter

        self.assertIn("请先由房主确认当前方案", reply)
        self.assertNotIn("AgentRuntime 主控", reply)

    def test_lanchat_worker_legacy_busy_note_side_channel_is_flag_guarded(self) -> None:
        source = (
            REPO_ROOT
            / "editor"
            / "plugins"
            / "AITool"
            / "services"
            / "lanchat_agent_worker.py"
        ).read_text(encoding="utf-8")
        self.assertIn("can_call_legacy_main_workflow()", source)
        self.assertIn("_record_active_runtime_busy_intervention", source)
        self.assertIn("_agent_runtime_flags", source)

    def test_legacy_busy_note_side_channel_mirrors_pending_intervention_to_runtime(self) -> None:
        class FakeSceneRuntime:
            def active_snapshot(self) -> dict[str, Any]:
                return {"active": True}

            def classify_scene_note(self, text: str) -> str:
                return "add_object"

            def record_busy_message(
                self,
                *,
                agent_name: str,
                text: str,
                source_user_id: str = "",
            ) -> str:
                return "已记录后续补充：天使雕像。我会优先尝试加入后续批次。"

            def handle_targeted_planning_message(
                self,
                target: str,
                text: str,
                *,
                draft_action: str = "",
                source_context_agent: str = "",
            ):
                return "pass", None, None

            def handle_pending_planning_message(self, text: str):
                return "pass", None, None

        flags = AgentRuntimeFlags(
            agent_runtime_enabled=True,
            old_workflow_direct_entry_disabled=False,
            allow_legacy_function_adapter=True,
            allow_legacy_main_workflow=True,
        )
        engine = _FakeReplyEngine()
        worker = _TestWorker(
            corona_engine=engine,
            interaction_coordinator=_ExplodingCoordinator(),
            agent_runtime_flags=flags,
        )
        plan = worker._agent_runtime.handle_message(
            room_id="room-legacy-busy-runtime",
            external_plan_id="seed-legacy-busy-runtime",
            text="强盗藏宝室方案：中央宝箱，两侧武器架，入口火把",
            sender_id="host-1",
            sender_name="房主",
            owner_agent="商人",
            action="plan",
        )["plan"]
        worker._agent_runtime.confirm_scene_plan(plan["plan_id"], confirmed_by="host-1")
        worker._agent_runtime.plan_batches(plan["plan_id"], max_items_per_batch=8)
        worker._agent_runtime.enqueue_planned_batches(plan["plan_id"], max_items_per_batch=8)

        with patch(
            "plugins.AITool.services.lanchat_scene_runtime.get_lanchat_scene_runtime",
            return_value=FakeSceneRuntime(),
        ), patch.object(
            worker,
            "_seed_agent_trigger_planning_context_in_runtime",
            return_value=None,
        ), patch.object(
            worker,
            "_handle_agent_trigger_planning_gate",
            return_value=False,
        ), patch.object(
            worker,
            "_run_agent",
            side_effect=AssertionError("busy note quick reply must avoid free RoleAgent path"),
        ):
            handled = worker._process_trigger({
                "room_id": "room-legacy-busy-runtime",
                "message_id": "msg-legacy-busy-runtime",
                "text": "再添加一个天使雕像",
                "sender_id": "host-1",
                "sender_name": "房主",
                "sender_type": "host",
                "message_kind": "chat",
                "agent_id": "merchant",
                "agent_name": "商人",
            })

        self.assertTrue(handled)
        self.assertEqual(len(engine.replies), 1)
        self.assertIn("已记录", engine.replies[0]["text"])
        state = worker._agent_runtime.state.snapshot("room-legacy-busy-runtime")["room"]
        self.assertEqual(state["active_plan_id"], plan["plan_id"])
        accepted = list(state["accepted_interventions"].values())
        self.assertEqual(len(accepted), 1)
        self.assertEqual(accepted[0]["plan_id"], plan["plan_id"])
        self.assertEqual(accepted[0]["patch_type"], "intervention_add")
        self.assertTrue(accepted[0]["items"])
        self.assertGreaterEqual(len(state["batch_plans"]), 2)
        self.assertIn("plan_patch_recorded", worker._agent_runtime.operation_log.events())

    def test_lanchat_planning_gate_legacy_process_trigger_fallbacks_are_flag_guarded(self) -> None:
        source = (
            REPO_ROOT
            / "editor"
            / "plugins"
            / "AITool"
            / "services"
            / "lanchat_agent_worker.py"
        ).read_text(encoding="utf-8")
        for function_name in (
            "def _handle_structured_planning_gate(",
            "def _handle_plain_chat_planning_gate(",
        ):
            function_index = source.index(function_name)
            process_index = source.index("self._process_trigger(trigger)", function_index)
            guarded_prefix = source[function_index:process_index]
            self.assertIn("can_call_legacy_main_workflow()", guarded_prefix)
            self.assertIn("旧生成链路已关闭", guarded_prefix)

    def test_agent_trigger_scene_write_fallback_blocks_legacy_role_agent(self) -> None:
        cases = (
            ("intervention_add", "再添加一个天使雕像"),
            ("intervention_modify", "把宝箱往左移动一点"),
            ("intervention_delete", "删除宝箱"),
            ("final_adjustment_request", "调整一下布局，模型有点浮空"),
        )
        for expected_intent, text in cases:
            with self.subTest(expected_intent=expected_intent):
                engine = _FakeReplyEngine()
                worker = _TestWorker(
                    corona_engine=engine,
                    interaction_coordinator=_ExplodingCoordinator(),
                    agent_runtime_flags=AgentRuntimeFlags.from_env({}),
                )
                worker._agent_runtime.handle_message(
                    room_id="room-role-agent-write-block",
                    external_plan_id="seed-role-agent-write-block",
                    text="强盗藏宝室方案：中央宝箱，两侧武器架，入口火把",
                    sender_id="host-1",
                    sender_name="房主",
                    owner_agent="商人",
                    action="plan",
                )

                with patch.object(
                    worker,
                    "_handle_coordinator_generation_start",
                    return_value=None,
                ), patch.object(
                    worker,
                    "_handle_coordinator_completed_intervention",
                    return_value=None,
                ), patch.object(
                    worker,
                    "_handle_coordinator_executing_intervention",
                    return_value=None,
                ), patch.object(
                    worker,
                    "_seed_agent_trigger_planning_context_in_runtime",
                    return_value=None,
                ), patch.object(
                    worker,
                    "_handle_agent_trigger_planning_gate",
                    return_value=False,
                ), patch.object(
                    worker,
                    "_run_agent",
                    side_effect=AssertionError("scene write fallback must not reach legacy RoleAgent"),
                ):
                    handled = worker._process_trigger({
                        "room_id": "room-role-agent-write-block",
                        "message_id": f"msg-role-agent-write-block-{expected_intent}",
                        "text": text,
                        "sender_id": "host-1",
                        "sender_name": "房主",
                        "sender_type": "host",
                        "message_kind": "chat",
                        "agent_id": "merchant",
                        "agent_name": "商人",
                    })

                self.assertTrue(handled)
                reply_texts = [str(reply.get("text") or "") for reply in engine.replies]
                self.assertTrue(any(item.strip() for item in reply_texts))
                self.assertIn("legacy_role_agent_scene_write_blocked", worker._agent_runtime.operation_log.events())
                block_entries = [
                    entry
                    for entry in worker._agent_runtime.operation_log.entries()
                    if entry.event == "legacy_role_agent_scene_write_blocked"
                ]
                self.assertTrue(block_entries)
                self.assertEqual(block_entries[-1].payload.get("intent"), expected_intent)

    def test_deprecated_workflow_commands_hidden_from_user_registry_by_default(self) -> None:
        with patch.dict("os.environ", {}, clear=True):
            for command in DEPRECATED_USER_WORKFLOW_COMMANDS:
                self.assertTrue(is_deprecated_user_workflow_command(command))
                self.assertEqual(classify_workflow_command_exposure(command), "deprecated")
                self.assertFalse(
                    should_register_workflow_command(command),
                    msg=f"{command} must stay hidden from the user command registry",
                )
            for command in INTERNAL_DEBUG_WORKFLOW_COMMANDS:
                self.assertEqual(classify_workflow_command_exposure(command), "internal")
                self.assertFalse(
                    should_register_workflow_command(command),
                    msg=f"{command} must stay internal unless explicitly enabled",
                )
            self.assertEqual(classify_workflow_command_exposure("/new_safe_tool"), "public")
            self.assertEqual(classify_workflow_command_exposure(""), "invalid")

    def test_workflow_command_policy_keeps_legacy_hidden_even_when_env_requests_it(self) -> None:
        legacy_command = next(iter(DEPRECATED_USER_WORKFLOW_COMMANDS))
        internal_command = next(iter(INTERNAL_DEBUG_WORKFLOW_COMMANDS))
        with patch.dict("os.environ", {"CORONA_ENABLE_LEGACY_WORKFLOW_COMMANDS": "1"}, clear=True):
            self.assertFalse(
                should_register_workflow_command(legacy_command),
                msg="Deprecated workflow main-control commands must not be re-exposed by env flags",
            )
            self.assertFalse(
                should_register_workflow_command(internal_command),
                msg="Legacy env must not implicitly enable internal/debug commands",
            )
        with patch.dict("os.environ", {"CORONA_ENABLE_INTERNAL_WORKFLOW_COMMANDS": "1"}, clear=True):
            self.assertFalse(should_register_workflow_command(legacy_command))
            self.assertTrue(should_register_workflow_command(internal_command))

    def test_master_agent_blocks_deprecated_workflow_command_text_entry(self) -> None:
        agent_adapter = _load_agent_adapter_module()
        fallback_calls: list[tuple[str, list[str]]] = []

        def _fallback(system: str, messages: list[str]) -> str:
            fallback_calls.append((system, messages))
            return "fallback should not run"

        agent = agent_adapter.create_master_agent(fallback_chat=_fallback)

        for command in ("/scene_agent", "/pipeline", "/parallel_generate_v2"):
            with self.subTest(command=command):
                reply = agent("长者", [f"房主: {command} 做一个强盗藏宝室"])
                self.assertEqual(reply, DEPRECATED_WORKFLOW_COMMAND_MESSAGE)

        self.assertEqual(fallback_calls, [])

    def test_cabbage_workflow_plugin_reports_hidden_command_classes(self) -> None:
        from plugins.AITool.cai_extensions.register import CabbageContext, CabbageWorkflowPlugin

        class _Registry:
            def __init__(self) -> None:
                self.calls: list[tuple[Any, Any]] = []

            def register(self, key, value, overwrite: bool = False) -> None:  # noqa: ANN001
                self.calls.append((key, value))

        class _Runtime:
            def __init__(self) -> None:
                self.metadata: dict[str, Any] = {}
                self.workflow_registry = _Registry()
                self.command_registry = _Registry()

            def get_registry(self, name: str):  # noqa: ANN201
                if name == "workflow":
                    return self.workflow_registry
                if name == "workflow_command":
                    return self.command_registry
                raise AssertionError(name)

        module_name = "fake_agent_native_workflow_policy_module"
        fake_module = types.ModuleType(module_name)
        fake_module.WORKFLOWS = {90001: object()}
        fake_module.WORKFLOW_COMMANDS = {
            "/scene_agent": 90001,
            "/model_retrieval": 90001,
            "/safe_runtime_tool": 90001,
        }
        runtime = _Runtime()
        context = CabbageContext(aitool_dir=REPO_ROOT / "editor" / "plugins" / "AITool", cai_dir=REPO_ROOT)
        plugin = CabbageWorkflowPlugin(context)
        plugin.flow_modules = (module_name,)

        with patch.dict(sys.modules, {module_name: fake_module}), patch.dict("os.environ", {}, clear=True):
            result = plugin.register(runtime)

        self.assertEqual(result["commands"], ["/safe_runtime_tool"])
        self.assertEqual(result["hidden_deprecated_commands"], ["/scene_agent"])
        self.assertEqual(result["hidden_internal_commands"], ["/model_retrieval"])
        self.assertIn("/scene_agent", result["hidden_commands"])
        self.assertIn("/model_retrieval", result["hidden_commands"])
        self.assertEqual(runtime.command_registry.calls, [("/safe_runtime_tool", 90001)])
        self.assertEqual(classify_workflow_function_exposure(90001, runtime.command_registry), "deprecated")
        self.assertFalse(should_execute_workflow_function(90001, runtime.command_registry))
        self.assertEqual(getattr(runtime.command_registry, "_corona_deprecated_workflow_function_ids"), {90001})
        self.assertEqual(getattr(runtime.command_registry, "_corona_internal_workflow_function_ids"), {90001})
        self.assertEqual(getattr(runtime.command_registry, "_corona_public_workflow_function_ids"), {90001})

    def test_default_workflow_registration_does_not_import_legacy_main_workflows(self) -> None:
        from plugins.AITool.cai_extensions.register import CabbageWorkflowPlugin

        self.assertEqual(CabbageWorkflowPlugin.flow_modules, ())
        self.assertIn(".agent", CabbageWorkflowPlugin.legacy_flow_modules)
        self.assertIn(
            ".flows.scene_composition_workflow",
            CabbageWorkflowPlugin.legacy_flow_modules,
        )
        self.assertIn(
            ".flows.model_retrieval_workflow",
            CabbageWorkflowPlugin.legacy_flow_modules,
        )

    def test_workflow_command_policy_patches_dynamic_discovery_registry(self) -> None:
        class _QuasarLikeCommandRegistry:
            def __init__(self) -> None:
                self._commands: dict[str, int] = {}

            def register(self, command: str, function_id: int, *, overwrite: bool = False) -> None:
                key = command if command.startswith("/") else f"/{command}"
                if key in self._commands and not overwrite:
                    raise ValueError(key)
                self._commands[key] = function_id

            def resolve(self, command: str) -> int | None:
                key = command if command.startswith("/") else f"/{command}"
                return self._commands.get(key)

            def list_commands(self) -> dict[str, int]:
                return dict(self._commands)

            def discover(self, *, force: bool = False) -> int:
                self.register("/scene_agent", 90001, overwrite=True)
                self.register("/model_retrieval", 90002, overwrite=True)
                self.register("/safe_runtime_tool", 90003, overwrite=True)
                return 3

        registry = _QuasarLikeCommandRegistry()

        with patch.dict("os.environ", {}, clear=True):
            self.assertTrue(install_workflow_command_policy(registry))
            registry.discover(force=True)

        self.assertIsNone(registry.resolve("/scene_agent"))
        self.assertIsNone(registry.resolve("/model_retrieval"))
        self.assertEqual(registry.resolve("/safe_runtime_tool"), 90003)
        self.assertEqual(registry.list_commands(), {"/safe_runtime_tool": 90003})

        with patch.dict("os.environ", {"CORONA_ENABLE_INTERNAL_WORKFLOW_COMMANDS": "1"}, clear=True):
            registry.discover(force=True)
            self.assertIsNone(registry.resolve("/scene_agent"))
            self.assertEqual(registry.resolve("/model_retrieval"), 90002)
            self.assertEqual(
                registry.list_commands(),
                {"/model_retrieval": 90002, "/safe_runtime_tool": 90003},
            )
        self.assertIsNone(registry.resolve("/model_retrieval"))

    def test_workflow_function_policy_blocks_explicit_hidden_function_ids(self) -> None:
        class _QuasarLikeCommandRegistry:
            def __init__(self) -> None:
                self._commands: dict[str, int] = {}

            def register(self, command: str, function_id: int, *, overwrite: bool = False) -> None:
                key = command if command.startswith("/") else f"/{command}"
                if key in self._commands and not overwrite:
                    raise ValueError(key)
                self._commands[key] = function_id

            def resolve(self, command: str) -> int | None:
                key = command if command.startswith("/") else f"/{command}"
                return self._commands.get(key)

            def list_commands(self) -> dict[str, int]:
                return dict(self._commands)

            def discover(self, *, force: bool = False) -> int:
                self.register("/scene_agent", 90001, overwrite=True)
                self.register("/model_retrieval", 90002, overwrite=True)
                self.register("/safe_runtime_tool", 90003, overwrite=True)
                return 3

        class _QuasarLikeWorkflowRegistry:
            def __init__(self) -> None:
                self._workflows = {
                    90001: "deprecated-main-control",
                    90002: "internal-debug-tool",
                    90003: "public-runtime-tool",
                }

            def get(self, function_id: int):  # noqa: ANN201
                return self._workflows.get(function_id)

            def has(self, function_id: int) -> bool:
                return function_id in self._workflows

            def list_function_ids(self) -> list[int]:
                return list(self._workflows)

        command_registry = _QuasarLikeCommandRegistry()
        workflow_registry = _QuasarLikeWorkflowRegistry()

        with patch.dict("os.environ", {}, clear=True):
            self.assertTrue(install_workflow_command_policy(command_registry))
            command_registry.discover(force=True)
            self.assertTrue(install_workflow_function_policy(workflow_registry, command_registry))

            self.assertFalse(should_execute_workflow_function(90001, command_registry))
            self.assertFalse(should_execute_workflow_function(90002, command_registry))
            self.assertTrue(should_execute_workflow_function(90003, command_registry))
            self.assertIsNone(workflow_registry.get(90001))
            self.assertIsNone(workflow_registry.get(90002))
            self.assertEqual(workflow_registry.get(90003), "public-runtime-tool")
            self.assertFalse(workflow_registry.has(90001))
            self.assertFalse(workflow_registry.has(90002))
            self.assertEqual(workflow_registry.list_function_ids(), [90003])

        with patch.dict("os.environ", {"CORONA_ENABLE_INTERNAL_WORKFLOW_COMMANDS": "1"}, clear=True):
            self.assertIsNone(workflow_registry.get(90001))
            self.assertEqual(workflow_registry.get(90002), "internal-debug-tool")
            self.assertEqual(workflow_registry.list_function_ids(), [90002, 90003])

    def test_plain_generation_start_trigger_routes_to_agent_runtime_by_default(self) -> None:
        coordinator = _FakeCoordinator()
        worker = _LayoutDirectExecutionTrackingWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        reply = worker._handle_coordinator_generation_start({
            "room_id": "room-plain-start",
            "message_id": "msg-start",
            "text": "确认生成",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
        })

        self.assertIn("没有可确认的 AgentRuntime", reply or "")
        self.assertEqual(coordinator.execute_calls, [])
        runtime_state = worker._agent_runtime.query_state("room-plain-start")["room"]
        self.assertEqual(runtime_state["scene_plans"], {})

    def test_runtime_pause_command_from_lanchat_updates_runtime_state(self) -> None:
        coordinator = _FakeCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        plan = worker._agent_runtime.sync_external_plan_context(
            room_id="room-runtime-pause",
            external_plan_id="seed-test",
            text="做一个藏宝室，有宝箱、金币和火把",
            owner_agent="商人",
        )
        worker._agent_runtime.confirm_scene_plan(plan.plan_id, confirmed_by="房主")

        reply = worker._handle_agent_runtime_command({
            "room_id": "room-runtime-pause",
            "message_id": "msg-pause",
            "text": "先暂停生成",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
        })

        self.assertIn("Runtime", reply or "")
        state = worker._agent_runtime.query_state("room-runtime-pause")["room"]
        self.assertEqual(state["scene_plans"][plan.plan_id]["status"], "paused")
        self.assertEqual(state["runtime_commands"][-1]["command"], "pause")
        self.assertEqual(coordinator.execute_calls, [])

    def test_gm_pace_control_mirrors_pause_to_agent_runtime_command(self) -> None:
        coordinator = _FakePaceCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        plan = worker._agent_runtime.handle_message(
            room_id="room-gm-pace-runtime",
            external_plan_id="seed-gm-pace-runtime",
            text="做一个强盗藏宝室，包含宝箱、金币和火把",
            sender_id="host-1",
            sender_name="房主",
            owner_agent="GM",
            action="plan",
        )["plan"]
        worker._agent_runtime.confirm_scene_plan(plan["plan_id"], confirmed_by="房主")

        reply = worker._handle_coordinator_gm_control({
            "room_id": "room-gm-pace-runtime",
            "message_id": "msg-gm-pause-runtime",
            "text": "@GM 先暂停生成",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("【GM】", reply or "")
        self.assertEqual(coordinator.control_calls[-1]["action"], "pause")
        state = worker._agent_runtime.state.snapshot("room-gm-pace-runtime")["room"]
        self.assertEqual(state["scene_plans"][plan["plan_id"]]["status"], "paused")
        self.assertEqual(state["runtime_commands"][-1]["command"], "pause")
        self.assertIn("runtime_pause_command_applied", worker._agent_runtime.operation_log.events())

    def test_gm_summary_reads_agent_runtime_status_when_runtime_plan_exists(self) -> None:
        coordinator = _FakeCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._start_active_coordinator_generation(
            coordinator,
            room_id="room-runtime-status",
            host_id="房主",
        )
        active_plan_id = worker._agent_runtime.status_summary("room-runtime-status").get("plan_id", "")
        worker._agent_runtime.operation_log.append(
            "agent_reply_send_requested",
            room_id="room-runtime-status",
            plan_id=str(active_plan_id or ""),
            payload={
                "message_kind": "agent_reply",
                "channel": "network_send_agent_reply_ex",
                "sent": False,
                "stage": "资源调度",
                "progress": 30,
                "source_user_id": "房主@secret",
                "provider": "secret-provider",
                "prompt": "hidden prompt",
            },
        )
        worker._agent_runtime.operation_log.append(
            "agent_reply_send_failed",
            room_id="room-runtime-status",
            plan_id=str(active_plan_id or ""),
            payload={
                "message_kind": "agent_reply",
                "channel": "network_send_agent_reply_ex",
                "sent": False,
                "stage": "资源调度",
                "progress": 30,
                "failure_code": "message_delivery_failed",
                "url": "https://example.invalid/private",
            },
        )
        worker._agent_runtime.operation_log.append(
            "tool_call_succeeded",
            room_id="room-runtime-status",
            plan_id=str(active_plan_id or ""),
            payload={
                "import_results": [
                    {"actor_id": "actor-chest", "actor_name": "藏宝箱", "status": "success"},
                    {
                        "actor_id": "actor-gold",
                        "actor_name": "金币堆",
                        "status": "failed",
                        "reason": "provider raw https://example.invalid/native",
                    },
                ],
            },
        )
        worker._agent_runtime.operation_log.append(
            "runtime_engine_write_status_exported",
            room_id="room-runtime-status",
            plan_id=str(active_plan_id or ""),
            payload={
                "recorded": True,
                "reason": "status query",
                "engine_write_boundary_fact_count": 2,
                "engine_write_import_boundary_count": 1,
                "engine_write_environment_import_boundary_count": 0,
                "engine_write_transform_boundary_count": 1,
                "engine_write_delete_boundary_count": 0,
                "engine_write_bridge_call_count": 3,
                "engine_write_bridge_success_count": 2,
                "engine_write_bridge_failed_count": 1,
                "engine_write_bridge_error_code_counts": {"cpp_actor_transform_failed": 1},
                "engine_write_readiness_native_enabled_count": 1,
                "engine_write_readiness_runtime_state_only_count": 2,
                "engine_write_readiness_fallback_count": 1,
                "engine_write_readiness_disabled_count": 1,
                "engine_write_readiness_native_enabled_channels": ["actor_import"],
                "engine_write_readiness_runtime_state_only_channels": ["actor_delete", "layout_transform"],
                "engine_write_readiness_fallback_channels": ["actor_import"],
                "engine_write_readiness_disabled_channels": ["environment_import"],
                "provider": "hidden-provider",
                "prompt": "hidden prompt",
            },
        )
        worker._agent_runtime.operation_log.append(
            "sync_event_recorded",
            room_id="room-runtime-status",
            plan_id=str(active_plan_id or ""),
            message="asset_transfer_progress",
            payload={
                "event_type": "asset_transfer_progress",
                "asset_id": "asset-gm-secret",
                "peer_id": "peer-gm-secret",
                "asset_path": "E:/secret/gm.glb",
                "transfer_status": "transferring",
                "progress": 60,
                "chunk_index": 3,
                "chunk_count": 5,
                "bytes_transferred": 600,
                "total_bytes": 1000,
            },
        )
        worker._agent_runtime.handle_message(
            room_id="room-runtime-status",
            text="asset transfer progress",
            action="asset_transfer_event",
            sync_event={
                "event_type": "asset_transfer_progress",
                "asset_id": "asset-gm-visible",
                "transfer_status": "transferring",
                "progress": 60,
                "chunk_index": 3,
                "chunk_count": 5,
                "bytes_transferred": 600,
                "total_bytes": 1000,
            },
        )
        worker._agent_runtime.operation_log.append(
            "sync_event_recorded",
            room_id="room-runtime-status",
            plan_id=str(active_plan_id or ""),
            message="peer_asset_ready",
            payload={
                "event_type": "peer_asset_ready",
                "asset_id": "asset-gm-secret",
                "peer_id": "peer-gm-secret",
                "transfer_status": "completed",
            },
        )
        worker._agent_runtime.operation_log.append(
            "sync_event_recorded",
            room_id="room-runtime-status",
            plan_id=str(active_plan_id or ""),
            message="room_joined",
            payload={"event_type": "room_joined", "peer_id": "peer-gm-secret"},
        )
        worker._agent_runtime.operation_log.append(
            "sync_event_recorded",
            room_id="room-runtime-status",
            plan_id=str(active_plan_id or ""),
            message="sync_reconcile_completed",
            payload={"event_type": "sync_reconcile_completed", "peer_id": "peer-gm-secret", "status": "completed"},
        )
        worker._agent_runtime.operation_log.append(
            "sync_event_record_failed",
            room_id="room-runtime-status",
            plan_id=str(active_plan_id or ""),
            message="sync_event_record_failed",
            payload={
                "event_type": "actor_transform",
                "failure_code": "sync_event_record_failed",
                "provider": "hidden-provider",
                "url": "https://example.invalid/sync",
            },
        )

        reply = worker._handle_coordinator_status_query({
            "room_id": "room-runtime-status",
            "message_id": "msg-summary",
            "text": "@GM 总结当前方案",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("GM Runtime", reply or "")
        self.assertIn("当前方案：测试方案", reply or "")
        self.assertIn("上下文：0 条", reply or "")
        self.assertIn("多人同步健康：needs_attention", reply or "")
        self.assertIn("Report health:", reply or "")
        self.assertIn("attention yes", reply or "")
        self.assertIn("message-delivery-failed", reply or "")
        self.assertIn("failure codes message-delivery-failed:1", reply or "")
        self.assertIn("latest failure message-delivery-failed", reply or "")
        self.assertIn("同步复盘：", reply or "")
        self.assertIn("asset progress", reply or "")
        self.assertIn("peer-ready 1", reply or "")
        self.assertIn("peer join/leave 1/0", reply or "")
        self.assertIn("reconcile 1/0", reply or "")
        self.assertIn("failure codes sync-event-record-failed:1", reply or "")
        self.assertIn("latest failure sync-event-record-failed", reply or "")
        self.assertIn("模型同传：assets 1", reply or "")
        self.assertIn("progress 60%", reply or "")
        self.assertIn("Engine write:", reply or "")
        self.assertNotIn("readiness-mismatch", reply or "")
        self.assertIn("status-export 1(recorded, bridge-failed:1", reply or "")
        self.assertIn("readiness native:1,runtime-state:2,fallback:1,disabled:1", reply or "")
        self.assertIn("channels native actor-import", reply or "")
        self.assertIn("runtime-state actor-delete/layout-transform", reply or "")
        self.assertIn("disabled environment-import", reply or "")
        self.assertIn("cpp-actor-transform-failed:1", reply or "")
        self.assertNotIn("ToolCallGraph", reply or "")
        self.assertNotIn("agent_reply", reply or "")
        self.assertNotIn("network_send_agent_reply_ex", reply or "")
        self.assertNotIn("tool_name", reply or "")
        self.assertNotIn("provider", reply or "")
        self.assertNotIn("prompt", reply or "")
        self.assertNotIn("source_user_id", reply or "")
        self.assertNotIn("房主@secret", reply or "")
        self.assertNotIn("https://example.invalid", reply or "")
        self.assertNotIn("peer-gm-secret", reply or "")
        self.assertNotIn("asset-gm-secret", reply or "")
        self.assertNotIn("E:/secret/gm.glb", reply or "")
        self.assertEqual(coordinator.ingest_calls, [])
        self.assertIn("runtime_gm_summary_exported", worker._agent_runtime.operation_log.events())

    def test_gm_summary_reads_runtime_draft_plan_without_coordinator_active_plan(self) -> None:
        coordinator = _ExplodingCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._agent_runtime.sync_external_plan_context(
            room_id="room-runtime-draft-summary",
            external_plan_id="planning:msg-topic",
            text="围绕强盗藏宝室主题讨论一下，中央宝箱，两侧火把和武器架。",
            owner_agent="长者",
        )

        reply = worker._handle_coordinator_status_query({
            "room_id": "room-runtime-draft-summary",
            "message_id": "msg-runtime-draft-summary",
            "text": "@GM 总结当前方案",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("GM Runtime", reply or "")
        self.assertIn("强盗藏宝室", reply or "")
        self.assertIn("上下文：1 条", reply or "")
        self.assertIn("最近用户要点", reply or "")
        self.assertEqual(coordinator.ingest_calls, [])
        self.assertIn("runtime_gm_summary_exported", worker._agent_runtime.operation_log.events())

    def test_gm_r3_gate_query_is_read_only_and_bypasses_coordinator(self) -> None:
        coordinator = _ExplodingCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        rooms_before = dict(worker._agent_runtime.state.rooms)
        version_before = worker._agent_runtime.state.version
        operations_before = [
            entry.as_dict() for entry in worker._agent_runtime.operation_log.entries()
        ]

        trigger = {
            "room_id": "room-r3-gate-missing",
            "message_id": "msg-r3-gate",
            "text": "@GM R3门禁",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        }
        reply = worker._handle_coordinator_status_query(trigger)

        self.assertIn("【R3 门禁】RED", reply or "")
        self.assertIn("Snapshot 完整性：RED", reply or "")
        self.assertIn("环境就绪：RED", reply or "")
        self.assertIn("实体就绪：RED", reply or "")
        self.assertIn("多人一致性：YELLOW", reply or "")
        self.assertIn("Game-ready：0/0", reply or "")
        self.assertEqual(worker._agent_runtime.state.rooms, rooms_before)
        self.assertEqual(worker._agent_runtime.state.version, version_before)
        self.assertEqual(
            [entry.as_dict() for entry in worker._agent_runtime.operation_log.entries()],
            operations_before,
        )
        self.assertEqual(coordinator.ingest_calls, [])
        self.assertTrue(worker._is_runtime_r3_gate_query(trigger))
        self.assertFalse(worker._is_runtime_r3_gate_query({
            **trigger,
            "text": "检查门禁",
            "agent_id": "agent-1",
            "agent_name": "商人",
        }))

    def test_gm_r3_gate_reply_exposes_render_readiness_diagnostics(self) -> None:
        worker = _TestWorker(
            interaction_coordinator=_FakeCoordinator(),
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        dimensions = {
            name: {"status": "green", "metrics": {}}
            for name in (
                "snapshot_integrity",
                "environment_readiness",
                "entity_readiness",
                "finalizer_completeness",
                "business_graph_consistency",
                "multiplayer_consistency",
                "runtime_write_safety",
            )
        }
        dimensions["entity_readiness"] = {
            "status": "yellow",
            "metrics": {
                "entity_count": 9,
                "expected_entity_count": 14,
                "game_ready_entity_count": 6,
                "render_status_observed_count": 9,
                "render_ready_entity_count": 7,
                "invalid_mesh_entity_count": 2,
                "invalid_mesh_slot_count": 3,
                "readiness_missing_field_counts": {
                    "grounding_status": 3,
                    "render_not_ready": 2,
                },
            },
        }
        gate_report = {
            "gate_report_id": "r3gate-test",
            "plan_id": "plan-test",
            "scene_version": 4,
            "overall": "yellow",
            "dimensions": dimensions,
            "metrics": {"entity_count": 9, "game_ready_entity_count": 6},
            "blockers": [],
            "capability_unlocks": ["readonly_snapshot_analysis"],
        }

        with patch.object(
            worker._agent_runtime,
            "handle_message",
            return_value={"plan_id": "plan-test", "gate_report": gate_report},
        ):
            reply = worker._agent_runtime_r3_gate_reply(room_id="room-render-gate")

        self.assertIn("Game-ready：6/14", reply)
        self.assertIn("渲染就绪：7/9（已观测 9/9；无效 Mesh 实体 2，slot 3）", reply)
        self.assertIn("实体待检查：grounding_status x3；render_not_ready x2", reply)

    def test_gm_summary_includes_runtime_pending_intervention_summary(self) -> None:
        coordinator = _FakeCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        plan = worker._agent_runtime.sync_external_plan_context(
            room_id="room-runtime-intervention-summary",
            external_plan_id="seed-test",
            text="强盗藏宝室方案：中央宝箱，两侧武器架，入口火把",
            owner_agent="山贼",
        )
        worker._agent_runtime.record_intervention(
            room_id="room-runtime-intervention-summary",
            plan_id=plan.plan_id,
            text="再添加一个天使雕像",
            source_user="房主",
            target_agent="山贼",
        )

        reply = worker._handle_coordinator_status_query({
            "room_id": "room-runtime-intervention-summary",
            "message_id": "msg-intervention-summary",
            "text": "@GM 总结当前方案",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("pending 1", reply or "")
        self.assertIn("absorbable 1", reply or "")
        self.assertNotIn("patch_id", reply or "")
        self.assertNotIn("metadata", reply or "")
        self.assertNotIn("tool_name", reply or "")

    def test_gm_summary_includes_runtime_intervention_batch_summary(self) -> None:
        coordinator = _ExplodingCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        plan = worker._agent_runtime.handle_message(
            room_id="room-runtime-intervention-batch-summary",
            text="做一个强盗藏宝室，包含宝箱、金币和火把",
            sender_id="host-1",
            sender_name="房主",
            action="plan",
            external_plan_id="seed-intervention-batch-summary",
        )["plan"]
        runtime_plan_id = plan["plan_id"]
        worker._agent_runtime.confirm_scene_plan(runtime_plan_id, confirmed_by="房主")
        worker._agent_runtime.enqueue_planned_batches(runtime_plan_id, max_items_per_batch=2)
        worker._agent_runtime.handle_message(
            room_id="room-runtime-intervention-batch-summary",
            text="再添加一个天使雕像",
            sender_id="host-1",
            sender_name="房主",
            action="intervention_add",
            external_plan_id="seed-intervention-batch-summary",
        )
        worker._agent_runtime.handle_message(
            room_id="room-runtime-intervention-batch-summary",
            text="排入下一批",
            sender_id="host-1",
            sender_name="房主",
            action="enqueue_pending_interventions",
            external_plan_id="seed-intervention-batch-summary",
        )

        reply = worker._handle_coordinator_status_query({
            "room_id": "room-runtime-intervention-batch-summary",
            "message_id": "msg-intervention-batch-summary",
            "text": "@GM 现在生成到哪里了",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("Runtime", reply or "")
        self.assertIn("1 batch(es)", reply or "")
        self.assertIn("Tool execution", reply or "")
        self.assertNotIn("patch_id", reply or "")
        self.assertNotIn("tool_name", reply or "")
        self.assertNotIn("provider", reply or "")
        self.assertNotIn("mock_actor_import", reply or "")
        self.assertEqual(coordinator.ingest_calls, [])

    def test_gm_environment_summary_discloses_requested_and_failed_components(self) -> None:
        ready_text = LANChatAgentWorker._format_agent_runtime_environment_report({
            "component_count": 2,
            "requested_count": 2,
            "ready_count": 2,
            "imported_count": 2,
            "failed_count": 0,
            "type_counts": {"skybox": 1, "terrain": 1},
        })
        failed_text = LANChatAgentWorker._format_agent_runtime_environment_report({
            "component_count": 0,
            "event_count": 1,
            "requested_count": 2,
            "ready_count": 0,
            "failed_count": 2,
            "status_counts": {"failed": 1},
            "type_counts": {},
        })

        self.assertIn("2 component(s)", ready_text)
        self.assertIn("ready 2", ready_text)
        self.assertIn("imported 2", ready_text)
        self.assertIn("requested 2", ready_text)
        self.assertIn("skybox: 1", ready_text)
        self.assertNotEqual(failed_text, "none")
        self.assertIn("ready 0", failed_text)
        self.assertIn("requested 2", failed_text)
        self.assertIn("failed 2", failed_text)

    def test_runtime_replay_report_discloses_environment_import_events(self) -> None:
        text = LANChatAgentWorker._format_agent_runtime_replay_report({
            "entry_count": 8,
            "event_counts": {"tool_graph_completed": 1},
            "latest_events": [{"event": "runtime_event_emitted"}],
            "engine_write_boundary_summary": {
                "boundary_fact_count": 1,
                "import_boundary_count": 1,
                "transform_boundary_count": 0,
                "delete_boundary_count": 0,
                "write_source_counts": {"engine_actor_import": 1},
                "status_counts": {"success": 1},
            },
            "environment_component_replay_summary": {
                "import_event_count": 2,
                "import_failed_event_count": 1,
            },
        })

        self.assertIn("env-import:2", text)
        self.assertIn("env-import-failed:1", text)
        self.assertIn("engine_write_boundary", text)
        self.assertIn("boundary 1", text)
        self.assertIn("engine_actor_import:1", text)
        self.assertNotIn("provider", text)

    def test_engine_write_report_discloses_environment_import_results(self) -> None:
        text = LANChatAgentWorker._format_agent_runtime_engine_write_report({
            "import_result_count": 1,
            "transform_result_count": 0,
            "environment_import_result_count": 2,
            "delete_result_count": 1,
            "import_status_counts": {"success": 1},
            "transform_status_counts": {},
            "environment_import_status_counts": {"failed": 1, "success": 1},
            "delete_status_counts": {"success": 1},
            "status_export_count": 1,
            "latest_status_export": {
                "recorded": True,
                "engine_write_bridge_failed_count": 2,
                "engine_write_bridge_error_code_counts": {"cpp_actor_import_failed": 2},
            },
        })

        self.assertIn("import 1(success:1)", text)
        self.assertIn("transform 0", text)
        self.assertIn("env-import 2(failed:1,success:1)", text)
        self.assertIn("actor-delete 1(success:1)", text)
        self.assertIn("status-export 1(recorded, bridge-failed:2", text)
        self.assertIn("cpp-actor-import-failed:2", text)
        self.assertNotIn("provider", text)

    def test_engine_write_boundary_report_is_safe_and_user_readable(self) -> None:
        text = LANChatAgentWorker._format_agent_runtime_engine_write_boundary_report({
            "boundary_fact_count": 3,
            "import_boundary_count": 1,
            "transform_boundary_count": 1,
            "delete_boundary_count": 1,
            "write_source_counts": {
                "engine_actor_import": 1,
                "runtime_layout_transform": 1,
                "secret_provider_raw": 1,
            },
            "status_counts": {"success": 2, "failed": 1},
            "bridge_call_count": 2,
            "bridge_success_count": 2,
            "bridge_failed_count": 0,
        })

        self.assertIn("boundary 3", text)
        self.assertIn("import/transform/delete 1/1/1", text)
        self.assertIn("engine_actor_import:1", text)
        self.assertIn("runtime_layout_transform:1", text)
        self.assertIn("success:2", text)
        self.assertIn("native verified", text)
        self.assertNotIn("provider", text)
        self.assertNotIn("secret_provider_raw", text)

        runtime_state_only_text = LANChatAgentWorker._format_agent_runtime_engine_write_boundary_report({
            "boundary_fact_count": 1,
            "import_boundary_count": 1,
            "status_counts": {"runtime_state_only": 1},
            "bridge_call_count": 0,
            "bridge_success_count": 0,
            "bridge_failed_count": 0,
        })
        self.assertIn("runtime_state_only:1", runtime_state_only_text)
        self.assertIn("bridge 0/0/0", runtime_state_only_text)
        self.assertIn("native pending F5", runtime_state_only_text)

    def test_gm_summary_includes_runtime_sync_summary(self) -> None:
        coordinator = _ExplodingCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        plan = worker._agent_runtime.handle_message(
            room_id="room-runtime-gm-sync-summary",
            text="做一个强盗藏宝室，包含宝箱、金币和火把",
            sender_id="host-1",
            sender_name="房主",
            action="plan",
            external_plan_id="seed-gm-sync-summary",
        )["plan"]
        worker._agent_runtime.confirm_scene_plan(plan["plan_id"], confirmed_by="房主")
        worker.handle_lanchat_sync_event({
            "event": "actor_created",
            "room_id": "room-runtime-gm-sync-summary",
            "actor_guid": "actor-sync-gm-001",
            "actor_name": "藏宝箱",
            "status": "broadcast",
            "asset_path": "E:/secret/box.glb",
            "message_id": "msg-internal",
        })
        worker.handle_lanchat_sync_event({
            "event": "file_chunk_received",
            "room_id": "room-runtime-gm-sync-summary",
            "asset_id": "asset-gm-progress",
            "progress": 75,
            "chunk_index": 6,
            "chunk_count": 8,
            "asset_path": "E:/secret/progress.glb",
            "message_id": "msg-internal-progress",
        })
        worker.handle_lanchat_sync_event({
            "event": "room_joined",
            "room_id": "room-runtime-gm-sync-summary",
            "peer_id": "peer-secret",
            "message_id": "msg-peer-join",
        })
        worker.handle_lanchat_sync_event({
            "event": "left",
            "room_id": "room-runtime-gm-sync-summary",
            "peer_id": "peer-secret",
            "message_id": "msg-peer-left",
        })
        worker._agent_runtime.operation_log.append(
            "runtime_system_event_disclosure_skipped",
            room_id="room-runtime-gm-sync-summary",
            plan_id=plan["plan_id"],
            batch_id="batch-gm-runtime-event",
            payload={
                "runtime_event_type": "agent_internal",
                "runtime_audience": "agent",
                "reason": "audience_not_user_visible",
                "provider": "secret-provider",
                "prompt": "secret-prompt",
            },
        )

        reply = worker._handle_coordinator_status_query({
            "room_id": "room-runtime-gm-sync-summary",
            "message_id": "msg-gm-sync-summary",
            "text": "@GM 总结当前方案",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("GM Runtime", reply or "")
        self.assertIn("多人同步健康：", reply or "")
        self.assertIn("partial, attention 1", reply or "")
        self.assertIn("needs asset-transfer-in-progress", reply or "")
        self.assertIn("actors create/transform/delete 1/0/0", reply or "")
        self.assertIn("active 1", reply or "")
        self.assertIn("peers join/leave 1/1", reply or "")
        self.assertIn("RuntimeEvent replay:", reply or "")
        self.assertIn("skipped 1", reply or "")
        self.assertIn("latest-skip agent-internal:agent", reply or "")
        self.assertNotIn("asset-gm-progress", reply or "")
        self.assertNotIn("复盘 recorded", reply or "")
        self.assertNotIn("secret-provider", reply or "")
        self.assertNotIn("secret-prompt", reply or "")
        self.assertIn("藏宝箱", reply or "")
        self.assertNotIn("box.glb", reply or "")
        self.assertNotIn("progress.glb", reply or "")
        self.assertNotIn("peer-secret", reply or "")
        self.assertNotIn("message_id", reply or "")
        self.assertEqual(coordinator.ingest_calls, [])

    def test_runtime_provider_status_query_runs_preflight_without_creating_plan(self) -> None:
        coordinator = _FakeCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({
                "AGENT_RUNTIME_USE_SCENE_SNAPSHOT_PROVIDER": "1",
                "AGENT_RUNTIME_USE_ENGINE_IMPORT_PROVIDER": "1",
            }),
        )
        worker._agent_runtime.operation_log.append(
            "runtime_system_event_send_failed",
            room_id="room-runtime-provider-status",
            payload={
                "message_kind": "runtime_status",
                "channel": "network_send_system_message_ex",
                "sent": False,
                "stage": "资源预检",
                "progress": 0,
                "source_user_id": "host-secret",
                "provider": "hidden",
                "prompt": "hidden",
                "url": "https://example.invalid/private",
            },
        )
        worker._agent_runtime.operation_log.append(
            "tool_call_succeeded",
            room_id="room-runtime-provider-status",
            payload={
                "transform_results": [
                    {"actor_id": "actor-box", "actor_name": "藏宝箱", "status": "success", "observed_position": True}
                ],
            },
        )
        worker._agent_runtime.state.apply_patch(
            StatePatch(
                room_id="room-runtime-provider-status",
                changes={
                    "custom_import_facts": {
                        "batch-provider-status:actor_import_result": {
                            "batch_id": "batch-provider-status",
                            "status": "completed",
                            "engine_write_boundary": {
                                "provider_source": "engine_actor_import_provider",
                                "requested_count": 1,
                                "identity_result_count": 1,
                                "missing_identity_count": 0,
                                "status_counts": {"success": 1},
                            },
                        }
                    }
                },
                expected_version=worker._agent_runtime.state.version,
            )
        )

        reply = worker._handle_agent_runtime_provider_status_query({
            "room_id": "room-runtime-provider-status",
            "message_id": "msg-provider-status",
            "text": "@GM runtime provider status preflight",
            "sender_id": "host-1",
            "sender_name": "host",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("scene-snapshot", reply or "")
        self.assertIn("environment-component", reply or "")
        self.assertIn("missing_engine", reply or "")
        self.assertIn("readiness:", reply or "")
        self.assertIn("engine_write:", reply or "")
        self.assertIn("engine_write_boundary:", reply or "")
        self.assertIn("boundary 1", reply or "")
        self.assertIn("engine_actor_import:1", reply or "")
        self.assertIn("transform 1", reply or "")
        self.assertIn("success:1", reply or "")
        self.assertIn("message_delivery:", reply or "")
        self.assertIn("runtime_status", reply or "")
        self.assertIn("network_send_system_message_ex", reply or "")
        self.assertNotIn("provider", reply or "")
        self.assertNotIn("mock_provider_model", reply or "")
        self.assertNotIn("tool_name", reply or "")
        self.assertNotIn("prompt", reply or "")
        self.assertNotIn("source_user_id", reply or "")
        self.assertNotIn("host-secret", reply or "")
        self.assertNotIn("https://example.invalid", reply or "")
        room = worker._agent_runtime.query_state("room-runtime-provider-status")["room"]
        self.assertEqual(room["scene_plans"], {})
        self.assertEqual(room["active_plan_id"], "")
        self.assertIn("provider_readiness", room)
        self.assertIn("runtime_provider_status_queried", worker._agent_runtime.operation_log.events())
        self.assertEqual(coordinator.ingest_calls, [])

    def test_model_provider_flag_does_not_fallback_to_legacy_model_provider(self) -> None:
        with patch.object(
            LANChatAgentWorker,
            "_get_runtime_tool",
            lambda self, name: None,
        ):
            worker = _TestWorker(
                agent_runtime_flags=AgentRuntimeFlags.from_env({
                    "AGENT_RUNTIME_USE_MODEL_PROVIDER": "1",
                }),
            )

        result = worker._agent_runtime.handle_message(
            room_id="room-model-provider-no-legacy-fallback",
            text="check runtime provider readiness",
            action="provider_status",
        )

        room = worker._agent_runtime.query_state("room-model-provider-no-legacy-fallback")["room"]
        readiness = dict(room.get("provider_readiness") or {})
        model_readiness = dict(readiness.get("model_resource") or {})
        configured = worker._agent_runtime.operation_log.query(
            event="runtime_provider_modes_configured",
            limit=1,
        )[0].payload
        model_configured = dict(configured.get("model_resource") or {})

        self.assertTrue(result["handled"])
        self.assertEqual(model_readiness.get("requested"), True)
        self.assertEqual(model_readiness.get("status"), "unavailable")
        self.assertNotIn("legacy_model_provider", str(model_readiness))
        self.assertNotIn("legacy_model_provider", str(model_configured))
        self.assertNotIn("legacy_model", str(result))
        self.assertEqual(room["active_plan_id"], "")
        self.assertEqual(room["scene_plans"], {})

    def test_runtime_worker_drain_query_drains_queue_without_legacy_execute(self) -> None:
        coordinator = _FakeCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        plan = worker._agent_runtime.sync_external_plan_context(
            room_id="room-runtime-worker-drain",
            external_plan_id="seed-test",
            text="做一个强盗藏宝室，包含宝箱、金币和火把",
            owner_agent="山贼",
        )
        queued = worker._agent_runtime.handle_message(
            room_id="room-runtime-worker-drain",
            text="确认并排队",
            sender_id="host-1",
            sender_name="房主",
            action="confirm_and_enqueue",
            external_plan_id="seed-test",
            max_items_per_batch=2,
        )
        queued_graph_count = len(queued["graphs"])
        self.assertGreaterEqual(queued_graph_count, 2)

        reply = worker._handle_agent_runtime_worker_drain_query({
            "room_id": "room-runtime-worker-drain",
            "message_id": "msg-worker-drain",
            "text": "@GM runtime worker drain",
            "sender_id": "host-1",
            "sender_name": "host",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("Runtime Worker", reply or "")
        self.assertIn("drained graphs: 1", reply or "")
        self.assertEqual(
            worker._agent_runtime.status_summary("room-runtime-worker-drain", plan_id=plan.plan_id)["tool_graph_summary"]["queue_status_counts"],
            {"completed": 1, "queued": queued_graph_count - 1},
        )
        execution_digest = worker._agent_runtime.status_summary(
            "room-runtime-worker-drain",
            plan_id=plan.plan_id,
        )["tool_execution_digest"]
        self.assertTrue(execution_digest["available"])
        self.assertGreater(execution_digest["node_count"], 0)
        self.assertEqual(execution_digest["failed_count"], 0)
        self.assertIn("runtime_message_drained", worker._agent_runtime.operation_log.events())
        self.assertEqual(coordinator.execute_calls, [])
        self.assertEqual(coordinator.ingest_calls, [])

    def test_runtime_worker_drain_query_reads_active_plan_from_runtime_not_coordinator(self) -> None:
        coordinator = _ExplodingCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._agent_runtime.handle_message(
            room_id="room-worker-runtime-first",
            text="做一个强盗藏宝室，包含宝箱和火把",
            sender_id="host-1",
            sender_name="房主",
            action="confirm_and_enqueue",
            external_plan_id="seed-runtime-first",
            max_items_per_batch=2,
        )

        reply = worker._handle_agent_runtime_worker_drain_query({
            "room_id": "room-worker-runtime-first",
            "message_id": "msg-worker-runtime-first",
            "text": "@GM runtime worker drain",
            "sender_id": "host-1",
            "sender_name": "host",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("Runtime Worker", reply or "")
        self.assertIn("runtime_message_drained", worker._agent_runtime.operation_log.events())

    def test_runtime_command_reads_active_plan_from_runtime_not_coordinator(self) -> None:
        coordinator = _ExplodingCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._agent_runtime.handle_message(
            room_id="room-command-runtime-first",
            text="做一个强盗藏宝室，包含宝箱和火把",
            sender_id="host-1",
            sender_name="房主",
            action="confirm_and_enqueue",
            external_plan_id="seed-runtime-command",
            max_items_per_batch=2,
        )

        reply = worker._handle_agent_runtime_command({
            "room_id": "room-command-runtime-first",
            "message_id": "msg-command-runtime-first",
            "text": "暂停生成",
            "sender_id": "host-1",
            "sender_name": "host",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("Runtime", reply or "")
        state = worker._agent_runtime.query_state("room-command-runtime-first")["room"]
        runtime_plan_id = state["external_plan_links"]["seed-runtime-command"]
        self.assertEqual(state["scene_plans"][runtime_plan_id]["status"], "paused")

    def test_runtime_worker_drain_query_without_plan_does_not_create_plan(self) -> None:
        coordinator = _FakeNoActiveCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        reply = worker._handle_agent_runtime_worker_drain_query({
            "room_id": "room-runtime-worker-empty",
            "message_id": "msg-worker-drain-empty",
            "text": "@GM runtime worker drain",
            "sender_id": "host-1",
            "sender_name": "host",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("Runtime Worker", reply or "")
        self.assertIn("drained graphs: 0", reply or "")
        room = worker._agent_runtime.query_state("room-runtime-worker-empty")["room"]
        self.assertEqual(room["scene_plans"], {})
        self.assertEqual(room["active_plan_id"], "")
        self.assertIn("runtime_worker_drain_requested", worker._agent_runtime.operation_log.events())
        self.assertEqual(coordinator.execute_calls, [])
        self.assertEqual(coordinator.ingest_calls, [])

    def test_runtime_enqueue_generation_query_queues_without_legacy_execute(self) -> None:
        coordinator = _FakeCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        plan = worker._agent_runtime.sync_external_plan_context(
            room_id="room-runtime-enqueue-generation",
            external_plan_id="seed-test",
            text="做一个强盗藏宝室，包含宝箱、金币和火把",
            owner_agent="山贼",
        )

        with patch.object(
            worker._agent_runtime,
            "query_state",
            side_effect=AssertionError("LANChat enqueue bridge must not pre-read RuntimeState"),
        ):
            reply = worker._handle_agent_runtime_enqueue_generation_query({
                "room_id": "room-runtime-enqueue-generation",
                "message_id": "msg-runtime-enqueue",
                "text": "@GM runtime enqueue generation",
                "sender_id": "host-1",
                "sender_name": "host",
                "sender_type": "host",
                "message_kind": "chat",
                "agent_id": "gm",
                "agent_name": "GM",
                "is_host": True,
            })

        self.assertIn("AgentRuntime Enqueue", reply or "")
        self.assertIn("queued", reply or "")
        room = worker._agent_runtime.query_state("room-runtime-enqueue-generation")["room"]
        runtime_plan_id = room["external_plan_links"]["seed-test"]
        self.assertEqual(runtime_plan_id, plan.plan_id)
        queue_counts = worker._agent_runtime.status_summary(
            "room-runtime-enqueue-generation",
            plan_id=runtime_plan_id,
        )["tool_graph_summary"]["queue_status_counts"]
        self.assertIn("queued", queue_counts)
        self.assertEqual(room["actors"], {})
        self.assertIn("runtime_message_enqueued", worker._agent_runtime.operation_log.events())
        self.assertNotIn("runtime_message_executed", worker._agent_runtime.operation_log.events())
        self.assertEqual(coordinator.seed_plan_calls, [])
        self.assertEqual(coordinator.action_payload_calls, [])
        self.assertEqual(coordinator.execute_calls, [])
        self.assertEqual(coordinator.ingest_calls, [])

    def test_runtime_enqueue_generation_failure_does_not_leak_internal_exception_text(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        worker._agent_runtime.sync_external_plan_context(
            room_id="room-runtime-enqueue-error-safe",
            external_plan_id="seed-enqueue-error-safe",
            text="做一个强盗藏宝室，包含宝箱、金币和火把",
            owner_agent="山贼",
        )
        sensitive_error = RuntimeError(
            "provider=https://secret.invalid prompt=hidden E:/private/enqueue.json api_key=SECRET"
        )

        with patch.object(worker._agent_runtime, "handle_message", side_effect=sensitive_error):
            reply = worker._handle_agent_runtime_enqueue_generation_query({
                "room_id": "room-runtime-enqueue-error-safe",
                "message_id": "msg-runtime-enqueue-error-safe",
                "text": "@GM runtime enqueue generation",
                "sender_id": "host-1",
                "sender_name": "host",
                "sender_type": "host",
                "message_kind": "chat",
                "agent_id": "gm",
                "agent_name": "GM",
                "is_host": True,
            })

        self.assertIn("内部执行异常已记录", reply or "")
        self.assertNotIn("https://secret.invalid", reply or "")
        self.assertNotIn("prompt=hidden", reply or "")
        self.assertNotIn("E:/private", reply or "")
        self.assertNotIn("api_key", reply or "")

    def test_runtime_enqueue_generation_query_without_runtime_plan_does_not_touch_coordinator_by_default(self) -> None:
        coordinator = _ExplodingCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        reply = worker._handle_agent_runtime_enqueue_generation_query({
            "room_id": "room-runtime-enqueue-no-plan",
            "message_id": "msg-runtime-enqueue-no-plan",
            "text": "@GM runtime enqueue generation",
            "sender_id": "host-1",
            "sender_name": "host",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("no active Runtime ScenePlan", reply or "")
        room = worker._agent_runtime.query_state("room-runtime-enqueue-no-plan")["room"]
        self.assertEqual(room["scene_plans"], {})
        self.assertEqual(room["active_plan_id"], "")

    def test_runtime_engine_write_status_query_reports_write_adapters_without_creating_plan(self) -> None:
        coordinator = _FakeCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({
                "AGENT_RUNTIME_USE_ENGINE_IMPORT_PROVIDER": "1",
                "AGENT_RUNTIME_USE_ENGINE_TRANSFORM_PROVIDER": "1",
            }),
        )
        worker._agent_runtime.operation_log.append(
            "tool_call_succeeded",
            room_id="room-runtime-engine-write",
            payload={
                "import_results": [
                    {
                        "actor_id": "actor-secret",
                        "actor_name": "藏宝箱",
                        "status": "success",
                        "provider": "hidden",
                        "prompt": "hidden",
                        "url": "https://example.invalid/private",
                    }
                ],
                "transform_results": [
                    {"actor_id": "actor-box", "actor_name": "藏宝箱", "status": "success"}
                ],
                "environment_import_results": [
                    {"component_id": "env-room", "component_name": "__room_box", "status": "success"}
                ],
            },
        )

        reply = worker._handle_agent_runtime_engine_write_status_query({
            "room_id": "room-runtime-engine-write",
            "message_id": "msg-engine-write",
            "text": "@GM runtime engine write status",
            "sender_id": "host-1",
            "sender_name": "host",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("environment_import", reply or "")
        self.assertIn("actor_import", reply or "")
        self.assertIn("actor_delete", reply or "")
        self.assertIn("layout_transform", reply or "")
        self.assertIn("missing_engine", reply or "")
        self.assertIn("replay:", reply or "")
        self.assertIn("engine boundary:", reply or "")
        self.assertIn("boundary 0", reply or "")
        self.assertIn("import 1", reply or "")
        self.assertIn("transform 1", reply or "")
        self.assertIn("env-import 1", reply or "")
        self.assertIn("success:1", reply or "")
        self.assertNotIn("provider", reply or "")
        self.assertNotIn("prompt", reply or "")
        self.assertNotIn("https://example.invalid", reply or "")
        room = worker._agent_runtime.query_state("room-runtime-engine-write")["room"]
        self.assertEqual(room["scene_plans"], {})
        self.assertEqual(room["active_plan_id"], "")
        self.assertIn("provider_readiness", room)
        self.assertEqual(coordinator.ingest_calls, [])

    def test_runtime_engine_write_status_query_reports_engine_write_boundary(self) -> None:
        coordinator = _FakeCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({
                "AGENT_RUNTIME_USE_ENGINE_IMPORT_PROVIDER": "1",
            }),
        )
        runtime_result = worker._agent_runtime.handle_message(
            room_id="room-runtime-engine-boundary",
            text="做一个强盗藏宝室，有宝箱和火把",
            sender_id="host-1",
            sender_name="host",
            action="confirm_and_execute",
            external_plan_id="seed-engine-boundary",
            max_items_per_batch=2,
        )
        batch_id = runtime_result["batches"][0]["batch_id"]
        worker._agent_runtime.state.apply_patch(
            StatePatch(
                room_id="room-runtime-engine-boundary",
                changes={
                    "custom_import_facts": {
                        f"{batch_id}:actor_import_result": {
                            "batch_id": batch_id,
                            "status": "completed",
                            "engine_write_boundary": {
                                "provider_source": "engine_actor_import_provider",
                                "requested_count": 2,
                                "identity_result_count": 2,
                                "missing_identity_count": 0,
                                "status_counts": {"success": 2},
                            },
                        }
                    }
                },
                expected_version=worker._agent_runtime.state.version,
            )
        )

        reply = worker._handle_agent_runtime_engine_write_status_query({
            "room_id": "room-runtime-engine-boundary",
            "message_id": "msg-engine-boundary",
            "text": "@GM runtime engine write status",
            "sender_id": "host-1",
            "sender_name": "host",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("engine boundary:", reply or "")
        self.assertIn("boundary ", reply or "")
        self.assertIn("import/transform/delete ", reply or "")
        self.assertIn("engine_actor_import:1", reply or "")
        self.assertIn("success:", reply or "")
        self.assertNotIn("provider", reply or "")
        self.assertNotIn("prompt", reply or "")
        self.assertEqual(coordinator.ingest_calls, [])

    def test_runtime_scene_snapshot_query_refreshes_without_creating_plan(self) -> None:
        class FakeSnapshotTool:
            def __init__(self) -> None:
                self.calls: list[dict] = []

            def invoke(self, payload: dict) -> dict:
                self.calls.append(dict(payload))
                return {
                    "scene_name": payload.get("scene_name") or "Scene/场景1.scene",
                    "source": "fake_engine",
                    "actors": [
                        {
                            "actor_id": "actor-001",
                            "actor_guid": "actor-001",
                            "name": "藏宝箱",
                            "position": [1.0, 0.0, 2.0],
                        }
                    ],
                    "bounds_ready": True,
                }

        snapshot_tool = FakeSnapshotTool()
        coordinator = _FakeCoordinator()
        flags = AgentRuntimeFlags.from_env({
            "AGENT_RUNTIME_USE_SCENE_SNAPSHOT_PROVIDER": "1",
        })
        with patch.object(
            LANChatAgentWorker,
            "_get_runtime_tool",
            lambda self, name: snapshot_tool if name == "get_scene_snapshot" else None,
        ):
            worker = _TestWorker(
                corona_engine=object(),
                interaction_coordinator=coordinator,
                agent_runtime_flags=flags,
            )

        reply = worker._handle_agent_runtime_scene_snapshot_query({
            "room_id": "room-runtime-scene-snapshot",
            "message_id": "msg-scene-snapshot",
            "text": "@GM runtime scene snapshot status",
            "sender_id": "host-1",
            "sender_name": "host",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("completed", reply or "")
        self.assertIn("actor_count: 1", reply or "")
        self.assertIn("scene_snapshot_tool", reply or "")
        self.assertNotIn("fake_engine", reply or "")
        self.assertTrue(snapshot_tool.calls)
        room = worker._agent_runtime.query_state("room-runtime-scene-snapshot")["room"]
        self.assertEqual(room["scene_plans"], {})
        self.assertEqual(room["active_plan_id"], "")
        self.assertIn("actor-001", room["observed_actors"])
        self.assertEqual(coordinator.ingest_calls, [])

    def test_runtime_tool_manifest_query_lists_capabilities_without_creating_plan(self) -> None:
        coordinator = _FakeCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        reply = worker._handle_agent_runtime_tool_manifest_query({
            "room_id": "room-runtime-tools",
            "message_id": "msg-runtime-tools",
            "text": "@GM runtime tool capabilities",
            "sender_id": "host-1",
            "sender_name": "host",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("tool_count", reply or "")
        self.assertIn("runtime.scene.snapshot", reply or "")
        self.assertIn("runtime.environment.import_components", reply or "")
        self.assertIn("runtime.actor.import_batch", reply or "")
        self.assertIn("runtime.layout.apply_delta", reply or "")
        self.assertIn("runtime.actor.mark_deleted", reply or "")
        self.assertNotIn("handler", reply or "")
        self.assertNotIn("provider", reply or "")
        room = worker._agent_runtime.query_state("room-runtime-tools")["room"]
        self.assertEqual(room["scene_plans"], {})
        self.assertEqual(room["active_plan_id"], "")
        self.assertEqual(coordinator.ingest_calls, [])
        self.assertIn("runtime_tool_manifest_queried", worker._agent_runtime.operation_log.events())

    def test_runtime_tool_manifest_exposes_engine_plane_tools_without_internals(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))

        manifest = worker._agent_runtime.tool_manifest()

        tools = manifest["tools"]
        tool_by_name = {
            str(item.get("name") or ""): item
            for item in tools
            if isinstance(item, dict)
        }
        for tool_name in (
            "runtime.environment.import_components",
            "runtime.actor.import_batch",
            "runtime.layout.apply_delta",
            "runtime.actor.mark_deleted",
        ):
            self.assertIn(tool_name, tool_by_name)
        self.assertEqual(tool_by_name["runtime.environment.import_components"]["category"], "import")
        self.assertEqual(tool_by_name["runtime.actor.import_batch"]["category"], "import")
        self.assertEqual(tool_by_name["runtime.layout.apply_delta"]["category"], "geometry")
        self.assertEqual(tool_by_name["runtime.actor.mark_deleted"]["default_risk_level"], "high")
        self.assertTrue(tool_by_name["runtime.environment.import_components"]["requires_write"])
        self.assertTrue(tool_by_name["runtime.actor.import_batch"]["requires_write"])
        self.assertTrue(tool_by_name["runtime.layout.apply_delta"]["requires_write"])
        manifest_text = json.dumps(manifest, ensure_ascii=False)
        self.assertNotIn("handler", manifest_text)
        self.assertNotIn("provider", manifest_text)
        self.assertNotIn("api_key", manifest_text)
        self.assertNotIn("model_path", manifest_text)
        self.assertNotIn("tool_call_id", manifest_text)

    def test_runtime_operation_replay_query_exports_safe_summary_without_creating_plan(self) -> None:
        coordinator = _FakeCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._agent_runtime.operation_log.append(
            "custom_room_event",
            room_id="room-runtime-replay",
            message="safe event",
            payload={"provider": "hidden", "prompt": "hidden"},
        )
        worker._agent_runtime.operation_log.append(
            "review_advisory_proposal_created",
            room_id="room-runtime-replay",
            plan_id="plan-review",
            payload={"proposal_id": "review-plan-review", "item_count": 1},
        )
        worker._agent_runtime.operation_log.append(
            "review_advisory_confirmation_recorded",
            room_id="room-runtime-replay",
            plan_id="plan-review",
            message="confirmed",
            payload={"proposal_id": "review-plan-review", "decision": "confirmed", "provider": "hidden"},
        )
        worker._agent_runtime.operation_log.append(
            "final_adjustment_confirmation_recorded",
            room_id="room-runtime-replay",
            plan_id="plan-review",
            message="confirmed",
            payload={
                "proposal_id": "final-plan-review",
                "decision": "confirmed",
                "conflict_item_count": 2,
                "conflict_items": ["hidden raw conflict"],
            },
        )
        worker._agent_runtime.operation_log.append(
            "tool_call_succeeded",
            room_id="room-runtime-replay",
            payload={
                "import_results": [
                    {"actor_id": "actor-chest", "actor_name": "藏宝箱", "status": "success"},
                    {
                        "actor_id": "actor-gold",
                        "actor_name": "金币堆",
                        "status": "failed",
                        "reason": "provider raw https://example.invalid/native",
                    },
                ]
            },
        )
        worker._agent_runtime.operation_log.append(
            "runtime_engine_write_status_exported",
            room_id="room-runtime-replay",
            payload={
                "recorded": True,
                "engine_write_boundary_fact_count": 1,
                "engine_write_import_boundary_count": 1,
                "engine_write_bridge_call_count": 2,
                "engine_write_bridge_success_count": 1,
                "engine_write_bridge_failed_count": 1,
                "engine_write_bridge_error_code_counts": {"cpp_actor_import_failed": 1},
                "engine_write_readiness_native_enabled_count": 1,
                "engine_write_readiness_runtime_state_only_count": 1,
                "engine_write_readiness_fallback_count": 1,
                "engine_write_readiness_disabled_count": 1,
                "engine_write_readiness_native_enabled_channels": ["actor_import"],
                "engine_write_readiness_runtime_state_only_channels": ["actor_delete"],
                "engine_write_readiness_fallback_channels": ["actor_import"],
                "engine_write_readiness_disabled_channels": ["environment_import"],
                "provider": "hidden",
                "prompt": "hidden",
            },
        )
        worker._agent_runtime.operation_log.append(
            "runtime_worker_drain_failed",
            room_id="room-runtime-replay",
            payload={
                "drained_count": 0,
                "reason": "synthetic queue drain failure",
                "provider": "hidden",
                "url": "https://example.invalid/drain",
            },
        )

        reply = worker._handle_agent_runtime_operation_replay_query({
            "room_id": "room-runtime-replay",
            "message_id": "msg-runtime-replay",
            "text": "@GM runtime operation replay",
            "sender_id": "host-1",
            "sender_name": "host",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("entry_count", reply or "")
        self.assertIn("custom_room_event", reply or "")
        self.assertIn("context:", reply or "")
        self.assertIn("0 context", reply or "")
        self.assertIn("review_advisory", reply or "")
        self.assertIn("proposals 1", reply or "")
        self.assertIn("confirmations 1", reply or "")
        self.assertIn("latest confirmed", reply or "")
        self.assertIn("final_adjustment", reply or "")
        self.assertIn("decisions confirmed:1", reply or "")
        self.assertIn("conflicts 2", reply or "")
        self.assertIn("engine_write:", reply or "")
        self.assertNotIn("readiness-mismatch", reply or "")
        self.assertIn("worker_drain:", reply or "")
        self.assertIn("failed 1", reply or "")
        self.assertIn("import 2", reply or "")
        self.assertIn("success:1", reply or "")
        self.assertIn("failed:1", reply or "")
        self.assertIn("status-export 1(recorded, bridge-failed:1", reply or "")
        self.assertIn("readiness native:1,runtime-state:1,fallback:1,disabled:1", reply or "")
        self.assertIn("channels native actor-import", reply or "")
        self.assertIn("runtime-state actor-delete", reply or "")
        self.assertIn("disabled environment-import", reply or "")
        self.assertIn("cpp-actor-import-failed:1", reply or "")
        self.assertNotIn("conflict_items", reply or "")
        self.assertNotIn("hidden raw conflict", reply or "")
        self.assertNotIn("provider", reply or "")
        self.assertNotIn("prompt", reply or "")
        self.assertNotIn("https://example.invalid", reply or "")
        room = worker._agent_runtime.query_state("room-runtime-replay")["room"]
        self.assertEqual(room["scene_plans"], {})
        self.assertEqual(room["active_plan_id"], "")
        self.assertEqual(coordinator.ingest_calls, [])
        self.assertIn("runtime_operation_replay_queried", worker._agent_runtime.operation_log.events())

    def test_runtime_operation_replay_reports_engine_write_readiness_mismatch(self) -> None:
        coordinator = _FakeCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._agent_runtime.operation_log.append(
            "tool_call_succeeded",
            room_id="room-runtime-replay-mismatch",
            payload={
                "transform_results": [
                    {"actor_id": "actor-chair", "actor_name": "??", "status": "success"}
                ],
            },
        )
        worker._agent_runtime.operation_log.append(
            "runtime_engine_write_status_exported",
            room_id="room-runtime-replay-mismatch",
            payload={
                "recorded": True,
                "engine_write_readiness_native_enabled_count": 1,
                "engine_write_readiness_native_enabled_channels": ["actor_import"],
                "engine_write_readiness_runtime_state_only_count": 1,
                "engine_write_readiness_runtime_state_only_channels": ["layout_transform"],
            },
        )
        worker._agent_runtime.operation_log.append(
            "runtime_event_emitted",
            room_id="room-runtime-replay-mismatch",
            payload={
                "event_type": "report_ready",
                "report_health_status": "needs_attention",
                "report_attention_required": True,
                "report_health_reasons": ["engine_write_readiness_mismatch"],
                "engine_write_readiness_mismatch_count": 1,
                "engine_write_readiness_mismatch_channels": ["layout_transform"],
            },
        )

        reply = worker._handle_agent_runtime_operation_replay_query({
            "room_id": "room-runtime-replay-mismatch",
            "message_id": "msg-runtime-replay-mismatch",
            "text": "@GM runtime operation replay",
            "sender_id": "host-1",
            "sender_name": "host",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("engine_write:", reply or "")
        self.assertIn("transform 1", reply or "")
        self.assertIn("readiness-mismatch 1(layout-transform)", reply or "")
        self.assertIn("channels native actor-import", reply or "")
        replay = worker._agent_runtime.operation_replay(room_id="room-runtime-replay-mismatch")
        report_health = replay["report_health_summary"]
        self.assertEqual(report_health["status"], "needs_attention")
        self.assertTrue(report_health["attention_required"])
        self.assertEqual(report_health["engine_write_readiness_mismatch_count"], 1)
        self.assertEqual(report_health["engine_write_readiness_mismatch_channels"], ["layout-transform"])
        self.assertIn("engine_write_readiness_mismatch", report_health["reasons"])
        latest_report_ready = replay["runtime_event_replay_summary"]["latest_report_ready"]
        self.assertEqual(latest_report_ready["status"], "needs_attention")
        self.assertEqual(latest_report_ready["engine_write_readiness_mismatch_count"], 1)
        self.assertEqual(latest_report_ready["engine_write_readiness_mismatch_channels"], ["layout-transform"])
        self.assertIn(
            "engine_write_readiness_mismatch",
            replay["runtime_event_replay_summary"]["report_health_reason_counts"],
        )
        self.assertNotIn("provider", reply or "")
        self.assertNotIn("prompt", reply or "")
        self.assertEqual(coordinator.ingest_calls, [])

    def test_runtime_operation_replay_query_filters_to_active_runtime_plan(self) -> None:
        coordinator = _ExplodingCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        created = worker._agent_runtime.handle_message(
            room_id="room-runtime-replay-plan",
            text="做一个强盗藏宝室，包含宝箱和金币",
            sender_id="host-1",
            sender_name="host",
            action="plan",
            external_plan_id="seed-runtime-replay-plan",
        )
        worker._agent_runtime.operation_log.append(
            "agent_reply_send_failed",
            room_id="room-runtime-replay-plan",
            plan_id=created["plan"]["plan_id"],
            payload={
                "message_kind": "agent_reply",
                "channel": "network_send_agent_reply_ex",
                "sent": False,
                "stage": "资源调度",
                "progress": 20,
                "source_user_id": "host-secret",
                "provider": "hidden",
                "prompt": "hidden",
                "url": "https://example.invalid/private",
            },
        )
        worker._agent_runtime.operation_log.append(
            "custom_room_event",
            room_id="room-runtime-replay-plan",
            message="room-only event",
            payload={"provider": "hidden", "prompt": "hidden"},
        )

        reply = worker._handle_agent_runtime_operation_replay_query({
            "room_id": "room-runtime-replay-plan",
            "message_id": "msg-runtime-replay-plan",
            "text": "@GM runtime operation replay",
            "sender_id": "host-1",
            "sender_name": "host",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("external_plan_context_synced", reply or "")
        self.assertIn("context:", reply or "")
        self.assertIn("1 context", reply or "")
        self.assertIn("plan-context:1", reply or "")
        self.assertIn("user:1", reply or "")
        self.assertIn("plan_lifecycle:", reply or "")
        self.assertIn("created 1", reply or "")
        self.assertIn("latest scene-plan-created", reply or "")
        self.assertIn("message_delivery:", reply or "")
        self.assertIn("agent_reply", reply or "")
        self.assertIn("network_send_agent_reply_ex", reply or "")
        self.assertNotIn("custom_room_event", reply or "")
        self.assertNotIn("provider", reply or "")
        self.assertNotIn("prompt", reply or "")
        self.assertNotIn("host-secret", reply or "")
        self.assertNotIn("https://example.invalid", reply or "")

    def test_runtime_operation_replay_query_uses_metadata_batch_scope(self) -> None:
        coordinator = _ExplodingCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        plan = worker._agent_runtime.propose_scene_plan(
            room_id="room-runtime-replay-batch",
            text="做一个可爱卧室，有床、书桌、衣柜、台灯、地毯",
            owner_agent="小女孩",
        )
        worker._agent_runtime.confirm_scene_plan(plan.plan_id, confirmed_by="房主")
        result = worker._agent_runtime.execute_planned_batches(plan.plan_id, max_items_per_batch=2)
        first_batch_id = result["batches"][0]["batch_id"]
        second_batch_id = result["batches"][1]["batch_id"]
        for event_type in ("image_resources_ready", "model_resources_ready", "actors_imported"):
            worker._agent_runtime.operation_log.append(
                "runtime_event_emitted",
                room_id="room-runtime-replay-batch",
                batch_id=first_batch_id,
                message=event_type,
                payload={"event_type": event_type},
            )
        worker._agent_runtime.operation_log.append(
            "runtime_pause_command_applied",
            room_id="room-runtime-replay-batch",
            batch_id=first_batch_id,
            message="pause applied",
            payload={
                "command": "pause",
                "old_status": "executing",
                "new_status": "paused",
                "cancelled_batches": 1,
                "cancelled_graphs": 2,
            },
        )
        worker._agent_runtime.operation_log.append(
            "tool_graph_queued",
            room_id="room-runtime-replay-batch",
            batch_id=first_batch_id,
            message="queued",
            payload={"status": "queued"},
        )
        worker._agent_runtime.operation_log.append(
            "tool_call_blocked",
            room_id="room-runtime-replay-batch",
            batch_id=first_batch_id,
            message="write tool call requires confirmed plan",
            payload={"risk_level": "medium", "requires_write": True, "confirmed": False},
        )
        worker._agent_runtime.operation_log.append(
            "tool_call_retry_scheduled",
            room_id="room-runtime-replay-batch",
            batch_id=first_batch_id,
            message="retry scheduled",
            payload={"status": "scheduled", "error_code": "transient"},
        )
        worker._agent_runtime.operation_log.append(
            "pending_interventions_routed_via_tool_graph",
            room_id="room-runtime-replay-batch",
            batch_id=first_batch_id,
            message="routed",
            payload={
                "absorbable_count": 2,
                "non_absorbable_count": 1,
                "requested_item_count": 3,
            },
        )
        worker._agent_runtime.operation_log.append(
            "batch_interventions_merged_via_tool_graph",
            room_id="room-runtime-replay-batch",
            batch_id=first_batch_id,
            message="merged",
            payload={
                "item_count": 2,
                "absorbed_count": 2,
            },
        )
        worker._agent_runtime.operation_log.append(
            "pending_intervention_batch_state_persisted",
            room_id="room-runtime-replay-batch",
            batch_id=first_batch_id,
            message="persisted",
            payload={"status_counts": {"accepted": 2}, "batch_count": 2},
        )
        worker._agent_runtime.operation_log.append(
            "pending_intervention_batch_queued",
            room_id="room-runtime-replay-batch",
            batch_id=first_batch_id,
            message="queued",
            payload={"status": "queued", "requested_items": ["angel", "dog"]},
        )
        worker._agent_runtime.operation_log.append(
            "runtime_state_patch_applied",
            room_id="room-runtime-replay-batch",
            batch_id=first_batch_id,
            message="geometry facts applied",
            payload={
                "change_key_count": 1,
                "operation_count": 1,
                "applied_version": 9,
                "geometry_fact_patch_summary": {
                    "fact_count": 2,
                    "aabb_actor_count": 3,
                    "aabb_skipped_count": 1,
                    "overlap_issue_count": 1,
                    "status_counts": {"recorded": 1, "needs_adjustment": 1},
                    "fact_type_counts": {
                        "runtime_geometry_aabb": 1,
                        "runtime_geometry_overlap": 1,
                    },
                    "latest_fact": {
                        "fact_type": "runtime_geometry_overlap",
                        "status": "needs_adjustment",
                        "actor_count": 2,
                        "issue_count": 1,
                        "skipped_count": 0,
                    },
                },
            },
        )
        worker._agent_runtime.operation_log.append(
            "layout_adjustment_requested",
            room_id="room-runtime-replay-batch",
            batch_id=first_batch_id,
            message="layout proposal",
            payload={"proposal_id": "layout-test", "delta_count": 3},
        )
        worker._agent_runtime.operation_log.append(
            "layout_adjustment_confirmed",
            room_id="room-runtime-replay-batch",
            batch_id=first_batch_id,
            message="layout confirmed",
            payload={
                "proposal_id": "layout-test",
                "graph_status": "completed",
                "applied_count": 3,
                "transform_success_count": 2,
                "transform_failed_count": 1,
                "ground_snapped_count": 2,
                "overlap_resolved_count": 1,
            },
        )
        worker._agent_runtime.operation_log.append(
            "final_adjustment_confirmation_recorded",
            room_id="room-runtime-replay-batch",
            batch_id=first_batch_id,
            message="confirmed",
            payload={
                "proposal_id": "final-first-batch",
                "batch_id": first_batch_id,
                "decision": "confirmed",
                "conflict_item_count": 1,
            },
        )
        worker._agent_runtime.operation_log.append(
            "final_adjustment_confirmation_recorded",
            room_id="room-runtime-replay-batch",
            batch_id=second_batch_id,
            message="confirmed",
            payload={
                "proposal_id": "final-second-batch",
                "batch_id": second_batch_id,
                "decision": "confirmed",
                "conflict_item_count": 1,
            },
        )
        worker._agent_runtime.operation_log.append(
            "tool_call_succeeded",
            room_id="room-runtime-replay-batch",
            batch_id=first_batch_id,
            message="vlm checkpoint",
            payload={
                "source": "runtime_vlm_checkpoint",
                "status": "advisory",
                "checkpoint_type": "final_consistency_review",
                "advisory_item_count": 2,
                "reviewed_targets": ["入口", "主街"],
            },
        )
        for event_type in ("environment_components_ready", "environment_components_imported"):
            worker._agent_runtime.operation_log.append(
                "runtime_event_emitted",
                room_id="room-runtime-replay-batch",
                batch_id=first_batch_id,
                message=event_type,
                payload={"event_type": event_type},
            )
        worker._agent_runtime.operation_log.append(
            "runtime_provider_readiness_published",
            room_id="room-runtime-replay-batch",
            batch_id=first_batch_id,
            message="provider readiness",
            payload={
                "readiness_requested_count": 2,
                "readiness_enabled_count": 1,
                "readiness_unavailable_count": 1,
                "readiness_status_counts": {
                    "enabled": 1,
                    "disabled": 1,
                    "provider_url_hidden": 1,
                },
            },
        )
        worker._agent_runtime.operation_log.append(
            "runtime_provider_status_queried",
            room_id="room-runtime-replay-batch",
            batch_id=first_batch_id,
            message="provider status",
            payload={
                "recorded": True,
                "readiness_requested_count": 2,
                "readiness_enabled_count": 1,
                "readiness_unavailable_count": 1,
                "readiness_status_counts": {
                    "enabled": 1,
                    "disabled": 1,
                    "provider_url_hidden": 1,
                },
            },
        )
        worker._agent_runtime.operation_log.append(
            "runtime_event_emitted",
            room_id="room-runtime-replay-batch",
            batch_id=first_batch_id,
            message="provider_readiness",
            payload={"event_type": "provider_readiness", "payload": {"status": "ready"}, "level": "info"},
        )
        worker._agent_runtime.operation_log.append(
            "sync_event_recorded",
            room_id="room-runtime-replay-batch",
            batch_id=first_batch_id,
            message="actor_transform",
            payload={
                "event_type": "actor_transform",
                "actor_id": "actor-secret",
                "peer_id": "peer-secret",
            },
        )
        worker._agent_runtime.operation_log.append(
            "sync_event_recorded",
            room_id="room-runtime-replay-batch",
            batch_id=first_batch_id,
            message="asset_transfer_progress",
            payload={
                "event_type": "asset_transfer_progress",
                "asset_id": "asset-secret",
                "peer_id": "peer-secret",
                "asset_path": "C:/secret/model.glb",
                "transfer_status": "transferring",
                "progress": 50,
                "chunk_index": 2,
                "chunk_count": 4,
                "bytes_transferred": 512,
                "total_bytes": 1024,
            },
        )
        worker._agent_runtime.operation_log.append(
            "sync_event_recorded",
            room_id="room-runtime-replay-batch",
            batch_id=first_batch_id,
            message="peer_asset_ready",
            payload={
                "event_type": "peer_asset_ready",
                "asset_id": "asset-secret",
                "peer_id": "peer-secret",
                "transfer_status": "completed",
            },
        )
        worker._agent_runtime.operation_log.append(
            "sync_event_recorded",
            room_id="room-runtime-replay-batch",
            batch_id=first_batch_id,
            message="room_joined",
            payload={"event_type": "room_joined", "peer_id": "peer-secret", "room_status": "joined"},
        )
        worker._agent_runtime.operation_log.append(
            "sync_event_recorded",
            room_id="room-runtime-replay-batch",
            batch_id=first_batch_id,
            message="sync_reconcile_completed",
            payload={
                "event_type": "sync_reconcile_completed",
                "peer_id": "peer-secret",
                "status": "completed",
            },
        )
        worker._agent_runtime.operation_log.append(
            "batch_marker_first",
            room_id="room-runtime-replay-batch",
            batch_id=first_batch_id,
            message="first batch marker",
        )
        worker._agent_runtime.operation_log.append(
            "batch_marker_second",
            room_id="room-runtime-replay-batch",
            batch_id=second_batch_id,
            message="second batch marker",
        )

        reply = worker._handle_agent_runtime_operation_replay_query({
            "room_id": "room-runtime-replay-batch",
            "message_id": "msg-runtime-replay-batch",
            "text": "@GM runtime operation replay",
            "metadata": {"runtime_batch_id": first_batch_id},
            "sender_id": "host-1",
            "sender_name": "host",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("batch_marker_first", reply or "")
        self.assertNotIn("batch_marker_second", reply or "")
        self.assertIn("batch_resources:", reply or "")
        self.assertIn("image 1/0", reply or "")
        self.assertIn("model 1/0", reply or "")
        self.assertIn("import 1/0", reply or "")
        self.assertIn("commands: 1 command(s)", reply or "")
        self.assertIn("cancelled batch/graph 1/2", reply or "")
        self.assertIn("latest pause:executing->paused", reply or "")
        self.assertIn("tools:", reply or "")
        self.assertIn("started 2", reply or "")
        self.assertIn("succeeded 3", reply or "")
        self.assertIn("queue:", reply or "")
        self.assertIn("queued 1", reply or "")
        self.assertIn("state_patch:", reply or "")
        self.assertIn("applied 3", reply or "")
        self.assertIn("guard:", reply or "")
        self.assertIn("blocked 1", reply or "")
        self.assertIn("write-confirm 1", reply or "")
        self.assertIn("write-blocked 1", reply or "")
        self.assertIn("unconfirmed 1", reply or "")
        self.assertIn("risk medium:1", reply or "")
        self.assertIn("latest write-confirmation-required risk:medium/write/unconfirmed", reply or "")
        self.assertIn("plan_lifecycle:", reply or "")
        self.assertIn("created 0", reply or "")
        self.assertIn("confirmed 0", reply or "")
        self.assertIn("interventions:", reply or "")
        self.assertIn("routed 1", reply or "")
        self.assertIn("queued 1", reply or "")
        self.assertIn("absorbed 2", reply or "")
        self.assertIn("route 2/1 items 3", reply or "")
        self.assertIn("merge 1 items 2 absorbed 2", reply or "")
        self.assertIn("geometry:", reply or "")
        self.assertIn("patches 1", reply or "")
        self.assertIn("facts 2", reply or "")
        self.assertIn("aabb 3/1", reply or "")
        self.assertIn("overlap 1", reply or "")
        self.assertIn("latest runtime-geometry-overlap:needs-adjustment", reply or "")
        self.assertIn("runtime_events:", reply or "")
        self.assertIn("emitted 8", reply or "")
        self.assertIn("types actors-imported:1", reply or "")
        self.assertIn("failure_strategy:", reply or "")
        self.assertIn("retry 1", reply or "")
        self.assertIn("latest retry-scheduled:scheduled", reply or "")
        self.assertIn("layout:", reply or "")
        self.assertIn("requests 1/0", reply or "")
        self.assertIn("confirmations 1/0", reply or "")
        self.assertIn("ground 2", reply or "")
        self.assertIn("final_adjustment:", reply or "")
        self.assertIn("final-first-batch", reply or "")
        self.assertIn("decisions confirmed:1", reply or "")
        self.assertNotIn("final-second-batch", reply or "")
        self.assertIn("vlm:", reply or "")
        self.assertIn("checkpoints 1", reply or "")
        self.assertIn("advisory 2", reply or "")
        self.assertIn("latest final-consistency-review:advisory", reply or "")
        self.assertIn("environment:", reply or "")
        self.assertIn("ready 1/0", reply or "")
        self.assertIn("import 1/0", reply or "")
        self.assertIn("resource_readiness:", reply or "")
        self.assertIn("published 1/0", reply or "")
        self.assertIn("publish-ready requested/enabled/unavailable 2/1/1", reply or "")
        self.assertIn("publish-status disabled:1,enabled:1,resource-resource-hidden:1", reply or "")
        self.assertIn("query-ready requested/enabled/unavailable 2/1/1", reply or "")
        self.assertIn("query-status disabled:1,enabled:1,resource-resource-hidden:1", reply or "")
        self.assertIn("latest ready", reply or "")
        self.assertIn("sync:", reply or "")
        self.assertIn("recorded 5", reply or "")
        self.assertIn("actor-transform 1", reply or "")
        self.assertIn("transfer-progress 1", reply or "")
        self.assertIn("asset_transfer:", reply or "")
        self.assertIn("events 2", reply or "")
        self.assertIn("progress 1", reply or "")
        self.assertIn("completed 1", reply or "")
        self.assertIn("peer-ready 1", reply or "")
        self.assertIn("latest 50% chunk 2/4 512B/1KB completed", reply or "")
        self.assertIn("peer_sync:", reply or "")
        self.assertIn("join 1", reply or "")
        self.assertIn("reconcile 1/0", reply or "")
        self.assertNotIn("provider", reply or "")
        self.assertNotIn("prompt", reply or "")
        self.assertNotIn("peer-secret", reply or "")
        self.assertNotIn("asset-secret", reply or "")
        self.assertNotIn("C:/secret/model.glb", reply or "")
        replay_export = worker._agent_runtime.operation_log.query(
            event="runtime_operation_replay_queried",
            room_id="room-runtime-replay-batch",
        )[-1]
        self.assertEqual(replay_export.batch_id, first_batch_id)

    def test_runtime_report_query_generates_safe_summary_without_coordinator_ingest(self) -> None:
        coordinator = _ExplodingCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        created = worker._agent_runtime.handle_message(
            room_id="room-runtime-report",
            text="做一个森林营地，有天空、草地、森林、帐篷和小木桌",
            sender_id="host-1",
            sender_name="host",
            action="plan",
            external_plan_id="seed-runtime-report",
        )
        worker.handle_lanchat_sync_event({
            "event": "actor_created",
            "room_id": "room-runtime-report",
            "actor_guid": "actor-camp-table",
            "actor_name": "小木桌",
            "asset_path": "E:/secret/internal/table.glb",
            "message_id": "hidden-sync-message",
        })
        worker.handle_lanchat_sync_event({
            "event": "file_chunk_received",
            "room_id": "room-runtime-report",
            "asset_id": "asset-camp-table",
            "peer_id": "peer-report-secret",
            "chunk_index": 2,
            "chunk_count": 4,
            "bytes_transferred": 2048,
            "total_bytes": 4096,
            "progress": 50,
            "asset_path": "E:/secret/internal/table.glb",
            "message_id": "hidden-asset-message",
        })
        worker.handle_lanchat_sync_event({
            "event": "peer_asset_ready",
            "room_id": "room-runtime-report",
            "asset_id": "asset-camp-table",
            "peer_id": "peer-report-secret",
            "asset_path": "E:/secret/internal/table.glb",
        })
        worker.handle_lanchat_sync_event({
            "event": "room_joined",
            "room_id": "room-runtime-report",
            "peer_id": "peer-report-secret",
            "message_id": "hidden-peer-message",
        })
        worker._agent_runtime.operation_log.append(
            "sync_event_recorded",
            room_id="room-runtime-report",
            plan_id=created["plan"]["plan_id"],
            message="sync_reconcile_completed",
            payload={
                "event_type": "sync_reconcile_completed",
                "peer_id": "peer-report-secret",
                "status": "completed",
            },
        )
        worker._agent_runtime.operation_log.append(
            "sync_event_record_failed",
            room_id="room-runtime-report",
            plan_id=created["plan"]["plan_id"],
            message="sync_event_record_failed",
            payload={
                "event_type": "actor_transform",
                "failure_code": "sync_event_record_failed",
                "provider": "hidden-provider",
                "url": "https://example.invalid/sync",
            },
        )
        worker._agent_runtime.state.apply_patch(
            StatePatch(
                room_id="room-runtime-report",
                changes={
                    "geometry_reviews": {
                        created["plan"]["plan_id"]: {
                            "status": "needs_adjustment",
                            "issues": [
                                {
                                    "name": "小木桌",
                                    "type": "out_of_bounds",
                                    "current_position": [9.0, 0.4, 0.0],
                                    "bounds": [-4.0, 4.0, -4.0, 4.0],
                                    "prompt": "hidden layout prompt",
                                }
                            ],
                        }
                    }
                },
                expected_version=worker._agent_runtime.state.version,
            )
        )
        worker._agent_runtime.state.apply_patch(
            StatePatch(
                room_id="room-runtime-report",
                changes={
                    "custom_geometry_facts": {
                        f"{created['plan']['plan_id']}:aabb": {
                            "fact_type": "runtime_geometry_aabb",
                            "plan_id": created["plan"]["plan_id"],
                            "batch_id": "",
                            "status": "recorded",
                            "actor_count": 2,
                            "skipped_count": 1,
                        },
                        f"{created['plan']['plan_id']}:overlap": {
                            "fact_type": "runtime_geometry_overlap",
                            "plan_id": created["plan"]["plan_id"],
                            "batch_id": "",
                            "status": "needs_adjustment",
                            "issue_count": 1,
                        },
                    }
                },
                expected_version=worker._agent_runtime.state.version,
            )
        )
        worker._agent_runtime.propose_layout_adjustment(
            room_id="room-runtime-report",
            plan_id=created["plan"]["plan_id"],
        )
        worker._agent_runtime.operation_log.append(
            "runtime_system_event_send_succeeded",
            room_id="room-runtime-report",
            plan_id=created["plan"]["plan_id"],
            payload={
                "message_kind": "runtime_status",
                "channel": "network_send_system_message_ex",
                "sent": True,
                "stage": "导入",
                "progress": 80,
                "source_user_id": "host-secret",
                "provider": "hidden",
                "prompt": "hidden",
                "url": "https://example.invalid/private",
            },
        )
        worker._agent_runtime.operation_log.append(
            "agent_reply_send_failed",
            room_id="room-runtime-report",
            plan_id=created["plan"]["plan_id"],
            payload={
                "message_kind": "agent_reply",
                "channel": "network_send_agent_reply_ex",
                "sent": False,
                "stage": "resource",
                "progress": 30,
                "failure_code": "message_delivery_failed",
                "url": "https://example.invalid/private",
            },
        )
        worker._agent_runtime.operation_log.append(
            "tool_call_succeeded",
            room_id="room-runtime-report",
            plan_id=created["plan"]["plan_id"],
            payload={
                "transform_results": [
                    {"actor_id": "actor-camp-table", "actor_name": "小木桌", "status": "success", "observed_position": True},
                    {
                        "actor_id": "actor-hidden",
                        "actor_name": "隐藏物体",
                        "status": "failed",
                        "reason": "provider raw https://example.invalid/native",
                    },
                ],
            },
        )
        worker._agent_runtime.operation_log.append(
            "tool_call_succeeded",
            room_id="room-runtime-report",
            plan_id=created["plan"]["plan_id"],
            batch_id="batch-report-vlm",
            payload={
                "source": "runtime_vlm_checkpoint",
                "status": "advisory",
                "checkpoint_type": "final_consistency_review",
                "advisory_item_count": 2,
                "reviewed_targets": ["入口", "主街"],
            },
        )
        worker._agent_runtime.operation_log.append(
            "review_advisory_proposal_created",
            room_id="room-runtime-report",
            plan_id=created["plan"]["plan_id"],
            batch_id="batch-report-vlm",
            payload={"proposal_id": "review-vlm-report", "item_count": 2},
        )
        worker._agent_runtime.apply_runtime_command(
            room_id="room-runtime-report",
            command="pause",
            source_user="host-1",
            reason="用户请求暂停，prompt/provider/url 不应泄露",
        )
        worker._agent_runtime.operation_log.append(
            "runtime_system_event_disclosure_skipped",
            room_id="room-runtime-report",
            plan_id=created["plan"]["plan_id"],
            batch_id="batch-report-runtime-event",
            payload={
                "runtime_event_type": "agent_internal",
                "runtime_audience": "agent",
                "reason": "audience_not_user_visible",
                "provider": "secret-provider",
                "prompt": "secret-prompt",
            },
        )
        worker._agent_runtime.operation_log.append(
            "runtime_worker_drain_failed",
            room_id="room-runtime-report",
            plan_id=created["plan"]["plan_id"],
            payload={
                "drained_count": 0,
                "reason": "synthetic queue drain failure",
                "provider": "hidden-provider",
                "url": "https://example.invalid/drain",
            },
        )

        reply = worker._handle_agent_runtime_report_query({
            "room_id": "room-runtime-report",
            "message_id": "msg-runtime-report",
            "text": "@GM runtime report",
            "sender_id": "host-1",
            "sender_name": "host",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("Runtime Report", reply or "")
        self.assertIn("objects:", reply or "")
        self.assertIn("classification:", reply or "")
        self.assertIn("model/substrate", reply or "")
        self.assertIn("models:", reply or "")
        self.assertIn("substrate:", reply or "")
        self.assertIn("scene registry:", reply or "")
        self.assertIn("entities", reply or "")
        self.assertIn("actor", reply or "")
        self.assertIn("actor import:", reply or "")
        self.assertIn("registered actor", reply or "")
        self.assertIn("scene contract:", reply or "")
        self.assertIn("outdoor-scene", reply or "")
        self.assertIn("outdoor-ground", reply or "")
        self.assertIn("closure:", reply or "")
        self.assertIn("patch applied/conflict/invalid", reply or "")
        self.assertIn("semantic arbitration:", reply or "")
        self.assertIn("ready-for-next-runtime-step", reply or "")
        self.assertIn("scene snapshot:", reply or "")
        self.assertIn("snapshots", reply or "")
        self.assertIn("runtime resources:", reply or "")
        self.assertIn("image", reply or "")
        self.assertIn("model", reply or "")
        self.assertIn("report health:", reply or "")
        self.assertIn("needs-attention", reply or "")
        self.assertIn("worker-drain", reply or "")
        self.assertIn("import:", reply or "")
        self.assertIn("imported", reply or "")
        self.assertIn("environment:", reply or "")
        self.assertIn("review:", reply or "")
        self.assertIn("geometry facts:", reply or "")
        self.assertIn("AABB actors 2", reply or "")
        self.assertIn("overlap issues 1", reply or "")
        self.assertIn("skipped 1", reply or "")
        self.assertIn("layout:", reply or "")
        self.assertIn("proposal", reply or "")
        self.assertIn("deltas", reply or "")
        self.assertIn("commands:", reply or "")
        self.assertIn("pause", reply or "")
        self.assertIn("帐篷", reply or "")
        self.assertTrue(("天空" in (reply or "")) or ("草地" in (reply or "")) or ("森林" in (reply or "")))
        self.assertIn("sync:", reply or "")
        self.assertIn("小木桌", reply or "")
        self.assertIn("asset transfer:", reply or "")
        self.assertIn("asset-camp-table", reply or "")
        self.assertIn("progress 50%", reply or "")
        self.assertIn("sync replay:", reply or "")
        self.assertIn("transfer-progress", reply or "")
        self.assertIn("failure codes sync-event-record-failed:1", reply or "")
        self.assertIn("latest failure sync-event-record-failed", reply or "")
        self.assertIn("asset transfer replay:", reply or "")
        self.assertIn("peer-ready 1", reply or "")
        self.assertIn("worker drain replay:", reply or "")
        self.assertIn("failed 1", reply or "")
        self.assertIn("peer sync replay:", reply or "")
        self.assertIn("join 1", reply or "")
        self.assertIn("reconcile 1/0", reply or "")
        self.assertIn("batch tooling:", reply or "")
        self.assertIn("created-batches", reply or "")
        self.assertIn("state patch:", reply or "")
        self.assertIn("failure strategy:", reply or "")
        self.assertIn("guard:", reply or "")
        self.assertIn("blocked", reply or "")
        self.assertIn("plan lifecycle:", reply or "")
        self.assertIn("created", reply or "")
        self.assertIn("confirmed", reply or "")
        self.assertIn("vlm replay:", reply or "")
        self.assertIn("checkpoints 1", reply or "")
        self.assertIn("advisory 2", reply or "")
        self.assertIn("review advisory replay:", reply or "")
        self.assertIn("proposals 1", reply or "")
        self.assertIn("tool execution:", reply or "")
        self.assertIn("graphs", reply or "")
        self.assertIn("runtime queue:", reply or "")
        self.assertIn("queue", reply or "")
        self.assertIn("pressure", reply or "")
        self.assertIn("resources:", reply or "")
        self.assertIn("image-resource", reply or "")
        self.assertIn("environment-component", reply or "")
        self.assertIn("resource readiness:", reply or "")
        self.assertIn("channels", reply or "")
        self.assertIn("engine write readiness:", reply or "")
        self.assertIn("runtime-state", reply or "")
        self.assertIn("fallback", reply or "")
        self.assertIn("disabled", reply or "")
        self.assertIn("message delivery:", reply or "")
        self.assertIn("failure codes message-delivery-failed:1", reply or "")
        self.assertIn("latest failure message-delivery-failed", reply or "")
        self.assertIn("runtime_status", reply or "")
        self.assertIn("runtime_status", reply or "")
        self.assertIn("network_send_system_message_ex", reply or "")
        self.assertIn("engine write:", reply or "")
        self.assertIn("transform 2", reply or "")
        self.assertIn("success:1", reply or "")
        self.assertIn("failed:1", reply or "")
        self.assertIn("context:", reply or "")
        self.assertIn("1 context", reply or "")
        self.assertIn("plan-context:1", reply or "")
        self.assertIn("replay:", reply or "")
        self.assertIn("entries", reply or "")
        self.assertIn("runtime-events", reply or "")
        self.assertIn("skipped 1", reply or "")
        self.assertIn("latest-skip agent-internal:agent", reply or "")
        self.assertIn("scene-plan-created", reply or "")
        self.assertIn("interventions:", reply or "")
        self.assertNotIn("provider", reply or "")
        self.assertNotIn("prompt", reply or "")
        self.assertNotIn("url", reply or "")
        self.assertNotIn("hidden layout prompt", reply or "")
        self.assertNotIn("tool_name", reply or "")
        self.assertNotIn("E:/secret", reply or "")
        self.assertNotIn("hidden-sync-message", reply or "")
        self.assertNotIn("hidden-asset-message", reply or "")
        self.assertNotIn("hidden-peer-message", reply or "")
        self.assertNotIn("peer-report-secret", reply or "")
        self.assertNotIn("source_user_id", reply or "")
        self.assertNotIn("host-secret", reply or "")
        status_reply = worker._agent_runtime_status_reply(
            room_id="room-runtime-report",
            external_plan_id="seed-runtime-report",
        )
        self.assertIn("几何事实：", status_reply)
        self.assertIn("场景实体：", status_reply)
        self.assertIn("entities", status_reply)
        self.assertIn("actor", status_reply)
        self.assertIn("写入边界：", status_reply)
        self.assertIn("报告健康：", status_reply)
        self.assertIn("Engine write readiness:", status_reply)
        self.assertIn("Worker drain replay:", status_reply)
        self.assertIn("failed 1", status_reply)
        self.assertIn("runtime-state", status_reply)
        self.assertIn("fallback", status_reply)
        self.assertIn("disabled", status_reply)
        self.assertIn("AABB actors 2", status_reply)
        self.assertIn("overlap issues 1", status_reply)
        room = worker._agent_runtime.query_state("room-runtime-report")["room"]
        self.assertEqual(room["active_plan_id"], created["plan"]["plan_id"])
        self.assertEqual(len(room["scene_plans"]), 1)
        self.assertEqual(len(room["reports"]), 1)
        self.assertEqual(coordinator.seed_plan_calls, [])
        self.assertEqual(coordinator.action_payload_calls, [])
        self.assertEqual(coordinator.ingest_calls, [])
        self.assertIn("user_report_generated", worker._agent_runtime.operation_log.events())

    def test_world_consistency_disclosure_is_count_only(self) -> None:
        consistent = LANChatAgentWorker._format_agent_runtime_scene_world_consistency_report({
            "status": "consistent",
            "expected_entity_count": 4,
            "engine_actor_count": 4,
            "matched_entity_count": 4,
            "issue_count": 0,
            "missing_in_engine_entity_ids": ["entity-secret"],
        })
        needs_review = LANChatAgentWorker._format_agent_runtime_scene_world_consistency_report({
            "status": "needs_review",
            "expected_entity_count": 4,
            "engine_actor_count": 3,
            "matched_entity_count": 3,
            "issue_count": 1,
            "missing_in_engine_entity_ids": ["entity-secret"],
        })

        self.assertIn("对账通过", consistent)
        self.assertIn("匹配 4/4", consistent)
        self.assertIn("需要复核", needs_review)
        self.assertIn("问题 1 项", needs_review)
        self.assertNotIn("entity-secret", consistent)
        self.assertNotIn("entity-secret", needs_review)

    def test_world_consistency_disclosure_reaches_report_and_status(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        audit = {
            "status": "needs_review",
            "expected_entity_count": 4,
            "engine_actor_count": 3,
            "matched_entity_count": 3,
            "issue_count": 1,
            "missing_in_engine_entity_ids": ["entity-secret"],
        }
        with patch.object(worker._agent_runtime, "handle_message", return_value={
            "recorded": True,
            "report": {
                "plan_id": "plan-report-audit",
                "scene_world_consistency_audit": audit,
            },
        }):
            report_reply = worker._handle_agent_runtime_report_query({
                "room_id": "room-report-audit",
                "message_id": "msg-report-audit",
                "text": "@GM runtime report",
                "message_kind": "chat",
            })
        with patch.object(worker._agent_runtime, "handle_message", return_value={
            "status": {
                "available": True,
                "plan_id": "plan-status-audit",
                "scene_world_consistency_audit": audit,
            },
        }):
            status_reply = worker._agent_runtime_status_reply(room_id="room-status-audit")

        self.assertIn("world consistency: 需要复核", report_reply or "")
        self.assertIn("场景事实对账：需要复核", status_reply)
        self.assertIn("问题 1 项", report_reply or "")
        self.assertIn("问题 1 项", status_reply)
        self.assertNotIn("entity-secret", report_reply or "")
        self.assertNotIn("entity-secret", status_reply)

    def test_runtime_report_query_uses_metadata_batch_scope(self) -> None:
        coordinator = _ExplodingCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        plan = worker._agent_runtime.propose_scene_plan(
            room_id="room-runtime-report-batch",
            text="做一个可爱卧室，有床、书桌、衣柜、台灯、地毯",
            owner_agent="小女孩",
        )
        worker._agent_runtime.confirm_scene_plan(plan.plan_id, confirmed_by="房主")
        result = worker._agent_runtime.execute_planned_batches(plan.plan_id, max_items_per_batch=2)
        first_batch_id = result["batches"][0]["batch_id"]

        reply = worker._handle_agent_runtime_report_query({
            "room_id": "room-runtime-report-batch",
            "message_id": "msg-runtime-report-batch",
            "text": "@GM runtime report",
            "metadata": {"runtime_batch_id": first_batch_id},
            "sender_id": "host-1",
            "sender_name": "host",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("[Runtime Report]", reply or "")
        self.assertIn("- batches: 1", reply or "")
        model_line = next(line for line in (reply or "").splitlines() if line.startswith("- models:"))
        self.assertIn("床", model_line)
        self.assertIn("书桌", model_line)
        self.assertNotIn("衣柜", model_line)

    def test_runtime_sync_status_query_exports_safe_summary_without_creating_plan(self) -> None:
        coordinator = _FakeCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker.handle_lanchat_sync_event({
            "event": "actor_created",
            "room_id": "room-runtime-sync",
            "actor_guid": "actor-001",
            "actor_name": "藏宝箱",
            "message_id": "hidden-message",
            "asset_path": "E:/hidden/chest.glb",
        })
        worker.handle_lanchat_sync_event({
            "event": "asset_transfer_completed",
            "room_id": "room-runtime-sync",
            "asset_id": "asset-001",
            "asset_path": "E:/hidden/chest.glb",
        })
        worker.handle_lanchat_sync_event({
            "event": "file_chunk_received",
            "room_id": "room-runtime-sync",
            "asset_id": "asset-progress",
            "progress": 50,
            "chunk_index": 3,
            "chunk_count": 8,
            "bytes_transferred": 1024,
            "total_bytes": 8192,
            "message_id": "hidden-progress-message",
            "asset_path": "E:/hidden/progress.glb",
        })
        worker._agent_runtime.operation_log.append(
            "runtime_system_event_send_failed",
            room_id="room-runtime-sync",
            payload={
                "message_kind": "runtime_status",
                "channel": "network_send_system_message_ex",
                "sent": False,
                "stage": "同步",
                "progress": 0,
                "source_user_id": "host-secret",
                "provider": "hidden",
                "prompt": "hidden",
                "url": "https://example.invalid/private",
            },
        )

        reply = worker._handle_agent_runtime_sync_status_query({
            "room_id": "room-runtime-sync",
            "message_id": "msg-runtime-sync",
            "text": "@GM runtime sync status",
            "sender_id": "host-1",
            "sender_name": "host",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("event_count: 3", reply or "")
        self.assertIn("actor_events: 1", reply or "")
        self.assertIn("asset_events: 2", reply or "")
        self.assertIn("message_delivery:", reply or "")
        self.assertIn("runtime_status", reply or "")
        self.assertIn("network_send_system_message_ex", reply or "")
        self.assertIn("sync_replay:", reply or "")
        self.assertIn("transfer-progress 1 latest 50% chunk 3/8 1KB/8KB", reply or "")
        self.assertIn("asset-001", reply or "")
        self.assertIn("asset-progress:transferring 50% chunk 3/8 1KB/8KB", reply or "")
        self.assertNotIn("{'asset_id'", reply or "")
        self.assertNotIn('{"asset_id"', reply or "")
        self.assertNotIn("asset_path", reply or "")
        self.assertNotIn("message_id", reply or "")
        self.assertNotIn("hidden-progress-message", reply or "")
        self.assertNotIn("source_user_id", reply or "")
        self.assertNotIn("host-secret", reply or "")
        self.assertNotIn("E:/hidden", reply or "")
        self.assertNotIn("provider", reply or "")
        self.assertNotIn("prompt", reply or "")
        self.assertNotIn("https://example.invalid", reply or "")
        room = worker._agent_runtime.query_state("room-runtime-sync")["room"]
        self.assertEqual(room["scene_plans"], {})
        self.assertEqual(room["active_plan_id"], "")
        self.assertEqual(coordinator.ingest_calls, [])
        sync_status_export = worker._agent_runtime.operation_log.query(
            event="runtime_sync_status_exported",
            room_id="room-runtime-sync",
        )[-1]
        self.assertEqual(sync_status_export.payload["transfer_progress_count"], 1)
        self.assertEqual(sync_status_export.payload["latest_transfer_progress"], 50)
        self.assertEqual(sync_status_export.payload["latest_chunk_index"], 3)
        self.assertEqual(sync_status_export.payload["latest_chunk_count"], 8)
        self.assertEqual(sync_status_export.payload["latest_bytes_transferred"], 1024)
        self.assertEqual(sync_status_export.payload["latest_total_bytes"], 8192)
        self.assertNotIn("asset_path", str(sync_status_export.payload))
        self.assertNotIn("message_id", str(sync_status_export.payload))

    def test_runtime_sync_status_query_uses_metadata_batch_scope(self) -> None:
        coordinator = _FakeCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        plan = worker._agent_runtime.propose_scene_plan(
            room_id="room-runtime-sync-batch",
            text="做一个可爱卧室，有床、书桌、衣柜、台灯、地毯",
            owner_agent="小女孩",
        )
        worker._agent_runtime.confirm_scene_plan(plan.plan_id, confirmed_by="房主")
        result = worker._agent_runtime.execute_planned_batches(plan.plan_id, max_items_per_batch=2)
        first_batch_id = result["batches"][0]["batch_id"]
        second_batch_id = result["batches"][1]["batch_id"]
        worker.handle_lanchat_sync_event({
            "event": "actor_created",
            "room_id": "room-runtime-sync-batch",
            "batch_id": first_batch_id,
            "actor_guid": "actor-first",
            "actor_name": "床",
        })
        worker.handle_lanchat_sync_event({
            "event": "actor_created",
            "room_id": "room-runtime-sync-batch",
            "batch_id": second_batch_id,
            "actor_guid": "actor-second",
            "actor_name": "衣柜",
        })

        reply = worker._handle_agent_runtime_sync_status_query({
            "room_id": "room-runtime-sync-batch",
            "message_id": "msg-runtime-sync-batch",
            "text": "@GM runtime sync status",
            "metadata": {"runtime_batch_id": first_batch_id},
            "sender_id": "host-1",
            "sender_name": "host",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("event_count: 1", reply or "")
        self.assertIn("actor_events: 1", reply or "")
        sync_status_export = worker._agent_runtime.operation_log.query(
            event="runtime_sync_status_exported",
            room_id="room-runtime-sync-batch",
        )[-1]
        self.assertEqual(sync_status_export.batch_id, first_batch_id)
        self.assertEqual(sync_status_export.payload["event_count"], 1)

    def test_status_query_prefers_agent_runtime_status_when_runtime_plan_exists(self) -> None:
        coordinator = _FakeCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._start_active_coordinator_generation(
            coordinator,
            room_id="room-runtime-status-query",
            host_id="房主",
        )
        worker.handle_lanchat_sync_event({
            "event": "file_chunk_received",
            "room_id": "room-runtime-status-query",
            "asset_id": "asset-status-progress",
            "progress": 40,
            "chunk_index": 2,
            "chunk_count": 5,
            "asset_path": "E:/secret/status-progress.glb",
            "message_id": "msg-status-progress",
        })

        reply = worker._handle_coordinator_status_query({
            "room_id": "room-runtime-status-query",
            "message_id": "msg-status",
            "text": "现在生成到哪里",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "is_host": True,
        })

        self.assertIn("asset-status-progress:transferring 40% chunk 2/5", reply or "")
        self.assertIn("场景快照", reply or "")
        self.assertIn("snapshots", reply or "")
        self.assertIn("事实来源", reply or "")
        self.assertIn("runtime", reply or "")
        self.assertIn("external", reply or "")
        self.assertIn("Runtime 资源", reply or "")
        self.assertIn("image", reply or "")
        self.assertIn("model", reply or "")
        self.assertIn("导入", reply or "")
        self.assertIn("imported", reply or "")
        self.assertIn("Engine write readiness:", reply or "")
        self.assertIn("runtime-state", reply or "")
        self.assertIn("fallback", reply or "")
        self.assertIn("disabled", reply or "")
        self.assertNotIn("status-progress.glb", reply or "")
        self.assertNotIn("msg-status-progress", reply or "")
        self.assertEqual(coordinator.ingest_calls, [])

    def test_agent_status_query_uses_runtime_before_coordinator_lookup(self) -> None:
        class _ForbiddenCoordinatorLookupWorker(_TestWorker):
            def _get_interaction_coordinator(self):  # noqa: ANN001
                raise AssertionError("Runtime status query must not construct or read old Coordinator first")

        worker = _ForbiddenCoordinatorLookupWorker(
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._agent_runtime.sync_external_plan_context(
            room_id="room-agent-status-runtime-first",
            external_plan_id="seed-agent-status-runtime-first",
            text="做一个强盗藏宝室，包含宝箱、金币和火把",
            owner_agent="山贼",
        )

        reply = worker._handle_coordinator_status_query({
            "room_id": "room-agent-status-runtime-first",
            "message_id": "msg-agent-status-runtime-first",
            "text": "@GM 总结当前方案",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("GM Runtime", reply or "")
        self.assertIn("强盗藏宝室", reply or "")
        self.assertIn("runtime_status_queried", worker._agent_runtime.operation_log.events())

    def test_gm_summary_reply_includes_runtime_resource_flow_digest(self) -> None:
        coordinator = _ExplodingCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        result = worker._agent_runtime.handle_message(
            room_id="room-gm-resource-flow",
            external_plan_id="seed-gm-resource-flow",
            action="confirm_and_execute",
            text="做一个强盗藏宝室，包含宝箱、金币堆和火把",
            sender_id="host-1",
            sender_name="房主",
        )
        plan_id = str((result.get("plan") or {}).get("plan_id") or "")
        worker._agent_runtime.operation_log.append(
            "tool_call_succeeded",
            room_id="room-gm-resource-flow",
            plan_id=plan_id,
            batch_id="batch-gm-vlm",
            payload={
                "source": "runtime_vlm_checkpoint",
                "status": "advisory",
                "checkpoint_type": "final_consistency_review",
                "advisory_item_count": 2,
                "reviewed_targets": ["入口", "主街"],
            },
        )
        worker._agent_runtime.operation_log.append(
            "review_advisory_proposal_created",
            room_id="room-gm-resource-flow",
            plan_id=plan_id,
            batch_id="batch-gm-vlm",
            payload={"proposal_id": "review-gm-vlm", "item_count": 2},
        )
        worker._agent_runtime.provider_status("room-gm-resource-flow", plan_id=plan_id)

        reply = worker._handle_coordinator_status_query({
            "room_id": "room-gm-resource-flow",
            "message_id": "msg-gm-resource-flow",
            "text": "@GM 总结当前方案",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("GM Runtime", reply or "")
        self.assertIn("Batch tooling", reply or "")
        self.assertIn("created-batches", reply or "")
        self.assertIn("StatePatch", reply or "")
        self.assertIn("Failure strategy", reply or "")
        self.assertIn("RuntimeGuard", reply or "")
        self.assertIn("Plan lifecycle", reply or "")
        self.assertIn("created", reply or "")
        self.assertIn("blocked", reply or "")
        self.assertIn("VLM replay", reply or "")
        self.assertIn("checkpoints 1", reply or "")
        self.assertIn("Review advisory replay", reply or "")
        self.assertIn("proposals 1", reply or "")
        self.assertIn("Tool execution", reply or "")
        self.assertIn("nodes", reply or "")
        self.assertIn("Scene contract", reply or "")
        self.assertIn("indoor-room", reply or "")
        self.assertIn("room-box", reply or "")
        self.assertIn("Semantic arbitration", reply or "")
        self.assertIn("ready-for-next-runtime-step", reply or "")
        self.assertIn("Scene snapshot", reply or "")
        self.assertIn("snapshots", reply or "")
        self.assertIn("Fact source", reply or "")
        self.assertIn("plan/batch", reply or "")
        self.assertIn("Runtime resources", reply or "")
        self.assertIn("image", reply or "")
        self.assertIn("model", reply or "")
        self.assertIn("Import", reply or "")
        self.assertIn("imported", reply or "")
        self.assertIn("Engine write", reply or "")
        self.assertIn("Engine write readiness", reply or "")
        self.assertIn("runtime-state", reply or "")
        self.assertIn("fallback", reply or "")
        self.assertIn("disabled", reply or "")
        self.assertIn("Engine write boundary", reply or "")
        self.assertIn("Message delivery", reply or "")
        self.assertIn("资源批次：batches", reply or "")
        self.assertIn("资源通道复盘：", reply or "")
        self.assertIn("query-ready", reply or "")
        self.assertIn("publish-ready", reply or "")
        self.assertIn("Runtime queue:", reply or "")
        self.assertIn("pressure", reply or "")
        self.assertIn("completed", reply or "")
        self.assertIn("failed 0", reply or "")
        self.assertIn("img/model/import", reply or "")
        self.assertIn("runtime_gm_summary_exported", worker._agent_runtime.operation_log.events())
        self.assertNotIn("provider", reply or "")
        self.assertNotIn("prompt", reply or "")
        self.assertNotIn("ToolCallGraph", reply or "")

    def test_gm_summary_uses_room_runtime_context_before_coordinator_lookup_without_plan(self) -> None:
        class _ForbiddenCoordinatorLookupWorker(_TestWorker):
            def _get_interaction_coordinator(self):  # noqa: ANN001
                raise AssertionError("Room-level Runtime summary must not construct or read old Coordinator first")

        worker = _ForbiddenCoordinatorLookupWorker(
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._agent_runtime.handle_message(
            room_id="room-gm-summary-room-context",
            text="围绕强盗藏宝室主题讨论一下，重点是宝箱、金币和火把。",
            sender_id="host-1",
            sender_name="房主",
            action="user_discussion",
        )

        reply = worker._handle_coordinator_status_query({
            "room_id": "room-gm-summary-room-context",
            "message_id": "msg-gm-summary-room-context",
            "text": "@GM 总结一下当前讨论",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("GM Runtime", reply or "")
        self.assertIn("ScenePlan", reply or "")
        self.assertIn("Semantic arbitration", reply or "")
        self.assertIn("context-only-needs-plan", reply or "")
        self.assertIn("clarify yes", reply or "")
        self.assertIn("Tool execution", reply or "")
        self.assertIn("none", reply or "")
        self.assertIn("runtime_gm_summary_exported", worker._agent_runtime.operation_log.events())

    def test_plain_chat_status_query_uses_runtime_before_coordinator_lookup(self) -> None:
        coordinator = _ExplodingCoordinator()
        worker = _LayoutDirectExecutionTrackingWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._agent_runtime.sync_external_plan_context(
            room_id="room-sync-status-runtime-first",
            external_plan_id="seed-sync-status-runtime-first",
            text="做一个强盗藏宝室，包含宝箱、金币和火把",
            owner_agent="山贼",
        )

        handled = worker.sync_chat_message_to_coordinator(
            {
                "room_id": "room-sync-status-runtime-first",
                "message_id": "msg-sync-status-runtime-first",
                "text": "现在生成到哪里了",
                "sender_id": "host-1",
                "sender_name": "房主",
                "sender_type": "host",
                "message_kind": "chat",
                "is_host": True,
            },
            source="lanchat_native_queue",
            emit_disclosure=False,
        )

        self.assertTrue(handled)
        self.assertTrue(worker.coordinator_system_replies)
        self.assertIn("Runtime", worker.coordinator_system_replies[-1])
        self.assertIn("Tool execution", worker.coordinator_system_replies[-1])
        self.assertIn("ToolGraph replay", worker.coordinator_system_replies[-1])
        self.assertIn("runtime_status_queried", worker._agent_runtime.operation_log.events())

    def test_status_query_does_not_fallback_to_old_coordinator_by_default_when_runtime_unavailable(self) -> None:
        class _ForbiddenCoordinatorLookupWorker(_TestWorker):
            def _get_interaction_coordinator(self):  # noqa: ANN001
                raise AssertionError("Runtime status unavailable must not fallback to old Coordinator by default")

        worker = _ForbiddenCoordinatorLookupWorker(
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        with patch.object(worker, "_agent_runtime_status_reply", return_value=""):
            reply = worker._handle_coordinator_status_query({
                "room_id": "room-status-no-legacy-fallback",
                "message_id": "msg-status-no-legacy-fallback",
                "text": "现在生成到哪里了",
                "sender_id": "host-1",
                "sender_name": "房主",
                "sender_type": "host",
                "message_kind": "chat",
                "is_host": True,
            })

        self.assertIn("Runtime 状态暂不可用", reply or "")
        self.assertIn("旧状态源默认已关闭", reply or "")

    def test_status_query_can_use_old_coordinator_when_legacy_main_workflow_flag_enabled(self) -> None:
        coordinator = _FakeCoordinator()
        flags = AgentRuntimeFlags.from_env({
            "AGENT_RUNTIME_ENABLED": "1",
            "OLD_WORKFLOW_DIRECT_ENTRY_DISABLED": "0",
            "ALLOW_LEGACY_MAIN_WORKFLOW": "1",
        })
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=flags,
        )

        with patch.object(worker, "_agent_runtime_status_reply", return_value=""):
            reply = worker._handle_coordinator_status_query({
                "room_id": "room-status-legacy-fallback",
                "message_id": "msg-status-legacy-fallback",
                "text": "现在生成到哪里了",
                "sender_id": "host-1",
                "sender_name": "房主",
                "sender_type": "host",
                "message_kind": "chat",
                "is_host": True,
            })

        self.assertEqual(reply, "coordinator status")
        self.assertEqual(len(coordinator.ingest_calls), 1)

    def test_runtime_planning_compose_routes_directly_to_agent_runtime(self) -> None:
        engine = _FakeReplyEngine()
        coordinator = _FakeCoordinator()
        worker = _TestWorker(
            corona_engine=engine,
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        handled = worker._execute_runtime_planning_compose(
            {
                "room_id": "room-planning-compose",
                "message_id": "msg-compose",
                "sender_id": "host-1",
                "sender_name": "房主",
                "sender_type": "host",
                "message_kind": "chat",
                "agent_id": "merchant",
                "agent_name": "商人",
                "metadata": {"scene_name": "Scene/场景1.scene"},
            },
            "build a treasure room with chest and torch",
            "商人",
        )

        self.assertTrue(handled)
        self.assertEqual(coordinator.seed_plan_calls, [])
        self.assertEqual(coordinator.execute_calls, [])
        self.assertEqual(len(engine.replies), 1)
        self.assertIn("【AgentRuntime 执行结果】ScenePlan", engine.replies[0]["text"])
        state = worker._agent_runtime.query_state("room-planning-compose")["room"]
        runtime_plan_id = state["external_plan_links"]["planning:msg-compose"]
        self.assertEqual(state["active_plan_id"], runtime_plan_id)
        import_nodes = [
            node
            for graph in state["tool_graphs"].values()
            for node in graph["nodes"].values()
            if node["tool_name"] == "runtime.actor.import_batch"
        ]
        self.assertTrue(import_nodes)
        self.assertEqual(import_nodes[0]["args"]["scene_name"], "Scene/场景1.scene")

    def test_runtime_planning_compose_failure_does_not_fallback_to_legacy_by_default(self) -> None:
        class FailingRuntime:
            def handle_message(self, **kwargs):  # noqa: ANN001
                raise RuntimeError("runtime unavailable")

        engine = _FakeReplyEngine()
        coordinator = _FakeCoordinator()
        worker = _TestWorker(
            corona_engine=engine,
            interaction_coordinator=coordinator,
            agent_runtime=FailingRuntime(),
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        handled = worker._execute_runtime_planning_compose(
            {
                "room_id": "room-planning-compose-fail",
                "message_id": "msg-compose-fail",
                "sender_id": "host-1",
                "sender_name": "房主",
                "agent_name": "商人",
            },
            "build a treasure room with chest and torch",
            "商人",
        )

        self.assertFalse(handled)
        self.assertEqual(coordinator.seed_plan_calls, [])
        self.assertEqual(coordinator.execute_calls, [])
        self.assertEqual(engine.replies, [])

    def test_runtime_planning_compose_failure_can_fallback_only_with_legacy_flags(self) -> None:
        class FailingRuntime:
            def handle_message(self, **kwargs):  # noqa: ANN001
                raise RuntimeError("runtime unavailable")

        engine = _FakeReplyEngine()
        coordinator = _FakeCoordinator()
        flags = AgentRuntimeFlags.from_env(
            {
                "AGENT_RUNTIME_ENABLED": "1",
                "OLD_WORKFLOW_DIRECT_ENTRY_DISABLED": "0",
                "ALLOW_LEGACY_MAIN_WORKFLOW": "1",
            }
        )
        worker = _TestWorker(
            corona_engine=engine,
            interaction_coordinator=coordinator,
            agent_runtime=FailingRuntime(),
            agent_runtime_flags=flags,
        )

        handled = worker._execute_runtime_planning_compose(
            {
                "room_id": "room-planning-compose-legacy-fallback",
                "message_id": "msg-compose-legacy-fallback",
                "sender_id": "host-1",
                "sender_name": "房主",
                "agent_name": "商人",
            },
            "build a treasure room with chest and torch",
            "商人",
        )

        self.assertTrue(handled)
        self.assertEqual(len(coordinator.seed_plan_calls), 1)
        self.assertEqual(coordinator.execute_calls, ["seed-test"])
        self.assertEqual(len(engine.replies), 1)
        self.assertIn("SeedPlan seed-test 已进入生成队列", engine.replies[0]["text"])

    def test_worker_mirrors_planning_context_to_runtime_without_tool_graph(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))

        result = worker._mirror_planning_context_in_agent_runtime(
            room_id="room-planning-context",
            text="在长者方案基础上进一步修改",
            trigger={"room_id": "room-planning-context", "message_id": "msg-plan"},
            plan=_FakeDraftPlan(),
            metadata={"source_context_agent": "长者", "target_agent_name": "山贼"},
        )

        self.assertTrue(result["mirrored"])
        state = worker._agent_runtime.query_state("room-planning-context")["room"]
        self.assertEqual(state["external_plan_links"], {})
        self.assertEqual(state["scene_plans"], {})
        self.assertEqual(self._non_planning_tool_graphs(state), [])
        self.assertEqual(len(state["planning_context_events"]), 1)
        self.assertIn("强盗藏宝室", state["planning_context_events"][0]["text_preview"])
        self.assertEqual(state["planning_context_events"][0]["context_type"], "room_chat")
        self.assertIn("room_chat_message_recorded", worker._agent_runtime.operation_log.events())
        self.assertNotIn("external_plan_context_synced", worker._agent_runtime.operation_log.events())

    def test_worker_mirrors_agent_reply_context_after_successful_send(self) -> None:
        engine = _FakeReplyEngine()
        worker = _TestWorker(corona_engine=engine, agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        worker._mirror_planning_context_in_agent_runtime(
            room_id="room-agent-reply-context",
            text="在长者方案基础上进一步修改",
            trigger={"room_id": "room-agent-reply-context", "message_id": "msg-plan"},
            plan=_FakeDraftPlan(),
            metadata={"source_context_agent": "长者", "target_agent_name": "山贼"},
        )

        sent = worker._send_final_reply(
            "agent-merchant",
            "商人",
            "我建议加入地图桌和首领座椅，让藏宝室更像强盗据点。",
            {
                "room_id": "room-agent-reply-context",
                "message_id": "msg-agent",
                "target_plan_id": "seed-draft",
                "correlation_id": "corr-agent",
            },
        )

        self.assertTrue(sent)
        self.assertEqual(len(engine.replies), 1)
        state = worker._agent_runtime.query_state("room-agent-reply-context")["room"]
        self.assertEqual(self._non_planning_tool_graphs(state), [])
        self.assertEqual(len(state["planning_context_events"]), 2)
        latest = state["planning_context_events"][-1]
        self.assertEqual(latest["context_type"], "room_agent_reply")
        self.assertEqual(latest["speaker_type"], "agent")
        self.assertEqual(latest["agent_name"], "商人")
        self.assertEqual(latest["reply_to"], "msg-agent")
        self.assertEqual(state["scene_plans"], {})
        self.assertIn("room_agent_reply_recorded", worker._agent_runtime.operation_log.events())

    def test_runtime_status_reply_keeps_frozen_brief_when_agent_reply_is_recorded(self) -> None:
        engine = _FakeReplyEngine()
        worker = _TestWorker(corona_engine=engine, agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        worker._agent_runtime.sync_external_plan_context(
            room_id="room-status-promoted",
            external_plan_id="seed-status-promoted",
            text="做一个可爱卧室，包含床和台灯",
            owner_agent="小女孩",
        )

        sent = worker._send_final_reply(
            "agent-merchant",
            "商人",
            "客厅方案：中央放沙发和茶几，侧边放电视墙和地毯。",
            {
                "room_id": "room-status-promoted",
                "message_id": "msg-status-promoted",
                "target_plan_id": "seed-status-promoted",
                "text": "在小女孩基础上改进",
                "agent_id": "agent-merchant",
                "agent_name": "商人",
            },
        )

        self.assertTrue(sent)
        reply = worker._agent_runtime_status_reply(
            room_id="room-status-promoted",
            external_plan_id="seed-status-promoted",
        )
        self.assertIn("方案摘要", reply)
        self.assertIn("可爱卧室", reply)
        self.assertNotIn("沙发", reply)
        self.assertIn("主要模型", reply)

    def test_runtime_status_reply_can_scope_to_explicit_batch_id(self) -> None:
        engine = _FakeReplyEngine()
        worker = _TestWorker(corona_engine=engine, agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        plan = worker._agent_runtime.propose_scene_plan(
            room_id="room-status-batch",
            text="做一个可爱卧室，有床、书桌、衣柜、台灯、地毯",
            owner_agent="小女孩",
        )
        worker._agent_runtime.confirm_scene_plan(plan.plan_id, confirmed_by="房主")
        result = worker._agent_runtime.execute_planned_batches(plan.plan_id, max_items_per_batch=2)
        batches = result["batches"]
        self.assertGreater(len(batches), 1)
        first_batch_id = batches[0]["batch_id"]

        self.assertEqual(
            worker._runtime_batch_id_from_message({
                "metadata": {"runtime_batch_id": first_batch_id},
            }),
            first_batch_id,
        )
        worker._agent_runtime.operation_log.append(
            "sync_event_recorded",
            room_id="room-status-batch",
            plan_id=plan.plan_id,
            batch_id=first_batch_id,
            message="asset_transfer_progress",
            payload={
                "event_type": "asset_transfer_progress",
                "asset_id": "asset-status-secret",
                "peer_id": "peer-status-secret",
                "asset_path": "E:/secret/status.glb",
                "transfer_status": "transferring",
                "progress": 40,
                "chunk_index": 1,
                "chunk_count": 3,
                "bytes_transferred": 256,
                "total_bytes": 768,
            },
        )
        worker._agent_runtime.operation_log.append(
            "sync_event_recorded",
            room_id="room-status-batch",
            plan_id=plan.plan_id,
            batch_id=first_batch_id,
            message="peer_asset_ready",
            payload={
                "event_type": "peer_asset_ready",
                "asset_id": "asset-status-secret",
                "peer_id": "peer-status-secret",
                "transfer_status": "completed",
            },
        )
        worker._agent_runtime.operation_log.append(
            "sync_event_recorded",
            room_id="room-status-batch",
            plan_id=plan.plan_id,
            batch_id=first_batch_id,
            message="room_joined",
            payload={"event_type": "room_joined", "peer_id": "peer-status-secret"},
        )
        worker._agent_runtime.operation_log.append(
            "runtime_system_event_disclosure_skipped",
            room_id="room-status-batch",
            plan_id=plan.plan_id,
            batch_id=first_batch_id,
            payload={
                "runtime_event_type": "agent_internal",
                "runtime_audience": "agent",
                "reason": "audience_not_user_visible",
                "provider": "secret-provider",
                "prompt": "secret-prompt",
            },
        )
        worker._agent_runtime.operation_log.append(
            "tool_call_succeeded",
            room_id="room-status-batch",
            plan_id=plan.plan_id,
            batch_id=first_batch_id,
            payload={
                "source": "runtime_vlm_checkpoint",
                "status": "advisory",
                "checkpoint_type": "structure_review",
                "advisory_item_count": 1,
                "reviewed_targets": ["入口"],
            },
        )
        worker._agent_runtime.operation_log.append(
            "review_advisory_proposal_created",
            room_id="room-status-batch",
            plan_id=plan.plan_id,
            batch_id=first_batch_id,
            payload={"proposal_id": "review-status-batch", "item_count": 1},
        )
        worker._agent_runtime.gm_summary(
            "room-status-batch",
            plan_id=plan.plan_id,
            batch_id=first_batch_id,
        )
        reply = worker._agent_runtime_status_reply(
            room_id="room-status-batch",
            external_plan_id=plan.plan_id,
            batch_id=first_batch_id,
        )

        self.assertIn("- 批次：1 个", reply)
        self.assertIn("- 资源批次：batches 1", reply)
        self.assertIn("- Batch tooling:", reply)
        self.assertIn("created-batches", reply)
        self.assertIn("- StatePatch:", reply)
        self.assertIn("- Failure strategy:", reply)
        self.assertIn("- RuntimeGuard:", reply)
        self.assertIn("- Plan lifecycle:", reply)
        self.assertIn("created", reply)
        self.assertIn("blocked", reply)
        self.assertIn("- VLM replay:", reply)
        self.assertIn("checkpoints 1", reply)
        self.assertIn("advisory 1", reply)
        self.assertIn("- Review advisory replay:", reply)
        self.assertIn("proposals 1", reply)
        self.assertIn("- GM replay:", reply)
        self.assertIn("exported 1", reply)
        self.assertIn("- 场景契约：", reply)
        self.assertIn("indoor-room", reply)
        self.assertIn("room-box", reply)
        self.assertIn("- 语义仲裁：", reply)
        self.assertIn("ready-for-next-runtime-step", reply)
        self.assertIn("completed 1", reply)
        self.assertIn("failed 0", reply)
        self.assertIn("床", reply)
        self.assertIn("书桌", reply)
        self.assertIn("ToolCallGraph：1", reply)
        self.assertIn("Tool execution：", reply)
        self.assertIn("nodes", reply)
        self.assertIn("Runtime queue:", reply)
        self.assertIn("pressure", reply)
        self.assertIn("同传复盘：", reply)
        self.assertIn("progress 1", reply)
        self.assertIn("peer-ready 1", reply)
        self.assertIn("Peer 复盘：", reply)
        self.assertIn("join 1", reply)
        self.assertIn("RuntimeEvent replay:", reply)
        self.assertIn("skipped 1", reply)
        self.assertIn("latest-skip agent-internal:agent", reply)
        self.assertNotIn("asset-status-secret", reply)
        self.assertNotIn("peer-status-secret", reply)
        self.assertNotIn("E:/secret/status.glb", reply)
        full_reply = worker._agent_runtime_status_reply(
            room_id="room-status-batch",
            external_plan_id=plan.plan_id,
        )
        self.assertIn("- 资源批次：batches", full_reply)
        self.assertIn("completed", full_reply)
        self.assertNotIn("provider", full_reply)
        self.assertNotIn("prompt", full_reply)

    def test_sync_chat_status_query_uses_metadata_batch_id(self) -> None:
        engine = _FakeIdleEngine()
        worker = _TestWorker(corona_engine=engine, agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        plan = worker._agent_runtime.propose_scene_plan(
            room_id="room-sync-status-batch",
            text="做一个可爱卧室，有床、书桌、衣柜、台灯、地毯",
            owner_agent="小女孩",
        )
        worker._agent_runtime.confirm_scene_plan(plan.plan_id, confirmed_by="房主")
        result = worker._agent_runtime.execute_planned_batches(plan.plan_id, max_items_per_batch=2)
        first_batch_id = result["batches"][0]["batch_id"]

        handled = worker.sync_chat_message_to_coordinator({
            "room_id": "room-sync-status-batch",
            "message_id": "msg-sync-status-batch",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "is_host": True,
            "text": "现在生成到哪里了",
            "metadata": {"runtime_batch_id": first_batch_id},
        })

        self.assertTrue(handled)
        self.assertEqual(len(engine.system_messages), 1)
        reply = engine.system_messages[-1]["text"]
        self.assertIn("- 批次：1 个", reply)
        self.assertIn("床", reply)
        self.assertIn("书桌", reply)
        model_line = next(line for line in reply.splitlines() if "主要模型" in line)
        self.assertNotIn("衣柜", model_line)

    def test_agent_reply_uses_runtime_active_plan_without_coordinator_lookup(self) -> None:
        class _TrackingCoordinatorLookupWorker(_TestWorker):
            coordinator_lookup_count = 0

            def _get_interaction_coordinator(self):  # noqa: ANN001
                self.coordinator_lookup_count += 1
                raise AssertionError("Agent reply context must use Runtime active plan first")

        engine = _FakeReplyEngine()
        worker = _TrackingCoordinatorLookupWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._agent_runtime.sync_external_plan_context(
            room_id="room-agent-reply-runtime-first",
            external_plan_id="seed-agent-reply-runtime-first",
            text="做一个强盗藏宝室，包含宝箱和火把",
            owner_agent="山贼",
        )

        sent = worker._send_final_reply(
            "agent-merchant",
            "商人",
            "方案内容：中央放藏宝箱和金币堆，两侧放火把与武器架。",
            {
                "room_id": "room-agent-reply-runtime-first",
                "message_id": "msg-agent-reply-runtime-first",
                "correlation_id": "corr-agent-reply-runtime-first",
                "text": "在山贼基础上改进",
                "agent_id": "agent-merchant",
                "agent_name": "商人",
            },
        )

        self.assertTrue(sent)
        self.assertEqual(worker.coordinator_lookup_count, 0)
        state = worker._agent_runtime.query_state("room-agent-reply-runtime-first")["room"]
        self.assertEqual(len(state["planning_context_events"]), 2)
        latest = state["planning_context_events"][-1]
        self.assertEqual(latest["context_type"], "agent_reply")
        self.assertEqual(latest["agent_name"], "商人")
        runtime_plan = state["scene_plans"][state["active_plan_id"]]
        self.assertEqual(runtime_plan["design_brief"], "做一个强盗藏宝室，包含宝箱和火把")
        self.assertIn("藏宝箱", latest["text_preview"])
        requested_entry = worker._agent_runtime.operation_log.query(
            event="agent_reply_send_requested",
            room_id="room-agent-reply-runtime-first",
        )[-1]
        succeeded_entry = worker._agent_runtime.operation_log.query(
            event="agent_reply_send_succeeded",
            room_id="room-agent-reply-runtime-first",
        )[-1]
        self.assertEqual(requested_entry.plan_id, state["active_plan_id"])
        self.assertEqual(succeeded_entry.plan_id, state["active_plan_id"])
        plan_replay = worker._agent_runtime.operation_replay(
            room_id="room-agent-reply-runtime-first",
            plan_id=state["active_plan_id"],
        )
        replay_events = [entry["event"] for entry in plan_replay["entries"]]
        self.assertIn("agent_reply_send_requested", replay_events)
        self.assertIn("agent_reply_send_succeeded", replay_events)

    def test_worker_mirrors_agent_reply_without_plan_as_room_context(self) -> None:
        engine = _FakeReplyEngine()
        worker = _TestWorker(
            corona_engine=engine,
            interaction_coordinator=_FakeNoActiveCoordinator(),
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        sent = worker._send_final_reply(
            "agent-elder",
            "长者",
            "我是长者，可以先讨论方案；需要确认后才会生成。",
            {
                "room_id": "room-agent-reply-no-plan",
                "message_id": "msg-agent-reply-no-plan",
                "correlation_id": "corr-agent-reply-no-plan",
            },
        )

        self.assertTrue(sent)
        self.assertEqual(len(engine.replies), 1)
        state = worker._agent_runtime.query_state("room-agent-reply-no-plan")["room"]
        self.assertEqual(state["scene_plans"], {})
        self.assertEqual(self._non_planning_tool_graphs(state), [])
        self.assertEqual(len(state["planning_context_events"]), 1)
        latest = state["planning_context_events"][-1]
        self.assertEqual(latest["context_type"], "room_agent_reply")
        self.assertEqual(latest["agent_name"], "长者")
        self.assertEqual(latest["reply_to"], "msg-agent-reply-no-plan")
        reply = worker._agent_runtime_status_reply(room_id="room-agent-reply-no-plan")
        self.assertIn("ScenePlan", reply)
        self.assertIn("当前方案：尚未形成 ScenePlan", reply)
        events = worker._agent_runtime.operation_log.events()
        self.assertIn("agent_reply_send_requested", events)
        self.assertIn("agent_reply_send_succeeded", events)
        self.assertIn("room_agent_reply_recorded", events)
        self.assertLess(events.index("agent_reply_send_requested"), events.index("room_agent_reply_recorded"))
        self.assertLess(events.index("agent_reply_send_succeeded"), events.index("room_agent_reply_recorded"))

    def test_failed_agent_reply_send_records_audit_without_runtime_context(self) -> None:
        engine = _FakeFailingReplyEngine()
        worker = _TestWorker(
            corona_engine=engine,
            interaction_coordinator=_FakeNoActiveCoordinator(),
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        sent = worker._send_final_reply(
            "agent-elder",
            "长者",
            "我是长者，可以先讨论方案；需要确认后才会生成。",
            {
                "room_id": "room-agent-reply-failed",
                "message_id": "msg-agent-reply-failed",
                "correlation_id": "corr-agent-reply-failed",
            },
        )

        self.assertFalse(sent)
        self.assertEqual(len(engine.replies), 1)
        state = worker._agent_runtime.query_state("room-agent-reply-failed")["room"]
        self.assertEqual(state["scene_plans"], {})
        self.assertEqual(state["planning_context_events"], [])
        events = worker._agent_runtime.operation_log.events()
        self.assertIn("agent_reply_send_requested", events)
        self.assertIn("agent_reply_send_failed", events)
        self.assertNotIn("room_agent_reply_recorded", events)
        failed_entry = worker._agent_runtime.operation_log.query(
            event="agent_reply_send_failed",
            room_id="room-agent-reply-failed",
        )[-1]
        self.assertFalse(failed_entry.payload["sent"])
        self.assertEqual(failed_entry.payload["reply_to"], "msg-agent-reply-failed")

    def test_worker_mirrors_gm_proposal_system_reply_into_runtime_context(self) -> None:
        engine = _FakeIdleEngine()
        worker = _TestWorker(corona_engine=engine, agent_runtime_flags=AgentRuntimeFlags.from_env({}))

        sent = worker._send_final_reply(
            "gm",
            "GM",
            "【GM 提案 gm-test】建议按强盗藏宝室方案继续，请房主确认。",
            {
                "room_id": "room-gm-proposal-context",
                "message_id": "msg-gm-proposal-context",
                "correlation_id": "corr-gm-proposal-context",
            },
            {
                "proposal_id": "gm-test",
                "status": "pending_host_confirmation",
                "requires_host_confirm": True,
                "action_type": "proposal",
            },
        )

        self.assertTrue(sent)
        self.assertEqual(len(engine.system_messages), 1)
        state = worker._agent_runtime.query_state("room-gm-proposal-context")["room"]
        self.assertEqual(state["scene_plans"], {})
        self.assertEqual(self._non_planning_tool_graphs(state), [])
        self.assertEqual(len(state["planning_context_events"]), 1)
        latest = state["planning_context_events"][-1]
        self.assertEqual(latest["context_type"], "room_agent_reply")
        self.assertEqual(latest["speaker_type"], "agent")
        self.assertEqual(latest["agent_name"], "GM")
        self.assertIn("强盗藏宝室", latest["text_preview"])
        reply = worker._agent_runtime_status_reply(room_id="room-gm-proposal-context")
        self.assertIn("当前方案：尚未形成 ScenePlan", reply)
        events = worker._agent_runtime.operation_log.events()
        self.assertIn("gm_proposal_send_requested", events)
        self.assertIn("gm_proposal_send_succeeded", events)
        self.assertIn("room_agent_reply_recorded", events)
        self.assertLess(events.index("gm_proposal_send_requested"), events.index("room_agent_reply_recorded"))
        self.assertLess(events.index("gm_proposal_send_succeeded"), events.index("room_agent_reply_recorded"))

    def test_worker_mirrors_gm_proposal_to_active_runtime_plan_when_target_plan_present(self) -> None:
        engine = _FakeIdleEngine()
        worker = _TestWorker(corona_engine=engine, agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        worker._agent_runtime.sync_external_plan_context(
            room_id="room-gm-proposal-plan-context",
            external_plan_id="seed-gm-proposal-plan",
            text="做一个强盗藏宝室，包含宝箱、金币和火把",
            owner_agent="山贼",
        )

        sent = worker._send_final_reply(
            "gm",
            "GM",
            "【GM 提案 gm-plan】建议确认强盗藏宝室方案。",
            {
                "room_id": "room-gm-proposal-plan-context",
                "message_id": "msg-gm-proposal-plan-context",
                "correlation_id": "corr-gm-proposal-plan-context",
            },
            {
                "proposal_id": "gm-plan",
                "target_plan_id": "seed-gm-proposal-plan",
                "status": "pending_host_confirmation",
                "requires_host_confirm": True,
                "action_type": "proposal",
            },
        )

        self.assertTrue(sent)
        state = worker._agent_runtime.query_state("room-gm-proposal-plan-context")["room"]
        runtime_plan_id = state["external_plan_links"]["seed-gm-proposal-plan"]
        latest = state["planning_context_events"][-1]
        self.assertEqual(latest["context_type"], "agent_reply")
        self.assertEqual(latest["plan_id"], runtime_plan_id)
        self.assertEqual(latest["external_plan_id"], "seed-gm-proposal-plan")
        self.assertEqual(latest["agent_name"], "GM")
        self.assertIn("强盗藏宝室", latest["text_preview"])
        self.assertEqual(self._non_planning_tool_graphs(state), [])
        reply = worker._agent_runtime_status_reply(
            room_id="room-gm-proposal-plan-context",
            external_plan_id="seed-gm-proposal-plan",
        )
        self.assertIn("当前方案：", reply)
        events = worker._agent_runtime.operation_log.events()
        self.assertIn("gm_proposal_send_requested", events)
        self.assertIn("gm_proposal_send_succeeded", events)
        self.assertIn("agent_context_message_recorded", events)
        self.assertLess(events.index("gm_proposal_send_requested"), events.index("agent_context_message_recorded"))
        self.assertLess(events.index("gm_proposal_send_succeeded"), events.index("agent_context_message_recorded"))
        request_entry = worker._agent_runtime.operation_log.query(
            event="gm_proposal_send_requested",
            room_id="room-gm-proposal-plan-context",
        )[-1]
        self.assertEqual(request_entry.plan_id, runtime_plan_id)
        self.assertEqual(request_entry.payload["external_plan_id"], "seed-gm-proposal-plan")

    def test_failed_gm_proposal_send_records_audit_without_runtime_context(self) -> None:
        engine = _FakeFailingSystemMessageEngine()
        worker = _TestWorker(corona_engine=engine, agent_runtime_flags=AgentRuntimeFlags.from_env({}))

        sent = worker._send_final_reply(
            "gm",
            "GM",
            "【GM 提案 gm-fail】建议按强盗藏宝室方案继续，请房主确认。",
            {
                "room_id": "room-gm-proposal-failed",
                "message_id": "msg-gm-proposal-failed",
                "correlation_id": "corr-gm-proposal-failed",
            },
            {
                "proposal_id": "gm-fail",
                "status": "pending_host_confirmation",
                "requires_host_confirm": True,
                "action_type": "proposal",
            },
        )

        self.assertFalse(sent)
        self.assertEqual(len(engine.system_messages), 1)
        state = worker._agent_runtime.query_state("room-gm-proposal-failed")["room"]
        self.assertEqual(state["scene_plans"], {})
        self.assertEqual(state["planning_context_events"], [])
        events = worker._agent_runtime.operation_log.events()
        self.assertIn("gm_proposal_send_requested", events)
        self.assertIn("gm_proposal_send_failed", events)
        self.assertNotIn("room_agent_reply_recorded", events)
        failed_entry = worker._agent_runtime.operation_log.query(
            event="gm_proposal_send_failed",
            room_id="room-gm-proposal-failed",
        )[-1]
        self.assertFalse(failed_entry.payload["sent"])
        self.assertEqual(failed_entry.payload["proposal_id"], "gm-fail")

    def test_worker_mirrors_user_discussion_context_without_tool_graph(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        worker._mirror_planning_context_in_agent_runtime(
            room_id="room-user-discussion-context",
            text="在长者方案基础上进一步修改",
            trigger={"room_id": "room-user-discussion-context", "message_id": "msg-plan"},
            plan=_FakeDraftPlan(),
            metadata={"source_context_agent": "长者", "target_agent_name": "山贼"},
        )

        result = worker._mirror_user_context_in_agent_runtime(
            room_id="room-user-discussion-context",
            text="我同意商人的评价，藏宝室里要突出诅咒王冠和暗门机关。",
            trigger={
                "room_id": "room-user-discussion-context",
                "message_id": "msg-user-discussion",
                "sender_id": "host-1",
                "sender_name": "房主",
            },
            plan=_FakeDraftPlan(),
            metadata={},
        )

        self.assertTrue(result["recorded"])
        state = worker._agent_runtime.query_state("room-user-discussion-context")["room"]
        self.assertEqual(self._non_planning_tool_graphs(state), [])
        self.assertEqual(len(state["planning_context_events"]), 2)
        latest = state["planning_context_events"][-1]
        self.assertEqual(latest["context_type"], "room_chat")
        self.assertEqual(latest["speaker_type"], "user")
        self.assertEqual(latest["agent_name"], "房主")
        self.assertEqual(state["scene_plans"], {})
        self.assertIn("room_chat_message_recorded", worker._agent_runtime.operation_log.events())

    def test_agent_trigger_scene_topic_discussion_does_not_seed_runtime_plan(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))

        with patch.object(
            worker._agent_runtime,
            "query_state",
            side_effect=AssertionError("planning seed bridge must not pre-read RuntimeState"),
        ):
            result = worker._seed_agent_trigger_planning_context_in_runtime({
                "room_id": "room-agent-topic",
                "message_id": "msg-topic",
                "text": "围绕强盗藏宝室主题讨论一下",
                "sender_id": "host-1",
                "sender_name": "房主",
                "sender_type": "host",
                "message_kind": "chat",
                "agent_id": "elder",
                "agent_name": "长者",
            })

        self.assertFalse(result["recorded"])
        self.assertEqual(result["reason"], "intent:discussion")
        state = worker._agent_runtime.state.snapshot("room-agent-topic")["room"]
        self.assertFalse(state["active_plan_id"])
        self.assertEqual(state["scene_plans"], {})
        self.assertEqual(self._non_planning_tool_graphs(state), [])
        self.assertEqual(state["planning_context_events"], [])

    def test_agent_trigger_generate_one_scene_seeds_runtime_plan_without_legacy_compose(self) -> None:
        cases = (
            (
                "room-agent-generate-bedroom",
                "生成一个儿童卧室，有小木马、玩具、小木桌。",
                "儿童卧室",
                "girl",
                "小女孩",
            ),
            (
                "room-agent-generate-forest",
                "生成一个简单森林营地，有草地、帐篷、小木桌。",
                "森林营地",
                "girl",
                "小女孩",
            ),
        )
        for room_id, text, expected_preview, agent_id, agent_name in cases:
            with self.subTest(room_id=room_id):
                worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))

                with patch.object(
                    worker._agent_runtime,
                    "query_state",
                    side_effect=AssertionError("planning seed bridge must not pre-read RuntimeState"),
                ):
                    result = worker._seed_agent_trigger_planning_context_in_runtime({
                        "room_id": room_id,
                        "message_id": f"msg-{room_id}",
                        "text": text,
                        "sender_id": "host-1",
                        "sender_name": "房主",
                        "sender_type": "host",
                        "message_kind": "chat",
                        "agent_id": agent_id,
                        "agent_name": agent_name,
                    })

                self.assertTrue(result["recorded"])
                self.assertEqual(result["action"], "runtime.plan_context.record")
                state = worker._agent_runtime.state.snapshot(room_id)["room"]
                self.assertFalse(state["active_plan_id"])
                self.assertEqual(state["scene_plans"], {})
                self.assertEqual(self._non_planning_tool_graphs(state), [])
                self.assertEqual(len(state["planning_context_events"]), 1)
                self.assertIn(expected_preview, state["planning_context_events"][0]["text_preview"])

    def test_agent_trigger_generation_seed_reports_confirmable_runtime_draft(self) -> None:
        engine = _FakeReplyEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        with patch.object(
            worker,
            "_handle_coordinator_generation_start",
            return_value=None,
        ), patch.object(
            worker,
            "_handle_coordinator_completed_intervention",
            return_value=None,
        ), patch.object(
            worker,
            "_handle_coordinator_executing_intervention",
            return_value=None,
        ), patch.object(
            worker,
            "_handle_agent_trigger_planning_gate",
            return_value=False,
        ), patch.object(
            worker,
            "_handle_collaboration_proposal",
            return_value=False,
        ), patch.object(
            worker,
            "_run_agent",
            side_effect=AssertionError("Runtime-seeded generation request must not fall through to RoleAgent compose"),
        ):
            handled = worker._process_trigger({
                "room_id": "room-agent-generate-draft-visible",
                "message_id": "msg-generate-draft-visible",
                "text": "生成一个儿童卧室，有小木马、玩具、小木桌。",
                "sender_id": "host-1",
                "sender_name": "房主",
                "sender_type": "host",
                "message_kind": "chat",
                "agent_id": "girl",
                "agent_name": "小女孩",
            })

        self.assertTrue(handled)
        self.assertTrue(engine.replies)
        visible_text = "\n".join(str(reply.get("text") or "") for reply in engine.replies)
        self.assertIn("尚未冻结为可执行 Runtime 方案", visible_text)
        self.assertIn("agent_plan_id/artifact_ref", visible_text)
        self.assertNotIn("RoleAgent", visible_text)
        state = worker._agent_runtime.query_state("room-agent-generate-draft-visible")["room"]
        self.assertFalse(state["active_plan_id"])
        self.assertEqual(state["scene_plans"], {})
        self.assertEqual(self._non_planning_tool_graphs(state), [])
        self.assertIn("legacy_role_agent_scene_write_blocked", worker._agent_runtime.operation_log.events())

    def test_process_trigger_direct_scene_requests_use_same_runtime_planning_gate(self) -> None:
        cases = (
            (
                "room-process-direct-bedroom",
                "\u751f\u6210\u4e00\u4e2a\u513f\u7ae5\u5367\u5ba4\uff0c\u6709\u5c0f\u6728\u9a6c\u3001\u73a9\u5177\u3001\u5c0f\u6728\u684c\u3002",
                "\u513f\u7ae5\u5367\u5ba4",
            ),
            (
                "room-process-direct-forest",
                "\u751f\u6210\u4e00\u4e2a\u7b80\u5355\u68ee\u6797\u8425\u5730\uff0c\u6709\u8349\u5730\u3001\u5e10\u7bf7\u3001\u5c0f\u6728\u684c\u3002",
                "\u68ee\u6797\u8425\u5730",
            ),
        )
        for room_id, text, expected_goal in cases:
            with self.subTest(room_id=room_id):
                engine = _FakeReplyEngine()
                worker = _TestWorker(
                    corona_engine=engine,
                    agent_runtime_flags=AgentRuntimeFlags.from_env({}),
                )

                with patch.object(
                    worker,
                    "_run_agent",
                    side_effect=AssertionError("direct scene request must be handled before RoleAgent compose"),
                ), patch.object(
                    worker,
                    "_handle_collaboration_proposal",
                    return_value=False,
                ):
                    handled = worker._process_trigger({
                        "room_id": room_id,
                        "message_id": f"msg-{room_id}",
                        "text": text,
                        "sender_id": "host-1",
                        "sender_name": "\u623f\u4e3b",
                        "sender_type": "host",
                        "message_kind": "chat",
                        "agent_id": "girl",
                        "agent_name": "\u5c0f\u5973\u5b69",
                    })

                self.assertTrue(handled)
                self.assertTrue(engine.replies)
                visible_text = "\n".join(str(reply.get("text") or "") for reply in engine.replies)
                self.assertIn(expected_goal, visible_text)
                self.assertNotIn("AgentRuntime \u63a5\u7ba1", visible_text)
                self.assertNotIn("RoleAgent", visible_text)
                state = worker._agent_runtime.query_state(room_id)["room"]
                self.assertFalse(state["active_plan_id"])
                self.assertEqual(state["scene_plans"], {})
                self.assertEqual(self._non_planning_tool_graphs(state), [])

    def test_agent_trigger_seed_then_confirm_enqueues_runtime_graph(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        seeded = worker._seed_agent_trigger_planning_context_in_runtime({
            "room_id": "room-agent-seed-confirm",
            "message_id": "msg-seed-confirm-draft",
            "text": "生成一个儿童卧室，有小木马、玩具、小木桌。",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "girl",
            "agent_name": "小女孩",
        })
        confirmed_reply = worker._execute_active_runtime_plan_generation(
            {
                "room_id": "room-agent-seed-confirm",
                "message_id": "msg-seed-confirm-confirm",
                "text": "确认生成",
                "sender_name": "房主",
                "agent_name": "小女孩",
            },
            room_id="room-agent-seed-confirm",
            host_id="host-1",
        )

        self.assertTrue(seeded["recorded"])
        self.assertIsNone(confirmed_reply)
        self.assertNotIn("room-agent-seed-confirm", worker._active_room_ids)
        state = worker._agent_runtime.query_state("room-agent-seed-confirm")["room"]
        self.assertFalse(state["active_plan_id"])
        self.assertEqual(state["batch_plans"], {})
        self.assertEqual(state["tool_graphs"], {})
        events = worker._agent_runtime.operation_log.events()
        self.assertNotIn("scene_plan_confirmed", events)
        self.assertNotIn("runtime_message_enqueued", events)
        self.assertNotIn("runtime_message_executed", events)
        self.assertNotIn("user_report_generated", events)

    def test_agent_trigger_basis_revision_records_target_and_source_agents(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))

        with patch.object(
            worker._agent_runtime,
            "query_state",
            side_effect=AssertionError("planning seed bridge must not pre-read RuntimeState"),
        ):
            result = worker._seed_agent_trigger_planning_context_in_runtime({
                "room_id": "room-agent-basis-revision",
                "message_id": "msg-basis-revision",
                "text": "@商人 在长者基础上改进，加入商会交易厅和展示区",
                "sender_id": "host-1",
                "sender_name": "房主",
                "sender_type": "host",
                "message_kind": "chat",
                "agent_id": "merchant",
                "agent_name": "商人",
            })

        self.assertTrue(result["recorded"])
        self.assertEqual(result["action"], "runtime.plan_context.record")
        state = worker._agent_runtime.state.snapshot("room-agent-basis-revision")["room"]
        self.assertFalse(state["active_plan_id"])
        self.assertEqual(state["scene_plans"], {})
        self.assertEqual(self._non_planning_tool_graphs(state), [])
        context_event = state["planning_context_events"][0]
        self.assertEqual(context_event["context_type"], "room_chat")
        self.assertIn("长者", context_event["source_context_agents"])

    def test_generation_start_executes_runtime_draft_without_coordinator_active_plan(self) -> None:
        coordinator = _FakeNoActiveCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._agent_runtime.sync_external_plan_context(
            room_id="room-runtime-draft-generate",
            external_plan_id="planning:msg-topic",
            text="围绕强盗藏宝室主题讨论一下：中央宝箱，两侧武器架，入口火把。",
            owner_agent="长者",
        )

        reply = worker._handle_coordinator_generation_start({
            "room_id": "room-runtime-draft-generate",
            "message_id": "msg-confirm-runtime",
            "text": "确认生成",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "elder",
            "agent_name": "长者",
        })

        self.assertIn("【AgentRuntime 执行结果】ScenePlan", reply or "")
        self.assertEqual(coordinator.execute_calls, [])
        self.assertEqual(coordinator.ingest_calls, [])
        state = worker._agent_runtime.query_state("room-runtime-draft-generate")["room"]
        self.assertTrue(state["active_plan_id"])
        runtime_plan = state["scene_plans"][state["active_plan_id"]]
        self.assertEqual(runtime_plan["status"], "executing")
        self.assertTrue(state["tool_graphs"])
        self.assertIn("runtime_message_enqueued", worker._agent_runtime.operation_log.events())
        self.assertNotIn("runtime_message_executed", worker._agent_runtime.operation_log.events())

    def test_gm_confirm_plan_phrase_executes_existing_runtime_draft(self) -> None:
        worker = _TestWorker(
            interaction_coordinator=_FakeNoActiveCoordinator(),
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._agent_runtime.sync_external_plan_context(
            room_id="room-gm-confirm-plan",
            external_plan_id="planning:bedroom-plan",
            text="children bedroom with bed, desk and toys",
            owner_agent="girl",
        )

        reply = worker._handle_coordinator_generation_start({
            "room_id": "room-gm-confirm-plan",
            "message_id": "msg-gm-confirm-plan",
            "text": "@GM \u786e\u8ba4\u65b9\u6848",
            "sender_id": "host-1",
            "sender_name": "host",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
        })

        self.assertIn("ScenePlan", reply or "")
        state = worker._agent_runtime.query_state("room-gm-confirm-plan")["room"]
        runtime_plan = state["scene_plans"][state["active_plan_id"]]
        self.assertEqual(runtime_plan["status"], "executing")
        self.assertEqual(len(state["scene_plans"]), 1)

    def test_full_generation_request_without_runtime_plan_records_draft_only(self) -> None:
        worker = _TestWorker(
            interaction_coordinator=_FakeNoActiveCoordinator(),
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        reply = worker._handle_coordinator_generation_start({
            "room_id": "room-generation-request-race",
            "message_id": "msg-generation-request-race",
            "text": "\u6309\u7167\u8fd9\u4e2a\u65b9\u6848\u751f\u6210\uff0c\u505a\u4e00\u4e2a\u660e\u4eae\u7684\u513f\u7ae5\u5367\u5ba4\uff0c\u6709\u5e8a\u3001\u4e66\u684c\u548c\u73a9\u5177",
            "sender_id": "host-1",
            "sender_name": "host",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "girl",
            "agent_name": "girl",
        })

        self.assertIn("\u5df2\u8bb0\u5f55\u672c\u8f6e\u573a\u666f\u9700\u6c42\u4e0a\u4e0b\u6587", reply or "")
        self.assertIn("\u5c1a\u672a\u51bb\u7ed3\u4e3a\u53ef\u6267\u884c Runtime \u65b9\u6848", reply or "")
        state = worker._agent_runtime.query_state("room-generation-request-race")["room"]
        self.assertEqual(state["scene_plans"], {})
        self.assertFalse(state["active_plan_id"])
        self.assertTrue(state["planning_context_events"])
        self.assertEqual(self._non_planning_tool_graphs(state), [])

    def test_pure_generation_confirmation_without_runtime_plan_does_not_create_plan(self) -> None:
        worker = _TestWorker(
            interaction_coordinator=_FakeNoActiveCoordinator(),
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        reply = worker._handle_coordinator_generation_start({
            "room_id": "room-confirm-without-plan",
            "message_id": "msg-confirm-without-plan",
            "text": "\u65b9\u6848\u786e\u8ba4\uff0c\u8fdb\u5165\u751f\u6210",
            "sender_id": "host-1",
            "sender_name": "host",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "girl",
            "agent_name": "girl",
        })

        self.assertIn("\u6ca1\u6709\u53ef\u786e\u8ba4", reply or "")
        state = worker._agent_runtime.query_state("room-confirm-without-plan")["room"]
        self.assertEqual(state["scene_plans"], {})
        self.assertEqual(state["tool_graphs"], {})

    def test_generation_confirmation_waits_for_inflight_discussion_reply(self) -> None:
        engine = _FakeReplyEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        discussion = {
            "room_id": "room-pending-discussion-confirm",
            "message_id": "msg-discussion",
            "text": "@\u957f\u8005 \u5e2e\u6211\u751f\u6210\u4e00\u4e2a\u513f\u7ae5\u5367\u5ba4",
            "sender_id": "host-1",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "elder",
            "agent_name": "\u957f\u8005",
        }
        tracked = worker._begin_pending_discussion_reply(discussion)
        self.assertEqual(tracked, "msg-discussion")

        confirmation = {
            "room_id": "room-pending-discussion-confirm",
            "message_id": "msg-confirm-too-early",
            "text": "@\u957f\u8005 \u786e\u5b9a\u751f\u6210",
            "sender_id": "host-1",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "elder",
            "agent_name": "\u957f\u8005",
        }
        try:
            with patch.object(
                worker._get_orchestrator(),
                "handle_control_trigger",
                side_effect=AssertionError("early confirmation must not create a GM proposal"),
            ), patch.object(
                worker,
                "_run_agent",
                side_effect=AssertionError("early confirmation must not call the LLM"),
            ):
                handled = worker._process_trigger(confirmation)
        finally:
            worker._finish_pending_discussion_reply(
                "room-pending-discussion-confirm",
                tracked,
            )

        self.assertTrue(handled)
        self.assertEqual(len(engine.replies), 1)
        self.assertIn("\u65b9\u6848\u4ecd\u5728\u6574\u7406\u4e2d", engine.replies[0]["text"])
        state = worker._agent_runtime.state.snapshot("room-pending-discussion-confirm")["room"]
        self.assertEqual(state["scene_plans"], {})
        self.assertFalse(any(
            str(graph.get("graph_role") or "") == "business_batch"
            for graph in state["tool_graphs"].values()
            if isinstance(graph, dict)
        ))

    def test_native_sync_blocks_early_confirmation_before_orchestrator(self) -> None:
        engine = _FakeIdleEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        tracked = worker._begin_pending_discussion_reply({
            "room_id": "room-native-early-confirm",
            "message_id": "msg-native-discussion",
            "text": "@elder \u5e2e\u6211\u751f\u6210\u4e00\u4e2a\u513f\u7ae5\u5367\u5ba4",
            "sender_id": "host-1",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "elder",
            "agent_name": "elder",
        })
        confirmation = {
            "room_id": "room-native-early-confirm",
            "message_id": "msg-native-confirm",
            "text": "@elder \u786e\u5b9a\u751f\u6210",
            "sender_id": "host-1",
            "sender_name": "host",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "elder",
            "agent_name": "elder",
        }
        try:
            with patch.object(
                worker._get_orchestrator(),
                "handle_control_trigger",
                side_effect=AssertionError("native sync must block before proposal routing"),
            ):
                handled = worker.sync_chat_message_to_coordinator(confirmation)
        finally:
            worker._finish_pending_discussion_reply("room-native-early-confirm", tracked)

        self.assertTrue(handled)
        self.assertEqual(len(engine.system_messages), 1)
        self.assertIn("\u65b9\u6848\u4ecd\u5728\u6574\u7406\u4e2d", engine.system_messages[0]["text"])
        self.assertEqual(
            worker._message_dispatch_ledger.entry(
                "room-native-early-confirm",
                "msg-native-confirm",
            )["state"],
            "replied",
        )

    def test_agent_trigger_generation_confirmation_defers_to_authoritative_sync(self) -> None:
        worker = _TestWorker(
            interaction_coordinator=_FakeCoordinator(),
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        trigger = {
            "room_id": "room-authoritative-confirm",
            "message_id": "msg-authoritative-confirm",
            "text": "确认生成",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "girl",
            "agent_name": "小女孩",
        }

        with patch.object(
            worker,
            "_handle_coordinator_generation_start",
            side_effect=AssertionError("agent trigger must not execute authoritative generation control"),
        ):
            handled = worker._process_trigger(trigger)

        self.assertTrue(handled)
        state = worker._agent_runtime.state.snapshot("room-authoritative-confirm")["room"]
        self.assertEqual(state["scene_plans"], {})

    def test_agent_reply_reuses_active_discussion_external_plan(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        plan = worker._agent_runtime.sync_external_plan_context(
            room_id="room-discussion-plan-reuse",
            external_plan_id="seed-discussion-plan-reuse",
            text="Create a kids bedroom with a bed and desk",
            owner_agent="elder",
        )
        trigger = {
            "room_id": "room-discussion-plan-reuse",
            "message_id": "msg-role-reply",
            "correlation_id": "different-correlation",
            "agent_id": "elder",
            "agent_name": "elder",
        }

        self.assertEqual(
            worker._runtime_planning_external_id(trigger, "elder"),
            "seed-discussion-plan-reuse",
        )
        recorded = worker._mirror_agent_reply_context_in_agent_runtime(
            room_id="room-discussion-plan-reuse",
            text="Plan: keep a clear path between the bed and desk.",
            trigger=trigger,
            agent_id="elder",
            agent_name="elder",
        )

        self.assertTrue(recorded["recorded"])
        state = worker._agent_runtime.query_state("room-discussion-plan-reuse")["room"]
        self.assertEqual(len(state["scene_plans"]), 1)
        self.assertEqual(recorded["runtime_plan_id"], plan.plan_id)

    def test_agent_reply_plan_context_survives_followup_confirm_generation(self) -> None:
        coordinator = _FakeNoActiveCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        trigger = {
            "room_id": "room-agent-reply-confirm-context",
            "message_id": "msg-user-bedroom",
            "text": "@小女孩 生成一个儿童卧室，有小木马、玩具、小木桌。",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "girl",
            "agent_name": "小女孩",
        }
        reply_text = (
            "小女孩先帮你整理一个温柔、好落地的版本。\n\n"
            "我理解你的目标是：生成一个儿童卧室，有小木马、玩具、小木桌。\n\n"
            "方案内容：\n"
            "1. 风格定位：可爱、柔和、安全的儿童房。\n"
            "2. 空间布局：床边留出玩具区，小木桌靠窗，中央保留活动区。\n"
            "3. 核心物件：小木马、玩具、小木桌、儿童床、地毯。\n"
            "建议先做：确认空间边界，再生成核心家具和玩具。"
        )

        self.assertTrue(worker._should_promote_agent_reply_to_runtime_plan(trigger, reply_text))
        recorded = worker._mirror_agent_reply_context_in_agent_runtime(
            room_id="room-agent-reply-confirm-context",
            text=reply_text,
            trigger=trigger,
            agent_id="girl",
            agent_name="小女孩",
        )

        self.assertTrue(recorded["recorded"])
        reply = worker._handle_coordinator_generation_start({
            "room_id": "room-agent-reply-confirm-context",
            "message_id": "msg-confirm-agent-reply-plan",
            "text": "确认生成",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "girl",
            "agent_name": "小女孩",
        })

        self.assertIn("当前没有可确认的 AgentRuntime 方案", reply or "")
        self.assertEqual(coordinator.execute_calls, [])
        state = worker._agent_runtime.query_state("room-agent-reply-confirm-context")["room"]
        self.assertFalse(state["active_plan_id"])
        self.assertEqual(state["scene_plans"], {})
        self.assertEqual(state["tool_graphs"], {})
        self.assertEqual(state["planning_context_events"][-1]["context_type"], "room_agent_reply")

    def test_agent_reply_design_opinion_does_not_create_runtime_plan(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        trigger = {
            "room_id": "room-agent-opinion-context",
            "message_id": "msg-opinion",
            "text": "评价一下长者方案",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "merchant",
            "agent_name": "商人",
        }
        reply_text = (
            "长者方案整体清楚，布局有层次。"
            "我的建议是保留中央藏宝区和入口火把，但后续仍需要房主确认具体生成方案。"
        )

        self.assertFalse(worker._should_promote_agent_reply_to_runtime_plan(trigger, reply_text))
        recorded = worker._mirror_agent_reply_context_in_agent_runtime(
            room_id="room-agent-opinion-context",
            text=reply_text,
            trigger=trigger,
            agent_id="merchant",
            agent_name="商人",
        )

        self.assertTrue(recorded["recorded"])
        self.assertFalse(recorded["runtime_plan_id"])
        state = worker._agent_runtime.query_state("room-agent-opinion-context")["room"]
        self.assertFalse(state["active_plan_id"])
        self.assertEqual(state["scene_plans"], {})

    def test_generation_start_uses_runtime_active_plan_without_coordinator_active_lookup(self) -> None:
        coordinator = _ExplodingCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._agent_runtime.handle_message(
            room_id="room-runtime-active-generate",
            external_plan_id="planning:runtime-active",
            text="做一个强盗藏宝室，中央宝箱，两侧武器架，入口火把。",
            sender_id="host-1",
            sender_name="房主",
            owner_agent="长者",
            action="plan",
        )

        reply = worker._handle_coordinator_generation_start({
            "room_id": "room-runtime-active-generate",
            "message_id": "msg-confirm-runtime-active",
            "text": "确认生成",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "elder",
            "agent_name": "长者",
        })

        self.assertIn("【AgentRuntime 执行结果】ScenePlan", reply or "")
        self.assertEqual(coordinator.execute_calls, [])
        self.assertEqual(coordinator.ingest_calls, [])
        state = worker._agent_runtime.state.snapshot("room-runtime-active-generate")["room"]
        runtime_plan = state["scene_plans"][state["active_plan_id"]]
        self.assertEqual(runtime_plan["status"], "executing")
        self.assertTrue(state["tool_graphs"])

    def test_plain_chat_generation_start_executes_runtime_draft_without_coordinator_active_plan(self) -> None:
        coordinator = _FakeNoActiveCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._agent_runtime.sync_external_plan_context(
            room_id="room-runtime-draft-chat-generate",
            external_plan_id="planning:msg-topic",
            text="围绕强盗藏宝室主题讨论一下：中央宝箱，两侧武器架，入口火把。",
            owner_agent="长者",
        )

        handled = worker.sync_chat_message_to_coordinator(
            {
                "room_id": "room-runtime-draft-chat-generate",
                "message_id": "msg-confirm-runtime-chat",
                "text": "确认生成",
                "sender_id": "host-1",
                "sender_name": "房主",
                "sender_type": "host",
                "message_kind": "chat",
                "is_host": True,
            },
            source="lanchat_direct",
            emit_disclosure=False,
        )

        self.assertTrue(handled)
        self.assertEqual(coordinator.execute_calls, [])
        self.assertEqual(coordinator.ingest_calls, [])
        state = worker._agent_runtime.query_state("room-runtime-draft-chat-generate")["room"]
        runtime_plan = state["scene_plans"][state["active_plan_id"]]
        self.assertEqual(runtime_plan["status"], "executing")
        self.assertTrue(state["tool_graphs"])
        self.assertIn("runtime_message_enqueued", worker._agent_runtime.operation_log.events())

    def test_plain_chat_generation_start_uses_runtime_before_coordinator_lookup(self) -> None:
        coordinator = _ExplodingCoordinator()
        worker = _LayoutDirectExecutionTrackingWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._agent_runtime.handle_message(
            room_id="room-runtime-first-chat-generate",
            external_plan_id="planning:chat-runtime-active",
            text="做一个强盗藏宝室，中央宝箱，两侧武器架，入口火把。",
            sender_id="host-1",
            sender_name="房主",
            owner_agent="长者",
            action="plan",
        )

        handled = worker.sync_chat_message_to_coordinator(
            {
                "room_id": "room-runtime-first-chat-generate",
                "message_id": "msg-confirm-runtime-first-chat",
                "text": "确认生成",
                "sender_id": "host-1",
                "sender_name": "房主",
                "sender_type": "host",
                "message_kind": "chat",
                "is_host": True,
            },
            source="lanchat_direct",
            emit_disclosure=False,
        )

        self.assertTrue(handled)
        self.assertTrue(worker.coordinator_system_replies)
        self.assertIn("【AgentRuntime 执行结果】ScenePlan", worker.coordinator_system_replies[-1])
        state = worker._agent_runtime.query_state("room-runtime-first-chat-generate")["room"]
        runtime_plan = state["scene_plans"][state["active_plan_id"]]
        self.assertEqual(runtime_plan["status"], "executing")
        self.assertTrue(state["tool_graphs"])

    def test_plain_chat_contextual_plan_update_uses_runtime_before_planning_gate(self) -> None:
        coordinator = _ExplodingCoordinator()
        worker = _LayoutDirectExecutionTrackingWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._agent_runtime.handle_message(
            room_id="room-runtime-contextual-plan-update",
            external_plan_id="planning:contextual-update",
            text="围绕强盗藏宝室主题讨论一下：中央宝箱，两侧武器架，入口火把。",
            sender_id="host-1",
            sender_name="房主",
            owner_agent="长者",
            action="plan",
        )

        with patch.object(
            worker,
            "_handle_plain_chat_planning_gate",
            side_effect=AssertionError("Runtime active plan update must not use legacy pending planning gate"),
        ):
            handled = worker.sync_chat_message_to_coordinator(
                {
                    "room_id": "room-runtime-contextual-plan-update",
                    "message_id": "msg-runtime-contextual-update",
                    "text": "在长者方案基础上进一步修改，加入机关暗门和更强的藏宝叙事",
                    "sender_id": "host-1",
                    "sender_name": "房主",
                    "sender_type": "host",
                    "message_kind": "chat",
                    "is_host": True,
                },
                source="lanchat_direct",
                emit_disclosure=False,
            )

        self.assertTrue(handled)
        self.assertTrue(worker.coordinator_system_replies)
        self.assertIn("已更新当前 Runtime 方案", worker.coordinator_system_replies[-1])
        state = worker._agent_runtime.state.snapshot("room-runtime-contextual-plan-update")["room"]
        self.assertEqual(self._non_planning_tool_graphs(state), [])
        runtime_plan = state["scene_plans"][state["active_plan_id"]]
        self.assertIn("机关暗门", runtime_plan["design_brief"])
        self.assertEqual(len(state["planning_context_events"]), 2)
        latest = state["planning_context_events"][-1]
        self.assertEqual(latest["context_type"], "plan_context")
        self.assertIn("机关暗门", latest["text_preview"])

    def test_agent_trigger_self_intro_does_not_seed_runtime_plan(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))

        result = worker._seed_agent_trigger_planning_context_in_runtime({
            "room_id": "room-agent-intro",
            "message_id": "msg-intro",
            "text": "介绍一下自己",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "elder",
            "agent_name": "长者",
        })

        self.assertFalse(result["recorded"])
        state = worker._agent_runtime.query_state("room-agent-intro")["room"]
        self.assertEqual(state["scene_plans"], {})
        self.assertEqual(self._non_planning_tool_graphs(state), [])
        self.assertEqual(state["planning_context_events"], [])

    def test_plain_chat_skip_path_records_user_discussion_in_runtime(self) -> None:
        coordinator = _FakeCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._agent_runtime.sync_external_plan_context(
            room_id="room-skip-chat-context",
            external_plan_id="seed-test",
            text="强盗藏宝室方案：中央宝箱，两侧武器架，入口火把",
            owner_agent="山贼",
        )

        with patch.object(worker, "_should_sync_chat_to_coordinator", return_value=False), \
             patch.object(worker, "_handle_plain_chat_planning_gate", return_value=""):
            handled = worker.sync_chat_message_to_coordinator(
                {
                    "room_id": "room-skip-chat-context",
                    "message_id": "msg-skip-user",
                    "sender_id": "host-1",
                    "sender_name": "房主",
                    "sender_type": "host",
                    "message_kind": "chat",
                    "text": "我同意商人评价里的诅咒王冠设定。",
                },
                source="unit-test",
                emit_disclosure=False,
            )

        self.assertFalse(handled)
        self.assertEqual(coordinator.ingest_calls, [])
        state = worker._agent_runtime.query_state("room-skip-chat-context")["room"]
        self.assertEqual(self._non_planning_tool_graphs(state), [])
        self.assertEqual(len(state["planning_context_events"]), 2)
        latest = state["planning_context_events"][-1]
        self.assertEqual(latest["context_type"], "user_discussion")
        self.assertEqual(latest["speaker_type"], "user")
        self.assertIn("诅咒王冠", latest["text_preview"])
        self.assertIn("user_context_message_recorded", worker._agent_runtime.operation_log.events())

    def test_plain_chat_without_plan_records_room_chat_in_runtime(self) -> None:
        coordinator = _FakeNoActiveCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        with patch.object(worker, "_should_sync_chat_to_coordinator", return_value=False), \
             patch.object(worker, "_handle_plain_chat_planning_gate", return_value=""):
            handled = worker.sync_chat_message_to_coordinator(
                {
                    "room_id": "room-no-plan-chat-context",
                    "message_id": "msg-no-plan-chat",
                    "sender_id": "host-1",
                    "sender_name": "房主",
                    "sender_type": "host",
                    "message_kind": "chat",
                    "text": "今天先不生成，大家先介绍一下自己。",
                },
                source="unit-test",
                emit_disclosure=False,
            )

        self.assertFalse(handled)
        self.assertEqual(coordinator.ingest_calls, [])
        state = worker._agent_runtime.query_state("room-no-plan-chat-context")["room"]
        self.assertEqual(state["scene_plans"], {})
        self.assertEqual(self._non_planning_tool_graphs(state), [])
        self.assertEqual(len(state["planning_context_events"]), 1)
        latest = state["planning_context_events"][-1]
        self.assertEqual(latest["context_type"], "room_chat")
        self.assertEqual(latest["plan_id"], "")
        self.assertIn("介绍一下自己", latest["text_preview"])
        reply = worker._agent_runtime_status_reply(room_id="room-no-plan-chat-context")
        self.assertIn("Runtime", reply)
        self.assertIn("ScenePlan", reply)
        self.assertIn("当前方案：尚未形成 ScenePlan", reply)
        self.assertIn("Tool execution", reply)
        self.assertNotIn("未命名方案", reply)
        self.assertNotIn("unknown", reply)
        self.assertIn("room_chat_message_recorded", worker._agent_runtime.operation_log.events())

    def test_gm_summary_uses_runtime_room_context_without_scene_plan(self) -> None:
        engine = _FakeReplyEngine()
        coordinator = _FakeNoActiveCoordinator()
        worker = _TestWorker(
            corona_engine=engine,
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._agent_runtime.handle_message(
            room_id="room-gm-runtime-summary",
            action="user_discussion",
            text="围绕强盗藏宝室讨论，要有中央宝箱、金币堆和火把。",
            sender_id="host-1",
            sender_name="房主",
        )
        worker._agent_runtime.handle_message(
            room_id="room-gm-runtime-summary",
            action="agent_context",
            text="长者建议做地下藏宝密室，中央宝箱，两侧武器架，入口火把。",
            sender_id="agent-elder",
            sender_name="长者",
        )

        processed = worker._process_trigger({
            "room_id": "room-gm-runtime-summary",
            "message_id": "msg-gm-summary",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "text": "@GM 总结一下大家的想法",
        })

        self.assertTrue(processed)
        self.assertEqual(len(engine.replies), 1)
        reply = engine.replies[0]["text"]
        self.assertIn("GM Runtime", reply or "")
        self.assertIn("尚未形成 ScenePlan", reply)
        self.assertIn("上下文：2 条，用户 1 / Agent 1", reply)
        self.assertIn("中央宝箱", reply)
        self.assertIn("长者", reply)
        self.assertNotIn("message_id", reply)
        self.assertNotIn("msg-gm-summary", reply)
        self.assertEqual(coordinator.ingest_calls, [])

    def test_gm_runtime_context_summary_returns_none_without_runtime_context(self) -> None:
        worker = _TestWorker(
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        reply = worker._handle_agent_runtime_gm_summary_query({
            "room_id": "room-gm-empty-summary",
            "message_id": "msg-gm-empty",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "text": "@GM 总结一下大家的想法",
        })

        self.assertIsNone(reply)
        state = worker._agent_runtime.query_state("room-gm-empty-summary")["room"]
        self.assertEqual(state["scene_plans"], {})
        self.assertEqual(state["planning_context_events"], [])

    def test_plain_planning_gate_reply_records_user_and_agent_context_in_runtime(self) -> None:
        class FakeSceneRuntime:
            def handle_pending_planning_message(self, text: str):
                return (
                    "reply",
                    "强盗藏宝室方案：中央宝箱，两侧武器架，入口火把。",
                    "长者",
                )

        engine = _FakeReplyEngine()
        worker = _TestWorker(corona_engine=engine, agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        with patch(
            "plugins.AITool.services.lanchat_scene_runtime.get_lanchat_scene_runtime",
            return_value=FakeSceneRuntime(),
        ):
            handled = worker._handle_plain_chat_planning_gate(
                {
                    "room_id": "room-plain-planning-gate",
                    "message_id": "msg-plain-plan",
                    "text": "围绕强盗藏宝室主题讨论一下",
                    "sender_id": "host-1",
                    "sender_name": "房主",
                    "sender_type": "host",
                    "message_kind": "chat",
                },
                "围绕强盗藏宝室主题讨论一下",
            )

        self.assertEqual(handled, "reply")
        self.assertEqual(len(engine.replies), 1)
        state = worker._agent_runtime.query_state("room-plain-planning-gate")["room"]
        self.assertFalse(state["active_plan_id"])
        self.assertEqual(self._non_planning_tool_graphs(state), [])
        self.assertEqual(state["scene_plans"], {})
        self.assertEqual(len(state["planning_context_events"]), 2)
        self.assertTrue(all(event["context_type"] == "room_agent_reply" for event in state["planning_context_events"]))
        self.assertIn("中央宝箱", state["planning_context_events"][-1]["text_preview"])

    def test_plain_planning_gate_compose_uses_agent_runtime_not_legacy_trigger(self) -> None:
        class FakeSceneRuntime:
            def handle_pending_planning_message(self, text: str):
                return (
                    "compose",
                    "用户确认开始生成：强盗藏宝室\n建议物体清单：藏宝箱、金币堆、火把",
                    "山贼",
                )

        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        message = {
            "room_id": "room-plain-compose-runtime",
            "message_id": "msg-plain-compose",
            "text": "确认生成",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
        }

        with patch(
            "plugins.AITool.services.lanchat_scene_runtime.get_lanchat_scene_runtime",
            return_value=FakeSceneRuntime(),
        ), patch.object(
            worker,
            "_execute_runtime_planning_compose",
            return_value=True,
        ) as runtime_compose, patch.object(
            worker,
            "_process_trigger",
            side_effect=AssertionError("legacy trigger must not be used"),
        ):
            handled = worker._handle_plain_chat_planning_gate(message, "确认生成")

        self.assertEqual(handled, "compose")
        runtime_compose.assert_called_once()
        trigger_arg, payload_arg, agent_name_arg = runtime_compose.call_args.args
        self.assertEqual(trigger_arg["room_id"], "room-plain-compose-runtime")
        self.assertEqual(trigger_arg["agent_name"], "山贼")
        self.assertEqual(trigger_arg["target_agent_name"], "山贼")
        self.assertIn("强盗藏宝室", payload_arg)
        self.assertEqual(agent_name_arg, "山贼")

    def test_plain_planning_gate_compose_blocks_legacy_when_runtime_fails(self) -> None:
        class FakeSceneRuntime:
            def handle_pending_planning_message(self, text: str):
                return (
                    "compose",
                    "用户确认开始生成：强盗藏宝室\n建议物体清单：藏宝箱、金币堆、火把",
                    "山贼",
                )

        engine = _FakeReplyEngine()
        worker = _TestWorker(corona_engine=engine, agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        message = {
            "room_id": "room-plain-compose-blocked",
            "message_id": "msg-plain-compose-blocked",
            "text": "确认生成",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
        }

        with patch(
            "plugins.AITool.services.lanchat_scene_runtime.get_lanchat_scene_runtime",
            return_value=FakeSceneRuntime(),
        ), patch.object(
            worker,
            "_execute_runtime_planning_compose",
            return_value=False,
        ), patch.object(
            worker,
            "_process_trigger",
            side_effect=AssertionError("legacy trigger must not be used"),
        ):
            handled = worker._handle_plain_chat_planning_gate(message, "确认生成")

        self.assertEqual(handled, "compose")
        self.assertEqual(len(engine.replies), 1)
        self.assertIn("AgentRuntime", engine.replies[0]["text"])
        self.assertIn("旧生成链路已关闭", engine.replies[0]["text"])

    def test_structured_planning_gate_compose_uses_agent_runtime_not_legacy_trigger(self) -> None:
        class FakeSceneRuntime:
            def handle_targeted_planning_message(
                self,
                target: str,
                text: str,
                *,
                draft_action: str = "",
                source_context_agent: str = "",
            ):
                return (
                    "compose",
                    "用户确认开始生成：商会主交易厅\n建议物体清单：交易桌、徽记背景、等候长椅",
                    "商人",
                )

        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        message = {
            "room_id": "room-structured-compose-runtime",
            "message_id": "msg-structured-compose",
            "text": "确认生成",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
        }
        metadata = {
            "draft_action": "generate",
            "target_agent_id": "merchant",
            "target_agent_name": "商人",
        }

        with patch(
            "plugins.AITool.services.lanchat_scene_runtime.get_lanchat_scene_runtime",
            return_value=FakeSceneRuntime(),
        ), patch.object(
            worker,
            "_execute_runtime_planning_compose",
            return_value=True,
        ) as runtime_compose, patch.object(
            worker,
            "_process_trigger",
            side_effect=AssertionError("legacy trigger must not be used"),
        ):
            handled = worker._handle_structured_planning_gate(message, "确认生成", metadata)

        self.assertEqual(handled, "planning_compose")
        runtime_compose.assert_called_once()
        trigger_arg, payload_arg, agent_name_arg = runtime_compose.call_args.args
        self.assertEqual(trigger_arg["room_id"], "room-structured-compose-runtime")
        self.assertEqual(trigger_arg["agent_id"], "merchant")
        self.assertEqual(trigger_arg["agent_name"], "商人")
        self.assertIn("商会主交易厅", payload_arg)
        self.assertEqual(agent_name_arg, "商人")

    def test_structured_planning_gate_compose_blocks_legacy_when_runtime_fails(self) -> None:
        class FakeSceneRuntime:
            def handle_targeted_planning_message(
                self,
                target: str,
                text: str,
                *,
                draft_action: str = "",
                source_context_agent: str = "",
            ):
                return (
                    "compose",
                    "用户确认开始生成：商会主交易厅\n建议物体清单：交易桌、徽记背景、等候长椅",
                    "商人",
                )

        engine = _FakeReplyEngine()
        worker = _TestWorker(corona_engine=engine, agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        message = {
            "room_id": "room-structured-compose-blocked",
            "message_id": "msg-structured-compose-blocked",
            "text": "确认生成",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
        }
        metadata = {
            "draft_action": "generate",
            "target_agent_id": "merchant",
            "target_agent_name": "商人",
        }

        with patch(
            "plugins.AITool.services.lanchat_scene_runtime.get_lanchat_scene_runtime",
            return_value=FakeSceneRuntime(),
        ), patch.object(
            worker,
            "_execute_runtime_planning_compose",
            return_value=False,
        ), patch.object(
            worker,
            "_process_trigger",
            side_effect=AssertionError("legacy trigger must not be used"),
        ):
            handled = worker._handle_structured_planning_gate(message, "确认生成", metadata)

        self.assertEqual(handled, "planning_compose")
        self.assertEqual(len(engine.replies), 1)
        self.assertIn("AgentRuntime", engine.replies[0]["text"])
        self.assertIn("旧生成链路已关闭", engine.replies[0]["text"])

    def test_agent_trigger_planning_gate_compose_uses_agent_runtime_not_free_agent(self) -> None:
        class FakeSceneRuntime:
            def handle_targeted_planning_message(
                self,
                target: str,
                text: str,
                *,
                draft_action: str = "",
                source_context_agent: str = "",
            ):
                return (
                    "compose",
                    "用户确认开始生成：强盗藏宝室\n建议物体清单：藏宝箱、金币堆、火把",
                    "山贼",
                )

            def handle_pending_planning_message(self, text: str):
                return "pass", None, None

        worker = _TestWorker(
            corona_engine=_FakeReplyEngine(),
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        trigger = {
            "room_id": "room-agent-trigger-compose-runtime",
            "message_id": "msg-agent-trigger-compose",
            "text": "确认生成",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "bandit",
            "agent_name": "山贼",
        }

        with patch(
            "plugins.AITool.services.lanchat_scene_runtime.get_lanchat_scene_runtime",
            return_value=FakeSceneRuntime(),
        ), patch.object(
            worker,
            "_execute_runtime_planning_compose",
            return_value=True,
        ) as runtime_compose, patch.object(
            worker,
            "_run_agent",
            side_effect=AssertionError("free agent path must not be used"),
        ):
            handled = worker._process_trigger(trigger)

        self.assertTrue(handled)
        runtime_compose.assert_called_once()
        self.assertEqual(len(worker._corona_engine.replies), 0)

    def test_agent_trigger_planning_gate_rejects_untyped_planning_reply(self) -> None:
        class FakeSceneRuntime:
            def handle_targeted_planning_message(
                self,
                target: str,
                text: str,
                *,
                draft_action: str = "",
                source_context_agent: str = "",
            ):
                return (
                    "reply",
                    "整理后的强盗藏宝室方案：中央藏宝箱，两侧武器架，入口火把，后方暗门。",
                    "山贼",
                )

            def handle_pending_planning_message(self, text: str):
                return "pass", None, None

        engine = _FakeReplyEngine()
        worker = _TestWorker(corona_engine=engine, agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        trigger = {
            "room_id": "room-agent-trigger-reply-runtime-context",
            "message_id": "msg-agent-trigger-reply-runtime-context",
            "text": "整理强盗藏宝室方案",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "bandit",
            "agent_name": "山贼",
        }

        with patch(
            "plugins.AITool.services.lanchat_scene_runtime.get_lanchat_scene_runtime",
            return_value=FakeSceneRuntime(),
        ), patch.object(
            worker,
            "_run_agent",
            side_effect=AssertionError("planning reply must not fall through to free agent"),
        ):
            handled = worker._process_trigger(trigger)

        self.assertTrue(handled)
        self.assertEqual(len(engine.replies), 1)
        self.assertIn("未通过强类型契约校验", engine.replies[0]["text"])
        state = worker._agent_runtime.state.snapshot("room-agent-trigger-reply-runtime-context")["room"]
        self.assertEqual(state["scene_plans"], {})
        self.assertFalse(state["active_plan_id"])
        self.assertNotIn("runtime_plan_context_updated", worker._agent_runtime.operation_log.events())

    def test_agent_trigger_planning_gate_compose_blocks_free_agent_when_runtime_fails(self) -> None:
        class FakeSceneRuntime:
            def handle_targeted_planning_message(
                self,
                target: str,
                text: str,
                *,
                draft_action: str = "",
                source_context_agent: str = "",
            ):
                return (
                    "compose",
                    "用户确认开始生成：强盗藏宝室\n建议物体清单：藏宝箱、金币堆、火把",
                    "山贼",
                )

            def handle_pending_planning_message(self, text: str):
                return "pass", None, None

        engine = _FakeReplyEngine()
        worker = _TestWorker(corona_engine=engine, agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        trigger = {
            "room_id": "room-agent-trigger-compose-blocked",
            "message_id": "msg-agent-trigger-compose-blocked",
            "text": "确认生成",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "bandit",
            "agent_name": "山贼",
        }

        with patch(
            "plugins.AITool.services.lanchat_scene_runtime.get_lanchat_scene_runtime",
            return_value=FakeSceneRuntime(),
        ), patch.object(
            worker,
            "_execute_runtime_planning_compose",
            return_value=False,
        ), patch.object(
            worker,
            "_run_agent",
            side_effect=AssertionError("free agent path must not be used"),
        ):
            handled = worker._process_trigger(trigger)

        self.assertTrue(handled)
        self.assertEqual(len(engine.replies), 1)
        self.assertIn("AgentRuntime", engine.replies[0]["text"])
        self.assertIn("旧生成链路已关闭", engine.replies[0]["text"])

    def test_lanchat_worker_routes_runtime_bridge_facts_through_handle_message(self) -> None:
        source = (
            REPO_ROOT
            / "editor"
            / "plugins"
            / "AITool"
            / "services"
            / "lanchat_agent_worker.py"
        ).read_text(encoding="utf-8")

        forbidden_runtime_internal_calls = (
            "_agent_runtime.sync_external_plan_context(",
            "_agent_runtime.record_sync_event(",
            "_agent_runtime.record_user_context_message(",
            "_agent_runtime.record_agent_context_message(",
            "_agent_runtime.apply_runtime_command(",
            "_agent_runtime.generate_report(",
            "_agent_runtime.provider_status(",
            "_agent_runtime.operation_replay(",
            "_agent_runtime.tool_manifest(",
            "_agent_runtime.status_summary(",
            "_agent_runtime.user_visible_events(",
            "_agent_runtime.drain_tool_graph_queue(",
            "_agent_runtime.scene_plans",
            "_agent_runtime.batch_plans",
            "_agent_runtime.plan_patches",
        )
        for forbidden_call in forbidden_runtime_internal_calls:
            self.assertNotIn(
                forbidden_call,
                source,
                msg=f"LANChat bridge must route {forbidden_call} through AgentRuntime.handle_message()",
            )
        self.assertIn('action="plan"', source)
        self.assertIn("runtime_sync_event", source)

    def test_lanchat_worker_handle_message_literal_actions_are_policy_classified(self) -> None:
        source_path = (
            REPO_ROOT
            / "editor"
            / "plugins"
            / "AITool"
            / "services"
            / "lanchat_agent_worker.py"
        )
        module = ast.parse(source_path.read_text(encoding="utf-8"))

        def literal_actions(node: ast.AST) -> list[str]:
            if isinstance(node, ast.Constant) and isinstance(node.value, str):
                return [node.value]
            if isinstance(node, ast.IfExp):
                return literal_actions(node.body) + literal_actions(node.orelse)
            return []

        def function_action_assignments(function: ast.FunctionDef) -> dict[str, set[str]]:
            assigned: dict[str, set[str]] = {}
            dict_values: dict[str, set[str]] = {}
            for node in ast.walk(function):
                if not isinstance(node, ast.Assign):
                    continue
                target_names = [target.id for target in node.targets if isinstance(target, ast.Name)]
                if not target_names:
                    continue
                if isinstance(node.value, ast.Dict):
                    values = {
                        value.value
                        for value in node.value.values
                        if isinstance(value, ast.Constant) and isinstance(value.value, str)
                    }
                    for target_name in target_names:
                        dict_values.setdefault(target_name, set()).update(values)
                    continue
                values = set(literal_actions(node.value))
                if (
                    not values
                    and isinstance(node.value, ast.Call)
                    and isinstance(node.value.func, ast.Attribute)
                    and node.value.func.attr == "get"
                    and isinstance(node.value.func.value, ast.Name)
                ):
                    values = set(dict_values.get(node.value.func.value.id, set()))
                if values:
                    for target_name in target_names:
                        assigned.setdefault(target_name, set()).update(values)
            return assigned

        actions: set[str] = set()
        unresolved_dynamic_actions: list[str] = []
        for function in (node for node in ast.walk(module) if isinstance(node, ast.FunctionDef)):
            assignments = function_action_assignments(function)
            for call in (node for node in ast.walk(function) if isinstance(node, ast.Call)):
                if not isinstance(call.func, ast.Attribute) or call.func.attr != "handle_message":
                    continue
                for keyword in call.keywords:
                    if keyword.arg != "action":
                        continue
                    if isinstance(keyword.value, ast.Name):
                        values = assignments.get(keyword.value.id, set())
                        if values:
                            actions.update(values)
                        else:
                            unresolved_dynamic_actions.append(
                                f"{function.name}:{keyword.value.id}:line{keyword.value.lineno}"
                            )
                    else:
                        actions.update(literal_actions(keyword.value))

        self.assertIn("plan", actions)
        self.assertIn("runtime_sync_event", actions)
        self.assertIn("confirm_and_enqueue", actions)
        self.assertIn("worker_drain", actions)
        self.assertIn("provider_status", actions)
        self.assertIn("post_generation_add", actions)
        self.assertIn("intervention_add", actions)
        self.assertEqual(unresolved_dynamic_actions, [])

        unknown_actions = sorted(
            action
            for action in actions
            if AgentRuntime.message_action_policy(action)["category"] == "unknown"
        )
        self.assertEqual(unknown_actions, [])

    def test_lanchat_worker_operation_log_appends_are_limited_to_audit_events(self) -> None:
        source_path = (
            REPO_ROOT
            / "editor"
            / "plugins"
            / "AITool"
            / "services"
            / "lanchat_agent_worker.py"
        )
        module = ast.parse(source_path.read_text(encoding="utf-8"))
        offenders: list[str] = []

        for function in (node for node in ast.walk(module) if isinstance(node, ast.FunctionDef)):
            for call in (node for node in ast.walk(function) if isinstance(node, ast.Call)):
                if not isinstance(call.func, ast.Attribute) or call.func.attr != "append":
                    continue
                owner = ast.unparse(call.func.value) if hasattr(ast, "unparse") else ""
                if not owner.endswith(".operation_log"):
                    continue
                if function.name not in {
                    "_record_model_call_summary",
                    "_record_unapproved_confirmed_action_block",
                } and "send" not in function.name:
                    offenders.append(f"{function.name}:line{call.lineno}")

        self.assertEqual(
            offenders,
            [],
            "LANChat worker may append OperationLog only for narrow audit events; "
            "runtime state changes must go through AgentRuntime.handle_message() or Runtime methods.",
        )

    def test_lanchat_worker_exception_reason_payloads_are_sanitized(self) -> None:
        source_path = (
            REPO_ROOT
            / "editor"
            / "plugins"
            / "AITool"
            / "services"
            / "lanchat_agent_worker.py"
        )
        module = ast.parse(source_path.read_text(encoding="utf-8"))
        offenders: list[str] = []

        for function in (node for node in ast.walk(module) if isinstance(node, ast.FunctionDef)):
            for dict_node in (node for node in ast.walk(function) if isinstance(node, ast.Dict)):
                for key, value in zip(dict_node.keys, dict_node.values):
                    if not isinstance(key, ast.Constant) or key.value != "reason":
                        continue
                    if (
                        isinstance(value, ast.Call)
                        and isinstance(value.func, ast.Name)
                        and value.func.id == "str"
                        and value.args
                        and isinstance(value.args[0], ast.Name)
                        and value.args[0].id == "exc"
                    ):
                        offenders.append(f"{function.name}:line{value.lineno}")

        self.assertEqual(
            offenders,
            [],
            "LANChat worker bridge payloads must not expose raw exception text in reason; "
            "use stable reason tokens plus error_type.",
        )

    def test_lanchat_worker_exception_surfaces_do_not_stringify_raw_exception(self) -> None:
        source_path = (
            REPO_ROOT
            / "editor"
            / "plugins"
            / "AITool"
            / "services"
            / "lanchat_agent_worker.py"
        )
        source = source_path.read_text(encoding="utf-8")
        module = ast.parse(source)
        offenders: list[str] = []

        if "str(exc)" in source:
            offenders.append("source:str(exc)")
        if "exc_info=True" in source:
            offenders.append("source:exc_info=True")

        for function in (node for node in ast.walk(module) if isinstance(node, ast.FunctionDef)):
            for node in ast.walk(function):
                if isinstance(node, ast.Call):
                    if (
                        isinstance(node.func, ast.Name)
                        and node.func.id == "str"
                        and node.args
                        and isinstance(node.args[0], ast.Name)
                        and node.args[0].id == "exc"
                    ):
                        offenders.append(f"{function.name}:str(exc):line{node.lineno}")
                    if (
                        isinstance(node.func, ast.Attribute)
                        and node.func.attr in {"debug", "info", "warning", "error", "exception"}
                    ):
                        for arg in node.args:
                            if isinstance(arg, ast.Name) and arg.id == "exc":
                                offenders.append(f"{function.name}:logger_raw_exc:line{arg.lineno}")
                    for keyword in node.keywords:
                        if keyword.arg == "exc_info" and isinstance(keyword.value, ast.Constant) and keyword.value.value is True:
                            offenders.append(f"{function.name}:exc_info:line{keyword.value.lineno}")
                if isinstance(node, ast.JoinedStr):
                    for value in node.values:
                        if (
                            isinstance(value, ast.FormattedValue)
                            and isinstance(value.value, ast.Name)
                            and value.value.id == "exc"
                        ):
                            offenders.append(f"{function.name}:fstring_exc:line{node.lineno}")

        self.assertEqual(
            offenders,
            [],
            "LANChat worker must not stringify raw exceptions or emit traceback logging flags; "
            "use stable safe messages plus error_type/exc_type.",
        )

    def test_host_action_executor_exception_surfaces_are_sanitized(self) -> None:
        source_path = (
            REPO_ROOT
            / "editor"
            / "plugins"
            / "AITool"
            / "services"
            / "lanchat_host_action_executor.py"
        )
        source = source_path.read_text(encoding="utf-8")
        module = ast.parse(source)
        offenders: list[str] = []

        if "exc_info=True" in source:
            offenders.append("source:exc_info=True")

        str_exc_occurrences = source.count("str(exc)")
        if str_exc_occurrences != 1 or "text = str(exc)" not in source:
            offenders.append(
                "source:str(exc) must appear only in _safe_agent_error_text internal classification"
            )

        for function in (node for node in ast.walk(module) if isinstance(node, ast.FunctionDef)):
            for call in (node for node in ast.walk(function) if isinstance(node, ast.Call)):
                if (
                    isinstance(call.func, ast.Name)
                    and call.func.id == "str"
                    and call.args
                    and isinstance(call.args[0], ast.Name)
                    and call.args[0].id == "exc"
                    and function.name != "_safe_agent_error_text"
                ):
                    offenders.append(f"{function.name}:str(exc):line{call.lineno}")
                if (
                    isinstance(call.func, ast.Attribute)
                    and call.func.attr in {"debug", "info", "warning", "error", "exception"}
                ):
                    for arg in call.args:
                        if isinstance(arg, ast.Name) and arg.id == "exc":
                            offenders.append(f"{function.name}:logger_raw_exc:line{arg.lineno}")

                for keyword in call.keywords:
                    if keyword.arg == "exc_info" and isinstance(keyword.value, ast.Constant) and keyword.value.value is True:
                        offenders.append(f"{function.name}:exc_info:line{keyword.value.lineno}")

                if isinstance(call.func, ast.Name) and call.func.id == "_result_payload":
                    for arg in call.args:
                        if (
                            isinstance(arg, ast.Call)
                            and isinstance(arg.func, ast.Name)
                            and arg.func.id == "str"
                            and arg.args
                            and isinstance(arg.args[0], ast.Name)
                            and arg.args[0].id == "exc"
                        ):
                            offenders.append(f"{function.name}:result_payload_str_exc:line{arg.lineno}")

            for dict_node in (node for node in ast.walk(function) if isinstance(node, ast.Dict)):
                for key, value in zip(dict_node.keys, dict_node.values):
                    if not isinstance(key, ast.Constant) or key.value not in {"message", "reason"}:
                        continue
                    if (
                        isinstance(value, ast.Call)
                        and isinstance(value.func, ast.Name)
                        and value.func.id == "str"
                        and value.args
                        and isinstance(value.args[0], ast.Name)
                        and value.args[0].id == "exc"
                    ):
                        offenders.append(f"{function.name}:{key.value}_str_exc:line{value.lineno}")

        self.assertEqual(
            offenders,
            [],
            "HostActionExecutor must not expose raw exception text or traceback details; "
            "return stable safe messages and log only exception type.",
        )

    def test_explicit_legacy_flags_allow_existing_generation_path_for_transition(self) -> None:
        coordinator = _FakeCoordinator()
        flags = AgentRuntimeFlags.from_env(
            {
                "AGENT_RUNTIME_ENABLED": "1",
                "OLD_WORKFLOW_DIRECT_ENTRY_DISABLED": "0",
                "ALLOW_LEGACY_MAIN_WORKFLOW": "1",
            }
        )
        worker = _TestWorker(agent_runtime_flags=flags)

        reply = worker._start_active_coordinator_generation(
            coordinator,
            room_id="room-1",
            host_id="房主",
        )

        self.assertIn("SeedPlan seed-test 已进入生成队列", reply or "")
        self.assertEqual(coordinator.execute_calls, ["seed-test"])

    def test_host_action_structured_seed_plan_routes_to_agent_runtime_by_default(self) -> None:
        coordinator = _FakeCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        executor = worker._get_host_action_executor()

        result = executor.enqueue_and_process({
            "action_type": "start_generation",
            "plan_id": "seed-host-action",
            "room_id": "room-host-action",
            "intent_text": "做一个藏宝室，有宝箱和火把",
            "source_user_id": "房主",
        })

        self.assertIsNotNone(result)
        self.assertTrue(result.ok)
        self.assertEqual(coordinator.action_payload_calls, [])
        self.assertEqual(coordinator.execute_calls, [])
        state = worker._agent_runtime.query_state("room-host-action")["room"]
        self.assertEqual(state["external_plan_links"]["seed-host-action"], state["active_plan_id"])
        self.assertTrue(state["tool_graphs"])

    def test_host_action_structured_external_plan_id_routes_to_agent_runtime_by_default(self) -> None:
        agent_calls: list[tuple[str, list[str]]] = []

        def forbidden_agent(persona: str, messages: list[str]) -> str:
            agent_calls.append((persona, messages))
            return "legacy agent should not run"

        coordinator = _FakeCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_factory=lambda: forbidden_agent,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        executor = worker._get_host_action_executor()

        result = executor.enqueue_and_process({
            "action_type": "start_generation",
            "external_plan_id": "seed-host-external-action",
            "room_id": "room-host-external-action",
            "intent_text": "做一个藏宝室，有宝箱和火把",
            "source_user_id": "房主",
        })

        self.assertIsNotNone(result)
        self.assertTrue(result.ok)
        self.assertEqual(agent_calls, [])
        self.assertEqual(coordinator.action_payload_calls, [])
        self.assertEqual(coordinator.execute_calls, [])
        state = worker._agent_runtime.query_state("room-host-external-action")["room"]
        self.assertEqual(state["external_plan_links"]["seed-host-external-action"], state["active_plan_id"])
        self.assertTrue(state["tool_graphs"])

    def test_host_action_post_generation_add_reports_runtime_patch_facts(self) -> None:
        coordinator = _FakeCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._agent_runtime.handle_message(
            room_id="room-host-post-add",
            external_plan_id="seed-host-post-add",
            text="强盗藏宝室方案：中央宝箱，两侧武器架，入口火把",
            owner_agent="商人",
            action="plan",
        )
        executor = worker._get_host_action_executor()

        result = executor.enqueue_and_process({
            "action_type": "post_generation_add",
            "plan_id": "seed-host-post-add",
            "room_id": "room-host-post-add",
            "intent_text": "追加一个天使雕像",
            "source_user_id": "房主",
        })

        self.assertIsNotNone(result)
        self.assertTrue(result.ok)
        self.assertEqual(coordinator.action_payload_calls, [])
        self.assertEqual(coordinator.execute_calls, [])
        self.assertIn("AgentRuntime 介入结果", result.message)
        self.assertIn("post-generation-add", result.message)
        self.assertIn("状态", result.message)
        self.assertIn("对象", result.message)
        self.assertNotIn("AgentRuntime 执行结果。", result.message)
        state = worker._agent_runtime.query_state("room-host-post-add")["room"]
        self.assertTrue(state["pending_interventions"])
        patch = next(iter(state["pending_interventions"].values()))
        self.assertEqual(patch["patch_type"], "post_generation_add")
        self.assertIn("天使雕像", patch["items"])

    def test_host_action_structured_success_result_is_sanitized(self) -> None:
        engine = _FakeIdleEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        executor = worker._get_host_action_executor()

        def unsafe_structured_handler(payload: dict[str, Any]) -> str:
            return (
                "场景已完成 provider=https://secret.invalid "
                "prompt=hidden E:/private/model.glb api_key=SECRET"
            )

        executor._structured_action_handler = unsafe_structured_handler
        result = executor.enqueue_and_process({
            "action_type": "start_generation",
            "plan_id": "seed-host-success-sanitize",
            "room_id": "room-host-success-sanitize",
            "intent_text": "做一个藏宝室，有宝箱和火把",
            "source_user_id": "房主",
            "proposal_id": "gm-host-success-sanitize",
        })

        self.assertIsNotNone(result)
        self.assertTrue(result.ok)
        visible = result.message + " " + str(result.payload) + " " + " ".join(
            message["text"] for message in engine.system_messages
        )
        self.assertIn("场景已完成", visible)
        self.assertNotIn("https://secret.invalid", visible)
        self.assertNotIn("prompt=hidden", visible)
        self.assertNotIn("E:/private", visible)
        self.assertNotIn("api_key", visible)

    def test_host_action_structured_failed_result_is_sanitized(self) -> None:
        engine = _FakeIdleEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        executor = worker._get_host_action_executor()

        def unsafe_structured_handler(payload: dict[str, Any]) -> str:
            return (
                "failed: provider=https://secret.invalid "
                "prompt=hidden E:/private/failure.json token=SECRET"
            )

        executor._structured_action_handler = unsafe_structured_handler
        result = executor.enqueue_and_process({
            "action_type": "start_generation",
            "plan_id": "seed-host-failed-sanitize",
            "room_id": "room-host-failed-sanitize",
            "intent_text": "做一个藏宝室，有宝箱和火把",
            "source_user_id": "房主",
            "proposal_id": "gm-host-failed-sanitize",
        })

        self.assertIsNotNone(result)
        self.assertFalse(result.ok)
        visible = result.message + " " + str(result.payload) + " " + " ".join(
            message["text"] for message in engine.system_messages
        )
        self.assertIn("failed", visible)
        self.assertNotIn("https://secret.invalid", visible)
        self.assertNotIn("prompt=hidden", visible)
        self.assertNotIn("E:/private", visible)
        self.assertNotIn("token=", visible)

    def test_host_action_structured_unknown_action_is_rejected_without_agent_fallback(self) -> None:
        coordinator = _FakeCoordinator()
        agent_calls: list[tuple[str, list[str]]] = []

        def forbidden_agent(persona: str, messages: list[str]) -> str:
            agent_calls.append((persona, messages))
            return "legacy agent should not run"

        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_factory=lambda: forbidden_agent,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        executor = worker._get_host_action_executor()

        result = executor.enqueue_and_process({
            "action_type": "legacy_freeform_scene_write",
            "plan_id": "seed-unsupported-host-action",
            "room_id": "room-unsupported-host-action",
            "intent_text": "直接按旧链路改场景",
            "source_user_id": "房主",
        })

        self.assertIsNotNone(result)
        self.assertFalse(result.ok)
        self.assertEqual(agent_calls, [])
        self.assertEqual(coordinator.action_payload_calls, [])
        self.assertEqual(coordinator.execute_calls, [])
        self.assertIn("AgentRuntime 受控入口", result.message)

    def test_host_action_freeform_agent_fallback_is_disabled_by_default(self) -> None:
        coordinator = _FakeCoordinator()
        agent_calls: list[tuple[str, list[str]]] = []

        def forbidden_agent(persona: str, messages: list[str]) -> str:
            agent_calls.append((persona, messages))
            return "legacy agent should not run"

        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_factory=lambda: forbidden_agent,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        executor = worker._get_host_action_executor()

        result = executor.enqueue_and_process({
            "proposal_id": "gm-freeform-fallback",
            "intent_text": "按旧链路直接调整场景",
            "source_user_id": "房主",
        })

        self.assertIsNotNone(result)
        self.assertFalse(result.ok)
        self.assertEqual(agent_calls, [])
        self.assertEqual(coordinator.action_payload_calls, [])
        self.assertEqual(coordinator.execute_calls, [])
        self.assertIn("非结构化确认动作", result.message)
        self.assertIn("AgentRuntime 受控入口", result.message)

    def test_host_action_freeform_agent_fallback_requires_explicit_legacy_main_workflow_flag(self) -> None:
        agent_calls: list[tuple[str, list[str]]] = []

        def legacy_agent(persona: str, messages: list[str]) -> str:
            agent_calls.append((persona, messages))
            return "legacy scene delta executed"

        flags = AgentRuntimeFlags.from_env({
            "AGENT_RUNTIME_ENABLED": "1",
            "OLD_WORKFLOW_DIRECT_ENTRY_DISABLED": "0",
            "ALLOW_LEGACY_MAIN_WORKFLOW": "1",
        })
        worker = _TestWorker(
            agent_factory=lambda: legacy_agent,
            agent_runtime_flags=flags,
        )
        executor = worker._get_host_action_executor()

        result = executor.enqueue_and_process({
            "proposal_id": "gm-freeform-legacy",
            "intent_text": "按旧链路直接调整场景",
            "source_user_id": "房主",
        })

        self.assertIsNotNone(result)
        self.assertTrue(result.ok)
        self.assertEqual(len(agent_calls), 1)
        self.assertIn("legacy scene delta executed", result.message)

    def test_host_action_engine_gate_failure_does_not_leak_internal_exception_text(self) -> None:
        class FailingGate:
            def run(self, fn, payload):  # noqa: ANN001
                raise RuntimeError(
                    "provider=https://secret.invalid prompt=hidden E:/private/host.json api_key=SECRET"
                )

        engine = _FakeIdleEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        executor = worker._get_host_action_executor()
        executor._engine_gate = FailingGate()

        result = executor.enqueue_and_process({
            "action_type": "start_generation",
            "plan_id": "seed-host-gate-error",
            "room_id": "room-host-gate-error",
            "intent_text": "做一个藏宝室，有宝箱和火把",
            "source_user_id": "房主",
            "proposal_id": "gm-host-gate-error",
        })

        self.assertIsNotNone(result)
        self.assertFalse(result.ok)
        visible = result.message + " " + str(result.payload) + " " + " ".join(
            message["text"] for message in engine.system_messages
        )
        self.assertIn("执行失败", visible)
        self.assertNotIn("https://secret.invalid", visible)
        self.assertNotIn("prompt=hidden", visible)
        self.assertNotIn("E:/private", visible)
        self.assertNotIn("api_key", visible)

    def test_host_action_legacy_agent_failure_does_not_leak_internal_exception_text(self) -> None:
        def failing_legacy_agent(persona: str, messages: list[str]) -> str:
            raise RuntimeError(
                "provider=https://secret.invalid prompt=hidden E:/private/legacy.json token=SECRET"
            )

        flags = AgentRuntimeFlags.from_env({
            "AGENT_RUNTIME_ENABLED": "1",
            "OLD_WORKFLOW_DIRECT_ENTRY_DISABLED": "0",
            "ALLOW_LEGACY_MAIN_WORKFLOW": "1",
        })
        engine = _FakeIdleEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_factory=lambda: failing_legacy_agent,
            agent_runtime_flags=flags,
        )
        executor = worker._get_host_action_executor()

        result = executor.enqueue_and_process({
            "proposal_id": "gm-freeform-legacy-error",
            "intent_text": "按旧链路直接调整场景",
            "source_user_id": "房主",
        })

        self.assertIsNotNone(result)
        self.assertFalse(result.ok)
        visible = result.message + " " + str(result.payload) + " " + " ".join(
            message["text"] for message in engine.system_messages
        )
        self.assertIn("暂时无法响应", visible)
        self.assertNotIn("https://secret.invalid", visible)
        self.assertNotIn("prompt=hidden", visible)
        self.assertNotIn("E:/private", visible)
        self.assertNotIn("token=", visible)

    def test_host_action_visible_status_and_result_send_are_audited(self) -> None:
        coordinator = _FakeCoordinator()
        engine = _FakeIdleEngine()
        worker = _TestWorker(
            corona_engine=engine,
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        executor = worker._get_host_action_executor()

        with patch.object(
            worker._agent_runtime,
            "query_state",
            side_effect=AssertionError("host action send audit must not pre-read RuntimeState"),
        ):
            result = executor.enqueue_and_process({
                "action_type": "start_generation",
                "plan_id": "seed-host-action-audit",
                "room_id": "room-host-action-audit",
                "intent_text": "做一个藏宝室，有宝箱和火把",
                "source_user_id": "房主",
                "proposal_id": "gm-host-action-audit",
            })

        self.assertIsNotNone(result)
        self.assertTrue(result.ok)
        self.assertGreaterEqual(len(engine.system_messages), 3)
        self.assertTrue(all(message["message_kind"] == "action_status" for message in engine.system_messages))
        self.assertGreaterEqual(len(engine.intent_broadcasts), 3)
        events = worker._agent_runtime.operation_log.events()
        self.assertIn("host_action_message_send_requested", events)
        self.assertIn("host_action_message_send_succeeded", events)
        entries = worker._agent_runtime.operation_log.query(event="host_action_message_send_succeeded")
        statuses = {entry.payload["status"] for entry in entries}
        self.assertIn("queued_host_action", statuses)
        self.assertIn("executing_host_action", statuses)
        self.assertIn("host_action_executed", statuses)
        self.assertIn("executed", statuses)
        status_entries = [
            entry
            for entry in entries
            if entry.payload["message_kind"] == "action_status"
        ]
        self.assertTrue(status_entries)
        for entry in status_entries:
            self.assertEqual(entry.payload["message_kind"], "action_status")
            self.assertEqual(entry.payload["proposal_id"], "gm-host-action-audit")
            self.assertTrue(entry.payload["sent"])
            self.assertNotIn("tool_name", entry.payload)
            self.assertNotIn("provider", entry.payload)
            self.assertNotIn("prompt", entry.payload)
        intent_entries = [
            entry
            for entry in entries
            if entry.payload["message_kind"] == "intent_broadcast"
        ]
        self.assertTrue(intent_entries)
        intent_statuses = {entry.payload["status"] for entry in intent_entries}
        self.assertIn("queued_host_action", intent_statuses)
        self.assertIn("executing_host_action", intent_statuses)
        self.assertIn("host_action_executed", intent_statuses)
        self.assertTrue(all(entry.payload["channel"] == "host_action_intent_broadcast" for entry in intent_entries))
        state = worker._agent_runtime.state.snapshot("room-host-action-audit")["room"]
        runtime_plan_id = state["external_plan_links"]["seed-host-action-audit"]
        replay = worker._agent_runtime.operation_replay(
            room_id="room-host-action-audit",
            plan_id=runtime_plan_id,
        )
        replay_events = [entry["event"] for entry in replay["entries"]]
        self.assertIn("host_action_message_send_requested", replay_events)
        self.assertIn("host_action_message_send_succeeded", replay_events)
        replay_statuses = {
            entry["payload"]["status"]
            for entry in replay["entries"]
            if entry["event"] == "host_action_message_send_succeeded"
        }
        self.assertIn("host_action_executed", replay_statuses)
        self.assertIn("executed", replay_statuses)

    def test_confirmed_action_payload_preparation_uses_runtime_payload_without_seed_plan_by_default(self) -> None:
        coordinator = _FakeCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        trigger = {
            "room_id": "room-action-payload-runtime",
            "message_id": "msg-action-payload-runtime",
            "sender_id": "房主",
            "sender_name": "房主",
            "agent_id": "agent-merchant",
            "agent_name": "商人",
            "text": "按照这个藏宝室方案生成",
        }
        payload = {
            "action_type": "start_generation",
            "status": "confirmed",
            "intent_text": "做一个强盗藏宝室，有宝箱、金币和火把",
            "source_user_id": "房主",
        }

        prepared = worker._prepare_confirmed_action_payload(payload, trigger)

        self.assertEqual(prepared["execution"], "agent_runtime_structured")
        self.assertTrue(prepared["runtime_payload_prepared_by_worker"])
        self.assertEqual(prepared["plan_id"], "planning:msg-action-payload-runtime")
        self.assertNotIn("seed_plan", prepared)
        self.assertEqual(coordinator.seed_plan_calls, [])
        self.assertEqual(coordinator.action_payload_calls, [])

        worker._broadcast_confirmed_action(prepared)
        self.assertEqual(coordinator.seed_plan_calls, [])
        self.assertEqual(coordinator.action_payload_calls, [])
        self.assertEqual(coordinator.execute_calls, [])
        state = worker._agent_runtime.query_state("room-action-payload-runtime")["room"]
        self.assertEqual(state["external_plan_links"]["planning:msg-action-payload-runtime"], state["active_plan_id"])
        self.assertTrue(state["tool_graphs"])

    def test_unapproved_confirmed_role_action_payload_is_blocked_by_default(self) -> None:
        coordinator = _FakeCoordinator()
        engine = _FakeIdleEngine()
        worker = _TestWorker(
            corona_engine=engine,
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        payload = {
            "action_type": "delete_actor",
            "status": "confirmed",
            "intent_text": "删除藏宝箱",
            "source_user_id": "房主",
        }

        self.assertIsNone(worker._filter_confirmed_action_payload_for_runtime(payload))
        worker._broadcast_confirmed_action(payload)

        self.assertEqual(engine.intent_broadcasts, [])
        self.assertEqual(engine.system_messages, [])
        self.assertEqual(coordinator.action_payload_calls, [])
        self.assertEqual(coordinator.execute_calls, [])
        audit_entries = worker._agent_runtime.operation_log.query(event="unapproved_confirmed_action_blocked")
        self.assertEqual(len(audit_entries), 2)
        phases = {entry.payload["phase"] for entry in audit_entries}
        self.assertEqual(phases, {"reply_metadata", "broadcast"})
        for entry in audit_entries:
            self.assertEqual(entry.payload["action_type"], "delete_actor")
            self.assertEqual(entry.payload["status"], "confirmed")
            self.assertFalse(entry.payload["runtime_payload_prepared_by_worker"])
            self.assertNotIn("intent_text", entry.payload)
            self.assertNotIn("prompt", entry.payload)
            self.assertNotIn("api_key", entry.payload)

    def test_forged_runtime_structured_action_payload_is_blocked_by_default(self) -> None:
        coordinator = _FakeCoordinator()
        engine = _FakeIdleEngine()
        worker = _TestWorker(
            corona_engine=engine,
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        payload = {
            "action_type": "start_generation",
            "status": "confirmed",
            "execution": "agent_runtime_structured",
            "plan_id": "seed-runtime-approved",
            "room_id": "room-runtime-approved",
            "intent_text": "做一个藏宝室",
            "source_user_id": "房主",
        }

        worker._broadcast_confirmed_action(payload)

        self.assertEqual(engine.intent_broadcasts, [])
        self.assertEqual(engine.system_messages, [])
        self.assertEqual(coordinator.action_payload_calls, [])
        self.assertEqual(coordinator.execute_calls, [])
        audit_entries = worker._agent_runtime.operation_log.query(event="unapproved_confirmed_action_blocked")
        self.assertEqual(len(audit_entries), 1)
        self.assertEqual(audit_entries[0].payload["phase"], "broadcast")
        self.assertEqual(audit_entries[0].payload["execution"], "agent_runtime_structured")
        self.assertEqual(audit_entries[0].payload["plan_id"], "seed-runtime-approved")
        self.assertFalse(audit_entries[0].payload["runtime_payload_prepared_by_worker"])

    def test_worker_prepared_runtime_structured_action_payload_can_broadcast_by_default(self) -> None:
        coordinator = _FakeCoordinator()
        engine = _FakeIdleEngine()
        worker = _TestWorker(
            corona_engine=engine,
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        trigger = {
            "room_id": "room-runtime-approved",
            "message_id": "msg-runtime-approved",
            "sender_id": "房主",
            "sender_name": "房主",
            "agent_id": "agent-merchant",
            "agent_name": "商人",
            "text": "按照这个藏宝室方案生成",
        }
        prepared = worker._prepare_confirmed_action_payload(
            {
                "action_type": "start_generation",
                "status": "confirmed",
                "intent_text": "做一个藏宝室",
                "source_user_id": "房主",
            },
            trigger,
        )

        worker._broadcast_confirmed_action(prepared)

        self.assertEqual(len(engine.intent_broadcasts), 1)
        self.assertTrue(engine.system_messages)
        self.assertIn("AgentRuntime", engine.system_messages[-1]["text"])
        self.assertIn("ScenePlan", engine.system_messages[-1]["text"])
        self.assertEqual(engine.system_messages[-1]["message_kind"], "action_status")
        state = worker._agent_runtime.query_state("room-runtime-approved")["room"]
        self.assertEqual(state["external_plan_links"]["planning:msg-runtime-approved"], state["active_plan_id"])
        self.assertTrue(state["tool_graphs"])
        worker._agent_runtime.handle_message(
            room_id="room-runtime-approved",
            text="worker drain",
            action="worker_drain",
            max_graphs=16,
        )
        result_entries = worker._agent_runtime.operation_log.query(event="tool_call_started")
        self.assertTrue([
            entry
            for entry in result_entries
            if entry.payload.get("tool_name") == "runtime.actor.import_batch"
            and entry.payload.get("runtime_guard") == "authorized"
        ])

    def test_explicit_legacy_flags_keep_host_action_structured_handler_for_transition(self) -> None:
        coordinator = _FakeCoordinator()
        flags = AgentRuntimeFlags.from_env(
            {
                "AGENT_RUNTIME_ENABLED": "1",
                "OLD_WORKFLOW_DIRECT_ENTRY_DISABLED": "0",
                "ALLOW_LEGACY_MAIN_WORKFLOW": "1",
            }
        )
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=flags,
        )
        executor = worker._get_host_action_executor()

        result = executor.enqueue_and_process({
            "action_type": "start_generation",
            "plan_id": "seed-host-action-legacy",
            "room_id": "room-host-action-legacy",
            "intent_text": "做一个藏宝室，有宝箱和火把",
            "source_user_id": "房主",
        })

        self.assertIsNotNone(result)
        self.assertTrue(result.ok)
        self.assertEqual(len(coordinator.action_payload_calls), 1)
        self.assertEqual(coordinator.action_payload_calls[0]["plan_id"], "seed-host-action-legacy")

    def test_completed_layout_adjustment_is_mirrored_to_agent_runtime(self) -> None:
        coordinator = _FakeCompletedCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        reply = worker._handle_coordinator_completed_intervention({
            "room_id": "room-completed",
            "text": "调整一下布局，模型浮空了",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
        })

        self.assertIn("已生成布局调整建议", reply or "")
        state = worker._agent_runtime.query_state("room-completed")["room"]
        runtime_plan_id = state["external_plan_links"]["seed-completed"]
        self.assertEqual(state["active_plan_id"], runtime_plan_id)
        self.assertGreaterEqual(len(state["layout_adjustment_proposals"]), 1)
        self.assertIn("scene_plan_created", worker._agent_runtime.operation_log.events())

    def test_layout_reflow_confirmation_defaults_to_agent_runtime_not_direct_actor_transform(self) -> None:
        coordinator = _FakeCompletedConfirmedCoordinator()
        worker = _LayoutDirectExecutionTrackingWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        with patch.object(
            worker._agent_runtime,
            "query_state",
            side_effect=AssertionError("layout confirmation bridge must not pre-read RuntimeState"),
        ):
            reply = worker._handle_coordinator_completed_intervention({
                "room_id": "room-layout-confirm",
                "text": "确认调整",
                "sender_id": "host-1",
                "sender_name": "房主",
                "sender_type": "host",
            })

        self.assertIn("布局调整建议已确认", reply or "")
        self.assertIn("AgentRuntime", reply or "")
        self.assertIn("AgentRuntime 布局结果", reply or "")
        self.assertIn("ScenePlan", reply or "")
        self.assertIn("ToolCallGraph", reply or "")
        self.assertIn("graph", reply or "")
        self.assertIn("应用", reply or "")
        self.assertIn("跳过", reply or "")
        self.assertIn("贴地", reply or "")
        self.assertIn("重叠修正", reply or "")
        self.assertNotIn("AgentRuntime 执行结果：已应用", reply or "")
        self.assertNotIn("纭", reply or "")
        self.assertEqual(worker.direct_layout_reflow_calls, 0)
        events = worker._agent_runtime.operation_log.events()
        self.assertIn("layout_adjustment_confirmed", events)
        self.assertIn("tool_call_succeeded", events)
        state = worker._agent_runtime.state.snapshot("room-layout-confirm")["room"]
        runtime_plan_id = state["external_plan_links"]["seed-completed"]
        self.assertIn(runtime_plan_id, state["layout_adjustment_proposals"])

    def test_explicit_legacy_flags_keep_layout_reflow_direct_transform_for_transition(self) -> None:
        coordinator = _FakeCompletedConfirmedCoordinator()
        flags = AgentRuntimeFlags.from_env(
            {
                "AGENT_RUNTIME_ENABLED": "1",
                "OLD_WORKFLOW_DIRECT_ENTRY_DISABLED": "0",
                "ALLOW_LEGACY_MAIN_WORKFLOW": "1",
            }
        )
        worker = _LayoutDirectExecutionTrackingWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=flags,
        )

        reply = worker._handle_coordinator_completed_intervention({
            "room_id": "room-layout-confirm-legacy",
            "text": "确认调整",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
        })

        self.assertIn("legacy layout reflow executed", reply or "")
        self.assertEqual(worker.direct_layout_reflow_calls, 1)

    def test_sync_layout_reflow_confirmation_defaults_to_agent_runtime(self) -> None:
        coordinator = _FakeCompletedConfirmedCoordinator()
        worker = _LayoutDirectExecutionTrackingWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        handled = worker.sync_chat_message_to_coordinator(
            {
                "room_id": "room-layout-sync-confirm",
                "message_id": "msg-layout-confirm",
                "text": "确认调整",
                "sender_id": "host-1",
                "sender_name": "房主",
                "sender_type": "host",
                "message_kind": "chat",
                "is_host": True,
            },
            source="lanchat_history_snapshot",
            emit_disclosure=False,
        )

        self.assertTrue(handled)
        self.assertEqual(worker.direct_layout_reflow_calls, 0)
        self.assertTrue(worker.coordinator_system_replies)
        self.assertIn("AgentRuntime", worker.coordinator_system_replies[-1])
        self.assertIn("layout_adjustment_confirmed", worker._agent_runtime.operation_log.events())

    def test_completed_final_adjustment_defaults_to_runtime_proposal_not_direct_actor_transform(self) -> None:
        coordinator = _FakeFinalAdjustmentCoordinator()
        worker = _LayoutDirectExecutionTrackingWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        with patch.object(
            worker._agent_runtime,
            "query_state",
            side_effect=AssertionError("final adjustment bridge must not pre-read RuntimeState"),
        ):
            reply = worker._handle_coordinator_completed_intervention({
                "room_id": "room-final-adjustment",
                "text": "把藏宝箱放大一点",
                "sender_id": "host-1",
                "sender_name": "房主",
                "sender_type": "host",
            })

        self.assertIn("已记录最终调整", reply or "")
        self.assertIn("AgentRuntime", reply or "")
        self.assertEqual(worker.direct_final_adjustment_calls, 0)
        state = worker._agent_runtime.state.snapshot("room-final-adjustment")["room"]
        runtime_plan_id = state["external_plan_links"]["seed-completed"]
        self.assertIn(runtime_plan_id, state["layout_adjustment_proposals"])

    def test_explicit_legacy_flags_keep_completed_final_adjustment_direct_transform_for_transition(self) -> None:
        coordinator = _FakeFinalAdjustmentCoordinator()
        flags = AgentRuntimeFlags.from_env(
            {
                "AGENT_RUNTIME_ENABLED": "1",
                "OLD_WORKFLOW_DIRECT_ENTRY_DISABLED": "0",
                "ALLOW_LEGACY_MAIN_WORKFLOW": "1",
            }
        )
        worker = _LayoutDirectExecutionTrackingWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=flags,
        )

        reply = worker._handle_coordinator_completed_intervention({
            "room_id": "room-final-adjustment-legacy",
            "text": "把藏宝箱放大一点",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
        })

        self.assertIn("legacy final adjustment executed", reply or "")
        self.assertEqual(worker.direct_final_adjustment_calls, 1)

    def test_final_adjustment_conflict_confirmation_is_mirrored_to_agent_runtime(self) -> None:
        coordinator = _FakeFinalAdjustmentConfirmCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._agent_runtime.handle_message(
            room_id="room-final-confirm-worker",
            text="做一个藏宝室，有宝箱和火把",
            sender_id="host-1",
            sender_name="房主",
            action="plan",
            external_plan_id="seed-completed",
        )

        worker._record_final_adjustment_confirmation({
            "action_type": "final_adjustment_confirmation",
            "proposal_id": "final-proposal-worker",
            "source_user_id": "房主",
            "decision": "confirm",
        })

        self.assertEqual(len(coordinator.confirm_calls), 1)
        state = worker._agent_runtime.query_state("room-final-confirm-worker")["room"]
        confirmations = state["final_adjustment_confirmations"]
        self.assertEqual(len(confirmations), 1)
        self.assertEqual(confirmations[0]["proposal_id"], "final-proposal-worker")
        self.assertEqual(confirmations[0]["decision"], "confirmed")
        self.assertIn("final_adjustment_confirmation_recorded", worker._agent_runtime.operation_log.events())

    def test_executing_intervention_is_mirrored_to_agent_runtime_pending_patch(self) -> None:
        coordinator = _FakeExecutingCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        handled = worker.sync_chat_message_to_coordinator(
            {
                "room_id": "room-executing",
                "message_id": "msg-intervention",
                "text": "再添加一个天使雕像",
                "sender_id": "host-1",
                "sender_name": "房主",
                "sender_type": "host",
                "message_kind": "chat",
                "is_host": True,
            },
            source="lanchat_history_snapshot",
            emit_disclosure=False,
        )

        self.assertTrue(handled)
        state = worker._agent_runtime.query_state("room-executing")["room"]
        runtime_plan_id = state["external_plan_links"]["seed-executing"]
        self.assertEqual(state["active_plan_id"], runtime_plan_id)
        pending = list(state["pending_interventions"].values())
        self.assertEqual(len(pending), 1)
        self.assertIn("天使雕像", pending[0]["items"])
        self.assertIn("plan_patch_recorded", worker._agent_runtime.operation_log.events())
        events = worker._agent_runtime.user_visible_events(
            room_id="room-executing",
            plan_id=runtime_plan_id,
        )
        intervention_events = [
            event for event in events if event["event_type"] == "intervention_recorded"
        ]
        self.assertTrue(intervention_events)
        event_text = str(intervention_events[-1])
        self.assertIn("天使雕像", intervention_events[-1]["message"])
        self.assertNotIn("patch_id", event_text)
        self.assertNotIn("tool_name", event_text)

    def test_runtime_executing_intervention_does_not_require_coordinator_active_plan(self) -> None:
        worker = _TestWorker(
            interaction_coordinator=_ExplodingCoordinator(),
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        plan = worker._agent_runtime.handle_message(
            room_id="room-runtime-executing",
            text="做一个强盗藏宝室，包含宝箱、金币和火把",
            sender_id="host-1",
            sender_name="房主",
            action="plan",
            external_plan_id="seed-runtime-executing",
        )["plan"]
        runtime_plan_id = plan["plan_id"]
        worker._agent_runtime.confirm_scene_plan(runtime_plan_id, confirmed_by="房主")
        worker._agent_runtime.enqueue_scene_plan(runtime_plan_id)

        with patch.object(
            worker._agent_runtime,
            "query_state",
            side_effect=AssertionError("executing intervention bridge must use runtime_status action"),
        ):
            reply = worker._handle_coordinator_executing_intervention({
                "room_id": "room-runtime-executing",
                "message_id": "msg-runtime-intervention",
                "text": "再添加一个天使雕像",
                "sender_id": "host-1",
                "sender_name": "房主",
                "sender_type": "host",
                "message_kind": "chat",
                "is_host": True,
                "agent_id": "agent-merchant",
                "agent_name": "商人",
            })

        self.assertIn("已记录该介入", reply or "")
        self.assertIn("已排入第", reply or "")
        state = worker._agent_runtime.state.snapshot("room-runtime-executing")["room"]
        self.assertEqual(state["active_plan_id"], runtime_plan_id)
        pending = list(state["pending_interventions"].values())
        self.assertEqual(len(pending), 1)
        self.assertEqual(pending[0]["plan_id"], runtime_plan_id)
        self.assertEqual(pending[0]["status"], "accepted")
        intervention_batches = [
            batch
            for batch in state["batch_plans"].values()
            if "天使雕像" in batch.get("requested_items", [])
        ]
        self.assertEqual(len(intervention_batches), 1)
        self.assertTrue(intervention_batches[0]["absorbed_intervention_ids"])
        self.assertIn("plan_patch_recorded", worker._agent_runtime.operation_log.events())
        self.assertIn("pending_intervention_batch_queued", worker._agent_runtime.operation_log.events())

    def test_agent_trigger_executing_intervention_routes_before_role_agent(self) -> None:
        coordinator = _FakeExecutingCoordinator()
        worker = _TestWorker(
            interaction_coordinator=coordinator,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        reply = worker._handle_coordinator_executing_intervention({
            "room_id": "room-agent-executing",
            "message_id": "msg-agent-intervention",
            "text": "@商人 再添加一个天使雕像",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "is_host": True,
            "agent_id": "agent-merchant",
            "agent_name": "商人",
        })

        self.assertIn("已记录该介入", reply or "")
        self.assertEqual(len(coordinator.ingested), 1)
        state = worker._agent_runtime.query_state("room-agent-executing")["room"]
        runtime_plan_id = state["external_plan_links"]["seed-executing"]
        pending = list(state["pending_interventions"].values())
        self.assertEqual(len(pending), 1)
        self.assertIn("天使雕像", pending[0]["items"])
        events = worker._agent_runtime.user_visible_events(
            room_id="room-agent-executing",
            plan_id=runtime_plan_id,
        )
        self.assertTrue([
            event for event in events if event["event_type"] == "intervention_recorded"
        ])

    def test_room_close_event_is_mirrored_to_agent_runtime_sync_state(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))

        result = worker.handle_lanchat_room_event({
            "event": "room_closed",
            "room_id": "room-sync-close",
            "message_id": "evt-close",
            "peer_id": "peer-host",
            "timestamp": 321.0,
        })

        self.assertTrue(result["handled"])
        self.assertEqual(len(result["runtime_sync"]), 1)
        self.assertTrue(result["runtime_sync"][0]["recorded"])
        state = worker._agent_runtime.query_state("room-sync-close")["room"]
        self.assertEqual(state["sync_state"]["room_status"], "closed")
        self.assertEqual(state["sync_state"]["last_event"]["peer_id"], "peer-host")
        self.assertNotIn("message_id", state["sync_state"]["last_event"])
        self.assertIn("sync_event_recorded", worker._agent_runtime.operation_log.events())

    def test_room_close_event_cancels_active_agent_runtime_plan(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        plan = worker._agent_runtime.handle_message(
            room_id="room-sync-close-cancel",
            text="做一个强盗藏宝室，包含宝箱、金币、火把和木桶",
            sender_id="host-1",
            sender_name="房主",
            action="plan",
            external_plan_id="seed-close-cancel",
        )["plan"]
        worker._agent_runtime.confirm_scene_plan(plan["plan_id"], confirmed_by="房主")
        worker._agent_runtime.plan_batches(plan["plan_id"], max_items_per_batch=2)

        result = worker.handle_lanchat_room_event({
            "event": "room_closed",
            "room_id": "room-sync-close-cancel",
            "message_id": "evt-close-cancel",
            "peer_id": "peer-host",
        })

        self.assertTrue(result["handled"])
        self.assertEqual(len(result["runtime_cancel"]), 1)
        self.assertTrue(result["runtime_cancel"][0]["recorded"])
        self.assertEqual(result["runtime_cancel"][0]["command"], "cancel")
        self.assertEqual(result["runtime_cancel"][0]["new_status"], "cancelled")
        state = worker._agent_runtime.query_state("room-sync-close-cancel")["room"]
        runtime_plan_id = state["external_plan_links"]["seed-close-cancel"]
        self.assertEqual(state["scene_plans"][runtime_plan_id]["status"], "cancelled")
        self.assertTrue(state["batch_plans"])
        self.assertTrue(all(batch["status"] == "cancelled" for batch in state["batch_plans"].values()))
        self.assertEqual(state["sync_state"]["room_status"], "closed")
        events = worker._agent_runtime.operation_log.events()
        self.assertIn("sync_event_recorded", events)
        self.assertIn("runtime_cancel_command_applied", events)
        summary = worker._agent_runtime.status_summary("room-sync-close-cancel")
        self.assertEqual(summary["runtime_command_summary"]["latest_commands"][-1]["command"], "cancel")
        self.assertNotIn("evt-close-cancel", str(summary))
        self.assertNotIn("peer-host", str(summary["runtime_command_summary"]))

    def test_actor_sync_event_bridge_records_runtime_actor_fact(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))

        result = worker.handle_lanchat_sync_event({
            "event": "actor_created",
            "room_id": "room-sync-actor",
            "actor_guid": "actor-bridge-001",
            "actor_name": "藏宝箱",
            "status": "broadcast",
            "timestamp": 456.0,
        })

        self.assertTrue(result["handled"])
        state = worker._agent_runtime.query_state("room-sync-actor")["room"]
        self.assertEqual(state["sync_state"]["actor_events"]["actor-bridge-001"]["event_type"], "actor_created")
        self.assertEqual(state["actors"]["actor-bridge-001"]["name"], "藏宝箱")
        self.assertEqual(state["actors"]["actor-bridge-001"]["last_sync_status"], "broadcast")

    def test_actor_transform_and_delete_sync_updates_runtime_actor_fact(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        plan = worker._agent_runtime.handle_message(
            room_id="room-sync-actor-transform",
            text="做一个强盗藏宝室，包含宝箱和金币",
            sender_id="host-1",
            sender_name="房主",
            action="plan",
            external_plan_id="seed-actor-transform",
        )["plan"]
        worker._agent_runtime.confirm_scene_plan(plan["plan_id"], confirmed_by="房主")
        worker.handle_lanchat_sync_event({
            "event": "actor_created",
            "room_id": "room-sync-actor-transform",
            "actor_guid": "actor-transform-001",
            "actor_name": "藏宝箱",
            "position": [0, 0, 0],
            "rotation": [0, 0, 0],
            "scale": [1, 1, 1],
            "message_id": "hidden-create-msg",
        })
        worker.handle_lanchat_sync_event({
            "event": "actor_transform_changed",
            "room_id": "room-sync-actor-transform",
            "actor_guid": "actor-transform-001",
            "actor_name": "藏宝箱",
            "position": {"x": 1.25, "y": 0.5, "z": -2.0},
            "rotation": {"x": 0, "y": 45, "z": 0},
            "scale": {"x": 1.2, "y": 1.0, "z": 0.8},
            "asset_path": "E:/hidden/chest.glb",
            "message_id": "hidden-transform-msg",
        })
        worker.handle_lanchat_sync_event({
            "event": "actor_deleted",
            "room_id": "room-sync-actor-transform",
            "actor_guid": "actor-transform-001",
            "actor_name": "藏宝箱",
            "message_id": "hidden-delete-msg",
        })
        worker.handle_lanchat_sync_event({
            "event": "file_chunk_received",
            "room_id": "room-sync-actor-transform",
            "asset_id": "asset-progress",
            "asset_path": "E:/hidden/models/chest.glb",
            "chunk_index": 3,
            "chunk_count": 8,
            "bytes_transferred": 4096,
            "total_bytes": 8192,
            "progress": 50,
            "message_id": "hidden-asset-msg",
        })
        worker._agent_runtime.state.apply_patch(
            StatePatch(
                room_id="room-sync-actor-transform",
                changes={
                    "custom_geometry_facts": {
                        f"{plan['plan_id']}:overlap": {
                            "fact_type": "runtime_geometry_overlap",
                            "plan_id": plan["plan_id"],
                            "status": "needs_adjustment",
                            "issue_count": 1,
                        }
                    }
                },
                expected_version=worker._agent_runtime.state.version,
            )
        )

        state = worker._agent_runtime.query_state("room-sync-actor-transform")["room"]
        actor = state["actors"]["actor-transform-001"]
        self.assertEqual(actor["position"], [1.25, 0.5, -2.0])
        self.assertEqual(actor["rotation"], [0.0, 45.0, 0.0])
        self.assertEqual(actor["scale"], [1.2, 1.0, 0.8])
        self.assertEqual(actor["sync_lifecycle_status"], "deleted")
        self.assertTrue(actor["deleted"])
        self.assertEqual(actor["last_sync_event"], "actor_deleted")

        reply = worker._handle_coordinator_status_query({
            "room_id": "room-sync-actor-transform",
            "message_id": "msg-sync-actor-transform-status",
            "text": "@GM 总结当前方案",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        })

        self.assertIn("多人同步健康：partial", reply or "")
        self.assertIn("Geometry facts:", reply or "")
        self.assertIn("overlap issues 1", reply or "")
        self.assertIn("actors create/transform/delete 1/1/1", reply or "")
        self.assertIn("active 0", reply or "")
        self.assertIn("needs asset-transfer-in-progress", reply or "")
        self.assertNotIn("asset-progress:transferring", reply or "")
        self.assertNotIn("hidden-transform-msg", reply or "")
        self.assertNotIn("hidden-delete-msg", reply or "")
        self.assertNotIn("hidden-asset-msg", reply or "")
        self.assertNotIn("E:/hidden", reply or "")

    def test_lanchat_sync_bridge_uses_agent_runtime_handle_message_entry(self) -> None:
        class FakeRuntime:
            def __init__(self) -> None:
                self.calls: list[dict[str, Any]] = []

            def handle_message(self, **kwargs):  # noqa: ANN001
                self.calls.append(dict(kwargs))
                return {
                    "handled": True,
                    "recorded": True,
                    "sync_event": {"event_type": "asset_transfer_completed", "asset_id": "asset-bridge"},
                    "sync_status": {"event_count": 1, "asset_event_count": 1},
                    "message": "ok",
                }

        runtime = FakeRuntime()
        worker = _TestWorker(
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
            agent_runtime=runtime,
        )

        result = worker.handle_lanchat_sync_event({
            "event": "asset_transfer_completed",
            "room_id": "room-sync-entry",
            "asset_id": "asset-bridge",
            "model_path": "E:/secret/model.glb",
        })

        self.assertTrue(result["handled"])
        self.assertEqual(len(runtime.calls), 1)
        self.assertEqual(runtime.calls[0]["action"], "runtime_sync_event")
        self.assertEqual(runtime.calls[0]["room_id"], "room-sync-entry")
        self.assertEqual(runtime.calls[0]["sync_event"]["asset_id"], "asset-bridge")
        self.assertNotIn("model.glb", str(result))

    def test_lanchat_sync_bridge_trusts_runtime_recorded_flag(self) -> None:
        class FakeRuntime:
            def __init__(self) -> None:
                self.calls: list[dict[str, Any]] = []

            def handle_message(self, **kwargs):  # noqa: ANN001
                self.calls.append(dict(kwargs))
                return {
                    "handled": True,
                    "recorded": False,
                    "sync_event": {"event_type": "actor_created", "actor_id": "actor-not-persisted"},
                    "sync_status": {"event_count": 0},
                    "message": "RuntimeState rejected sync patch",
                }

        runtime = FakeRuntime()
        worker = _TestWorker(
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
            agent_runtime=runtime,
        )

        result = worker.handle_lanchat_sync_event({
            "event": "actor_created",
            "room_id": "room-sync-rejected",
            "actor_id": "actor-not-persisted",
        })

        self.assertFalse(result["handled"])
        self.assertFalse(result["runtime_sync"]["recorded"])
        self.assertEqual(result["runtime_sync"]["reason"], "RuntimeState rejected sync patch")
        self.assertEqual(len(runtime.calls), 1)
        self.assertEqual(runtime.calls[0]["action"], "runtime_sync_event")

    def test_lanchat_sync_bridge_sanitizes_rejected_runtime_message(self) -> None:
        class FakeRuntime:
            def __init__(self) -> None:
                self.calls: list[dict[str, Any]] = []

            def handle_message(self, **kwargs):  # noqa: ANN001
                self.calls.append(dict(kwargs))
                return {
                    "handled": True,
                    "recorded": False,
                    "sync_event": {"event_type": "asset_transfer_failed", "asset_id": "asset-secret"},
                    "sync_status": {"event_count": 0},
                    "message": "provider raw https://secret.invalid E:/secret/model.glb prompt=hidden",
                }

        runtime = FakeRuntime()
        worker = _TestWorker(
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
            agent_runtime=runtime,
        )

        result = worker.handle_lanchat_sync_event({
            "event": "asset_transfer_failed",
            "room_id": "room-sync-rejected-secret",
            "asset_id": "asset-secret",
            "asset_path": "E:/secret/model.glb",
        })

        self.assertFalse(result["handled"])
        self.assertEqual(result["runtime_sync"]["reason"], "runtime_sync_rejected")
        self.assertNotIn("provider", str(result))
        self.assertNotIn("prompt", str(result))
        self.assertNotIn("https://secret.invalid", str(result))
        self.assertNotIn("E:/secret", str(result))
        self.assertEqual(len(runtime.calls), 1)
        self.assertEqual(runtime.calls[0]["action"], "runtime_sync_event")

    def test_runtime_system_event_send_records_operation_log_before_visible_message(self) -> None:
        engine = _FakeIdleEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        plan = worker._agent_runtime.sync_external_plan_context(
            room_id="room-runtime-system-event",
            external_plan_id="seed-runtime-system-event",
            text="做一个强盗藏宝室，包含宝箱和火把",
            owner_agent="山贼",
        )

        sent = worker._send_agent_runtime_system_event(
            "room-runtime-system-event",
            "Runtime：第 1 批资源准备完成。",
            runtime_event={
                "event_id": "evt-runtime-visible",
                "event_type": "batch_progress",
                "plan_id": plan.plan_id,
                "batch_id": "batch-01",
                "stage": "model_generating",
                "audience": "host",
                "level": "warning",
                "progress": 42,
                "provider": "secret-provider",
                "prompt": "secret-prompt",
                "asset_path": "E:/secret/model.glb",
            },
        )

        self.assertTrue(sent)
        self.assertEqual(len(engine.system_messages), 1)
        self.assertEqual(engine.system_messages[0]["message_kind"], "runtime_status")
        visible_metadata = json.loads(engine.system_messages[0]["metadata_json"])
        self.assertEqual(visible_metadata["phase"], "agent_runtime")
        self.assertEqual(visible_metadata["room_id"], "room-runtime-system-event")
        self.assertEqual(visible_metadata["runtime_event_id"], "evt-runtime-visible")
        self.assertEqual(visible_metadata["runtime_event_type"], "batch_progress")
        self.assertEqual(visible_metadata["runtime_plan_id"], plan.plan_id)
        self.assertEqual(visible_metadata["runtime_batch_id"], "batch-01")
        self.assertEqual(visible_metadata["runtime_stage"], "model_generating")
        self.assertEqual(visible_metadata["runtime_audience"], "host")
        self.assertEqual(visible_metadata["runtime_level"], "warning")
        self.assertEqual(visible_metadata["runtime_progress"], 42)
        self.assertNotIn("provider", visible_metadata)
        self.assertNotIn("prompt", visible_metadata)
        self.assertNotIn("asset_path", visible_metadata)
        self.assertNotIn("secret-provider", str(engine.system_messages))
        self.assertNotIn("secret-prompt", str(engine.system_messages))
        self.assertNotIn("model.glb", str(engine.system_messages))
        events = worker._agent_runtime.operation_log.events()
        self.assertIn("runtime_system_event_send_requested", events)
        self.assertIn("runtime_system_event_send_succeeded", events)
        self.assertLess(
            events.index("runtime_system_event_send_requested"),
            events.index("runtime_system_event_send_succeeded"),
        )
        entry = worker._agent_runtime.operation_log.query(
            event="runtime_system_event_send_succeeded",
            room_id="room-runtime-system-event",
        )[-1]
        self.assertEqual(entry.plan_id, plan.plan_id)
        self.assertTrue(entry.payload["sent"])
        self.assertEqual(entry.payload["message_kind"], "runtime_status")
        self.assertEqual(entry.payload["runtime_event_id"], "evt-runtime-visible")
        self.assertEqual(entry.payload["runtime_event_type"], "batch_progress")
        self.assertEqual(entry.payload["runtime_batch_id"], "batch-01")
        self.assertEqual(entry.payload["runtime_audience"], "host")
        self.assertEqual(entry.payload["runtime_level"], "warning")
        self.assertEqual(entry.payload["runtime_progress"], 42)
        replay = worker._agent_runtime.operation_replay(
            room_id="room-runtime-system-event",
            plan_id=plan.plan_id,
        )
        replay_events = [item["event"] for item in replay["entries"]]
        self.assertIn("runtime_system_event_send_requested", replay_events)
        self.assertIn("runtime_system_event_send_succeeded", replay_events)
        replay_text = str(replay)
        self.assertIn("runtime_event_id", replay_text)
        self.assertIn("evt-runtime-visible", replay_text)
        self.assertNotIn("secret-provider", replay_text)
        self.assertNotIn("secret-prompt", replay_text)
        self.assertNotIn("model.glb", replay_text)

    def test_runtime_event_auto_disclosure_skips_agent_audience(self) -> None:
        engine = _FakeIdleEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._agent_runtime.emit_runtime_event(
            room_id="room-runtime-audience",
            event_type="host_progress",
            title="Host progress",
            message="visible to host",
            audience="host",
            level="info",
            progress=10,
        )
        worker._agent_runtime.emit_runtime_event(
            room_id="room-runtime-audience",
            event_type="agent_internal",
            title="Agent internal",
            message="not for LANChat auto disclosure",
            audience="agent",
            level="info",
            progress=20,
            payload={
                "provider": "secret-provider",
                "prompt": "secret-prompt",
                "asset_path": "E:/secret/model.glb",
            },
        )

        sent = worker._emit_agent_runtime_events_since("room-runtime-audience", after_timestamp=0.0)

        self.assertEqual(sent, 1)
        self.assertEqual(len(engine.system_messages), 1)
        self.assertIn("Host progress", engine.system_messages[0]["text"])
        self.assertNotIn("Agent internal", str(engine.system_messages))
        metadata = json.loads(engine.system_messages[0]["metadata_json"])
        self.assertEqual(metadata["runtime_audience"], "host")
        self.assertEqual(metadata["runtime_level"], "info")
        skipped = worker._agent_runtime.operation_log.query(
            event="runtime_system_event_disclosure_skipped",
            room_id="room-runtime-audience",
        )
        self.assertEqual(len(skipped), 1)
        self.assertEqual(skipped[-1].payload["runtime_event_type"], "agent_internal")
        self.assertEqual(skipped[-1].payload["runtime_audience"], "agent")
        replay_text = str(worker._agent_runtime.operation_replay(room_id="room-runtime-audience"))
        self.assertIn("runtime_system_event_disclosure_skipped", replay_text)
        self.assertNotIn("secret-provider", replay_text)
        self.assertNotIn("secret-prompt", replay_text)
        self.assertNotIn("model.glb", replay_text)

    def test_runtime_event_auto_disclosure_filters_before_last_three_limit(self) -> None:
        engine = _FakeIdleEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._agent_runtime.emit_runtime_event(
            room_id="room-runtime-event-starvation",
            event_type="host_progress",
            title="Host progress",
            message="must still be visible",
            audience="host",
            level="info",
            progress=5,
        )
        for index in range(4):
            worker._agent_runtime.emit_runtime_event(
                room_id="room-runtime-event-starvation",
                event_type=f"agent_internal_{index}",
                title=f"Agent internal {index}",
                message="internal event",
                audience="agent",
                level="info",
            )

        sent = worker._emit_agent_runtime_events_since("room-runtime-event-starvation", after_timestamp=0.0)

        self.assertEqual(sent, 1)
        self.assertEqual(len(engine.system_messages), 1)
        self.assertIn("Host progress", engine.system_messages[0]["text"])
        self.assertNotIn("Agent internal", str(engine.system_messages))
        skipped = worker._agent_runtime.operation_log.query(
            event="runtime_system_event_disclosure_skipped",
            room_id="room-runtime-event-starvation",
        )
        self.assertEqual(len(skipped), 4)

    def test_runtime_event_auto_disclosure_lookback_survives_internal_event_burst(self) -> None:
        engine = _FakeIdleEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._agent_runtime.emit_runtime_event(
            room_id="room-runtime-event-burst",
            event_type="host_progress",
            title="Host progress",
            message="visible despite internal burst",
            audience="host",
            level="info",
            progress=15,
        )
        for index in range(12):
            worker._agent_runtime.emit_runtime_event(
                room_id="room-runtime-event-burst",
                event_type=f"agent_internal_{index}",
                title=f"Agent internal {index}",
                message="internal event",
                audience="agent",
                level="info",
            )

        sent = worker._emit_agent_runtime_events_since("room-runtime-event-burst", after_timestamp=0.0)

        self.assertEqual(sent, 1)
        self.assertEqual(len(engine.system_messages), 1)
        self.assertIn("Host progress", engine.system_messages[0]["text"])
        skipped = worker._agent_runtime.operation_log.query(
            event="runtime_system_event_disclosure_skipped",
            room_id="room-runtime-event-burst",
        )
        self.assertEqual(len(skipped), 12)

    def test_runtime_event_disclosure_skip_audit_is_plan_scoped(self) -> None:
        engine = _FakeIdleEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        plan = worker._agent_runtime.sync_external_plan_context(
            room_id="room-runtime-event-plan-scope",
            external_plan_id="seed-runtime-event-plan-scope",
            text="做一个强盗藏宝室，包含宝箱和火把",
            owner_agent="山贼",
        )
        after_timestamp = worker._latest_agent_runtime_event_timestamp("room-runtime-event-plan-scope")
        worker._agent_runtime.emit_runtime_event(
            room_id="room-runtime-event-plan-scope",
            plan_id=plan.plan_id,
            batch_id="batch-plan-scope",
            event_type="agent_internal",
            title="Agent internal",
            message="not for LANChat auto disclosure",
            audience="agent",
            level="info",
        )

        sent = worker._emit_agent_runtime_events_since(
            "room-runtime-event-plan-scope",
            after_timestamp=after_timestamp,
        )

        self.assertEqual(sent, 0)
        skipped = worker._agent_runtime.operation_log.query(
            event="runtime_system_event_disclosure_skipped",
            room_id="room-runtime-event-plan-scope",
            plan_id=plan.plan_id,
        )
        self.assertEqual(len(skipped), 1)
        self.assertEqual(skipped[-1].batch_id, "batch-plan-scope")
        replay = worker._agent_runtime.operation_replay(
            room_id="room-runtime-event-plan-scope",
            plan_id=plan.plan_id,
        )
        replay_text = str(replay)
        self.assertIn("runtime_system_event_disclosure_skipped", replay_text)
        self.assertIn("batch-plan-scope", replay_text)
        runtime_event_summary = replay.get("runtime_event_replay_summary", {})
        self.assertEqual(runtime_event_summary.get("disclosure_skipped_count"), 1)
        self.assertEqual(
            runtime_event_summary.get("latest_disclosure_skip", {}).get("event_type"),
            "agent_internal",
        )
        self.assertEqual(
            runtime_event_summary.get("latest_disclosure_skip", {}).get("audience"),
            "agent",
        )
        formatted = worker._format_agent_runtime_replay_runtime_event_report(runtime_event_summary)
        self.assertIn("skipped 1", formatted)
        self.assertIn("latest-skip agent-internal:agent", formatted)

    def test_runtime_event_disclosure_is_serial_deduped_and_terminal_ordered(self) -> None:
        engine = _FakeIdleEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        plan_id = "plan-terminal-order"
        worker._agent_runtime.emit_runtime_event(
            room_id="room-runtime-terminal-order",
            plan_id=plan_id,
            event_type="scene_snapshot_refreshed",
            title="场景快照已刷新",
            message="终态 Engine Snapshot 已记录。",
            audience="host",
            payload={"scene_version": 1, "actor_count": 3},
        )
        worker._agent_runtime.emit_runtime_event(
            room_id="room-runtime-terminal-order",
            plan_id=plan_id,
            event_type="report_ready",
            title="最终报告已就绪",
            message="同版本终态事实已完成。",
            audience="host",
            payload={"scene_version": 1},
        )
        sent_counts: list[int] = []
        first = threading.Thread(
            target=lambda: sent_counts.append(worker._emit_agent_runtime_events_since(
                "room-runtime-terminal-order",
                after_timestamp=0.0,
            ))
        )
        second = threading.Thread(
            target=lambda: sent_counts.append(worker._emit_agent_runtime_events_since(
                "room-runtime-terminal-order",
                after_timestamp=0.0,
            ))
        )

        first.start()
        second.start()
        first.join(timeout=2.0)
        second.join(timeout=2.0)

        self.assertEqual(sum(sent_counts), len(engine.system_messages))
        event_types = [
            json.loads(message["metadata_json"])["runtime_event_type"]
            for message in engine.system_messages
        ]
        self.assertEqual(event_types.count("scene_snapshot_refreshed"), 1)
        self.assertEqual(event_types.count("report_ready"), 1)
        self.assertLess(
            event_types.index("scene_snapshot_refreshed"),
            event_types.index("report_ready"),
        )
        event_ids = [
            json.loads(message["metadata_json"])["runtime_event_id"]
            for message in engine.system_messages
        ]
        self.assertEqual(len(set(event_ids)), len(event_ids))

    def test_failed_runtime_system_event_send_records_audit(self) -> None:
        engine = _FakeFailingSystemMessageEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        sent = worker._send_agent_runtime_system_event(
            "room-runtime-system-event-failed",
            "Runtime：第 1 批资源准备失败。",
        )

        self.assertFalse(sent)
        self.assertEqual(len(engine.system_messages), 1)
        events = worker._agent_runtime.operation_log.events()
        self.assertIn("runtime_system_event_send_requested", events)
        self.assertIn("runtime_system_event_send_failed", events)
        failed_entry = worker._agent_runtime.operation_log.query(
            event="runtime_system_event_send_failed",
            room_id="room-runtime-system-event-failed",
        )[-1]
        self.assertFalse(failed_entry.payload["sent"])
        self.assertEqual(failed_entry.payload["message_kind"], "runtime_status")

    def test_coordinator_system_reply_send_records_operation_log(self) -> None:
        engine = _FakeIdleEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        plan = worker._agent_runtime.sync_external_plan_context(
            room_id="room-coordinator-reply",
            external_plan_id="seed-coordinator-reply",
            text="做一个强盗藏宝室，包含宝箱和火把",
            owner_agent="山贼",
        )

        sent = worker._send_coordinator_sync_system_reply(
            {"room_id": "room-coordinator-reply", "message_id": "msg-coordinator-reply"},
            "【AgentRuntime 执行结果】ScenePlan 已进入 ToolCallGraph。",
        )

        self.assertTrue(sent)
        self.assertEqual(len(engine.system_messages), 1)
        self.assertEqual(engine.system_messages[0]["message_kind"], "action_status")
        events = worker._agent_runtime.operation_log.events()
        self.assertIn("coordinator_system_reply_send_requested", events)
        self.assertIn("coordinator_system_reply_send_succeeded", events)
        self.assertLess(
            events.index("coordinator_system_reply_send_requested"),
            events.index("coordinator_system_reply_send_succeeded"),
        )
        entry = worker._agent_runtime.operation_log.query(
            event="coordinator_system_reply_send_succeeded",
            room_id="room-coordinator-reply",
        )[-1]
        self.assertEqual(entry.plan_id, plan.plan_id)
        self.assertTrue(entry.payload["sent"])
        self.assertEqual(entry.payload["message_kind"], "action_status")
        self.assertEqual(entry.payload["reply_to"], "msg-coordinator-reply")
        replay = worker._agent_runtime.operation_replay(
            room_id="room-coordinator-reply",
            plan_id=plan.plan_id,
        )
        replay_events = [item["event"] for item in replay["entries"]]
        self.assertIn("coordinator_system_reply_send_requested", replay_events)
        self.assertIn("coordinator_system_reply_send_succeeded", replay_events)

    def test_failed_coordinator_system_reply_send_records_audit(self) -> None:
        engine = _FakeFailingSystemMessageEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        sent = worker._send_coordinator_sync_system_reply(
            {"room_id": "room-coordinator-reply-failed", "message_id": "msg-coordinator-reply-failed"},
            "【AgentRuntime 执行结果】ScenePlan 执行失败。",
        )

        self.assertFalse(sent)
        self.assertEqual(len(engine.system_messages), 1)
        events = worker._agent_runtime.operation_log.events()
        self.assertIn("coordinator_system_reply_send_requested", events)
        self.assertIn("coordinator_system_reply_send_failed", events)
        failed_entry = worker._agent_runtime.operation_log.query(
            event="coordinator_system_reply_send_failed",
            room_id="room-coordinator-reply-failed",
        )[-1]
        self.assertFalse(failed_entry.payload["sent"])
        self.assertEqual(failed_entry.payload["message_kind"], "action_status")
        self.assertEqual(failed_entry.payload["reply_to"], "msg-coordinator-reply-failed")

    def test_participant_disclosure_send_records_operation_log(self) -> None:
        engine = _FakeIdleEngine()
        worker = LANChatAgentWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
            async_agent_execution=False,
        )
        plan = worker._agent_runtime.sync_external_plan_context(
            room_id="room-disclosure",
            external_plan_id="seed-disclosure",
            text="做一个强盗藏宝室，包含宝箱和火把",
            owner_agent="山贼",
        )
        event = DisclosureEvent(
            room_id="room-disclosure",
            audience="participant",
            stage="generating",
            public_message="生成中 25%：正在准备资源。",
            progress=25,
            event_id="disc-participant",
        )
        coordinator = types.SimpleNamespace(
            disclosure_events=[event],
            disclosure_events_since=lambda start: ([event], 1),
        )

        emitted = worker._emit_new_disclosure_events(coordinator, 0)

        self.assertEqual(emitted, 1)
        self.assertEqual(len(engine.system_messages), 1)
        self.assertEqual(engine.system_messages[0]["message_kind"], "action_status")
        events = worker._agent_runtime.operation_log.events()
        self.assertIn("disclosure_event_send_requested", events)
        self.assertIn("disclosure_event_send_succeeded", events)
        entry = worker._agent_runtime.operation_log.query(
            event="disclosure_event_send_succeeded",
            room_id="room-disclosure",
        )[-1]
        self.assertTrue(entry.payload["sent"])
        self.assertEqual(entry.payload["event_id"], "disc-participant")
        self.assertEqual(entry.payload["audience"], "participant")
        self.assertEqual(entry.payload["stage"], "generating")
        self.assertEqual(entry.payload["progress"], 25)
        self.assertEqual(entry.plan_id, plan.plan_id)
        replay = worker._agent_runtime.operation_replay(
            room_id="room-disclosure",
            plan_id=plan.plan_id,
        )
        replay_events = [item["event"] for item in replay["entries"]]
        self.assertIn("disclosure_event_send_requested", replay_events)
        self.assertIn("disclosure_event_send_succeeded", replay_events)

    def test_host_disclosure_targeted_send_records_operation_log(self) -> None:
        engine = _FakeTargetedHostDisclosureEngine()
        worker = LANChatAgentWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
            async_agent_execution=False,
        )
        plan = worker._agent_runtime.sync_external_plan_context(
            room_id="room-host-disclosure",
            external_plan_id="seed-host-disclosure",
            text="做一个商会大厅，包含中心议价区",
            owner_agent="商人",
        )
        event = DisclosureEvent(
            room_id="room-host-disclosure",
            audience="host",
            stage="proposed",
            public_message="等待房主确认方案。",
            progress=0,
            event_id="disc-host",
            metadata={"target_user_id": "host-1", "proposal_id": "gm-1", "plan_id": "seed-host-disclosure"},
        )
        coordinator = types.SimpleNamespace(
            disclosure_events=[event],
            disclosure_events_since=lambda start: ([event], 1),
        )

        emitted = worker._emit_new_disclosure_events(coordinator, 0)

        self.assertEqual(emitted, 1)
        self.assertEqual(len(engine.host_messages), 1)
        self.assertEqual(engine.system_messages, [])
        events = worker._agent_runtime.operation_log.events()
        self.assertIn("disclosure_event_send_requested", events)
        self.assertIn("disclosure_event_send_succeeded", events)
        entry = worker._agent_runtime.operation_log.query(
            event="disclosure_event_send_succeeded",
            room_id="room-host-disclosure",
        )[-1]
        self.assertTrue(entry.payload["sent"])
        self.assertEqual(entry.payload["event_id"], "disc-host")
        self.assertEqual(entry.payload["audience"], "host")
        self.assertIn("network_send_system_message_to_host_ex", entry.payload["channel"])
        self.assertEqual(entry.plan_id, plan.plan_id)
        replay = worker._agent_runtime.operation_replay(
            room_id="room-host-disclosure",
            plan_id=plan.plan_id,
        )
        replay_events = [item["event"] for item in replay["entries"]]
        self.assertIn("disclosure_event_send_requested", replay_events)
        self.assertIn("disclosure_event_send_succeeded", replay_events)

    def test_generation_scheduler_snapshot_disclosure_records_operation_log(self) -> None:
        engine = _FakeIdleEngine()
        worker = LANChatAgentWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
            async_agent_execution=False,
        )

        worker._emit_generation_scheduler_snapshot_disclosure(
            "room-scheduler-disclosure",
            {
                "available": True,
                "queued_count": 2,
                "active_count": 1,
                "total_jobs": 3,
                "queue_pressure": 0.5,
                "diagnosis": {
                    "state": "busy",
                    "reasons": ["internal queue detail"],
                    "recommended_actions": ["wait"],
                },
                "recent_events": [{"event_type": "queued"}],
            },
        )

        self.assertEqual(len(engine.system_messages), 1)
        self.assertEqual(engine.system_messages[0]["message_kind"], "action_status")
        events = worker._agent_runtime.operation_log.events()
        self.assertIn("disclosure_event_send_requested", events)
        self.assertIn("disclosure_event_send_succeeded", events)
        entry = worker._agent_runtime.operation_log.query(
            event="disclosure_event_send_succeeded",
            room_id="room-scheduler-disclosure",
        )[-1]
        self.assertTrue(entry.payload["sent"])
        self.assertTrue(str(entry.payload.get("stage") or "").strip())
        self.assertEqual(entry.payload["progress"], 50)
        self.assertEqual(entry.payload["message_kind"], "action_status")
        self.assertEqual(entry.payload["channel"], "scheduler_broadcast_ex")
        self.assertNotIn("diagnosis", entry.payload)
        self.assertNotIn("recent_events", entry.payload)

    def test_process_once_auto_drains_one_runtime_graph_for_active_room(self) -> None:
        engine = _FakeIdleEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        plan = worker._agent_runtime.propose_scene_plan(
            room_id="room-auto-drain",
            text="做一个可爱卧室，有床、书桌、衣柜、台灯",
            owner_agent="小女孩",
        )
        worker._agent_runtime.confirm_scene_plan(plan.plan_id, confirmed_by="房主")
        queued = worker._agent_runtime.enqueue_planned_batches(plan.plan_id, max_items_per_batch=1)
        self.assertGreaterEqual(len(queued["graphs"]), 2)
        worker._remember_room_id("room-auto-drain")

        with patch.object(worker, "_log_agent_runtime_evidence") as evidence_log:
            processed = worker.process_once()

        self.assertTrue(processed)
        evidence_log.assert_called_once()
        self.assertEqual(
            evidence_log.call_args.kwargs["phase"],
            "runtime_queue_drain_result",
        )
        self.assertEqual(evidence_log.call_args.kwargs["runtime_plan_id"], plan.plan_id)
        summary = worker._agent_runtime.status_summary(
            "room-auto-drain",
            plan_id=plan.plan_id,
        )["tool_graph_summary"]
        self.assertEqual(summary["queue_status_counts"].get("completed"), 1)
        self.assertEqual(
            summary["queue_status_counts"].get("queued"),
            len(queued["graphs"]) - 1,
        )
        self.assertIn("room-auto-drain", worker._active_room_ids)
        self.assertIn("room-auto-drain", worker._active_room_order)
        self.assertGreaterEqual(
            len(worker._agent_runtime.query_state("room-auto-drain")["room"]["actors"]),
            1,
        )
        self.assertTrue(engine.system_messages)
        self.assertTrue(all(message["message_kind"] == "runtime_status" for message in engine.system_messages))
        visible_text = "\n".join(message["text"] for message in engine.system_messages)
        self.assertIn("Runtime", visible_text)
        self.assertNotIn("tool_name", visible_text)
        self.assertNotIn("provider", visible_text)
        self.assertNotIn("prompt", visible_text)
        self.assertNotIn("patch_id", visible_text)

    def test_process_once_auto_drain_completed_graph_does_not_log_exception(self) -> None:
        engine = _FakeIdleEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        plan = worker._agent_runtime.sync_external_plan_context(
            room_id="room-auto-drain-completed",
            external_plan_id="seed-auto-drain-completed",
            text="生成一个简单森林营地，有草地、天空、帐篷、小木桌。",
            owner_agent="长者",
        )
        result = worker._agent_runtime.handle_message(
            room_id="room-auto-drain-completed",
            text="确认生成",
            sender_id="host-1",
            sender_name="房主",
            action="confirm_and_execute",
            external_plan_id=plan.plan_id,
            max_items_per_batch=8,
        )
        self.assertEqual(len(result["graphs"]), 1)
        self.assertEqual(result["graphs"][0]["status"], "completed")
        worker._remember_room_id("room-auto-drain-completed")
        before_state = worker._agent_runtime.query_state("room-auto-drain-completed")["room"]
        before_graph_count = len(before_state["tool_graphs"])

        processed = worker.process_once()

        self.assertTrue(processed)
        events = worker._agent_runtime.operation_log.events()
        self.assertIn("scene_plan_finalized", events)
        self.assertIn("finalizer_started", events)
        self.assertIn("tool_graph_queue_empty", events)
        self.assertIn("latest_completed_plan_set", events)
        self.assertIn("active_execution_plan_cleared", events)
        self.assertIn("scene_entity_registry_ready", events)
        self.assertIn("runtime_message_drained", events)
        self.assertNotIn("runtime_worker_drain_exception", events)
        self.assertNotIn("runtime_worker_drain_failed", events)
        self.assertNotIn("room-auto-drain-completed", worker._active_room_ids)
        self.assertNotIn("room-auto-drain-completed", worker._active_room_order)
        state = worker._agent_runtime.query_state("room-auto-drain-completed")["room"]
        self.assertEqual(state["active_plan_id"], plan.plan_id)
        self.assertEqual(state["active_execution_plan_id"], "")
        self.assertEqual(state["latest_completed_plan_id"], plan.plan_id)
        self.assertEqual(len(state["batch_plans"]), 1)
        self.assertGreaterEqual(len(state["tool_graphs"]), before_graph_count)
        self.assertIn("runtime_plan_identity_persisted", events)

    def test_zero_drain_late_finalizer_still_discloses_report_ready(self) -> None:
        worker = _LayoutDirectExecutionTrackingWorker(
            corona_engine=_FakeIdleEngine(),
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        worker._remember_room_id("room-zero-drain-finalizer")
        runtime_result = {
            "drain": {
                "drained_count": 0,
                "status": "idle",
                "finalized_plans": [{"plan_id": "plan-late-ready", "status": "completed"}],
            },
            "plan": {"plan_id": "plan-late-ready", "status": "completed"},
            "report": {"plan_id": "plan-late-ready", "status": "completed"},
        }

        with (
            patch.object(worker, "_active_runtime_external_plan_id", return_value="plan-late-ready"),
            patch.object(worker, "_latest_agent_runtime_event_timestamp", return_value=10.0),
            patch.object(worker._agent_runtime, "handle_message", return_value=runtime_result),
            patch.object(worker, "_emit_agent_runtime_events_since", return_value=1) as emit_events,
            patch.object(worker, "_log_agent_runtime_evidence") as evidence_log,
        ):
            processed = worker._drain_agent_runtime_queue_once()

        self.assertTrue(processed)
        emit_events.assert_called_once_with(
            "room-zero-drain-finalizer",
            after_timestamp=10.0,
        )
        evidence_log.assert_called_once()
        self.assertEqual(evidence_log.call_args.kwargs["runtime_plan_id"], "plan-late-ready")

    def test_auto_drain_backs_off_and_suspends_repeated_finalizer_pending_state(self) -> None:
        for reason in ("final_report_persist_pending", "engine_readiness_pending"):
            with self.subTest(reason=reason):
                worker = _TestWorker(
                    corona_engine=_FakeIdleEngine(),
                    agent_runtime_flags=AgentRuntimeFlags.from_env({}),
                )
                room_id = f"room-finalizer-retry-{reason}"
                plan_id = f"plan-finalizer-retry-{reason}"
                worker._remember_room_id(room_id)
                runtime_result = {
                    "drain": {
                        "drained_count": 0,
                        "status": "idle",
                        "finalized_plans": [{
                            "plan_id": plan_id,
                            "status": "executing",
                            "reason": reason,
                        }],
                    },
                    "plan": {"plan_id": plan_id, "status": "executing"},
                }
                clock = [100.0]

                with (
                    patch.object(worker, "_active_runtime_execution_plan_id", return_value=plan_id),
                    patch.object(worker._agent_runtime, "handle_message", return_value=runtime_result) as handle_message,
                    patch.object(worker, "_latest_agent_runtime_event_timestamp", return_value=10.0),
                    patch.object(worker, "_start_runtime_drain_heartbeat", return_value=None),
                    patch.object(worker, "_emit_agent_runtime_events_since", return_value=0),
                    patch.object(worker, "_runtime_evidence_result", side_effect=lambda result, **_: result),
                    patch.object(worker, "_record_runtime_audit_event") as audit_event,
                    patch.object(worker, "_log_agent_runtime_evidence"),
                    patch("plugins.AITool.services.lanchat_agent_worker.time.monotonic", side_effect=lambda: clock[0]),
                    patch("plugins.AITool.services.lanchat_agent_worker.MAX_AGENT_RUNTIME_FINALIZER_RETRY_ATTEMPTS", 2),
                ):
                    self.assertFalse(worker._drain_agent_runtime_queue_once())
                    self.assertEqual(handle_message.call_count, 1)

                    clock[0] = 100.5
                    self.assertFalse(worker._drain_agent_runtime_queue_once())
                    self.assertEqual(handle_message.call_count, 1)

                    clock[0] = 101.1
                    self.assertTrue(worker._drain_agent_runtime_queue_once())

                self.assertEqual(handle_message.call_count, 2)
                self.assertNotIn(room_id, worker._active_room_ids)
                self.assertNotIn(room_id, worker._active_room_order)
                retry_state = worker._runtime_finalizer_retry_by_room[room_id]
                self.assertTrue(retry_state["exhausted"])
                self.assertEqual(retry_state["attempt_count"], 2)
                audit_event.assert_called_once()
                self.assertEqual(
                    audit_event.call_args.kwargs["event"],
                    "runtime_finalizer_retry_exhausted",
                )
                with patch.object(worker, "_runtime_plan_has_active_graph", return_value=True):
                    self.assertTrue(worker._runtime_finalizer_retry_due(room_id, plan_id))
                self.assertNotIn(room_id, worker._runtime_finalizer_retry_by_room)

    def test_process_once_forgets_room_when_runtime_plan_is_terminal(self) -> None:
        class _TerminalRuntime:
            def __init__(self) -> None:
                self.handle_calls = 0
                self.actions: list[str] = []

            def query_state(self, room_id: str) -> dict[str, Any]:
                return {
                    "room": {
                        "active_plan_id": "plan-complete",
                        "scene_plans": {"plan-complete": {"status": "completed"}},
                        "external_plan_links": {"seed-complete": "plan-complete"},
                    }
                }

            def handle_message(self, **kwargs):  # noqa: ANN003, ANN201
                self.handle_calls += 1
                self.actions.append(str(kwargs.get("action") or ""))
                return {}

        engine = _FakeIdleEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        runtime = _TerminalRuntime()
        worker._agent_runtime = runtime
        worker._remember_room_id("room-terminal")

        processed = worker.process_once()

        self.assertFalse(processed)
        self.assertEqual(runtime.handle_calls, 3)
        self.assertEqual(
            runtime.actions,
            [
                "runtime_events",
                "worker_drain",
                "runtime_events",
            ],
        )
        self.assertNotIn("room-terminal", worker._active_room_ids)
        self.assertNotIn("room-terminal", worker._active_room_order)

    def test_runtime_heartbeat_rejects_empty_plan_and_omits_progress(self) -> None:
        engine = _FakeIdleEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        self.assertIsNone(worker._start_runtime_drain_heartbeat(
            room_id="room-heartbeat",
            plan_id="",
            stop_event=threading.Event(),
        ))
        worker._emit_generation_progress_disclosure(
            "模型和环境组件仍在进入场景，你可以继续补充要求。",
            room_id="room-heartbeat",
            plan_id="seed-heartbeat",
            include_progress=False,
        )

        disclosure = json.loads(engine.system_messages[-1]["metadata_json"])["disclosure"]
        self.assertNotIn("progress", disclosure)
        self.assertEqual(disclosure["metadata"]["plan_id"], "seed-heartbeat")

    def test_generation_progress_updates_one_stable_event_per_plan(self) -> None:
        engine = _FakeIdleEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )

        worker._emit_generation_progress_disclosure(
            "生成进度 20%：正在准备所需模型。",
            room_id="room-progress",
            plan_id="plan-progress-a",
        )
        worker._emit_generation_progress_disclosure(
            "生成进度 20%：模型准备仍在继续。",
            room_id="room-progress",
            plan_id="plan-progress-a",
        )
        self.assertEqual(len(engine.system_messages), 1)

        worker._emit_generation_progress_disclosure(
            "生成进度 30%：正在准备所需模型。",
            room_id="room-progress",
            plan_id="plan-progress-a",
        )
        worker._emit_generation_progress_disclosure(
            "生成进度 30%：开始组装场景。",
            room_id="room-progress",
            plan_id="plan-progress-a",
        )
        self.assertEqual(len(engine.system_messages), 3)
        plan_a_event_ids = [row["correlation_id"] for row in engine.system_messages]
        self.assertEqual(len(set(plan_a_event_ids)), 1)

        worker._emit_generation_progress_disclosure(
            "生成进度 20%：正在准备所需模型。",
            room_id="room-progress",
            plan_id="plan-progress-b",
        )
        self.assertEqual(len(engine.system_messages), 4)
        self.assertNotEqual(
            engine.system_messages[-1]["correlation_id"],
            plan_a_event_ids[0],
        )
        self.assertTrue(
            all(row["agent_name"] == "系统" for row in engine.system_messages)
        )

        worker._emit_generation_progress_disclosure(
            "模型和环境组件仍在进入场景。",
            room_id="room-progress",
            plan_id="plan-progress-b",
            include_progress=False,
        )
        worker._emit_generation_progress_disclosure(
            "模型资源仍在进入场景。",
            room_id="room-progress",
            plan_id="plan-progress-b",
            include_progress=False,
        )
        self.assertEqual(len(engine.system_messages), 5)
        heartbeat_disclosure = json.loads(
            engine.system_messages[-1]["metadata_json"]
        )["disclosure"]
        self.assertNotIn("progress", heartbeat_disclosure)

    def test_process_once_records_worker_drain_failed_status_as_audit_event(self) -> None:
        class _DrainFailedRuntime:
            def __init__(self) -> None:
                self.audit_events: list[dict[str, Any]] = []

            def handle_message(self, *, action: str, sync_event: dict[str, Any] | None = None, **kwargs):  # noqa: ANN001
                if action == "worker_drain":
                    return {
                        "drain": {
                            "status": "failed",
                            "reason": "synthetic queue drain failure",
                            "drained_count": 0,
                        }
                    }
                if action == "runtime_audit_event":
                    self.audit_events.append(dict(sync_event or {}))
                    return {
                        "recorded": True,
                        "event": str((sync_event or {}).get("event") or ""),
                        "runtime_plan_id": "",
                    }
                return {}

        engine = _FakeIdleEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        runtime = _DrainFailedRuntime()
        worker._agent_runtime = runtime
        worker._remember_room_id("room-drain-failed")

        processed = worker.process_once()

        self.assertFalse(processed)
        self.assertEqual(len(runtime.audit_events), 1)
        audit_event = runtime.audit_events[0]
        self.assertEqual(audit_event["event"], "runtime_worker_drain_failed")
        self.assertEqual(audit_event["payload"]["phase"], "agent_runtime_worker_drain")
        self.assertEqual(audit_event["payload"]["drained_count"], 0)
        self.assertIn("synthetic queue drain failure", audit_event["payload"]["reason"])

    def test_process_once_records_failed_drained_graph_as_audit_event(self) -> None:
        class _DrainFailedGraphRuntime:
            def __init__(self) -> None:
                self.audit_events: list[dict[str, Any]] = []

            def handle_message(self, *, action: str, sync_event: dict[str, Any] | None = None, **kwargs):  # noqa: ANN001
                if action == "worker_drain":
                    return {
                        "drain": {
                            "status": "failed",
                            "reason": "one or more tool graphs did not complete",
                            "drained_count": 1,
                            "graphs": [{"graph_id": "graph-failed", "status": "failed"}],
                        }
                    }
                if action == "runtime_audit_event":
                    self.audit_events.append(dict(sync_event or {}))
                    return {
                        "recorded": True,
                        "event": str((sync_event or {}).get("event") or ""),
                        "runtime_plan_id": "",
                    }
                if action == "runtime_events":
                    return {"runtime_events": []}
                return {}

        engine = _FakeIdleEngine()
        worker = _TestWorker(
            corona_engine=engine,
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        runtime = _DrainFailedGraphRuntime()
        worker._agent_runtime = runtime
        worker._remember_room_id("room-drain-failed-graph")

        processed = worker.process_once()

        self.assertTrue(processed)
        self.assertEqual(len(runtime.audit_events), 1)
        audit_event = runtime.audit_events[0]
        self.assertEqual(audit_event["event"], "runtime_worker_drain_failed")
        self.assertEqual(audit_event["payload"]["phase"], "agent_runtime_worker_drain")
        self.assertEqual(audit_event["payload"]["drained_count"], 1)
        self.assertIn("one or more tool graphs", audit_event["payload"]["reason"])

    def test_runtime_confirmed_seedplan_execution_remembers_room_for_worker_drain(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))

        reply = worker._execute_confirmed_plan_via_agent_runtime(
            _FakePlan(),
            room_id="room-confirm-remembers",
            host_id="host-1",
        )

        self.assertIn("AgentRuntime 执行结果", reply)
        self.assertIn("queued", reply)
        self.assertNotIn("执行图 completed", reply)
        self.assertIn("room-confirm-remembers", worker._active_room_ids)
        state = worker._agent_runtime.query_state("room-confirm-remembers")["room"]
        self.assertTrue(state["tool_graph_queue"])

    def test_runtime_discussion_update_does_not_replace_active_execution_plan(self) -> None:
        runtime = AgentRuntime()
        discussion = runtime.sync_external_plan_context(
            room_id="room-plan-identity",
            external_plan_id="seed-discussion",
            text="讨论儿童卧室",
            owner_agent="小女孩",
        )
        execution = runtime.propose_scene_plan(
            room_id="room-plan-identity",
            text="儿童卧室，有床、书桌、衣柜",
            owner_agent="小女孩",
            external_plan_id="execution-plan",
        )
        runtime.confirm_scene_plan(execution.plan_id, confirmed_by="host")
        runtime.plan_batches(execution.plan_id, max_items_per_batch=1)
        runtime.enqueue_planned_batches(execution.plan_id, max_items_per_batch=1)
        runtime.record_agent_context_message(
            room_id="room-plan-identity",
            external_plan_id="seed-discussion",
            text="长者补充一条讨论意见",
            agent_name="长者",
            update_plan_brief=True,
        )

        room = runtime.query_state("room-plan-identity")["room"]
        self.assertEqual(room["active_discussion_plan_id"], discussion.plan_id)
        self.assertEqual(room["active_execution_plan_id"], execution.plan_id)
        self.assertEqual(room["active_plan_id"], discussion.plan_id)
        self.assertTrue(all(
            item["plan_id"] == execution.plan_id
            for item in room["tool_graph_queue"].values()
            if item.get("status") == "queued"
        ))

    def test_runtime_specific_statue_intervention_creates_one_execution_plan_patch(self) -> None:
        runtime = AgentRuntime()
        plan = runtime.propose_scene_plan(
            room_id="room-cupid-patch",
            text="children bedroom with bed and desk",
            external_plan_id="execution-cupid",
        )
        runtime.confirm_scene_plan(plan.plan_id, confirmed_by="host")
        runtime.plan_batches(plan.plan_id, max_items_per_batch=1)
        runtime.enqueue_planned_batches(plan.plan_id, max_items_per_batch=1)

        recorded = runtime.handle_message(
            room_id="room-cupid-patch",
            plan_id=plan.plan_id,
            text="再加入一个爱神丘比特雕像",
            action="intervention_add",
        )
        queued = runtime.handle_message(
            room_id="room-cupid-patch",
            plan_id=plan.plan_id,
            text="enqueue intervention",
            action="enqueue_pending_interventions",
        )

        self.assertTrue(recorded["recorded"])
        self.assertEqual(len(recorded["patch"]["items"]), 1)
        self.assertIn("丘比特", recorded["patch"]["items"][0])
        self.assertTrue(queued["recorded"])
        self.assertEqual(queued["batch"]["plan_id"], plan.plan_id)
        self.assertEqual(queued["batch"]["requested_items"], recorded["patch"]["items"])

    def test_completed_scene_add_targets_latest_plan_and_enqueues_one_deduped_item(self) -> None:
        worker = _TestWorker(
            corona_engine=_FakeIdleEngine(),
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        plan = worker._agent_runtime.sync_external_plan_context(
            room_id="room-completed-add",
            external_plan_id="seed-completed-add",
            text="儿童卧室，有床、书桌和衣柜",
            owner_agent="小女孩",
        )
        worker._agent_runtime.handle_message(
            room_id="room-completed-add",
            text="确认生成",
            sender_id="host-1",
            sender_name="房主",
            action="confirm_and_execute",
            external_plan_id=plan.plan_id,
            max_items_per_batch=8,
        )
        worker._remember_room_id("room-completed-add")
        self.assertTrue(worker.process_once())
        before = worker._agent_runtime.query_state("room-completed-add")["room"]
        self.assertEqual(before["latest_completed_plan_id"], plan.plan_id)
        self.assertEqual(before["active_execution_plan_id"], "")

        discussion = worker._agent_runtime.sync_external_plan_context(
            room_id="room-completed-add",
            external_plan_id="seed-new-discussion",
            text="讨论一个新的室外集市方案，但不要开始生成",
            owner_agent="商人",
        )
        after_discussion = worker._agent_runtime.query_state("room-completed-add")["room"]
        self.assertEqual(after_discussion["active_discussion_plan_id"], discussion.plan_id)
        self.assertEqual(after_discussion["latest_completed_plan_id"], plan.plan_id)

        message = {
            "room_id": "room-completed-add",
            "message_id": "msg-completed-add",
            "sender_id": "host-1",
            "sender_name": "房主",
            "agent_id": "girl",
            "agent_name": "小女孩",
            "text": "再加入一个爱神丘比特雕像再加入一个爱神丘比特雕像",
        }
        reply = worker._handle_runtime_completed_increment(message)

        self.assertIsNotNone(reply)
        self.assertIn("追加批", reply or "")
        state = worker._agent_runtime.query_state("room-completed-add")["room"]
        self.assertEqual(state["active_execution_plan_id"], plan.plan_id)
        self.assertEqual(state["active_discussion_plan_id"], discussion.plan_id)
        self.assertEqual(len(state["scene_plans"]), len(after_discussion["scene_plans"]))
        accepted_before_reload = dict(state.get("accepted_interventions") or {})
        original_patch_id = next(iter(accepted_before_reload))
        reloaded_runtime = AgentRuntime(
            state=worker._agent_runtime.state,
            operation_log=worker._agent_runtime.operation_log,
        )
        replayed = reloaded_runtime.handle_message(
            room_id="room-completed-add",
            plan_id=plan.plan_id,
            text=message["text"],
            sender_id="host-1",
            sender_name="房主",
            owner_agent="小女孩",
            action="post_generation_add_object",
            reply_to=message["message_id"],
        )
        self.assertEqual(replayed["patch"]["patch_id"], original_patch_id)
        state_after_reload = reloaded_runtime.query_state("room-completed-add")["room"]
        self.assertEqual(
            len(state_after_reload.get("accepted_interventions") or {}),
            len(accepted_before_reload),
        )
        self.assertIn("plan_patch_idempotent_replay", reloaded_runtime.operation_log.events())
        appended = sorted(
            (
                item for item in state["batch_plans"].values()
                if item.get("plan_id") == plan.plan_id
            ),
            key=lambda item: int(item.get("batch_index") or 0),
        )[-1]
        self.assertEqual(len(appended["requested_items"]), 1)
        self.assertIn("丘比特", appended["requested_items"][0])
        self.assertTrue(worker._process_trigger(message))
        state_after_dedupe = worker._agent_runtime.query_state("room-completed-add")["room"]
        self.assertEqual(len(state_after_dedupe["batch_plans"]), len(state["batch_plans"]))

        report_count_before_append_drain = len(state_after_dedupe.get("reports") or [])
        self.assertTrue(worker.process_once())
        finalized = worker._agent_runtime.query_state("room-completed-add")["room"]
        reports = list(finalized.get("reports") or [])
        self.assertGreater(len(reports), report_count_before_append_drain)
        latest_report = dict(reports[-1])
        latest_plan = dict(finalized["scene_plans"][plan.plan_id])
        self.assertEqual(
            int(dict(latest_report.get("plan_summary") or {}).get("version") or 0),
            int(latest_plan.get("version") or 0),
        )
        self.assertEqual(
            int(dict(latest_report.get("batch_summary") or {}).get("batch_count") or 0),
            len([
                batch
                for batch in finalized["batch_plans"].values()
                if batch.get("plan_id") == plan.plan_id
            ]),
        )

    def test_runtime_evidence_uses_persisted_batch_graph_and_node_facts(self) -> None:
        worker = _TestWorker(
            corona_engine=_FakeIdleEngine(),
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        plan = worker._agent_runtime.sync_external_plan_context(
            room_id="room-runtime-evidence",
            external_plan_id="seed-runtime-evidence",
            text="儿童卧室，有床和书桌",
            owner_agent="小女孩",
        )
        worker._agent_runtime.handle_message(
            room_id="room-runtime-evidence",
            text="确认生成",
            sender_id="host-1",
            sender_name="房主",
            action="confirm_and_execute",
            external_plan_id=plan.plan_id,
            max_items_per_batch=8,
        )

        runtime_room = worker._agent_runtime.query_state("room-runtime-evidence")["room"]
        all_plan_graphs = [
            dict(graph)
            for graph in runtime_room["tool_graphs"].values()
            if graph.get("plan_id") == plan.plan_id
        ]
        evidence_result = worker._runtime_evidence_result(
            {"graphs": all_plan_graphs},
            room_id="room-runtime-evidence",
            plan_id=plan.plan_id,
        )
        summary = worker._agent_runtime_evidence_summary(evidence_result)

        self.assertGreater(summary["batch_count"], 0)
        self.assertGreater(summary["graph_count"], 0)
        self.assertGreater(summary["internal_graph_count"], 0)
        self.assertGreater(len(all_plan_graphs), summary["graph_count"])
        self.assertEqual(
            summary["business_graph_count"],
            summary["graph_count"],
        )
        self.assertGreater(summary["node_count"], 0)
        self.assertEqual(
            summary["batch_count"],
            summary["batch_active_count"] + summary["batch_terminal_count"],
        )
        self.assertEqual(
            summary["graph_count"],
            summary["graph_active_count"] + summary["graph_terminal_count"],
        )

    def test_runtime_evidence_uses_live_summary_before_final_report_exists(self) -> None:
        worker = _TestWorker(
            corona_engine=_FakeIdleEngine(),
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        room = {
            "active_execution_plan_id": "plan-live-evidence",
            "batch_plans": {},
            "tool_graphs": {},
        }
        live_summary = {
            "plan_id": "plan-live-evidence",
            "operation_count": 7,
            "operation_total_count": 11,
            "scene_entity_registry": {
                "entity_count": 2,
                "actor_count": 1,
                "environment_count": 1,
                "game_ready_entity_count": 1,
            },
            "engine_write_boundary_summary": {
                "boundary_fact_count": 2,
                "import_boundary_count": 2,
                "bridge_call_count": 2,
                "bridge_success_count": 1,
            },
        }
        with patch.object(
            worker._agent_runtime,
            "query_state",
            return_value={"room": room, "summary": live_summary},
        ):
            evidence_result = worker._runtime_evidence_result(
                {},
                room_id="room-live-evidence",
                plan_id="plan-live-evidence",
            )

        summary = worker._agent_runtime_evidence_summary(evidence_result)
        self.assertEqual(summary["entity_count"], 2)
        self.assertEqual(summary["actor_count"], 1)
        self.assertEqual(summary["environment_count"], 1)
        self.assertEqual(summary["operation_count"], 7)
        self.assertEqual(summary["engine_write_import_boundary_count"], 2)
        self.assertEqual(summary["engine_write_bridge_success_count"], 1)

    def test_structured_gm_target_routes_before_generic_agent_chat(self) -> None:
        worker = _TestWorker(
            corona_engine=_FakeIdleEngine(),
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        processed: list[dict[str, Any]] = []
        message = {
            "room_id": "room-gm-control-first",
            "message_id": "msg-gm-control-first",
            "text": "@GM 确认",
        }
        metadata = {
            "draft_action": "chat",
            "target_scope": "agent",
            "target_agent_id": "gm",
            "target_agent_name": "GM",
            "source": "lanchat_native_queue",
        }
        with patch.object(worker, "_process_trigger", side_effect=lambda trigger: processed.append(trigger) or True):
            routed = worker._handle_structured_chat_route(message, message["text"], metadata)

        self.assertEqual(routed, "gm_control")
        self.assertEqual(len(processed), 1)
        self.assertEqual(processed[0]["agent_name"], "GM")

    def test_bare_gm_confirmation_is_deterministic_and_second_queue_copy_is_deduped(self) -> None:
        worker = _TestWorker(
            corona_engine=_FakeIdleEngine(),
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        trigger = {
            "room_id": "room-gm-deterministic",
            "message_id": "msg-gm-deterministic",
            "sender_id": "host-1",
            "sender_name": "房主",
            "agent_id": "gm",
            "agent_name": "GM",
            "target_agent_id": "gm",
            "text": "@GM 确认",
            "message_kind": "chat",
        }
        with patch.object(worker, "_run_agent", side_effect=AssertionError("GM control must not call LLM")):
            self.assertTrue(worker._process_trigger(trigger))
            self.assertTrue(worker._process_trigger(trigger))

        self.assertIn("msg-gm-deterministic", worker._gm_control_message_ids)

    def test_native_sync_gm_confirmation_does_not_create_or_update_seed_plan(self) -> None:
        worker = _LayoutDirectExecutionTrackingWorker(
            corona_engine=_FakeIdleEngine(),
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
        )
        before = worker._agent_runtime.query_state("room-native-gm-control")["room"]
        handled = worker.sync_chat_message_to_coordinator(
            {
                "room_id": "room-native-gm-control",
                "message_id": "msg-native-gm-control",
                "text": "@GM 确认",
                "sender_id": "host-1",
                "sender_name": "房主",
                "sender_type": "host",
                "message_kind": "chat",
                "target_agent_id": "gm",
                "agent_id": "gm",
                "agent_name": "GM",
                "is_host": True,
            },
            source="lanchat_native_queue",
            emit_disclosure=False,
        )

        self.assertTrue(handled)
        after = worker._agent_runtime.query_state("room-native-gm-control")["room"]
        self.assertEqual(after["scene_plans"], before["scene_plans"])
        self.assertEqual(after["planning_context_events"], before["planning_context_events"])
        self.assertIn("msg-native-gm-control", worker._gm_control_message_ids)
        self.assertTrue(worker.coordinator_system_replies)

    def test_single_default_local_player_is_treated_as_host(self) -> None:
        self.assertTrue(LANChatAgentWorker._message_sender_is_host({
            "room_id": "single-default",
            "sender_id": "local-single-player",
            "sender_name": "房主",
            "sender_type": "user",
        }))
        self.assertFalse(LANChatAgentWorker._message_sender_is_host({
            "room_id": "room-remote",
            "sender_id": "user-2",
            "sender_name": "普通用户",
            "sender_type": "user",
        }))

    def test_active_runtime_plan_generation_remembers_room_for_worker_drain(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        plan = worker._agent_runtime.propose_scene_plan(
            room_id="room-active-remembers",
            text="做一个强盗藏宝室，包含宝箱、金币和火把",
            owner_agent="商人",
        )

        reply = worker._execute_active_runtime_plan_generation(
            {
                "room_id": "room-active-remembers",
                "sender_name": "房主",
                "text": "确认生成",
                "agent_name": "商人",
            },
            room_id="room-active-remembers",
            host_id="host-1",
        )

        self.assertIn("AgentRuntime 执行结果", reply or "")
        self.assertIn("queued", reply or "")
        self.assertNotIn("执行图 completed", reply or "")
        self.assertIn("room-active-remembers", worker._active_room_ids)
        state = worker._agent_runtime.query_state("room-active-remembers")["room"]
        self.assertEqual(state["active_plan_id"], plan.plan_id)
        self.assertTrue(state["tool_graph_queue"])

    def test_active_runtime_plan_generation_logs_non_authoritative_skip(self) -> None:
        worker = _RemoteGenerationWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))

        with self.assertLogs("plugins.AITool.services.lanchat_agent_worker", level="INFO") as logs:
            reply = worker._execute_active_runtime_plan_generation(
                {
                    "room_id": "room-remote-skip",
                    "sender_name": "host",
                    "text": "confirm generation",
                    "agent_name": "merchant",
                },
                room_id="room-remote-skip",
                host_id="host-1",
            )

        self.assertIsNone(reply)
        joined = "\n".join(logs.output)
        self.assertIn("phase=runtime_active_plan_execute_skipped", joined)
        self.assertIn("reason=not_authoritative", joined)

    def test_active_runtime_plan_generation_logs_no_plan_result(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        worker._agent_runtime.sync_external_plan_context(
            room_id="room-no-runtime-plan-result",
            external_plan_id="seed-no-runtime-plan-result",
            text="merchant hall with a table",
            owner_agent="merchant",
        )
        with patch.object(
            worker,
            "_active_runtime_external_plan_id",
            return_value="seed-no-runtime-plan-result",
        ), patch.object(
            worker._agent_runtime,
            "handle_message",
            return_value={"handled": True, "action": "confirm_and_enqueue", "message": "no plan"},
        ):
            with self.assertLogs("plugins.AITool.services.lanchat_agent_worker", level="INFO") as logs:
                reply = worker._execute_active_runtime_plan_generation(
                    {
                        "room_id": "room-no-runtime-plan-result",
                        "sender_name": "host",
                        "text": "confirm generation",
                        "agent_name": "merchant",
                    },
                    room_id="room-no-runtime-plan-result",
                    host_id="host-1",
                )

        self.assertIsNone(reply)
        joined = "\n".join(logs.output)
        self.assertIn("phase=runtime_active_plan_execute_no_plan", joined)
        self.assertIn("action=confirm_and_enqueue", joined)
        self.assertIn("handled=True", joined)

    def test_agent_runtime_execution_reply_accepts_single_graph_shape(self) -> None:
        result = {
            "plan": {"plan_id": "plan-single-graph"},
            "batch": {"batch_id": "batch-single"},
            "graph": {"graph_id": "graph-single", "status": "completed"},
            "report": {
                "report_health_summary": {
                    "status": "needs_attention",
                    "attention_required": True,
                    "engine_write_runtime_state_only_count": 1,
                    "engine_write_runtime_state_only_channels": ["actor_import"],
                    "reasons": ["engine_write_runtime_state_only"],
                },
                "scene_entity_registry": {
                    "entity_count": 3,
                    "readiness_missing_field_counts": {
                        "engine_actual_aabb": 2,
                        "grounding_status": 1,
                    },
                    "entity_type_counts": {
                        "actor": 1,
                        "terrain": 1,
                        "skybox": 1,
                    },
                },
                "runtime_scene_flow_summary": {
                    "status": "ok",
                    "steps": [
                        {"step": "plan"},
                        {"step": "terrain"},
                        {"step": "asset"},
                        {"step": "actor"},
                        {"step": "review"},
                        {"step": "report"},
                    ],
                },
                "classification_summary": {
                    "model_items": ["帐篷"],
                    "substrate_items": ["草地", "天空"],
                },
                "tool_execution_digest": {
                    "available": True,
                    "succeeded_count": 6,
                    "failed_count": 0,
                    "blocked_count": 0,
                },
                "state_patch_summary": {
                    "applied": 5,
                    "conflict": 0,
                    "invalid": 0,
                },
                "tool_queue_health_summary": {
                    "queue_count": 2,
                    "queued_count": 0,
                    "running_count": 0,
                    "blocked_count": 0,
                    "terminal_count": 2,
                    "active_count": 0,
                    "queue_pressure": 0.0,
                },
                "batch_tooling_summary": {
                    "fact_count": 4,
                    "created_batch_fact_count": 1,
                    "created_batch_count": 1,
                    "prioritized_item_count": 3,
                    "merged_intervention_fact_count": 1,
                    "merged_intervention_item_count": 2,
                    "absorbed_intervention_count": 2,
                },
                "runtime_guard_replay_summary": {
                    "blocked_count": 2,
                    "requires_write_blocked_count": 1,
                    "system_actor_write_blocked_count": 1,
                    "high_risk_confirmation_required_count": 1,
                    "write_confirmation_required_count": 2,
                    "confirmed_blocked_count": 1,
                    "unconfirmed_blocked_count": 1,
                },
                "operation_count": 12,
                "operation_total_count": 15,
                "fact_source_boundary_summary": {
                    "runtime_state_source": "RuntimeState",
                },
                "engine_write_boundary_summary": {
                    "boundary_fact_count": 1,
                    "import_boundary_count": 1,
                    "bridge_call_count": 0,
                    "bridge_success_count": 0,
                    "bridge_failed_count": 0,
                    "bridge_error_code_counts": {"cpp_actor_import_failed": 1},
                    "status_counts": {"runtime_state_only": 1},
                    "write_source_counts": {"runtime_default_import_provider": 1},
                },
                "import_summary": {
                    "import_failure_code_counts": {"missing_ready_model_resource": 1},
                    "environment_import_failure_code_counts": {"terrain_import_unavailable": 1},
                },
                "operation_replay_summary": {
                    "resource_summary": {
                        "by_phase": {
                            "image": {"requested_count": 1, "failed_count": 0},
                            "model": {"requested_count": 1, "failed_count": 0},
                        },
                    },
                    "geometry_fact_replay_summary": {
                        "fact_count": 1,
                        "aabb_actor_count": 1,
                        "overlap_issue_count": 0,
                    },
                    "vlm_checkpoint_summary": {
                        "checkpoint_count": 1,
                        "advisory_count": 0,
                    },
                    "sync_replay_summary": {
                        "recorded_count": 1,
                        "failed_count": 0,
                    },
                    "asset_transfer_replay_summary": {
                        "asset_transfer_progress_count": 1,
                        "asset_transfer_failed_count": 0,
                    },
                    "batch_execution_summary": {
                        "completed_count": 1,
                    },
                },
            },
            "drain": {
                "status": "drained",
                "drained_count": 1,
                "graphs": [{"graph_id": "graph-single", "status": "completed"}],
            },
        }
        reply = LANChatAgentWorker._format_agent_runtime_execution_reply(result)
        evidence = LANChatAgentWorker._agent_runtime_evidence_summary(result)

        self.assertIn("已执行 Runtime 批次 1 个", reply)
        self.assertIn("执行图 completed:1", reply)
        self.assertNotIn("执行图 none", reply)
        self.assertIn("实体注册：3 个", reply)
        self.assertIn("actor 1", reply)
        self.assertIn("needs-attention", reply)
        self.assertIn("terrain 1", reply)
        self.assertIn("skybox 1", reply)
        self.assertIn("Classification", reply)
        self.assertIn("model/substrate 1/2", reply)
        self.assertIn("Flow：ok", reply)
        self.assertIn("plan>terrain>asset>actor>review>report", reply)
        self.assertIn("Tool/State：tools ok/fail/block 6/0/0", reply)
        self.assertIn("patch applied/conflict/invalid 5/0/0", reply)
        self.assertIn("OperationLog 15", reply)
        self.assertIn("Guard", reply)
        self.assertIn("block/write/system 2/1/1", reply)
        self.assertIn("confirm high/write 1/2", reply)
        self.assertIn("Queue", reply)
        self.assertIn("total/queued/running/active/block 2/0/0/0/0", reply)
        self.assertIn("pressure 0%", reply)
        self.assertIn("Drain：drained", reply)
        self.assertIn("drained 1", reply)
        self.assertIn("BatchTooling", reply)
        self.assertIn("facts/created/prioritized/merged/absorbed 4/1/3/2/2", reply)
        self.assertIn("ReportSource", reply)
        self.assertIn("state RuntimeState", reply)
        self.assertIn("operation 12/15", reply)
        self.assertIn("Engine写入：RuntimeState-only 1 项", reply)
        self.assertIn("真实引擎写入待 F5/实机验证", reply)
        self.assertEqual(evidence["batch_count"], 1)
        self.assertEqual(evidence["graph_count"], 1)
        self.assertEqual(evidence["flow_steps"], "plan>terrain>asset>actor>review>report")
        self.assertEqual(evidence["entity_count"], 3)
        self.assertEqual(
            evidence["readiness_missing_field_counts"],
            {"engine_actual_aabb": 2, "grounding_status": 1},
        )
        self.assertEqual(evidence["model_items"], 1)
        self.assertEqual(evidence["substrate_items"], 2)
        self.assertEqual(evidence["operation_count"], 12)
        self.assertEqual(evidence["operation_total_count"], 15)
        self.assertEqual(evidence["tool_execution_succeeded_count"], 6)
        self.assertEqual(evidence["tool_execution_failed_count"], 0)
        self.assertEqual(evidence["tool_execution_blocked_count"], 0)
        self.assertEqual(evidence["state_patch_applied_count"], 5)
        self.assertEqual(evidence["state_patch_conflict_count"], 0)
        self.assertEqual(evidence["state_patch_invalid_count"], 0)
        self.assertEqual(evidence["runtime_guard_blocked_count"], 2)
        self.assertEqual(evidence["runtime_guard_requires_write_blocked_count"], 1)
        self.assertEqual(evidence["runtime_guard_system_actor_write_blocked_count"], 1)
        self.assertEqual(evidence["runtime_guard_high_risk_confirmation_required_count"], 1)
        self.assertEqual(evidence["runtime_guard_write_confirmation_required_count"], 2)
        self.assertEqual(evidence["runtime_guard_confirmed_blocked_count"], 1)
        self.assertEqual(evidence["runtime_guard_unconfirmed_blocked_count"], 1)
        self.assertEqual(evidence["tool_queue_count"], 2)
        self.assertEqual(evidence["tool_queue_queued_count"], 0)
        self.assertEqual(evidence["tool_queue_running_count"], 0)
        self.assertEqual(evidence["tool_queue_blocked_count"], 0)
        self.assertEqual(evidence["tool_queue_terminal_count"], 2)
        self.assertEqual(evidence["tool_queue_active_count"], 0)
        self.assertEqual(evidence["tool_queue_pressure"], 0.0)
        self.assertEqual(evidence["drain_status"], "drained")
        self.assertEqual(evidence["drain_drained_count"], 1)
        self.assertEqual(evidence["drain_reason"], "")
        self.assertEqual(evidence["batch_tooling_fact_count"], 4)
        self.assertEqual(evidence["batch_tooling_created_batch_fact_count"], 1)
        self.assertEqual(evidence["batch_tooling_created_batch_count"], 1)
        self.assertEqual(evidence["batch_tooling_prioritized_item_count"], 3)
        self.assertEqual(evidence["batch_tooling_merged_intervention_fact_count"], 1)
        self.assertEqual(evidence["batch_tooling_merged_intervention_item_count"], 2)
        self.assertEqual(evidence["batch_tooling_absorbed_intervention_count"], 2)
        self.assertEqual(evidence["runtime_state_source"], "RuntimeState")
        self.assertEqual(evidence["engine_write_boundary_count"], 1)
        self.assertEqual(evidence["engine_write_import_boundary_count"], 1)
        self.assertEqual(evidence["engine_write_bridge_call_count"], 0)
        self.assertEqual(evidence["engine_write_bridge_success_count"], 0)
        self.assertEqual(evidence["engine_write_bridge_failed_count"], 0)
        self.assertEqual(evidence["engine_write_bridge_error_code_counts"], {"cpp_actor_import_failed": 1})
        self.assertEqual(evidence["engine_write_status_counts"], {"runtime_state_only": 1})
        self.assertEqual(evidence["engine_write_source_counts"], {"runtime_default_import_provider": 1})
        self.assertEqual(evidence["import_failure_code_counts"], {"missing_ready_model_resource": 1})
        self.assertEqual(evidence["environment_import_failure_code_counts"], {"terrain_import_unavailable": 1})
        self.assertEqual(evidence["resource_image_requested_count"], 1)
        self.assertEqual(evidence["resource_model_requested_count"], 1)
        self.assertEqual(evidence["geometry_fact_count"], 1)
        self.assertEqual(evidence["geometry_aabb_actor_count"], 1)
        self.assertEqual(evidence["vlm_checkpoint_count"], 1)
        self.assertEqual(evidence["sync_recorded_count"], 1)
        self.assertEqual(evidence["asset_transfer_progress_count"], 1)
        self.assertEqual(evidence["batch_execution_completed_count"], 1)

    def test_scene_registry_report_uses_entity_type_counts_for_environment_entities(self) -> None:
        text = LANChatAgentWorker._format_agent_runtime_scene_registry_report({
            "entity_count": 3,
            "entity_type_counts": {
                "actor": 1,
                "terrain": 1,
                "skybox": 1,
            },
            "entities": [
                {"entity_type": "terrain", "semantic_role": "ground"},
                {"entity_type": "skybox", "semantic_role": "sky"},
                {"entity_type": "actor", "semantic_role": "tent"},
            ],
        })

        self.assertIn("entities 3", text)
        self.assertIn("actor 1", text)
        self.assertIn("terrain 1", text)
        self.assertIn("skybox 1", text)

        actor_import_text = LANChatAgentWorker._format_agent_runtime_actor_import_boundary_report(
            {"requested_count": 1, "imported_count": 1, "failed_count": 0},
            {"entity_type_counts": {"actor": 1}},
            {"bridge_call_count": 1, "bridge_success_count": 1, "bridge_failed_count": 0},
        )
        self.assertIn("registered actor 1", actor_import_text)

    def test_active_runtime_plan_generation_repeated_agent_confirm_reuses_queued_graph(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        plan = worker._agent_runtime.propose_scene_plan(
            room_id="room-active-repeat-confirm",
            text="生成一个简单森林营地，有草地、天空、帐篷、小木桌。",
            owner_agent="小女孩",
            external_plan_id="seed-active-repeat-confirm",
        )

        first_reply = worker._execute_active_runtime_plan_generation(
            {
                "room_id": "room-active-repeat-confirm",
                "sender_name": "房主",
                "text": "确认生成",
                "agent_name": "",
            },
            room_id="room-active-repeat-confirm",
            host_id="local-single-player",
        )
        second_reply = worker._execute_active_runtime_plan_generation(
            {
                "room_id": "room-active-repeat-confirm",
                "sender_name": "房主",
                "text": "@小女孩 确认生成",
                "agent_name": "小女孩",
            },
            room_id="room-active-repeat-confirm",
            host_id="local-single-player",
        )

        for reply in (first_reply, second_reply):
            self.assertIn("AgentRuntime 执行结果", reply or "")
            self.assertIn("queued:1", reply or "")
            self.assertNotIn("Runtime 批次 0 个", reply or "")
            self.assertNotIn("执行图 none", reply or "")
        state = worker._agent_runtime.query_state("room-active-repeat-confirm")["room"]
        self.assertEqual(state["active_plan_id"], plan.plan_id)
        self.assertEqual(len(state["batch_plans"]), 1)
        execution_graphs = [
            graph
            for graph in state["tool_graphs"].values()
            if any(
                node.get("tool_name") == "runtime.asset.model.prepare"
                for node in graph.get("nodes", {}).values()
            )
        ]
        self.assertEqual(len(execution_graphs), 1)
        self.assertEqual(execution_graphs[0]["status"], "queued")
        worker._agent_runtime.drain_tool_graph_queue(
            "room-active-repeat-confirm",
            plan_id=plan.plan_id,
        )
        query_summary = worker._agent_runtime.query_state("room-active-repeat-confirm")["summary"]
        roles = {
            entity["semantic_role"]: entity
            for entity in query_summary["scene_entity_registry"]["entities"]
        }
        self.assertTrue({"草地", "天空", "森林", "帐篷", "小木桌"}.issubset(roles))
        self.assertEqual(roles["草地"]["entity_type"], "environment", roles)
        self.assertEqual(roles["天空"]["entity_type"], "environment", roles)
        self.assertIn(roles["森林"]["entity_type"], {"environment", "substrate"}, roles)
        for role in ("帐篷", "小木桌"):
            self.assertEqual(roles[role]["entity_type"], "actor")
            self.assertTrue(roles[role]["actor_id"])
            self.assertTrue(roles[role]["transform"])
            self.assertTrue(roles[role].get("aabb") or roles[role].get("bounds"))
            self.assertIn("grounding_status", roles[role])
        self.assertEqual(query_summary["runtime_scene_flow_summary"]["status"], "ok")
        flow_steps = [
            step["step"]
            for step in query_summary["runtime_scene_flow_summary"]["steps"]
        ]
        self.assertEqual(flow_steps, ["plan", "terrain", "asset", "actor", "review", "report"])
        reused = worker._agent_runtime.operation_log.query(
            event="planned_batches_enqueue_reused",
            room_id="room-active-repeat-confirm",
            plan_id=plan.plan_id,
        )
        self.assertEqual(len(reused), 1)
        report = worker._agent_runtime.generate_report("room-active-repeat-confirm", plan_id=plan.plan_id)
        report_roles = {
            entity["semantic_role"]: entity
            for entity in report["scene_entity_registry"]["entities"]
        }
        self.assertEqual(report_roles["帐篷"]["entity_type"], "actor")
        self.assertEqual(report_roles["小木桌"]["entity_type"], "actor")
        self.assertEqual(report["runtime_scene_flow_summary"]["status"], "ok")

    def test_agent_runtime_execution_failures_do_not_leak_internal_exception_text(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        sensitive_error = RuntimeError(
            "provider=https://secret.invalid prompt=hidden E:/private/model.glb api_key=SECRET"
        )

        with patch.object(
            worker,
            "_active_runtime_external_plan_id",
            return_value="seed-active-error-safe",
        ), patch.object(worker._agent_runtime, "handle_message", side_effect=sensitive_error):
            confirmed_reply = worker._execute_confirmed_plan_via_agent_runtime(
                _FakePlan(),
                room_id="room-execute-error-safe",
                host_id="host-1",
            )
        plan = worker._agent_runtime.sync_external_plan_context(
            room_id="room-active-error-safe",
            external_plan_id="seed-active-error-safe",
            text="做一个强盗藏宝室，包含宝箱、金币和火把",
            owner_agent="商人",
        )
        with patch.object(
            worker,
            "_active_runtime_external_plan_id",
            return_value="seed-active-error-safe",
        ), patch.object(worker._agent_runtime, "handle_message", side_effect=sensitive_error):
            active_reply = worker._execute_active_runtime_plan_generation(
                {
                    "room_id": "room-active-error-safe",
                    "sender_name": "房主",
                    "text": "确认生成",
                    "agent_name": "商人",
                },
                room_id="room-active-error-safe",
                host_id="host-1",
            )

        for reply in (confirmed_reply, active_reply):
            self.assertIn("内部执行异常已记录", reply or "")
            self.assertNotIn("https://secret.invalid", reply or "")
            self.assertNotIn("prompt=hidden", reply or "")
            self.assertNotIn("E:/private", reply or "")
            self.assertNotIn("api_key", reply or "")
        self.assertEqual(
            worker._agent_runtime.query_state("room-active-error-safe")["room"]["active_plan_id"],
            plan.plan_id,
        )

    def test_layout_reflow_runtime_failure_does_not_leak_internal_exception_text(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        worker._agent_runtime.sync_external_plan_context(
            room_id="room-layout-error-safe",
            external_plan_id="seed-completed",
            text="做一个强盗藏宝室，包含宝箱、金币和火把",
            owner_agent="商人",
        )
        sensitive_error = RuntimeError(
            "provider=https://secret.invalid prompt=hidden E:/private/layout.json token=SECRET"
        )

        with patch.object(worker._agent_runtime, "handle_message", side_effect=sensitive_error):
            reply = worker._confirm_layout_reflow_via_agent_runtime(
                room_id="room-layout-error-safe",
                plan=_FakeCompletedPlan(),
                payload={
                    "status": "confirmed",
                    "sender_id": "host-1",
                    "sender_name": "房主",
                },
            )

        self.assertIn("内部异常已记录", reply)
        self.assertNotIn("https://secret.invalid", reply)
        self.assertNotIn("prompt=hidden", reply)
        self.assertNotIn("E:/private", reply)
        self.assertNotIn("token=", reply)

    def test_structured_host_action_runtime_failure_does_not_leak_internal_exception_text(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        sensitive_error = RuntimeError(
            "provider=https://secret.invalid prompt=hidden E:/private/action.json api_key=SECRET"
        )

        with patch.object(worker._agent_runtime, "handle_message", side_effect=sensitive_error):
            reply = worker._execute_structured_host_action_via_agent_runtime({
                "action_type": "start_generation",
                "status": "confirmed",
                "execution": "agent_runtime_structured",
                "runtime_payload_prepared_by_worker": True,
                "plan_id": "seed-structured-error-safe",
                "room_id": "room-structured-error-safe",
                "intent_text": "做一个强盗藏宝室，包含宝箱、金币和火把",
                "source_user_id": "房主",
            })

        self.assertIn("内部执行异常已记录", reply)
        self.assertNotIn("https://secret.invalid", reply)
        self.assertNotIn("prompt=hidden", reply)
        self.assertNotIn("E:/private", reply)
        self.assertNotIn("api_key", reply)

    def test_runtime_status_reply_discloses_substrate_classification(self) -> None:
        worker = _TestWorker(agent_runtime_flags=AgentRuntimeFlags.from_env({}))
        plan = worker._agent_runtime.propose_scene_plan(
            room_id="room-substrate-status",
            text="做一个森林营地，有天空、树林、草地、小木桌、帐篷",
            owner_agent="长者",
        )
        plan.concrete_object_items = ["天空", "树林", "草地", "小木桌", "帐篷"]
        worker._agent_runtime.confirm_scene_plan(plan.plan_id, confirmed_by="房主")
        worker._agent_runtime.execute_scene_plan(plan.plan_id)
        room_state = worker._agent_runtime.state.room("room-substrate-status")
        room_state["geometry_reviews"][plan.plan_id] = {
            "plan_id": plan.plan_id,
            "batch_id": "batch-review",
            "checkpoint_type": "structure_review",
            "status": "needs_adjustment",
            "overall": "WARN",
            "issues": [
                {
                    "name": "小木桌",
                    "type": "out_of_bounds",
                    "current_position": [9.0, 0.4, 0.0],
                    "bounds": [-4.0, 4.0, -4.0, 4.0],
                    "prompt": "hidden layout prompt",
                }
            ],
            "advisory_items": [{"type": "layout", "message": "清出入口"}],
            "reviewed_targets": ["入口", "主街"],
        }
        proposal = worker._agent_runtime.propose_review_advisories(
            room_id="room-substrate-status",
            plan_id=plan.plan_id,
        )["proposal"]
        worker._agent_runtime.record_review_advisory_confirmation(
            room_id="room-substrate-status",
            plan_id=plan.plan_id,
            proposal_id=proposal["proposal_id"],
            decision="confirmed",
            confirmed_by="房主",
        )
        worker._agent_runtime.propose_layout_adjustment(
            room_id="room-substrate-status",
            plan_id=plan.plan_id,
        )
        worker._agent_runtime.apply_runtime_command(
            room_id="room-substrate-status",
            command="cancel",
            source_user="房主",
            reason="用户请求取消，prompt/provider 不应泄露",
        )
        worker._agent_runtime.record_sync_event(
            room_id="room-substrate-status",
            event={
                "event": "file_chunk_received",
                "asset_id": "asset-camp-table",
                "plan_id": plan.plan_id,
                "batch_id": "batch-review",
                "chunk_index": 2,
                "chunk_count": 4,
                "bytes_transferred": 2048,
                "total_bytes": 4096,
                "progress": 50,
                "asset_path": "E:/private/models/camp_table.glb",
                "message_id": "internal-message",
            },
            source="unit-test",
        )

        reply = worker._agent_runtime_status_reply(
            room_id="room-substrate-status",
            external_plan_id=plan.plan_id,
        )

        self.assertIn("主要模型", reply)
        self.assertIn("小木桌", reply)
        self.assertIn("帐篷", reply)
        self.assertIn("环境/地形", reply)
        self.assertIn("model/substrate", reply)
        self.assertIn("天空", reply)
        self.assertIn("草地", reply)
        self.assertIn("环境组件", reply)
        self.assertIn("skybox", reply)
        self.assertIn("terrain", reply)
        self.assertIn("ActorImport", reply)
        self.assertIn("registered actor", reply)
        self.assertIn("Closure", reply)
        self.assertIn("patch applied/conflict/invalid", reply)
        self.assertIn("审查", reply)
        self.assertIn("structure-review", reply)
        self.assertIn("审查建议", reply)
        self.assertIn("confirmed", reply)
        self.assertIn("host decision recorded", reply)
        self.assertNotIn("waiting host confirmation", reply)
        self.assertIn("审查确认", reply)
        self.assertIn("布局调整", reply)
        self.assertIn("proposal", reply)
        self.assertIn("deltas", reply)
        self.assertIn("Runtime 命令", reply)
        self.assertIn("cancel", reply)
        self.assertIn("模型同传", reply)
        self.assertIn("asset-camp-table", reply)
        self.assertIn("progress 50%", reply)
        self.assertNotIn("tool_name", reply)
        self.assertNotIn("provider", reply)
        self.assertNotIn("prompt", reply)
        self.assertNotIn("hidden layout prompt", reply)
        self.assertNotIn("asset_path", reply)
        self.assertNotIn("E:/private", reply)
        self.assertNotIn("internal-message", reply)


if __name__ == "__main__":
    unittest.main()
