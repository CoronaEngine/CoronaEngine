"""Scene-level VLM review capture tool."""
from __future__ import annotations

import datetime
import json
import os
from typing import List

from langchain_core.tools import StructuredTool
from pydantic import BaseModel, Field

from Quasar.ai_tools.response_adapter import (
    build_part,
    build_success_result,
    build_error_result,
)

DEFAULT_SCENE_NAME = ""


def _resolve_scene(scene_manager, scene_name: str):
    if scene_name:
        scene = scene_manager.get(scene_name)
        if scene is not None:
            return scene
        for route in scene_manager.list_all():
            s = scene_manager.get(route)
            if s is not None and getattr(s, "name", None) == scene_name:
                return s
    routes = scene_manager.list_all()
    if routes:
        return scene_manager.get(routes[0])
    return None


# ===========================================================================
# Input Schema
# ===========================================================================

class SceneMultiViewInput(BaseModel):
    scene_name: str = Field(
        default=DEFAULT_SCENE_NAME,
        description="目标场景名称，为空则使用当前场景",
    )
    output_dir: str | None = Field(
        default=None,
        description="输出目录路径。为空则自动生成到项目 screenshots/ 目录下",
    )


# ===========================================================================
# Helpers
# ===========================================================================

def _get_screenshot_dir() -> str:
    from Quasar.ai_config.paths_config import get_project_screenshots_dir
    return str(get_project_screenshots_dir())


def _import_vlm_capture():
    try:
        from plugins.AITool.cai_extensions.agent.vlm_capture import capture_vlm_views
    except ModuleNotFoundError:
        from cai_extensions.agent.vlm_capture import capture_vlm_views
    return capture_vlm_views


# ===========================================================================
# Tool builder
# ===========================================================================

def _build_scene_multi_view_tool(scene_manager) -> StructuredTool:
    """构建场景级多视图拍摄工具"""

    def _scene_multi_view(
        *,
        scene_name: str = DEFAULT_SCENE_NAME,
        output_dir: str | None = None,
    ) -> str:
        try:
            scene = _resolve_scene(scene_manager, scene_name)
            if scene is None:
                return build_error_result(
                    error_message="No scene loaded",
                ).to_envelope(interface_type="scene")

            ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
            if not output_dir:
                output_dir = os.path.join(
                    _get_screenshot_dir(), f"scene_multiview_{ts}",
                )
            elif not os.path.isabs(output_dir):
                output_dir = os.path.join(_get_screenshot_dir(), output_dir)
            capture_vlm_views = _import_vlm_capture()
            capture = capture_vlm_views(
                scene_name,
                output_dir,
                scope="scene",
                timeout_sec=3.0,
                scene=scene,
            )
            if capture.status != "success":
                return build_error_result(
                    error_message=f"VLM scene capture skipped: {capture.skipped_reason}",
                ).to_envelope(interface_type="scene")

            result_data = {
                "status": "success",
                "scene_center": capture.center,
                "scene_radius": capture.radius,
                "view_count": capture.view_count,
                "output_mode": capture.output_mode,
                "total_images": len(capture.files),
                "output_dir": capture.output_dir,
                "files": capture.files,
                "target_bounds": capture.to_dict().get("target_bounds"),
            }
            part = build_part(
                content_type="text",
                content_text=json.dumps(result_data, ensure_ascii=False),
            )
            return build_success_result(parts=[part]).to_envelope(
                interface_type="scene"
            )
        except Exception as e:
            return build_error_result(error_message=str(e)).to_envelope(
                interface_type="scene"
            )

    return StructuredTool(
        name="scene_multi_view_capture",
        description=(
            "使用隐藏 VLM 审查摄像头对整个场景进行 4 视角 base_color 拍摄。"
            "基于场景 AABB 自适应构图，不支持多输出通道。"
        ),
        args_schema=SceneMultiViewInput,
        func=_scene_multi_view,
    )


# ===========================================================================
# Loader
# ===========================================================================

def load_multi_view_tools() -> List[StructuredTool]:
    from CoronaCore.core.managers import scene_manager
    return [
        _build_scene_multi_view_tool(scene_manager),
    ]


__all__ = ["load_multi_view_tools"]
