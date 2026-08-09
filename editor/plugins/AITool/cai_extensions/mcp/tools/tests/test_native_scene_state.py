from __future__ import annotations

import json
from pathlib import Path
import sys
from unittest import mock

TOOLS_DIR = Path(__file__).resolve().parents[1]
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import native_scene_state as state


def test_native_scene_state_does_not_import_editor_runtime_container() -> None:
    source = (TOOLS_DIR / "native_scene_state.py").read_text(encoding="utf-8")
    assert "from CoronaCore.core.corona_editor import CoronaEditor" not in source


class FakeEditorApi:
    def __init__(self) -> None:
        self.transform_calls = []
        self.physics_calls = []
        self.snapshot = {
            "status": "success",
            "scene": "Scene/default.scene",
            "scene_name": "default",
            "actors": [
                {
                    "name": "Bed",
                    "actor_guid": "guid-bed",
                    "route": "Models/bed.glb",
                    "actor_type": "model",
                    "geometry": {"position": [10, 0, 2], "rotation": [0, 0, 0], "scale": [2, 1, 3]},
                    "local_aabb": [-1, 0, -1, 1, 2, 1],
                    "world_aabb": [8, 0, -1, 12, 2, 5],
                    "bounds_ready": True,
                    "size": [4, 2, 6],
                    "mechanics": {
                        "mass": 2.0,
                        "restitution": 0.1,
                        "damping": 0.9,
                        "physics_enabled": False,
                    },
                },
                {
                    "name": "Loading",
                    "actor_guid": "guid-loading",
                    "route": "Models/loading.glb",
                    "actor_type": "model",
                    "geometry": {"position": [0, 0, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1]},
                    "local_aabb": None,
                    "world_aabb": None,
                    "bounds_ready": False,
                    "size": [0, 0, 0],
                },
            ],
            "scene_aabb": [8, 0, -1, 12, 2, 5],
            "bounds_ready": True,
        }

        self.scene = self.SceneApi(self)
        self.scene_tools = self.scene

    class SceneApi:
        def __init__(self, owner) -> None:
            self.owner = owner

        def get_snapshot(self, scene_name: str) -> dict:
            return self.owner.snapshot

        def set_actor_transform(self, scene_name: str, actor_name: str, transform: dict) -> dict:
            self.owner.transform_calls.append((scene_name, actor_name, transform))
            actor = self.owner.snapshot["actors"][0]
            actor["geometry"].update(transform["geometry"])
            return {"status": "success", "scene": scene_name, "actor": actor}

        def set_actor_physics(self, scene_name: str, actor_name: str, physics: dict) -> dict:
            self.owner.physics_calls.append((scene_name, actor_name, physics))
            return {
                "status": "success",
                "scene": scene_name,
                "actor": self.owner.snapshot["actors"][0],
            }


def setup_function() -> None:
    state.EDITOR_API_OVERRIDE = FakeEditorApi()


def teardown_function() -> None:
    state.EDITOR_API_OVERRIDE = None


def test_snapshot_returns_native_actors_when_python_actor_list_is_empty() -> None:
    snapshot = state.get_native_scene_snapshot("")
    assert snapshot["actors"][0]["name"] == "Bed"
    assert len(state.native_actor_views("")) == 2


def test_actor_view_exposes_world_aabb_and_bounds_ready() -> None:
    bed = state.find_native_actor("", "Bed")
    loading = state.find_native_actor("", "Loading")
    assert bed is not None
    assert loading is not None
    assert bed.bounds_ready is True
    assert bed.get_world_aabb() == [8, 0, -1, 12, 2, 5]
    assert loading.bounds_ready is False


def test_actor_view_exposes_mechanics_as_a_value_object() -> None:
    bed = state.find_native_actor("", "Bed")
    assert bed is not None
    assert bed.mechanics == {
        "mass": 2.0,
        "restitution": 0.1,
        "damping": 0.9,
        "physics_enabled": False,
    }


def test_set_native_actor_transform_calls_native_api() -> None:
    result = state.set_native_actor_transform("Scene/default.scene", "Bed", position=[1, 2, 3])
    assert result["actor"]["geometry"]["position"] == [1.0, 2.0, 3.0]
    api = state.EDITOR_API_OVERRIDE
    assert api.transform_calls[0][1] == "Bed"
    assert api.transform_calls[0][2]["geometry"]["position"] == [1.0, 2.0, 3.0]


def test_set_native_actor_physics_calls_native_api() -> None:
    result = state.set_native_actor_physics(
        "Scene/default.scene",
        "Bed",
        physics={"physics_enabled": True, "damping": 0.9},
    )
    assert result["status"] == "success"
    api = state.EDITOR_API_OVERRIDE
    assert api.physics_calls == [
        (
            "Scene/default.scene",
            "Bed",
            {"physics_enabled": True, "damping": 0.9},
        )
    ]


def test_legacy_scene_resolution_is_centralized_at_adapter_boundary() -> None:
    class FakeScene:
        name = "level"

    class FakeManager:
        def get(self, route):
            return FakeScene() if route == "level.scene" else None

        def list_all(self):
            return ["level.scene"]

    scene = state.get_legacy_scene("level.scene", manager=FakeManager())
    assert isinstance(scene, FakeScene)


def test_scene_resolution_prefers_native_route_value_object() -> None:
    with mock.patch("CoronaCore.core.editor_api.get_scene_adapter", return_value=object()):
        scene = state.resolve_scene_value("", manager=object())

    assert scene.route == "Scene/default.scene"
    assert scene.name == "default"


def test_scene_resolution_uses_legacy_manager_when_native_adapter_is_unavailable() -> None:
    class FakeScene:
        name = "level"

    class FakeManager:
        def get(self, route):
            return FakeScene() if route == "level.scene" else None

        def list_all(self):
            return ["level.scene"]

    with mock.patch("CoronaCore.core.editor_api.get_scene_adapter", return_value=None):
        scene = state.resolve_scene_value("level.scene", manager=FakeManager())

    assert isinstance(scene, FakeScene)


def test_actor_views_with_legacy_fallback_prefers_native_views() -> None:
    native_actor = object()
    with mock.patch.object(state, "native_actor_views", return_value=[native_actor]):
        result = state.native_actor_views_with_legacy_fallback(manager=object())

    assert result == [native_actor]


def test_actor_views_with_legacy_fallback_reads_legacy_scene_only_on_native_failure() -> None:
    legacy_actor = object()

    class FakeScene:
        def get_actors(self):
            return [legacy_actor]

    class FakeManager:
        def get(self, route):
            return FakeScene() if route == "" else None

        def list_all(self):
            return [""]

    with mock.patch.object(state, "native_actor_views", side_effect=RuntimeError("native unavailable")):
        result = state.native_actor_views_with_legacy_fallback(manager=FakeManager())

    assert result == [legacy_actor]


def test_find_actor_with_legacy_fallback_prefers_native_actor_view() -> None:
    native_actor = object()
    with mock.patch.object(state, "find_native_actor", return_value=native_actor), mock.patch.object(
        state, "get_legacy_scene"
    ) as legacy_lookup:
        result = state.find_actor_with_legacy_fallback("Scene/default.scene", "Bed")

    assert result is native_actor
    legacy_lookup.assert_not_called()


def test_find_actor_with_legacy_fallback_uses_centralized_legacy_lookup() -> None:
    legacy_actor = object()
    legacy_scene = mock.Mock()
    legacy_scene.find_actor.return_value = legacy_actor
    with mock.patch.object(state, "find_native_actor", side_effect=RuntimeError("native unavailable")), mock.patch.object(
        state, "get_legacy_scene", return_value=legacy_scene
    ) as legacy_lookup:
        result = state.find_actor_with_legacy_fallback("Scene/default.scene", "Bed")

    assert result is legacy_actor
    legacy_lookup.assert_called_once_with("Scene/default.scene", manager=None)


def test_set_actor_physics_value_uses_native_value_object_setter() -> None:
    actor = mock.Mock()

    assert state.set_actor_physics_value(actor, {"physics_enabled": False})

    actor.set_mechanics.assert_called_once_with({"physics_enabled": False})


def test_set_actor_physics_value_updates_legacy_mechanics_only_at_adapter_boundary() -> None:
    mechanics = mock.Mock()
    actor = mock.Mock(set_mechanics=None, _mechanics=mechanics)

    assert state.set_actor_physics_value(
        actor,
        {"physics_enabled": True, "damping": 0.9},
    )

    mechanics.set_physics_enabled.assert_called_once_with(True)
    mechanics.set_damping.assert_called_once_with(0.9)
