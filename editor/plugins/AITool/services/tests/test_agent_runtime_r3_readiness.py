from __future__ import annotations

from copy import deepcopy
import unittest

from editor.plugins.AITool.services.agent_runtime import (
    AgentRuntime,
    R3_DIMENSION_NAMES,
    R3GateReportValidator,
    evaluate_r3_gate,
)
from editor.plugins.AITool.services.agent_runtime.scene_world_consistency import (
    audit_scene_world_consistency,
    scene_world_fingerprint,
)
from editor.plugins.AITool.services.agent_collaboration import (
    GameplayEntitySlot,
    derive_demo_readiness_requirements,
)
from editor.plugins.AITool.services.integration_contracts import DemoReadinessRequirement


def _entity(index: int, *, game_ready: bool, environment: bool = False) -> dict:
    entity_id = f"entity-{index:02d}"
    actor_id = f"actor-{index:02d}"
    component_type = "room_box" if index == 0 else "room_floor" if index == 1 else ""
    row = {
        "entity_id": entity_id,
        "actor_id": actor_id,
        "asset_id": f"asset-{index:02d}",
        "model_ref": f"model-{index:02d}",
        "version": 1,
        "entity_type": "environment" if environment else "furniture",
        "semantic_role": component_type or f"prop-{index:02d}",
        "component_type": component_type,
        "transform": {
            "position": [float(index), 0.0, 0.0],
            "rotation": [0.0, 0.0, 0.0],
            "scale": [1.0, 1.0, 1.0],
        },
        "world_aabb": {
            "min": [float(index), 0.0, 0.0],
            "max": [float(index + 1), 1.0, 1.0],
        },
        "bounds_source": "engine_actual",
        "grounding_status": "enclosure" if component_type == "room_box" else "grounded",
        "sync_status": "synced",
        "engine_write_verification_status": "engine_verified",
        "render_status_observed": True,
        "render_ready": True,
        "render_failed": False,
        "gpu_build_state": "Ready",
        "mesh_count": 1,
        "renderable_mesh_count": 1,
        "invalid_mesh_count": 0,
        "game_ready": bool(game_ready),
        "readiness_missing_fields": [] if game_ready else ["support_classification"],
    }
    return row


def _gate_facts(*, game_ready_count: int) -> dict:
    entities = [
        _entity(index, game_ready=index < game_ready_count, environment=index < 2)
        for index in range(14)
    ]
    environment_entities = entities[:2]
    actor_entities = entities[2:]
    plan_id = "plan-bedroom"
    scene_version = 3
    fingerprint = scene_world_fingerprint(
        entities,
        plan_id=plan_id,
        scene_version=scene_version,
    )
    snapshot = {
        "room_id": "room-1",
        "plan_id": plan_id,
        "scene_version": scene_version,
        "world_readiness": "needs_review",
        "snapshot_authority": "local_runtime",
        "environment_entities": environment_entities,
        "actor_entities": actor_entities,
        "readiness_summary": {
            "entity_count": 14,
            "game_ready_entity_count": game_ready_count,
        },
        "world_fingerprint": fingerprint,
        "operation_cursor": "op:7",
    }
    engine_snapshot = {
        "snapshot_id": "engine-snapshot-3",
        "plan_id": plan_id,
        "scene_version": scene_version,
        "actors": [dict(entity) for entity in entities],
    }
    consistency = audit_scene_world_consistency(
        world_snapshot=snapshot,
        engine_snapshot=engine_snapshot,
    )
    consistency["engine_snapshot_available"] = True
    operation_entries = [
        {
            "event": event,
            "timestamp": float(index + 1),
            "payload": {"scene_version": scene_version},
        }
        for index, event in enumerate(
            (
                "finalizer_started",
                "tool_graph_queue_empty",
                "scene_plan_finalized",
                "scene_entity_registry_ready",
                "runtime_scene_world_consistency_audited",
                "scene_world_snapshot_ready",
            )
        )
    ]
    batches = [
        {
            "batch_id": f"batch-{index}",
            "plan_id": plan_id,
            "status": "completed",
            "tool_graph_id": f"graph-{index}",
        }
        for index in range(1, 4)
    ]
    graphs = [
        {
            "graph_id": f"graph-{index}",
            "batch_id": f"batch-{index}",
            "plan_id": plan_id,
            "graph_role": "business_batch",
            "status": "completed",
            "nodes": {
                f"tool-{index}": {
                    "tool_call_id": f"tool-{index}",
                    "status": "succeeded",
                }
            },
        }
        for index in range(1, 4)
    ]
    registry = {
        "plan_id": plan_id,
        "scene_version": scene_version,
        "entity_count": 14,
        "game_ready_entity_count": game_ready_count,
        "engine_write_verified_count": 14,
        "readiness_missing_field_counts": {
            "support_classification": 14 - game_ready_count,
        },
        "entities": entities,
    }
    return {
        "room_id": "room-1",
        "plan_id": plan_id,
        "scene_version": scene_version,
        "snapshot_result": {
            "found": True,
            "plan_id": plan_id,
            "scene_version": scene_version,
            "snapshot_authority": "local_runtime",
            "snapshot_stability": "immutable",
            "world_fingerprint": fingerprint,
            "snapshot": snapshot,
        },
        "consistency_audit": consistency,
        "scene_entity_registry": registry,
        "required_environment_components": ["room_box", "room_floor"],
        "batch_plans": batches,
        "tool_graphs": graphs,
        "operation_entries": operation_entries,
        "runtime_events": [
            {
                "event_type": "report_ready",
                "timestamp": 7.0,
                "payload": {"scene_version": scene_version},
            }
        ],
        "engine_write_summary": {
            "boundary_fact_count": 3,
            "bridge_call_count": 14,
            "bridge_success_count": 14,
            "bridge_failed_count": 0,
        },
        "multiplayer_evidence": {
            "applicable": True,
            "peer_count": 1,
            "acknowledged_peer_count": 1,
            "unacknowledged_peer_count": 0,
            "comparison_mode": "peer_snapshot_ack",
            "entity_count": 14,
            "verified_entity_count": 14,
            "partial_entity_count": 0,
            "identity_drift_count": 0,
            "version_drift_count": 0,
            "missing_fields_explicit": True,
        },
        "state_version": 9,
        "benchmark_profile": "bedroom_14",
        "expected_entity_count": 14,
        "evaluated_at": 7.0,
    }


def _apply_demo_roles(facts: dict, roles: list[tuple[str, tuple[str, ...]]]) -> None:
    entities = facts["scene_entity_registry"]["entities"]
    for offset, (semantic_role, capabilities) in enumerate(roles, start=2):
        entities[offset]["semantic_role"] = semantic_role
        entities[offset]["interaction_capability"] = list(capabilities)
    snapshot = facts["snapshot_result"]["snapshot"]
    fingerprint = scene_world_fingerprint(
        [*snapshot["environment_entities"], *snapshot["actor_entities"]],
        plan_id=facts["plan_id"],
        scene_version=facts["scene_version"],
    )
    snapshot["world_fingerprint"] = fingerprint
    facts["snapshot_result"]["world_fingerprint"] = fingerprint
    facts["consistency_audit"] = audit_scene_world_consistency(
        world_snapshot=snapshot,
        engine_snapshot={
            "snapshot_id": "engine-demo-snapshot",
            "plan_id": facts["plan_id"],
            "scene_version": facts["scene_version"],
            "actors": [dict(entity) for entity in entities],
        },
    )
    facts["consistency_audit"]["engine_snapshot_available"] = True
    facts["multiplayer_evidence"] = {
        "applicable": False,
        "peer_count": 0,
        "acknowledged_peer_count": 0,
        "unacknowledged_peer_count": 0,
        "comparison_mode": "none",
        "entity_count": len(entities),
        "verified_entity_count": len(entities),
        "partial_entity_count": 0,
        "identity_drift_count": 0,
        "version_drift_count": 0,
        "missing_fields_explicit": True,
    }


class R3ReadinessGateTests(unittest.TestCase):
    def test_eight_of_fourteen_with_all_hard_conditions_is_green(self) -> None:
        report = evaluate_r3_gate(**_gate_facts(game_ready_count=8))

        self.assertEqual(report.overall, "green")
        self.assertEqual(tuple(report.dimensions), R3_DIMENSION_NAMES)
        self.assertEqual(report.dimensions["entity_readiness"].status, "green")
        R3GateReportValidator.validate(report)

    def test_multiplayer_gate_stays_yellow_until_every_known_peer_acknowledges(self) -> None:
        facts = _gate_facts(game_ready_count=8)
        facts["multiplayer_evidence"] = dict(facts["multiplayer_evidence"])
        facts["multiplayer_evidence"].update({
            "peer_count": 2,
            "acknowledged_peer_count": 1,
            "unacknowledged_peer_count": 1,
        })

        report = evaluate_r3_gate(**facts)

        dimension = report.dimensions["multiplayer_consistency"]
        self.assertEqual(dimension.status, "yellow")
        self.assertIn("peer_snapshot_ack", dimension.missing)
        self.assertEqual(dimension.metrics["unacknowledged_peer_count"], 1)

    def test_five_of_fourteen_is_yellow(self) -> None:
        report = evaluate_r3_gate(**_gate_facts(game_ready_count=5))

        self.assertEqual(report.overall, "yellow")
        self.assertEqual(report.dimensions["entity_readiness"].status, "yellow")
        metrics = report.dimensions["entity_readiness"].metrics
        self.assertEqual(metrics["entity_diagnostics_total_count"], 9)
        self.assertEqual(metrics["entity_diagnostics_truncated_count"], 0)
        self.assertEqual(
            [item["entity_ref"] for item in metrics["entity_diagnostics"]],
            [f"entity-{index:02d}" for index in range(5, 14)],
        )
        self.assertTrue(
            all(
                item["readiness_missing_fields"] == ["support_classification"]
                for item in metrics["entity_diagnostics"]
            )
        )
        self.assertIn("readonly_snapshot_analysis", report.capability_unlocks)
        self.assertNotIn("action_proposal", report.capability_unlocks)

    def test_finalizer_events_from_different_scene_versions_do_not_form_green_chain(self) -> None:
        facts = _gate_facts(game_ready_count=8)
        facts["operation_entries"] = deepcopy(facts["operation_entries"])
        registry_event = next(
            entry
            for entry in facts["operation_entries"]
            if entry["event"] == "scene_entity_registry_ready"
        )
        registry_event["payload"]["scene_version"] = 2

        report = evaluate_r3_gate(**facts)

        dimension = report.dimensions["finalizer_completeness"]
        self.assertEqual(dimension.status, "red")
        self.assertIn("scene_entity_registry_ready", dimension.missing)
        self.assertIn("finalizer_scene_version_mismatch", dimension.contradictions)

    def test_business_graph_dimension_ignores_internal_graphs_and_counts_terminal_nodes(self) -> None:
        facts = _gate_facts(game_ready_count=8)
        facts["tool_graphs"] = deepcopy(facts["tool_graphs"])
        facts["tool_graphs"].append(
            {
                "graph_id": "graph-query",
                "batch_id": "",
                "plan_id": facts["plan_id"],
                "graph_role": "query_snapshot",
                "status": "completed",
                "nodes": {
                    "tool-query": {
                        "tool_call_id": "tool-query",
                        "status": "succeeded",
                    }
                },
            }
        )

        report = evaluate_r3_gate(**facts)

        dimension = report.dimensions["business_graph_consistency"]
        self.assertEqual(dimension.status, "green")
        self.assertEqual(dimension.metrics["business_batch_count"], 3)
        self.assertEqual(dimension.metrics["business_graph_count"], 3)
        self.assertEqual(dimension.metrics["business_node_count"], 3)
        self.assertEqual(dimension.metrics["succeeded_node_count"], 3)
        self.assertEqual(dimension.metrics["active_node_count"], 0)

    def test_business_graph_dimension_rejects_wrong_role_and_active_terminal_nodes(self) -> None:
        wrong_role = _gate_facts(game_ready_count=8)
        wrong_role["tool_graphs"] = deepcopy(wrong_role["tool_graphs"])
        wrong_role["tool_graphs"][0]["graph_role"] = "internal_state"

        wrong_role_report = evaluate_r3_gate(**wrong_role)

        wrong_role_dimension = wrong_role_report.dimensions["business_graph_consistency"]
        self.assertEqual(wrong_role_dimension.status, "red")
        self.assertIn(
            "batch-1:graph_role_mismatch:internal_state",
            wrong_role_dimension.contradictions,
        )
        self.assertIn(
            "business_batch_graph_count_mismatch",
            wrong_role_dimension.contradictions,
        )

        active_node = _gate_facts(game_ready_count=8)
        active_node["tool_graphs"] = deepcopy(active_node["tool_graphs"])
        active_node["tool_graphs"][1]["nodes"]["tool-2"]["status"] = "running"

        active_node_report = evaluate_r3_gate(**active_node)

        active_node_dimension = active_node_report.dimensions["business_graph_consistency"]
        self.assertEqual(active_node_dimension.status, "red")
        self.assertIn(
            "batch-2:business_graph_nodes_active",
            active_node_dimension.contradictions,
        )
        self.assertEqual(active_node_dimension.metrics["active_node_count"], 1)

    def test_business_graph_dimension_rejects_plan_batch_and_orphan_mismatches(self) -> None:
        facts = _gate_facts(game_ready_count=8)
        facts["tool_graphs"] = deepcopy(facts["tool_graphs"])
        facts["tool_graphs"][0]["plan_id"] = "plan-other"
        facts["tool_graphs"][1]["batch_id"] = "batch-other"
        facts["tool_graphs"].append(
            {
                "graph_id": "graph-orphan",
                "batch_id": "batch-orphan",
                "plan_id": facts["plan_id"],
                "graph_role": "business_batch",
                "status": "completed",
                "nodes": {
                    "tool-orphan": {
                        "tool_call_id": "tool-orphan",
                        "status": "succeeded",
                    }
                },
            }
        )

        report = evaluate_r3_gate(**facts)

        dimension = report.dimensions["business_graph_consistency"]
        self.assertEqual(dimension.status, "red")
        self.assertIn("batch-1:graph_plan_mismatch", dimension.contradictions)
        self.assertIn("batch-2:graph_batch_mismatch", dimension.contradictions)
        self.assertIn(
            "orphan_business_graph:graph-orphan",
            dimension.contradictions,
        )
        self.assertIn(
            "business_batch_graph_count_mismatch",
            dimension.contradictions,
        )

    def test_business_graph_dimension_rejects_terminal_failed_execution(self) -> None:
        facts = _gate_facts(game_ready_count=8)
        facts["batch_plans"] = deepcopy(facts["batch_plans"])
        facts["tool_graphs"] = deepcopy(facts["tool_graphs"])
        facts["batch_plans"][0]["status"] = "failed"
        facts["tool_graphs"][0]["status"] = "failed"
        facts["tool_graphs"][0]["nodes"]["tool-1"]["status"] = "failed"

        report = evaluate_r3_gate(**facts)

        dimension = report.dimensions["business_graph_consistency"]
        self.assertEqual(dimension.status, "red")
        self.assertIn(
            "batch-1:batch_terminal_unsuccessful:failed",
            dimension.contradictions,
        )
        self.assertIn(
            "batch-1:graph_terminal_unsuccessful:failed",
            dimension.contradictions,
        )
        self.assertIn(
            "batch-1:business_graph_nodes_unsuccessful",
            dimension.contradictions,
        )
        self.assertEqual(dimension.metrics["successful_batch_count"], 2)
        self.assertEqual(dimension.metrics["successful_graph_count"], 2)
        self.assertEqual(dimension.metrics["failed_node_count"], 1)

    def test_entity_diagnostics_include_identity_failures_without_trusting_game_ready(self) -> None:
        facts = _gate_facts(game_ready_count=8)
        registry = deepcopy(facts["scene_entity_registry"])
        registry["entities"][0]["asset_id"] = ""
        registry["entities"][0]["model_ref"] = ""
        facts["scene_entity_registry"] = registry

        report = evaluate_r3_gate(**facts)

        dimension = report.dimensions["entity_readiness"]
        self.assertEqual(dimension.status, "red")
        diagnostics = dimension.metrics["entity_diagnostics"]
        entity = next(item for item in diagnostics if item["entity_ref"] == "entity-00")
        self.assertTrue(entity["declared_game_ready"])
        self.assertFalse(entity["game_ready"])
        self.assertEqual(
            entity["readiness_missing_fields"],
            ["asset_id", "asset_identity", "model_ref"],
        )
        self.assertIn("entity-00:asset_identity", dimension.missing)
        self.assertIn(
            "entity-00:game_ready_without_hard_facts",
            dimension.contradictions,
        )

    def test_entity_readiness_recomputes_engine_facts_instead_of_trusting_flag(self) -> None:
        facts = _gate_facts(game_ready_count=8)
        registry = deepcopy(facts["scene_entity_registry"])
        registry["entities"][2]["bounds_source"] = "estimated"
        facts["scene_entity_registry"] = registry

        report = evaluate_r3_gate(**facts)

        dimension = report.dimensions["entity_readiness"]
        self.assertEqual(dimension.status, "red")
        self.assertEqual(dimension.metrics["game_ready_entity_count"], 7)
        self.assertEqual(dimension.metrics["declared_game_ready_entity_count"], 8)
        self.assertEqual(report.metrics["game_ready_entity_count"], 7)
        self.assertEqual(report.metrics["declared_game_ready_entity_count"], 8)
        self.assertIn(
            "entity-02:game_ready_without_hard_facts",
            dimension.contradictions,
        )
        entity = next(
            item
            for item in dimension.metrics["entity_diagnostics"]
            if item["entity_ref"] == "entity-02"
        )
        self.assertTrue(entity["declared_game_ready"])
        self.assertFalse(entity["game_ready"])
        self.assertIn("engine_actual_aabb", entity["readiness_missing_fields"])

    def test_entity_readiness_summarizes_render_observation_and_invalid_meshes(self) -> None:
        facts = _gate_facts(game_ready_count=8)
        registry = deepcopy(facts["scene_entity_registry"])
        registry["entities"][2]["render_ready"] = False
        registry["entities"][2]["invalid_mesh_count"] = 2
        registry["entities"][3]["render_status_observed"] = False
        registry["entities"][3]["render_ready"] = False
        facts["scene_entity_registry"] = registry

        report = evaluate_r3_gate(**facts)

        metrics = report.dimensions["entity_readiness"].metrics
        self.assertEqual(metrics["render_status_observed_count"], 13)
        self.assertEqual(metrics["render_status_unobserved_count"], 1)
        self.assertEqual(metrics["render_ready_entity_count"], 12)
        self.assertEqual(metrics["render_not_ready_entity_count"], 2)
        self.assertEqual(metrics["invalid_mesh_entity_count"], 1)
        self.assertEqual(metrics["invalid_mesh_slot_count"], 2)
        self.assertEqual(metrics["readiness_missing_field_counts"]["render_not_ready"], 1)
        self.assertEqual(
            metrics["readiness_missing_field_counts"]["render_readiness_unobserved"],
            1,
        )
        self.assertEqual(report.metrics["invalid_mesh_entity_count"], 1)
        self.assertEqual(report.metrics["invalid_mesh_slot_count"], 2)

    def test_environment_fingerprint_and_identity_failures_are_red(self) -> None:
        missing_environment = _gate_facts(game_ready_count=8)
        missing_environment["required_environment_components"] = ["room_box", "room_floor", "ceiling"]
        environment_report = evaluate_r3_gate(**missing_environment)
        self.assertEqual(environment_report.overall, "red")
        self.assertIn(
            "environment_readiness:ceiling",
            environment_report.blockers,
        )

        bad_fingerprint = _gate_facts(game_ready_count=8)
        bad_fingerprint["snapshot_result"] = dict(bad_fingerprint["snapshot_result"])
        bad_fingerprint["snapshot_result"]["world_fingerprint"] = "0" * 64
        bad_fingerprint["snapshot_result"]["snapshot"] = dict(
            bad_fingerprint["snapshot_result"]["snapshot"]
        )
        bad_fingerprint["snapshot_result"]["snapshot"]["world_fingerprint"] = "0" * 64
        fingerprint_report = evaluate_r3_gate(**bad_fingerprint)
        self.assertEqual(fingerprint_report.dimensions["snapshot_integrity"].status, "red")

        identity_drift = _gate_facts(game_ready_count=8)
        registry = deepcopy(identity_drift["scene_entity_registry"])
        registry["entities"][1]["entity_id"] = registry["entities"][0]["entity_id"]
        identity_drift["scene_entity_registry"] = registry
        identity_report = evaluate_r3_gate(**identity_drift)
        self.assertEqual(identity_report.dimensions["entity_readiness"].status, "red")
        self.assertTrue(
            any("duplicate_entity_id" in item for item in identity_report.blockers)
        )

    def test_snapshot_integrity_rejects_same_version_registry_drift(self) -> None:
        facts = _gate_facts(game_ready_count=8)
        registry = deepcopy(facts["scene_entity_registry"])
        registry["entities"][2]["semantic_role"] = "stale-registry-role"
        facts["scene_entity_registry"] = registry

        report = evaluate_r3_gate(**facts)

        dimension = report.dimensions["snapshot_integrity"]
        self.assertEqual(dimension.status, "red")
        self.assertFalse(dimension.metrics["registry_fingerprint_matches_snapshot"])
        self.assertIn(
            "scene_entity_registry_snapshot_fingerprint_mismatch",
            dimension.contradictions,
        )

    def test_snapshot_integrity_rejects_registry_version_drift(self) -> None:
        facts = _gate_facts(game_ready_count=8)
        registry = deepcopy(facts["scene_entity_registry"])
        registry["scene_version"] = facts["scene_version"] - 1
        facts["scene_entity_registry"] = registry

        report = evaluate_r3_gate(**facts)

        dimension = report.dimensions["snapshot_integrity"]
        self.assertEqual(dimension.status, "red")
        self.assertFalse(dimension.metrics["registry_scene_version_matches"])
        self.assertIn(
            "scene_entity_registry_scene_version_mismatch",
            dimension.contradictions,
        )

    def test_required_environment_recomputes_engine_facts_instead_of_trusting_flag(self) -> None:
        facts = _gate_facts(game_ready_count=8)
        snapshot = deepcopy(facts["snapshot_result"])
        room_floor = snapshot["snapshot"]["environment_entities"][1]
        room_floor["game_ready"] = True
        room_floor["bounds_source"] = "estimated"
        room_floor["grounding_status"] = "enclosure"
        facts["snapshot_result"] = snapshot

        report = evaluate_r3_gate(**facts)

        dimension = report.dimensions["environment_readiness"]
        self.assertEqual(dimension.status, "red")
        self.assertEqual(dimension.metrics["ready_count"], 1)
        self.assertIn("room_floor:engine_actual_aabb", dimension.contradictions)
        self.assertIn("room_floor:grounding_status", dimension.contradictions)
        diagnostic = next(
            item
            for item in dimension.metrics["component_diagnostics"]
            if item["component_type"] == "room_floor"
        )
        self.assertFalse(diagnostic["ready"])

    def test_required_environment_rejects_duplicate_partial_component_instance(self) -> None:
        facts = _gate_facts(game_ready_count=8)
        snapshot = deepcopy(facts["snapshot_result"])
        duplicate_floor = deepcopy(snapshot["snapshot"]["environment_entities"][1])
        duplicate_floor["entity_id"] = "entity-room-floor-duplicate"
        duplicate_floor["actor_id"] = "actor-room-floor-duplicate"
        duplicate_floor["bounds_source"] = "estimated"
        duplicate_floor["game_ready"] = False
        duplicate_floor["readiness_missing_fields"] = ["engine_actual_aabb"]
        snapshot["snapshot"]["environment_entities"].append(duplicate_floor)
        snapshot["snapshot"]["world_fingerprint"] = scene_world_fingerprint(
            [
                *snapshot["snapshot"]["environment_entities"],
                *snapshot["snapshot"]["actor_entities"],
            ],
            plan_id=facts["plan_id"],
            scene_version=facts["scene_version"],
        )
        snapshot["world_fingerprint"] = snapshot["snapshot"]["world_fingerprint"]
        facts["snapshot_result"] = snapshot

        report = evaluate_r3_gate(**facts)

        dimension = report.dimensions["environment_readiness"]
        self.assertEqual(dimension.status, "red")
        self.assertEqual(dimension.metrics["ready_count"], 1)
        self.assertIn("environment_not_ready:room_floor", dimension.contradictions)
        self.assertIn("room_floor:engine_actual_aabb", dimension.contradictions)
        diagnostics = [
            item
            for item in dimension.metrics["component_diagnostics"]
            if item["component_type"] == "room_floor"
        ]
        self.assertEqual(len(diagnostics), 2)
        self.assertTrue(any(item["ready"] for item in diagnostics))
        self.assertTrue(any(not item["ready"] for item in diagnostics))

    def test_required_environment_accepts_canonical_component_aliases(self) -> None:
        facts = _gate_facts(game_ready_count=8)
        snapshot = deepcopy(facts["snapshot_result"])
        room_box = snapshot["snapshot"]["environment_entities"][0]
        room_box["component_type"] = "room_shell"
        room_box["semantic_role"] = "indoor_enclosure"
        facts["snapshot_result"] = snapshot

        report = evaluate_r3_gate(**facts)

        dimension = report.dimensions["environment_readiness"]
        self.assertEqual(dimension.status, "green")
        self.assertEqual(dimension.metrics["ready_count"], 2)

    def test_engine_snapshot_from_other_scene_version_is_red_even_when_entities_match(self) -> None:
        facts = _gate_facts(game_ready_count=8)
        snapshot = deepcopy(facts["snapshot_result"]["snapshot"])
        engine_snapshot = {
            "snapshot_id": "engine-old-version",
            "plan_id": facts["plan_id"],
            "scene_version": facts["scene_version"] - 1,
            "actors": [
                dict(entity)
                for entity in (
                    snapshot["environment_entities"] + snapshot["actor_entities"]
                )
            ],
        }
        facts["consistency_audit"] = audit_scene_world_consistency(
            world_snapshot=snapshot,
            engine_snapshot=engine_snapshot,
        )

        report = evaluate_r3_gate(**facts)

        dimension = report.dimensions["snapshot_integrity"]
        self.assertEqual(dimension.status, "red")
        self.assertFalse(dimension.metrics["engine_snapshot_scene_version_matches"])
        self.assertIn(
            "engine_snapshot_scene_version_mismatch",
            dimension.contradictions,
        )

    def test_report_is_deterministic_for_identical_facts(self) -> None:
        facts = _gate_facts(game_ready_count=8)
        first = evaluate_r3_gate(**facts).as_dict()
        second = evaluate_r3_gate(**deepcopy(facts)).as_dict()

        self.assertEqual(first, second)
        self.assertEqual(first["gate_report_id"], second["gate_report_id"])

    def test_demo_requirements_are_derived_from_arbitrary_gameplay_slots(self) -> None:
        requirements = derive_demo_readiness_requirements(
            (
                GameplayEntitySlot("hero", "hero_anchor", ("spawnable",)),
                GameplayEntitySlot("token", "interactable_token", ("collectible", "inspectable")),
            )
        )

        self.assertEqual(
            [item.requirement_id for item in requirements],
            ["demo.slot.hero", "demo.slot.token"],
        )
        self.assertEqual(requirements[0].semantic_role, "hero_anchor")
        self.assertEqual(requirements[1].required_capabilities, ("collectible", "inspectable"))

    def test_single_player_profile_matches_dynamic_game_ready_entities(self) -> None:
        facts = _gate_facts(game_ready_count=14)
        _apply_demo_roles(
            facts,
            [
                ("hero_anchor", ("spawnable",)),
                ("interactable_token", ("collectible", "inspectable")),
            ],
        )
        requirements = derive_demo_readiness_requirements(
            (
                GameplayEntitySlot("hero", "hero_anchor", ("spawnable",)),
                GameplayEntitySlot("token", "interactable_token", ("collectible", "inspectable")),
            )
        )

        report = evaluate_r3_gate(
            **facts,
            profile="single_player_demo",
            demo_requirements=requirements,
            project_mode="single_player",
        )

        self.assertEqual(report.overall, "green")
        self.assertEqual(
            report.capability_unlocks,
            (
                "single_player_entity_binding",
                "single_player_local_action",
                "single_player_preview",
            ),
        )
        self.assertEqual(report.metrics["gate_profile"], "single_player_demo")
        self.assertTrue(report.metrics["requirements_fingerprint"].startswith("sha256:"))
        matches = report.metrics["requirement_matches"]
        self.assertEqual(matches["demo.slot.hero"], ["entity-02"])
        self.assertEqual(matches["demo.slot.token"], ["entity-03"])
        self.assertEqual(report.dimensions["multiplayer_consistency"].status, "green")

    def test_single_player_profile_fails_closed_on_missing_capability(self) -> None:
        facts = _gate_facts(game_ready_count=14)
        _apply_demo_roles(facts, [("interactable_token", ("inspectable",))])
        requirement = DemoReadinessRequirement(
            requirement_id="demo.slot.token",
            semantic_role="interactable_token",
            required_capabilities=("collectible",),
            min_count=1,
        )

        report = evaluate_r3_gate(
            **facts,
            profile="single_player_demo",
            demo_requirements=(requirement,),
            project_mode="single_player",
        )

        self.assertEqual(report.overall, "red")
        self.assertIn(
            "entity_readiness:demo_requirement:demo.slot.token",
            report.blockers,
        )
        self.assertNotIn("single_player_local_action", report.capability_unlocks)

    def test_single_player_requirements_cannot_reuse_one_entity(self) -> None:
        facts = _gate_facts(game_ready_count=14)
        _apply_demo_roles(facts, [("shared_anchor", ("activatable",))])
        requirements = (
            DemoReadinessRequirement("demo.slot.first", "shared_anchor", ("activatable",)),
            DemoReadinessRequirement("demo.slot.second", "shared_anchor", ("activatable",)),
        )

        report = evaluate_r3_gate(
            **facts,
            profile="single_player_demo",
            demo_requirements=requirements,
            project_mode="single_player",
        )

        self.assertEqual(report.overall, "red")
        matches = report.metrics["requirement_matches"]
        self.assertEqual(sum(len(value) for value in matches.values()), 1)

    def test_explicit_full_r3_profile_preserves_full_gate_result(self) -> None:
        facts = _gate_facts(game_ready_count=8)
        implicit = evaluate_r3_gate(**facts)
        explicit = evaluate_r3_gate(**deepcopy(facts), profile="full_r3")

        self.assertEqual(implicit.overall, explicit.overall)
        self.assertEqual(implicit.capability_unlocks, explicit.capability_unlocks)
        self.assertEqual(
            {name: dimension.status for name, dimension in implicit.dimensions.items()},
            {name: dimension.status for name, dimension in explicit.dimensions.items()},
        )

    def test_public_runtime_action_is_read_only_and_does_not_create_room(self) -> None:
        runtime = AgentRuntime()
        before_rooms = deepcopy(runtime.state.rooms)
        before_version = runtime.state.version
        before_log = [entry.as_dict() for entry in runtime.operation_log.entries()]

        first = runtime.handle_message(
            room_id="room-missing",
            text="",
            action="runtime.r3_readiness.evaluate",
        )
        second = runtime.handle_message(
            room_id="room-missing",
            text="",
            action="runtime.r3_readiness.evaluate",
        )

        self.assertTrue(first["handled"])
        self.assertFalse(first["recorded"])
        self.assertEqual(first["gate_report"]["overall"], "red")
        self.assertEqual(first["gate_report"], second["gate_report"])
        self.assertEqual(runtime.state.rooms, before_rooms)
        self.assertEqual(runtime.state.version, before_version)
        self.assertEqual(
            [entry.as_dict() for entry in runtime.operation_log.entries()],
            before_log,
        )
        policy = runtime.message_action_policy("runtime.r3_readiness.evaluate")
        self.assertTrue(policy["read_only"])
        self.assertFalse(policy["may_create_plan"])

    def test_single_player_runtime_action_is_read_only_without_snapshot(self) -> None:
        runtime = AgentRuntime()
        before_rooms = deepcopy(runtime.state.rooms)
        before_version = runtime.state.version
        before_log = [entry.as_dict() for entry in runtime.operation_log.entries()]

        result = runtime.handle_message(
            room_id="room-missing-single-player",
            text="",
            action="runtime.r3_readiness.evaluate",
            sync_event={
                "profile": "single_player_demo",
                "project_mode": "single_player",
                "demo_requirements": [
                    DemoReadinessRequirement(
                        "demo.slot.hero",
                        "hero_anchor",
                        ("spawnable",),
                    ).as_dict()
                ],
            },
        )

        self.assertEqual(result["gate_report"]["overall"], "red")
        self.assertEqual(result["gate_report"]["metrics"]["gate_profile"], "single_player_demo")
        self.assertEqual(runtime.state.rooms, before_rooms)
        self.assertEqual(runtime.state.version, before_version)
        self.assertEqual(
            [entry.as_dict() for entry in runtime.operation_log.entries()],
            before_log,
        )

    def test_public_runtime_action_does_not_mutate_existing_room(self) -> None:
        runtime = AgentRuntime()
        room = runtime.state.room("room-existing")
        room["latest_completed_plan_id"] = "plan-existing"
        room["scene_plans"] = {
            "plan-existing": {
                "plan_id": "plan-existing",
                "room_id": "room-existing",
                "title": "bedroom",
                "status": "completed",
                "version": 1,
            }
        }
        before_room = deepcopy(room)
        before_version = runtime.state.version
        before_log = [entry.as_dict() for entry in runtime.operation_log.entries()]

        result = runtime.handle_message(
            room_id="room-existing",
            text="",
            action="runtime.r3_readiness.evaluate",
        )

        self.assertTrue(result["found"])
        self.assertEqual(result["gate_report"]["overall"], "red")
        self.assertEqual(runtime.state.rooms["room-existing"], before_room)
        self.assertEqual(runtime.state.version, before_version)
        self.assertEqual(
            [entry.as_dict() for entry in runtime.operation_log.entries()],
            before_log,
        )


if __name__ == "__main__":
    unittest.main()
