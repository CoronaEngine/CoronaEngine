"""Unified VLM review capture.

All VLM/review multi-view screenshots go through this module: four adaptive
views, hidden review camera, base_color only.
"""
from __future__ import annotations

import logging
import math
import os
import hashlib
import shutil
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, List, Optional

logger = logging.getLogger(__name__)

VLM_OUTPUT_MODE = "base_color"
VLM_VIEW_COUNT = 4
VLM_REVIEW_CAMERA_NAME = "vlm_review_camera"

_DEFAULT_CENTER = [0.0, 0.75, 0.0]
_DEFAULT_RADIUS = 2.0


def get_project_screenshots_root() -> Path:
    """Return the active project's screenshots directory."""
    try:
        from Quasar.ai_config.paths_config import get_project_screenshots_dir
        return Path(get_project_screenshots_dir())
    except Exception as exc:
        logger.debug("[VlmCapture] project screenshots resolver unavailable: %s", exc)

    override = os.environ.get("CAI_SCREENSHOTS_DIR")
    if override:
        root = Path(override)
    else:
        project_root = os.environ.get("CAI_PROJECT_ROOT")
        root = Path(project_root) / "screenshots" if project_root else Path(os.getcwd()) / "screenshots"
    root.mkdir(parents=True, exist_ok=True)
    return root


def resolve_vlm_output_dir(output_dir: str) -> str:
    """Resolve relative VLM screenshot paths under the active project's screenshots dir."""
    path = Path(str(output_dir or "").strip() or "_vlm_review")
    if path.is_absolute():
        resolved = path
    else:
        resolved = get_project_screenshots_root() / path
    resolved.mkdir(parents=True, exist_ok=True)
    return str(resolved)


def _is_ascii_path(path: str) -> bool:
    try:
        str(path).encode("ascii")
        return True
    except UnicodeEncodeError:
        return False


def _ascii_bridge_root() -> Path:
    candidates = [
        os.environ.get("CORONA_VLM_ASCII_TEMP_DIR"),
        tempfile.gettempdir(),
        os.environ.get("LOCALAPPDATA"),
        os.environ.get("ProgramData"),
        str(Path.cwd()),
    ]
    if os.name == "nt":
        candidates.append(os.environ.get("SystemDrive", "C:") + os.sep)
    else:
        candidates.append("/tmp")
    for candidate in candidates:
        if not candidate or not _is_ascii_path(candidate):
            continue
        root = Path(candidate) / "CoronaEngineVlmCapture"
        try:
            root.mkdir(parents=True, exist_ok=True)
            return root
        except OSError:
            continue
    root = Path(tempfile.gettempdir()) / "CoronaEngineVlmCapture"
    root.mkdir(parents=True, exist_ok=True)
    return root


def _engine_safe_screenshot_path(final_path: str, view_index: int) -> tuple[str, bool]:
    """Return an ASCII path for engine screenshot APIs that reject unicode paths."""
    final = Path(final_path)
    final.parent.mkdir(parents=True, exist_ok=True)
    if _is_ascii_path(str(final)):
        return str(final), False
    suffix = final.suffix or ".png"
    digest = hashlib.sha1(str(final).encode("utf-8", errors="surrogatepass")).hexdigest()[:16]
    bridge_name = f"vlm_{digest}_{view_index:02d}{suffix}"
    return str(_ascii_bridge_root() / bridge_name), True


def _copy_engine_screenshot_to_final(engine_path: str, final_path: str) -> bool:
    if os.path.abspath(engine_path) == os.path.abspath(final_path):
        return True
    try:
        Path(final_path).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(engine_path, final_path)
        try:
            os.remove(engine_path)
        except OSError:
            pass
        return os.path.exists(final_path) and os.path.getsize(final_path) > 0
    except Exception as exc:
        logger.warning(
            "[VlmCapture] copy screenshot back to unicode path failed src=%s dst=%s error=%s",
            engine_path,
            final_path,
            exc,
        )
        return False


@dataclass
class TargetBounds:
    source: str
    min: List[float]
    max: List[float]
    center: List[float]
    radius: float
    valid: bool = True


@dataclass
class ViewPose:
    name: str
    position: List[float]
    forward: List[float]
    up: List[float]
    fov: float
    azimuth_deg: float
    elevation_deg: float


@dataclass
class VlmCaptureResult:
    status: str
    output_dir: str
    files: List[str] = field(default_factory=list)
    view_count: int = VLM_VIEW_COUNT
    output_mode: str = VLM_OUTPUT_MODE
    target_bounds: Optional[TargetBounds] = None
    center: List[float] = field(default_factory=lambda: list(_DEFAULT_CENTER))
    radius: float = _DEFAULT_RADIUS
    poses: List[ViewPose] = field(default_factory=list)
    camera_name: str = VLM_REVIEW_CAMERA_NAME
    skipped_reason: str = ""

    def to_dict(self) -> dict:
        return {
            "status": self.status,
            "output_dir": self.output_dir,
            "files": list(self.files),
            "view_count": self.view_count,
            "output_mode": self.output_mode,
            "target_bounds": (
                {
                    "source": self.target_bounds.source,
                    "min": list(self.target_bounds.min),
                    "max": list(self.target_bounds.max),
                    "center": list(self.target_bounds.center),
                    "radius": self.target_bounds.radius,
                    "valid": self.target_bounds.valid,
                }
                if self.target_bounds else None
            ),
            "center": list(self.center),
            "radius": self.radius,
            "poses": [
                {
                    "name": p.name,
                    "position": list(p.position),
                    "forward": list(p.forward),
                    "up": list(p.up),
                    "fov": p.fov,
                    "azimuth_deg": p.azimuth_deg,
                    "elevation_deg": p.elevation_deg,
                }
                for p in self.poses
            ],
            "camera_name": self.camera_name,
            "skipped_reason": self.skipped_reason,
        }


def _resolve_scene(scene_name: str, scene: Any = None) -> Any:
    if scene is not None:
        return scene
    try:
        from CoronaCore.core.managers import scene_manager
        if scene_name:
            found = scene_manager.get(scene_name)
            if found is not None:
                return found
        routes = scene_manager.list_all()
        return scene_manager.get(routes[0]) if routes else None
    except Exception as exc:
        logger.warning("[VlmCapture] resolve scene failed: %s", exc)
        return None


def _coerce_aabb(value: Any) -> Optional[List[float]]:
    if not isinstance(value, (list, tuple)) or len(value) < 6:
        return None
    try:
        nums = [float(value[i]) for i in range(6)]
    except Exception:
        return None
    if any(not math.isfinite(v) for v in nums):
        return None
    if nums[3] < nums[0] or nums[4] < nums[1] or nums[5] < nums[2]:
        return None
    return nums


def _bounds_from_aabb(aabb: List[float], source: str) -> TargetBounds:
    min_pt = [aabb[0], aabb[1], aabb[2]]
    max_pt = [aabb[3], aabb[4], aabb[5]]
    center = [
        (min_pt[0] + max_pt[0]) * 0.5,
        (min_pt[1] + max_pt[1]) * 0.5,
        (min_pt[2] + max_pt[2]) * 0.5,
    ]
    dx = max_pt[0] - min_pt[0]
    dy = max_pt[1] - min_pt[1]
    dz = max_pt[2] - min_pt[2]
    radius = max(math.sqrt(dx * dx + dy * dy + dz * dz) * 0.5, 0.25)
    return TargetBounds(source=source, min=min_pt, max=max_pt, center=center, radius=radius)


def _actor_world_aabb(actor: Any) -> Optional[List[float]]:
    geometry = getattr(actor, "_geometry", None)
    get_aabb = getattr(geometry, "get_aabb", None)
    aabb = _coerce_aabb(get_aabb() if callable(get_aabb) else None)
    if aabb is None:
        return None
    get_position = getattr(actor, "get_position", None)
    get_scale = getattr(actor, "get_scale", None)
    pos = list(get_position() if callable(get_position) else [0.0, 0.0, 0.0])
    scale = list(get_scale() if callable(get_scale) else [1.0, 1.0, 1.0])
    while len(pos) < 3:
        pos.append(0.0)
    while len(scale) < 3:
        scale.append(1.0)
    mins = []
    maxs = []
    for axis in range(3):
        a = pos[axis] + aabb[axis] * scale[axis]
        b = pos[axis] + aabb[axis + 3] * scale[axis]
        mins.append(min(a, b))
        maxs.append(max(a, b))
    return [mins[0], mins[1], mins[2], maxs[0], maxs[1], maxs[2]]


def _find_actor(scene: Any, actor_name: Optional[str]) -> Any:
    if not scene or not actor_name:
        return None
    find_actor = getattr(scene, "find_actor", None)
    if callable(find_actor):
        actor = find_actor(actor_name)
        if actor is not None:
            return actor
    get_actors = getattr(scene, "get_actors", None)
    if callable(get_actors):
        try:
            for actor in get_actors() or []:
                if getattr(actor, "name", None) == actor_name or getattr(actor, "actor_id", None) == actor_name:
                    return actor
        except Exception:
            return None
    return None


def resolve_vlm_target_bounds(scene: Any, actor_name: Optional[str] = None, scope: str = "actor") -> TargetBounds:
    if scene is not None and scope == "actor":
        actor = _find_actor(scene, actor_name)
        actor_aabb = _actor_world_aabb(actor) if actor is not None else None
        if actor_aabb is not None:
            return _bounds_from_aabb(actor_aabb, f"actor:{actor_name or getattr(actor, 'name', '')}")

    if scene is not None:
        get_aabb = getattr(scene, "get_aabb", None)
        scene_aabb = _coerce_aabb(get_aabb() if callable(get_aabb) else None)
        if scene_aabb is not None:
            return _bounds_from_aabb(scene_aabb, "scene")

    return TargetBounds(
        source="default",
        min=[_DEFAULT_CENTER[0] - _DEFAULT_RADIUS, 0.0, _DEFAULT_CENTER[2] - _DEFAULT_RADIUS],
        max=[_DEFAULT_CENTER[0] + _DEFAULT_RADIUS, _DEFAULT_CENTER[1] * 2.0, _DEFAULT_CENTER[2] + _DEFAULT_RADIUS],
        center=list(_DEFAULT_CENTER),
        radius=_DEFAULT_RADIUS,
        valid=False,
    )


def _normalize(vec: List[float], fallback: List[float]) -> List[float]:
    length = math.sqrt(sum(v * v for v in vec))
    if length <= 1e-6:
        return list(fallback)
    return [v / length for v in vec]


def build_vlm_view_poses(bounds: TargetBounds) -> List[ViewPose]:
    center = list(bounds.center)
    radius = max(float(bounds.radius), 0.25)
    distance = max(radius * 2.6, 1.5)
    pose_specs = [
        ("front", 0.0, 25.0),
        ("right_front", 45.0, 25.0),
        ("left_back", 225.0, 25.0),
        ("top_oblique", 315.0, 55.0),
    ]
    poses: List[ViewPose] = []
    for name, azimuth_deg, elevation_deg in pose_specs:
        az = math.radians(azimuth_deg)
        el = math.radians(elevation_deg)
        cos_el = math.cos(el)
        position = [
            center[0] + distance * cos_el * math.sin(az),
            center[1] + distance * math.sin(el),
            center[2] + distance * cos_el * math.cos(az),
        ]
        forward = _normalize(
            [center[0] - position[0], center[1] - position[1], center[2] - position[2]],
            [0.0, 0.0, -1.0],
        )
        poses.append(ViewPose(
            name=name,
            position=position,
            forward=forward,
            up=[0.0, 1.0, 0.0],
            fov=45.0,
            azimuth_deg=azimuth_deg,
            elevation_deg=elevation_deg,
        ))
    return poses


def _snapshot_camera_state(camera: Any) -> Optional[dict]:
    if camera is None:
        return None
    try:
        return {
            "position": list(camera.get_position()),
            "forward": list(camera.get_forward()),
            "up": list(camera.get_world_up()),
            "fov": camera.get_fov(),
            "output_mode": camera.get_output_mode() if hasattr(camera, "get_output_mode") else None,
        }
    except Exception:
        return None


def _restore_camera_state(camera: Any, state: Optional[dict]) -> None:
    if camera is None or not state:
        return
    try:
        camera.set(state["position"], state["forward"], state["up"], state["fov"])
        if state.get("output_mode") is not None and hasattr(camera, "set_output_mode"):
            camera.set_output_mode(state["output_mode"])
    except Exception:
        pass


def _camera_state_changed(camera: Any, state: Optional[dict]) -> bool:
    if camera is None or not state:
        return False
    return _snapshot_camera_state(camera) != state


def _get_active_camera(scene: Any) -> Any:
    get_active = getattr(scene, "get_active_camera", None)
    if callable(get_active):
        return get_active()
    find_camera = getattr(scene, "find_camera", None)
    return find_camera(None) if callable(find_camera) else None


def _get_review_camera(scene: Any, review_camera: Any = None, camera_factory: Any = None) -> Any:
    if review_camera is not None:
        return review_camera
    try:
        from .model_reviewer import get_or_create_vlm_review_camera
    except ImportError:
        from model_reviewer import get_or_create_vlm_review_camera
    if camera_factory is None:
        active = _get_active_camera(scene)
        camera_factory = type(active) if active is not None else None
    return get_or_create_vlm_review_camera(scene, camera_factory=camera_factory)


def capture_pose_with_review_camera(
    scene: Any,
    camera: Any,
    pose: ViewPose,
    output_path: str,
    timeout_sec: float,
    view_index: int = 0,
) -> bool:
    try:
        camera.set(pose.position, pose.forward, pose.up, pose.fov)
        if hasattr(camera, "set_output_mode") and getattr(camera, "get_output_mode", lambda: VLM_OUTPUT_MODE)() != VLM_OUTPUT_MODE:
            camera.set_output_mode(VLM_OUTPUT_MODE)
        try:
            from .model_reviewer import _save_camera_screenshot_with_timeout
        except ImportError:
            from model_reviewer import _save_camera_screenshot_with_timeout
        engine_path, needs_copy = _engine_safe_screenshot_path(output_path, view_index)
        saved = bool(_save_camera_screenshot_with_timeout(camera, engine_path, timeout=timeout_sec))
        if not saved:
            return False
        return _copy_engine_screenshot_to_final(engine_path, output_path) if needs_copy else True
    except Exception as exc:
        logger.warning("[VlmCapture] capture pose failed name=%s path=%s error=%s", pose.name, output_path, exc)
        return False


def capture_vlm_views(
    scene_name: str,
    output_dir: str,
    actor_name: Optional[str] = None,
    scope: str = "actor",
    timeout_sec: float = 5.0,
    scene: Any = None,
    review_camera: Any = None,
    camera_factory: Any = None,
) -> VlmCaptureResult:
    output_dir = resolve_vlm_output_dir(output_dir)
    resolved_scene = _resolve_scene(scene_name, scene)
    if resolved_scene is None:
        return VlmCaptureResult(
            status="skipped",
            output_dir=output_dir,
            skipped_reason="scene_not_found",
        )

    camera = _get_review_camera(resolved_scene, review_camera=review_camera, camera_factory=camera_factory)
    if camera is None:
        return VlmCaptureResult(
            status="skipped",
            output_dir=output_dir,
            skipped_reason="review_camera_unavailable",
        )

    main_camera = _get_active_camera(resolved_scene)
    main_state = _snapshot_camera_state(main_camera)
    if main_camera is camera:
        return VlmCaptureResult(
            status="skipped",
            output_dir=output_dir,
            skipped_reason="review_camera_is_active_camera",
        )

    old_review_mode = None
    try:
        old_review_mode = camera.get_output_mode() if hasattr(camera, "get_output_mode") else None
        if hasattr(camera, "set_output_mode") and old_review_mode != VLM_OUTPUT_MODE:
            camera.set_output_mode(VLM_OUTPUT_MODE)
    except Exception:
        old_review_mode = None

    bounds = resolve_vlm_target_bounds(resolved_scene, actor_name=actor_name, scope=scope)
    poses = build_vlm_view_poses(bounds)
    files: List[str] = []
    try:
        for index, pose in enumerate(poses):
            safe_target = (actor_name or scope or "scene").replace(os.sep, "_")
            output_path = os.path.join(output_dir, f"{safe_target}_{index:02d}_{pose.name}_{VLM_OUTPUT_MODE}.png")
            if capture_pose_with_review_camera(resolved_scene, camera, pose, output_path, timeout_sec, index):
                files.append(output_path)
    finally:
        if old_review_mode and old_review_mode != VLM_OUTPUT_MODE:
            try:
                camera.set_output_mode(old_review_mode)
            except Exception:
                pass
        if _camera_state_changed(main_camera, main_state):
            _restore_camera_state(main_camera, main_state)
            logger.warning("[VlmCapture] main camera changed during capture; restored and skipped result")
            return VlmCaptureResult(
                status="skipped",
                output_dir=output_dir,
                files=[],
                target_bounds=bounds,
                center=list(bounds.center),
                radius=bounds.radius,
                poses=poses,
                camera_name=getattr(camera, "name", VLM_REVIEW_CAMERA_NAME),
                skipped_reason="main_camera_leak",
            )

    status = "success" if files else "skipped"
    skipped_reason = "" if files else "no_screenshots_saved"
    logger.info(
        "[VlmCapture] scope=%s target=%s source=%s center=%s radius=%.3f output_mode=%s view_count=%d success=%d",
        scope,
        actor_name or "",
        bounds.source,
        bounds.center,
        bounds.radius,
        VLM_OUTPUT_MODE,
        VLM_VIEW_COUNT,
        len(files),
    )
    return VlmCaptureResult(
        status=status,
        output_dir=output_dir,
        files=files,
        target_bounds=bounds,
        center=list(bounds.center),
        radius=bounds.radius,
        poses=poses,
        camera_name=getattr(camera, "name", VLM_REVIEW_CAMERA_NAME),
        skipped_reason=skipped_reason,
    )
