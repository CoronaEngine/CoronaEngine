from __future__ import annotations

import unittest

import editor.plugins.AITool.services.agent_collaboration.agents.planning_agent as planning_agent_module
from editor.plugins.AITool.services.agent_collaboration import (
    AgentTask,
    AgentTaskGraphStore,
    ArtDirection,
    ArtifactEnvelope,
    ArtifactRegistry,
    GameDesignBrief,
    LevelPlan,
    InvalidArtifactError,
    NonExecutableArtifactError,
    ProjectStatePatch,
    ProjectStateStore,
    assert_executable,
)
from editor.plugins.AITool.services.tests.support._test_import_guard import assert_module_has_no_forbidden_imports
from editor.plugins.AITool.services.agent_collaboration.agents import (
    PlanningAgent,
    PlanningAgentDraft,
    PlanningAgentError,
    PlanningContextStaleError,
    PlanningIsolationError,
    PlanningOutputValidationError,
    PlanningRequest,
)


class FakePlanningReasoner:
    def __init__(self, *, label: str = "v1", result=None, side_effect=None) -> None:
        self.label = label
        self.result = result
        self.side_effect = side_effect
        self.calls = []

    def generate(self, request, context):
        self.calls.append((request, context))
        if self.side_effect is not None:
            self.side_effect()
        if self.result is not None:
            return self.result
        return PlanningAgentDraft(
            game_design_brief=GameDesignBrief(
                project_goal=f"{request.project_goal} {self.label}",
                player_experience=("explore", "cooperate"),
                core_rules=("host owns authoritative state",),
                acceptance_criteria=request.acceptance_criteria,
            ),
            level_plan=LevelPlan(
                level_goal=f"Complete the shared objective {self.label}",
                zones=("entry", "objective"),
                progression=("enter", "cooperate", "complete"),
                acceptance_criteria=request.acceptance_criteria,
            ),
        )


def _planning_task(task_id: str = "task-planning", *, inputs=()) -> AgentTask:
    return AgentTask(
        task_id=task_id,
        assigned_role="planning",
        objective="Produce the planning contract",
        input_artifact_refs=tuple(inputs),
        output_types=("GameDesignBrief", "LevelPlan"),
        depends_on=(),
        acceptance_criteria=("both Artifacts validate",),
        capability_set=("artifact.write",),
    )


def _request(
    *,
    request_id: str = "request-plan-1",
    graph_id: str = "graph-planning-1",
    task_id: str = "task-planning",
    goal: str = "Build a cooperative treasure room",
) -> PlanningRequest:
    return PlanningRequest(
        request_id=request_id,
        project_id="project-1",
        graph_id=graph_id,
        task_id=task_id,
        project_goal=goal,
        constraints=("no direct Engine writes", "shared multiplayer state"),
        acceptance_criteria=("two users see the same objective",),
        requested_by="host",
    )


class PlanningAgentTests(unittest.TestCase):
    def setUp(self) -> None:
        self.projects = ProjectStateStore()
        self.projects.create_project(project_id="project-1", room_id="room-1", source="test")
        self.artifacts = ArtifactRegistry(self.projects)
        self.graphs = AgentTaskGraphStore(self.projects, self.artifacts)

    def _create_graph(
        self,
        *,
        graph_id: str = "graph-planning-1",
        task_id: str = "task-planning",
        inputs=(),
    ) -> None:
        version = self.projects.get("project-1").project_version
        self.graphs.create_graph(
            graph_id=graph_id,
            project_id="project-1",
            tasks=(_planning_task(task_id, inputs=inputs),),
            expected_project_version=version,
            patch_id=f"patch-create-{graph_id}",
            source="test",
        )

    def _agent(self, reasoner) -> PlanningAgent:
        return PlanningAgent(
            project_states=self.projects,
            artifacts=self.artifacts,
            task_graphs=self.graphs,
            reasoner=reasoner,
        )

    def test_planning_agent_produces_two_validated_non_executable_artifacts(self) -> None:
        self._create_graph()
        reasoner = FakePlanningReasoner()
        agent = self._agent(reasoner)

        result = agent.run(_request())

        self.assertEqual(
            result.artifact_refs,
            ("planning.game-design-brief@1", "planning.level-plan@1"),
        )
        self.assertEqual(self.graphs.get("graph-planning-1").status, "completed")
        self.assertEqual(
            self.graphs.get("graph-planning-1").task("task-planning").output_artifact_refs,
            result.artifact_refs,
        )
        brief = self.artifacts.get("project-1", "planning.game-design-brief@1")
        level = self.artifacts.get("project-1", "planning.level-plan@1")
        self.assertTrue(brief.usable)
        self.assertTrue(level.usable)
        self.assertTrue(brief.artifact.non_executable)
        self.assertEqual(brief.artifact.snapshot_source, "none")
        self.assertEqual(level.artifact.dependencies, ("planning.game-design-brief@1",))
        with self.assertRaises(NonExecutableArtifactError):
            assert_executable(brief.artifact)
        self.assertEqual(len(reasoner.calls), 1)
        self.assertEqual(reasoner.calls[0][1].prior_artifacts, ())

    def test_same_request_is_idempotent_and_changed_reuse_is_rejected(self) -> None:
        self._create_graph()
        reasoner = FakePlanningReasoner()
        agent = self._agent(reasoner)
        request = _request()

        first = agent.run(request)
        replay = agent.run(request)

        self.assertIs(first, replay)
        self.assertEqual(len(reasoner.calls), 1)
        self.assertEqual(len(self.artifacts.list_versions("project-1", "planning.level-plan")), 1)
        with self.assertRaises(PlanningAgentError):
            agent.run(_request(goal="different goal with the same request id"))

    def test_invalid_reasoner_result_fails_only_planning_task(self) -> None:
        self._create_graph()
        reasoner = FakePlanningReasoner(result={"not": "a PlanningAgentDraft"})
        agent = self._agent(reasoner)

        with self.assertRaises(PlanningOutputValidationError):
            agent.run(_request())

        task = self.graphs.get("graph-planning-1").task("task-planning")
        self.assertEqual(task.status, "failed")
        self.assertIn("PlanningOutputValidationError", task.last_error)
        self.assertEqual(self.artifacts.list_current("project-1"), ())

    def test_schema_invalid_draft_is_rejected_and_task_records_failure(self) -> None:
        self._create_graph()
        invalid_draft = PlanningAgentDraft(
            game_design_brief=GameDesignBrief(
                project_goal="goal",
                player_experience=(),
                core_rules=(),
                acceptance_criteria=(),
            ),
            level_plan=LevelPlan(
                level_goal="level",
                zones=("entry",),
                progression=("enter",),
                acceptance_criteria=("done",),
            ),
        )
        agent = self._agent(FakePlanningReasoner(result=invalid_draft))

        with self.assertRaises(InvalidArtifactError):
            agent.run(_request())

        self.assertEqual(
            self.graphs.get("graph-planning-1").task("task-planning").status,
            "failed",
        )
        self.assertEqual(self.artifacts.list_current("project-1"), ())

    def test_project_version_change_during_reasoning_rejects_stale_output(self) -> None:
        self._create_graph()

        def advance_project() -> None:
            project = self.projects.get("project-1")
            self.projects.apply_patch(
                ProjectStatePatch(
                    patch_id="patch-concurrent-project-change",
                    project_id="project-1",
                    expected_project_version=project.project_version,
                    source="test",
                    changes={"validation_status": "blocked"},
                )
            )

        agent = self._agent(FakePlanningReasoner(side_effect=advance_project))
        with self.assertRaises(PlanningContextStaleError):
            agent.run(_request())

        task = self.graphs.get("graph-planning-1").task("task-planning")
        self.assertEqual(task.status, "failed")
        self.assertEqual(self.artifacts.list_current("project-1"), ())

    def test_mock_or_runtime_sourced_input_is_rejected_before_reasoning(self) -> None:
        mock = ArtifactEnvelope(
            artifact_id="planning-seed",
            artifact_type="GameDesignBrief",
            version=1,
            producer_role="planning",
            source_task_id="fixture",
            base_project_version=1,
            base_world_version=0,
            snapshot_source="mock",
            non_executable=True,
            status="validated",
            payload=GameDesignBrief(
                project_goal="fixture",
                player_experience=("inspect",),
                core_rules=("no writes",),
                acceptance_criteria=("isolated",),
            ),
        )
        self.artifacts.register(
            project_id="project-1",
            artifact=mock,
            expected_project_version=1,
            patch_id="patch-register-mock",
            source="test",
        )
        self._create_graph(inputs=("planning-seed@1",))
        reasoner = FakePlanningReasoner()
        agent = self._agent(reasoner)

        with self.assertRaises(PlanningIsolationError):
            agent.run(_request())

        self.assertEqual(len(reasoner.calls), 0)
        self.assertEqual(
            self.graphs.get("graph-planning-1").task("task-planning").status,
            "ready",
        )

    def test_planning_revision_versions_outputs_and_stales_downstream_artifact(self) -> None:
        self._create_graph()
        first_reasoner = FakePlanningReasoner(label="v1")
        self._agent(first_reasoner).run(_request())

        project_version = self.projects.get("project-1").project_version
        downstream = ArtifactEnvelope(
            artifact_id="art.direction",
            artifact_type="ArtDirection",
            version=1,
            producer_role="art",
            source_task_id="task-art",
            base_project_version=project_version,
            base_world_version=0,
            dependencies=("planning.level-plan@1",),
            status="validated",
            payload=ArtDirection(
                style_keywords=("warm",),
                palette=("amber",),
                lighting=("lantern",),
                avoid_keywords=("horror",),
            ),
        )
        self.artifacts.register(
            project_id="project-1",
            artifact=downstream,
            expected_project_version=project_version,
            patch_id="patch-art-direction",
            source="test",
        )
        self._create_graph(graph_id="graph-planning-2", task_id="task-planning-2")
        second_reasoner = FakePlanningReasoner(label="v2")
        result = self._agent(second_reasoner).run(
            _request(
                request_id="request-plan-2",
                graph_id="graph-planning-2",
                task_id="task-planning-2",
                goal="Revise the cooperative treasure room",
            )
        )

        self.assertEqual(
            result.artifact_refs,
            ("planning.game-design-brief@2", "planning.level-plan@2"),
        )
        self.assertTrue(self.artifacts.current("project-1", "planning.game-design-brief").usable)
        self.assertTrue(self.artifacts.current("project-1", "planning.level-plan").usable)
        stale_art = self.artifacts.current("project-1", "art.direction", require_usable=False)
        self.assertEqual(stale_art.registry_status, "stale")
        context_refs = {
            item.artifact_ref for item in second_reasoner.calls[0][1].prior_artifacts
        }
        self.assertEqual(
            context_refs,
            {"planning.game-design-brief@1", "planning.level-plan@1"},
        )

    def test_planning_agent_module_does_not_import_runtime_lanchat_or_snapshot(self) -> None:
        forbidden = (
            "editor.plugins.AITool.services.agent_runtime",
            "editor.plugins.AITool.services.lanchat",
        )
        assert_module_has_no_forbidden_imports(self, planning_agent_module, forbidden)


if __name__ == "__main__":
    unittest.main()
