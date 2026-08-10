from __future__ import annotations

from copy import deepcopy
import unittest

from editor.plugins.AITool.services.agent_runtime import (
    make_current_unversioned_v1_scene_snapshot_reader,
)
from editor.plugins.AITool.services.agent_runtime.engine_snapshot_input import (
    CURRENT_UNVERSIONED_V1_ENGINE_BUILD_FINGERPRINT,
)
from editor.plugins.AITool.services.engine_adapter_reconciliation import (
    CurrentEngineAdapterReconciler,
)
from editor.plugins.AITool.services.schema_versions import (
    ENGINE_ADAPTER_CONTRACT_VERSION,
    SCENE_WORLD_SNAPSHOT_SCHEMA_VERSION,
)
from editor.plugins.AITool.services.tests.test_agent_runtime_current_snapshot_fixture import (
    _current_snapshot_fixture,
)


class _SnapshotTool:
    def __init__(self, result: dict) -> None:
        self.result = result
        self.calls: list[dict] = []

    def invoke(self, payload: dict):  # noqa: ANN201
        self.calls.append(dict(payload))
        return deepcopy(self.result)


def _capability_manifest(*, operations: tuple[str, ...] | None = None) -> dict:
    return {
        "contract_version": ENGINE_ADAPTER_CONTRACT_VERSION,
        "bridge_version": "native-bridge-current",
        "snapshot_schema_version": SCENE_WORLD_SNAPSHOT_SCHEMA_VERSION,
        "supported_operations": list(operations or (
            "scene_snapshot.read",
            "actual_aabb",
            "render_ready",
        )),
        "supported_gameplay_primitives": [],
    }


def _reconciler(
    tool: _SnapshotTool,
    *,
    build_fingerprint: str = CURRENT_UNVERSIONED_V1_ENGINE_BUILD_FINGERPRINT,
    manifest: dict | None = None,
) -> CurrentEngineAdapterReconciler:
    reader = make_current_unversioned_v1_scene_snapshot_reader(
        snapshot_tool=tool,
        build_fingerprint=build_fingerprint,
        scene_name="Scene/native-default.scene",
    )
    return CurrentEngineAdapterReconciler(
        manifest_reader=lambda: manifest or _capability_manifest(),
        snapshot_reader=reader,
    )


class CurrentEngineAdapterReconciliationTests(unittest.TestCase):
    def test_current_build_reconciles_capability_identity_and_actual_facts(self) -> None:
        tool = _SnapshotTool(_current_snapshot_fixture())

        result = _reconciler(tool).reconcile(
            room_id="room-demo",
            scene_name="semantic bedroom",
            scene_route="Scene/demo.scene",
        )

        self.assertEqual(result.status, "accepted")
        self.assertFalse(result.executable)
        self.assertEqual(result.plan_id, "plan-demo-001")
        self.assertEqual(result.scene_version, 3)
        self.assertEqual(result.snapshot["actor_count"], 1)
        actor = result.snapshot["actors"][0]
        self.assertEqual(actor["actor_id"], "actor-key-001")
        self.assertEqual(actor["entity_id"], "entity-key-001")
        self.assertEqual(actor["bounds_source"], "engine_actual")
        self.assertTrue(actor["render_status_observed"])
        self.assertTrue(actor["render_ready"])
        self.assertEqual(
            tool.calls,
            [{"scene_name": "Scene/demo.scene", "wait_for_bounds": True}],
        )

    def test_missing_observation_capability_blocks_before_snapshot_read(self) -> None:
        tool = _SnapshotTool(_current_snapshot_fixture())
        result = _reconciler(
            tool,
            manifest=_capability_manifest(operations=("scene_snapshot.read",)),
        ).reconcile(room_id="room-demo")

        self.assertEqual(result.status, "blocked")
        self.assertEqual(result.blocked_result.error_code, "engine_observation_capability_missing")
        self.assertEqual(tool.calls, [])

    def test_unknown_field_and_build_drift_fail_closed(self) -> None:
        drifted = _current_snapshot_fixture()
        drifted["input_dto_version"] = "unexpected-v2"
        cases = (
            (
                _SnapshotTool(drifted),
                CURRENT_UNVERSIONED_V1_ENGINE_BUILD_FINGERPRINT,
                "engine_snapshot_field_set_mismatch",
            ),
            (
                _SnapshotTool(_current_snapshot_fixture()),
                "other-engine-build",
                "engine_snapshot_build_fingerprint_mismatch",
            ),
        )
        for tool, build_fingerprint, expected_error in cases:
            with self.subTest(expected_error=expected_error):
                result = _reconciler(
                    tool,
                    build_fingerprint=build_fingerprint,
                ).reconcile(room_id="room-demo")
                self.assertEqual(result.status, "blocked")
                self.assertEqual(result.blocked_result.error_code, expected_error)
                self.assertIsNone(result.snapshot)

    def test_plan_scene_version_and_entity_identity_drift_are_blocked(self) -> None:
        for mutation, expected_error in (
            ("plan", "engine_snapshot_plan_identity_drift"),
            ("version", "engine_snapshot_scene_version_drift"),
            ("entity", "engine_snapshot_actor_identity_drift"),
        ):
            with self.subTest(mutation=mutation):
                fixture = _current_snapshot_fixture()
                second = deepcopy(fixture["actors"][0])
                second["name"] = "Door"
                second["actor_guid"] = "actor-door-001"
                second["entity_id"] = "entity-door-001"
                second["asset_id"] = "asset-door-001"
                second["model_ref"] = "Assets/door.glb"
                if mutation == "plan":
                    second["source_plan_id"] = "plan-other-001"
                elif mutation == "version":
                    second["source_scene_version"] = 4
                else:
                    second["entity_id"] = fixture["actors"][0]["entity_id"]
                fixture["actors"].append(second)
                fixture["actor_count"] = 2

                result = _reconciler(_SnapshotTool(fixture)).reconcile(room_id="room-demo")

                self.assertEqual(result.status, "blocked")
                self.assertEqual(result.blocked_result.error_code, expected_error)


if __name__ == "__main__":
    unittest.main()
