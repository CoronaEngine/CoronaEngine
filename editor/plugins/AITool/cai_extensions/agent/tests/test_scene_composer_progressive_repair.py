from __future__ import annotations

import json
from pathlib import Path
import sys

EDITOR_DIR = Path(__file__).resolve().parents[5]
AITOOL_DIR = EDITOR_DIR / "plugins" / "AITool"
for candidate in (EDITOR_DIR, AITOOL_DIR):
    if str(candidate) not in sys.path:
        sys.path.insert(0, str(candidate))

from plugins.AITool.cai_extensions.agent.scene_composer_progressive import (
    _repair_recent_imports,
    _runtime_status_to_scene_mode,
)
from plugins.AITool.cai_extensions.data_model.layout import LayoutInstance, SceneLayout
from plugins.AITool.cai_extensions.mcp.tools import native_scene_state


class DelayedBoundsEngine:
    def __init__(self, ready_after: int = 25) -> None:
        self.calls = 0
        self.transform_calls = []
        self.ready_after = ready_after
        self.position = [0.0, 0.0, 0.0]

    def get_editor_scene_snapshot(self, scene_name: str) -> str:
        self.calls += 1
        ready = self.calls >= self.ready_after
        actor = {
            "name": "床",
            "actor_guid": "guid-bed",
            "route": "models/bed/base.glb",
            "actor_type": "model",
            "geometry": {"position": list(self.position), "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0]},
            "local_aabb": [-1.0, -0.5, -1.0, 1.0, 0.5, 1.0] if ready else None,
            "world_aabb": [-1.0, self.position[1] - 0.5, -1.0, 1.0, self.position[1] + 0.5, 1.0] if ready else None,
            "bounds_ready": ready,
            "size": [2.0, 1.0, 2.0] if ready else [0.0, 0.0, 0.0],
        }
        return json.dumps({
            "status": "success",
            "scene": "Scene/default.scene",
            "scene_name": "default",
            "actors": [actor],
            "scene_aabb": actor["world_aabb"],
            "bounds_ready": ready,
        })

    def set_editor_actor_transform(self, scene_name: str, actor_name: str, transform_json: str) -> str:
        transform = json.loads(transform_json)
        self.transform_calls.append((scene_name, actor_name, transform))
        self.position = list(transform["geometry"]["position"])
        actor = json.loads(self.get_editor_scene_snapshot(scene_name))["actors"][0]
        return json.dumps({"status": "success", "scene": scene_name, "actor": actor})


class FakeEditorApi:
    def __init__(self, engine: DelayedBoundsEngine) -> None:
        self.engine = engine
        self.scene = self.SceneApi(engine)

    class SceneApi:
        def __init__(self, engine: DelayedBoundsEngine) -> None:
            self.engine = engine

        def get_snapshot(self, scene_name: str) -> str:
            return self.engine.get_editor_scene_snapshot(scene_name)

        def set_actor_transform(self, scene_name: str, actor_name: str, transform: dict) -> str:
            return self.engine.set_editor_actor_transform(
                scene_name,
                actor_name,
                json.dumps(transform),
            )

def setup_function() -> None:
    native_scene_state.EDITOR_API_OVERRIDE = FakeEditorApi(DelayedBoundsEngine())


def teardown_function() -> None:
    native_scene_state.EDITOR_API_OVERRIDE = None


def test_repair_recent_imports_waits_for_delayed_native_bounds() -> None:
    layout = SceneLayout()
    layout.add(LayoutInstance(
        instance_id="床",
        asset_id="床",
        zone_id="default",
        transform={"pos": [0.0, 0.0, 0.0], "rot": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0]},
        provenance="AGENT",
        batch_id="batch-1",
    ))

    repaired = _repair_recent_imports(["床"], layout, None, zone_aabbs={}, door_aabbs={}, issue_sink=[])

    engine = native_scene_state.EDITOR_API_OVERRIDE.engine
    assert repaired == 1
    assert engine.transform_calls
    assert engine.transform_calls[0][2]["geometry"]["position"] == [0.0, 0.52, 0.0]
    assert layout.get("床").transform["pos"] == [0.0, 0.52, 0.0]


def test_runtime_status_to_scene_mode_prefers_runtime_plan_status() -> None:
    assert _runtime_status_to_scene_mode({
        "status": {
            "plan_summary": {"status": "paused"},
            "runtime_command_summary": {"latest_commands": []},
        }
    }) == "PAUSED"


def test_runtime_status_to_scene_mode_reads_latest_runtime_command() -> None:
    assert _runtime_status_to_scene_mode({
        "runtime_command_summary": {
            "latest_commands": [
                {"command": "resume", "new_status": "confirmed"},
                {"command": "pause", "new_status": "paused"},
            ]
        }
    }) == "PAUSED"


def test_runtime_status_to_scene_mode_ignores_active_runtime_status() -> None:
    assert _runtime_status_to_scene_mode({"plan_summary": {"status": "confirmed"}}) == ""
    assert _runtime_status_to_scene_mode({
        "runtime_command_summary": {
            "latest_commands": [
                {"command": "resume", "new_status": "confirmed"},
            ]
        }
    }) == ""
