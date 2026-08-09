from __future__ import annotations

import json
from typing import Literal, List, Optional, Tuple

from api.editor_api import CoronaEditorApi
from langchain_core.tools import StructuredTool
from pydantic import BaseModel, Field

from Quasar.ai_tools.response_adapter import build_error_result, build_part, build_success_result
from .native_scene_state import (
    find_native_actor,
    get_native_scene_snapshot,
    native_actor_views,
    set_native_actor_transform,
)
from .transform_grounding import resolve_actor_overlaps, snap_actor_to_ground

DEFAULT_SCENE_NAME = ""


class SceneQueryInput(BaseModel):
    scene_name: str = Field(default=DEFAULT_SCENE_NAME, description="Target scene name")
    query: Literal["list_models", "get_model_by_name"] = Field(description="Query type")
    name: str | None = Field(default=None, description="Actor name")


class TransformModelInput(BaseModel):
    scene_name: str = Field(default=DEFAULT_SCENE_NAME, description="Target scene name")
    model_name: str = Field(description="Actor name")
    operation: Literal["scale", "move", "rotate", "scale_delta", "translate", "rotate_delta"] = Field(default="scale", description="Transform operation")
    scale_factor: float | None = Field(default=None, description="Scale factor")
    vector: Tuple[float, float, float] | None = Field(default=None, description="Vector")
    snap_to_ground: bool = Field(default=True, description="Snap using native bounds when ready")
    ground_y: float = Field(default=0.0, description="Ground height")
    ground_clearance: float = Field(default=0.02, description="Ground clearance")


class SceneActorsInput(BaseModel):
    scene_name: str = Field(default=DEFAULT_SCENE_NAME, description="Target scene name")
    actor_name: str | None = Field(default=None, description="Optional actor name")


class SceneListInput(BaseModel):
    pass


def _json_result(payload: dict) -> str:
    part = build_part(content_type="text", content_text=json.dumps(payload, ensure_ascii=False))
    return build_success_result(parts=[part]).to_envelope(interface_type="scene")


def _build_scene_query_tool(scene_manager) -> StructuredTool:
    def _query_scene(*, scene_name: str = DEFAULT_SCENE_NAME, query: Literal["list_models", "get_model_by_name"], name: str | None = None) -> str:
        try:
            snapshot = get_native_scene_snapshot(scene_name)
            actors = snapshot.get("actors") if isinstance(snapshot.get("actors"), list) else []
            if query == "list_models":
                return _json_result({"scene": snapshot.get("scene", scene_name), "actors": [a.get("name") for a in actors if isinstance(a, dict)]})
            if query == "get_model_by_name":
                actor = find_native_actor(scene_name, name or "")
                return _json_result({"scene": snapshot.get("scene", scene_name), "actor": actor.name if actor else None, "model_path": actor.model_path if actor else "", "found": actor is not None, "actor_data": actor.data if actor else None})
            return build_error_result(error_message=f"Unsupported query type: {query}").to_envelope(interface_type="scene")
        except Exception as exc:
            return build_error_result(error_message=str(exc)).to_envelope(interface_type="scene")
    return StructuredTool(name="scene_query", description="Query native scene actors.", args_schema=SceneQueryInput, func=_query_scene)


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


def _build_scene_actors_tool(scene_manager) -> StructuredTool:
    def _scene_actors(*, scene_name: str = DEFAULT_SCENE_NAME, actor_name: str | None = None) -> str:
        try:
            if actor_name:
                actor = find_native_actor(scene_name, actor_name)
                if actor is None:
                    return build_error_result(error_message=f"Actor {actor_name!r} not found").to_envelope(interface_type="scene")
                return _json_result(actor.data)
            snapshot = get_native_scene_snapshot(scene_name)
            actors = snapshot.get("actors") if isinstance(snapshot.get("actors"), list) else []
            return _json_result({"scene": snapshot.get("scene", scene_name), "count": len(actors), "actors": actors, "scene_aabb": snapshot.get("scene_aabb"), "bounds_ready": bool(snapshot.get("bounds_ready"))})
        except Exception as exc:
            return build_error_result(error_message=str(exc)).to_envelope(interface_type="scene")
    return StructuredTool(name="scene_get_actors", description="Return authoritative native scene actor data.", args_schema=SceneActorsInput, func=_scene_actors)


def _build_scene_list_tool(scene_manager) -> StructuredTool:
    def _scene_list() -> str:
        try:
            raw = CoronaEditorApi.scene.list_routes()
            payload = json.loads(raw) if isinstance(raw, str) else raw
            if not isinstance(payload, dict) or payload.get("status") == "error":
                raise RuntimeError(
                    str((payload or {}).get("message", "Native scene route query failed"))
                )
            scenes = payload.get("scenes")
            if not isinstance(scenes, list):
                scenes = []
            return _json_result(
                {
                    "count": len(scenes),
                    "scenes": [
                        {
                            "route": item.get("path", ""),
                            "name": item.get("name", ""),
                        }
                        for item in scenes
                        if isinstance(item, dict)
                    ],
                    "active_index": payload.get("active_index", 0),
                    "entrance_scene": payload.get("entrance_scene", ""),
                    "active_scene": payload.get("active_scene", ""),
                }
            )
        except Exception as exc:
            return build_error_result(error_message=str(exc)).to_envelope(interface_type="scene")
    return StructuredTool(name="scene_list", description="List loaded scenes.", args_schema=SceneListInput, func=_scene_list)


def load_scene_tools(scene_manager=None) -> List[StructuredTool]:
    """Register native scene tools without importing the legacy manager.

    The optional argument remains for old registries and test doubles.  Native
    scene reads and writes are handled by ``native_scene_state``.
    """
    return [_build_scene_list_tool(scene_manager), _build_scene_actors_tool(scene_manager), _build_scene_query_tool(scene_manager), _build_transform_tool(scene_manager)]


__all__ = ["load_scene_tools"]
