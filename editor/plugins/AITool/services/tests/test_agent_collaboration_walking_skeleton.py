from __future__ import annotations

from dataclasses import replace
from datetime import datetime, timezone
from pathlib import Path
import unittest

import editor.plugins.AITool.services.agent_collaboration.walking_skeleton as walking_skeleton_module
from editor.plugins.AITool.services.tests.support._test_import_guard import assert_module_has_no_forbidden_imports
from editor.plugins.AITool.services.agent_collaboration import (
    ArtifactEnvelope,
    GameDesignBrief,
    NonExecutableArtifactError,
    assert_executable,
)
from editor.plugins.AITool.services.agent_collaboration.walking_skeleton import (
    EngineCapabilityManifest,
    SKELETON_NODE_ORDER,
    DemoScenarioRunner,
    UnavailableEngineCapabilityPort,
    build_skeleton_manifest,
    default_user_command_fixture,
)


class _CapabilityPort:
    def __init__(self, result) -> None:
        self._result = result

    def get_manifest(self):
        return self._result


class _FailOnceArtReasoner:
    def __init__(self) -> None:
        self.calls = 0
        self._delegate = walking_skeleton_module._ArtReasoner()

    def generate(self, request, context):
        self.calls += 1
        if self.calls == 1:
            raise RuntimeError("transient art failure")
        return self._delegate.generate(request, context)


class WalkingSkeletonTests(unittest.TestCase):
    @staticmethod
    def _runner() -> DemoScenarioRunner:
        return DemoScenarioRunner(
            engine_capabilities=UnavailableEngineCapabilityPort(),
            clock=lambda: datetime(2026, 7, 18, tzinfo=timezone.utc),
        )

    def test_fixture_runs_complete_non_executable_five_artifact_skeleton(self) -> None:
        result = self._runner().run(default_user_command_fixture())

        self.assertFalse(result.preflight.executable)
        self.assertEqual(result.preflight.status, "pending_runtime_verification")
        self.assertTrue(all(check.status == "passed" for check in result.preflight.checks))
        self.assertEqual(len(result.demo_result.artifact_refs), 5)
        self.assertFalse(result.demo_result.executable)
        self.assertEqual(result.demo_result.status, "integration_ready")
        self.assertEqual(result.demo_result.project_id, "project.walking-skeleton")
        self.assertTrue(result.demo_result.task_graph_id)
        self.assertEqual(len(result.demo_result.pending_runtime_verifications), 1)
        self.assertIn("operation:gameplay.apply_manifest", result.demo_result.required_capabilities)
        self.assertEqual(result.progress_event.status, "pending_runtime_verification")
        self.assertEqual(result.demo_result.blocked_results, result.progress_event.blocked_results)

    def test_baseline_node_order_status_and_engine_diagnostics_are_exact(self) -> None:
        result = self._runner().run(default_user_command_fixture())
        nodes = result.demo_result.skeleton_report.nodes

        self.assertEqual(tuple(node.node_id for node in nodes), SKELETON_NODE_ORDER)
        expected_status = {
            node_id: "pending_runtime_verification"
            if node_id in {"project_gate_preflight", "engine_capability_port"}
            else "completed"
            for node_id in SKELETON_NODE_ORDER
        }
        self.assertEqual({node.node_id: node.status for node in nodes}, expected_status)
        engine = next(node for node in nodes if node.node_id == "engine_capability_port")
        self.assertEqual(engine.blocker_code, "engine_capability_manifest_unavailable")
        self.assertEqual(engine.owner_domain, "engine")
        blocked = result.demo_result.blocked_results[0]
        self.assertEqual(blocked.owner_domain, "engine")
        self.assertEqual(blocked.missing_requirements[0].requirement_id, "engine.capability_manifest")
        self.assertTrue(blocked.next_action)

    def test_preflight_completes_only_with_the_full_declared_engine_contract(self) -> None:
        runner = DemoScenarioRunner(
            engine_capabilities=_CapabilityPort(
                EngineCapabilityManifest(
                    contract_version="r3-engine-week1-v1",
                    bridge_version="fixture-bridge-v1",
                    snapshot_schema_version="r3-scene-world-v1",
                    supported_operations=(
                        "actor_create",
                        "scene_snapshot.read",
                        "actual_aabb",
                        "render_ready",
                        "gameplay.apply_manifest",
                        "gameplay.preview.start",
                    ),
                    supported_gameplay_primitives=(
                        "on_enter",
                        "on_collect",
                        "set_state",
                        "unlock",
                        "complete_objective",
                    ),
                )
            ),
            clock=lambda: datetime(2026, 7, 18, tzinfo=timezone.utc),
        )

        result = runner.run(default_user_command_fixture())

        self.assertEqual(result.preflight.status, "completed")
        self.assertEqual(result.demo_result.status, "integration_ready")
        self.assertEqual(result.demo_result.blocked_results, ())

    def test_preflight_blocks_a_declared_engine_without_required_gameplay_capabilities(self) -> None:
        runner = DemoScenarioRunner(
            engine_capabilities=_CapabilityPort(
                EngineCapabilityManifest(
                    contract_version="r3-engine-week1-v1",
                    bridge_version="fixture-bridge-v1",
                    snapshot_schema_version="r3-scene-world-v1",
                    supported_operations=("actor_create",),
                    supported_gameplay_primitives=("on_collect",),
                )
            ),
            clock=lambda: datetime(2026, 7, 18, tzinfo=timezone.utc),
        )

        result = runner.run(default_user_command_fixture())

        self.assertEqual(result.preflight.status, "blocked")
        blocked = result.preflight.blocked_results[0]
        self.assertEqual(blocked.error_code, "engine_capability_missing")
        self.assertEqual(blocked.owner_domain, "engine")
        self.assertIn("engine.operation.actual_aabb", {item.requirement_id for item in blocked.missing_requirements})
        self.assertIn("engine.gameplay_primitive.unlock", {item.requirement_id for item in blocked.missing_requirements})

    def test_runner_retries_a_transient_agent_failure_using_task_graph_policy(self) -> None:
        reasoner = _FailOnceArtReasoner()
        runner = DemoScenarioRunner(
            engine_capabilities=UnavailableEngineCapabilityPort(),
            clock=lambda: datetime(2026, 7, 18, tzinfo=timezone.utc),
            art_reasoner=reasoner,
        )

        result = runner.run(default_user_command_fixture())

        self.assertEqual(reasoner.calls, 2)
        self.assertEqual(result.demo_result.status, "integration_ready")
        self.assertEqual(len(result.demo_result.artifact_refs), 5)

    def test_runner_replays_identical_command_once_and_rejects_changed_reuse(self) -> None:
        runner = self._runner()
        command = default_user_command_fixture()

        first = runner.run(command)
        replay = runner.run(command)

        self.assertIs(first, replay)
        with self.assertRaisesRegex(ValueError, "reused with different content"):
            runner.run(replace(command, project_goal="different goal"))

    def test_same_fixture_and_clock_produce_same_manifest_report_and_hash(self) -> None:
        first = self._runner().run(default_user_command_fixture())
        second = self._runner().run(default_user_command_fixture())

        self.assertEqual(first.manifest, second.manifest)
        self.assertEqual(first.manifest.contract_hash(), second.manifest.contract_hash())
        self.assertEqual(first.demo_result.skeleton_report, second.demo_result.skeleton_report)
        self.assertEqual(first.demo_result, second.demo_result)

    def test_contract_manifest_contains_fixed_nodes_edges_and_versions(self) -> None:
        manifest = build_skeleton_manifest()
        self.assertEqual(tuple(node_id for node_id, _ in manifest.skeleton_nodes), SKELETON_NODE_ORDER)
        self.assertEqual(
            manifest.skeleton_edges,
            tuple(zip(SKELETON_NODE_ORDER, SKELETON_NODE_ORDER[1:])),
        )
        self.assertTrue(manifest.contract_hash().startswith("sha256:"))
        self.assertEqual(len(dict(manifest.schema_versions)), 7)
        dto_fields = {item.name: item.fields for item in manifest.public_dtos}
        self.assertIn("payload_schema_version", dto_fields["PlanPatch"])
        self.assertIn("structured_payload", dto_fields["PlanPatch"])
        self.assertIn("payload_hash", dto_fields["PlanPatch"])
        self.assertIn("proposal_id", dto_fields["PlanPatch"])

    def test_collaboration_skeleton_has_no_runtime_frontend_or_cpp_import(self) -> None:
        assert_module_has_no_forbidden_imports(
            self,
            walking_skeleton_module,
            (
                "editor.plugins.AITool.services.agent_runtime",
                "editor.plugins.AITool.services.lanchat",
                "editor.plugins.AITool.services.schema_versions",
                "editor.Frontend",
                "src.systems",
            ),
        )
        source = Path(walking_skeleton_module.__file__).read_text(encoding="utf-8")
        for forbidden_constructor in (
            "ActionProposal(",
            "EntityBindingPlan(",
            "PlanPatch(",
            "ToolCallGraph(",
            "RuntimeCppBridge(",
            "EngineWriteGate(",
        ):
            self.assertNotIn(forbidden_constructor, source)

    def test_fixture_artifacts_cannot_cross_execution_boundary(self) -> None:
        result = self._runner().run(default_user_command_fixture())
        self.assertFalse(result.demo_result.executable)
        mock_artifact = ArtifactEnvelope(
            artifact_id="planning.mock-brief",
            artifact_type="GameDesignBrief",
            version=1,
            producer_role="planning",
            source_task_id="task.mock-planning",
            base_project_version=1,
            base_world_version=1,
            snapshot_source="mock",
            non_executable=True,
            status="validated",
            payload=GameDesignBrief(
                project_goal="Mock goal",
                player_experience=("inspect",),
                core_rules=("do not execute",),
                acceptance_criteria=("mock stays blocked",),
            ),
        )
        with self.assertRaises(NonExecutableArtifactError):
            assert_executable(mock_artifact)


if __name__ == "__main__":
    unittest.main()
