from __future__ import annotations

import json
from typing import Literal, List, Tuple

from langchain_core.tools import StructuredTool
from pydantic import BaseModel, Field

from Quasar.ai_tools.response_adapter import build_error_result, build_part, build_success_result
from .native_scene_state import (
    find_native_actor,
    native_actor_views,
    set_native_actor_transform,
)
from .transform_grounding import resolve_actor_overlaps, snap_actor_to_ground

DEFAULT_SCENE_NAME = ""


class TransformModelInput(BaseModel):
    scene_name: str = Field(default=DEFAULT_SCENE_NAME, description="Target scene name")
    model_name: str = Field(description="Actor name")
    operation: Literal["scale", "move", "rotate", "scale_delta", "translate", "rotate_delta"] = Field(default="scale", description="Transform operation")
    scale_factor: float | None = Field(default=None, description="Scale factor")
    vector: Tuple[float, float, float] | None = Field(default=None, description="Vector")
    snap_to_ground: bool = Field(default=True, description="Snap using native bounds when ready")
    ground_y: float = Field(default=0.0, description="Ground height")
    ground_clearance: float = Field(default=0.02, description="Ground clearance")


def _json_result(payload: dict) -> str:
    part = build_part(content_type="text", content_text=json.dumps(payload, ensure_ascii=False))
    return build_success_result(parts=[part]).to_envelope(interface_type="scene")


def _build_transform_tool(scene_manager) -> StructuredTool:
    def _transform_model(*, scene_name: str = DEFAULT_SCENE_NAME, model_name: str, operation: Literal["scale", "move", "rotate", "scale_delta", "translate", "rotate_delta"] = "scale", scale_factor: float | None = None, vector: Tuple[float, float, float] | None = None, snap_to_ground: bool = True, ground_y: float = 0.0, ground_clearance: float = 0.02) -> str:
        try:
            actor = find_native_actor(scene_name, model_name, wait_for_bounds=False)
            if actor is None:
                return build_error_result(error_message=f"Actor {model_name!r} not found").to_envelope(interface_type="scene")
            op = operation.lower()
            pos = actor.get_position()
            rot = actor.get_rotation()
            scl = actor.get_scale()
            if op in {"scale", "scale_delta"}:
                if scale_factor is not None:
                    scl = [v * float(scale_factor) for v in scl]
                elif vector is not None:
                    scl = [scl[i] * float(vector[i]) for i in range(3)]
                else:
                    raise ValueError("scale operation requires scale_factor or vector")
            elif op in {"move", "translate"}:
                if vector is None:
                    raise ValueError("move operation requires vector")
                pos = [pos[i] + float(vector[i]) for i in range(3)]
            elif op in {"rotate", "rotate_delta"}:
                if vector is None:
                    raise ValueError("rotate operation requires vector")
                rot = [rot[i] + float(vector[i]) for i in range(3)]
            else:
                return build_error_result(error_message=f"Unsupported operation {operation!r}").to_envelope(interface_type="scene")
            set_native_actor_transform(scene_name or actor.scene_name, actor.name, position=pos, rotation=rot, scale=scl)
            actor = find_native_actor(scene_name or actor.scene_name, actor.name, wait_for_bounds=bool(snap_to_ground and op in {"scale", "scale_delta", "move", "translate"})) or actor
            snap_position = None
            overlap_result = None
            skipped_reason = ""
            if snap_to_ground and op in {"scale", "scale_delta", "move", "translate"}:
                if actor.bounds_ready:
                    snap_position = snap_actor_to_ground(actor, ground_y=ground_y, clearance=ground_clearance)
                    obstacles = [other for other in native_actor_views(actor.scene_name) if other.name != actor.name]
                    overlap_result = resolve_actor_overlaps(actor, obstacles)
                    actor.refresh()
                else:
                    skipped_reason = "bounds_not_ready"
            return _json_result({"actor": actor.name, "operation": op, "position": actor.get_position(), "rotation": actor.get_rotation(), "scale": actor.get_scale(), "bounds_ready": actor.bounds_ready, "ground_snapped": snap_position is not None, "overlap_resolved": bool(overlap_result and overlap_result.get("changed")), "skipped_reason": skipped_reason})
        except Exception as exc:
            return build_error_result(error_message=str(exc)).to_envelope(interface_type="scene")
    return StructuredTool(name="transform_model", description="Apply relative transform to a native actor.", args_schema=TransformModelInput, func=_transform_model)


def load_scene_tools(scene_manager=None) -> List[StructuredTool]:
    """Register native scene tools without importing the legacy manager.

    The optional argument remains for old registries and test doubles.  Native
    scene reads and writes are handled by ``native_scene_state``.
    """
    return [_build_transform_tool(scene_manager)]


__all__ = ["load_scene_tools"]
