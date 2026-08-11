"""Canonical ownership and exposure boundaries for AI tools.

The catalog separates tools that a general assistant may request from tools
that belong to the AgentRuntime orchestration loop or directly touch the
editor engine.  It intentionally contains names, not implementations, so the
boundary can be checked without importing optional AI or editor dependencies.
"""

from __future__ import annotations

from typing import Literal


ToolLayer = Literal["public", "runtime_internal", "engine_native", "unclassified"]


RUNTIME_INTERNAL_PREFIXES = frozenset(
    {
        "runtime.",
        "scene.",
        "room.",
        "zone.",
        "asset.",
        "placement.",
        "batch.",
    }
)


ENGINE_NATIVE_TOOLS = frozenset(
    {
        "camera_move",
        "camera_get",
        "camera_focus",
        "camera_list",
        "camera_screenshot",
        "camera_multiview_capture",
        "import_model",
        "import_environment_component",
        "remove_model",
        "get_scene_snapshot",
        "set_actor_transform",
        "transform_model",
        "scene_rationality_review",
    }
)


PUBLIC_TOOLS = frozenset(
    {
        "generate_product_text",
        "generate_marketing_text",
        "generate_creative_text",
        "generate_image",
        "generate_video_from_image",
        "text_to_speech",
        "generate_bgm_music",
        "analyze_media",
        "hunyuan_generate_3d",
    }
)


def classify_tool_layer(name: str) -> ToolLayer:
    """Return the intended ownership/exposure layer for a tool name."""

    normalized = str(name or "").strip()
    if normalized in ENGINE_NATIVE_TOOLS:
        return "engine_native"
    if normalized in PUBLIC_TOOLS:
        return "public"
    if any(normalized.startswith(prefix) for prefix in RUNTIME_INTERNAL_PREFIXES):
        return "runtime_internal"
    return "unclassified"


__all__ = [
    "ENGINE_NATIVE_TOOLS",
    "PUBLIC_TOOLS",
    "RUNTIME_INTERNAL_PREFIXES",
    "ToolLayer",
    "classify_tool_layer",
]
