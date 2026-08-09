from __future__ import annotations

import json
from typing import List, Optional, Tuple

from langchain_core.tools import StructuredTool
from pydantic import BaseModel, Field

from Quasar.ai_tools.response_adapter import build_error_result, build_part, build_success_result
from .native_scene_state import find_native_actor, native_actor_views, set_native_actor_transform
from .transform_grounding import resolve_actor_overlaps, snap_actor_to_ground

DEFAULT_SCENE_NAME = ""


class SetActorTransformInput(BaseModel):
    scene_name: str = Field(default=DEFAULT_SCENE_NAME, description="Target scene name; empty means current scene")
    actor_name: str = Field(description="Actor name")
    position: Optional[Tuple[float, float, float]] = Field(default=None, description="Absolute position")
    rotation: Optional[Tuple[float, float, float]] = Field(default=None, description="Absolute rotation")
    scale: Optional[Tuple[float, float, float]] = Field(default=None, description="Absolute scale")
    snap_to_ground: bool = Field(default=True, description="Snap to ground using native bounds when ready")
    ground_y: float = Field(default=0.0, description="Ground height")
    ground_clearance: float = Field(default=0.02, description="Ground clearance")


def _build_set_actor_transform_tool(_scene_manager=None) -> StructuredTool:
    def _set_actor_transform(*, scene_name: str = DEFAULT_SCENE_NAME, actor_name: str, position: Optional[Tuple[float, float, float]] = None, rotation: Optional[Tuple[float, float, float]] = None, scale: Optional[Tuple[float, float, float]] = None, snap_to_ground: bool = True, ground_y: float = 0.0, ground_clearance: float = 0.02) -> str:
        try:
            if position is None and rotation is None and scale is None:
                return build_error_result(error_message="position, rotation, or scale is required").to_envelope(interface_type="scene")
            actor = find_native_actor(scene_name, actor_name, wait_for_bounds=False)
            if actor is None:
                return build_error_result(error_message=f"Actor {actor_name!r} not found").to_envelope(interface_type="scene")
            result = set_native_actor_transform(scene_name or actor.scene_name, actor.name, position=position, rotation=rotation, scale=scale)
            updated = result.get("actor") if isinstance(result.get("actor"), dict) else None
            actor = find_native_actor(result.get("scene") or scene_name, actor.name, wait_for_bounds=bool(snap_to_ground and (position is not None or scale is not None))) or actor
            snap_position = None
            overlap_result = None
            skipped_reason = ""
            if snap_to_ground and (position is not None or scale is not None):
                if not actor.bounds_ready:
                    skipped_reason = "bounds_not_ready"
                else:
                    snap_position = snap_actor_to_ground(actor, ground_y=ground_y, clearance=ground_clearance)
                    obstacles = [other for other in native_actor_views(actor.scene_name) if other.name != actor.name]
                    overlap_result = resolve_actor_overlaps(actor, obstacles)
                    actor.refresh()
                    updated = actor.data
            actor_data = updated or actor.data
            if isinstance(actor_data, dict):
                actor_data = dict(actor_data)
                actor_data.setdefault("sync_status", "engine_transformed")
                actor_data.setdefault("sync_lifecycle_status", "engine_transformed")
            actor_id = ""
            if isinstance(actor_data, dict):
                actor_id = str(
                    actor_data.get("actor_guid")
                    or actor_data.get("actor_id")
                    or actor_data.get("guid")
                    or ""
                )
            payload = {
                "actor_id": actor_id,
                "actor": actor.name,
                "position": actor.get_position(),
                "rotation": actor.get_rotation(),
                "scale": actor.get_scale(),
                "bounds_ready": actor.bounds_ready,
                "sync_status": "engine_transformed",
                "sync_lifecycle_status": "engine_transformed",
                "ground_snapped": snap_position is not None,
                "overlap_resolved": bool(overlap_result and overlap_result.get("changed")),
                "skipped_reason": skipped_reason,
                "actor_data": actor_data,
            }
            part = build_part(content_type="text", content_text=json.dumps(payload, ensure_ascii=False))
            return build_success_result(parts=[part]).to_envelope(interface_type="scene")
        except Exception as exc:
            return build_error_result(error_message=str(exc)).to_envelope(interface_type="scene")

    return StructuredTool(name="set_actor_transform", description="Set native actor absolute position/rotation/scale and optionally snap/resolve using native bounds.", args_schema=SetActorTransformInput, func=_set_actor_transform)


def load_set_actor_transform_tools() -> List[StructuredTool]:
    return [_build_set_actor_transform_tool()]


__all__ = ["load_set_actor_transform_tools"]
