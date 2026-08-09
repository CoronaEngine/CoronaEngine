"""Compatibility boundary for the legacy project-script host."""

import logging
import os

from script_runtime.compat.legacy_scene_adapter import get_scene, list_scene_routes


logger = logging.getLogger(__name__)


def initialize_scripts(host, project_path):
    """Lazily bind the legacy ScriptsManager to the first legacy scene."""
    if not project_path:
        return

    scenes = list_scene_routes()
    if not scenes:
        return

    from script_runtime.engine.scripts_manager import ScriptsManager

    if host.scripts_mgr is None:
        host.scripts_mgr = ScriptsManager()
    project_script = os.path.join(project_path, "Scripts", "project_script.py")
    scene = get_scene(scenes[0])
    if scene:
        host.scripts_mgr.initialize_project(project_script, scene)
        logger.debug("ScriptsManager: 懒初始化完成，场景=%s", scene.name)
    host._scripts_initialized = True
