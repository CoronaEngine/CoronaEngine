from __future__ import annotations

import unittest

import editor.plugins.AITool.services.frontend_adapter as frontend_adapter_module
from editor.plugins.AITool.services.tests.support._test_import_guard import assert_module_has_no_forbidden_imports
from editor.plugins.AITool.services.frontend_adapter import (
    FrontendBusinessProtocolAdapter,
)
from editor.plugins.AITool.services.schema_versions import FRONTEND_INTERACTION_SCHEMA_VERSION


def _command(*, command_id: str = "command.frontend-001", command_type: str = "start_project") -> dict[str, object]:
    return {
        "schema_version": FRONTEND_INTERACTION_SCHEMA_VERSION,
        "command_id": command_id,
        "room_id": "room.frontend-demo",
        "command_type": command_type,
        "payload": {
            "project_id": "project.frontend-demo",
            "task_id": "task.frontend-demo",
            "plan_id": "plan.frontend-demo",
            "scene_version": 3,
        },
    }


class FrontendBusinessProtocolAdapterTests(unittest.TestCase):
    def test_valid_command_produces_deterministic_progress_event(self) -> None:
        adapter = FrontendBusinessProtocolAdapter()

        result = adapter.dispatch(_command())

        self.assertEqual(result.status, "accepted")
        self.assertEqual(len(result.events), 1)
        event = result.events[0]
        self.assertEqual(event.event_id, "event.command.frontend-001.project_start_requested")
        self.assertEqual(event.command_id, "command.frontend-001")
        self.assertEqual(event.project_id, "project.frontend-demo")
        self.assertEqual(event.scene_version, 3)
        self.assertEqual(event.event_type, "project_start_requested")

    def test_unknown_command_type_returns_specific_blocked_result(self) -> None:
        adapter = FrontendBusinessProtocolAdapter()

        result = adapter.dispatch(_command(command_type="create_actor"))

        self.assertEqual(result.status, "blocked")
        self.assertEqual(result.blocked_result.error_code, "unknown_command_type")
        self.assertEqual(result.blocked_result.owner_domain, "frontend")
        self.assertTrue(result.blocked_result.next_action)
        self.assertTrue(result.blocked_result.missing_requirements)

    def test_incompatible_schema_returns_specific_blocked_result(self) -> None:
        adapter = FrontendBusinessProtocolAdapter()
        command = _command()
        command["schema_version"] = "frontend-v0"

        result = adapter.dispatch(command)

        self.assertEqual(result.status, "blocked")
        self.assertEqual(result.blocked_result.error_code, "frontend_schema_version_incompatible")

    def test_command_replay_emits_no_second_business_event(self) -> None:
        adapter = FrontendBusinessProtocolAdapter()
        command = _command()

        first = adapter.dispatch(command)
        replay = adapter.dispatch(command)

        self.assertEqual(first.status, "accepted")
        self.assertEqual(len(first.events), 1)
        self.assertEqual(replay.status, "blocked")
        self.assertEqual(replay.events, ())
        self.assertEqual(replay.blocked_result.error_code, "duplicate_command_id")

    def test_progress_event_replay_is_deduplicated(self) -> None:
        adapter = FrontendBusinessProtocolAdapter()
        event = adapter.dispatch(_command()).events[0]

        replay = adapter.forward_event(event)

        self.assertEqual(replay.error_code, "duplicate_event_id")
        self.assertEqual(replay.owner_domain, "frontend")

    def test_progress_event_preserves_origin_turn_identifiers(self) -> None:
        adapter = FrontendBusinessProtocolAdapter()
        event = adapter.forward_event({
            "schema_version": FRONTEND_INTERACTION_SCHEMA_VERSION,
            "event_id": "event.origin.progress",
            "command_id": "command.origin",
            "room_id": "room.origin",
            "project_id": "project.origin",
            "task_id": "task.origin",
            "plan_id": "",
            "scene_version": 0,
            "event_type": "planning_in_progress",
            "status": "in_progress",
            "detail": {"owner_role": "planning"},
            "origin_message_id": "message:origin-1",
            "origin_correlation_id": "correlation:origin-1",
        })

        self.assertEqual(event.origin_message_id, "message:origin-1")
        self.assertEqual(event.origin_correlation_id, "correlation:origin-1")

    def test_adapter_has_no_runtime_or_engine_write_dependency(self) -> None:
        assert_module_has_no_forbidden_imports(
            self,
            frontend_adapter_module,
            (
                "editor.plugins.AITool.services.agent_runtime",
                "editor.plugins.AITool.services.lanchat_agent_worker",
                "editor.Frontend",
                "src.systems",
            ),
        )


if __name__ == "__main__":
    unittest.main()
