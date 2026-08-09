from __future__ import annotations

from copy import deepcopy
import unittest

from editor.plugins.AITool.services.agent_collaboration.action_proposal import (
    GameplayEntityBinding,
    GameplayManifest,
)
from editor.plugins.AITool.services.agent_collaboration.contracts import GameplayPrimitiveSpec
from editor.plugins.AITool.services.agent_runtime import (
    AgentRuntime,
    PlanPatch,
    PlanPatchValidator,
    RuntimeGuard,
    ScenePlan,
    ScenePlanStatus,
    StatePatch,
    make_engine_gameplay_manifest_provider,
)
from editor.plugins.AITool.services.agent_runtime.core import scene_world_fingerprint
from editor.plugins.AITool.services.gameplay_contracts import gameplay_command_idempotency_key
from editor.plugins.AITool.services.schema_versions import (
    ACTION_PROPOSAL_SCHEMA_VERSION,
    PLAN_PATCH_PAYLOAD_SCHEMA_VERSION,
)


def _manifest() -> GameplayManifest:
    bindings = (
        GameplayEntityBinding("player", "player_spawn", "entity-player", 1, "asset-player", ("player",)),
        GameplayEntityBinding("key", "collectible_key", "entity-key", 1, "asset-key", ("collectible",)),
        GameplayEntityBinding("door", "locked_door", "entity-door", 1, "asset-door", ("lockable",)),
        GameplayEntityBinding("goal", "goal_zone", "entity-goal", 1, "asset-goal", ("trigger_zone",)),
    )
    primitives = (
        GameplayPrimitiveSpec("collect", "on_collect", "key", "player", {"state_key": "key", "set_value": True}),
        GameplayPrimitiveSpec("state", "set_state", "player", "door", {"state_key": "key", "value": True}),
        GameplayPrimitiveSpec("unlock", "unlock", "player", "door", {"required_state": "key"}),
        GameplayPrimitiveSpec("enter", "on_enter", "goal", "player", {}),
        GameplayPrimitiveSpec("finish", "complete_objective", "goal", "player", {"objective_id": "finish"}),
    )
    return GameplayManifest(
        project_id="project-demo",
        plan_id="plan-demo",
        scene_version=3,
        entity_bindings=bindings,
        primitives=primitives,
        objective_id="finish",
    )


def _proposal(*, command_id: str = "command-demo", manifest: GameplayManifest | None = None) -> dict:
    gameplay_manifest = manifest or _manifest()
    return {
        "schema_version": ACTION_PROPOSAL_SCHEMA_VERSION,
        "proposal_id": "proposal-demo",
        "command_id": command_id,
        "project_id": gameplay_manifest.project_id,
        "room_id": "room-demo",
        "plan_id": gameplay_manifest.plan_id,
        "scene_version": gameplay_manifest.scene_version,
        "execution_scope": "single_player_local",
        "operation": "gameplay.apply_manifest",
        "gate_report_id": "gate-demo",
        "gate_profile": "single_player_demo",
        "binding_artifact_id": "binding-demo",
        "binding_artifact_hash": "sha256:" + "1" * 64,
        "gameplay_manifest": gameplay_manifest.as_dict(),
        "idempotency_key": gameplay_command_idempotency_key(command_id, gameplay_manifest.content_hash),
        "risk_level": "low",
        "status": "validated",
    }


def _seed_runtime(runtime: AgentRuntime) -> None:
    scene_plan = ScenePlan(
        plan_id="plan-demo",
        room_id="room-demo",
        title="Demo",
        design_brief="Single-player demo",
        status=ScenePlanStatus.COMPLETED,
        version=3,
    )
    snapshot = {
        "room_id": "room-demo",
        "plan_id": "plan-demo",
        "scene_version": 3,
        "world_readiness": "game_ready",
        "snapshot_authority": "local_runtime",
        "environment_entities": [],
        "actor_entities": [],
        "readiness_summary": {"game_ready": 0},
        "operation_cursor": "cursor-demo",
    }
    snapshot["world_fingerprint"] = scene_world_fingerprint([], plan_id="plan-demo", scene_version=3)
    applied, reason = runtime.state.apply_patch(StatePatch(
        room_id="room-demo",
        changes={
            "scene_plans": {"plan-demo": scene_plan.as_dict()},
            "scene_world_snapshots": {"plan-demo@v3": snapshot},
        },
        expected_version=runtime.state.version,
        source_tool_call_id="test-seed",
    ))
    if not applied:
        raise AssertionError(reason)


class _GuardSpy(RuntimeGuard):
    def __init__(self) -> None:
        self.calls = []

    def authorize(self, call, definition=None):
        self.calls.append((call.tool_name, call.requires_write, call.confirmed))
        return super().authorize(call, definition)


class _EngineGateSpy:
    def __init__(self) -> None:
        self.calls = []

    def invoke_tool(self, tool, payload):
        self.calls.append((tool, deepcopy(payload)))
        return {"success": True, "status": "success", "receipt_id": "receipt-demo"}


class GameplayPlanPatchTests(unittest.TestCase):
    def test_legacy_plan_patch_remains_readable(self) -> None:
        patch = PlanPatch(
            patch_id="patch-legacy",
            room_id="room-demo",
            plan_id="plan-demo",
            text="Add a chair",
            items=["chair"],
        )

        PlanPatchValidator.validate(patch)
        restored = AgentRuntime._plan_patch_from_state_row(patch.patch_id, patch.as_dict())

        self.assertEqual(restored.patch_type, "intervention_add")
        self.assertEqual(restored.structured_payload, {})
        self.assertEqual(restored.payload_hash, "")

    def test_structured_gameplay_patch_rejects_hash_tamper_and_unknown_primitive(self) -> None:
        manifest = _manifest().as_dict()
        patch = PlanPatch(
            patch_id="patch-gameplay",
            room_id="room-demo",
            plan_id="plan-demo",
            text="Apply gameplay manifest",
            patch_type="gameplay_manifest_apply",
            payload_schema_version=PLAN_PATCH_PAYLOAD_SCHEMA_VERSION,
            structured_payload=manifest,
            payload_hash=manifest["content_hash"],
            proposal_id="proposal-demo",
        )
        PlanPatchValidator.validate(patch)

        tampered = deepcopy(patch.as_dict())
        tampered["structured_payload"]["project_id"] = "project-tampered"
        with self.assertRaisesRegex(ValueError, "content_hash mismatch"):
            PlanPatchValidator.validate(tampered)

        unknown = deepcopy(patch.as_dict())
        unknown["structured_payload"]["primitives"][0]["kind"] = "run_script"
        with self.assertRaisesRegex(ValueError, "unsupported gameplay primitive"):
            PlanPatchValidator.validate(unknown)

    def test_submission_uses_runtime_guard_engine_gate_and_is_idempotent(self) -> None:
        guard = _GuardSpy()
        engine_gate = _EngineGateSpy()
        engine_tool = object()
        provider = make_engine_gameplay_manifest_provider(
            engine_gate=engine_gate,
            gameplay_apply_tool=engine_tool,
            capability_manifest_reader=lambda: {
                "supported_operations": ["gameplay.apply_manifest"],
                "supported_gameplay_primitives": [
                    "on_enter",
                    "on_collect",
                    "set_state",
                    "unlock",
                    "complete_objective",
                ],
            },
        )
        runtime = AgentRuntime(guard=guard, gameplay_manifest_provider=provider)
        _seed_runtime(runtime)

        first = runtime.submit_gameplay_action_proposal(_proposal())
        graph_count = sum(
            1
            for graph in runtime.state.room("room-demo").get("tool_graphs", {}).values()
            if graph.get("graph_role") == "business_action"
        )
        second = runtime.submit_gameplay_action_proposal(_proposal())
        replay_graph_count = sum(
            1
            for graph in runtime.state.room("room-demo").get("tool_graphs", {}).values()
            if graph.get("graph_role") == "business_action"
        )

        self.assertEqual(first["status"], "accepted")
        self.assertFalse(first["replayed"])
        self.assertTrue(second["replayed"])
        self.assertEqual(second["patch_id"], first["patch_id"])
        self.assertEqual(graph_count, 1)
        self.assertEqual(replay_graph_count, 1)
        self.assertEqual(len(engine_gate.calls), 1)
        self.assertIs(engine_gate.calls[0][0], engine_tool)
        self.assertEqual(engine_gate.calls[0][1]["patch_type"], "gameplay_manifest_apply")
        self.assertIn(("gameplay.apply_manifest", True, True), guard.calls)
        self.assertIn(
            first["payload_hash"],
            {
                row.get("payload_hash")
                for row in runtime.state.room("room-demo").get("accepted_interventions", {}).values()
            },
        )

    def test_missing_engine_capability_fails_closed_without_registering_or_writing(self) -> None:
        runtime = AgentRuntime()
        _seed_runtime(runtime)

        first = runtime.submit_gameplay_action_proposal(_proposal(command_id="command-no-engine"))
        second = runtime.submit_gameplay_action_proposal(_proposal(command_id="command-no-engine"))

        self.assertEqual(first["status"], "blocked")
        self.assertEqual(first["error_code"], "engine_gameplay_manifest_unavailable")
        self.assertFalse(second["replayed"])
        self.assertFalse(runtime.registry.has("gameplay.apply_manifest"))
        self.assertEqual(
            sum(
                1
                for graph in runtime.state.room("room-demo").get("tool_graphs", {}).values()
                if graph.get("graph_role") == "business_action"
            ),
            0,
        )
        self.assertFalse(runtime.state.room("room-demo").get("pending_interventions"))
        self.assertFalse(runtime.state.room("room-demo").get("custom_gameplay_facts"))

    def test_adapter_rejects_unadvertised_operation_before_engine_gate(self) -> None:
        engine_gate = _EngineGateSpy()
        manifest = _manifest().as_dict()
        provider = make_engine_gameplay_manifest_provider(
            engine_gate=engine_gate,
            gameplay_apply_tool=object(),
            capability_manifest_reader=lambda: {
                "supported_operations": [],
                "supported_gameplay_primitives": [
                    "on_enter",
                    "on_collect",
                    "set_state",
                    "unlock",
                    "complete_objective",
                ],
            },
        )

        result = provider({
            "room_id": "room-demo",
            "plan_id": "plan-demo",
            "proposal_id": "proposal-demo",
            "patch_id": "patch-demo",
            "manifest_schema_version": PLAN_PATCH_PAYLOAD_SCHEMA_VERSION,
            "manifest": manifest,
            "manifest_hash": manifest["content_hash"],
            "idempotency_key": gameplay_command_idempotency_key("command-demo", manifest["content_hash"]),
        })

        self.assertFalse(result["success"])
        self.assertEqual(result["error_code"], "engine_gameplay_operation_unsupported")
        self.assertEqual(engine_gate.calls, [])


if __name__ == "__main__":
    unittest.main()
