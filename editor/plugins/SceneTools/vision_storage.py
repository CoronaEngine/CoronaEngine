"""Vision derived-scene paths and atomic document storage."""

import json
import logging
import os
import re
import uuid

from runtime import project_context


logger = logging.getLogger(__name__)


def _safe_filename_stem(value: str) -> str:
    stem = re.sub(r"[^0-9A-Za-z_.-]+", "_", value.strip())
    return stem.strip("._") or "vision_shape"


def derived_vision_scene_path(source_path: str, scene) -> str:
    source_dir = os.path.dirname(os.path.abspath(source_path))
    source_stem, source_ext = os.path.splitext(os.path.basename(source_path))
    scene_stem = _safe_filename_stem(getattr(scene, "name", "") or "scene")
    key = f"{os.path.abspath(source_path)}|{getattr(scene, 'route', '')}"
    suffix = uuid.uuid5(uuid.NAMESPACE_URL, key).hex[:12]
    return os.path.join(
        source_dir,
        f"{source_stem}.corona_{scene_stem}_{suffix}{source_ext or '.json'}",
    )


def runtime_vision_scene_path(scene, active_project_path: str = "") -> str:
    route = getattr(scene, "route", "") or ""
    project_path = active_project_path or ""
    if os.path.isabs(route):
        project_path = project_path or os.path.dirname(os.path.abspath(route))
    base_dir = os.path.abspath(project_path or str(project_context.get_project_root()))
    scene_stem = _safe_filename_stem(getattr(scene, "name", "") or "scene")
    key = f"{os.path.abspath(route) if route else scene_stem}|embedded_vision"
    suffix = uuid.uuid5(uuid.NAMESPACE_URL, key).hex[:12]
    return os.path.join(base_dir, ".corona", "vision_live", f"{scene_stem}_{suffix}.json")


def atomic_write_json(path: str, document: dict) -> None:
    directory = os.path.dirname(os.path.abspath(path))
    os.makedirs(directory, exist_ok=True)
    temp_path = os.path.join(
        directory, f".{os.path.basename(path)}.{uuid.uuid4().hex}.tmp"
    )
    try:
        with open(temp_path, "w", encoding="utf-8") as file:
            json.dump(document, file, ensure_ascii=False, indent=2)
            file.write("\n")
            file.flush()
            os.fsync(file.fileno())
        os.replace(temp_path, path)
    except Exception:
        try:
            if os.path.exists(temp_path):
                os.remove(temp_path)
        except OSError:
            logger.warning("Failed to remove temporary Vision scene file: %s", temp_path)
        raise
