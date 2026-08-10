"""Project and asset template operations owned by the runtime.

The template files themselves live under ``plugins/ProjectLauncher/templates``;
this module owns the filesystem copy and initialization rules.  Native
``project.*`` remains the public lifecycle contract.
"""

import configparser
import datetime
import logging
import os
import shutil
from pathlib import Path

from config.project_state import version

logger = logging.getLogger(__name__)


def _project_launcher_template_root() -> Path:
    """Return the canonical template owner without coupling to this module's path."""
    editor_root = Path(__file__).resolve().parents[1]
    return editor_root / "plugins" / "ProjectLauncher" / "templates"


def create_project_from_template(target_path, project_name, mode):
    """Copy and initialize a project from the ProjectLauncher template."""
    try:
        template_src = _project_launcher_template_root() / "project"
        if not template_src.exists():
            raise Exception(f"模板目录未找到: {template_src}")
        if os.path.exists(target_path):
            raise Exception("目标文件夹已存在，请更换名称或路径")

        shutil.copytree(template_src, target_path)
        project_ini = os.path.join(target_path, "project.ini")
        normalize_project_runtime_paths(target_path)
        update_project_config(project_ini, project_name, mode, False)
        return project_ini
    except Exception as exc:
        logger.error("Project template error: %s", exc)
        raise


def normalize_project_runtime_paths(project_path: str) -> None:
    """Migrate demo-template paths to ASCII filenames for native loading."""
    scene_dir = os.path.join(project_path, "Scene")
    old_scene = os.path.join(scene_dir, "场景1.scene")
    new_scene = os.path.join(scene_dir, "default.scene")
    if os.path.exists(old_scene) and not os.path.exists(new_scene):
        os.replace(old_scene, new_scene)

    project_ini = os.path.join(project_path, "project.ini")
    if os.path.exists(project_ini):
        config = configparser.ConfigParser()
        config.read(project_ini, encoding="utf-8")
        if "Project" in config:
            for key in ("entrance_scene", "active_scene"):
                if config["Project"].get(key) == "Scene/场景1.scene":
                    config["Project"][key] = "Scene/default.scene"
            if config["Project"].get("scenes", ""):
                scenes = [
                    "Scene/default.scene"
                    if item.strip() == "Scene/场景1.scene"
                    else item.strip()
                    for item in config["Project"]["scenes"].split(",")
                    if item.strip()
                ]
                config["Project"]["scenes"] = ",".join(scenes)
            with open(project_ini, "w", encoding="utf-8") as handle:
                config.write(handle)


def update_project_config(ini_path, name=None, mode="3d", update_only_time=False):
    """Update project metadata while preserving the historical INI schema."""
    config = configparser.ConfigParser()
    if os.path.exists(ini_path):
        config.read(ini_path, encoding="utf-8")
    if "Project" not in config:
        config["Project"] = {}

    now_str = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    if not update_only_time:
        config["Project"]["name"] = name
        config["Project"]["mode"] = mode
        config["Project"]["create_time"] = now_str
        config["Project"]["core_version"] = version
    config["Project"]["last_opened"] = now_str
    with open(ini_path, "w", encoding="utf-8") as handle:
        config.write(handle)


def create_scene_from_template(target_path, scene_name):
    """Copy and initialize a scene template, adding a suffix on collision."""
    try:
        if not scene_name.endswith(".scene"):
            scene_name += ".scene"
        template_file = _project_launcher_template_root() / "scene" / "demo.scene"
        if not template_file.exists():
            raise FileNotFoundError(f"模板文件未找到: {template_file}")

        os.makedirs(target_path, exist_ok=True)
        base_name = os.path.splitext(scene_name)[0]
        target_file = os.path.join(target_path, f"{base_name}.scene")
        counter = 1
        while os.path.exists(target_file):
            target_file = os.path.join(target_path, f"{base_name}_{counter}.scene")
            counter += 1

        shutil.copy2(template_file, target_file)
        logger.info("场景文件创建成功: %s", target_file)
        update_config_name(target_file, base_name)
        return os.path.basename(target_file)
    except Exception:
        logger.exception("Scene template creation failed")
        raise


def create_actor_from_template(target_path, scene_name):
    """Copy and initialize an actor template, adding a suffix on collision."""
    try:
        if not scene_name.endswith(".actor"):
            scene_name += ".actor"
        template_file = _project_launcher_template_root() / "actor" / "demo.actor"
        if not template_file.exists():
            raise FileNotFoundError(f"模板文件未找到: {template_file}")

        os.makedirs(target_path, exist_ok=True)
        base_name = os.path.splitext(scene_name)[0]
        target_file = os.path.join(target_path, f"{base_name}.actor")
        counter = 1
        while os.path.exists(target_file):
            target_file = os.path.join(target_path, f"{base_name}_{counter}.actor")
            counter += 1

        shutil.copy2(template_file, target_file)
        logger.info("单位文件创建成功: %s", target_file)
        update_config_name(target_file, base_name)
        return os.path.basename(target_file)
    except Exception:
        logger.exception("Actor template creation failed")
        raise


def update_config_name(target_file, file_name):
    config = configparser.ConfigParser()
    config.read(target_file, encoding="utf-8")
    config["base"]["name"] = file_name
    with open(target_file, "w", encoding="utf-8") as handle:
        config.write(handle)
