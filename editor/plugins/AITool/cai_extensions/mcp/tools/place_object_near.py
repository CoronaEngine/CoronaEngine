from __future__ import annotations

import json
from typing import List, Literal, Tuple

from langchain_core.tools import StructuredTool
from pydantic import BaseModel, Field

from Quasar.ai_tools.response_adapter import build_error_result, build_part, build_success_result
from .native_scene_state import find_native_actor, native_actor_views

DEFAULT_SCENE_NAME = ""


def calculate_position(ref_aabb: Tuple[float, float, float, float, float, float], relation: str, gap_m: float, obj_half_dx: float = 0.25, obj_half_dz: float = 0.25) -> List[float]:
    min_x, min_y, min_z, max_x, max_y, max_z = ref_aabb
    cx = (min_x + max_x) / 2.0
    cz = (min_z + max_z) / 2.0
    relation = relation.lower()
    if relation == "left":
        return [min_x - gap_m - obj_half_dx, 0.0, cz]
    if relation == "right":
        return [max_x + gap_m + obj_half_dx, 0.0, cz]
    if relation == "front":
        return [cx, 0.0, min_z - gap_m - obj_half_dz]
    if relation == "behind":
        return [cx, 0.0, max_z + gap_m + obj_half_dz]
    if relation == "above":
        return [cx, max_y + gap_m, cz]
    if relation == "below":
        return [cx, min_y - gap_m, cz]
    raise ValueError(f"Unknown relation: {relation}")


class PlaceObjectNearInput(BaseModel):
    object_id: str = Field(description="Object id to place")
    model_path: str = Field(description="Model path to place")
    reference_actor: str = Field(description="Existing native actor name")
    relation: Literal["left", "right", "front", "behind", "above", "below"] = Field(description="Spatial relation")
    gap_m: float = Field(default=0.3, description="Gap in meters")
    scene_name: str = Field(default=DEFAULT_SCENE_NAME, description="Target scene name; empty means current scene")


def _build_place_object_near_tool(scene_manager) -> StructuredTool:
    def _place_object_near(*, object_id: str, model_path: str, reference_actor: str, relation: str, gap_m: float = 0.3, scene_name: str = DEFAULT_SCENE_NAME) -> str:
        try:
            ref = find_native_actor(scene_name, reference_actor, wait_for_bounds=True)
            if ref is None:
                available = [actor.name for actor in native_actor_views(scene_name)]
                return build_error_result(error_message=f"Reference actor {reference_actor!r} not found. Available: {available}").to_envelope(interface_type="scene")
            if not ref.bounds_ready:
                payload = {"reference_actor": reference_actor, "bounds_ready": False, "message": "Reference native bounds are not ready; placement was not calculated."}
                part = build_part(content_type="text", content_text=json.dumps(payload, ensure_ascii=False))
                return build_success_result(parts=[part]).to_envelope(interface_type="scene")
            ref_aabb = tuple(ref.get_world_aabb())
            pos = calculate_position(ref_aabb, relation, gap_m)
            payload = {
                "object_id": object_id,
                "model_path": model_path,
                "reference_actor": ref.name,
                "reference_position": ref.get_position(),
                "reference_aabb": {"min": list(ref_aabb[:3]), "max": list(ref_aabb[3:])},
                "bounds_ready": True,
                "relation": relation,
                "gap_m": gap_m,
                "calculated_position": pos,
                "rotation": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
            }
            part = build_part(content_type="text", content_text=json.dumps(payload, ensure_ascii=False))
            return build_success_result(parts=[part]).to_envelope(interface_type="scene")
        except Exception as exc:
            return build_error_result(error_message=str(exc)).to_envelope(interface_type="scene")

    return StructuredTool(name="place_object_near", description="Calculate a placement position from a native reference actor world AABB.", args_schema=PlaceObjectNearInput, func=_place_object_near)


def load_place_object_near_tools(scene_manager=None) -> List[StructuredTool]:
    """Register native placement tools without resolving a legacy SceneManager.

    ``scene_manager`` remains an ignored compatibility parameter for old tool
    registries; all scene reads are performed by ``native_scene_state``.
    """
    return [_build_place_object_near_tool(scene_manager)]


__all__ = ["load_place_object_near_tools", "calculate_position"]
