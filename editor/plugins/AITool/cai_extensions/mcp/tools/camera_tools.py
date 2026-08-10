from __future__ import annotations

import json
import math
import os
import datetime
from typing import List, Literal, Optional, Tuple, TYPE_CHECKING

from langchain_core.tools import StructuredTool
from pydantic import BaseModel, Field

from Quasar.ai_tools.response_adapter import (
    build_part,
    build_success_result,
    build_error_result,
)

DEFAULT_SCENE_NAME = ""


def _import_vlm_capture():
    try:
        from .vlm_capture import capture_vlm_views
    except ModuleNotFoundError:
        from plugins.AITool.cai_extensions.mcp.tools.vlm_capture import capture_vlm_views
    return capture_vlm_views


def _resolve_scene(scene_manager, scene_name: str):
    from .native_scene_state import resolve_native_scene_value

    return resolve_native_scene_value(scene_name)


def _get_authoritative_snapshot(scene_api, scene_route: str) -> dict:
    raw_snapshot = scene_api.get_snapshot(scene_route)
    snapshot = json.loads(raw_snapshot) if isinstance(raw_snapshot, str) else raw_snapshot
    if not isinstance(snapshot, dict):
        raise RuntimeError("Invalid scene snapshot response")
    if snapshot.get("status") in ("error", "failed"):
        raise RuntimeError(snapshot.get("message", "Scene snapshot failed"))
    return snapshot


def _find_snapshot_camera(snapshot: dict, camera_name: str | None):
    cameras = snapshot.get("cameras")
    if camera_name and isinstance(cameras, list):
        for camera in cameras:
            if isinstance(camera, dict) and camera_name in (
                camera.get("name"), camera.get("id"), camera.get("camera_id")
            ):
                return camera
        return None
    camera = snapshot.get("camera")
    return camera if isinstance(camera, dict) else None


# ===========================================================================
# Input Schemas
# ===========================================================================

class CameraMoveInput(BaseModel):
    scene_name: str = Field(default=DEFAULT_SCENE_NAME, description="目标场景名称")
    camera_name: str | None = Field(default=None, description="摄像头名称，为空则使用主摄像头")
    position: Tuple[float, float, float] = Field(description="摄像头位置 (x, y, z)")
    forward: Tuple[float, float, float] = Field(description="摄像头朝向 (x, y, z)")
    up: Tuple[float, float, float] = Field(
        default=(0.0, 1.0, 0.0), description="摄像头上方向 (x, y, z)，默认 (0, 1, 0)"
    )
    fov: float = Field(default=45.0, description="视野角度，默认 45")


class CameraGetInput(BaseModel):
    scene_name: str = Field(default=DEFAULT_SCENE_NAME, description="目标场景名称")
    camera_name: str | None = Field(default=None, description="摄像头名称，为空则使用主摄像头")


class CameraFocusInput(BaseModel):
    scene_name: str = Field(default=DEFAULT_SCENE_NAME, description="目标场景名称")
    actor_name: str = Field(description="要聚焦的对象名称")
    camera_name: str | None = Field(default=None, description="摄像头名称，为空则使用主摄像头")


class CameraListInput(BaseModel):
    scene_name: str = Field(default=DEFAULT_SCENE_NAME, description="目标场景名称")


class CameraScreenshotInput(BaseModel):
    scene_name: str = Field(default=DEFAULT_SCENE_NAME, description="目标场景名称")
    camera_name: str | None = Field(default=None, description="摄像头名称；为空则使用隐藏离屏审查摄像头，避免扰动主摄像头")
    output_path: str | None = Field(
        default=None,
        description="截图保存路径。为空则自动生成路径保存到项目目录下的 screenshots/ 文件夹",
    )


class CameraMultiviewInput(BaseModel):
    scene_name: str = Field(default=DEFAULT_SCENE_NAME, description="目标场景名称")
    actor_name: str = Field(description="要环绕拍摄的对象名称")
    output_dir: str | None = Field(
        default=None,
        description="截图输出目录。为空则自动生成目录",
    )


# ===========================================================================
# Tool builders
# ===========================================================================

def _build_camera_move_tool(scene_manager) -> StructuredTool:
    """构建摄像头移动工具"""

    def _camera_move(
        *,
        scene_name: str = DEFAULT_SCENE_NAME,
        camera_name: str | None = None,
        position: Tuple[float, float, float],
        forward: Tuple[float, float, float],
        up: Tuple[float, float, float] = (0.0, 1.0, 0.0),
        fov: float = 45.0,
    ) -> str:
        try:
            scene = _resolve_scene(scene_manager, scene_name)
            if scene is None:
                return build_error_result(
                    error_message="No scene loaded"
                ).to_envelope(interface_type="scene")

            from api.editor_api import CoronaEditorApi

            viewport = CoronaEditorApi.viewport
            scene_route = getattr(scene, "route", scene_name or "")
            target_camera_name = camera_name or ""
            camera_data = {
                "position": list(position),
                "forward": list(forward),
                "world_up": list(up),
                "fov": float(fov),
                "persist": True,
            }

            if viewport is None or not callable(getattr(viewport, "set_camera_pose", None)):
                return build_error_result(
                    error_message="Viewport camera pose aggregate API is unavailable"
                ).to_envelope(interface_type="scene")
            native_result_raw = viewport.set_camera_pose(
                scene_route,
                target_camera_name,
                camera_data,
            )
            native_result = (
                json.loads(native_result_raw)
                if isinstance(native_result_raw, str)
                else native_result_raw
            )
            if not isinstance(native_result, dict) or native_result.get("status") not in ("success", "ok"):
                message = (
                    native_result.get("message")
                    if isinstance(native_result, dict)
                    else "Native camera pose update failed"
                ) or "Native camera pose update failed"
                return build_error_result(error_message=message).to_envelope(
                    interface_type="scene"
                )

            native_camera = native_result.get("camera")
            if isinstance(native_camera, dict):
                target_camera_name = native_camera.get("name") or target_camera_name
                camera_data = {
                    "position": native_camera.get("position", camera_data["position"]),
                    "forward": native_camera.get("forward", camera_data["forward"]),
                    "world_up": native_camera.get("world_up", camera_data["world_up"]),
                    "fov": native_camera.get("fov", camera_data["fov"]),
                }

            result_data = {
                "status": "success",
                "camera": target_camera_name,
                "position": camera_data["position"],
                "forward": camera_data["forward"],
                "up": camera_data["world_up"],
                "fov": camera_data["fov"],
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
        name="camera_move",
        description=(
            "移动摄像头到指定位置和朝向。需要提供位置坐标 (x,y,z)、朝向 (x,y,z)，可选上方向和视野角度。"
            "坐标系：X正为右，Y正为上，Z正为朝屏幕里侧（左手坐标系）。"
        ),
        args_schema=CameraMoveInput,
        func=_camera_move,
    )


def _build_camera_get_tool(scene_manager) -> StructuredTool:
    """构建获取摄像头信息工具"""

    def _camera_get(
        *,
        scene_name: str = DEFAULT_SCENE_NAME,
        camera_name: str | None = None,
    ) -> str:
        try:
            scene = _resolve_scene(scene_manager, scene_name)
            if scene is None:
                return build_error_result(
                    error_message="No scene loaded"
                ).to_envelope(interface_type="scene")

            from api.editor_api import CoronaEditorApi

            scene_api = CoronaEditorApi.scene
            camera_data = _find_snapshot_camera(
                _get_authoritative_snapshot(scene_api, getattr(scene, "route", scene_name or "")),
                camera_name,
            )
            if camera_data is None:
                return build_error_result(
                    error_message=f"No camera available in scene '{scene_name}'"
                ).to_envelope(interface_type="scene")
            result_data = {
                "camera": camera_data.get("name", camera_name),
                "position": camera_data.get("position", []),
                "forward": camera_data.get("forward", []),
                "up": camera_data.get("world_up", camera_data.get("up", [])),
                "fov": camera_data.get("fov", 45.0),
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
        name="camera_get",
        description=(
            "获取摄像头当前状态，包括位置、朝向、上方向和视野角度。"
            "坐标系：X正为右，Y正为上，Z正为朝屏幕里侧（左手坐标系）。"
        ),
        args_schema=CameraGetInput,
        func=_camera_get,
    )


def _build_camera_focus_tool(scene_manager) -> StructuredTool:
    """构建摄像头聚焦工具"""

    def _camera_focus(
        *,
        scene_name: str = DEFAULT_SCENE_NAME,
        actor_name: str,
        camera_name: str | None = None,
    ) -> str:
        try:
            scene = _resolve_scene(scene_manager, scene_name)
            if scene is None:
                return build_error_result(
                    error_message="No scene loaded"
                ).to_envelope(interface_type="scene")

            from api.editor_api import CoronaEditorApi

            aggregate_focus = CoronaEditorApi.scene_tools.focus_actor
            native_result_raw = aggregate_focus(
                getattr(scene, "route", scene_name or ""),
                actor_name,
                camera_name or "",
            )
            native_result = (
                json.loads(native_result_raw)
                if isinstance(native_result_raw, str)
                else native_result_raw
            )
            if not isinstance(native_result, dict) or native_result.get("status") not in ("success", "ok"):
                message = (
                    native_result.get("message")
                    if isinstance(native_result, dict)
                    else "Native camera focus failed"
                ) or "Native camera focus failed"
                return build_error_result(error_message=message).to_envelope(
                    interface_type="scene"
                )
            camera_data = native_result.get("camera") if isinstance(native_result.get("camera"), dict) else {}
            result_data = {
                "status": "success",
                "target": actor_name,
                "center": native_result.get("center", []),
                "distance": native_result.get("distance", 0.0),
                "camera": camera_data.get("name", camera_name),
                "position": camera_data.get("position", []),
                "forward": camera_data.get("forward", []),
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
        name="camera_focus",
        description="将摄像头聚焦到场景中的指定对象上。会自动计算合适的观察距离。",
        args_schema=CameraFocusInput,
        func=_camera_focus,
    )


def _build_camera_list_tool(scene_manager) -> StructuredTool:
    """构建列出场景摄像头工具"""

    def _camera_list(
        *,
        scene_name: str = DEFAULT_SCENE_NAME,
    ) -> str:
        try:
            scene = _resolve_scene(scene_manager, scene_name)
            if scene is None:
                return build_error_result(
                    error_message="No scene loaded"
                ).to_envelope(interface_type="scene")

            from api.editor_api import CoronaEditorApi

            scene_api = CoronaEditorApi.scene
            snapshot = _get_authoritative_snapshot(
                scene_api, getattr(scene, "route", scene_name or "")
            )
            cameras_info = []
            for camera in snapshot.get("cameras", []):
                if not isinstance(camera, dict):
                    continue
                cameras_info.append({
                    "name": camera.get("name", "Unknown"),
                    "position": camera.get("position", []),
                    "fov": camera.get("fov", 45.0),
                })

            result_data = {
                "scene": scene_name,
                "cameras": cameras_info,
                "count": len(cameras_info),
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
        name="camera_list",
        description="列出场景中所有摄像头及其基本信息。",
        args_schema=CameraListInput,
        func=_camera_list,
    )


# ===========================================================================
# Loader
# ===========================================================================

def _get_screenshot_dir():
    """获取截图输出基础目录"""
    from config.paths_config import get_project_screenshots_dir

    return str(get_project_screenshots_dir())


def _build_camera_screenshot_tool(scene_manager) -> StructuredTool:
    """构建截图工具"""

    def _camera_screenshot(
        *,
        scene_name: str = DEFAULT_SCENE_NAME,
        camera_name: str | None = None,
        output_path: str | None = None,
    ) -> str:
        try:
            scene = _resolve_scene(scene_manager, scene_name)
            if scene is None:
                return build_error_result(
                    error_message="No scene loaded"
                ).to_envelope(interface_type="scene")

            from api.editor_api import CoronaEditorApi

            scene_route = getattr(scene, "route", scene_name or "")
            scene_api = CoronaEditorApi.scene
            resolved_camera_name = camera_name or ""
            snapshot_raw = scene_api.get_snapshot(scene_route)
            snapshot = json.loads(snapshot_raw) if isinstance(snapshot_raw, str) else snapshot_raw
            if isinstance(snapshot, dict) and snapshot.get("status") in ("error", "failed"):
                return build_error_result(
                    error_message=snapshot.get("message", "Scene snapshot failed")
                ).to_envelope(interface_type="scene")
            cameras = snapshot.get("cameras", []) if isinstance(snapshot, dict) else []
            camera_data = None
            if camera_name:
                camera_data = next(
                    (
                        item for item in cameras
                        if isinstance(item, dict)
                        and camera_name in (item.get("name"), item.get("id"), item.get("camera_id"))
                    ),
                    None,
                )
            if camera_data is None and isinstance(snapshot, dict):
                camera_data = snapshot.get("camera")
            if not isinstance(camera_data, dict):
                return build_error_result(
                    error_message=f"No camera available in scene '{scene_name}'"
                ).to_envelope(interface_type="scene")
            resolved_camera_name = camera_data.get("name") or camera_name or ""

            # 确定输出路径
            if not output_path:
                ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S_%f")
                output_path = os.path.join(
                    _get_screenshot_dir(), f"shot_base_color_{ts}.png"
                )
            else:
                # 相对路径统一放到项目截图目录下
                if not os.path.isabs(output_path):
                    output_path = os.path.join(
                        _get_screenshot_dir(), output_path
                    )
            os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)

            viewport = CoronaEditorApi.viewport

            camera_data = dict(camera_data)
            camera_data.setdefault("width", 512)
            camera_data.setdefault("height", 512)
            camera_data["output_mode"] = "base_color"
            camera_data.setdefault("render_backend", "native")
            camera_data.setdefault("vision_render_mode", "path_tracing")
            raw_result = viewport.capture(
                scene_route,
                resolved_camera_name,
                camera_data,
                output_path,
            )
            native_result = json.loads(raw_result) if isinstance(raw_result, str) else raw_result
            if not isinstance(native_result, dict) or native_result.get("status") not in ("success", "ok"):
                return build_error_result(
                    error_message=f"Screenshot timed out or failed: {output_path}"
                ).to_envelope(interface_type="scene")

            result_data = {
                "status": "success",
                "path": output_path,
                "output_mode": "base_color",
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
        name="camera_screenshot",
        description="使用摄像头按 base_color 拍摄截图并保存到文件。",
        args_schema=CameraScreenshotInput,
        func=_camera_screenshot,
    )


def _build_camera_multiview_tool(scene_manager) -> StructuredTool:
    """构建多视图环绕拍摄工具"""

    def _camera_multiview(
        *,
        scene_name: str = DEFAULT_SCENE_NAME,
        actor_name: str,
        output_dir: str | None = None,
    ) -> str:
        try:
            scene = _resolve_scene(scene_manager, scene_name)
            if scene is None:
                return build_error_result(
                    error_message="No scene loaded"
                ).to_envelope(interface_type="scene")

            ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
            if not output_dir:
                output_dir = os.path.join(
                    _get_screenshot_dir(),
                    f"multiview_{actor_name}_{ts}",
                )
            elif not os.path.isabs(output_dir):
                output_dir = os.path.join(_get_screenshot_dir(), output_dir)
            capture_vlm_views = _import_vlm_capture()
            capture = capture_vlm_views(
                scene_name,
                output_dir,
                actor_name=actor_name,
                scope="actor",
                timeout_sec=3.0,
                scene=scene,
            )
            if capture.status != "success":
                return build_error_result(
                    error_message=f"VLM capture skipped: {capture.skipped_reason}"
                ).to_envelope(interface_type="scene")

            result_data = {
                "status": "success",
                "actor": actor_name,
                "view_count": capture.view_count,
                "output_mode": capture.output_mode,
                "output_dir": capture.output_dir,
                "files": capture.files,
                "total_images": len(capture.files),
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
        name="camera_multiview_capture",
        description=(
            "使用隐藏 VLM 审查摄像头对目标对象进行 4 视角 base_color 拍摄。"
            "不会移动主摄像头，也不支持多输出通道。"
        ),
        args_schema=CameraMultiviewInput,
        func=_camera_multiview,
    )


def load_camera_tools() -> List[StructuredTool]:
    return [
        _build_camera_move_tool(None),
        _build_camera_get_tool(None),
        _build_camera_focus_tool(None),
        _build_camera_list_tool(None),
        _build_camera_screenshot_tool(None),
        _build_camera_multiview_tool(None),
    ]


__all__ = ["load_camera_tools"]
