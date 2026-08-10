from __future__ import annotations

from pathlib import Path
import unittest

from editor.plugins.AITool.services.agent_collaboration import (
    AgentTask,
    AgentTaskGraphStore,
    ArtDirection,
    ArtifactEnvelope,
    ArtifactRegistry,
    GameDesignBrief,
    InvalidArtifactError,
    LevelPlan,
    NonExecutableArtifactError,
    ProjectStatePatch,
    ProjectStateStore,
    SceneCompositionPlan,
    assert_executable,
)
from editor.plugins.AITool.services.agent_collaboration.agents import (
    ArtAgent,
    ArtAgentDraft,
    ArtAgentError,
    ArtContextStaleError,
    ArtInputValidationError,
    ArtIsolationError,
    ArtOutputValidationError,
    ArtRequest,
    PlanningAgent,
    PlanningAgentDraft,
    PlanningRequest,
)
import editor.plugins.AITool.services.agent_collaboration.agents.art_agent as art_agent_module


class FakePlanningReasoner:
    def __init__(self, *, label: str = "v1") -> None:
        self.label = label
        self.calls = []

    def generate(self, request, context):
        self.calls.append((request, context))
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


class FakeArtReasoner:
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
        return ArtAgentDraft(
            art_direction=ArtDirection(
                style_keywords=("warm fantasy", self.label),
                palette=("amber", "deep green"),
                lighting=("lantern key light",),
                avoid_keywords=("flat poster", "horror"),
            ),
            scene_composition_plan=SceneCompositionPlan(
                scene_type="indoor_treasure_room",
                environment_requirements=("room shell", "walkable floor"),
                entity_requirements=("treasure chest", "map table"),
                layout_rules=("keep entry clear", "focus treasure at rear axis"),
            ),
        )


def _planning_task(task_id: str) -> AgentTask:
    return AgentTask(
        task_id=task_id,
        assigned_role="planning",
        objective="Produce planning contracts",
        input_artifact_refs=(),
        output_types=("GameDesignBrief", "LevelPlan"),
        depends_on=(),
        acceptance_criteria=("both planning Artifacts validate",),
        capability_set=("artifact.write",),
    )


def _art_task(task_id: str, inputs) -> AgentTask:
    return AgentTask(
        task_id=task_id,
        assigned_role="art",
        objective="Produce non-executing art contracts",
        input_artifact_refs=tuple(inputs),
        output_types=("ArtDirection", "SceneCompositionPlan"),
        depends_on=(),
        acceptance_criteria=("both art Artifacts validate",),
        capability_set=("artifact.write",),
    )


def _art_request(
    *,
    request_id: str = "request-art-1",
    graph_id: str = "graph-art-1",
    task_id: str = "task-art-1",
    objective: str = "Define a coherent treasure room art direction",
) -> ArtRequest:
    return ArtRequest(
        request_id=request_id,
        project_id="project-1",
        graph_id=graph_id,
        task_id=task_id,
        art_objective=objective,
        constraints=("no scene writes", "preserve planning intent"),
        acceptance_criteria=("scene requirements are structured",),
        requested_by="host",
    )


class ArtAgentTests(unittest.TestCase):
    def setUp(self) -> None:
        self.projects = ProjectStateStore()
        self.projects.create_project(project_id="project-1", room_id="room-1", source="test")
        self.artifacts = ArtifactRegistry(self.projects)
        self.graphs = AgentTaskGraphStore(self.projects, self.artifacts)

    def _publish_planning(
        self,
        *,
        suffix: str = "1",
        label: str = "v1",
    ) -> tuple[str, ...]:
        graph_id = f"graph-planning-{suffix}"
        task_id = f"task-planning-{suffix}"
        version = self.projects.get("project-1").project_version
        self.graphs.create_graph(
            graph_id=graph_id,
            project_id="project-1",
            tasks=(_planning_task(task_id),),
            expected_project_version=version,
            patch_id=f"patch-create-{graph_id}",
            source="test",
        )
        result = PlanningAgent(
            project_states=self.projects,
            artifacts=self.artifacts,
            task_graphs=self.graphs,
            reasoner=FakePlanningReasoner(label=label),
        ).run(
            PlanningRequest(
                request_id=f"request-planning-{suffix}",
                project_id="project-1",
                graph_id=graph_id,
                task_id=task_id,
                project_goal="Build a cooperative treasure room",
                constraints=("shared multiplayer state",),
                acceptance_criteria=("two users see the same objective",),
                requested_by="host",
            )
        )
        return result.artifact_refs

    def _create_art_graph(
        self,
        inputs,
        *,
        graph_id: str = "graph-art-1",
        task_id: str = "task-art-1",
    ) -> None:
        version = self.projects.get("project-1").project_version
        self.graphs.create_graph(
            graph_id=graph_id,
            project_id="project-1",
            tasks=(_art_task(task_id, inputs),),
            expected_project_version=version,
            patch_id=f"patch-create-{graph_id}",
            source="test",
        )

    def _agent(self, reasoner) -> ArtAgent:
        return ArtAgent(
            project_states=self.projects,
            artifacts=self.artifacts,
            task_graphs=self.graphs,
            reasoner=reasoner,
        )

    def test_art_agent_produces_structured_non_executable_artifacts(self) -> None:
        planning_refs = self._publish_planning()
        self._create_art_graph(planning_refs)
        reasoner = FakeArtReasoner()

        result = self._agent(reasoner).run(_art_request())

        self.assertEqual(
            result.artifact_refs,
            ("art.direction@1", "art.scene-composition@1"),
        )
        self.assertEqual(self.graphs.get("graph-art-1").status, "completed")
        direction = self.artifacts.get("project-1", "art.direction@1")
        composition = self.artifacts.get("project-1", "art.scene-composition@1")
        self.assertTrue(direction.usable)
        self.assertTrue(composition.usable)
        self.assertEqual(direction.artifact.dependencies, planning_refs)
        self.assertEqual(
            composition.artifact.dependencies,
            tuple(sorted((*planning_refs, "art.direction@1"))),
        )
        self.assertEqual(direction.artifact.snapshot_source, "none")
        self.assertTrue(direction.artifact.non_executable)
        with self.assertRaises(NonExecutableArtifactError):
            assert_executable(composition.artifact)
        self.assertEqual(
            tuple(item.artifact_type for item in reasoner.calls[0][1].planning_artifacts),
            ("GameDesignBrief", "LevelPlan"),
        )

    def test_missing_planning_type_is_rejected_before_reasoning(self) -> None:
        planning_refs = self._publish_planning()
        self._create_art_graph((planning_refs[0],))
        reasoner = FakeArtReasoner()

        with self.assertRaises(ArtInputValidationError):
            self._agent(reasoner).run(_art_request())

        self.assertEqual(reasoner.calls, [])
        self.assertEqual(self.graphs.get("graph-art-1").task("task-art-1").status, "ready")

    def test_duplicate_planning_type_is_rejected_before_reasoning(self) -> None:
        planning_refs = self._publish_planning()
        current_version = self.projects.get("project-1").project_version
        duplicate_brief = ArtifactEnvelope(
            artifact_id="planning.alternate-game-design-brief",
            artifact_type="GameDesignBrief",
            version=1,
            producer_role="planning",
            source_task_id="fixture",
            base_project_version=current_version,
            base_world_version=0,
            snapshot_source="none",
            non_executable=True,
            status="validated",
            payload=GameDesignBrief(
                project_goal="alternate fixture",
                player_experience=("inspect",),
                core_rules=("no ambiguous inputs",),
                acceptance_criteria=("reject duplicate type",),
            ),
        )
        self.artifacts.register(
            project_id="project-1",
            artifact=duplicate_brief,
            expected_project_version=current_version,
            patch_id="patch-register-duplicate-planning-type",
            source="test",
        )
        self._create_art_graph(
            (*planning_refs, "planning.alternate-game-design-brief@1")
        )
        reasoner = FakeArtReasoner()

        with self.assertRaises(ArtInputValidationError):
            self._agent(reasoner).run(_art_request())

        self.assertEqual(reasoner.calls, [])
        self.assertEqual(self.graphs.get("graph-art-1").task("task-art-1").status, "ready")

    def test_non_planning_artifact_type_is_rejected_before_reasoning(self) -> None:
        planning_refs = self._publish_planning()
        current_version = self.projects.get("project-1").project_version
        extra = ArtifactEnvelope(
            artifact_id="fixture.art-note",
            artifact_type="ArtDirection",
            version=1,
            producer_role="art",
            source_task_id="fixture",
            base_project_version=current_version,
            base_world_version=0,
            dependencies=planning_refs,
            snapshot_source="none",
            non_executable=True,
            status="validated",
            payload=ArtDirection(
                style_keywords=("fixture",),
                palette=("gray",),
                lighting=("neutral",),
                avoid_keywords=("writes",),
            ),
        )
        self.artifacts.register(
            project_id="project-1",
            artifact=extra,
            expected_project_version=current_version,
            patch_id="patch-register-extra-art-input",
            source="test",
        )
        self._create_art_graph((*planning_refs, "fixture.art-note@1"))
        reasoner = FakeArtReasoner()

        with self.assertRaises(ArtInputValidationError):
            self._agent(reasoner).run(_art_request())

        self.assertEqual(reasoner.calls, [])
        self.assertEqual(self.graphs.get("graph-art-1").task("task-art-1").status, "ready")

    def test_wrong_producer_for_planning_type_is_rejected_before_reasoning(self) -> None:
        current_version = self.projects.get("project-1").project_version
        with self.assertRaisesRegex(ValueError, "requires producer_role planning"):
            ArtifactEnvelope(
                artifact_id="fixture.game-design-brief",
                artifact_type="GameDesignBrief",
                version=1,
                producer_role="art",
                source_task_id="fixture",
                base_project_version=current_version,
                base_world_version=0,
                snapshot_source="none",
                non_executable=True,
                status="validated",
                payload=GameDesignBrief(
                    project_goal="fixture",
                    player_experience=("inspect",),
                    core_rules=("no writes",),
                    acceptance_criteria=("reject wrong producer",),
                ),
            )
        self.assertEqual(self.artifacts.list_versions("project-1", "fixture.game-design-brief"), ())

    def test_invalid_reasoner_result_fails_only_art_task(self) -> None:
        planning_refs = self._publish_planning()
        self._create_art_graph(planning_refs)
        reasoner = FakeArtReasoner(result={"not": "an ArtAgentDraft"})

        with self.assertRaises(ArtOutputValidationError):
            self._agent(reasoner).run(_art_request())

        task = self.graphs.get("graph-art-1").task("task-art-1")
        self.assertEqual(task.status, "failed")
        self.assertIn("ArtOutputValidationError", task.last_error)
        self.assertEqual(self.artifacts.list_versions("project-1", "art.direction"), ())

    def test_schema_invalid_draft_is_not_published(self) -> None:
        planning_refs = self._publish_planning()
        self._create_art_graph(planning_refs)
        invalid = ArtAgentDraft(
            art_direction=ArtDirection(
                style_keywords=(),
                palette=("amber",),
                lighting=("lantern",),
                avoid_keywords=("horror",),
            ),
            scene_composition_plan=SceneCompositionPlan(
                scene_type="indoor",
                environment_requirements=("room shell",),
                entity_requirements=("treasure chest",),
                layout_rules=("clear entry",),
            ),
        )

        with self.assertRaises(InvalidArtifactError):
            self._agent(FakeArtReasoner(result=invalid)).run(_art_request())

        self.assertEqual(self.graphs.get("graph-art-1").task("task-art-1").status, "failed")
        self.assertEqual(self.artifacts.list_versions("project-1", "art.direction"), ())
        self.assertEqual(self.artifacts.list_versions("project-1", "art.scene-composition"), ())

    def test_project_version_change_during_reasoning_rejects_outputs(self) -> None:
        planning_refs = self._publish_planning()
        self._create_art_graph(planning_refs)

        def advance_project() -> None:
            project = self.projects.get("project-1")
            self.projects.apply_patch(
                ProjectStatePatch(
                    patch_id="patch-concurrent-art-change",
                    project_id="project-1",
                    expected_project_version=project.project_version,
                    source="test",
                    changes={"validation_status": "blocked"},
                )
            )

        with self.assertRaises(ArtContextStaleError):
            self._agent(FakeArtReasoner(side_effect=advance_project)).run(_art_request())

        self.assertEqual(self.graphs.get("graph-art-1").task("task-art-1").status, "failed")
        self.assertEqual(self.artifacts.list_versions("project-1", "art.direction"), ())

    def test_same_request_is_idempotent_and_changed_reuse_is_rejected(self) -> None:
        planning_refs = self._publish_planning()
        self._create_art_graph(planning_refs)
        reasoner = FakeArtReasoner()
        agent = self._agent(reasoner)
        request = _art_request()

        first = agent.run(request)
        replay = agent.run(request)

        self.assertIs(first, replay)
        self.assertEqual(len(reasoner.calls), 1)
        self.assertEqual(len(self.artifacts.list_versions("project-1", "art.direction")), 1)
        with self.assertRaises(ArtAgentError):
            agent.run(_art_request(objective="different objective with reused request id"))

    def test_planning_revision_stales_art_and_allows_versioned_rebuild(self) -> None:
        planning_v1 = self._publish_planning(suffix="1", label="v1")
        self._create_art_graph(planning_v1, graph_id="graph-art-1", task_id="task-art-1")
        self._agent(FakeArtReasoner(label="v1")).run(_art_request())

        planning_v2 = self._publish_planning(suffix="2", label="v2")

        stale_direction = self.artifacts.current(
            "project-1",
            "art.direction",
            require_usable=False,
        )
        stale_composition = self.artifacts.current(
            "project-1",
            "art.scene-composition",
            require_usable=False,
        )
        self.assertEqual(stale_direction.registry_status, "stale")
        self.assertEqual(stale_composition.registry_status, "stale")

        self._create_art_graph(planning_v2, graph_id="graph-art-2", task_id="task-art-2")
        result = self._agent(FakeArtReasoner(label="v2")).run(
            _art_request(
                request_id="request-art-2",
                graph_id="graph-art-2",
                task_id="task-art-2",
            )
        )

        self.assertEqual(result.artifact_refs, ("art.direction@2", "art.scene-composition@2"))
        self.assertEqual(
            self.artifacts.get("project-1", "art.direction@2").artifact.dependencies,
            planning_v2,
        )

    def test_stale_task_inputs_are_rejected_before_reasoning(self) -> None:
        planning_v1 = self._publish_planning(suffix="1", label="v1")
        self._create_art_graph(planning_v1)
        current_version = self.projects.get("project-1").project_version
        brief_v2 = ArtifactEnvelope(
            artifact_id="planning.game-design-brief",
            artifact_type="GameDesignBrief",
            version=2,
            producer_role="planning",
            source_task_id="task-planning-revision",
            base_project_version=current_version,
            base_world_version=0,
            snapshot_source="none",
            non_executable=True,
            status="validated",
            payload=GameDesignBrief(
                project_goal="Revised goal",
                player_experience=("explore",),
                core_rules=("shared state",),
                acceptance_criteria=("revision visible",),
            ),
        )
        level_v2 = ArtifactEnvelope(
            artifact_id="planning.level-plan",
            artifact_type="LevelPlan",
            version=2,
            producer_role="planning",
            source_task_id="task-planning-revision",
            base_project_version=current_version,
            base_world_version=0,
            dependencies=("planning.game-design-brief@2",),
            snapshot_source="none",
            non_executable=True,
            status="validated",
            payload=LevelPlan(
                level_goal="Revised level",
                zones=("entry", "vault"),
                progression=("enter", "discover"),
                acceptance_criteria=("revision visible",),
            ),
        )
        self.artifacts.register_many(
            project_id="project-1",
            artifacts=(brief_v2, level_v2),
            expected_project_version=current_version,
            patch_id="patch-register-planning-revision",
            source="test",
        )
        reasoner = FakeArtReasoner()

        with self.assertRaises(ArtInputValidationError):
            self._agent(reasoner).run(_art_request())

        self.assertEqual(reasoner.calls, [])
        self.assertEqual(self.graphs.get("graph-art-1").task("task-art-1").status, "blocked")

    def test_snapshot_sourced_planning_inputs_are_rejected_before_reasoning(self) -> None:
        for source in ("mock", "runtime"):
            with self.subTest(source=source):
                projects = ProjectStateStore()
                projects.create_project(
                    project_id="project-1",
                    room_id="room-1",
                    source="test",
                )
                projects.apply_patch(ProjectStatePatch(
                    patch_id=f"patch-world-{source}",
                    project_id="project-1",
                    expected_project_version=1,
                    source="test",
                    changes={"scene_world_version": 1},
                ))
                artifacts = ArtifactRegistry(projects)
                graphs = AgentTaskGraphStore(projects, artifacts)
                brief = ArtifactEnvelope(
                    artifact_id="planning.game-design-brief",
                    artifact_type="GameDesignBrief",
                    version=1,
                    producer_role="planning",
                    source_task_id="fixture",
                    base_project_version=2,
                    base_world_version=1,
                    snapshot_source=source,
                    non_executable=True,
                    status="validated",
                    payload=GameDesignBrief(
                        project_goal="fixture",
                        player_experience=("inspect",),
                        core_rules=("no writes",),
                        acceptance_criteria=("isolated",),
                    ),
                )
                level = ArtifactEnvelope(
                    artifact_id="planning.level-plan",
                    artifact_type="LevelPlan",
                    version=1,
                    producer_role="planning",
                    source_task_id="fixture",
                    base_project_version=2,
                    base_world_version=1,
                    dependencies=("planning.game-design-brief@1",),
                    snapshot_source=source,
                    non_executable=True,
                    status="validated",
                    payload=LevelPlan(
                        level_goal="fixture",
                        zones=("entry",),
                        progression=("inspect",),
                        acceptance_criteria=("isolated",),
                    ),
                )
                artifacts.register_many(
                    project_id="project-1",
                    artifacts=(brief, level),
                    expected_project_version=2,
                    patch_id=f"patch-register-{source}",
                    source="test",
                )
                project_version = projects.get("project-1").project_version
                graphs.create_graph(
                    graph_id="graph-art-1",
                    project_id="project-1",
                    tasks=(
                        _art_task(
                            "task-art-1",
                            ("planning.game-design-brief@1", "planning.level-plan@1"),
                        ),
                    ),
                    expected_project_version=project_version,
                    patch_id=f"patch-create-art-{source}",
                    source="test",
                )
                reasoner = FakeArtReasoner()
                agent = ArtAgent(
                    project_states=projects,
                    artifacts=artifacts,
                    task_graphs=graphs,
                    reasoner=reasoner,
                )

                with self.assertRaises(ArtIsolationError):
                    agent.run(_art_request())

                self.assertEqual(reasoner.calls, [])
                self.assertEqual(graphs.get("graph-art-1").task("task-art-1").status, "ready")

    def test_art_agent_module_has_no_runtime_or_execution_imports(self) -> None:
        source = Path(art_agent_module.__file__).read_text(encoding="utf-8")
        forbidden = (
            "services.agent_runtime",
            "services.lanchat",
            "SceneWorldSnapshot",
            "SceneTools",
            "ActionProposal",
            "RuntimeCppBridge",
        )
        for value in forbidden:
            self.assertNotIn(value, source)


if __name__ == "__main__":
    unittest.main()
