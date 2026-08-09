"""Filesystem ownership for scene-placement intermediate artifacts."""

from __future__ import annotations

import os
import re
from pathlib import Path

from runtime import project_context


_GENERATED_ROOT = Path("Resource") / "generated" / "scene_placement"


def _safe_name(value: str) -> str:
    name = str(value or "").strip().replace("\\", "_").replace("/", "_")
    name = re.sub(r'[:*?"<>|]+', "_", name)
    name = re.sub(r"\s+", " ", name).strip()
    return name or "scene"


def _generated_root() -> Path:
    return project_context.get_project_root().resolve() / _GENERATED_ROOT


def resolve_scene_placement_output_path(scene_path: str, scene_name: str) -> Path:
    """Return the canonical intermediate JSON path for a placement run.

    ``scene_path`` remains part of the tool contract for compatibility.  The
    placement JSON is workflow output, however, and must not be mistaken for
    the native ``Scene/*.scene`` document owned by the engine/editor.
    """
    del scene_path
    return Path(os.path.abspath(_generated_root() / f"{_safe_name(scene_name)}.json"))


def resolve_scene_placement_asset_root(asset_root: str) -> Path:
    """Resolve downloaded placement assets without depending on process cwd."""
    configured = Path(str(asset_root or "assets"))
    if configured.is_absolute():
        return Path(os.path.abspath(configured))
    return Path(os.path.abspath(_generated_root() / configured))


__all__ = [
    "resolve_scene_placement_asset_root",
    "resolve_scene_placement_output_path",
]
