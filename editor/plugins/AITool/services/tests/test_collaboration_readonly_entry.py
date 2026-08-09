from __future__ import annotations

from datetime import datetime, timezone
from pathlib import Path
import unittest

from editor.plugins.AITool.services.collaboration_readonly_entry import (
    CollaborationReadOnlyEntry,
)
from editor.plugins.AITool.services.frontend_adapter import UserCommand
from editor.plugins.AITool.services.schema_versions import FRONTEND_INTERACTION_SCHEMA_VERSION


def _command(*, command_id: str = "command.collaboration-001", goal: str = "Create a short demo") -> UserCommand:
    return UserCommand(
        schema_version=FRONTEND_INTERACTION_SCHEMA_VERSION,
        command_id=command_id,
        room_id="room.collaboration",
        command_type="start_project",
        payload={
            "project_id": "project.collaboration",
            "scenario_id": "scenario.collaboration",
            "project_goal": goal,
            "requested_by": "host",
        },
    )


class CollaborationReadOnlyEntryTests(unittest.TestCase):
    def _entry(self) -> CollaborationReadOnlyEntry:
        return CollaborationReadOnlyEntry(
            clock=lambda: datetime(2026, 7, 20, 0, 0, tzinfo=timezone.utc)
        )

    def test_start_project_runs_formal_artifact_chain_without_execution(self) -> None:
        result = self._entry().run(_command())

        self.assertEqual(result.status, "accepted")
        self.assertFalse(result.executable)
        self.assertIsNotNone(result.run_result)
        self.assertFalse(result.run_result.demo_result.executable)
        self.assertEqual(len(result.run_result.demo_result.artifact_refs), 5)
        self.assertEqual(len(result.progress_events), 2)
        self.assertEqual(result.progress_events[0].event_type, "project_start_requested")
        self.assertEqual(result.progress_events[1].event_type, "collaboration_result_ready")
        self.assertFalse(result.progress_events[1].detail["executable"])
        self.assertGreater(
            result.progress_events[1].detail["pending_runtime_verification_count"],
            0,
        )

    def test_same_command_replay_returns_cached_result_and_no_new_event(self) -> None:
        entry = self._entry()
        command = _command()
        first = entry.run(command)
        replay = entry.run(command)

        self.assertEqual(first.status, "accepted")
        self.assertEqual(replay.status, "replayed")
        self.assertIs(replay.run_result, first.run_result)
        self.assertEqual(replay.progress_events, ())

    def test_command_id_content_conflict_fails_closed(self) -> None:
        entry = self._entry()
        entry.run(_command(goal="First goal"))

        conflict = entry.run(_command(goal="Different goal"))

        self.assertEqual(conflict.status, "blocked")
        self.assertEqual(
            conflict.blocked_result.error_code,
            "collaboration_command_content_conflict",
        )
        self.assertIsNone(conflict.run_result)

    def test_non_start_command_is_blocked(self) -> None:
        command = UserCommand(
            schema_version=FRONTEND_INTERACTION_SCHEMA_VERSION,
            command_id="command.collaboration-query",
            room_id="room.collaboration",
            command_type="query_status",
            payload={},
        )

        result = self._entry().run(command)

        self.assertEqual(result.status, "blocked")
        self.assertEqual(
            result.blocked_result.error_code,
            "collaboration_command_not_readonly_start",
        )

    def test_integration_entry_does_not_import_runtime_or_lanchat(self) -> None:
        source = Path(
            "editor/plugins/AITool/services/collaboration_readonly_entry.py"
        ).read_text(encoding="utf-8")

        self.assertNotIn("agent_runtime", source)
        self.assertNotIn("lanchat_agent_worker", source)
        self.assertNotIn("ActionProposal", source)
        self.assertNotIn("PlanPatch", source)
        self.assertNotIn("ToolCallGraph", source)


if __name__ == "__main__":
    unittest.main()
