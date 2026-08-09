"""Restricted manifest adapter for Script Runtime editor state operations.

The adapter reuses editor manifest names and schemas while keeping the
ScriptRuntime caller channel separate from the regular PythonScript channel.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any


@dataclass(frozen=True)
class ScriptRuntimeActorTarget:
    """Value-only actor surface accepted by the script lifecycle."""

    name: str
    script_path: str = ""


@dataclass(frozen=True)
class ScriptRuntimeSceneTarget:
    """Value-only scene surface accepted by ``ScriptsManager``."""

    name: str
    route: str
    script_path: str
    _actors: tuple[ScriptRuntimeActorTarget, ...]


def scene_target_from_snapshot(snapshot: Any) -> ScriptRuntimeSceneTarget:
    """Build a restricted Script Runtime target from a native scene value object."""
    if not isinstance(snapshot, dict):
        raise TypeError("native scene snapshot must be an object")

    route = str(
        snapshot.get("scene")
        or snapshot.get("scene_id")
        or snapshot.get("id")
        or snapshot.get("route")
        or ""
    )
    name = str(snapshot.get("scene_name") or snapshot.get("name") or route)
    script_path = str(snapshot.get("script") or snapshot.get("script_path") or "")
    actors = tuple(
        ScriptRuntimeActorTarget(
            name=str(item.get("name") or item.get("route") or ""),
            script_path=str(item.get("script") or item.get("script_path") or ""),
        )
        for item in snapshot.get("actors", [])
        if isinstance(item, dict)
    )
    return ScriptRuntimeSceneTarget(name, route, script_path, actors)


class ScriptRuntimeSceneAdapter:
    def __init__(self, invoke_manifest):
        self._invoke_manifest = invoke_manifest

    def list_routes(self):
        return self._invoke_manifest("scene.list_routes", [])

    def switch(self, route):
        return self._invoke_manifest("scene.switch", [route])

    def get_snapshot(self, scene_name=""):
        return self._invoke_manifest("scene.get_snapshot", [scene_name])

    def get_environment(self, scene_name):
        return self._invoke_manifest("scene.get_environment", [scene_name])

    def set_environment(self, scene_name, state):
        return self._invoke_manifest("scene.set_environment", [scene_name, state])

    def set_actor_transform(self, scene_name, actor_name, transform):
        return self._invoke_manifest(
            "scene.set_actor_transform", [scene_name, actor_name, transform]
        )


class ScriptRuntimeSceneToolsAdapter:
    def __init__(self, invoke_manifest):
        self._invoke_manifest = invoke_manifest

    def create_actor(self, scene_name, source_path, actor_type, actor_data):
        return self._invoke_manifest(
            "scene_tools.create_actor",
            [scene_name, source_path, actor_type, actor_data],
        )

    def remove_actor(self, scene_name, actor_name):
        return self._invoke_manifest("scene_tools.remove_actor", [scene_name, actor_name])


class ScriptRuntimeViewportAdapter:
    def __init__(self, invoke_manifest):
        self._invoke_manifest = invoke_manifest

    def set_camera_pose(self, scene_name, camera_name, camera):
        return self._invoke_manifest(
            "viewport.set_camera_pose", [scene_name, camera_name, camera]
        )


class ScriptRuntimeEditorApi:
    def __init__(self, invoke_manifest):
        self.scene = ScriptRuntimeSceneAdapter(invoke_manifest)
        self.scene_tools = ScriptRuntimeSceneToolsAdapter(invoke_manifest)
        self.viewport = ScriptRuntimeViewportAdapter(invoke_manifest)
