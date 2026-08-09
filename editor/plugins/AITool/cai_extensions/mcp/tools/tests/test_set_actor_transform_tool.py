from __future__ import annotations

import json
from pathlib import Path
import sys
import unittest

PROJECT_ROOT = Path(__file__).resolve().parents[7]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))
AI_TOOL_ROOT = PROJECT_ROOT / "editor" / "plugins" / "AITool"
if str(AI_TOOL_ROOT) not in sys.path:
    sys.path.insert(0, str(AI_TOOL_ROOT))

from editor.plugins.AITool.cai_extensions.mcp.tools import native_scene_state as state
from editor.plugins.AITool.cai_extensions.mcp.tools.set_actor_transform import _build_set_actor_transform_tool


class FakeEditorApi:
    def __init__(self) -> None:
        self.snapshot = {
            "status": "success",
            "scene": "Scene/default.scene",
            "actors": [
                {
                    "name": "Chest",
                    "actor_guid": "guid-chest",
                    "actor_type": "model",
                    "geometry": {
                        "position": [0.0, 0.0, 0.0],
                        "rotation": [0.0, 0.0, 0.0],
                        "scale": [1.0, 1.0, 1.0],
                    },
                    "world_aabb": [-0.5, 0.0, -0.5, 0.5, 1.0, 0.5],
                    "bounds_ready": True,
                    "size": [1.0, 1.0, 1.0],
                }
            ],
        }

        self.scene = self.SceneApi(self)

    class SceneApi:
        def __init__(self, owner) -> None:
            self.owner = owner

        def get_snapshot(self, scene_name: str) -> dict:
            return self.owner.snapshot

        def set_actor_transform(self, scene_name: str, actor_name: str, transform: dict) -> dict:
            actor = self.owner.snapshot["actors"][0]
            actor["geometry"].update(transform["geometry"])
            return {"status": "success", "scene": scene_name, "actor": actor}


def _payload_from_envelope(raw: str) -> dict:
    envelope = json.loads(raw)
    part = envelope["llm_content"][0]["part"][0]
    return json.loads(part["content_text"])


class SetActorTransformToolTests(unittest.TestCase):
    def setUp(self) -> None:
        state.EDITOR_API_OVERRIDE = FakeEditorApi()

    def tearDown(self) -> None:
        state.EDITOR_API_OVERRIDE = None

    def test_set_actor_transform_tool_returns_engine_transform_sync_identity(self) -> None:
        tool = _build_set_actor_transform_tool(None)
        payload = _payload_from_envelope(
            tool.invoke(
                {
                    "scene_name": "Scene/default.scene",
                    "actor_name": "Chest",
                    "position": [1.0, 0.0, 2.0],
                    "snap_to_ground": False,
                }
            )
        )

        self.assertEqual(payload["actor_id"], "guid-chest")
        self.assertEqual(payload["actor"], "Chest")
        self.assertEqual(payload["position"], [1.0, 0.0, 2.0])
        self.assertEqual(payload["sync_status"], "engine_transformed")
        self.assertEqual(payload["sync_lifecycle_status"], "engine_transformed")
        self.assertEqual(payload["actor_data"]["sync_status"], "engine_transformed")
        self.assertEqual(payload["actor_data"]["sync_lifecycle_status"], "engine_transformed")


if __name__ == "__main__":
    unittest.main()
