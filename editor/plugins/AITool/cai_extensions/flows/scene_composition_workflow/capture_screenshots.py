"""Legacy workflow wrapper for unified VLM scene capture."""
from __future__ import annotations

import logging
from pathlib import Path
from typing import Dict

from Quasar.ai_workflow.streaming import stream_output_node

from .formatters import NO_OUTPUT

logger = logging.getLogger(__name__)


def _capture_vlm_scene(output_dir: str):
    try:
        from ...agent.vlm_capture import capture_vlm_views
    except ImportError:
        from cai_extensions.agent.vlm_capture import capture_vlm_views
    return capture_vlm_views("", output_dir, scope="scene", timeout_sec=5.0)


@stream_output_node("integrated", NO_OUTPUT)
def capture_screenshots_node(state) -> Dict[str, object]:
    """Capture scene review screenshots via the unified 4-view base_color path."""
    intermediate = state.get("intermediate", {}) if isinstance(state, dict) else {}
    scene_json_path = intermediate.get("scene_json_path", "")
    imported_actors = intermediate.get("imported_actors", [])

    if not imported_actors:
        logger.info("capture_screenshots: no imported actors, skip unified VLM capture")
        return {}
    if not scene_json_path:
        logger.warning("capture_screenshots: scene_json_path is empty, skip unified VLM capture")
        return {}

    output_dir = str(Path(scene_json_path).parent / "review_screenshots")
    result = _capture_vlm_scene(output_dir)
    logger.info(
        "capture_screenshots: unified VLM capture status=%s count=%d output_mode=%s dir=%s",
        result.status,
        len(result.files),
        result.output_mode,
        result.output_dir,
    )
    if result.status != "success":
        return {}
    return {
        "intermediate": {
            "review_screenshot_dir": result.output_dir,
            "review_screenshot_count": len(result.files),
            "review_capture": result.to_dict(),
        },
    }
