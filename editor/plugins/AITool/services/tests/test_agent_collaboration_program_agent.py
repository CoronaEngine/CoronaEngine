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
    GameplayEntitySlot,
    GameplayLogicPlan,
    GameplayPrimitiveSpec,
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
    ArtRequest,
    PlanningAgent,
    PlanningAgentDraft,
    PlanningRequest,
    ProgramAgent,
    ProgramAgentDraft,
    ProgramAgentError,
    ProgramCapabilityError,
    ProgramContextStaleError,
    ProgramInputValidationError,
    ProgramIsolationError,
    ProgramOutputValidationError,
    ProgramRequest,
)
import editor.plugins.AITool.services.agent_collaboration.agents.program_agent as program_agent_module


class FakePlanningReasoner:
    def __init__(self, *, label: str = "v1") -> None:
        self.label = label

    def generate(self, request, _context):
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
    def __init__(self, *, label: str = "v1") -> None:
        self.label = label

    def generate(self, _request, _context):
        return ArtAgentDraft(
            art_direction=ArtDirection(
                style_keywords=("warm fantasy", self.label),
                palette=("amber", "deep green"),
                lighting=("lantern key light",),
                avoid_keywords=("horror",),
            ),
            scene_composition_plan=SceneCompositionPlan(
                scene_type="indoor_treasure_room",
                environment_requirements=("room shell", "walkable floor"),
                entity_requirements=("treasure chest", "map table"),
                layout_rules=("keep entry clear",),
            ),
        )


class FakeProgramReasoner:
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
        return ProgramAgentDraft(
            gameplay_logic_plan=GameplayLogicPlan(
                states=("exploring", "objective_active", f"complete_{self.label}"),
                entity_slots=(
                    GameplayEntitySlot("objective", "collectible_objective", ("collectible",)),
                    GameplayEntitySlot("player", "player_spawn", ("player",)),
                ),
                primitives=(
                    GameplayPrimitiveSpec("collect-objective", "on_collect", "objective", "player", {"state_key": "objective_active", "set_value": True}),
                ),
                triggers=("player_enters_objective_zone",),
                rules=("all players share objective progress",),
                win_conditions=("shared objective completed",),
                lose_conditions=("all players leave the mission",),
            )
        )


def _planning_task(task_id: str) -> AgentTask:
    return AgentTask(
        task_id=task_id,
        assigned_role="planning",
        objective="Produce planning contracts",
        input_artifact_refs=(),
        output_types=("GameDesignBrief", "LevelPlan"),
        depends_on=(),
        acceptance_criteria=("planning Artifacts validate",),
        capability_set=("artifact.write",),
    )


def _art_task(task_id: str, inputs) -> AgentTask:
    return AgentTask(
        task_id=task_id,
        assigned_role="art",
        objective="Produce art contracts",
        input_artifact_refs=tuple(inputs),
        output_types=("ArtDirection", "SceneCompositionPlan"),
        depends_on=(),
        acceptance_criteria=("art Artifacts validate",),
        capability_set=("artifact.write",),
    )


def _program_task(
    task_id: str,
    inputs,
    *,
    capabilities=("artifact.read", "artifact.write"),
) -> AgentTask:
    return AgentTask(
        task_id=task_id,
        assigned_role="program",
        objective="Produce a non-executing gameplay logic contract",
        input_artifact_refs=tuple(inputs),
        output_types=("GameplayLogicPlan",),
        depends_on=(),
        acceptance_criteria=("logic Artifact validates",),
        capability_set=tuple(capabilities),
    )


def _program_request(
    *,
    request_id: str = "request-program-1",
    graph_id: str = "graph-program-1",
    task_id: str = "task-program-1",
    objective: str = "Define cooperative objective logic",
) -> ProgramRequest:
    return ProgramRequest(
        request_id=request_id,
        project_id="project-1",
        graph_id=graph_id,
        task_id=task_id,
        logic_objective=objective,
        constraints=("no scripts", "no scene writes"),
        acceptance_criteria=("states and outcomes are structured",),
        requested_by="host",
    )


class ProgramAgentTests(unittest.TestCase):
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
        project_version = self.projects.get("project-1").project_version
        self.graphs.create_graph(
            graph_id=graph_id,
            project_id="project-1",
            tasks=(_planning_task(task_id),),
            expected_project_version=project_version,
            patch_id=f"patch-create-{graph_id}",
            source="test",
        )
        return PlanningAgent(
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
                acceptance_criteria=("two users share objective progress",),
                requested_by="host",
            )
        ).artifact_refs

    def _publish_art(
        self,
        planning_refs,
        *,
        suffix: str = "1",
        label: str = "v1",
    ) -> tuple[str, ...]:
        graph_id = f"graph-art-{suffix}"
        task_id = f"task-art-{suffix}"
        project_version = self.projects.get("project-1").project_version
        self.graphs.create_graph(
            graph_id=graph_id,
            project_id="project-1",
            tasks=(_art_task(task_id, planning_refs),),
            expected_project_version=project_version,
            patch_id=f"patch-create-{graph_id}",
            source="test",
        )
        return ArtAgent(
            project_states=self.projects,
            artifacts=self.artifacts,
            task_graphs=self.graphs,
            reasoner=FakeArtReasoner(label=label),
        ).run(
            ArtRequest(
                request_id=f"request-art-{suffix}",
                project_id="project-1",
                graph_id=graph_id,
                task_id=task_id,
                art_objective="Define coherent art direction",
                constraints=("no scene writes",),
                acceptance_criteria=("requirements are structured",),
                requested_by="host",
            )
        ).artifact_refs

    def _create_program_graph(
        self,
        inputs,
        *,
        graph_id: str = "graph-program-1",
        task_id: str = "task-program-1",
        capabilities=("artifact.read", "artifact.write"),
    ) -> None:
        project_version = self.projects.get("project-1").project_version
        self.graphs.create_graph(
            graph_id=graph_id,
            project_id="project-1",
            tasks=(_program_task(task_id, inputs, capabilities=capabilities),),
            expected_project_version=project_version,
            patch_id=f"patch-create-{graph_id}",
            source="test",
        )

    def _agent(self, reasoner) -> ProgramAgent:
        return ProgramAgent(
            project_states=self.projects,
            artifacts=self.artifacts,
            task_graphs=self.graphs,
            reasoner=reasoner,
        )

    def test_program_agent_produces_non_executable_logic_from_planning(self) -> None:
        planning_refs = self._publish_planning()
        self._create_program_graph(planning_refs)
        reasoner = FakeProgramReasoner()

        result = self._agent(reasoner).run(_program_request())

        self.assertEqual(result.artifact_refs, ("program.gameplay-logic-plan@1",))
        self.assertEqual(self.graphs.get("graph-program-1").status, "completed")
        logic = self.artifacts.get("project-1", result.artifact_refs[0])
        self.assertTrue(logic.usable)
        self.assertTrue(logic.artifact.non_executable)
        self.assertEqual(logic.artifact.snapshot_source, "none")
        self.assertEqual(logic.artifact.dependencies, planning_refs)
        with self.assertRaises(NonExecutableArtifactError):
            assert_executable(logic.artifact)
        self.assertEqual(
            tuple(item.artifact_type for item in reasoner.calls[0][1].input_artifacts),
            ("GameDesignBrief", "LevelPlan"),
        )

    def test_optional_art_direction_is_explicit_dependency(self) -> None:
        planning_refs = self._publish_planning()
        art_refs = self._publish_art(planning_refs)
        inputs = (*planning_refs, art_refs[0])
        self._create_program_graph(inputs)
        reasoner = FakeProgramReasoner()

        result = self._agent(reasoner).run(_program_request())

        logic = self.artifacts.get("project-1", result.artifact_refs[0])
        self.assertEqual(logic.artifact.dependencies, tuple(sorted(inputs)))
        self.assertEqual(
            tuple(item.artifact_type for item in reasoner.calls[0][1].input_artifacts),
            ("ArtDirection", "GameDesignBrief", "LevelPlan"),
        )

    def test_missing_required_planning_input_is_rejected_before_reasoning(self) -> None:
        planning_refs = self._publish_planning()
        self._create_program_graph((planning_refs[0],))
        reasoner = FakeProgramReasoner()

        with self.assertRaises(ProgramInputValidationError):
            self._agent(reasoner).run(_program_request())

        self.assertEqual(reasoner.calls, [])
        self.assertEqual(
            self.graphs.get("graph-program-1").task("task-program-1").status,
            "ready",
        )

    def test_duplicate_input_type_is_rejected_before_reasoning(self) -> None:
        planning_refs = self._publish_planning()
        current_version = self.projects.get("project-1").project_version
        alternate_brief = ArtifactEnvelope(
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
            artifact=alternate_brief,
            expected_project_version=current_version,
            patch_id="patch-register-program-duplicate-input",
            source="test",
        )
        self._create_program_graph(
            (*planning_refs, "planning.alternate-game-design-brief@1")
        )
        reasoner = FakeProgramReasoner()

        with self.assertRaises(ProgramInputValidationError):
            self._agent(reasoner).run(_program_request())

        self.assertEqual(reasoner.calls, [])

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
                    core_rules=("no role impersonation",),
                    acceptance_criteria=("reject wrong producer",),
                ),
            )
        self.assertEqual(self.artifacts.list_versions("project-1", "fixture.game-design-brief"), ())

    def test_scene_composition_input_is_rejected_before_reasoning(self) -> None:
        planning_refs = self._publish_planning()
        art_refs = self._publish_art(planning_refs)
        self._create_program_graph((*planning_refs, art_refs[1]))
        reasoner = FakeProgramReasoner()

        with self.assertRaises(ProgramInputValidationError):
            self._agent(reasoner).run(_program_request())

        self.assertEqual(reasoner.calls, [])

    def test_forbidden_capability_is_rejected_before_reasoning(self) -> None:
        planning_refs = self._publish_planning()
        self._create_program_graph(
            planning_refs,
            capabilities=("artifact.write", "shell.execute"),
        )
        reasoner = FakeProgramReasoner()

        with self.assertRaises(ProgramCapabilityError):
            self._agent(reasoner).run(_program_request())

        self.assertEqual(reasoner.calls, [])
        self.assertEqual(
            self.graphs.get("graph-program-1").task("task-program-1").status,
            "ready",
        )

    def test_missing_artifact_write_capability_is_rejected(self) -> None:
        planning_refs = self._publish_planning()
        self._create_program_graph(planning_refs, capabilities=("artifact.read",))

        with self.assertRaises(ProgramCapabilityError):
            self._agent(FakeProgramReasoner()).run(_program_request())

    def test_invalid_reasoner_result_fails_only_program_task(self) -> None:
        planning_refs = self._publish_planning()
        self._create_program_graph(planning_refs)
        reasoner = FakeProgramReasoner(result={"not": "a ProgramAgentDraft"})

        with self.assertRaises(ProgramOutputValidationError):
            self._agent(reasoner).run(_program_request())

        task = self.graphs.get("graph-program-1").task("task-program-1")
        self.assertEqual(task.status, "failed")
        self.assertIn("ProgramOutputValidationError", task.last_error)
        self.assertEqual(
            self.artifacts.list_versions("project-1", "program.gameplay-logic-plan"),
            (),
        )

    def test_schema_invalid_logic_is_not_published(self) -> None:
        planning_refs = self._publish_planning()
        self._create_program_graph(planning_refs)
        invalid = ProgramAgentDraft(
            gameplay_logic_plan=GameplayLogicPlan(
                states=(),
                entity_slots=(),
                primitives=(),
                triggers=("enter",),
                rules=("share progress",),
                win_conditions=("complete",),
                lose_conditions=("leave",),
            )
        )

        with self.assertRaises(InvalidArtifactError):
            self._agent(FakeProgramReasoner(result=invalid)).run(_program_request())

        self.assertEqual(
            self.graphs.get("graph-program-1").task("task-program-1").status,
            "failed",
        )
        self.assertEqual(
            self.artifacts.list_versions("project-1", "program.gameplay-logic-plan"),
            (),
        )

    def test_project_version_change_during_reasoning_rejects_output(self) -> None:
        planning_refs = self._publish_planning()
        self._create_program_graph(planning_refs)

        def advance_project() -> None:
            project = self.projects.get("project-1")
            self.projects.apply_patch(
                ProjectStatePatch(
                    patch_id="patch-concurrent-program-change",
                    project_id="project-1",
                    expected_project_version=project.project_version,
                    source="test",
                    changes={"validation_status": "blocked"},
                )
            )

        with self.assertRaises(ProgramContextStaleError):
            self._agent(FakeProgramReasoner(side_effect=advance_project)).run(
                _program_request()
            )

        self.assertEqual(
            self.graphs.get("graph-program-1").task("task-program-1").status,
            "failed",
        )

    def test_same_request_is_idempotent_and_changed_reuse_is_rejected(self) -> None:
        planning_refs = self._publish_planning()
        self._create_program_graph(planning_refs)
        reasoner = FakeProgramReasoner()
        agent = self._agent(reasoner)
        request = _program_request()

        first = agent.run(request)
        replay = agent.run(request)

        self.assertIs(first, replay)
        self.assertEqual(len(reasoner.calls), 1)
        with self.assertRaises(ProgramAgentError):
            agent.run(_program_request(objective="different objective with reused request id"))

    def test_planning_revision_stales_logic_and_allows_versioned_rebuild(self) -> None:
        planning_v1 = self._publish_planning(suffix="1", label="v1")
        self._create_program_graph(planning_v1, graph_id="graph-program-1")
        self._agent(FakeProgramReasoner(label="v1")).run(_program_request())

        planning_v2 = self._publish_planning(suffix="2", label="v2")

        stale = self.artifacts.current(
            "project-1",
            "program.gameplay-logic-plan",
            require_usable=False,
        )
        self.assertEqual(stale.registry_status, "stale")
        self._create_program_graph(
            planning_v2,
            graph_id="graph-program-2",
            task_id="task-program-2",
        )
        result = self._agent(FakeProgramReasoner(label="v2")).run(
            _program_request(
                request_id="request-program-2",
                graph_id="graph-program-2",
                task_id="task-program-2",
            )
        )

        self.assertEqual(result.artifact_refs, ("program.gameplay-logic-plan@2",))
        self.assertEqual(
            self.artifacts.get("project-1", result.artifact_refs[0]).artifact.dependencies,
            planning_v2,
        )

    def test_art_revision_stales_logic_when_art_direction_was_consumed(self) -> None:
        planning_refs = self._publish_planning()
        art_v1 = self._publish_art(planning_refs, suffix="1", label="v1")
        self._create_program_graph((*planning_refs, art_v1[0]))
        self._agent(FakeProgramReasoner()).run(_program_request())

        art_v2 = self._publish_art(planning_refs, suffix="2", label="v2")

        stale = self.artifacts.current(
            "project-1",
            "program.gameplay-logic-plan",
            require_usable=False,
        )
        self.assertEqual(stale.registry_status, "stale")
        self._create_program_graph(
            (*planning_refs, art_v2[0]),
            graph_id="graph-program-2",
            task_id="task-program-2",
        )
        result = self._agent(FakeProgramReasoner(label="v2")).run(
            _program_request(
                request_id="request-program-2",
                graph_id="graph-program-2",
                task_id="task-program-2",
            )
        )
        self.assertEqual(result.artifact_refs, ("program.gameplay-logic-plan@2",))

    def test_stale_optional_art_input_is_rejected_before_reasoning(self) -> None:
        planning_refs = self._publish_planning()
        art_v1 = self._publish_art(planning_refs)
        self._create_program_graph((*planning_refs, art_v1[0]))
        current_version = self.projects.get("project-1").project_version
        direction_v2 = ArtifactEnvelope(
            artifact_id="art.direction",
            artifact_type="ArtDirection",
            version=2,
            producer_role="art",
            source_task_id="task-art-revision",
            base_project_version=current_version,
            base_world_version=0,
            dependencies=planning_refs,
            snapshot_source="none",
            non_executable=True,
            status="validated",
            payload=ArtDirection(
                style_keywords=("warm fantasy v2",),
                palette=("amber",),
                lighting=("lantern",),
                avoid_keywords=("horror",),
            ),
        )
        self.artifacts.register(
            project_id="project-1",
            artifact=direction_v2,
            expected_project_version=current_version,
            patch_id="patch-register-art-direction-v2",
            source="test",
        )
        reasoner = FakeProgramReasoner()

        with self.assertRaises(ProgramInputValidationError):
            self._agent(reasoner).run(_program_request())

        self.assertEqual(reasoner.calls, [])
        self.assertEqual(
            self.graphs.get("graph-program-1").task("task-program-1").status,
            "blocked",
        )

    def test_snapshot_sourced_optional_art_is_rejected_before_reasoning(self) -> None:
        planning_refs = self._publish_planning()
        current_version = self.projects.get("project-1").project_version
        self.projects.apply_patch(ProjectStatePatch(
            patch_id="patch-program-snapshot-world-v1",
            project_id="project-1",
            expected_project_version=current_version,
            source="test",
            changes={"scene_world_version": 1},
        ))
        current_version = self.projects.get("project-1").project_version
        fixtures = []
        for source in ("mock", "runtime"):
            fixtures.append(
                ArtifactEnvelope(
                    artifact_id=f"fixture.{source}-art-direction",
                    artifact_type="ArtDirection",
                    version=1,
                    producer_role="art",
                    source_task_id="fixture",
                    base_project_version=current_version,
                    base_world_version=1,
                    dependencies=planning_refs,
                    snapshot_source=source,
                    non_executable=True,
                    status="validated",
                    payload=ArtDirection(
                        style_keywords=(source,),
                        palette=("gray",),
                        lighting=("neutral",),
                        avoid_keywords=("writes",),
                    ),
                )
            )
        self.artifacts.register_many(
            project_id="project-1",
            artifacts=fixtures,
            expected_project_version=current_version,
            patch_id="patch-register-snapshot-art-fixtures",
            source="test",
        )
        project_version = self.projects.get("project-1").project_version
        tasks = tuple(
            _program_task(
                f"task-program-{source}",
                (*planning_refs, f"fixture.{source}-art-direction@1"),
            )
            for source in ("mock", "runtime")
        )
        self.graphs.create_graph(
            graph_id="graph-program-snapshot",
            project_id="project-1",
            tasks=tasks,
            expected_project_version=project_version,
            patch_id="patch-create-program-snapshot",
            source="test",
        )

        for source in ("mock", "runtime"):
            with self.subTest(source=source):
                reasoner = FakeProgramReasoner()
                agent = self._agent(reasoner)
                with self.assertRaises(ProgramIsolationError):
                    agent.run(
                        _program_request(
                            request_id=f"request-program-{source}",
                            graph_id="graph-program-snapshot",
                            task_id=f"task-program-{source}",
                        )
                    )
                self.assertEqual(reasoner.calls, [])

    def test_program_agent_module_has_no_runtime_or_execution_imports(self) -> None:
        source = Path(program_agent_module.__file__).read_text(encoding="utf-8")
        forbidden = (
            "services.agent_runtime",
            "services.lanchat",
            "SceneWorldSnapshot",
            "SceneTools",
            "ActionProposal",
            "EntityBindingPlan",
            "RuntimeCppBridge",
            "subprocess",
        )
        for value in forbidden:
            self.assertNotIn(value, source)


if __name__ == "__main__":
    unittest.main()
