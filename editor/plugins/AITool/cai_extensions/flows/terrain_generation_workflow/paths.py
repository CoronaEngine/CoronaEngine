"""Filesystem ownership for terrain-generation artifacts."""

from __future__ import annotations

import os
from pathlib import Path

from runtime import project_context


def resolve_terrain_output_dir(
    output_base: str = "", session_id: str = "default", scene_name: str = ""
) -> Path:
    """Return the terrain artifact directory for a workflow run.

    A caller-provided base remains authoritative. Otherwise artifacts belong to
    the active project rather than the editor source tree or process cwd.
    """
    safe_session_id = str(session_id or "default")
    safe_scene_name = str(scene_name or "").strip()
    if output_base:
        output = Path(output_base) / safe_scene_name / "terrain" if safe_scene_name else Path(output_base)
    else:
        output = (
            project_context.get_project_root()
            / "Resource"
            / "generated"
            / "terrain"
            / safe_session_id
        )
        if safe_scene_name:
            output /= safe_scene_name
            output /= "terrain"
    return Path(os.path.abspath(output))
