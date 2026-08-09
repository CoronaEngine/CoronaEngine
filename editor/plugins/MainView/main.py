"""MainView's legacy-host adapter over the native main/scene contracts."""

import json
import logging
import os
from typing import Optional

from api.editor_api import CoronaEditorApi, emit_compat_editor_event
from runtime.plugin_base import PluginBase
from config.paths_config import get_default_paths

core_path = get_default_paths()

logger = logging.getLogger(__name__)


@PluginBase.register_web("MainView")
class MainView(PluginBase):
    @staticmethod
    def _active_project_path() -> str:
        try:
            info = CoronaEditorApi.project_settings.get_active_project_info()
        except Exception:
            return ""
        return info.get("project_path", "") if isinstance(info, dict) else ""

    @staticmethod
    def _normalize_scene_path(scene_path: str) -> str:
        scene_path = (scene_path or "").strip().replace("\\", "/")
        project_path = MainView._active_project_path()
        if project_path and os.path.isabs(scene_path):
            scene_path = os.path.relpath(scene_path, project_path).replace("\\", "/")
        return scene_path

    @staticmethod
    def on_init(project_path: str = ""):
        result = CoronaEditorApi.main.on_init(project_path)
        if not isinstance(result, dict) or result.get("status") != "success":
            message = result.get("message", "初始化场景失败") if isinstance(result, dict) else "初始化场景失败"
            raise RuntimeError(message)
        return {"scenes": result.get("scenes", []), "active_index": result.get("active_index", 0)}

    @staticmethod
    def create_new_scene(scene_name: str) -> dict:
        result = CoronaEditorApi.main.create_scene(scene_name)
        if not isinstance(result, dict) or result.get("status") != "success":
            message = result.get("message", "创建场景失败") if isinstance(result, dict) else "创建场景失败"
            raise RuntimeError(message)
        route = MainView._normalize_scene_path(result.get("path", ""))
        if not route:
            raise RuntimeError("native create_scene returned no scene path")
        scene_display_name = result.get("name") or os.path.splitext(os.path.basename(route))[0]
        logger.info("New scene file created: %s -> %s", scene_name, route)
        return {"path": route, "name": scene_display_name}

    @staticmethod
    def remove_scene(scene_path: str) -> dict:
        scene_path = MainView._normalize_scene_path(scene_path)
        index = CoronaEditorApi.main.on_init()
        scenes = index.get("scenes", []) if isinstance(index, dict) else []
        scene_paths = [
            MainView._normalize_scene_path(item.get("path", ""))
            for item in scenes
            if isinstance(item, dict)
        ]
        fallback_scene = next((route for route in scene_paths if route != scene_path), "")
        active_scene = MainView._normalize_scene_path(index.get("active_scene", "")) if isinstance(index, dict) else ""
        if active_scene == scene_path:
            if not fallback_scene or not MainView.switch_scene(active_scene, fallback_scene):
                return {"status": "error", "path": scene_path, "message": "Cannot remove the active scene without a fallback scene"}
        result = CoronaEditorApi.main.remove_scene(scene_path)
        if not isinstance(result, dict) or result.get("status") != "success":
            return result if isinstance(result, dict) else {"status": "error", "path": scene_path, "message": "删除场景失败"}
        logger.info("Scene removed from project: %s", scene_path)
        return {**result, "status": "success", "path": scene_path, "deleted_file": bool(result.get("deleted_file"))}

    @staticmethod
    def switch_scene(current_scene_path: str, to_scene_path: str) -> bool:
        to_scene_path = MainView._normalize_scene_path(to_scene_path)
        if not to_scene_path:
            logger.warning("switch_scene ignored empty target scene")
            return False
        result = CoronaEditorApi.scene_tools.reload_scene(to_scene_path)
        if not isinstance(result, dict) or result.get("status") != "success":
            logger.warning("Native scene switch failed: %s", result)
            return False
        scene_route = MainView._normalize_scene_path(result.get("scene") or to_scene_path)
        if not scene_route:
            logger.warning("Native scene switch returned no scene route")
            return False
        emit_compat_editor_event("actor-change", ["scene", scene_route, ""])
        return True

    @staticmethod
    def scene_save(scene_name: str) -> str:
        try:
            return json.dumps(CoronaEditorApi.main.scene_save(scene_name), ensure_ascii=False)
        except Exception as exc:
            return json.dumps({"status": "error", "message": str(exc)})

    @staticmethod
    def run_project(scene_path: Optional[str] = None) -> dict:
        try:
            if scene_path:
                snapshot = CoronaEditorApi.scene.get_snapshot(scene_path)
                if not isinstance(snapshot, dict) or snapshot.get("status") == "error":
                    message = snapshot.get("message", f"场景不存在: {scene_path}") if isinstance(snapshot, dict) else f"场景不存在: {scene_path}"
                    return {"status": "error", "message": message}
                scene_name = snapshot.get("scene_name") or snapshot.get("scene") or scene_path
                logger.info("开始运行场景: %s", scene_name)
            else:
                index = CoronaEditorApi.main.on_init()
                if not isinstance(index, dict) or index.get("status") != "success":
                    return {"status": "error", "message": "当前项目尚未完成初始化"}
                scene_name = index.get("entrance_scene") or index.get("active_scene")
                if not scene_name:
                    return {"status": "error", "message": "当前项目没有入口场景"}
                logger.info("开始运行项目...")
            blockly_result = None
            try:
                from script_runtime.runner import run_generated_script
                run_generated_script(core_path.repo_root)
                blockly_result = "executed"
            except Exception as exc:
                logger.exception("Blockly 脚本执行失败: %s", exc)
                blockly_result = f"error: {exc}"
            return {"status": "success", "type": "scene" if scene_path else "project", "scene_name": scene_name, "blockly_result": blockly_result}
        except Exception as exc:
            logger.error("运行失败: %s", exc)
            return {"status": "error", "message": str(exc)}

__all__ = ["MainView", "core_path"]
