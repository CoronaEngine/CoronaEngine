from __future__ import annotations

import threading
import time
import unittest

from editor.plugins.AITool.services.agent_collaboration.coordinator import (
    CollaborationCoordinator,
    CollaborationInProgressError,
)
from editor.plugins.AITool.services.agent_collaboration.walking_skeleton import CollaborationStageEvent
from editor.plugins.AITool.services.collaboration_readonly_entry import CollaborationReadOnlyEntry
from editor.plugins.AITool.services.frontend_adapter import UserCommand
from editor.plugins.AITool.services.schema_versions import FRONTEND_INTERACTION_SCHEMA_VERSION


def _command(command_id: str, goal: str) -> UserCommand:
    return UserCommand(
        schema_version=FRONTEND_INTERACTION_SCHEMA_VERSION,
        command_id=command_id,
        room_id="room-collaboration",
        command_type="start_project",
        payload={
            "project_id": "project.collaboration",
            "scenario_id": "scenario.single-player",
            "project_goal": goal,
            "requested_by": "host",
        },
    )


class CollaborationCoordinatorTests(unittest.TestCase):
    def test_proposal_uses_planning_program_art_order_and_stable_identity(self) -> None:
        coordinator = CollaborationCoordinator()

        first = coordinator.create_proposal(_command("command.first", "设计一个钥匙开门卧室"))
        second = coordinator.create_proposal(_command("command.second", "改成迪士尼风格卧室"))

        self.assertEqual(first.proposal.proposal_id, second.proposal.proposal_id)
        self.assertEqual(first.proposal.proposal_version, 1)
        self.assertEqual(second.proposal.proposal_version, 2)
        self.assertNotEqual(first.proposal.proposal_hash, second.proposal.proposal_hash)
        self.assertEqual(second.progress_events, ())
        attempt = coordinator.last_attempt(second.proposal.project_id)
        self.assertIsNotNone(attempt)
        self.assertEqual(
            [(stage.stage, stage.status) for stage in attempt.stages],
            [
                ("planning", "completed"),
                ("program", "completed"),
                ("art", "completed"),
                ("narration", "not_started"),
            ],
        )
        self.assertTrue(all(ref.endswith("@2") for ref in second.proposal.artifact_refs))
        self.assertEqual(len(second.proposal.supersedes), 1)
        composition = second.proposal.artifact_payloads["SceneCompositionPlan"]
        logic = second.proposal.artifact_payloads["GameplayLogicPlan"]
        roles = {slot["semantic_role"] for slot in logic["entity_slots"]}
        self.assertTrue(roles.issubset(set(composition["entity_requirements"])))
        self.assertTrue(composition["image_prompts"])

    def test_semantically_unchanged_proposal_does_not_increment_version(self) -> None:
        coordinator = CollaborationCoordinator()
        first = coordinator.create_proposal(_command("command.same-1", "设计一个卧室"))
        second = coordinator.create_proposal(_command("command.same-2", "设计一个卧室"))

        self.assertEqual(second.revision_status, "unchanged")
        self.assertTrue(second.replayed)
        self.assertEqual(second.proposal.proposal_version, first.proposal.proposal_version)
        self.assertEqual(second.proposal.proposal_hash, first.proposal.proposal_hash)
        self.assertEqual(len(coordinator.history(first.proposal.project_id)), 1)

    def test_freeze_requires_current_version_and_hash(self) -> None:
        coordinator = CollaborationCoordinator()
        result = coordinator.create_proposal(_command("command.freeze", "设计一个卧室"))
        proposal = result.proposal

        with self.assertRaisesRegex(ValueError, "stale"):
            coordinator.freeze(
                project_id=proposal.project_id,
                proposal_id=proposal.proposal_id,
                proposal_version=proposal.proposal_version,
                proposal_hash="sha256:stale",
            )

        frozen = coordinator.freeze(
            project_id=proposal.project_id,
            proposal_id=proposal.proposal_id,
            proposal_version=proposal.proposal_version,
            proposal_hash=proposal.proposal_hash,
        )
        self.assertEqual(frozen.status, "frozen")

    def test_inflight_attempt_is_queryable_without_waiting_for_model_work(self) -> None:
        coordinator = CollaborationCoordinator()
        command = _command("command.slow", "设计一个慢速卧室方案")
        project_id = command.payload["project_id"]
        entered = threading.Event()
        release = threading.Event()

        class BlockingEntry:
            def run(self, value):
                coordinator.observe_stage(
                    project_id,
                    CollaborationStageEvent(
                        stage="planning",
                        status="completed",
                        artifact_refs=("GameDesignBrief", "LevelPlan"),
                    ),
                )
                entered.set()
                if not release.wait(2.0):
                    raise RuntimeError("test release timed out")
                return CollaborationReadOnlyEntry().run(value)

        result_box: list[object] = []

        def create() -> None:
            result_box.append(coordinator.create_proposal(command, readonly_entry=BlockingEntry()))

        thread = threading.Thread(target=create)
        thread.start()
        self.assertTrue(entered.wait(1.0))

        started = time.perf_counter()
        report = coordinator.last_attempt(project_id)
        elapsed = time.perf_counter() - started

        self.assertLess(elapsed, 0.2)
        self.assertIsNotNone(report)
        self.assertEqual(report.overall_status, "in_progress")
        self.assertEqual(report.stage("planning").status, "completed")
        self.assertEqual(report.stage("program").status, "in_progress")
        with self.assertRaises(CollaborationInProgressError):
            coordinator.create_proposal(_command("command.concurrent", "设计另一个方案"))

        release.set()
        thread.join(3.0)
        self.assertFalse(thread.is_alive())
        self.assertEqual(len(result_box), 1)


if __name__ == "__main__":
    unittest.main()
