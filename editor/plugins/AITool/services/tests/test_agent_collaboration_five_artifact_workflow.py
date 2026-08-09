from __future__ import annotations

from pathlib import Path
import unittest

from editor.plugins.AITool.services.agent_collaboration import (
    ARTIFACT_LINEAGE_IDS,
    RED_PROJECT_ARTIFACT_TYPES,
    AgentTask,
    AgentTaskGraphStore,
    ArtDirection,
    ArtifactRegistry,
    GameDesignBrief,
    GameplayEntitySlot,
    GameplayLogicPlan,
    GameplayPrimitiveSpec,
    LevelPlan,
    NonExecutableArtifactError,
    ProjectArtifactBundleIncompleteError,
    ProjectArtifactBundleReader,
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
    ProgramRequest,
)
import editor.plugins.AITool.services.agent_collaboration.artifact_bundle as artifact_bundle_module


class PlanningReasoner:
    def __init__(self, label: str) -> None:
        self.label = label
        self.calls = 0

    def generate(self, request, _context):
        self.calls += 1
        return PlanningAgentDraft(
            game_design_brief=GameDesignBrief(
                project_goal=f"{request.project_goal} {self.label}",
                player_experience=("explore", "cooperate"),
                core_rules=("share objective progress",),
                acceptance_criteria=request.acceptance_criteria,
            ),
            level_plan=LevelPlan(
                level_goal=f"Complete the shared objective {self.label}",
                zones=("entry", "objective"),
                progression=("enter", "discover", "complete"),
                acceptance_criteria=request.acceptance_criteria,
            ),
        )


class ArtReasoner:
    def __init__(self, label: str, *, fail: bool = False) -> None:
        self.label = label
        self.fail = fail
        self.calls = 0

    def generate(self, _request, _context):
        self.calls += 1
        if self.fail:
            raise RuntimeError("art reasoner unavailable")
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
                layout_rules=("keep entry clear", "focus objective at rear axis"),
            ),
        )


class ProgramReasoner:
    def __init__(self, label: str) -> None:
        self.label = label
        self.calls = 0

    def generate(self, _request, _context):
        self.calls += 1
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


class FiveArtifactWorkflowTests(unittest.TestCase):
    def setUp(self) -> None:
        self.projects = ProjectStateStore()
        self.projects.create_project(project_id="project-1", room_id="room-1", source="test")
        self.artifacts = ArtifactRegistry(self.projects)
        self.graphs = AgentTaskGraphStore(self.projects, self.artifacts)
        self.bundle_reader = ProjectArtifactBundleReader(
            project_states=self.projects,
            artifacts=self.artifacts,
            task_graphs=self.graphs,
        )

    @staticmethod
    def _refs(version: int) -> dict[str, str]:
        return {
            artifact_type: f"{artifact_id}@{version}"
            for artifact_type, artifact_id in ARTIFACT_LINEAGE_IDS.items()
            if artifact_type in RED_PROJECT_ARTIFACT_TYPES
        }

    def _create_graph(self, version: int) -> tuple[str, str, str, str]:
        refs = self._refs(version)
        graph_id = f"graph-collaboration-v{version}"
        planning_task_id = f"task-planning-v{version}"
        art_task_id = f"task-art-v{version}"
        program_task_id = f"task-program-v{version}"
        tasks = (
            AgentTask(
                task_id=planning_task_id,
                assigned_role="planning",
                objective="Produce planning contracts",
                input_artifact_refs=(),
                output_types=("GameDesignBrief", "LevelPlan"),
                depends_on=(),
                acceptance_criteria=("planning contracts validate",),
                capability_set=("artifact.write",),
            ),
            AgentTask(
                task_id=art_task_id,
                assigned_role="art",
                objective="Produce art contracts",
                input_artifact_refs=(
                    refs["GameDesignBrief"],
                    refs["LevelPlan"],
                ),
                output_types=("ArtDirection", "SceneCompositionPlan"),
                depends_on=(planning_task_id,),
                acceptance_criteria=("art contracts validate",),
                capability_set=("artifact.write",),
            ),
            AgentTask(
                task_id=program_task_id,
                assigned_role="program",
                objective="Produce gameplay logic",
                input_artifact_refs=(
                    refs["GameDesignBrief"],
                    refs["LevelPlan"],
                    refs["ArtDirection"],
                ),
                output_types=("GameplayLogicPlan",),
                depends_on=(art_task_id,),
                acceptance_criteria=("gameplay logic validates",),
                capability_set=("artifact.read", "artifact.write"),
            ),
        )
        project_version = self.projects.get("project-1").project_version
        self.graphs.create_graph(
            graph_id=graph_id,
            project_id="project-1",
            tasks=tasks,
            expected_project_version=project_version,
            patch_id=f"patch-create-{graph_id}",
            source="test",
        )
        return graph_id, planning_task_id, art_task_id, program_task_id

    def _run_planning(self, graph_id: str, task_id: str, *, label: str) -> None:
        PlanningAgent(
            project_states=self.projects,
            artifacts=self.artifacts,
            task_graphs=self.graphs,
            reasoner=PlanningReasoner(label),
        ).run(
            PlanningRequest(
                request_id=f"request-{task_id}",
                project_id="project-1",
                graph_id=graph_id,
                task_id=task_id,
                project_goal="Build a cooperative treasure room",
                constraints=("shared multiplayer state",),
                acceptance_criteria=("players share objective progress",),
                requested_by="host",
            )
        )

    def _run_art(
        self,
        graph_id: str,
        task_id: str,
        *,
        label: str,
        request_suffix: str = "",
        fail: bool = False,
    ) -> ArtReasoner:
        reasoner = ArtReasoner(label, fail=fail)
        ArtAgent(
            project_states=self.projects,
            artifacts=self.artifacts,
            task_graphs=self.graphs,
            reasoner=reasoner,
        ).run(
            ArtRequest(
                request_id=f"request-{task_id}{request_suffix}",
                project_id="project-1",
                graph_id=graph_id,
                task_id=task_id,
                art_objective="Define coherent art direction",
                constraints=("no scene writes",),
                acceptance_criteria=("requirements are structured",),
                requested_by="host",
            )
        )
        return reasoner

    def _run_program(self, graph_id: str, task_id: str, *, label: str) -> None:
        ProgramAgent(
            project_states=self.projects,
            artifacts=self.artifacts,
            task_graphs=self.graphs,
            reasoner=ProgramReasoner(label),
        ).run(
            ProgramRequest(
                request_id=f"request-{task_id}",
                project_id="project-1",
                graph_id=graph_id,
                task_id=task_id,
                logic_objective="Define cooperative objective logic",
                constraints=("no scripts", "no scene writes"),
                acceptance_criteria=("states and outcomes are structured",),
                requested_by="host",
            )
        )

    def _complete_graph(self, version: int):
        graph_id, planning_task, art_task, program_task = self._create_graph(version)
        self._run_planning(graph_id, planning_task, label=f"v{version}")
        self._run_art(graph_id, art_task, label=f"v{version}")
        self._run_program(graph_id, program_task, label=f"v{version}")
        return self.bundle_reader.build(project_id="project-1", graph_id=graph_id)

    def test_single_business_dag_produces_deterministic_five_artifact_bundle(self) -> None:
        graph_id, planning_task, art_task, program_task = self._create_graph(1)
        initial = self.graphs.get(graph_id)
        self.assertEqual(initial.task(planning_task).status, "ready")
        self.assertEqual(initial.task(art_task).status, "pending")
        self.assertEqual(initial.task(program_task).status, "pending")
        with self.assertRaises(ProjectArtifactBundleIncompleteError):
            self.bundle_reader.build(project_id="project-1", graph_id=graph_id)

        self._run_planning(graph_id, planning_task, label="v1")
        after_planning = self.graphs.get(graph_id)
        self.assertEqual(after_planning.task(planning_task).status, "completed")
        self.assertEqual(after_planning.task(art_task).status, "ready")
        self.assertEqual(after_planning.task(program_task).status, "pending")

        self._run_art(graph_id, art_task, label="v1")
        after_art = self.graphs.get(graph_id)
        self.assertEqual(after_art.task(art_task).status, "completed")
        self.assertEqual(after_art.task(program_task).status, "ready")

        self._run_program(graph_id, program_task, label="v1")
        project_before = self.projects.get("project-1")
        history_before = self.graphs.history(graph_id)
        bundle = self.bundle_reader.build(project_id="project-1", graph_id=graph_id)
        replay = self.bundle_reader.build(project_id="project-1", graph_id=graph_id)

        self.assertEqual(self.graphs.get(graph_id).status, "completed")
        self.assertEqual(set(bundle.entries), RED_PROJECT_ARTIFACT_TYPES)
        self.assertEqual(len(bundle.artifact_refs), 5)
        self.assertEqual(bundle, replay)
        self.assertEqual(bundle.content_hash, replay.content_hash)
        self.assertTrue(bundle.content_hash.startswith("sha256:"))
        self.assertTrue(bundle.as_dict()["non_executable"])
        self.assertEqual(self.projects.get("project-1"), project_before)
        self.assertEqual(self.graphs.history(graph_id), history_before)
        self.assertEqual(set(project_before.artifact_refs), set(bundle.artifact_refs))
        for artifact_ref in bundle.artifact_refs:
            record = self.artifacts.get("project-1", artifact_ref)
            self.assertTrue(record.usable)
            self.assertTrue(record.artifact.validation_result.valid)
            with self.assertRaises(NonExecutableArtifactError):
                assert_executable(record.artifact)
        self.assertNotIn("EntityBindingPlan", bundle.entries)

    def test_planning_revision_stales_downstream_then_rebuilds_bundle_v2(self) -> None:
        bundle_v1 = self._complete_graph(1)
        graph_id, planning_task, art_task, program_task = self._create_graph(2)

        self._run_planning(graph_id, planning_task, label="v2")

        self.assertEqual(
            self.artifacts.current(
                "project-1",
                ARTIFACT_LINEAGE_IDS["ArtDirection"],
                require_usable=False,
            ).registry_status,
            "stale",
        )
        self.assertEqual(
            self.artifacts.current(
                "project-1",
                ARTIFACT_LINEAGE_IDS["GameplayLogicPlan"],
                require_usable=False,
            ).registry_status,
            "stale",
        )
        with self.assertRaises(ProjectArtifactBundleIncompleteError):
            self.bundle_reader.build(project_id="project-1", graph_id=graph_id)

        self._run_art(graph_id, art_task, label="v2")
        self._run_program(graph_id, program_task, label="v2")
        bundle_v2 = self.bundle_reader.build(project_id="project-1", graph_id=graph_id)

        self.assertNotEqual(bundle_v1.content_hash, bundle_v2.content_hash)
        self.assertTrue(all(value.endswith("@2") for value in bundle_v2.artifact_refs))
        self.assertEqual(
            set(self.projects.get("project-1").artifact_refs),
            set(bundle_v2.artifact_refs),
        )
        for artifact_type, artifact_id in ARTIFACT_LINEAGE_IDS.items():
            if artifact_type not in RED_PROJECT_ARTIFACT_TYPES:
                continue
            self.assertEqual(
                self.artifacts.get("project-1", f"{artifact_id}@1").registry_status,
                "superseded",
            )

    def test_failed_art_task_retries_without_replaying_planning(self) -> None:
        graph_id, planning_task, art_task, program_task = self._create_graph(1)
        self._run_planning(graph_id, planning_task, label="v1")

        with self.assertRaisesRegex(RuntimeError, "art reasoner unavailable"):
            self._run_art(graph_id, art_task, label="v1", fail=True)

        failed = self.graphs.get(graph_id)
        self.assertEqual(failed.task(planning_task).status, "completed")
        self.assertEqual(failed.task(planning_task).attempt_count, 1)
        self.assertEqual(failed.task(art_task).status, "failed")
        self.assertEqual(failed.task(program_task).status, "blocked")
        self.graphs.retry_task(graph_id, art_task, source="test")
        self.assertEqual(self.graphs.get(graph_id).task(art_task).status, "ready")

        self._run_art(
            graph_id,
            art_task,
            label="v1",
            request_suffix="-retry",
        )
        self.assertEqual(self.graphs.get(graph_id).task(program_task).status, "ready")
        self._run_program(graph_id, program_task, label="v1")
        bundle = self.bundle_reader.build(project_id="project-1", graph_id=graph_id)

        completed = self.graphs.get(graph_id)
        self.assertEqual(completed.status, "completed")
        self.assertEqual(completed.task(planning_task).attempt_count, 1)
        self.assertEqual(completed.task(art_task).attempt_count, 2)
        self.assertEqual(completed.task(program_task).attempt_count, 1)
        self.assertEqual(len(bundle.artifact_refs), 5)

    def test_bundle_reader_has_no_runtime_snapshot_or_execution_dependency(self) -> None:
        source = Path(artifact_bundle_module.__file__).read_text(encoding="utf-8")
        forbidden = (
            "services.agent_runtime",
            "services.lanchat",
            "SceneWorldSnapshot",
            "RuntimeState",
            "SceneTools",
            "ActionProposal",
            "ToolCallGraph",
            "RuntimeCppBridge",
        )
        for value in forbidden:
            self.assertNotIn(value, source)


if __name__ == "__main__":
    unittest.main()
