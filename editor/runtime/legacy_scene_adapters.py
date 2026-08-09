"""Compatibility adapters for pre-manifest scene and viewport bindings."""

import json


class _LegacySceneToolsAdapter:
    """Compatibility view for pre-manifest SceneTools bindings."""

    def __init__(self, native_engine):
        self._native_engine = native_engine

    def create_actor(self, scene_name, source_path, actor_type, actor_data):
        creator = getattr(self._native_engine, "create_editor_actor", None)
        if not callable(creator):
            raise RuntimeError("legacy create_editor_actor binding is unavailable")
        return creator(
            scene_name,
            source_path,
            actor_type,
            json.dumps(actor_data, ensure_ascii=False),
        )

    def remove_actor(self, scene_name, actor_name):
        remover = getattr(self._native_engine, "remove_editor_actor", None)
        if not callable(remover):
            raise RuntimeError("legacy remove_editor_actor binding is unavailable")
        return remover(scene_name, actor_name)


class _LegacySceneAdapter:
    """Compatibility view for pre-manifest editor scene bindings."""

    def __init__(self, native_engine):
        self._native_engine = native_engine

    def get_snapshot(self, scene_name=""):
        getter = getattr(self._native_engine, "get_editor_scene_snapshot", None)
        if not callable(getter):
            raise RuntimeError("legacy get_editor_scene_snapshot binding is unavailable")
        return getter(scene_name or "")

    def set_actor_transform(self, scene_name, actor_name, transform):
        setter = getattr(self._native_engine, "set_editor_actor_transform", None)
        if not callable(setter):
            raise RuntimeError("legacy set_editor_actor_transform binding is unavailable")
        return setter(
            scene_name,
            actor_name,
            json.dumps(transform, ensure_ascii=False),
        )

    def get_actor_bounds(self, scene_name, actor_name):
        getter = getattr(self._native_engine, "get_editor_actor_bounds", None)
        if not callable(getter):
            raise RuntimeError("legacy get_editor_actor_bounds binding is unavailable")
        return getter(scene_name, actor_name)


class _LegacyViewportAdapter:
    """Compatibility view for pre-manifest native viewport bindings."""

    def __init__(self, native_engine):
        self._native_engine = native_engine

    def capture(self, scene_name, camera_name, camera, output_path):
        capture = getattr(self._native_engine, "capture_editor_camera_view", None)
        if not callable(capture):
            raise RuntimeError("legacy capture_editor_camera_view binding is unavailable")
        return capture(
            scene_name,
            camera_name,
            json.dumps(camera, ensure_ascii=False),
            output_path,
        )

    def set_camera_pose(self, scene_name, camera_name, camera):
        setter = getattr(self._native_engine, "set_editor_camera_transform", None)
        if not callable(setter):
            raise RuntimeError("legacy set_editor_camera_transform binding is unavailable")
        return setter(
            scene_name,
            camera_name,
            json.dumps(camera, ensure_ascii=False),
        )
