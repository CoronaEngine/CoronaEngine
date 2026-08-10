from __future__ import annotations

from copy import deepcopy
import json
import unittest

from editor.plugins.AITool.services.agent_collaboration.agents.program_agent import (
    ProgramContext,
    ProgramRequest,
)
from editor.plugins.AITool.services.agent_collaboration.agents.art_agent import (
    ArtContext,
    ArtInputArtifactContext,
    ArtRequest,
)
from editor.plugins.AITool.services.agent_collaboration.production_reasoners import (
    CollaborationReasoningError,
    ProductionArtReasoner,
    ProductionProgramReasoner,
)


def _request() -> ProgramRequest:
    return ProgramRequest(
        request_id="request.program.production",
        project_id="project.production",
        graph_id="graph.production",
        task_id="task.production.program",
        logic_objective="Define a key, door, and goal loop.",
        constraints=("no scripts",),
        acceptance_criteria=("logic validates",),
        requested_by="host",
    )


def _context() -> ProgramContext:
    return ProgramContext(
        project_id="project.production",
        project_version=1,
        input_artifacts=(),
    )


def _valid_payload() -> dict:
    return {
        "gameplay_logic_plan": {
            "states": ["key_available", "key_collected", "door_unlocked", "complete"],
            "entity_roles": {
                "player_spawn": {"slot_id": "player", "required_capabilities": ["player"]},
                "collectible_key": {"slot_id": "key", "required_capabilities": ["collectible"]},
                "locked_door": {"slot_id": "door", "required_capabilities": ["lockable"]},
                "goal_zone": {"slot_id": "goal", "required_capabilities": ["trigger_zone"]},
            },
            "primitives": [
                {
                    "primitive_id": "collect",
                    "kind": "on_collect",
                    "subject_slot": "key",
                    "target_slot": "player",
                    "parameters": {"state_key": "has_key", "set_value": True},
                },
                {
                    "primitive_id": "unlock",
                    "kind": "unlock",
                    "subject_slot": "key",
                    "target_slot": "door",
                    "parameters": {"required_state": "has_key", "required_value": True},
                },
                {
                    "primitive_id": "enter",
                    "kind": "on_enter",
                    "subject_slot": "goal",
                    "target_slot": "player",
                    "parameters": {},
                },
                {
                    "primitive_id": "complete",
                    "kind": "complete_objective",
                    "subject_slot": "goal",
                    "target_slot": "player",
                    "parameters": {"objective_id": "reach_goal"},
                },
            ],
            "win_conditions": ["complete"],
            "lose_conditions": ["none"],
            "triggers": ["collect", "enter"],
            "rules": ["door requires key"],
        },
    }


class ProductionProgramReasonerTests(unittest.TestCase):
    def _generate(self, payload: dict):
        reasoner = ProductionProgramReasoner(
            lambda _purpose, _system, _user: json.dumps(payload, ensure_ascii=False)
        )
        return reasoner.generate(_request(), _context())

    def _assert_error(self, payload: dict, code: str) -> CollaborationReasoningError:
        with self.assertRaises(CollaborationReasoningError) as caught:
            self._generate(payload)
        error = caught.exception
        self.assertEqual(error.stage, "program")
        self.assertEqual(error.error_code, code)
        self.assertTrue(error.response_hash.startswith("sha256:"))
        return error

    def test_valid_payload_uses_authoritative_validator_manifest(self) -> None:
        captured: dict[str, str] = {}

        def complete(_purpose, _system, user):
            captured["user"] = user
            return json.dumps(_valid_payload(), ensure_ascii=False)

        result = ProductionProgramReasoner(complete).generate(_request(), _context())
        prompt = json.loads(captured["user"])

        self.assertEqual(len(result.gameplay_logic_plan.entity_slots), 4)
        self.assertIn("required_state", prompt["validator_manifest"]["required_parameters"]["unlock"])
        self.assertEqual(
            prompt["validator_manifest"]["capability_requirements"]["on_collect"]["subject_slot"],
            "collectible",
        )
        self.assertIn("entity_roles", prompt["output_schema"]["gameplay_logic_plan"])
        self.assertNotIn("entity_slots", prompt["output_schema"]["gameplay_logic_plan"])
        self.assertIn(
            "semantic_role values must be unique",
            " ".join(prompt["validator_manifest"]["identity_rules"]),
        )
        self.assertNotIn("form a cycle", " ".join(prompt["validator_manifest"]["identity_rules"]))
        self.assertIn("must differ", " ".join(prompt["validator_manifest"]["identity_rules"]))

    def test_unknown_slot_is_reported(self) -> None:
        payload = deepcopy(_valid_payload())
        payload["gameplay_logic_plan"]["primitives"][0]["target_slot"] = "missing"
        self._assert_error(payload, "unknown_slot")

    def test_unsupported_primitive_is_reported(self) -> None:
        payload = deepcopy(_valid_payload())
        payload["gameplay_logic_plan"]["primitives"][0]["kind"] = "run_script"
        self._assert_error(payload, "unsupported_primitive")

    def test_capability_mismatch_is_reported(self) -> None:
        payload = deepcopy(_valid_payload())
        payload["gameplay_logic_plan"]["entity_roles"]["collectible_key"]["required_capabilities"] = ["decorative"]
        self._assert_error(payload, "capability_mismatch")

    def test_invalid_parameters_are_reported(self) -> None:
        payload = deepcopy(_valid_payload())
        payload["gameplay_logic_plan"]["primitives"][1]["parameters"]["script"] = "open()"
        self._assert_error(payload, "invalid_parameters")

    def test_bidirectional_interaction_participants_are_not_a_dependency_cycle(self) -> None:
        payload = deepcopy(_valid_payload())
        payload["gameplay_logic_plan"]["primitives"] = [
            {
                "primitive_id": "set-a",
                "kind": "set_state",
                "subject_slot": "player",
                "target_slot": "key",
                "parameters": {"state_key": "a", "value": True},
            },
            {
                "primitive_id": "set-b",
                "kind": "set_state",
                "subject_slot": "key",
                "target_slot": "player",
                "parameters": {"state_key": "b", "value": True},
            },
        ]
        result = self._generate(payload)
        self.assertEqual(len(result.gameplay_logic_plan.primitives), 2)

    def test_self_slot_reference_is_reported_with_safe_diagnostics(self) -> None:
        payload = deepcopy(_valid_payload())
        payload["gameplay_logic_plan"]["primitives"][0]["target_slot"] = "key"
        error = self._assert_error(payload, "self_slot_reference")
        self.assertIn("validation:primitives[0]:self_slot_reference", error.diagnostic_refs)
        self.assertIn("primitive[0]:collect:key->key", error.diagnostic_refs)

    def test_duplicate_primitive_identity_is_reported(self) -> None:
        payload = deepcopy(_valid_payload())
        payload["gameplay_logic_plan"]["primitives"][1]["primitive_id"] = "collect"
        self._assert_error(payload, "duplicate_primitive_id")

    def test_duplicate_slot_id_is_reported(self) -> None:
        payload = deepcopy(_valid_payload())
        payload["gameplay_logic_plan"]["entity_roles"]["collectible_key"]["slot_id"] = "player"
        error = self._assert_error(payload, "duplicate_slot_id")
        self.assertIn("slot_id", error.field_path)

    def test_duplicate_semantic_role_is_reported(self) -> None:
        payload = deepcopy(_valid_payload())
        payload["gameplay_logic_plan"]["entity_roles"]["PLAYER_SPAWN"] = {
            "slot_id": "player-copy",
            "required_capabilities": ["player"],
        }
        error = self._assert_error(payload, "duplicate_semantic_role")
        self.assertIn("semantic_role", error.field_path)

    def test_invalid_semantic_role_is_reported(self) -> None:
        payload = deepcopy(_valid_payload())
        payload["gameplay_logic_plan"]["entity_roles"]["Goal Zone"] = (
            payload["gameplay_logic_plan"]["entity_roles"].pop("goal_zone")
        )
        self._assert_error(payload, "invalid_semantic_role")

    def test_invalid_json_has_structured_error(self) -> None:
        reasoner = ProductionProgramReasoner(lambda *_args: "not-json")
        with self.assertRaises(CollaborationReasoningError) as caught:
            reasoner.generate(_request(), _context())

        self.assertEqual(caught.exception.error_code, "invalid_json_object")
        self.assertEqual(caught.exception.stage, "program")
        self.assertTrue(caught.exception.as_dict()["response_hash"].startswith("sha256:"))


def _art_request() -> ArtRequest:
    return ArtRequest(
        request_id="request.art.production",
        project_id="project.production",
        graph_id="graph.production",
        task_id="task.production.art",
        art_objective="Create a Disney-inspired bedroom art direction.",
        constraints=("low detail is acceptable",),
        acceptance_criteria=("all gameplay roles remain traceable",),
        requested_by="host",
    )


def _art_context() -> ArtContext:
    program_result = ProductionProgramReasoner(
        lambda _purpose, _system, _user: json.dumps(_valid_payload(), ensure_ascii=False)
    ).generate(_request(), _context())
    gameplay = {
        "states": list(program_result.gameplay_logic_plan.states),
        "entity_slots": [
            {
                "slot_id": slot.slot_id,
                "semantic_role": slot.semantic_role,
                "required_capabilities": list(slot.required_capabilities),
            }
            for slot in program_result.gameplay_logic_plan.entity_slots
        ],
        "primitives": [],
        "win_conditions": list(program_result.gameplay_logic_plan.win_conditions),
        "lose_conditions": list(program_result.gameplay_logic_plan.lose_conditions),
    }
    return ArtContext(
        project_id="project.production",
        project_version=1,
        input_artifacts=(
            ArtInputArtifactContext(
                artifact_ref="artifact.brief@1",
                artifact_type="GameDesignBrief",
                version=1,
                content_hash="sha256:brief",
                payload={"project_goal": "Disney-inspired bedroom"},
            ),
            ArtInputArtifactContext(
                artifact_ref="artifact.gameplay@1",
                artifact_type="GameplayLogicPlan",
                version=1,
                content_hash="sha256:gameplay",
                payload=gameplay,
            ),
        ),
    )


def _valid_art_payload() -> dict:
    return {
        "art_direction": {
            "style_keywords": ["Disney-inspired bedroom", "storybook"],
            "palette": ["rose", "sky blue"],
            "lighting": ["soft warm window light"],
            "avoid_keywords": ["horror"],
        },
        "scene_composition_plan": {
            "scene_type": "bedroom",
            "environment_requirements": ["room shell", "walkable floor"],
            "layout_rules": ["keep the center path clear"],
            "global_visual_prompt": "whimsical Disney-inspired bedroom prop, clean low-detail 3D asset",
            "role_visual_overrides": {
                "collectible_key": "gold key with a star-shaped bow",
            },
        },
    }


class ProductionArtReasonerTests(unittest.TestCase):
    def _generate(self, payload: dict):
        reasoner = ProductionArtReasoner(
            lambda _purpose, _system, _user: json.dumps(payload, ensure_ascii=False)
        )
        return reasoner.generate(_art_request(), _art_context())

    def test_program_roles_are_authoritative_and_ordered(self) -> None:
        captured: dict[str, str] = {}

        def complete(_purpose, _system, user):
            captured["user"] = user
            return json.dumps(_valid_art_payload(), ensure_ascii=False)

        result = ProductionArtReasoner(complete).generate(_art_request(), _art_context())
        composition = result.scene_composition_plan
        prompt = json.loads(captured["user"])

        self.assertEqual(
            composition.entity_requirements,
            ("player_spawn", "collectible_key", "locked_door", "goal_zone"),
        )
        self.assertEqual(tuple(composition.image_prompts), composition.entity_requirements)
        self.assertIn("gold key", composition.image_prompts["collectible_key"])
        self.assertEqual(
            [item["semantic_role"] for item in prompt["art_role_manifest"]],
            list(composition.entity_requirements),
        )
        self.assertNotIn("entity_requirements", prompt["output_schema"]["scene_composition_plan"])

    def test_empty_avoid_keywords_is_a_valid_art_direction(self) -> None:
        payload = _valid_art_payload()
        payload["art_direction"]["avoid_keywords"] = []

        result = self._generate(payload)

        self.assertEqual(result.art_direction.avoid_keywords, ())

    def test_unknown_role_override_is_rejected(self) -> None:
        payload = _valid_art_payload()
        payload["scene_composition_plan"]["role_visual_overrides"]["invented_role"] = "invalid"
        with self.assertRaises(CollaborationReasoningError) as caught:
            self._generate(payload)
        self.assertEqual(caught.exception.error_code, "art_role_override_unknown")

    def test_global_visual_prompt_is_required(self) -> None:
        payload = _valid_art_payload()
        payload["scene_composition_plan"]["global_visual_prompt"] = ""
        with self.assertRaises(CollaborationReasoningError) as caught:
            self._generate(payload)
        self.assertEqual(caught.exception.error_code, "art_visual_prompt_missing")


if __name__ == "__main__":
    unittest.main()
