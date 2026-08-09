from __future__ import annotations

import json
from pathlib import Path
import sys
import unittest

_EDITOR_ROOT = Path(__file__).resolve().parents[5] / "editor"
if str(_EDITOR_ROOT) not in sys.path:
    sys.path.insert(0, str(_EDITOR_ROOT))

from editor.plugins.AITool.services.agent_runtime.flags import AgentRuntimeFlags
from editor.plugins.AITool.services.collaboration_readonly_entry import (
    CollaborationReadOnlyEntry,
)
from editor.plugins.AITool.services.lanchat_agent_worker import LANChatAgentWorker


class _ReplyProbeEngine:
    def __init__(self) -> None:
        self.agent_replies: list[dict[str, str]] = []
        self.system_messages: list[dict[str, str]] = []

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
        self.agent_replies.append({
            "agent_id": agent_id,
            "agent_name": agent_name,
            "text": text,
            "message_kind": message_kind,
            "target_agent_id": target_agent_id,
            "correlation_id": correlation_id,
            "metadata_json": metadata_json,
        })
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


class _CountingCollaborationEntry:
    def __init__(self) -> None:
        self.delegate = CollaborationReadOnlyEntry()
        self.run_calls = 0

    def run(self, command):  # noqa: ANN001, ANN201
        self.run_calls += 1
        return self.delegate.run(command)


class _NoControlOrchestrator:
    @staticmethod
    def handle_control_trigger(trigger):  # noqa: ANN001, ANN201
        return None


class _CollaborationRouteWorker(LANChatAgentWorker):
    """Keep this test on the LANChat route without touching Runtime facts."""

    def __init__(self, engine: _ReplyProbeEngine) -> None:
        super().__init__(
            corona_engine=engine,
            agent_runtime=object(),
            agent_runtime_flags=AgentRuntimeFlags.from_env({}),
            async_agent_execution=False,
        )
        self.entry = _CountingCollaborationEntry()
        self._collaboration_readonly_entry = self.entry
        self.legacy_path_calls: list[str] = []

    def _can_execute_agent_locally(self) -> bool:
        return True

    def _get_orchestrator(self):  # noqa: ANN201
        return _NoControlOrchestrator()

    def _pending_discussion_confirmation_reply(self, trigger):  # noqa: ANN001, ANN201
        return None

    def _runtime_action_intent_for_trigger(self, *args, **kwargs):  # noqa: ANN002, ANN003, ANN201
        return None

    def _active_runtime_execution_plan_id(self, room_id: str) -> str:
        return ""

    def _latest_runtime_completed_plan_id(self, room_id: str) -> str:
        return ""

    def _record_agent_reply_send_in_agent_runtime(self, **kwargs) -> None:  # noqa: ANN003
        return None

    def _mirror_agent_reply_context_in_agent_runtime(self, **kwargs) -> None:  # noqa: ANN003
        return None

    def _record_model_call_summary(self, trigger) -> None:  # noqa: ANN001
        return None

    def _seed_agent_trigger_planning_context_in_runtime(self, trigger):  # noqa: ANN001, ANN201
        self.legacy_path_calls.append("seed_plan")
        raise AssertionError("start_project must stop before the legacy planning seed")

    def _handle_agent_trigger_planning_gate(self, trigger):  # noqa: ANN001, ANN201
        self.legacy_path_calls.append("planning_gate")
        raise AssertionError("start_project must stop before the legacy planning gate")

    def _handle_agent_trigger_runtime_write_gate(self, trigger, *, planning_seed=None):  # noqa: ANN001, ANN201
        self.legacy_path_calls.append("runtime_write_gate")
        raise AssertionError("start_project must not enter the Runtime write gate")

    def _run_agent(self, trigger):  # noqa: ANN001, ANN201
        self.legacy_path_calls.append("role_agent")
        raise AssertionError("start_project must not invoke the legacy RoleAgent")


def _neutralize_pre_entry_handlers(worker: _CollaborationRouteWorker) -> None:
    names = (
        "_handle_coordinator_gm_control",
        "_handle_runtime_entity_status_query",
        "_handle_runtime_action_clarification",
        "_handle_coordinator_gm_clarification",
        "_handle_agent_runtime_command",
        "_handle_agent_runtime_worker_drain_query",
        "_handle_agent_runtime_provider_status_query",
        "_handle_agent_runtime_engine_write_status_query",
        "_handle_agent_runtime_scene_snapshot_query",
        "_handle_agent_runtime_tool_manifest_query",
        "_handle_agent_runtime_operation_replay_query",
        "_handle_agent_runtime_report_query",
        "_handle_agent_runtime_sync_status_query",
        "_handle_agent_runtime_gm_summary_query",
        "_handle_coordinator_status_query",
        "_handle_agent_runtime_enqueue_generation_query",
        "_handle_coordinator_generation_start",
        "_handle_coordinator_completed_intervention",
        "_handle_coordinator_executing_intervention",
    )
    for name in names:
        setattr(worker, name, lambda *args, **kwargs: None)


class LANChatCollaborationReadOnlyEntryTests(unittest.TestCase):
    def _worker(self) -> tuple[_CollaborationRouteWorker, _ReplyProbeEngine]:
        engine = _ReplyProbeEngine()
        worker = _CollaborationRouteWorker(engine)
        _neutralize_pre_entry_handlers(worker)
        return worker, engine

    @staticmethod
    def _structured_trigger() -> dict:
        return {
            "room_id": "room-collaboration",
            "message_id": "message-start-project",
            "correlation_id": "message-start-project",
            "sender_type": "user",
            "sender_id": "host",
            "sender_name": "Host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "text": "Start a small single-player key and door demo",
            "metadata": {
                "command_type": "start_project",
                "payload": {
                    "command_id": "command.start-project",
                    "project_id": "project.single-player-demo",
                    "scenario_id": "scenario.key-door-goal",
                    "project_goal": "Create a key, locked door, and goal demo",
                },
            },
        }

    def test_structured_start_project_uses_readonly_entry_once(self) -> None:
        worker, engine = self._worker()

        self.assertTrue(worker._process_trigger(self._structured_trigger()))

        self.assertEqual(worker.entry.run_calls, 1)
        self.assertEqual(worker.legacy_path_calls, [])
        self.assertEqual(len(engine.agent_replies), 1)
        self.assertEqual(engine.agent_replies[0]["message_kind"], "agent_reply")
        self.assertIn("Artifact", engine.agent_replies[0]["text"])
        self.assertEqual(len(engine.system_messages), 2)
        self.assertEqual(
            {message["message_kind"] for message in engine.system_messages},
            {"action_status"},
        )
        event_types = {
            json.loads(message["metadata_json"])["progress_event"]["event_type"]
            for message in engine.system_messages
        }
        self.assertEqual(
            event_types,
            {"project_start_requested", "collaboration_result_ready"},
        )

    def test_slash_command_routes_and_message_replay_has_no_second_effect(self) -> None:
        worker, engine = self._worker()
        trigger = {
            "room_id": "room-slash-command",
            "message_id": "message-slash-command",
            "correlation_id": "message-slash-command",
            "sender_type": "user",
            "sender_id": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "text": "/start_project Create a compact key and door demo",
        }

        self.assertTrue(worker._process_trigger(dict(trigger)))
        self.assertTrue(worker._process_trigger(dict(trigger)))

        self.assertEqual(worker.entry.run_calls, 1)
        self.assertEqual(len(engine.agent_replies), 1)
        self.assertEqual(len(engine.system_messages), 2)
        self.assertEqual(worker.legacy_path_calls, [])

    def test_missing_goal_clarifies_without_artifact_or_progress_event(self) -> None:
        worker, engine = self._worker()
        trigger = {
            "room_id": "room-empty-command",
            "message_id": "message-empty-command",
            "correlation_id": "message-empty-command",
            "sender_type": "user",
            "sender_id": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "text": "/start_project",
        }

        self.assertTrue(worker._process_trigger(trigger))

        self.assertEqual(worker.entry.run_calls, 0)
        self.assertEqual(len(engine.agent_replies), 1)
        self.assertIn("没有创建 Artifact", engine.agent_replies[0]["text"])
        self.assertEqual(engine.system_messages, [])
        self.assertEqual(worker.legacy_path_calls, [])

    def test_ordinary_chat_is_not_claimed_by_collaboration_entry(self) -> None:
        worker, _ = self._worker()
        trigger = {
            "room_id": "room-ordinary-chat",
            "message_id": "message-ordinary-chat",
            "text": "How is the project going?",
        }

        self.assertFalse(worker._is_collaboration_start_project_trigger(trigger))
        self.assertIsNone(worker._handle_collaboration_start_project(trigger))
        self.assertEqual(worker.entry.run_calls, 0)


if __name__ == "__main__":
    unittest.main()
