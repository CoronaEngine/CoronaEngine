"""Legacy project copy/open facade for older embedded hosts.

The active project lifecycle is native ``project.*``.  This module remains
only for older hosts that still import the historical ``ProjectCopy`` API.
"""

import configparser
import logging
import os
import shutil

from config.paths_config import get_default_paths, get_legacy_project_data_dir
from config.project_state import settings_manager
from runtime.project_templates import (
    create_project_from_template,
    normalize_project_runtime_paths,
    update_project_config,
)

logger = logging.getLogger(__name__)
core_path = get_default_paths()


def _safe_project_dir_name(name, fallback):
    raw_name = (name or fallback or "project").strip()
    safe_name = "".join("_" if c in '<>:"/\\|?*' else c for c in raw_name)
    safe_name = safe_name.strip(" .")
    return safe_name or "project"


def _is_path_within(path, root):
    try:
        normalized_path = os.path.normcase(os.path.abspath(path))
        normalized_root = os.path.normcase(os.path.abspath(root))
        return os.path.commonpath([normalized_path, normalized_root]) == normalized_root
    except ValueError:
        return False


def _ensure_vision_camera_defaults(scene_config):
    if "camera" not in scene_config:
        scene_config["camera"] = {}
    scene_config["camera"].setdefault("count", "1")
    scene_config["camera"].setdefault("active_id", "")
    scene_config["camera"]["camera0.render_backend"] = "vision"
    scene_config["camera"].setdefault("camera0.vision_render_mode", "path_tracing")
    scene_config["camera"].setdefault("camera0.output_mode", "final_color")


def _ensure_native_vision_metadata(project_path):
    project_ini = os.path.join(project_path, "project.ini")
    project_config = configparser.ConfigParser()
    project_config.read(project_ini, encoding="utf-8")
    entrance = project_config.get("Project", "entrance_scene", fallback="").strip()
    if not entrance:
        return

    scene_file = os.path.join(project_path, *entrance.replace("\\", "/").split("/"))
    if not os.path.isfile(scene_file):
        return

    scene_config = configparser.ConfigParser()
    scene_config.read(scene_file, encoding="utf-8")
    has_vision = "vision" in scene_config
    has_document = "vision_document" in scene_config
    if not has_vision and not has_document:
        return

    _ensure_vision_camera_defaults(scene_config)
    for section in (
        "vision",
        "vision_document",
        "vision_bindings",
        "vision_unsupported_shapes",
    ):
        if scene_config.has_section(section):
            scene_config.remove_section(section)
    with open(scene_file, "w", encoding="utf-8") as handle:
        scene_config.write(handle)


class ProjectCopy:
    @staticmethod
    def create_from_template(target_path, project_name, mode):
        """Create a project from the historical launcher template."""
        try:
            project_ini = create_project_from_template(target_path, project_name, mode)
            normalize_project_runtime_paths(os.path.dirname(project_ini))
            settings_manager.add_recent_project(os.path.dirname(project_ini))
            return project_ini
        except Exception:
            logger.exception("ProjectCopy create_from_template failed")
            raise

    @staticmethod
    def copy_existing_to_data(project_ini_path, data_root=None):
        """Copy an existing legacy project into the runtime data directory."""
        source_ini = os.path.abspath(project_ini_path)
        if not os.path.isfile(source_ini):
            raise FileNotFoundError(f"project.ini not found: {source_ini}")

        source_dir = os.path.dirname(source_ini)
        project_name = os.path.basename(source_dir)
        config = configparser.ConfigParser()
        config.read(source_ini, encoding="utf-8")
        if "Project" in config:
            project_name = config["Project"].get("name", project_name)

        data_dir = (
            os.path.abspath(os.fspath(data_root))
            if data_root is not None
            else str(get_legacy_project_data_dir(core_path.repo_root))
        )
        os.makedirs(data_dir, exist_ok=True)
        if _is_path_within(source_dir, data_dir):
            normalize_project_runtime_paths(source_dir)
            return {"name": os.path.basename(source_dir), "path": source_dir}

        base_name = _safe_project_dir_name(project_name, os.path.basename(source_dir))
        target_path = os.path.join(data_dir, base_name)
        counter = 1
        while os.path.exists(target_path):
            target_path = os.path.join(data_dir, f"{base_name}_{counter}")
            counter += 1

        shutil.copytree(source_dir, target_path)
        normalize_project_runtime_paths(target_path)
        target_ini = os.path.join(target_path, "project.ini")
        target_config = configparser.ConfigParser()
        target_config.read(target_ini, encoding="utf-8")
        if "Project" not in target_config:
            target_config["Project"] = {}
        final_name = os.path.basename(target_path)
        target_config["Project"]["name"] = final_name
        with open(target_ini, "w", encoding="utf-8") as handle:
            target_config.write(handle)

        logger.info("Copied existing project save: %s -> %s", source_dir, target_path)
        return {"name": final_name, "path": target_path}

    @staticmethod
    def open_and_update(project_path):
        """Update legacy project metadata for an older host opening it."""
        if not os.path.exists(project_path):
            return False

        try:
            normalize_project_runtime_paths(project_path)
            _ensure_native_vision_metadata(project_path)
            project_ini = os.path.join(project_path, "project.ini")
            update_project_config(project_ini, update_only_time=True)
            settings_manager.set_active_project(project_path)
            return True
        except Exception:
            logger.exception("ProjectCopy open_and_update failed")
            return False
