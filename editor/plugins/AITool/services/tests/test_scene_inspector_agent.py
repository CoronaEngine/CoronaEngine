from __future__ import annotations

import unittest

from editor.plugins.AITool.services.agent_runtime import AgentRuntime, StatePatch
from editor.plugins.AITool.services.scene_inspector_agent import SceneInspectorAgent


def _scene_state() -> dict:
    base = {
        "plan_id": "plan-inspect",
        "batch_id": "batch-inspect",
        "position": [0.0, 0.0, 0.0],
        "rotation": [0.0, 0.0, 0.0],
        "scale": [1.0, 1.0, 1.0],
        "bounds_source": "engine_actual",
        "bounds_ready": True,
        "engine_lifecycle_status": "bounds_ready",
        "sync_status": "engine_created",
        "source": "engine_actor_import",
        "status": "success",
        "grounding_status": "grounded",
    }
    return {
        "latest_completed_plan_id": "plan-inspect",
        "scene_plans": {
            "plan-inspect": {
                "plan_id": "plan-inspect",
                "room_id": "room-inspect",
                "title": "inspect",
                "status": "completed",
                "version": 4,
                "concrete_object_items": ["door", "chair"],
            }
        },
        "batch_plans": {
            "batch-inspect": {
                "batch_id": "batch-inspect",
                "plan_id": "plan-inspect",
                "room_id": "room-inspect",
                "status": "completed",
            }
        },
        "actors": {
            "actor-door": {
                **base,
                "name": "door",
                "asset_id": "asset-door",
                "model_ref": "door.obj",
                "aabb": {"min": [-0.5, 0.0, -0.1], "max": [0.5, 2.0, 0.1]},
                "interaction_capability": ["open"],
            },
            "actor-chair": {
                **base,
                "name": "chair",
                "asset_id": "asset-chair",
                "model_ref": "chair.obj",
                "aabb": {"min": [-0.5, 0.0, -0.5], "max": [0.5, 1.0, 0.5]},
                "interaction_capability": [],
            },
        },
    }


class SceneInspectorAgentTests(unittest.TestCase):
    def test_inspector_consumes_snapshot_without_scene_writes(self) -> None:
        runtime = AgentRuntime()
        applied, _ = runtime.state.apply_patch(StatePatch(room_id="room-inspect", changes=_scene_state()))
        self.assertTrue(applied)
        before = runtime.state.room("room-inspect")
        operation_count_before = len(runtime.operation_log.entries())

        result = SceneInspectorAgent(runtime).analyze(room_id="room-inspect")

        after = runtime.state.room("room-inspect")
        self.assertTrue(result["available"])
        self.assertEqual(result["analysis"]["scene_version"], 4)
        self.assertEqual(len(result["analysis"]["world_fingerprint"]), 64)
        self.assertTrue(all(item["version"] == 4 for item in result["analysis"]["entity_summary"]))
        self.assertEqual(
            [item["name"] for item in result["analysis"]["interaction_candidates"]],
            ["door"],
        )
        self.assertEqual(before.get("tool_graphs", {}), after.get("tool_graphs", {}))
        self.assertEqual(before.get("pending_interventions", {}), after.get("pending_interventions", {}))
        self.assertEqual(len(runtime.operation_log.entries()), operation_count_before)

    def test_inspector_does_not_reuse_an_older_scene_version(self) -> None:
        runtime = AgentRuntime()
        applied, _ = runtime.state.apply_patch(StatePatch(room_id="room-inspect", changes=_scene_state()))
        self.assertTrue(applied)
        inspector = SceneInspectorAgent(runtime)

        first = inspector.analyze(room_id="room-inspect")
        changed = _scene_state()
        changed["scene_plans"]["plan-inspect"]["version"] = 5
        changed["actors"]["actor-table"] = {
            **changed["actors"]["actor-chair"],
            "name": "table",
            "asset_id": "asset-table",
            "model_ref": "table.obj",
        }
        applied, _ = runtime.state.apply_patch(StatePatch(room_id="room-inspect", changes=changed))
        self.assertTrue(applied)

        second = inspector.analyze(room_id="room-inspect", min_version=5)

        self.assertEqual(first["analysis"]["scene_version"], 4)
        self.assertEqual(second["analysis"]["scene_version"], 5)
        self.assertNotEqual(
            first["analysis"]["world_fingerprint"],
            second["analysis"]["world_fingerprint"],
        )
        self.assertEqual(len(second["analysis"]["entity_summary"]), 3)
        self.assertTrue(all(item["version"] == 5 for item in second["analysis"]["entity_summary"]))


if __name__ == "__main__":
    unittest.main()
