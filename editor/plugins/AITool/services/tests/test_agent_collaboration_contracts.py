from __future__ import annotations

import unittest

from editor.plugins.AITool.services.agent_collaboration import (
    AgentTask,
    ArtifactEnvelope,
    ArtDirection,
    EntityBindingPlan,
    GameDesignBrief,
    GameProjectState,
    GameplayEntitySlot,
    GameplayLogicPlan,
    GameplayPrimitiveSpec,
    LevelPlan,
    NonExecutableArtifactError,
    SceneCompositionPlan,
    assert_executable,
    validate_artifact_payload,
)


def _brief(*, goal: str = "Build a cooperative treasure room") -> GameDesignBrief:
    return GameDesignBrief(
        project_goal=goal,
        player_experience=("explore", "cooperate"),
        core_rules=("host confirms world writes",),
        acceptance_criteria=("two players can inspect the same world version",),
    )


def _envelope(**overrides) -> ArtifactEnvelope:
    values = {
        "artifact_id": "artifact-brief-v1",
        "artifact_type": "GameDesignBrief",
        "version": 1,
        "producer_role": "planning",
        "source_task_id": "task-plan-1",
        "base_project_version": 1,
        "base_world_version": 0,
        "dependencies": (),
        "snapshot_source": "none",
        "non_executable": True,
        "status": "validated",
        "payload": _brief(),
    }
    values.update(overrides)
    return ArtifactEnvelope(**values)


class AgentCollaborationContractTests(unittest.TestCase):
    def test_art_direction_avoid_keywords_may_be_empty_but_must_be_a_clean_list(self) -> None:
        base = {
            "style_keywords": ["storybook"],
            "palette": ["rose"],
            "lighting": ["soft window light"],
        }
        valid = validate_artifact_payload("ArtDirection", {**base, "avoid_keywords": []})
        self.assertTrue(valid.valid)

        cases = {
            "missing": base,
            "not_a_list": {**base, "avoid_keywords": "horror"},
            "empty_item": {**base, "avoid_keywords": [""]},
            "normal": {**base, "avoid_keywords": ["horror"]},
        }
        self.assertFalse(validate_artifact_payload("ArtDirection", cases["missing"]).valid)
        self.assertFalse(validate_artifact_payload("ArtDirection", cases["not_a_list"]).valid)
        self.assertFalse(validate_artifact_payload("ArtDirection", cases["empty_item"]).valid)
        self.assertTrue(validate_artifact_payload("ArtDirection", cases["normal"]).valid)

    def test_artifact_type_has_one_functional_producer_role(self) -> None:
        with self.assertRaisesRegex(ValueError, "requires producer_role planning"):
            _envelope(producer_role="art")
        for legacy_persona in ("elder", "little_girl", "bandit", "merchant", "coordinator"):
            with self.subTest(legacy_persona=legacy_persona):
                with self.assertRaisesRegex(ValueError, "unsupported producer_role"):
                    _envelope(producer_role=legacy_persona)

    def test_artifact_hash_is_deterministic_and_payload_is_deeply_immutable(self) -> None:
        first = _envelope(payload={
            "acceptance_criteria": ["same world"],
            "core_rules": ["host confirms"],
            "player_experience": ["explore"],
            "project_goal": "treasure room",
        })
        second = _envelope(payload={
            "project_goal": "treasure room",
            "player_experience": ["explore"],
            "core_rules": ["host confirms"],
            "acceptance_criteria": ["same world"],
        })

        self.assertEqual(first.content_hash, second.content_hash)
        self.assertTrue(first.validation_result.valid)
        with self.assertRaises(TypeError):
            first.payload["project_goal"] = "mutated"  # type: ignore[index]
        exported = first.as_dict()
        exported["payload"]["project_goal"] = "local mutation"
        self.assertEqual(first.as_dict()["payload"]["project_goal"], "treasure room")

    def test_payload_change_changes_content_hash(self) -> None:
        first = _envelope(payload=_brief(goal="first goal"))
        second = _envelope(payload=_brief(goal="second goal"))

        self.assertNotEqual(first.content_hash, second.content_hash)

    def test_validation_result_is_computed_and_invalid_payload_cannot_claim_validated(self) -> None:
        artifact = _envelope(payload={"project_goal": "missing lists"})

        self.assertFalse(artifact.validation_result.valid)
        self.assertEqual(artifact.status, "invalid")
        self.assertIn("core_rules:required_nonempty_text_list", artifact.validation_result.errors)
        with self.assertRaises(TypeError):
            ArtifactEnvelope(  # type: ignore[call-arg]
                artifact_id="forged",
                artifact_type="GameDesignBrief",
                version=1,
                producer_role="planning",
                source_task_id="task-forged",
                base_project_version=1,
                base_world_version=0,
                payload=_brief(),
                validation_result={"valid": True},
            )

    def test_mock_artifact_is_constructible_for_audit_but_never_executable(self) -> None:
        artifact = _envelope(snapshot_source="mock", non_executable=True)

        self.assertEqual(artifact.snapshot_source, "mock")
        with self.assertRaises(NonExecutableArtifactError):
            assert_executable(artifact)
        with self.assertRaises(NonExecutableArtifactError):
            _envelope(snapshot_source="mock", non_executable=False)

    def test_validated_production_artifact_can_pass_future_execution_boundary(self) -> None:
        artifact = _envelope(
            snapshot_source="runtime",
            non_executable=False,
            status="validated",
            base_world_version=1,
        )

        assert_executable(artifact)

    def test_executable_artifact_requires_real_versioned_runtime_snapshot(self) -> None:
        with self.assertRaisesRegex(NonExecutableArtifactError, "runtime snapshot_source"):
            _envelope(snapshot_source="none", non_executable=False)
        with self.assertRaisesRegex(NonExecutableArtifactError, "positive base_world_version"):
            _envelope(
                snapshot_source="runtime",
                non_executable=False,
                base_world_version=0,
            )

    def test_non_executable_or_invalid_artifact_is_rejected(self) -> None:
        with self.assertRaises(NonExecutableArtifactError):
            assert_executable(_envelope())
        with self.assertRaises(NonExecutableArtifactError):
            assert_executable(
                _envelope(
                    snapshot_source="runtime",
                    non_executable=False,
                    payload={"project_goal": "invalid"},
                )
            )

    def test_all_six_first_stage_payload_dtos_validate_through_envelope(self) -> None:
        payloads = (
            ("GameDesignBrief", "planning", _brief()),
            (
                "LevelPlan",
                "planning",
                LevelPlan(
                    level_goal="Find the shared treasure",
                    zones=("entry", "vault"),
                    progression=("enter", "solve", "claim"),
                    acceptance_criteria=("both players reach the vault",),
                ),
            ),
            (
                "ArtDirection",
                "art",
                ArtDirection(
                    style_keywords=("warm", "mysterious"),
                    palette=("amber", "deep blue"),
                    lighting=("warm lanterns",),
                    avoid_keywords=("horror",),
                ),
            ),
            (
                "SceneCompositionPlan",
                "art",
                SceneCompositionPlan(
                    scene_type="indoor_room",
                    environment_requirements=("room_box", "room_floor"),
                    entity_requirements=("treasure_chest", "table"),
                    layout_rules=("keep the main path clear",),
                ),
            ),
            (
                "GameplayLogicPlan",
                "program",
                GameplayLogicPlan(
                    states=("searching", "complete"),
                    entity_slots=(
                        GameplayEntitySlot("treasure", "collectible_treasure", ("collectible",)),
                        GameplayEntitySlot("player", "player_spawn", ("player",)),
                    ),
                    primitives=(
                        GameplayPrimitiveSpec("collect-treasure", "on_collect", "treasure", "player", {"state_key": "complete", "set_value": True}),
                    ),
                    triggers=("treasure_found",),
                    rules=("host owns authoritative state",),
                    win_conditions=("treasure found",),
                    lose_conditions=("time expired",),
                ),
            ),
            (
                "EntityBindingPlan",
                "program",
                EntityBindingPlan(
                    snapshot_plan_id="plan-fixture",
                    snapshot_version=3,
                    bindings=({
                        "slot_id": "treasure",
                        "entity_id": "entity-1",
                        "entity_version": 1,
                        "asset_id": "asset-treasure-1",
                        "semantic_role": "treasure_chest",
                        "required_capabilities": ["collectible"],
                    },),
                ),
            ),
        )
        artifacts: dict[str, ArtifactEnvelope] = {}
        for artifact_type, role, payload in payloads:
            with self.subTest(artifact_type=artifact_type):
                artifact = ArtifactEnvelope(
                    artifact_id=f"artifact-{artifact_type}-v1",
                    artifact_type=artifact_type,
                    version=1,
                    producer_role=role,
                    source_task_id=f"task-{artifact_type}-1",
                    base_project_version=1,
                    base_world_version=3 if artifact_type == "EntityBindingPlan" else 0,
                    snapshot_source="mock" if artifact_type == "EntityBindingPlan" else "none",
                    non_executable=True,
                    payload=payload,
                )
                self.assertTrue(artifact.validation_result.valid)
                artifacts[artifact_type] = artifact

        binding = artifacts["EntityBindingPlan"]
        with self.assertRaises(NonExecutableArtifactError):
            assert_executable(binding)

    def test_gameplay_logic_plan_v1_1_validates_the_demo_primitive_chain(self) -> None:
        artifact = ArtifactEnvelope(
            artifact_id="program.gameplay-logic-plan",
            artifact_type="GameplayLogicPlan",
            version=1,
            producer_role="program",
            source_task_id="task-program-1",
            base_project_version=1,
            base_world_version=0,
            snapshot_source="none",
            non_executable=True,
            status="validated",
            payload=GameplayLogicPlan(
                states=("key_collected", "door_unlocked", "objective_complete"),
                entity_slots=(
                    GameplayEntitySlot("player", "player_spawn", ("player",)),
                    GameplayEntitySlot("key", "collectible_key", ("collectible",)),
                    GameplayEntitySlot("door", "locked_door", ("lockable",)),
                    GameplayEntitySlot("goal", "goal_zone", ("trigger_zone",)),
                ),
                primitives=(
                    GameplayPrimitiveSpec("collect-key", "on_collect", "key", "player", {"state_key": "key_collected", "set_value": True}),
                    GameplayPrimitiveSpec("set-door-state", "set_state", "key", "player", {"state_key": "door_unlocked", "value": True}),
                    GameplayPrimitiveSpec("unlock-door", "unlock", "key", "door", {"required_state": "key_collected"}),
                    GameplayPrimitiveSpec("enter-goal", "on_enter", "goal", "player", {}),
                    GameplayPrimitiveSpec("complete-objective", "complete_objective", "goal", "player", {"objective_id": "reach_goal"}),
                ),
                win_conditions=("objective_complete",),
                lose_conditions=("none",),
            ),
        )

        self.assertTrue(artifact.validation_result.valid)
        self.assertEqual(artifact.status, "validated")

    def test_gameplay_logic_plan_v1_1_rejects_unknown_parameters_without_dependency_cycle_inference(self) -> None:
        result = validate_artifact_payload(
            "GameplayLogicPlan",
            {
                "states": ["ready"],
                "entity_slots": [
                    {"slot_id": "key", "semantic_role": "collectible_key", "required_capabilities": ["collectible"]},
                    {"slot_id": "door", "semantic_role": "locked_door", "required_capabilities": ["lockable"]},
                ],
                "primitives": [
                    {
                        "primitive_id": "collect-key",
                        "kind": "on_collect",
                        "subject_slot": "key",
                        "target_slot": "door",
                        "parameters": {"unexpected": True},
                    },
                    {
                        "primitive_id": "unlock-key",
                        "kind": "unlock",
                        "subject_slot": "door",
                        "target_slot": "key",
                        "parameters": {"required_state": "ready"},
                    },
                ],
                "win_conditions": ["ready"],
                "lose_conditions": ["none"],
            },
        )

        self.assertFalse(result.valid)
        self.assertIn("primitives[0].parameters:unknown:unexpected", result.errors)
        self.assertIn("primitives[1].target_slot:requires_lockable", result.errors)
        self.assertNotIn("primitives:cyclic_slot_reference", result.errors)

    def test_gameplay_logic_plan_v1_1_rejects_self_slot_reference(self) -> None:
        result = validate_artifact_payload(
            "GameplayLogicPlan",
            {
                "states": ["ready"],
                "entity_slots": [
                    {"slot_id": "key", "semantic_role": "collectible_key", "required_capabilities": ["collectible"]},
                ],
                "primitives": [
                    {
                        "primitive_id": "collect-self",
                        "kind": "on_collect",
                        "subject_slot": "key",
                        "target_slot": "key",
                        "parameters": {"state_key": "ready", "set_value": True},
                    },
                ],
                "win_conditions": ["ready"],
                "lose_conditions": ["none"],
            },
        )

        self.assertFalse(result.valid)
        self.assertIn("primitives[0]:self_slot_reference", result.errors)

    def test_legacy_gameplay_logic_payload_is_retained_for_audit_but_invalid_for_new_use(self) -> None:
        legacy = ArtifactEnvelope(
            artifact_id="program.gameplay-logic-plan",
            artifact_type="GameplayLogicPlan",
            version=1,
            producer_role="program",
            source_task_id="task-program-legacy",
            base_project_version=1,
            base_world_version=0,
            snapshot_source="none",
            non_executable=True,
            status="validated",
            payload={
                "states": ["searching", "complete"],
                "triggers": ["treasure_found"],
                "rules": ["host owns progress"],
                "win_conditions": ["complete"],
                "lose_conditions": ["timeout"],
            },
        )

        self.assertEqual(legacy.status, "invalid")
        self.assertFalse(legacy.validation_result.valid)
        self.assertIn("entity_slots:required_nonempty_list", legacy.validation_result.errors)
        self.assertEqual(legacy.as_dict()["payload"]["triggers"], ["treasure_found"])
        with self.assertRaises(NonExecutableArtifactError):
            assert_executable(legacy)

    def test_project_and_task_contracts_validate_without_runtime_dependencies(self) -> None:
        project = GameProjectState(
            project_id="project-1",
            project_version=1,
            room_id="room-1",
            artifact_refs=("artifact-b", "artifact-a", "artifact-a"),
        )
        task = AgentTask(
            task_id="task-plan-1",
            assigned_role="planning",
            objective="Create a validated design brief",
            input_artifact_refs=(),
            output_types=("LevelPlan", "GameDesignBrief"),
            depends_on=(),
            acceptance_criteria=("schema valid",),
            capability_set=("artifact.write",),
        )

        self.assertEqual(project.artifact_refs, ("artifact-a", "artifact-b"))
        self.assertEqual(task.output_types, ("GameDesignBrief", "LevelPlan"))
        self.assertEqual(task.status, "pending")
        with self.assertRaises(ValueError):
            GameProjectState(
                project_id="project-invalid",
                project_version=1,
                room_id="room-1",
                validation_status="pretend-valid",
            )
        with self.assertRaises(TypeError):
            _envelope(dependencies="artifact-not-a-sequence")
        with self.assertRaises(ValueError):
            AgentTask(
                task_id="task-invalid",
                assigned_role="planning",
                objective="Missing acceptance criteria",
                input_artifact_refs=(),
                output_types=("GameDesignBrief",),
                depends_on=(),
                acceptance_criteria=(),
                capability_set=(),
            )


if __name__ == "__main__":
    unittest.main()
