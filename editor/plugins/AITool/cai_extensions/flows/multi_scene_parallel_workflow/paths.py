"""Filesystem ownership for multi-scene workflow artifacts."""

from __future__ import annotations

import os
from pathlib import Path

from runtime import project_context


def resolve_multi_scene_output_dir(
    parent_output: str = "", session_id: str = "default", scene_name: str = ""
) -> Path:
    """Return the output directory for one generated sub-scene."""
    safe_session_id = str(session_id or "default")
    safe_scene_name = str(scene_name or "").strip()
    if parent_output:
        output = Path(parent_output) / safe_scene_name
    else:
        output = (
            project_context.get_project_root()
            / "Resource"
            / "generated"
            / "multi_scene"
            / safe_session_id
            / safe_scene_name
        )
    return Path(os.path.abspath(output))
