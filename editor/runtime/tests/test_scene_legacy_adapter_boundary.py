from pathlib import Path
import sys

import pytest


CORE_ROOT = Path(__file__).resolve().parents[1].parent / "CoronaCore" / "core"
REPO_ROOT = CORE_ROOT.parents[2]
sys.path.insert(0, str(REPO_ROOT / "editor"))

from runtime.legacy.entities import scene as scene_module
from runtime.legacy.entities.scene import NativeCameraRecord, NativeSceneRecord, Scene


def test_scene_legacy_entity_routes_editor_operations_through_adapters():
    source = (REPO_ROOT / "editor/runtime/legacy/entities/scene.py").read_text(
        encoding="utf-8"
    )

    assert "from ...engine_runtime import CoronaEngine" not in source
    assert "get_scene_adapter" in source
    assert "get_scene_tools_adapter" in source
    assert "get_viewport_adapter" in source


def test_native_scene_record_reads_bounds_from_scene_value_object(monkeypatch):
    class SceneAdapter:
        def get_snapshot(self, route):
            assert route == "Scene/level.scene"
            return {"status": "success", "scene_aabb": [-1, -2, -3, 4, 5, 6]}

    monkeypatch.setattr(scene_module, "get_scene_adapter", lambda: SceneAdapter())
    record = NativeSceneRecord("Scene/level.scene")

    assert record.get_aabb() == [-1, -2, -3, 4, 5, 6]


def test_scene_preserves_missing_native_host_failure_semantics(monkeypatch):
    monkeypatch.setattr(scene_module, "is_native_engine_available", lambda: False)

    with pytest.raises(RuntimeError, match="CoronaEngine 未初始化"):
        Scene("Scene/level.scene")


def test_native_camera_record_uses_viewport_value_object_for_screenshot(monkeypatch):
    calls = []

    class ViewportAdapter:
        def capture(self, scene, camera, payload, output_path):
            calls.append((scene, camera, payload, output_path))
            return {"status": "success"}

    monkeypatch.setattr(scene_module, "get_viewport_adapter", lambda: ViewportAdapter())
    camera = NativeCameraRecord("Scene/level.scene", "MainCamera")

    assert camera.save_screenshot_sync("capture.png") is True
    assert calls[0][0:2] == ("Scene/level.scene", "MainCamera")
    assert calls[0][2]["position"] == [0.0, 0.0, -5.0]
    assert calls[0][3] == "capture.png"


def test_scene_add_actor_uses_scene_tools_value_object(monkeypatch):
    calls = []

    class SceneToolsAdapter:
        def create_actor(self, scene, source, actor_type, actor_data):
            calls.append((scene, source, actor_type, actor_data))
            return {"status": "success", "actor": {"name": "Created", "actor_guid": "g-1"}}

    class Actor:
        route = "Assets/box.actor"
        actor_type = "model"
        name = "Box"
        actor_guid = "old"

        @staticmethod
        def get_position():
            return [1, 2, 3]

        @staticmethod
        def get_rotation():
            return [0, 0, 0]

        @staticmethod
        def get_scale():
            return [1, 1, 1]

    scene = Scene.__new__(Scene)
    scene.route = "Scene/level.scene"
    scene.name = "level"
    scene.save_data = lambda: None
    monkeypatch.setattr(scene_module, "get_scene_tools_adapter", lambda: SceneToolsAdapter())
    monkeypatch.setattr(scene_module, "emit_compat_editor_event", lambda *_args: None)

    assert scene.add_actor(Actor()) is True
    assert calls[0][0:3] == ("Scene/level.scene", "Assets/box.actor", "model")
    assert calls[0][3]["position"] == [1, 2, 3]
    assert calls[0][3]["skip_if_exists"] is True
