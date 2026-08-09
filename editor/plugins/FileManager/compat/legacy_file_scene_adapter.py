"""Compatibility bindings between file operations and the legacy Scene store.

The file plugin owns filesystem operations.  This module owns the temporary
scene/actor binding updates required by legacy editor hosts and emits the
compatibility events they consume.
"""

import logging
import os

from api.editor_api import emit_compat_editor_event
from runtime.legacy_scene_store import legacy_scene_store


logger = logging.getLogger(__name__)


def rename_file_binding(old_path: str, new_name: str) -> bool:
    """Update legacy scene/actor routes after a file was renamed."""
    if old_path.endswith(".scene"):
        scene = legacy_scene_store.get_or_create(old_path)
        new_path = os.path.normpath(
            os.path.join(os.path.dirname(old_path), new_name)
        ).replace("\\", "/")
        scene.set_route(new_path)
        legacy_scene_store.remove(old_path)
        legacy_scene_store.register(new_path, scene)

        emit_compat_editor_event("scene-rename", [old_path, new_path, scene.name])
        emit_compat_editor_event("actor-change", ["scene", new_path, "", old_path])
        return True
    elif old_path.endswith(".actor"):
        actor = legacy_scene_store.find_actor_by_route(old_path)
        if actor is None:
            logger.warning("Rename actor: '%s' not found in any scene", old_path)
            return False
        new_path = os.path.normpath(
            os.path.join(os.path.dirname(old_path), new_name)
        ).replace("\\", "/")
        actor.set_route(new_path)

        emit_compat_editor_event("actor-change", ["actor", "", new_path, old_path])
        return True

    return True


def open_file_binding(path: str, file_type: str) -> None:
    """Open a legacy scene/actor binding and notify old editor hosts."""
    if file_type == "scene":
        scene = legacy_scene_store.get_or_create(path)
        scene.ensure_default_camera()
        emit_compat_editor_event("scene-add", [scene.name, scene.route])
        emit_compat_editor_event("actor-change", ["scene", scene.route, ""])
    elif file_type == "actor":
        actor = legacy_scene_store.find_actor_by_route(path)
        if actor is None:
            actor = legacy_scene_store.create_actor(path, "actor")
        emit_compat_editor_event("actor-change", ["actor", "", actor.route])
    else:
        raise ValueError(f"No file type: {file_type}")
