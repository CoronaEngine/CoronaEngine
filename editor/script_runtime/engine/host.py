"""Script Runtime host lifecycle helpers.

The editor host calls this module for canonical Script Runtime startup. Native
scene route/value objects are required; startup does not recreate legacy
Python Scene objects.
"""

from __future__ import annotations

import logging
import os
from typing import Any

from api.editor_api import get_script_runtime_editor_api
from script_runtime.manifest_adapter import scene_target_from_snapshot

logger = logging.getLogger(__name__)


def _native_scene_target():
    scene_api = get_script_runtime_editor_api().scene
    route_payload = scene_api.list_routes()
    scene_items = route_payload.get("scenes") if isinstance(route_payload, dict) else None
    routes = [
        str(item.get("path"))
        for item in scene_items or []
        if isinstance(item, dict) and item.get("path")
    ]
    if not routes:
        return None
    active_route = (
        str(route_payload.get("active_scene") or "")
        if isinstance(route_payload, dict)
        else ""
    )
    route = active_route if active_route in routes else routes[0]
    switched = scene_api.switch(route)
    if isinstance(switched, dict) and switched.get("status") in ("error", "failed"):
        return None
    return scene_target_from_snapshot(scene_api.get_snapshot(route))


def initialize_scripts(host: Any, project_path: str) -> None:
    """Lazily bind the project ScriptsManager to the first available scene."""
    if not project_path:
        return

    scene = None
    try:
        scene = _native_scene_target()
    except Exception as exc:
        logger.debug("Native Script Runtime scene target unavailable: %s", exc)

    if scene is None:
        return

    from .scripts_manager import ScriptsManager

    if host.scripts_mgr is None:
        host.scripts_mgr = ScriptsManager()
    project_script = os.path.join(project_path, "Scripts", "project_script.py")
    host.scripts_mgr.initialize_project(project_script, scene)
    logger.debug("ScriptsManager: canonical host initialization completed, scene=%s", scene.name)
    host._scripts_initialized = True


__all__ = ["initialize_scripts"]
