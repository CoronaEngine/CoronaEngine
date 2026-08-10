from __future__ import annotations

from copy import deepcopy
import unittest

from editor.plugins.AITool.services.agent_runtime import (
    normalize_current_unversioned_v1_scene_snapshot,
)
from editor.plugins.AITool.services.agent_runtime.engine_snapshot_input import (
    CURRENT_UNVERSIONED_V1_ENGINE_BUILD_FINGERPRINT,
    EngineSnapshotInputContractError,
    current_unversioned_v1_schema_fingerprint,
)
from editor.plugins.AITool.services.schema_versions import (
    ENGINE_SNAPSHOT_INPUT_CONTRACT_VERSION,
)


def _camera() -> dict:
    return {
        "id": "camera-main",
        "camera_id": "camera-main",
        "name": "MainCamera",
        "handle": 101,
        "position": [0.0, 2.0, -5.0],
        "forward": [0.0, 0.0, 1.0],
        "world_up": [0.0, 1.0, 0.0],
        "fov": 45.0,
        "width": 1280,
        "height": 720,
        "output_mode": "final_color",
        "render_backend": "native",
        "vision_render_mode": "path_tracing",
        "vision_spp": 1,
        "vision_max_depth": 4,
        "vision_denoise": False,
        "shadow_cascade_debug": False,
        "ssao_enabled": True,
        "move_speed": 5.0,
        "view_open": True,
        "view_x": 0,
        "view_y": 0,
        "view_width": 1280,
        "view_height": 720,
        "deletable": False,
    }


def _current_snapshot_fixture() -> dict:
    camera = _camera()
    actor = {
        "name": "Key",
        "actor_guid": "actor-key-001",
        "handle": 201,
        "path": "Assets/key.glb",
        "route": "Assets/key.glb",
        "scene": "Scene/demo.scene",
        "type": ".glb",
        "model": "Assets/key.glb",
        "model_dependencies": [],
        "actor_type": "model",
        "entity_id": "entity-key-001",
        "asset_id": "asset-key-001",
        "model_ref": "Assets/key.glb",
        "entity_type": "collectible",
        "semantic_role": "collectible_key",
        "source_plan_id": "plan-demo-001",
        "source_batch_id": "batch-demo-001",
        "source_scene_version": 3,
        "actor_version": 2,
        "version": 2,
        "collision": "box",
        "visible": True,
        "script": "",
        "follow_camera": False,
        "render_space": "scene",
        "geometry": {
            "position": [0.0, 0.5, 0.0],
            "rotation": [0.0, 0.0, 0.0],
            "scale": [1.0, 1.0, 1.0],
        },
        "local_aabb": [-0.2, -0.1, -0.2, 0.2, 0.1, 0.2],
        "world_aabb": [-0.2, 0.4, -0.2, 0.2, 0.6, 0.2],
        "aabb": [-0.2, 0.4, -0.2, 0.2, 0.6, 0.2],
        "bounds_ready": True,
        "render_status_observed": True,
        "render_ready": True,
        "render_failed": False,
        "gpu_build_state": "Ready",
        "mesh_count": 1,
        "renderable_mesh_count": 1,
        "invalid_mesh_count": 0,
        "size": [0.4, 0.2, 0.4],
        "camera_lock": {
            "lock_to_camera": False,
            "position_offset": [0.0, 0.0, 2.0],
            "rotation_offset": [0.0, 0.0, 0.0],
        },
    }
    return {
        "status": "success",
        "scene": "Scene/demo.scene",
        "scene_name": "demo",
        "actor_count": 1,
        "actors": [actor],
        "active_camera_id": "camera-main",
        "active_camera_name": "MainCamera",
        "camera": camera,
        "cameras": [deepcopy(camera)],
        "scene_aabb": [-5.0, 0.0, -5.0, 5.0, 3.0, 5.0],
        "bounds_ready": True,
    }


class CurrentUnversionedV1SnapshotFixtureTests(unittest.TestCase):
    def test_exact_current_fixture_normalizes_with_auditable_contract_identity(self) -> None:
        first_fingerprint = current_unversioned_v1_schema_fingerprint()
        second_fingerprint = current_unversioned_v1_schema_fingerprint()

        result = normalize_current_unversioned_v1_scene_snapshot(
            _current_snapshot_fixture(),
            room_id="room-demo",
            scene_name="Scene/demo.scene",
            build_fingerprint=CURRENT_UNVERSIONED_V1_ENGINE_BUILD_FINGERPRINT,
        )

        self.assertEqual(result["input_contract_version"], ENGINE_SNAPSHOT_INPUT_CONTRACT_VERSION)
        self.assertEqual(result["build_fingerprint"], CURRENT_UNVERSIONED_V1_ENGINE_BUILD_FINGERPRINT)
        self.assertEqual(result["schema_fingerprint"], first_fingerprint)
        self.assertEqual(first_fingerprint, second_fingerprint)
        self.assertTrue(first_fingerprint.startswith("sha256:"))
        snapshot = result["snapshot"]
        self.assertEqual(snapshot["actor_count"], 1)
        self.assertEqual(snapshot["actors"][0]["actor_id"], "actor-key-001")
        self.assertEqual(snapshot["actors"][0]["entity_id"], "entity-key-001")
        self.assertEqual(snapshot["actors"][0]["bounds_source"], "engine_actual")
        self.assertEqual(snapshot["actors"][0]["aabb"], [-0.2, 0.4, -0.2, 0.2, 0.6, 0.2])

    def test_build_fingerprint_mismatch_fails_closed(self) -> None:
        with self.assertRaises(EngineSnapshotInputContractError) as caught:
            normalize_current_unversioned_v1_scene_snapshot(
                _current_snapshot_fixture(),
                room_id="room-demo",
                scene_name="Scene/demo.scene",
                build_fingerprint="unknown-engine-build",
            )

        self.assertEqual(caught.exception.error_code, "engine_snapshot_build_fingerprint_mismatch")

    def test_unknown_top_level_or_actor_field_fails_closed(self) -> None:
        for mutation in ("top_level", "actor"):
            with self.subTest(mutation=mutation):
                fixture = _current_snapshot_fixture()
                if mutation == "top_level":
                    fixture["input_dto_version"] = "unapproved-v2"
                    expected = "engine_snapshot_field_set_mismatch"
                else:
                    fixture["actors"][0]["guessed_runtime_field"] = True
                    expected = "engine_snapshot_actor_field_set_mismatch"
                with self.assertRaises(EngineSnapshotInputContractError) as caught:
                    normalize_current_unversioned_v1_scene_snapshot(
                        fixture,
                        room_id="room-demo",
                        scene_name="Scene/demo.scene",
                        build_fingerprint=CURRENT_UNVERSIONED_V1_ENGINE_BUILD_FINGERPRINT,
                    )
                self.assertEqual(caught.exception.error_code, expected)

    def test_missing_actual_aabb_fails_closed_without_alias_guessing(self) -> None:
        fixture = _current_snapshot_fixture()
        fixture["actors"][0]["world_aabb"] = None
        fixture["actors"][0]["aabb"] = None
        fixture["actors"][0]["bounds_ready"] = False

        with self.assertRaises(EngineSnapshotInputContractError) as caught:
            normalize_current_unversioned_v1_scene_snapshot(
                fixture,
                room_id="room-demo",
                scene_name="Scene/demo.scene",
                build_fingerprint=CURRENT_UNVERSIONED_V1_ENGINE_BUILD_FINGERPRINT,
            )

        self.assertEqual(caught.exception.error_code, "engine_snapshot_actual_fact_missing")

    def test_missing_stable_entity_identity_fails_closed(self) -> None:
        fixture = _current_snapshot_fixture()
        fixture["actors"][0]["entity_id"] = ""

        with self.assertRaises(EngineSnapshotInputContractError) as caught:
            normalize_current_unversioned_v1_scene_snapshot(
                fixture,
                room_id="room-demo",
                scene_name="Scene/demo.scene",
                build_fingerprint=CURRENT_UNVERSIONED_V1_ENGINE_BUILD_FINGERPRINT,
            )

        self.assertEqual(caught.exception.error_code, "engine_snapshot_actor_identity_missing")


if __name__ == "__main__":
    unittest.main()
