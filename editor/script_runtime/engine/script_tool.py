"""
脚本工具模块：为脚本间通信提供广播接口。
"""
import logging
from typing import Any, Optional


class ScriptsTool:
    """脚本间事件广播。调用 broadcast_event 通知所有已注册脚本。"""

    _script_manager = None
    _logger = logging.getLogger("ScriptsTool")

    @classmethod
    def initialize(cls, script_manager):
        cls._script_manager = script_manager
        cls._logger.info("ScriptsTool initialized")

    @classmethod
    def broadcast_event(cls, event_name: str, data: Any = None, target: str = "all"):
        if not cls._script_manager:
            cls._logger.warning("ScriptManager not initialized")
            return

        cls._logger.info(f"Broadcasting event: {event_name} to {target}")

        if target in ("all", "project") and cls._script_manager.project_script:
            cls._script_manager.project_script.on_event(event_name, data)

        if target in ("all", "scene") and cls._script_manager.current_scene_script:
            cls._script_manager.current_scene_script.on_event(event_name, data)

        if target in ("all", "actors"):
            for script in cls._script_manager.actor_scripts.values():
                script.on_event(event_name, data)
