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
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, List, Optional

logger = logging.getLogger(__name__)

VLM_OUTPUT_MODE = "base_color"
VLM_VIEW_COUNT = 4
VLM_REVIEW_CAMERA_NAME = "vlm_review_camera"

_DEFAULT_CENTER = [0.0, 0.75, 0.0]
_DEFAULT_RADIUS = 2.0
_READY_POLL_INTERVAL = 0.1
_CAPTURE_FILE_GRACE_SEC = float(os.getenv("CORONA_VLM_CAPTURE_FILE_GRACE_SEC", "45.0") or "45.0")


@dataclass
class NativeSceneRef:
    route: str = ""
    name: str = ""


def get_project_screenshots_root() -> Path:
    """Return the active project's screenshots directory."""
    try:
        from config.paths_config import get_project_screenshots_dir
        return Path(get_project_screenshots_dir())
    except Exception as exc:
        logger.debug("[VlmCapture] project screenshots resolver unavailable: %s", exc)

    override = os.environ.get("CAI_SCREENSHOTS_DIR")
    if override:
        root = Path(override)
    else:
        project_root = os.environ.get("CAI_PROJECT_ROOT")
        if project_root:
            root = Path(project_root) / "screenshots"
        else:
            from runtime import project_context

            root = project_context.get_project_root() / "screenshots"
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


def _screenshot_file_ready(path: str) -> bool:
    try:
        return os.path.exists(path) and os.path.getsize(path) > 0
    except OSError:
        return False


def _wait_for_engine_screenshot_file(
    engine_path: str,
    final_path: str,
    *,
    needs_copy: bool,
    grace_sec: Optional[float] = None,
) -> bool:
    deadline = time.monotonic() + max(0.0, _CAPTURE_FILE_GRACE_SEC if grace_sec is None else float(grace_sec))
    while True:
        if _screenshot_file_ready(engine_path):
            return _copy_engine_screenshot_to_final(engine_path, final_path) if needs_copy else True
        if _screenshot_file_ready(final_path):
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(_READY_POLL_INTERVAL)


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
        from ..mcp.tools.native_scene_state import resolve_scene_value

        resolved = resolve_scene_value(scene_name or "")
        if resolved is not None:
            return resolved
    except Exception as exc:
        logger.debug("[VlmCapture] aggregate scene resolver unavailable: %s", exc)
    route = str(scene_name or "").strip()
    return NativeSceneRef(route=route, name=route)


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


def _native_scene_route(scene: Any) -> str:
    return str(getattr(scene, "route", "") or getattr(scene, "name", "") or "")


def _editor_api():
    try:
        from api.editor_api import CoronaEditorApi
    except Exception as exc:
        logger.debug("[VlmCapture] editor aggregate API unavailable: %s", exc)
        return None

    return CoronaEditorApi


def _native_scene_snapshot(scene: Any) -> Optional[dict]:
    api = _editor_api()
    getter = getattr(getattr(api, "scene", None), "get_snapshot", None) if api else None
    if not callable(getter):
        return None
    try:
        result = getter(_native_scene_route(scene))
    except Exception as exc:
        logger.debug("[VlmCapture] scene snapshot aggregate call failed error=%s", exc)
        return None
    return result if isinstance(result, dict) else None


def _native_actor_record(snapshot: Optional[dict], actor_name: Optional[str]) -> Optional[dict]:
    if not actor_name or not isinstance(snapshot, dict):
        return None
    actors = snapshot.get("actors") if isinstance(snapshot.get("actors"), list) else []
    wanted = str(actor_name)
    shell_wanted = f"__shell_{wanted}" if not wanted.startswith("__shell_") else wanted
    for actor in actors:
        if not isinstance(actor, dict):
            continue
        names = {
            str(actor.get("name") or ""),
            str(actor.get("actor_guid") or ""),
            str(actor.get("handle") or ""),
        }
        if wanted in names or shell_wanted in names:
            return actor
        if wanted.casefold() and any(name.casefold() == wanted.casefold() for name in names if name):
            return actor
    return None


def _native_actor_aabb(scene: Any, actor_name: Optional[str]) -> Optional[List[float]]:
    actor = _native_actor_record(_native_scene_snapshot(scene), actor_name)
    return _coerce_aabb(actor.get("world_aabb")) if actor else None


def _native_actor_geometry_status(scene: Any, actor_name: Optional[str]) -> Optional[dict]:
    snapshot = _native_scene_snapshot(scene)
    actor = _native_actor_record(snapshot, actor_name)
    if actor is None:
        return None
    return {
        "status": snapshot.get("status", "success") if isinstance(snapshot, dict) else "success",
        "ready": bool(actor.get("render_ready", actor.get("bounds_ready"))),
        "failed": bool(actor.get("render_failed")),
        "gpu_build_state": actor.get("gpu_build_state", ""),
    }


def _wait_for_native_actor_geometry_ready(
    scene: Any,
    actor_name: Optional[str],
    timeout_sec: float,
) -> tuple[bool, str]:
    if not actor_name:
        return True, ""
    status = _native_actor_geometry_status(scene, actor_name)
    if status is None:
        return True, "geometry_status_unavailable"

    deadline = time.monotonic() + max(0.0, float(timeout_sec or 0.0))
    last_reason = ""
    while True:
        state = str(status.get("gpu_build_state") or "").strip()
        if status.get("ready"):
            return True, state or "ready"
        if status.get("failed") or state.lower() == "failed":
            return False, f"geometry_failed:{state or status.get('message') or actor_name}"
        if status.get("status") == "error":
            last_reason = str(status.get("message") or "geometry_status_error")
        else:
            last_reason = state or "geometry_not_ready"
        if time.monotonic() >= deadline:
            return False, f"geometry_not_ready:{last_reason}"
        time.sleep(_READY_POLL_INTERVAL)
        next_status = _native_actor_geometry_status(scene, actor_name)
        if next_status is None:
            return True, "geometry_status_unavailable"
        status = next_status


def _scene_snapshot_bounds_ready(snapshot: Optional[dict]) -> tuple[bool, str]:
    if not isinstance(snapshot, dict):
        return True, "scene_snapshot_unavailable"
    if snapshot.get("status") == "error":
        return False, str(snapshot.get("message") or "scene_snapshot_error")
    actors = snapshot.get("actors") if isinstance(snapshot.get("actors"), list) else []
    if actors:
        pending: list[str] = []
        for actor in actors:
            if not isinstance(actor, dict):
                continue
            aabb = _coerce_aabb(actor.get("world_aabb"))
            if not actor.get("bounds_ready") or aabb is None:
                pending.append(str(actor.get("name") or actor.get("actor_guid") or "actor"))
        if not pending:
            return True, "ready"
        return False, "pending_bounds:" + ",".join(pending[:5])
    if snapshot.get("bounds_ready") or _coerce_aabb(snapshot.get("scene_aabb")) is not None:
        return True, "ready"
    return True, "scene_empty_or_unavailable"


def _wait_for_native_scene_geometry_ready(scene: Any, timeout_sec: float) -> tuple[bool, str]:
    status = _native_scene_snapshot(scene)
    ready, reason = _scene_snapshot_bounds_ready(status)
    if ready:
        return True, reason

    deadline = time.monotonic() + max(0.0, float(timeout_sec or 0.0))
    last_reason = reason
    while True:
        if time.monotonic() >= deadline:
            return False, f"scene_geometry_not_ready:{last_reason}"
        time.sleep(_READY_POLL_INTERVAL)
        status = _native_scene_snapshot(scene)
        ready, last_reason = _scene_snapshot_bounds_ready(status)
        if ready:
            return True, last_reason


def _native_scene_aabb(scene: Any) -> Optional[List[float]]:
    snapshot = _native_scene_snapshot(scene)
    return _coerce_aabb(snapshot.get("scene_aabb")) if snapshot else None


def resolve_vlm_target_bounds(scene: Any, actor_name: Optional[str] = None, scope: str = "actor") -> TargetBounds:
    if scene is not None and scope == "actor":
        actor_aabb = _native_actor_aabb(scene, actor_name)
        if actor_aabb is not None:
            return _bounds_from_aabb(actor_aabb, f"native_actor:{actor_name or ''}")

    if scene is not None:
        scene_aabb = _native_scene_aabb(scene)
        if scene_aabb is not None:
            return _bounds_from_aabb(scene_aabb, "native_scene")

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


def _native_camera_capture_available() -> bool:
    api = _editor_api()
    return callable(getattr(getattr(api, "viewport", None), "capture", None)) if api else False


def capture_pose_with_native_camera(
    scene: Any,
    pose: ViewPose,
    output_path: str,
    timeout_sec: float,
    view_index: int = 0,
) -> bool:
    try:
        api = _editor_api()
        capture_viewport = getattr(getattr(api, "viewport", None), "capture", None) if api else None
        if not callable(capture_viewport):
            logger.warning("[VlmCapture] viewport aggregate API unavailable")
            return False

        engine_path, needs_copy = _engine_safe_screenshot_path(output_path, view_index)
        camera_data = {
            "position": pose.position,
            "forward": pose.forward,
            "world_up": pose.up,
            "fov": pose.fov,
            "width": 512,
            "height": 512,
            "output_mode": VLM_OUTPUT_MODE,
            "render_backend": "native",
        }
        result = capture_viewport(
            _native_scene_route(scene),
            VLM_REVIEW_CAMERA_NAME,
            camera_data,
            engine_path,
        )
        saved = isinstance(result, dict) and result.get("status") in ("success", "ok")
        if saved:
            if not needs_copy:
                return True
            if _copy_engine_screenshot_to_final(engine_path, output_path):
                return True
            if _wait_for_engine_screenshot_file(engine_path, output_path, needs_copy=True):
                return True
            logger.warning("[VlmCapture] native camera reported success but screenshot file was not ready result=%s", result)
            return False
        if _wait_for_engine_screenshot_file(engine_path, output_path, needs_copy=needs_copy):
            logger.info("[VlmCapture] accepted late native screenshot after error path=%s result=%s", output_path, result)
            return True
        logger.warning("[VlmCapture] native camera capture failed result=%s", result)
        return False
    except Exception as exc:
        logger.warning("[VlmCapture] native capture pose failed name=%s path=%s error=%s",
                       pose.name, output_path, exc)
        return False


def capture_vlm_views(
    scene_name: str,
    output_dir: str,
    actor_name: Optional[str] = None,
    scope: str = "actor",
    timeout_sec: float = 5.0,
    scene: Any = None,
) -> VlmCaptureResult:
    output_dir = resolve_vlm_output_dir(output_dir)
    resolved_scene = _resolve_scene(scene_name, scene)
    if resolved_scene is None:
        return VlmCaptureResult(
            status="skipped",
            output_dir=output_dir,
            skipped_reason="scene_not_found",
        )

    if not _native_camera_capture_available():
        return VlmCaptureResult(
            status="skipped",
            output_dir=output_dir,
            skipped_reason="native_camera_capture_unavailable",
        )

    if scope == "actor" and actor_name:
        ready, ready_reason = _wait_for_native_actor_geometry_ready(
            resolved_scene,
            actor_name,
            timeout_sec=timeout_sec,
        )
        if not ready:
            logger.info(
                "[VlmCapture] skip target=%s before screenshot: %s",
                actor_name,
                ready_reason,
            )
            return VlmCaptureResult(
                status="skipped",
                output_dir=output_dir,
                skipped_reason=f"native_actor_not_ready:{ready_reason}",
            )

    if scope != "actor":
        ready, ready_reason = _wait_for_native_scene_geometry_ready(
            resolved_scene,
            timeout_sec=timeout_sec,
        )
        if not ready:
            logger.info(
                "[VlmCapture] skip scene before screenshot: %s",
                ready_reason,
            )
            return VlmCaptureResult(
                status="skipped",
                output_dir=output_dir,
                skipped_reason=f"native_scene_not_ready:{ready_reason}",
            )

    bounds = resolve_vlm_target_bounds(resolved_scene, actor_name=actor_name, scope=scope)
    if scope == "actor" and actor_name and bounds.source != f"native_actor:{actor_name}":
        return VlmCaptureResult(
            status="skipped",
            output_dir=output_dir,
            target_bounds=bounds,
            center=list(bounds.center),
            radius=bounds.radius,
            poses=[],
            camera_name=VLM_REVIEW_CAMERA_NAME,
            skipped_reason=f"native_actor_not_found:{actor_name}",
        )
    poses = build_vlm_view_poses(bounds)
    files: List[str] = []
    capture_failure_reason = ""
    for index, pose in enumerate(poses):
        safe_target = (actor_name or scope or "scene").replace(os.sep, "_")
        output_path = os.path.join(output_dir, f"{safe_target}_{index:02d}_{pose.name}_{VLM_OUTPUT_MODE}.png")
        if capture_pose_with_native_camera(resolved_scene, pose, output_path, timeout_sec, index):
            files.append(output_path)
            continue
        capture_failure_reason = (
            "native_capture_failed:first_view"
            if not files else f"native_capture_failed:view_{index}"
        )
        logger.warning(
            "[VlmCapture] stopping VLM capture after native failure target=%s view=%s reason=%s",
            actor_name or "",
            pose.name,
            capture_failure_reason,
        )
        break

    status = "success" if files else "skipped"
    skipped_reason = "" if files else (capture_failure_reason or "no_screenshots_saved")
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
        camera_name=VLM_REVIEW_CAMERA_NAME,
        skipped_reason=skipped_reason,
    )
