from __future__ import annotations

from copy import deepcopy
import unittest
from unittest.mock import patch

from editor.plugins.AITool.services.agent_runtime import (
    AgentRuntime,
    BatchPlan,
    BatchPlanStatus,
    ScenePlan,
    ScenePlanStatus,
    StatePatch,
    ToolCall,
    ToolCallGraph,
    ToolCallGraphValidator,
    make_scene_snapshot_provider,
)
from editor.plugins.AITool.services.agent_runtime.adapters import (
    make_engine_environment_component_import_provider,
)
from editor.plugins.AITool.services.agent_runtime.scene_world_consistency import (
    audit_scene_world_consistency,
    constrain_scene_world_snapshot_readiness,
    latest_engine_snapshot,
    scene_world_fingerprint,
)
from editor.plugins.AITool.services.agent_runtime.core import SceneWorldSnapshotRecordValidator
from editor.plugins.AITool.services.lanchat_agent_worker import LANChatAgentWorker
from editor.plugins.AITool.services.runtime_action_intent import RuntimeActionIntent


class _DispatchTrackingWorker(LANChatAgentWorker):
    def __init__(self, *, agent_runtime: AgentRuntime) -> None:
        super().__init__(agent_runtime=agent_runtime)
        self.authoritative_replies: list[str] = []

    def _send_coordinator_sync_system_reply(self, _message: dict, text: str) -> bool:
        self.authoritative_replies.append(str(text))
        return True

    def _send_final_reply(self, _agent_id: str, _agent_name: str, text: str, *_args, **_kwargs) -> bool:
        self.authoritative_replies.append(str(text))
        return True


class _ClientDispatchTrackingWorker(_DispatchTrackingWorker):
    def _can_execute_generation_locally(self) -> bool:
        return False


class _RuntimeIdentitySnapshotTool:
    def invoke(self, _payload: dict) -> dict:
        return {
            "status": "success",
            "actors": [
                {
                    "actor_guid": "actor-runtime-1",
                    "name": "desk",
                    "entity_id": "entity-runtime-1",
                    "asset_id": "asset-desk",
                    "model_ref": "asset-desk",
                    "entity_type": "furniture",
                    "semantic_role": "desk",
                    "source_plan_id": "plan-runtime-1",
                    "source_batch_id": "batch-runtime-1",
                    "actor_version": 4,
                    "bounds_ready": True,
                    "render_status_observed": True,
                    "render_ready": True,
                    "render_failed": False,
                    "gpu_build_state": "Ready",
                    "mesh_count": 1,
                    "renderable_mesh_count": 1,
                    "invalid_mesh_count": 0,
                    "world_aabb": [-1.0, 0.0, -0.5, 1.0, 1.2, 0.5],
                    "geometry": {
                        "position": [0.0, 0.0, 0.0],
                        "rotation": [0.0, 0.0, 0.0],
                        "scale": [1.0, 1.0, 1.0],
                    },
                }
            ],
        }


def _room_fact(*, game_ready: bool) -> dict:
    actor = {
        "plan_id": "plan-1",
        "batch_id": "batch-1",
        "name": "丘比特雕像",
        "requested_name": "丘比特雕像",
        "asset_id": "asset-cupid",
        "model_ref": "cupid.obj",
        "position": [0.0, 0.0, 0.0],
        "rotation": [0.0, 0.0, 0.0],
        "scale": [1.0, 1.0, 1.0],
        "aabb": {"min": [-0.5, 0.0, -0.5], "max": [0.5, 1.8, 0.5]},
        "bounds_source": "engine_actual" if game_ready else "estimated",
        "bounds_ready": True,
        "render_status_observed": True,
        "render_ready": True,
        "render_failed": False,
        "gpu_build_state": "Ready",
        "mesh_count": 1,
        "renderable_mesh_count": 1,
        "invalid_mesh_count": 0,
        "engine_lifecycle_status": "bounds_ready",
        "sync_status": "engine_created",
        "grounding_status": "grounded",
        "source": "engine_actor_import",
        "status": "success",
    }
    return {
        "active_execution_plan_id": "",
        "latest_completed_plan_id": "plan-1",
        "scene_plans": {
            "plan-1": {
                "plan_id": "plan-1",
                "room_id": "room-1",
                "title": "test",
                "status": "completed",
                "version": 2,
                "concrete_object_items": ["丘比特雕像"],
            }
        },
        "batch_plans": {
            "batch-1": {
                "batch_id": "batch-1",
                "plan_id": "plan-1",
                "room_id": "room-1",
                "status": "completed",
                "tool_graph_id": "graph-business",
            }
        },
        "actors": {"actor-cupid": actor},
        "observed_actors": {"actor-cupid": actor},
    }


class AgentRuntimeGameReadyTests(unittest.TestCase):
    def test_engine_snapshot_preserves_runtime_identity_and_actual_bounds(self) -> None:
        provider = make_scene_snapshot_provider(
            snapshot_tool=_RuntimeIdentitySnapshotTool(),
            scene_name="Scene/runtime-identity.scene",
        )

        snapshot = provider({"room_id": "room-runtime-identity"})
        self.assertEqual(snapshot["actor_count"], 1)
        actor = snapshot["actors"][0]
        self.assertEqual(actor["actor_id"], "actor-runtime-1")
        self.assertEqual(actor["entity_id"], "entity-runtime-1")
        self.assertEqual(actor["asset_id"], "asset-desk")
        self.assertEqual(actor["model_ref"], "asset-desk")
        self.assertEqual(actor["plan_id"], "plan-runtime-1")
        self.assertEqual(actor["batch_id"], "batch-runtime-1")
        self.assertEqual(actor["actor_version"], 4)
        self.assertEqual(actor["entity_version"], 4)
        self.assertEqual(actor["version"], 4)
        self.assertEqual(actor["bounds_source"], "engine_actual")
        self.assertEqual(actor["engine_lifecycle_status"], "bounds_ready")
        self.assertEqual(actor["sync_status"], "engine_imported")
        self.assertTrue(actor["render_status_observed"])
        self.assertTrue(actor["render_ready"])
        self.assertEqual(actor["gpu_build_state"], "Ready")
        self.assertEqual(actor["renderable_mesh_count"], 1)
        self.assertEqual(actor["invalid_mesh_count"], 0)

    def test_runtime_scene_snapshot_projection_preserves_render_readiness(self) -> None:
        runtime = AgentRuntime(
            scene_snapshot_provider=make_scene_snapshot_provider(
                snapshot_tool=_RuntimeIdentitySnapshotTool(),
                scene_name="Scene/runtime-render.scene",
            )
        )

        result = runtime.refresh_scene_snapshot("room-runtime-render")
        actor = runtime.query_state("room-runtime-render")["room"]["observed_actors"]["actor-runtime-1"]

        self.assertEqual(result["graph"]["status"], "completed")
        self.assertTrue(actor["render_status_observed"])
        self.assertTrue(actor["render_ready"])
        self.assertFalse(actor["render_failed"])
        self.assertEqual(actor["gpu_build_state"], "Ready")
        self.assertEqual(actor["mesh_count"], 1)
        self.assertEqual(actor["renderable_mesh_count"], 1)
        self.assertEqual(actor["invalid_mesh_count"], 0)

    def test_invalid_render_mesh_cannot_be_game_ready_even_with_actual_bounds(self) -> None:
        room = _room_fact(game_ready=True)
        actor = room["actors"]["actor-cupid"]
        actor["render_ready"] = False
        actor["renderable_mesh_count"] = 0
        actor["invalid_mesh_count"] = 1
        room["observed_actors"]["actor-cupid"] = deepcopy(actor)

        registry = AgentRuntime._scene_entity_registry_for_plan(room, "plan-1")
        entity = next(row for row in registry["entities"] if row.get("actor_id") == "actor-cupid")

        self.assertFalse(entity["game_ready"])
        self.assertIn("render_not_ready", entity["readiness_missing_fields"])
        self.assertNotEqual(entity["engine_write_verification_status"], "engine_verified")

    def test_engine_observation_overrides_stale_estimated_geometry_and_render_facts(self) -> None:
        room = _room_fact(game_ready=False)
        runtime_actor = room["actors"]["actor-cupid"]
        runtime_actor.update({
            "position": [9.0, 4.0, 9.0],
            "aabb": {"min": [8.5, 4.0, 8.5], "max": [9.5, 5.8, 9.5]},
            "bounds_source": "estimated",
            "render_status_observed": False,
            "render_ready": False,
            "engine_lifecycle_status": "engine_loading",
            "grounding_status": "needs_review",
            "support_type": "floor_supported",
        })
        observed_actor = deepcopy(runtime_actor)
        observed_actor.update({
            "position": [1.0, 0.0, 2.0],
            "aabb": {"min": [0.5, 0.0, 1.5], "max": [1.5, 1.8, 2.5]},
            "bounds_source": "engine_actual",
            "bounds_ready": True,
            "render_status_observed": True,
            "render_ready": True,
            "render_failed": False,
            "gpu_build_state": "Ready",
            "renderable_mesh_count": 1,
            "invalid_mesh_count": 0,
            "engine_lifecycle_status": "bounds_ready",
            "actor_version": 4,
        })
        room["observed_actors"]["actor-cupid"] = observed_actor

        registry = AgentRuntime._scene_entity_registry_for_plan(room, "plan-1")
        entity = next(
            row for row in registry["entities"] if row.get("actor_id") == "actor-cupid"
        )

        self.assertEqual(entity["transform"]["position"], [1.0, 0.0, 2.0])
        self.assertEqual(entity["world_aabb"]["min"], [0.5, 0.0, 1.5])
        self.assertEqual(entity["bounds_source"], "engine_actual")
        self.assertTrue(entity["render_ready"])
        self.assertEqual(entity["version"], 4)
        self.assertEqual(entity["grounding_status"], "grounded")
        self.assertTrue(entity["game_ready"])
        self.assertEqual(entity["readiness_missing_fields"], [])

    def test_non_authoritative_client_does_not_execute_completed_scene_write(self) -> None:
        runtime = AgentRuntime()
        applied, _ = runtime.state.apply_patch(StatePatch(room_id="room-1", changes=_room_fact(game_ready=True)))
        self.assertTrue(applied)
        worker = _ClientDispatchTrackingWorker(agent_runtime=runtime)
        intent = RuntimeActionIntent(
            message_id="msg-client-write",
            room_id="room-1",
            route="runtime_write",
            operation="add",
            modality="command",
            confidence=0.99,
            target_plan_id="plan-1",
        )
        message = {
            "room_id": "room-1",
            "message_id": "msg-client-write",
            "text": "add a table",
            "sender_id": "member-1",
            "sender_name": "member",
            "sender_type": "user",
            "message_kind": "chat",
        }

        with patch.object(worker, "_runtime_action_intent_for_trigger", return_value=intent):
            handled = worker.sync_chat_message_to_coordinator(message, source="lanchat_native_queue")

        self.assertTrue(handled)
        self.assertEqual(runtime.state.room("room-1").get("pending_interventions", {}), {})
        self.assertEqual(worker.authoritative_replies, [])

    def test_native_and_agent_trigger_share_one_authoritative_query_reply(self) -> None:
        runtime = AgentRuntime()
        applied, _ = runtime.state.apply_patch(StatePatch(room_id="room-1", changes=_room_fact(game_ready=True)))
        self.assertTrue(applied)
        worker = _DispatchTrackingWorker(agent_runtime=runtime)
        message = {
            "room_id": "room-1",
            "message_id": "msg-shared-query",
            "text": "@GM 丘比特雕像已经加入了吗",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "is_host": True,
        }

        self.assertTrue(worker.sync_chat_message_to_coordinator(dict(message), source="lanchat_native_queue"))
        self.assertTrue(worker._process_trigger(dict(message)))

        self.assertEqual(len(worker.authoritative_replies), 1)
        ledger = worker._message_dispatch_ledger.entry("room-1", "msg-shared-query")
        self.assertEqual(ledger.get("owner"), "native_queue")
        self.assertEqual(ledger.get("state"), "replied")

    def test_structured_gm_r3_query_is_claimed_once_across_both_ingress_paths(self) -> None:
        worker = _DispatchTrackingWorker(agent_runtime=AgentRuntime())
        message = {
            "room_id": "room-r3-query",
            "message_id": "msg-r3-query",
            "text": "@GM R3门禁",
            "sender_id": "host-1",
            "sender_name": "房主",
            "sender_type": "host",
            "message_kind": "chat",
            "agent_id": "gm",
            "agent_name": "GM",
            "target_agent_id": "gm",
            "target_agent_name": "GM",
            "is_host": True,
            "metadata": {
                "draft_action": "chat",
                "target_scope": "gm",
                "target_agent_id": "gm",
                "target_agent_name": "GM",
            },
        }

        with patch.object(worker, "_agent_runtime_r3_gate_reply", return_value="R3 gate result"):
            self.assertTrue(
                worker.sync_chat_message_to_coordinator(
                    dict(message),
                    source="lanchat_native_queue",
                )
            )
            self.assertTrue(worker._process_trigger(dict(message)))

        self.assertEqual(worker.authoritative_replies, ["R3 gate result"])
        ledger = worker._message_dispatch_ledger.entry("room-r3-query", "msg-r3-query")
        self.assertEqual(ledger.get("owner"), "native_queue")
        self.assertEqual(ledger.get("route"), "gm_control")
        self.assertEqual(ledger.get("state"), "replied")

    def test_worker_entity_question_is_read_only_end_to_end(self) -> None:
        runtime = AgentRuntime()
        applied, _ = runtime.state.apply_patch(StatePatch(room_id="room-1", changes=_room_fact(game_ready=True)))
        self.assertTrue(applied)
        worker = LANChatAgentWorker(agent_runtime=runtime)
        before = runtime.state.room("room-1")

        reply = worker._handle_runtime_entity_status_query({
            "room_id": "room-1",
            "message_id": "msg-query",
            "text": "@GM 丘比特雕像已经加入了吗",
        })

        after = runtime.state.room("room-1")
        self.assertIn("丘比特雕像", reply or "")
        self.assertEqual(before.get("tool_graphs", {}), after.get("tool_graphs", {}))
        self.assertEqual(before.get("pending_interventions", {}), after.get("pending_interventions", {}))

    def test_worker_entity_question_reads_latest_failed_terminal_plan(self) -> None:
        runtime = AgentRuntime()
        room_fact = _room_fact(game_ready=False)
        room_fact["scene_plans"]["plan-1"]["status"] = "failed"
        applied, _ = runtime.state.apply_patch(StatePatch(room_id="room-1", changes=room_fact))
        self.assertTrue(applied)
        worker = LANChatAgentWorker(agent_runtime=runtime)
        before = runtime.state.room("room-1")

        reply = worker._handle_runtime_entity_status_query({
            "room_id": "room-1",
            "message_id": "msg-query-failed-plan",
            "text": "@GM \u4e18\u6bd4\u7279\u96d5\u50cf\u5df2\u7ecf\u52a0\u5165\u4e86\u5417",
        })

        after = runtime.state.room("room-1")
        self.assertIn("\u4e18\u6bd4\u7279\u96d5\u50cf", reply or "")
        self.assertEqual(before.get("tool_graphs", {}), after.get("tool_graphs", {}))
        self.assertEqual(before.get("pending_interventions", {}), after.get("pending_interventions", {}))

    def test_worker_typo_add_returns_clarification_without_patch(self) -> None:
        runtime = AgentRuntime()
        applied, _ = runtime.state.apply_patch(StatePatch(room_id="room-1", changes=_room_fact(game_ready=True)))
        self.assertTrue(applied)
        worker = LANChatAgentWorker(agent_runtime=runtime)

        reply = worker._handle_runtime_completed_increment({
            "room_id": "room-1",
            "message_id": "msg-typo",
            "text": "再加入一个切比特雕像",
        })

        self.assertIn("丘比特雕像", reply or "")
        self.assertEqual(runtime.state.room("room-1").get("pending_interventions", {}), {})

    def test_entity_status_query_does_not_create_graph_or_patch(self) -> None:
        runtime = AgentRuntime()
        applied, _ = runtime.state.apply_patch(StatePatch(room_id="room-1", changes=_room_fact(game_ready=True)))
        self.assertTrue(applied)
        before = runtime.state.room("room-1")

        result = runtime.handle_message(
            room_id="room-1",
            plan_id="plan-1",
            text="",
            action="runtime.entity_status",
            sync_event={"entity_names": ["丘比特雕像"]},
        )

        after = runtime.state.room("room-1")
        self.assertFalse(result["recorded"])
        self.assertEqual(len(result["entity_status"]["丘比特雕像"]), 1)
        self.assertEqual(before.get("tool_graphs", {}), after.get("tool_graphs", {}))
        self.assertEqual(before.get("pending_interventions", {}), after.get("pending_interventions", {}))

    def test_snapshot_distinguishes_partial_world_from_pipeline_completion(self) -> None:
        room = _room_fact(game_ready=False)
        registry = AgentRuntime._scene_entity_registry_for_plan(room, "plan-1")
        snapshot = AgentRuntime._scene_world_snapshot_for_plan(
            room,
            "plan-1",
            room_id="room-1",
            scene_entity_registry=registry,
            operation_cursor="op:10",
        )

        self.assertEqual(snapshot["scene_version"], 2)
        self.assertEqual(snapshot["world_readiness"], "needs_review")
        self.assertEqual(len(snapshot["world_fingerprint"]), 64)
        self.assertIn("engine_actual_aabb", snapshot["actor_entities"][0]["readiness_missing_fields"])

    def test_partial_sync_status_blocks_game_ready_snapshot(self) -> None:
        room = _room_fact(game_ready=True)
        room["actors"]["actor-cupid"]["sync_status"] = "partial"
        room["observed_actors"]["actor-cupid"]["sync_status"] = "partial"

        registry = AgentRuntime._scene_entity_registry_for_plan(room, "plan-1")
        snapshot = AgentRuntime._scene_world_snapshot_for_plan(
            room,
            "plan-1",
            room_id="room-1",
            scene_entity_registry=registry,
            operation_cursor="op:11",
        )

        self.assertEqual(registry["game_ready_entity_count"], 0)
        self.assertEqual(snapshot["world_readiness"], "needs_review")
        self.assertEqual(snapshot["actor_entities"][0]["sync_status"], "partial")
        self.assertIn(
            "sync_status_ready",
            snapshot["actor_entities"][0]["readiness_missing_fields"],
        )

    def test_registry_entities_carry_stable_versions_and_source_identity(self) -> None:
        room = _room_fact(game_ready=True)
        actor = room["actors"]["actor-cupid"]
        actor["actor_request_id"] = "actor-request-cupid"
        actor["actor_version"] = 7
        room["element_routes"] = {
            "batch-1": [
                {
                    "name": "grass",
                    "target_pipeline": "environment",
                }
            ]
        }

        first = AgentRuntime._scene_entity_registry_for_plan(room, "plan-1")
        first_actor = next(entity for entity in first["entities"] if entity.get("actor_id") == "actor-cupid")
        substrate = next(entity for entity in first["entities"] if entity.get("entity_type") == "substrate")
        first_entity_id = first_actor["entity_id"]

        room["actors"]["actor-cupid-reloaded"] = room["actors"].pop("actor-cupid")
        second = AgentRuntime._scene_entity_registry_for_plan(room, "plan-1")
        second_actor = next(
            entity for entity in second["entities"] if entity.get("actor_id") == "actor-cupid-reloaded"
        )

        self.assertEqual(first_actor["version"], 7)
        self.assertEqual(first_actor["version_source"], "engine_actual")
        self.assertEqual(first_actor["entity_id_source"], "request_identity")
        self.assertEqual(first_entity_id, second_actor["entity_id"])
        self.assertEqual(first_actor["source_plan_id"], "plan-1")
        self.assertEqual(first_actor["source_batch_id"], "batch-1")
        self.assertEqual(substrate["version"], 2)
        self.assertEqual(substrate["version_source"], "scene_version")

    def test_public_snapshot_api_is_read_only_and_uses_latest_completed_plan(self) -> None:
        runtime = AgentRuntime()
        applied, _ = runtime.state.apply_patch(StatePatch(room_id="room-1", changes=_room_fact(game_ready=True)))
        self.assertTrue(applied)
        before = runtime.state.room("room-1")
        operation_count_before = len(runtime.operation_log.entries())

        result = runtime.handle_message(
            room_id="room-1",
            text="",
            action="runtime.scene_world_snapshot.get",
        )
        repeated = runtime.handle_message(
            room_id="room-1",
            text="",
            action="runtime.scene_world_snapshot.get",
        )

        after = runtime.state.room("room-1")
        self.assertTrue(result["found"])
        self.assertEqual(result["plan_id"], "plan-1")
        self.assertEqual(result["scene_version"], 2)
        self.assertEqual(result["snapshot_stability"], "provisional")
        self.assertEqual(result["world_readiness"], "needs_review")
        self.assertEqual(
            result["snapshot"]["readiness_summary"]["consistency_status"],
            "blocked",
        )
        self.assertEqual(before.get("tool_graphs", {}), after.get("tool_graphs", {}))
        self.assertEqual(before.get("pending_interventions", {}), after.get("pending_interventions", {}))
        self.assertEqual(len(runtime.operation_log.entries()), operation_count_before)
        self.assertEqual(repeated["operation_cursor"], result["operation_cursor"])
        self.assertEqual(repeated["world_fingerprint"], result["world_fingerprint"])

    def test_public_snapshot_api_rejects_unavailable_minimum_version(self) -> None:
        runtime = AgentRuntime()
        applied, _ = runtime.state.apply_patch(StatePatch(room_id="room-1", changes=_room_fact(game_ready=True)))
        self.assertTrue(applied)

        result = runtime.handle_message(
            room_id="room-1",
            text="",
            action="runtime.scene_world_snapshot.get",
            sync_event={"min_version": 3},
        )

        self.assertFalse(result["found"])
        self.assertEqual(result["scene_version"], 2)
        self.assertEqual(result["reason"], "minimum_scene_version_not_available")

    def test_legacy_report_snapshot_preserves_persisted_consistency_downgrade(self) -> None:
        runtime = AgentRuntime()
        applied, _ = runtime.state.apply_patch(StatePatch(room_id="room-1", changes=_room_fact(game_ready=True)))
        self.assertTrue(applied)
        room = runtime.state.room("room-1")
        registry = runtime._scene_entity_registry_for_plan(room, "plan-1")
        persisted_snapshot = runtime._scene_world_snapshot_for_plan(
            room,
            "plan-1",
            room_id="room-1",
            scene_entity_registry=registry,
            operation_cursor="op:1",
        )
        persisted_report = {
            "scene_world_snapshot": persisted_snapshot,
            "scene_world_consistency_audit": {
                "status": "needs_review",
                "issue_count": 1,
                "fingerprints_match": False,
            },
        }

        with patch.object(runtime, "_latest_persisted_report_for_plan", return_value=persisted_report):
            result = runtime.get_scene_world_snapshot(room_id="room-1")

        self.assertEqual(result["snapshot_stability"], "legacy_report")
        self.assertEqual(result["world_readiness"], "needs_review")
        self.assertEqual(
            result["snapshot"]["readiness_summary"]["consistency_status"],
            "needs_review",
        )

    def test_world_fingerprint_is_order_independent_and_covers_agent_contract(self) -> None:
        first = {
            "entity_id": "entity-1",
            "actor_id": "actor-1",
            "asset_id": "asset-1",
            "model_ref": "desk.obj",
            "entity_type": "furniture",
            "semantic_role": "desk",
            "version": 2,
            "transform": {"position": [0, 0, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1]},
            "world_aabb": {"min": [-1, 0, -1], "max": [1, 1, 1]},
            "bounds_source": "engine_actual",
            "grounding_status": "grounded",
            "interaction_capability": ["inspect", "use"],
            "gameplay_tags": ["workspace", "furniture"],
            "script_bindings": [
                {"event": "inspect", "handler": "show_details"},
                {"event": "use", "handler": "open_desk"},
            ],
            "sync_status": "synced",
            "game_ready": True,
            "readiness_missing_fields": [],
        }
        second = {
            "entity_id": "entity-2",
            "actor_id": "actor-2",
            "asset_id": "asset-2",
            "model_ref": "chair.obj",
            "entity_type": "furniture",
            "semantic_role": "chair",
            "version": 1,
            "transform": {"position": [2, 0, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1]},
            "world_aabb": {"min": [1.5, 0, -0.5], "max": [2.5, 1, 0.5]},
            "bounds_source": "engine_actual",
            "grounding_status": "grounded",
            "sync_status": "synced",
            "game_ready": True,
            "readiness_missing_fields": [],
        }

        baseline = scene_world_fingerprint(
            [first, second],
            plan_id="plan-1",
            scene_version=2,
        )
        reordered = scene_world_fingerprint(
            [
                second,
                {
                    **first,
                    "interaction_capability": ["use", "inspect"],
                    "script_bindings": list(reversed(first["script_bindings"])),
                },
            ],
            plan_id="plan-1",
            scene_version=2,
        )
        changed = deepcopy(first)
        changed["grounding_status"] = "needs_review"

        self.assertEqual(baseline, reordered)
        self.assertNotEqual(
            baseline,
            scene_world_fingerprint([changed, second], plan_id="plan-1", scene_version=2),
        )

    def test_terminal_snapshot_is_frozen_and_returned_by_deep_copy(self) -> None:
        runtime = AgentRuntime()
        applied, _ = runtime.state.apply_patch(
            StatePatch(room_id="room-1", changes=_room_fact(game_ready=True))
        )
        self.assertTrue(applied)

        first_report = runtime.generate_report("room-1", plan_id="plan-1")
        first_result = runtime.get_scene_world_snapshot(room_id="room-1")
        report_count = len(runtime.state.room("room-1").get("reports") or [])
        snapshot_key = "plan-1@v2"
        frozen_before = deepcopy(
            runtime.state.room("room-1")["scene_world_snapshots"][snapshot_key]
        )

        first_result["snapshot"]["actor_entities"][0]["semantic_role"] = "tampered"
        second_report = runtime.generate_report("room-1", plan_id="plan-1")
        second_result = runtime.get_scene_world_snapshot(room_id="room-1")

        self.assertEqual(first_result["snapshot_stability"], "immutable")
        self.assertEqual(second_result["snapshot_stability"], "immutable")
        self.assertEqual(second_result["snapshot"], frozen_before)
        self.assertEqual(len(runtime.state.room("room-1").get("reports") or []), report_count)
        self.assertEqual(
            first_report["scene_world_snapshot"]["world_fingerprint"],
            second_report["scene_world_snapshot"]["world_fingerprint"],
        )

    def test_snapshot_rejects_entity_without_stable_identity(self) -> None:
        runtime = AgentRuntime()
        applied, _ = runtime.state.apply_patch(
            StatePatch(room_id="room-1", changes=_room_fact(game_ready=True))
        )
        self.assertTrue(applied)
        snapshot = runtime._scene_world_snapshot_for_plan(
            runtime.state.room("room-1"),
            "plan-1",
            room_id="room-1",
            scene_entity_registry=runtime._scene_entity_registry_for_plan(
                runtime.state.room("room-1"), "plan-1"
            ),
            operation_cursor="op:1",
        )
        snapshot["actor_entities"][0]["entity_id"] = ""
        snapshot["world_fingerprint"] = scene_world_fingerprint(
            [*snapshot["environment_entities"], *snapshot["actor_entities"]],
            plan_id="plan-1",
            scene_version=snapshot["scene_version"],
        )

        with self.assertRaisesRegex(ValueError, "stable entity_id"):
            SceneWorldSnapshotRecordValidator.validate(snapshot)

    def test_same_version_terminal_snapshot_conflict_is_rejected(self) -> None:
        runtime = AgentRuntime()
        applied, _ = runtime.state.apply_patch(
            StatePatch(room_id="room-1", changes=_room_fact(game_ready=True))
        )
        self.assertTrue(applied)
        original_report = runtime.generate_report("room-1", plan_id="plan-1")
        frozen_before = deepcopy(runtime.state.room("room-1")["scene_world_snapshots"])
        report_count = len(runtime.state.room("room-1").get("reports") or [])

        conflicting_report = deepcopy(original_report)
        conflicting_snapshot = conflicting_report["scene_world_snapshot"]
        conflicting_registry = conflicting_report["scene_entity_registry"]
        conflicting_snapshot["actor_entities"][0]["semantic_role"] = "conflicting-role"
        conflicting_registry["entities"][0]["semantic_role"] = "conflicting-role"
        conflicting_snapshot["world_fingerprint"] = scene_world_fingerprint(
            [
                *conflicting_snapshot["environment_entities"],
                *conflicting_snapshot["actor_entities"],
            ],
            plan_id="plan-1",
            scene_version=2,
        )

        with self.assertRaises(RuntimeError):
            runtime._persist_user_report("room-1", "plan-1", "", conflicting_report)

        self.assertEqual(runtime.state.room("room-1")["scene_world_snapshots"], frozen_before)
        self.assertEqual(len(runtime.state.room("room-1").get("reports") or []), report_count)

    def test_report_downgrades_game_ready_registry_without_engine_snapshot(self) -> None:
        runtime = AgentRuntime()
        applied, _ = runtime.state.apply_patch(StatePatch(room_id="room-1", changes=_room_fact(game_ready=True)))
        self.assertTrue(applied)

        report = runtime.generate_report("room-1", plan_id="plan-1")

        self.assertEqual(report["scene_entity_registry"]["game_ready_entity_count"], 1)
        self.assertEqual(report["scene_world_consistency_audit"]["status"], "blocked")
        self.assertEqual(report["scene_world_snapshot"]["world_readiness"], "needs_review")
        self.assertEqual(report["completion_status"]["world_readiness"], "needs_review")

    def test_terminal_report_waits_for_same_version_finalizer_evidence(self) -> None:
        runtime = AgentRuntime()
        applied, _ = runtime.state.apply_patch(
            StatePatch(room_id="room-1", changes=_room_fact(game_ready=False))
        )
        self.assertTrue(applied)

        pending_report = runtime.generate_report("room-1", plan_id="plan-1")
        scene_version = int(pending_report["scene_world_snapshot"]["scene_version"])
        pending_event = runtime.state.room("room-1")["runtime_events"][-1]

        self.assertEqual(pending_event["event_type"], "report_pending")
        self.assertTrue(pending_event["payload"]["terminal_report_candidate"])
        self.assertFalse(pending_event["payload"]["terminal_prerequisites_ready"])
        self.assertEqual(
            pending_event["payload"]["missing_terminal_evidence"],
            [
                "scene_entity_registry_ready",
                "runtime_scene_world_consistency_audited",
                "scene_world_snapshot_ready",
            ],
        )

        for event_type in pending_event["payload"]["missing_terminal_evidence"]:
            runtime.operation_log.append(
                event_type,
                room_id="room-1",
                plan_id="plan-1",
                payload={"scene_version": scene_version},
            )

        ready_report = runtime.generate_report("room-1", plan_id="plan-1")
        ready_event = runtime.state.room("room-1")["runtime_events"][-1]

        self.assertEqual(ready_event["event_type"], "report_ready")
        self.assertTrue(ready_event["payload"]["terminal_prerequisites_ready"])
        self.assertEqual(ready_event["payload"]["missing_terminal_evidence"], [])
        self.assertEqual(
            ready_event["payload"]["scene_version"],
            ready_report["scene_world_snapshot"]["scene_version"],
        )
        self.assertEqual(
            ready_event["payload"]["world_fingerprint"],
            ready_report["scene_world_snapshot"]["world_fingerprint"],
        )
        self.assertGreater(ready_event["payload"]["needs_review_entity_count"], 0)
        self.assertTrue(ready_event["payload"]["readiness_missing_field_counts"])

    def test_world_consistency_audit_matches_runtime_and_engine_identity(self) -> None:
        room = _room_fact(game_ready=True)
        registry = AgentRuntime._scene_entity_registry_for_plan(room, "plan-1")
        entity = next(row for row in registry["entities"] if row.get("actor_id") == "actor-cupid")
        room["engine_scene_snapshots"] = {
            "snapshot-runtime-1": {
                "snapshot_id": "snapshot-runtime-1",
                "room_id": "room-1",
                "scene_name": "Scene/runtime.scene",
                "plan_id": "plan-1",
                "scene_version": 2,
                "actor_count": 1,
                "source": "scene_snapshot_tool",
                "timestamp": 10.0,
                "actors": [
                    {
                        "actor_id": entity["actor_id"],
                        "entity_id": entity["entity_id"],
                        "asset_id": entity["asset_id"],
                        "model_ref": entity["model_ref"],
                        "name": entity["name"],
                        "source": "scene_snapshot",
                        "status": "success",
                        "version": entity["version"],
                        "entity_version": entity["version"],
                        "bounds_ready": True,
                        "bounds_source": "engine_actual",
                        "engine_lifecycle_status": "bounds_ready",
                        "sync_status": "engine_imported",
                        "aabb": [-0.5, 0, -0.5, 0.5, 1.8, 0.5],
                        "position": [0, 0, 0],
                        "rotation": entity["transform"]["rotation"],
                        "scale": entity["transform"]["scale"],
                    }
                ],
            }
        }
        runtime = AgentRuntime()
        applied, _ = runtime.state.apply_patch(StatePatch(room_id="room-1", changes=room))
        self.assertTrue(applied)
        before = runtime.state.room("room-1")

        result = runtime.handle_message(
            room_id="room-1",
            text="",
            action="runtime.scene_world_consistency.audit",
        )

        after = runtime.state.room("room-1")
        self.assertTrue(result["found"])
        self.assertEqual(result["audit"]["status"], "consistent")
        self.assertEqual(result["audit"]["matched_entity_count"], 1)
        self.assertEqual(result["audit"]["issue_count"], 0)
        self.assertTrue(result["audit"]["fingerprints_match"])
        self.assertEqual(
            result["audit"]["world_fingerprint"],
            result["audit"]["engine_fingerprint"],
        )
        self.assertEqual(before.get("tool_graphs", {}), after.get("tool_graphs", {}))
        self.assertEqual(before.get("pending_interventions", {}), after.get("pending_interventions", {}))
        policy = AgentRuntime.message_action_policy("runtime.scene_world_consistency.audit")
        self.assertEqual(policy["category"], "read_only")

    def test_world_consistency_audit_reports_identity_and_version_drift(self) -> None:
        room = _room_fact(game_ready=True)
        registry = AgentRuntime._scene_entity_registry_for_plan(room, "plan-1")
        entity = next(row for row in registry["entities"] if row.get("actor_id") == "actor-cupid")
        room["engine_scene_snapshots"] = {
            "snapshot-drift": {
                "snapshot_id": "snapshot-drift",
                "room_id": "room-1",
                "scene_name": "Scene/runtime.scene",
                "plan_id": "plan-1",
                "actor_count": 2,
                "source": "scene_snapshot_tool",
                "timestamp": 20.0,
                "actors": [
                    {
                        "actor_id": entity["actor_id"],
                        "entity_id": entity["entity_id"],
                        "asset_id": "asset-wrong",
                        "name": entity["name"],
                        "source": "scene_snapshot",
                        "version": entity["version"] + 1,
                    },
                    {
                        "actor_id": "actor-without-runtime-identity",
                        "name": "manual actor",
                        "source": "scene_snapshot",
                        "version": 1,
                    },
                ],
            }
        }
        runtime = AgentRuntime()
        applied, _ = runtime.state.apply_patch(StatePatch(room_id="room-1", changes=room))
        self.assertTrue(applied)

        result = runtime.handle_message(
            room_id="room-1",
            text="",
            action="runtime.scene_world_consistency.audit",
        )

        audit = result["audit"]
        self.assertEqual(audit["status"], "needs_review")
        self.assertEqual(audit["unidentified_engine_actor_ids"], ["actor-without-runtime-identity"])
        self.assertEqual(audit["asset_id_mismatches"][0]["entity_id"], entity["entity_id"])
        self.assertEqual(audit["version_mismatches"][0]["actual"], entity["version"] + 1)
        self.assertEqual(audit["transform_mismatches"][0]["entity_id"], entity["entity_id"])
        self.assertEqual(audit["world_aabb_mismatches"][0]["entity_id"], entity["entity_id"])
        self.assertFalse(audit["fingerprints_match"])

    def test_world_consistency_audit_rejects_non_materialized_runtime_entity(self) -> None:
        materialized = {
            "entity_id": "entity-desk",
            "actor_id": "actor-desk",
            "asset_id": "asset-desk",
            "model_ref": "desk.obj",
            "version": 1,
            "transform": {
                "position": [0.0, 0.0, 0.0],
                "rotation": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
            },
            "world_aabb": {"min": [-0.5, 0.0, -0.5], "max": [0.5, 1.0, 0.5]},
        }
        planned_only = {
            "entity_id": "entity-terrain",
            "actor_id": "",
            "asset_id": "asset-terrain",
            "model_ref": "terrain.obj",
            "version": 1,
            "transform": {},
            "world_aabb": {},
        }

        audit = audit_scene_world_consistency(
            world_snapshot={
                "plan_id": "plan-1",
                "scene_version": 1,
                "environment_entities": [planned_only],
                "actor_entities": [materialized],
            },
            engine_snapshot={
                "snapshot_id": "snapshot-partial-world",
                "plan_id": "plan-1",
                "actors": [materialized],
            },
        )

        self.assertEqual(audit["status"], "needs_review")
        self.assertEqual(audit["expected_entity_count"], 2)
        self.assertEqual(audit["materialized_entity_count"], 1)
        self.assertEqual(audit["non_materialized_entity_count"], 1)
        self.assertGreaterEqual(audit["issue_count"], 1)
        self.assertFalse(audit["fingerprints_match"])

    def test_world_consistency_audit_rejects_missing_engine_resource_identity(self) -> None:
        runtime_entity = {
            "entity_id": "entity-desk",
            "actor_id": "actor-desk",
            "asset_id": "asset-desk",
            "model_ref": "desk.obj",
            "version": 3,
            "transform": {
                "position": [0.0, 0.0, 0.0],
                "rotation": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
            },
            "world_aabb": {"min": [-0.5, 0.0, -0.5], "max": [0.5, 1.0, 0.5]},
        }
        engine_actor = dict(runtime_entity)
        engine_actor["model_ref"] = ""
        engine_actor["version"] = 0

        audit = audit_scene_world_consistency(
            world_snapshot={
                "plan_id": "plan-1",
                "scene_version": 3,
                "environment_entities": [],
                "actor_entities": [runtime_entity],
            },
            engine_snapshot={
                "snapshot_id": "snapshot-missing-resource-identity",
                "plan_id": "plan-1",
                "actors": [engine_actor],
            },
        )

        self.assertEqual(audit["status"], "needs_review")
        self.assertEqual(audit["model_ref_mismatches"][0]["actual"], "")
        self.assertEqual(audit["version_mismatches"][0]["actual"], 0)
        self.assertFalse(audit["fingerprints_match"])

    def test_inconsistent_engine_audit_downgrades_game_ready_snapshot(self) -> None:
        snapshot = {
            "plan_id": "plan-1",
            "scene_version": 1,
            "world_readiness": "game_ready",
            "readiness_summary": {"entity_count": 1, "game_ready_entity_count": 1},
        }

        constrained = constrain_scene_world_snapshot_readiness(
            snapshot,
            {"status": "needs_review", "issue_count": 1, "fingerprints_match": False},
        )

        self.assertEqual(constrained["world_readiness"], "needs_review")
        self.assertEqual(constrained["readiness_summary"]["consistency_status"], "needs_review")
        self.assertEqual(constrained["readiness_summary"]["consistency_issue_count"], 1)
        self.assertEqual(snapshot["world_readiness"], "game_ready")

    def test_engine_snapshot_selection_is_scene_version_monotonic(self) -> None:
        snapshots = {
            "late-old": {
                "snapshot_id": "late-old",
                "plan_id": "plan-1",
                "scene_version": 1,
                "timestamp": 30.0,
                "actors": [],
            },
            "current": {
                "snapshot_id": "current",
                "plan_id": "plan-1",
                "scene_version": 2,
                "timestamp": 20.0,
                "actors": [],
            },
        }

        selected = latest_engine_snapshot(snapshots, plan_id="plan-1", scene_version=2)
        missing = latest_engine_snapshot(snapshots, plan_id="plan-1", scene_version=3)

        self.assertEqual(selected["snapshot_id"], "current")
        self.assertEqual(missing, {})

    def test_legacy_engine_snapshot_cannot_prove_current_world_version(self) -> None:
        entity = {
            "entity_id": "entity-desk",
            "actor_id": "actor-desk",
            "asset_id": "asset-desk",
            "model_ref": "desk.obj",
            "version": 1,
            "transform": {
                "position": [0.0, 0.0, 0.0],
                "rotation": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
            },
            "world_aabb": {
                "min": [-0.5, 0.0, -0.5],
                "max": [0.5, 1.0, 0.5],
            },
        }
        snapshots = {
            "legacy": {
                "snapshot_id": "legacy",
                "plan_id": "plan-1",
                "scene_version": 0,
                "timestamp": 30.0,
                "actors": [dict(entity)],
            }
        }

        selected = latest_engine_snapshot(snapshots, plan_id="plan-1", scene_version=3)
        audit = audit_scene_world_consistency(
            world_snapshot={
                "plan_id": "plan-1",
                "scene_version": 3,
                "environment_entities": [],
                "actor_entities": [dict(entity)],
            },
            engine_snapshot=selected,
        )

        self.assertEqual(selected["snapshot_id"], "legacy")
        self.assertEqual(audit["status"], "needs_review")
        self.assertEqual(audit["engine_scene_version"], 0)
        self.assertFalse(audit["scene_version_matches"])
        self.assertIn(
            "engine_snapshot_scene_version_missing",
            audit["snapshot_identity_issues"],
        )
        self.assertFalse(audit["fingerprints_match"])

        wrong_plan_audit = audit_scene_world_consistency(
            world_snapshot={
                "plan_id": "plan-1",
                "scene_version": 3,
                "environment_entities": [],
                "actor_entities": [dict(entity)],
            },
            engine_snapshot={
                "snapshot_id": "wrong-plan",
                "plan_id": "plan-other",
                "scene_version": 3,
                "actors": [dict(entity)],
            },
        )

        self.assertEqual(wrong_plan_audit["status"], "needs_review")
        self.assertFalse(wrong_plan_audit["plan_id_matches"])
        self.assertIn(
            "engine_snapshot_plan_id_mismatch",
            wrong_plan_audit["snapshot_identity_issues"],
        )
        self.assertFalse(wrong_plan_audit["fingerprints_match"])

    def test_batch_scene_snapshot_call_carries_scene_version(self) -> None:
        runtime = AgentRuntime()
        plan = ScenePlan(
            plan_id="plan-versioned-snapshot",
            room_id="room-versioned-snapshot",
            title="versioned",
            design_brief="bedroom",
            status=ScenePlanStatus.CONFIRMED,
            version=4,
        )
        batch = BatchPlan(
            batch_id="batch-versioned-snapshot",
            plan_id=plan.plan_id,
            room_id=plan.room_id,
            requested_items=["bed"],
        )

        graph = runtime._build_batch_execution_graph(plan, batch)
        snapshot_call = next(
            call for call in graph.nodes.values() if call.tool_name == "runtime.scene.snapshot"
        )

        self.assertEqual(snapshot_call.args["scene_version"], 4)

    def test_scene_snapshot_refresh_targets_latest_completed_plan_version(self) -> None:
        runtime = AgentRuntime()
        applied, _ = runtime.state.apply_patch(StatePatch(room_id="room-1", changes=_room_fact(game_ready=True)))
        self.assertTrue(applied)

        refreshed = runtime.refresh_scene_snapshot("room-1")
        room = runtime.state.room("room-1")
        snapshot = next(reversed(dict(room.get("engine_scene_snapshots") or {}).values()))

        self.assertEqual(refreshed["graph"]["status"], "completed")
        self.assertEqual(snapshot["plan_id"], "plan-1")
        self.assertEqual(snapshot["scene_version"], 2)

    def test_environment_support_semantics_are_game_ready_specific(self) -> None:
        room = _room_fact(game_ready=True)
        room["environment_components"] = {
            "batch-1": {
                "floor": {
                    "component_id": "floor",
                    "component_type": "room_floor",
                    "actor_id": "actor-floor",
                    "asset_id": "asset-floor",
                    "model_ref": "room_floor.obj",
                    "position": [0.0, 0.0, 0.0],
                    "rotation": [0.0, 0.0, 0.0],
                    "scale": [1.0, 1.0, 1.0],
                    "aabb": {"min": [-3.0, 0.0, -3.0], "max": [3.0, 0.1, 3.0]},
                    "bounds_source": "engine_actual",
                    "bounds_ready": True,
                    "render_status_observed": True,
                    "render_ready": True,
                    "gpu_build_state": "Ready",
                    "mesh_count": 1,
                    "renderable_mesh_count": 1,
                    "invalid_mesh_count": 0,
                    "engine_lifecycle_status": "bounds_ready",
                    "sync_status": "engine_created",
                    "source": "engine_environment_import",
                    "status": "success",
                    "requires_engine_write": False,
                    "grounding_status": "enclosure",
                },
                "shell": {
                    "component_id": "shell",
                    "component_type": "room_box",
                    "actor_id": "actor-shell",
                    "asset_id": "asset-shell",
                    "model_ref": "room_box.obj",
                    "position": [0.0, 0.0, 0.0],
                    "rotation": [0.0, 0.0, 0.0],
                    "scale": [1.0, 1.0, 1.0],
                    "aabb": {"min": [-3.0, 0.0, -3.0], "max": [3.0, 3.0, 3.0]},
                    "bounds_source": "engine_actual",
                    "bounds_ready": True,
                    "render_status_observed": True,
                    "render_ready": True,
                    "gpu_build_state": "Ready",
                    "mesh_count": 1,
                    "renderable_mesh_count": 1,
                    "invalid_mesh_count": 0,
                    "engine_lifecycle_status": "bounds_ready",
                    "sync_status": "engine_created",
                    "source": "engine_environment_import",
                    "status": "success",
                    "requires_engine_write": False,
                    "grounding_status": "grounded",
                },
                "transition": {
                    "component_id": "transition",
                    "component_type": "transition_zone",
                    "actor_id": "actor-transition",
                    "asset_id": "asset-transition",
                    "model_ref": "transition_zone.obj",
                    "position": [0.0, 0.0, 3.0],
                    "rotation": [0.0, 0.0, 0.0],
                    "scale": [1.0, 1.0, 1.0],
                    "aabb": {"min": [-2.0, 0.0, 2.0], "max": [2.0, 0.1, 4.0]},
                    "bounds_source": "engine_actual",
                    "bounds_ready": True,
                    "render_status_observed": True,
                    "render_ready": True,
                    "gpu_build_state": "Ready",
                    "mesh_count": 1,
                    "renderable_mesh_count": 1,
                    "invalid_mesh_count": 0,
                    "engine_lifecycle_status": "bounds_ready",
                    "sync_status": "engine_created",
                    "source": "engine_environment_import",
                    "status": "success",
                    "requires_engine_write": False,
                },
            }
        }

        registry = AgentRuntime._scene_entity_registry_for_plan(room, "plan-1")
        support_by_component = {
            entity["component_type"]: entity["grounding_status"]
            for entity in registry["entities"]
            if entity.get("entity_type") == "environment"
        }

        self.assertEqual(support_by_component["room_floor"], "grounded")
        self.assertEqual(support_by_component["room_box"], "enclosure")
        self.assertEqual(support_by_component["transition_zone"], "grounded")
        self.assertTrue(all(
            entity["game_ready"]
            for entity in registry["entities"]
            if entity.get("entity_type") == "environment"
        ))

    def test_environment_component_absorbs_engine_actor_observation(self) -> None:
        room = _room_fact(game_ready=True)
        room["environment_components"] = {
            "batch-1": {
                "floor": {
                    "component_id": "floor",
                    "component_type": "room_floor",
                    "actor_id": "actor-floor",
                    "asset_id": "asset-floor",
                    "model_ref": "room_floor.obj",
                    "position": [0.0, 0.0, 0.0],
                    "rotation": [0.0, 0.0, 0.0],
                    "scale": [1.0, 1.0, 1.0],
                    "aabb": {"min": [-3.0, 0.0, -3.0], "max": [3.0, 0.1, 3.0]},
                    "bounds_source": "estimated",
                    "bounds_ready": False,
                    "render_status_observed": False,
                    "render_ready": False,
                    "engine_lifecycle_status": "engine_loading",
                    "sync_status": "engine_created",
                    "source": "engine_environment_import",
                    "status": "success",
                    "grounding_status": "grounded",
                }
            }
        }
        room["observed_actors"]["actor-floor"] = {
            "actor_id": "actor-floor",
            "plan_id": "plan-1",
            "batch_id": "batch-1",
            "position": [0.0, 0.0, 0.0],
            "rotation": [0.0, 0.0, 0.0],
            "scale": [1.0, 1.0, 1.0],
            "aabb": {"min": [-3.0, 0.0, -3.0], "max": [3.0, 0.1, 3.0]},
            "bounds_source": "engine_actual",
            "bounds_ready": True,
            "render_status_observed": True,
            "render_ready": True,
            "render_failed": False,
            "gpu_build_state": "Ready",
            "mesh_count": 1,
            "renderable_mesh_count": 1,
            "invalid_mesh_count": 0,
            "engine_lifecycle_status": "bounds_ready",
            "actor_version": 3,
        }

        registry = AgentRuntime._scene_entity_registry_for_plan(room, "plan-1")
        floor = next(
            entity
            for entity in registry["entities"]
            if entity.get("component_type") == "room_floor"
        )

        self.assertEqual(floor["bounds_source"], "engine_actual")
        self.assertTrue(floor["render_status_observed"])
        self.assertTrue(floor["render_ready"])
        self.assertEqual(floor["version"], 3)
        self.assertEqual(floor["grounding_status"], "grounded")
        self.assertTrue(floor["game_ready"])
        self.assertEqual(floor["readiness_missing_fields"], [])

    def test_environment_import_persists_room_support_semantics_before_snapshot(self) -> None:
        class Gate:
            def invoke_tool(self, tool, payload):
                return tool.invoke(payload)

        class ImportTool:
            def invoke(self, payload):
                return {
                    "status": "success",
                    "component_id": payload["component_id"],
                    "component_type": payload["component_type"],
                    "entity_id": payload["entity_id"],
                    "actor_id": f"actor-{payload['component_id']}",
                    "asset_id": payload["asset_id"],
                    "model_ref": payload["model_ref"],
                    "actor_data": {
                        "actor_id": f"actor-{payload['component_id']}",
                        "bounds_ready": True,
                        "render_status_observed": True,
                        "render_ready": True,
                        "geometry": {
                            "position": [0.0, 0.0, 0.0],
                            "rotation": [0.0, 0.0, 0.0],
                            "scale": [1.0, 1.0, 1.0],
                            "aabb": {"min": [-3.0, 0.0, -3.0], "max": [3.0, 3.0, 3.0]},
                        },
                    },
                }

        provider = make_engine_environment_component_import_provider(
            environment_import_tool=ImportTool(),
            engine_gate=Gate(),
        )
        result = provider({
            "room_id": "room-support",
            "plan_id": "plan-support",
            "batch_id": "batch-support",
            "environment_components": {
                "shell": {
                    "component_id": "shell",
                    "component_type": "room_box",
                    "asset_id": "asset-shell",
                    "model_ref": "room_box.obj",
                },
                "floor": {
                    "component_id": "floor",
                    "component_type": "room_floor",
                    "asset_id": "asset-floor",
                    "model_ref": "room_floor.obj",
                },
            },
        })

        self.assertEqual(result["environment_components"]["shell"]["grounding_status"], "enclosure")
        self.assertEqual(result["environment_components"]["floor"]["grounding_status"], "grounded")
        self.assertEqual(result["environment_import_results"][0]["grounding_status"], "enclosure")
        self.assertEqual(result["environment_import_results"][1]["grounding_status"], "grounded")

    def test_business_graph_role_is_persisted_and_validated(self) -> None:
        graph = ToolCallGraph(
            graph_id="graph-business",
            plan_id="plan-1",
            batch_id="batch-1",
            graph_role="business_batch",
        )
        graph.add(ToolCall(tool_call_id="tool-1", tool_name="mock.echo"))
        fact = ToolCallGraphValidator.safe_graph_fact(graph)

        ToolCallGraphValidator.validate_graph_fact(fact)
        self.assertEqual(fact["graph_role"], "business_batch")

    def test_report_separates_business_batches_from_internal_graphs(self) -> None:
        runtime = AgentRuntime()
        plan = runtime.propose_scene_plan(
            room_id="room-graph-domains",
            text="simple bedroom with bed and desk",
            owner_agent="tester",
        )
        runtime.confirm_scene_plan(plan.plan_id, confirmed_by="host")
        queued = runtime.enqueue_planned_batches(plan.plan_id, max_items_per_batch=1)

        report = runtime.generate_report("room-graph-domains", plan_id=plan.plan_id)
        domains = report["tool_graph_domain_summary"]

        self.assertEqual(domains["business_batch_count"], len(queued["batches"]))
        self.assertGreater(domains["internal_graph_count"], 0)
        self.assertEqual(
            domains["total_graph_count"],
            domains["business_batch_count"] + domains["internal_graph_count"],
        )
        self.assertEqual(report["completion_status"]["pipeline_status"], "running")
        self.assertNotEqual(report["completion_status"]["world_readiness"], "game_ready")

    def test_finalizer_records_registry_and_snapshot_before_report_ready(self) -> None:
        runtime = AgentRuntime()
        plan = ScenePlan(
            plan_id="plan-finalizer-order",
            room_id="room-finalizer-order",
            title="order",
            design_brief="order",
            status=ScenePlanStatus.COMPLETED,
        )
        batch = BatchPlan(
            batch_id="batch-finalizer-order",
            plan_id=plan.plan_id,
            room_id=plan.room_id,
            requested_items=["table"],
            status=BatchPlanStatus.COMPLETED,
        )
        registry = {"entity_count": 1, "game_ready_entity_count": 0, "entities": [{}]}
        snapshot = {
            "scene_version": 1,
            "world_readiness": "needs_review",
            "environment_entities": [],
            "actor_entities": [{}],
            "operation_cursor": "op:1",
        }
        report = {"scene_entity_registry": registry, "scene_world_snapshot": snapshot}

        def generate_report(*_args, **_kwargs):
            runtime.operation_log.append("report_ready", room_id=plan.room_id, plan_id=plan.plan_id)
            return report

        with (
            patch.object(runtime, "_runtime_plan_by_id_from_state", return_value=plan),
            patch.object(runtime, "_planned_batches_for_plan", return_value=[batch]),
            patch.object(runtime, "_reconcile_partial_engine_readiness", return_value={}),
            patch.object(
                runtime,
                "refresh_scene_snapshot",
                return_value={"graph": {"status": "completed"}},
            ) as refresh_snapshot,
            patch.object(runtime, "_scene_entity_registry_for_plan", return_value=registry),
            patch.object(runtime, "_scene_world_snapshot_for_plan", return_value=snapshot),
            patch.object(runtime, "_latest_persisted_report_for_plan", side_effect=[{}, report]),
            patch.object(runtime, "generate_report", side_effect=generate_report),
            patch.object(runtime, "_persist_plan_identity_changes", return_value=True),
        ):
            result = runtime._finalize_plan_after_queue_drain(
                room_id=plan.room_id,
                plan_id=plan.plan_id,
            )

        self.assertTrue(result["report_ready"])
        refresh_snapshot.assert_called_once_with(
            plan.room_id,
            plan_id=plan.plan_id,
            scene_version=plan.version,
        )
        events = runtime.operation_log.events()
        self.assertLess(
            events.index("runtime_scene_world_consistency_audited"),
            events.index("scene_world_snapshot_ready"),
        )
        self.assertLess(
            events.index("scene_entity_registry_ready"),
            events.index("runtime_scene_world_consistency_audited"),
        )
        self.assertLess(
            events.index("runtime_scene_world_consistency_audited"),
            events.index("report_ready"),
        )
        self.assertLess(events.index("scene_world_snapshot_ready"), events.index("report_ready"))
        self.assertLess(events.index("report_ready"), events.index("latest_completed_plan_set"))

    def test_finalizer_ready_events_are_idempotent_per_scene_version(self) -> None:
        runtime = AgentRuntime()
        plan = ScenePlan(
            plan_id="plan-versioned-finalizer",
            room_id="room-versioned-finalizer",
            title="versioned",
            design_brief="versioned",
            status=ScenePlanStatus.COMPLETED,
            version=1,
        )
        batch = BatchPlan(
            batch_id="batch-versioned-finalizer",
            plan_id=plan.plan_id,
            room_id=plan.room_id,
            requested_items=["table"],
            status=BatchPlanStatus.COMPLETED,
        )
        registry = {"entity_count": 1, "game_ready_entity_count": 0, "entities": [{}]}

        def snapshot_for_version(*_args, **_kwargs):
            return {
                "plan_id": plan.plan_id,
                "scene_version": plan.version,
                "world_readiness": "needs_review",
                "environment_entities": [],
                "actor_entities": [{}],
                "operation_cursor": "op:1",
            }

        with (
            patch.object(runtime, "_runtime_plan_by_id_from_state", return_value=plan),
            patch.object(runtime, "_planned_batches_for_plan", return_value=[batch]),
            patch.object(runtime, "_reconcile_partial_engine_readiness", return_value={}),
            patch.object(runtime, "_scene_entity_registry_for_plan", return_value=registry),
            patch.object(runtime, "_scene_world_snapshot_for_plan", side_effect=snapshot_for_version),
            patch.object(runtime, "_latest_persisted_report_for_plan", return_value={"report": "ready"}),
            patch.object(runtime, "_report_covers_current_plan_state", return_value=True),
            patch.object(runtime, "_persist_plan_identity_changes", return_value=True),
        ):
            runtime._finalize_plan_after_queue_drain(room_id=plan.room_id, plan_id=plan.plan_id)
            plan.version = 2
            runtime._finalize_plan_after_queue_drain(room_id=plan.room_id, plan_id=plan.plan_id)
            runtime._finalize_plan_after_queue_drain(room_id=plan.room_id, plan_id=plan.plan_id)

        registry_versions = [
            int(entry.payload.get("scene_version") or 0)
            for entry in runtime.operation_log.query(
                event="scene_entity_registry_ready",
                room_id=plan.room_id,
                plan_id=plan.plan_id,
            )
        ]
        snapshot_versions = [
            int(entry.payload.get("scene_version") or 0)
            for entry in runtime.operation_log.query(
                event="scene_world_snapshot_ready",
                room_id=plan.room_id,
                plan_id=plan.plan_id,
            )
        ]
        self.assertEqual(registry_versions, [1, 2])
        self.assertEqual(snapshot_versions, [1, 2])

    def test_finalizer_same_world_short_circuits_before_snapshot_refresh(self) -> None:
        runtime = AgentRuntime()
        plan = ScenePlan(
            plan_id="plan-finalizer-same-world",
            room_id="room-finalizer-same-world",
            title="same-world",
            design_brief="same-world",
            status=ScenePlanStatus.COMPLETED,
            version=3,
        )
        batch = BatchPlan(
            batch_id="batch-finalizer-same-world",
            plan_id=plan.plan_id,
            room_id=plan.room_id,
            requested_items=["table"],
            status=BatchPlanStatus.COMPLETED,
        )
        registry = {"entity_count": 1, "game_ready_entity_count": 1, "entities": [{}]}
        snapshot = {
            "plan_id": plan.plan_id,
            "scene_version": plan.version,
            "world_fingerprint": "sha256:same-world",
            "world_readiness": "game_ready",
            "environment_entities": [],
            "actor_entities": [{}],
            "operation_cursor": "op:1",
        }
        report = {
            "plan_id": plan.plan_id,
            "plan_summary": {"status": "completed"},
            "scene_entity_registry": registry,
            "scene_world_snapshot": snapshot,
            "completion_status": {
                "pipeline_status": "completed",
                "engine_materialization_status": "complete",
                "world_readiness": "game_ready",
            },
            "report_health_summary": {"status": "ok", "attention_required": False},
        }

        with (
            patch.object(runtime, "_runtime_plan_by_id_from_state", return_value=plan),
            patch.object(runtime, "_planned_batches_for_plan", return_value=[batch]),
            patch.object(runtime, "_reconcile_partial_engine_readiness", return_value={}),
            patch.object(
                runtime,
                "refresh_scene_snapshot",
                return_value={"graph": {"status": "completed"}},
            ) as refresh_snapshot,
            patch.object(runtime, "_scene_entity_registry_for_plan", return_value=registry),
            patch.object(runtime, "_scene_world_snapshot_for_plan", return_value=snapshot),
            patch.object(runtime, "_latest_persisted_report_for_plan", return_value=report),
            patch.object(runtime, "_report_covers_current_plan_state", return_value=True),
            patch.object(runtime, "_persist_plan_identity_changes", return_value=True),
        ):
            first = runtime._finalize_plan_after_queue_drain(
                room_id=plan.room_id,
                plan_id=plan.plan_id,
            )
            operation_count = len(runtime.operation_log.entries())
            second = runtime._finalize_plan_after_queue_drain(
                room_id=plan.room_id,
                plan_id=plan.plan_id,
            )

        self.assertTrue(first["report_ready"])
        self.assertEqual(second["reason"], "already_finalized_same_world")
        self.assertTrue(second["idempotent"])
        self.assertEqual(len(runtime.operation_log.entries()), operation_count)
        refresh_snapshot.assert_called_once()

    def test_failed_terminal_key_survives_later_plan_version_changes(self) -> None:
        runtime = AgentRuntime()
        plan = ScenePlan(
            plan_id="plan-terminal-key-failed",
            room_id="room-terminal-key-failed",
            title="failed",
            design_brief="failed",
            status=ScenePlanStatus.FAILED,
            version=9,
        )
        batch = BatchPlan(
            batch_id="batch-terminal-key-failed",
            plan_id=plan.plan_id,
            room_id=plan.room_id,
            requested_items=["key"],
            status=BatchPlanStatus.FAILED,
        )
        fingerprint = "sha256:" + "a" * 64
        report = {
            "plan_id": plan.plan_id,
            "plan_summary": {"status": "failed", "version": 3},
            "batch_summary": {
                "batches": [{"batch_id": batch.batch_id, "status": "failed"}],
            },
        }
        applied, reason = runtime.state.apply_patch(StatePatch(
            room_id=plan.room_id,
            changes={
                "runtime_events": [{
                    "event_id": "event-terminal-key-failed",
                    "room_id": plan.room_id,
                    "plan_id": plan.plan_id,
                    "batch_id": "",
                    "event_type": "report_ready",
                    "phase": "report",
                    "audience": "host",
                    "level": "warning",
                    "title": "failed report ready",
                    "message": "failed report ready",
                    "progress": 100,
                    "timestamp": 1.0,
                    "payload": {
                        "status": "failed",
                        "terminal_status": "failed",
                        "scene_version": 3,
                        "world_fingerprint": fingerprint,
                        "terminal_key": f"{plan.plan_id}:3:{fingerprint}:failed",
                    },
                }],
            },
            expected_version=runtime.state.version,
            source_tool_call_id="test-terminal-key",
        ))
        self.assertTrue(applied, reason)

        with (
            patch.object(runtime, "_runtime_plan_by_id_from_state", return_value=plan),
            patch.object(runtime, "_planned_batches_for_plan", return_value=[batch]),
            patch.object(runtime, "_latest_persisted_report_for_plan", return_value=report),
            patch.object(runtime, "_reconcile_partial_engine_readiness") as reconcile,
            patch.object(runtime, "refresh_scene_snapshot") as refresh,
        ):
            result = runtime._finalize_plan_after_queue_drain(
                room_id=plan.room_id,
                plan_id=plan.plan_id,
            )

        self.assertEqual(result["reason"], "already_finalized_terminal_key")
        self.assertTrue(result["idempotent"])
        reconcile.assert_not_called()
        refresh.assert_not_called()


if __name__ == "__main__":
    unittest.main()
