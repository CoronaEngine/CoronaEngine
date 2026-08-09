from __future__ import annotations

import json
from typing import List

from langchain_core.tools import StructuredTool
from pydantic import BaseModel, Field

from Quasar.ai_tools.response_adapter import build_error_result, build_part, build_success_result
from .native_scene_state import get_native_scene_snapshot

DEFAULT_SCENE_NAME = ""


class SceneSnapshotInput(BaseModel):
    scene_name: str = Field(default=DEFAULT_SCENE_NAME, description="Target scene name; empty means current scene")
    wait_for_bounds: bool = Field(default=False, description="Wait briefly for native actor bounds before returning")


def _build_scene_snapshot_tool(_scene_manager=None) -> StructuredTool:
    def _scene_snapshot(*, scene_name: str = DEFAULT_SCENE_NAME, wait_for_bounds: bool = False) -> str:
        try:
            snapshot = get_native_scene_snapshot(scene_name, wait_for_bounds=wait_for_bounds)
            result_data = {
                "scene": snapshot.get("scene", scene_name),
                "scene_name": snapshot.get("scene_name", scene_name),
                "actor_count": snapshot.get("actor_count", len(snapshot.get("actors") or [])),
                "actors": snapshot.get("actors") or [],
                "scene_aabb": snapshot.get("scene_aabb"),
                "bounds_ready": bool(snapshot.get("bounds_ready")),
            }
            part = build_part(content_type="text", content_text=json.dumps(result_data, ensure_ascii=False))
            return build_success_result(parts=[part]).to_envelope(interface_type="scene")
        except Exception as exc:
            return build_error_result(error_message=str(exc)).to_envelope(interface_type="scene")

    return StructuredTool(
        name="get_scene_snapshot",
        description="Return the authoritative native scene snapshot, including actors, transforms, local/world AABB, sizes, and bounds_ready.",
        args_schema=SceneSnapshotInput,
        func=_scene_snapshot,
    )


def load_scene_snapshot_tools() -> List[StructuredTool]:
    return [_build_scene_snapshot_tool()]


__all__ = ["load_scene_snapshot_tools"]
