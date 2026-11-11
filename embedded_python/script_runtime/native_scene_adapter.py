"""Restricted native scene contract for the Script Runtime.

Every operation is a named binding on the ``CoronaEngine`` module
(``script_scene_<operation>``). There is no name dispatch, no argument schema and
no caller mask on the C++ side: the module boundary is the permission boundary,
so ordinary editor Python never imports this file.

The shapes returned here are unchanged from the retired JSON manifest; only the
channel moved.
"""

from __future__ import annotations

import json

_OPERATIONS = {
    "get_environment": "script_scene_get_environment",
    "read_active_scene": "script_scene_read_active_scene",
    "read_actor": "script_scene_read_actor",
    "read_transforms": "script_scene_read_transforms",
    "read_archive_scene": "script_scene_read_archive_scene",
    "get_snapshot": "script_scene_get_scene_snapshot",
    "set_actor_transform": "script_scene_set_actor_transform",
    "set_environment": "script_scene_set_environment",
    "set_actor_state": "script_scene_set_actor_state",
    "set_actor_animation": "script_scene_set_actor_animation",
    "pause_actor_animation": "script_scene_pause_actor_animation",
    "resume_actor_animation": "script_scene_resume_actor_animation",
    "set_actor_physics": "script_scene_set_actor_physics",
    "set_camera_pose": "script_scene_set_camera_pose",
    "create_actor": "script_scene_create_actor",
    "remove_actor": "script_scene_remove_actor",
    "play_actor_audio": "script_scene_play_actor_audio",
    "stop_actor_audio": "script_scene_stop_actor_audio",
}


def invoke_native_scene(operation, args=None):
    """Call one named native operation and return its payload."""
    import CoronaEngine

    binding = _OPERATIONS.get(operation)
    if binding is None:
        raise RuntimeError(f"Unknown Script Runtime operation: {operation}")
    function = getattr(CoronaEngine, binding, None)
    if not callable(function):
        raise RuntimeError(f"Script Runtime API channel is unavailable: {operation}")
    response = json.loads(function(json.dumps(args or [], ensure_ascii=False)))
    if not response.get("success", False):
        message = response.get("error") or f"Script Runtime API failed: {operation}"
        # The symbolic code rides in data.code; it is what lets a log say
        # "PROJECT_CONTEXT_CHANGED" instead of repeating the message.
        symbol = (response.get("data") or {}).get("code")
        raise RuntimeError(f"{symbol}: {message}" if symbol else message)
    return response.get("data")


class ScriptRuntimeSceneAdapter:
    def get_snapshot(self, scene_name=""):
        return invoke_native_scene("get_snapshot", [scene_name])

    def read_active(self):
        return invoke_native_scene("read_active_scene", [])

    def read_actor(self, session, actor_guid):
        return invoke_native_scene(
            "read_actor", [{"archive_session": session, "actor": actor_guid}])

    def read_transforms(self, session, actor_guids):
        return invoke_native_scene(
            "read_transforms", [{"archive_session": session, "actors": actor_guids}])

    def read_archive(self, session):
        return invoke_native_scene("read_archive_scene", [{"archive_session": session}])

    def get_environment(self, scene_name):
        return invoke_native_scene("get_environment", [scene_name])

    def set_environment(self, scene_name, state):
        return invoke_native_scene("set_environment", [scene_name, state])

    def set_actor_transform(self, scene_name, actor_name, transform):
        return invoke_native_scene("set_actor_transform", [scene_name, actor_name, transform])


class ScriptRuntimeSceneToolsAdapter:
    def create_actor(self, scene_name, source_path, actor_type, actor_data):
        from pathlib import Path
        from script_runtime.file_api import current_run
        run = current_run()
        if run is not None:
            actor_data = {**(actor_data or {}), "archive_session": run.session}
        if Path(source_path).suffix.lower() == ".actor" and not (actor_data or {}).get("resolved_model"):
            from runtime.archive.assets import actor_descriptor
            active = invoke_native_scene("read_active_scene", [])
            root = Path(active["project_path"]).resolve()
            descriptor = (root / source_path).resolve()
            if not descriptor.is_relative_to(root):
                raise ValueError("Actor descriptor escapes project")
            actor_data = {**(actor_data or {}), "resolved_model": actor_descriptor(descriptor, root)["model"]}
        return invoke_native_scene(
            "create_actor", [scene_name, source_path, actor_type, actor_data])

    def remove_actor(self, scene_name, actor_name):
        return invoke_native_scene("remove_actor", [scene_name, actor_name])

    def set_actor_state(self, scene_name, actor_name, state):
        return invoke_native_scene("set_actor_state", [scene_name, actor_name, state])

    def set_actor_animation(self, scene_name, actor_name, animation, blend_seconds=0.2):
        return invoke_native_scene(
            "set_actor_animation", [scene_name, actor_name, animation, blend_seconds])

    def pause_actor_animation(self, scene_name, actor_name):
        return invoke_native_scene("pause_actor_animation", [scene_name, actor_name])

    def resume_actor_animation(self, scene_name, actor_name):
        return invoke_native_scene("resume_actor_animation", [scene_name, actor_name])

    def set_actor_physics(self, scene_name, actor_name, physics):
        return invoke_native_scene("set_actor_physics", [scene_name, actor_name, physics])

    def play_actor_audio(self, scene_name, actor_name, loop=False):
        return invoke_native_scene("play_actor_audio", [scene_name, actor_name, {"loop": loop}])

    def stop_actor_audio(self, scene_name, actor_name):
        return invoke_native_scene("stop_actor_audio", [scene_name, actor_name, {}])


class ScriptRuntimeViewportAdapter:
    def set_camera_pose(self, scene_name, camera_name, camera):
        return invoke_native_scene("set_camera_pose", [scene_name, camera_name, camera])


class ScriptRuntimeEditorApi:
    def __init__(self):
        self.scene = ScriptRuntimeSceneAdapter()
        self.scene_tools = ScriptRuntimeSceneToolsAdapter()
        self.viewport = ScriptRuntimeViewportAdapter()
