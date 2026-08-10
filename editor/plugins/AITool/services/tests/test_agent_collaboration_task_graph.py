from __future__ import annotations

import threading
import unittest

import editor.plugins.AITool.services.agent_collaboration.task_graph as task_graph_module
from editor.plugins.AITool.services.agent_collaboration import (
    AgentTask,
    AgentTaskGraphStore,
    ArtDirection,
    ArtifactEnvelope,
    ArtifactRegistry,
    GameDesignBrief,
    GameplayEntitySlot,
    GameplayLogicPlan,
    GameplayPrimitiveSpec,
    ProjectStatePatch,
    ProjectStateStore,
    TaskGraphAlreadyExistsError,
    TaskGraphValidationError,
    TaskOutputValidationError,
    TaskTransitionError,
)
from editor.plugins.AITool.services.tests.support._test_import_guard import assert_module_has_no_forbidden_imports


def _task(
    task_id: str,
    role: str,
    output_type: str,
    *,
    inputs: tuple[str, ...] = (),
    depends_on: tuple[str, ...] = (),
    max_attempts: int = 2,
) -> AgentTask:
    return AgentTask(
        task_id=task_id,
        assigned_role=role,
        objective=f"Produce {output_type}",
        input_artifact_refs=inputs,
        output_types=(output_type,),
        depends_on=depends_on,
        acceptance_criteria=("schema valid", "dependencies current"),
        capability_set=("artifact.write",),
        max_attempts=max_attempts,
    )


def _artifact(
    *,
    artifact_id: str,
    artifact_type: str,
    role: str,
    task_id: str,
    version: int,
    base_project_version: int,
    dependencies: tuple[str, ...] = (),
    label: str = "v1",
    base_world_version: int = 0,
    snapshot_source: str = "none",
) -> ArtifactEnvelope:
    if artifact_type == "GameDesignBrief":
        payload = GameDesignBrief(
            project_goal=f"cooperative room {label}",
            player_experience=("explore",),
            core_rules=("host authoritative",),
            acceptance_criteria=("shared world",),
        )
    elif artifact_type == "ArtDirection":
        payload = ArtDirection(
            style_keywords=("coherent", label),
            palette=("amber",),
            lighting=("warm",),
            avoid_keywords=("horror",),
        )
    elif artifact_type == "GameplayLogicPlan":
        payload = GameplayLogicPlan(
            states=("ready", label),
            entity_slots=(
                GameplayEntitySlot("item", "collectible_item", ("collectible",)),
                GameplayEntitySlot("player", "player_spawn", ("player",)),
            ),
            primitives=(
                GameplayPrimitiveSpec("collect-item", "on_collect", "item", "player", {"state_key": "ready", "set_value": True}),
            ),
            triggers=("enter",),
            rules=("authoritative state",),
            win_conditions=("goal reached",),
            lose_conditions=("timeout",),
        )
    else:
        raise AssertionError(artifact_type)
    return ArtifactEnvelope(
        artifact_id=artifact_id,
        artifact_type=artifact_type,
        version=version,
        producer_role=role,
        source_task_id=task_id,
        base_project_version=base_project_version,
        base_world_version=base_world_version,
        dependencies=dependencies,
        snapshot_source=snapshot_source,
        status="validated",
        payload=payload,
    )


class AgentTaskGraphTests(unittest.TestCase):
    def setUp(self) -> None:
        self.projects = ProjectStateStore()
        self.projects.create_project(project_id="project-1", room_id="room-1", source="test")
        self.artifacts = ArtifactRegistry(self.projects)
        self.graphs = AgentTaskGraphStore(self.projects, self.artifacts)

    def _tasks(self, *, max_attempts: int = 2) -> tuple[AgentTask, ...]:
        return (
            _task("task-plan", "planning", "GameDesignBrief", max_attempts=max_attempts),
            _task(
                "task-art",
                "art",
                "ArtDirection",
                inputs=("brief@1",),
                depends_on=("task-plan",),
                max_attempts=max_attempts,
            ),
            _task(
                "task-program",
                "program",
                "GameplayLogicPlan",
                inputs=("art@1",),
                depends_on=("task-art",),
                max_attempts=max_attempts,
            ),
        )

    def _create_graph(self, *, max_attempts: int = 2):
        return self.graphs.create_graph(
            graph_id="graph-1",
            project_id="project-1",
            tasks=self._tasks(max_attempts=max_attempts),
            expected_project_version=1,
            patch_id="patch-create-graph",
            source="test",
        )

    def _publish_and_complete(
        self,
        task_id: str,
        *,
        artifact_id: str,
        artifact_type: str,
        role: str,
        version: int,
        dependencies: tuple[str, ...] = (),
        label: str = "v1",
    ) -> None:
        self.graphs.start_task("graph-1", task_id, source="test")
        project_version = self.projects.get("project-1").project_version
        artifact = _artifact(
            artifact_id=artifact_id,
            artifact_type=artifact_type,
            role=role,
            task_id=task_id,
            version=version,
            base_project_version=project_version,
            dependencies=dependencies,
            label=label,
        )
        self.artifacts.register(
            project_id="project-1",
            artifact=artifact,
            expected_project_version=project_version,
            patch_id=f"patch-{artifact_id}-{version}",
            source="test",
        )
        self.graphs.complete_task(
            "graph-1",
            task_id,
            output_artifact_refs=(f"{artifact_id}@{version}",),
            source="test",
        )

    def test_dependency_order_promotes_only_satisfied_tasks(self) -> None:
        graph = self._create_graph()

        self.assertEqual(graph.status, "ready")
        self.assertEqual(graph.task("task-plan").status, "ready")
        self.assertEqual(graph.task("task-plan").task.status, "ready")
        self.assertEqual(graph.task("task-art").status, "pending")
        self.assertEqual(graph.task("task-program").status, "pending")
        self.assertEqual(self.projects.get("project-1").active_task_graph_id, "graph-1")
        self.assertEqual(self.projects.get("project-1").project_version, 2)

        self._publish_and_complete(
            "task-plan",
            artifact_id="brief",
            artifact_type="GameDesignBrief",
            role="planning",
            version=1,
        )
        graph = self.graphs.get("graph-1")
        self.assertEqual(graph.task("task-plan").status, "completed")
        self.assertEqual(graph.task("task-art").status, "ready")
        self.assertEqual(graph.task("task-program").status, "pending")

    def test_three_role_graph_reaches_completed_with_auditable_outputs(self) -> None:
        self._create_graph()
        self._publish_and_complete(
            "task-plan",
            artifact_id="brief",
            artifact_type="GameDesignBrief",
            role="planning",
            version=1,
        )
        self._publish_and_complete(
            "task-art",
            artifact_id="art",
            artifact_type="ArtDirection",
            role="art",
            version=1,
            dependencies=("brief@1",),
        )
        self._publish_and_complete(
            "task-program",
            artifact_id="logic",
            artifact_type="GameplayLogicPlan",
            role="program",
            version=1,
            dependencies=("art@1",),
        )

        graph = self.graphs.get("graph-1")
        self.assertEqual(graph.status, "completed")
        self.assertEqual(
            graph.task("task-program").output_artifact_refs,
            ("logic@1",),
        )
        self.assertTrue(all(record.task.status == "completed" for record in graph.tasks.values()))
        self.assertEqual(self.graphs.history("graph-1")[-1].action, "task_completed")

    def test_failure_and_retry_only_reopens_responsible_task(self) -> None:
        self._create_graph()
        self._publish_and_complete(
            "task-plan",
            artifact_id="brief",
            artifact_type="GameDesignBrief",
            role="planning",
            version=1,
        )
        self.graphs.start_task("graph-1", "task-art", source="test")
        failed = self.graphs.fail_task(
            "graph-1",
            "task-art",
            error="provider unavailable",
            source="test",
        )

        self.assertEqual(failed.task("task-plan").status, "completed")
        self.assertEqual(failed.task("task-art").status, "failed")
        self.assertEqual(failed.task("task-program").status, "blocked")
        self.assertEqual(failed.task("task-art").attempt_count, 1)

        retried = self.graphs.retry_task("graph-1", "task-art", source="test")
        self.assertEqual(retried.task("task-plan").status, "completed")
        self.assertEqual(retried.task("task-art").status, "ready")
        self.assertEqual(retried.task("task-program").status, "pending")

    def test_retry_budget_is_enforced(self) -> None:
        self._create_graph(max_attempts=1)
        self.graphs.start_task("graph-1", "task-plan", source="test")
        self.graphs.fail_task("graph-1", "task-plan", error="bad output", source="test")
        with self.assertRaises(TaskTransitionError):
            self.graphs.retry_task("graph-1", "task-plan", source="test")

    def test_stale_input_blocks_precise_downstream_and_rebind_reopens_it(self) -> None:
        self._create_graph()
        self._publish_and_complete(
            "task-plan",
            artifact_id="brief",
            artifact_type="GameDesignBrief",
            role="planning",
            version=1,
        )
        self._publish_and_complete(
            "task-art",
            artifact_id="art",
            artifact_type="ArtDirection",
            role="art",
            version=1,
            dependencies=("brief@1",),
        )
        project_version = self.projects.get("project-1").project_version
        brief_v2 = _artifact(
            artifact_id="brief",
            artifact_type="GameDesignBrief",
            role="planning",
            task_id="task-plan",
            version=2,
            base_project_version=project_version,
            label="v2",
        )
        self.artifacts.register(
            project_id="project-1",
            artifact=brief_v2,
            expected_project_version=project_version,
            patch_id="patch-brief-2",
            source="test",
        )

        refreshed = self.graphs.refresh("graph-1", source="test")
        self.assertEqual(refreshed.task("task-plan").status, "completed")
        self.assertEqual(refreshed.task("task-art").status, "blocked")
        self.assertEqual(
            {reason.code for reason in refreshed.task("task-art").blocked_reasons},
            {"input_artifact_not_usable"},
        )
        self.assertEqual(refreshed.task("task-program").status, "blocked")

        rebound = self.graphs.rebind_inputs(
            "graph-1",
            "task-art",
            input_artifact_refs=("brief@2",),
            source="test",
        )
        self.assertEqual(rebound.task("task-art").status, "ready")
        self.assertEqual(rebound.task("task-art").attempt_count, 0)
        self.assertEqual(rebound.task("task-art").output_artifact_refs, ())
        self.assertEqual(rebound.task("task-program").status, "pending")

    def test_world_version_advance_blocks_task_bound_to_old_runtime_artifact(self) -> None:
        self.projects.apply_patch(ProjectStatePatch(
            patch_id="patch-world-v1",
            project_id="project-1",
            expected_project_version=1,
            source="test",
            changes={"scene_world_version": 1},
        ))
        runtime_brief = _artifact(
            artifact_id="brief",
            artifact_type="GameDesignBrief",
            role="planning",
            task_id="fixture-runtime-brief",
            version=1,
            base_project_version=2,
            base_world_version=1,
            snapshot_source="runtime",
        )
        self.artifacts.register(
            project_id="project-1",
            artifact=runtime_brief,
            expected_project_version=2,
            patch_id="patch-register-runtime-brief",
            source="test",
        )
        graph = self.graphs.create_graph(
            graph_id="graph-runtime-input",
            project_id="project-1",
            tasks=(
                _task(
                    "task-art-runtime",
                    "art",
                    "ArtDirection",
                    inputs=("brief@1",),
                ),
            ),
            expected_project_version=3,
            patch_id="patch-create-runtime-input-graph",
            source="test",
        )
        self.assertEqual(graph.task("task-art-runtime").status, "ready")
        self.projects.apply_patch(ProjectStatePatch(
            patch_id="patch-world-v2",
            project_id="project-1",
            expected_project_version=4,
            source="test",
            changes={"scene_world_version": 2},
        ))

        with self.assertRaisesRegex(TaskTransitionError, "expected ready, got blocked"):
            self.graphs.start_task("graph-runtime-input", "task-art-runtime", source="test")
        self.assertEqual(
            self.graphs.get("graph-runtime-input").task("task-art-runtime").attempt_count,
            0,
        )

        refreshed = self.graphs.refresh("graph-runtime-input", source="test")

        self.assertEqual(refreshed.task("task-art-runtime").status, "blocked")
        self.assertEqual(
            {reason.code for reason in refreshed.task("task-art-runtime").blocked_reasons},
            {"input_artifact_not_usable"},
        )

    def test_completion_requires_current_outputs_from_the_responsible_task(self) -> None:
        self._create_graph()
        self.graphs.start_task("graph-1", "task-plan", source="test")
        project_version = self.projects.get("project-1").project_version
        wrong_owner = _artifact(
            artifact_id="brief",
            artifact_type="GameDesignBrief",
            role="planning",
            task_id="other-task",
            version=1,
            base_project_version=project_version,
        )
        self.artifacts.register(
            project_id="project-1",
            artifact=wrong_owner,
            expected_project_version=project_version,
            patch_id="patch-wrong-owner",
            source="test",
        )
        with self.assertRaises(TaskOutputValidationError):
            self.graphs.complete_task(
                "graph-1",
                "task-plan",
                output_artifact_refs=("brief@1",),
                source="test",
            )
        self.assertEqual(self.graphs.get("graph-1").task("task-plan").status, "in_progress")

    def test_cycle_and_unknown_dependency_are_rejected_without_project_patch(self) -> None:
        cycle = (
            _task(
                "task-a",
                "planning",
                "GameDesignBrief",
                depends_on=("task-b",),
            ),
            _task(
                "task-b",
                "art",
                "ArtDirection",
                depends_on=("task-a",),
            ),
        )
        with self.assertRaises(TaskGraphValidationError):
            self.graphs.create_graph(
                graph_id="cycle",
                project_id="project-1",
                tasks=cycle,
                expected_project_version=1,
                patch_id="patch-cycle",
                source="test",
            )
        self.assertEqual(self.projects.get("project-1").project_version, 1)

        unknown = (
            _task(
                "task-a",
                "planning",
                "GameDesignBrief",
                depends_on=("missing",),
            ),
        )
        with self.assertRaises(TaskGraphValidationError):
            self.graphs.create_graph(
                graph_id="unknown",
                project_id="project-1",
                tasks=unknown,
                expected_project_version=1,
                patch_id="patch-unknown",
                source="test",
            )

    def test_graph_creation_is_idempotent_but_changed_definition_conflicts(self) -> None:
        first = self._create_graph()
        replay = self.graphs.create_graph(
            graph_id="graph-1",
            project_id="project-1",
            tasks=self._tasks(),
            expected_project_version=1,
            patch_id="patch-create-graph",
            source="test",
        )
        self.assertIs(first, replay)
        self.assertEqual(self.projects.get("project-1").project_version, 2)

        changed = self._tasks() + (
            _task("task-extra", "planning", "LevelPlan"),
        )
        with self.assertRaises(TaskGraphAlreadyExistsError):
            self.graphs.create_graph(
                graph_id="graph-1",
                project_id="project-1",
                tasks=changed,
                expected_project_version=2,
                patch_id="patch-changed-graph",
                source="test",
            )

        self.projects.create_project(project_id="project-2", room_id="room-2", source="test")
        with self.assertRaises(TaskGraphAlreadyExistsError):
            self.graphs.create_graph(
                graph_id="graph-1",
                project_id="project-2",
                tasks=self._tasks(),
                expected_project_version=1,
                patch_id="patch-cross-project-replay",
                source="test",
            )

    def test_concurrent_start_claims_ready_task_once(self) -> None:
        self._create_graph()
        outcomes: list[str] = []
        lock = threading.Lock()

        def start() -> None:
            try:
                self.graphs.start_task("graph-1", "task-plan", source="thread")
                outcome = "started"
            except TaskTransitionError:
                outcome = "rejected"
            with lock:
                outcomes.append(outcome)

        threads = [threading.Thread(target=start) for _ in range(2)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()

        self.assertEqual(sorted(outcomes), ["rejected", "started"])
        record = self.graphs.get("graph-1").task("task-plan")
        self.assertEqual(record.status, "in_progress")
        self.assertEqual(record.attempt_count, 1)

    def test_transition_history_tracks_only_changed_graph_versions(self) -> None:
        graph = self._create_graph()
        refreshed = self.graphs.refresh("graph-1", source="test")
        self.assertIs(graph, refreshed)
        self.assertEqual(len(self.graphs.history("graph-1")), 1)

        self.graphs.start_task("graph-1", "task-plan", source="test")
        history = self.graphs.history("graph-1")
        self.assertEqual(len(history), 2)
        self.assertEqual(history[-1].action, "task_started")
        self.assertEqual(history[-1].affected_tasks, ("task-plan",))

    def test_red_track_task_graph_does_not_import_runtime_or_lanchat(self) -> None:
        forbidden = (
            "editor.plugins.AITool.services.agent_runtime",
            "editor.plugins.AITool.services.lanchat",
        )
        assert_module_has_no_forbidden_imports(self, task_graph_module, forbidden)


if __name__ == "__main__":
    unittest.main()
