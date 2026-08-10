from __future__ import annotations

import ast
from pathlib import Path
import unittest

from editor.plugins.AITool.services.agent_collaboration import (
    ArtifactEnvelope,
    GameDesignBrief,
    NonExecutableArtifactError,
    assert_executable,
)
from editor.plugins.AITool.services.agent_collaboration.walking_skeleton import build_skeleton_manifest
from editor.plugins.AITool.services.integration_contracts import BlockedResult
from editor.plugins.AITool.services.tests.support.engine_test_double import (
    EngineTestDouble,
    MockSceneSnapshot,
)


class EngineTestDoubleTests(unittest.TestCase):
    @staticmethod
    def _advance_to_terminal(double: EngineTestDouble, request_id: str) -> object:
        result = None
        for _ in range(4):
            result = double.advance(request_id)
        return result

    def test_normal_path_materializes_actual_aabb_and_snapshot_version(self) -> None:
        double = EngineTestDouble()

        accepted = double.create_actor(request_id="request.normal-001", entity_id="entity.normal-001")
        terminal = self._advance_to_terminal(double, "request.normal-001")
        snapshot = double.get_snapshot(expected_version=1)

        self.assertEqual(accepted.lifecycle_status, "create_accepted")
        self.assertEqual(terminal.lifecycle_status, "render_ready")
        self.assertTrue(terminal.geometry_ready)
        self.assertIsNotNone(terminal.actual_aabb)
        self.assertEqual(terminal.grounding_status, "grounded")
        self.assertEqual(terminal.support_status, "floor_supported")
        self.assertTrue(terminal.render_ready)
        self.assertEqual(snapshot.scene_version, 1)
        self.assertEqual(snapshot.snapshot_source, "mock")

    def test_late_ready_delays_geometry_until_controlled_cycles_are_consumed(self) -> None:
        double = EngineTestDouble(scenario="late_ready", late_ready_cycles=2)
        double.create_actor(request_id="request.late-001", entity_id="entity.late-001")

        first = double.advance("request.late-001")
        second = double.advance("request.late-001")
        third = double.advance("request.late-001")

        self.assertEqual(first.lifecycle_status, "waiting_for_geometry")
        self.assertEqual(second.lifecycle_status, "waiting_for_geometry")
        self.assertEqual(third.lifecycle_status, "geometry_ready")

    def test_partial_path_keeps_identity_but_withholds_aabb_and_sync_facts(self) -> None:
        double = EngineTestDouble(scenario="partial")
        accepted = double.create_actor(request_id="request.partial-001", entity_id="entity.partial-001")
        terminal = self._advance_to_terminal(double, "request.partial-001")
        snapshot = double.get_snapshot()

        self.assertEqual(accepted.actor_id, terminal.actor_id)
        self.assertTrue(terminal.geometry_ready)
        self.assertIsNone(terminal.actual_aabb)
        self.assertEqual(terminal.sync_status, "partial")
        self.assertEqual(snapshot.snapshot_source, "mock")

    def test_failure_path_returns_specific_blocked_result(self) -> None:
        result = EngineTestDouble(scenario="failure", failure_mode="timeout").create_actor(
            request_id="request.failure-001",
            entity_id="entity.failure-001",
        )

        self.assertIsInstance(result, BlockedResult)
        self.assertEqual(result.error_code, "engine_create_timeout")
        self.assertEqual(result.owner_domain, "engine")
        self.assertTrue(result.next_action)
        self.assertTrue(result.missing_requirements)

    def test_duplicate_request_reuses_stable_actor_identity(self) -> None:
        double = EngineTestDouble()

        first = double.create_actor(request_id="request.duplicate-001", entity_id="entity.duplicate-001")
        replay = double.create_actor(request_id="request.duplicate-001", entity_id="entity.other-001")

        self.assertEqual(first, replay)
        self.assertEqual(first.actor_id, "mock_actor_entity.duplicate-001")
        self.assertEqual(len(double.get_snapshot().actors), 1)

    def test_version_conflict_returns_specific_blocked_result(self) -> None:
        double = EngineTestDouble()
        double.create_actor(request_id="request.version-001", entity_id="entity.version-001")
        self._advance_to_terminal(double, "request.version-001")

        result = double.get_snapshot(expected_version=0)

        self.assertIsInstance(result, BlockedResult)
        self.assertEqual(result.error_code, "engine_snapshot_version_conflict")

    def test_capability_missing_returns_specific_blocked_result(self) -> None:
        double = EngineTestDouble(scenario="capability_missing")
        manifest = double.get_manifest()
        result = double.create_actor(
            request_id="request.capability-001",
            entity_id="entity.capability-001",
            primitive="unlock",
        )

        self.assertNotIn("unlock", manifest.supported_gameplay_primitives)
        self.assertIsInstance(result, BlockedResult)
        self.assertEqual(result.error_code, "engine_capability_primitive_missing")

    def test_mock_snapshot_is_forced_and_cannot_cross_execution_boundary(self) -> None:
        double = EngineTestDouble()
        snapshot = double.get_snapshot()
        artifact = ArtifactEnvelope(
            artifact_id="planning.mock-snapshot",
            artifact_type="GameDesignBrief",
            version=1,
            producer_role="planning",
            source_task_id="task.mock-snapshot",
            base_project_version=1,
            base_world_version=snapshot.scene_version,
            snapshot_source=snapshot.snapshot_source,
            non_executable=True,
            status="validated",
            payload=GameDesignBrief(
                project_goal="Keep mock snapshots out of execution.",
                player_experience=("inspect",),
                core_rules=("no execution",),
                acceptance_criteria=("mock is blocked",),
            ),
        )

        self.assertEqual(snapshot.snapshot_source, "mock")
        with self.assertRaises(NonExecutableArtifactError):
            assert_executable(artifact)

    def test_every_test_double_snapshot_is_forced_to_mock_and_cannot_be_overridden(self) -> None:
        for scenario in ("normal", "late_ready", "partial", "failure", "capability_missing"):
            self.assertEqual(EngineTestDouble(scenario=scenario).get_snapshot().snapshot_source, "mock")
        with self.assertRaises(ValueError):
            MockSceneSnapshot(
                room_id="room.override",
                scene_version=0,
                actors=(),
                snapshot_source="engine",
            )

    def test_test_double_is_not_imported_by_production_modules(self) -> None:
        services_root = Path(__file__).resolve().parent.parent
        violations = []
        for path in services_root.rglob("*.py"):
            relative = path.relative_to(services_root)
            if relative.parts[0] == "test_support" or path.name.startswith("test_"):
                continue
            tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
            for node in ast.walk(tree):
                if isinstance(node, ast.ImportFrom) and node.module and "test_support" in node.module:
                    violations.append(str(relative))
                if isinstance(node, ast.Import) and any("test_support" in alias.name for alias in node.names):
                    violations.append(str(relative))
        self.assertEqual(violations, [])

    def test_test_double_preserves_frozen_skeleton_contract_hash(self) -> None:
        self.assertEqual(
            build_skeleton_manifest().contract_hash(),
            "sha256:822bbe65a1bfd410cc03bdc761059762f66501daa7f285c9ac48e0f54c889a71",
        )


if __name__ == "__main__":
    unittest.main()
