"""Restricted manifest adapter for Script Runtime editor state operations.

The adapter reuses editor manifest names and schemas while keeping the
ScriptRuntime caller channel separate from the regular PythonScript channel.
"""

from __future__ import annotations

class ScriptRuntimeSceneAdapter:
    def __init__(self, invoke_manifest):
        self._invoke_manifest = invoke_manifest


    def get_snapshot(self, scene_name=""):
        return self._invoke_manifest("scene.get_snapshot", [scene_name])

    def read_active(self):
        return self._invoke_manifest("scene.read_active_scene", [])

    def read_actor(self, session, actor_guid):
        return self._invoke_manifest("scene.read_actor", [{"archive_session": session, "actor": actor_guid}])

    def read_transforms(self, session, actor_guids):
        return self._invoke_manifest("scene.read_transforms", [{"archive_session": session, "actors": actor_guids}])

    def read_archive(self, session):
        return self._invoke_manifest("scene.read_archive_scene", [{"archive_session": session}])

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
        from pathlib import Path
        from script_runtime.file_api import current_run
        run = current_run()
        if run is not None:
            actor_data = {**(actor_data or {}), "archive_session": run.session}
        if Path(source_path).suffix.lower() == ".actor" and not (actor_data or {}).get("resolved_model"):
            from runtime.archive.assets import actor_descriptor
            active = self._invoke_manifest("scene.read_active_scene", [])
            root = Path(active["project_path"]).resolve()
            descriptor = (root / source_path).resolve()
            if not descriptor.is_relative_to(root):
                raise ValueError("Actor descriptor escapes project")
            actor_data = {**(actor_data or {}), "resolved_model": actor_descriptor(descriptor, root)["model"]}
        return self._invoke_manifest(
            "scene_tools.create_actor",
            [scene_name, source_path, actor_type, actor_data],
        )

    def remove_actor(self, scene_name, actor_name):
        return self._invoke_manifest("scene_tools.remove_actor", [scene_name, actor_name])

    def set_actor_state(self, scene_name, actor_name, state):
        return self._invoke_manifest(
            "scene_tools.set_actor_state", [scene_name, actor_name, state]
        )

    def set_actor_animation(self, scene_name, actor_name, animation, blend_seconds=0.2):
        return self._invoke_manifest(
            "scene_tools.set_actor_animation", [scene_name, actor_name, animation, blend_seconds]
        )

    def pause_actor_animation(self, scene_name, actor_name):
        return self._invoke_manifest(
            "scene_tools.pause_actor_animation", [scene_name, actor_name]
        )

    def resume_actor_animation(self, scene_name, actor_name):
        return self._invoke_manifest(
            "scene_tools.resume_actor_animation", [scene_name, actor_name]
        )

    def set_actor_physics(self, scene_name, actor_name, physics):
        return self._invoke_manifest(
            "scene_tools.set_actor_physics", [scene_name, actor_name, physics]
        )


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
