from __future__ import annotations

from pathlib import Path
import unittest

import editor.plugins.AITool.services.agent_collaboration.action_proposal as action_proposal_module
import editor.plugins.AITool.services.agent_collaboration.project_gate as project_gate_module
from editor.plugins.AITool.services.agent_collaboration import (
    ActionProposal,
    ArtifactEnvelope,
    EntityBindingPlan,
    GameplayEntitySlot,
    GameplayLogicPlan,
    GameplayPrimitiveSpec,
    NonExecutableArtifactError,
    ProjectGateService,
    gameplay_plan_patch_interface_change_decision,
    gameplay_plan_patch_interface_change_request,
)
from editor.plugins.AITool.services.agent_collaboration.walking_skeleton import (
    build_skeleton_manifest,
)


def _logic_payload(*, primitive_kind: str = "on_collect") -> GameplayLogicPlan:
    primitives = (
        GameplayPrimitiveSpec(
            "collect-key",
            primitive_kind,
            "key",
            "player",
            {"state_key": "key_collected", "set_value": True},
        ),
        GameplayPrimitiveSpec(
            "set-key-state",
            "set_state",
            "player",
            "door",
            {"state_key": "key_collected", "value": True},
        ),
        GameplayPrimitiveSpec(
            "unlock-door",
            "unlock",
            "player",
            "door",
            {"required_state": "key_collected", "required_value": True},
        ),
        GameplayPrimitiveSpec(
            "enter-goal",
            "on_enter",
            "goal",
            "player",
            {"state_key": "door_unlocked", "expected_value": True, "next_state": "complete"},
        ),
        GameplayPrimitiveSpec(
            "finish-demo",
            "complete_objective",
            "goal",
            "player",
            {"objective_id": "escape-room"},
        ),
    )
    return GameplayLogicPlan(
        states=("key_collected", "door_unlocked", "complete"),
        entity_slots=(
            GameplayEntitySlot("player", "player_spawn", ("player",)),
            GameplayEntitySlot("key", "collectible_key", ("collectible",)),
            GameplayEntitySlot("door", "locked_door", ("lockable",)),
            GameplayEntitySlot("goal", "goal_zone", ("trigger_zone",)),
        ),
        primitives=primitives,
        win_conditions=("escape-room",),
        lose_conditions=("none",),
    )


def _logic_artifact(*, primitive_kind: str = "on_collect") -> ArtifactEnvelope:
    return ArtifactEnvelope(
        artifact_id="program.gameplay-logic-plan",
        artifact_type="GameplayLogicPlan",
        version=1,
        producer_role="program",
        source_task_id="task-program-logic",
        base_project_version=1,
        base_world_version=0,
        snapshot_source="none",
        non_executable=True,
        status="validated",
        payload=_logic_payload(primitive_kind=primitive_kind),
    )


def _binding_rows() -> tuple[dict, ...]:
    return (
        {
            "slot_id": "player",
            "semantic_role": "player_spawn",
            "entity_id": "entity-player",
            "entity_version": 1,
            "asset_id": "asset-player",
            "required_capabilities": ["player"],
        },
        {
            "slot_id": "key",
            "semantic_role": "collectible_key",
            "entity_id": "entity-key",
            "entity_version": 1,
            "asset_id": "asset-key",
            "required_capabilities": ["collectible"],
        },
        {
            "slot_id": "door",
            "semantic_role": "locked_door",
            "entity_id": "entity-door",
            "entity_version": 1,
            "asset_id": "asset-door",
            "required_capabilities": ["lockable"],
        },
        {
            "slot_id": "goal",
            "semantic_role": "goal_zone",
            "entity_id": "entity-goal",
            "entity_version": 1,
            "asset_id": "asset-goal",
            "required_capabilities": ["trigger_zone"],
        },
    )


def _binding_artifact(
    logic: ArtifactEnvelope,
    *,
    snapshot_source: str = "runtime",
    non_executable: bool = False,
    scene_version: int = 3,
) -> ArtifactEnvelope:
    return ArtifactEnvelope(
        artifact_id="program.entity-binding-plan",
        artifact_type="EntityBindingPlan",
        version=1,
        producer_role="program",
        source_task_id="task-program-binding",
        base_project_version=1,
        base_world_version=scene_version,
        dependencies=(logic.artifact_id,),
        snapshot_source=snapshot_source,
        non_executable=non_executable,
        status="validated",
        payload=EntityBindingPlan(
            snapshot_plan_id="plan-demo",
            snapshot_version=scene_version,
            bindings=_binding_rows(),
        ),
    )


def _snapshot(*, key_ready: bool = True) -> dict:
    actors = []
    for binding in _binding_rows():
        actors.append({
            "entity_id": binding["entity_id"],
            "entity_version": binding["entity_version"],
            "asset_id": binding["asset_id"],
            "semantic_role": binding["semantic_role"],
            "interaction_capability": list(binding["required_capabilities"]),
            "gameplay_tags": [],
            "game_ready": key_ready if binding["slot_id"] == "key" else True,
        })
    return {
        "room_id": "room-demo",
        "plan_id": "plan-demo",
        "scene_version": 3,
        "world_readiness": "game_ready",
        "world_fingerprint": "sha256:" + "1" * 64,
        "environment_entities": [],
        "actor_entities": actors,
    }


def _green_gate(*, scene_version: int = 3, overall: str = "green") -> dict:
    return {
        "gate_report_id": "gate-single-player-demo",
        "room_id": "room-demo",
        "plan_id": "plan-demo",
        "scene_version": scene_version,
        "overall": overall,
        "metrics": {"gate_profile": "single_player_demo"},
        "capability_unlocks": [
            "single_player_entity_binding",
            "single_player_local_action",
            "single_player_preview",
        ],
    }


class ProjectGateServiceTests(unittest.TestCase):
    def test_asset_lineage_requires_matching_real_image_hash(self) -> None:
        service = ProjectGateService()
        images = {
            "key": {
                "status": "ready",
                "mode": "text_to_image",
                "resource_ref": "image:key:1",
                "image_url": "fileid://key.png",
                "prompt_hash": "sha256:prompt",
                "content_hash": "sha256:image",
            }
        }
        models = {
            "key": {
                "status": "ready",
                "generation_mode": "image_to_3d",
                "source_image_ref": "image:key:1",
                "source_image_hash": "sha256:image",
                "model_ref": "model:key:1",
                "local_path": "models/key.glb",
            }
        }

        passed = service.validate_asset_lineage(
            required_items=("key",),
            image_resources=images,
            model_resources=models,
        )
        mismatched = service.validate_asset_lineage(
            required_items=("key",),
            image_resources=images,
            model_resources={"key": {**models["key"], "source_image_hash": "sha256:other"}},
        )

        self.assertEqual(passed.status, "passed")
        self.assertEqual(mismatched.status, "blocked")
        self.assertEqual(
            mismatched.blocked_results[0].error_code,
            "asset_image_to_model_lineage_invalid",
        )

    def test_valid_runtime_artifacts_build_deterministic_manifest_and_proposal(self) -> None:
        logic = _logic_artifact()
        binding = _binding_artifact(logic)
        gate = ProjectGateService()

        first = gate.validate_single_player_action(
            proposal_id="proposal-demo",
            command_id="command-demo",
            room_id="room-demo",
            project_id="project-demo",
            gameplay_logic_artifact=logic,
            entity_binding_artifact=binding,
            snapshot=_snapshot(),
            gate_report=_green_gate(),
        )
        second = gate.validate_single_player_action(
            proposal_id="proposal-demo",
            command_id="command-demo",
            room_id="room-demo",
            project_id="project-demo",
            gameplay_logic_artifact=logic,
            entity_binding_artifact=binding,
            snapshot=_snapshot(),
            gate_report=_green_gate(),
        )

        self.assertEqual(first.status, "passed")
        self.assertIsInstance(first.proposal, ActionProposal)
        self.assertEqual(first.proposal.operation, "gameplay.apply_manifest")
        self.assertEqual(first.proposal.execution_scope, "single_player_local")
        self.assertEqual(
            {item.kind for item in first.proposal.gameplay_manifest.primitives},
            {"on_enter", "on_collect", "set_state", "unlock", "complete_objective"},
        )
        self.assertEqual(first.proposal.as_dict(), second.proposal.as_dict())

    def test_mock_binding_cannot_cross_action_proposal_constructor(self) -> None:
        logic = _logic_artifact()
        binding = _binding_artifact(
            logic,
            snapshot_source="mock",
            non_executable=True,
        )
        result = ProjectGateService().validate_single_player_action(
            proposal_id="proposal-mock",
            command_id="command-mock",
            room_id="room-demo",
            project_id="project-demo",
            gameplay_logic_artifact=logic,
            entity_binding_artifact=binding,
            snapshot=_snapshot(),
            gate_report=_green_gate(),
        )

        self.assertEqual(result.status, "blocked")
        self.assertEqual(result.blocked_results[0].error_code, "artifact_validation_failed")
        with self.assertRaises(NonExecutableArtifactError):
            ActionProposal(
                proposal_id="proposal-mock",
                command_id="command-mock",
                room_id="room-demo",
                binding_artifact=binding,
                gameplay_manifest=self._manifest_for_direct_constructor(),
                gate_report=_green_gate(),
            )

    def test_red_or_stale_world_and_needs_review_entity_are_blocked(self) -> None:
        logic = _logic_artifact()
        binding = _binding_artifact(logic)
        cases = (
            (_snapshot(), _green_gate(overall="red"), "single_player_gate_not_green"),
            (_snapshot(), _green_gate(scene_version=4), "single_player_gate_not_green"),
            (_snapshot(key_ready=False), _green_gate(), "entity_binding_fact_mismatch"),
        )
        for snapshot, gate_report, expected in cases:
            with self.subTest(expected=expected):
                result = ProjectGateService().validate_single_player_action(
                    proposal_id="proposal-blocked",
                    command_id="command-blocked",
                    room_id="room-demo",
                    project_id="project-demo",
                    gameplay_logic_artifact=logic,
                    entity_binding_artifact=binding,
                    snapshot=snapshot,
                    gate_report=gate_report,
                )
                self.assertEqual(result.status, "blocked")
                self.assertEqual(result.blocked_results[0].error_code, expected)

    def test_unsupported_primitive_is_rejected_before_project_gate(self) -> None:
        artifact = _logic_artifact(primitive_kind="run_arbitrary_script")

        self.assertEqual(artifact.status, "invalid")
        self.assertFalse(artifact.validation_result.valid)
        self.assertIn("primitives[0].kind:unsupported", artifact.validation_result.errors)

    def test_runtime_transport_gap_is_a_structured_interface_change_request(self) -> None:
        contract_hash = build_skeleton_manifest().contract_hash()
        request = gameplay_plan_patch_interface_change_request(contract_hash)

        self.assertEqual(request.request_id, "request.b6.4-gameplay-plan-patch-payload")
        self.assertIn("PlanPatch", request.affected_interfaces)
        self.assertIn("B7.3", request.blocked_dependents)
        self.assertEqual(request.current_contract_hash, contract_hash)

    def test_gameplay_plan_patch_interface_change_is_recorded_as_accepted_v6(self) -> None:
        contract_hash = build_skeleton_manifest().contract_hash()
        decision = gameplay_plan_patch_interface_change_decision(contract_hash)

        self.assertEqual(decision.decision, "accepted")
        self.assertEqual(decision.new_contract_version, "r3-skeleton-week1-v6")
        self.assertEqual(decision.new_contract_hash, contract_hash)
        self.assertIn("B6.4-runtime-submit", decision.required_revalidation)

    def test_new_gate_modules_do_not_import_runtime_or_engine_write_implementations(self) -> None:
        for module in (action_proposal_module, project_gate_module):
            source = Path(module.__file__).read_text(encoding="utf-8")
            for forbidden in (
                "services.agent_runtime",
                "RuntimeGuard",
                "EngineWriteGate",
                "RuntimeCppBridge",
                "lanchat_agent_worker",
            ):
                self.assertNotIn(forbidden, source)

    @staticmethod
    def _manifest_for_direct_constructor():
        logic = _logic_artifact()
        binding = _binding_artifact(logic)
        result = ProjectGateService().validate_single_player_action(
            proposal_id="proposal-valid",
            command_id="command-valid",
            room_id="room-demo",
            project_id="project-demo",
            gameplay_logic_artifact=logic,
            entity_binding_artifact=binding,
            snapshot=_snapshot(),
            gate_report=_green_gate(),
        )
        return result.proposal.gameplay_manifest


if __name__ == "__main__":
    unittest.main()
