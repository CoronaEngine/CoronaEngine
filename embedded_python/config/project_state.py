"""Persistent editor project state.

This module owns the legacy ``CoronaEditor.ini`` state used by the embedded
Python host: active project hydration, project history, and compatibility
metadata. Native ``project.*`` and ``projectSettings.*`` handlers remain the
authoritative public APIs for the editor UI.
"""

import configparser
import datetime
import json
import logging
import os
import shutil
from pathlib import Path

# Keep the historical logger name so older host log filters remain effective.
logger = logging.getLogger("config.project_state")

version = "1.2.0"


class CoronaSettings:
    """Manage legacy ``CoronaEditor.ini`` project state for Python hosts."""

    def __init__(self, config_path=None):
        self.project_path = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

        if config_path is None:
            self.config_path = os.path.join(
                os.path.dirname(self.project_path), "CoronaEditor.ini"
            )
        else:
            self.config_path = config_path

        self.config = configparser.ConfigParser(strict=False)
        self._ensure_file_exists()

    @staticmethod
    def _project_config_path(project_path):
        scene_ini = os.path.join(project_path, "scene.ini")
        try:
            if os.path.isfile(scene_ini):
                config = configparser.ConfigParser()
                config.read(scene_ini, encoding="utf-8-sig")
                if (config.get("format", "type", fallback="") == "corona_scene_folder"
                        and config.get("format", "version", fallback="") == "1"):
                    return scene_ini
        except (OSError, UnicodeError, configparser.Error) as exc:
            logger.error("Unable to read scene.ini: %s", exc)
        return ""

    def _ensure_file_exists(self):
        if not os.path.exists(self.config_path):
            template_path = os.path.join(self.project_path, "CoronaEditor.ini")
            if os.path.exists(template_path):
                try:
                    shutil.copy2(template_path, self.config_path)
                    logger.info("Config initialized from template: %s", template_path)
                except Exception as exc:
                    logger.error("Failed to copy template config: %s", exc)
            self.load()
        else:
            self.load()

    def load(self):
        try:
            self.config.read(self.config_path, encoding="utf-8-sig")
        except Exception as exc:
            logger.error("Failed to load config: %s", exc)

    def save(self):
        try:
            with open(self.config_path, "w", encoding="utf-8") as handle:
                self.config.write(handle)
        except Exception as exc:
            logger.error("Failed to save config: %s", exc)

    def get_version(self) -> str:
        return self.config.get("General", "version", fallback="1.0.0")

    def set_version(self, version_value: str):
        self.config.set("General", "version", version_value)
        self.save()

    @staticmethod
    def _canonical_project_path(project_path: str) -> str:
        return str(Path(str(project_path)).expanduser().resolve())

    def get_recent_projects(self) -> list:
        raw = self.config.get("History", "recent_projects", fallback="[]")
        try:
            path_list = json.loads(raw)
        except Exception:
            return []

        canonical_paths = []
        seen_paths = set()
        for raw_path in path_list:
            if not isinstance(raw_path, str) or not raw_path.strip():
                continue
            canonical_path = self._canonical_project_path(raw_path)
            path_key = os.path.normcase(canonical_path)
            if path_key in seen_paths:
                continue
            seen_paths.add(path_key)
            canonical_paths.append(canonical_path)
        if canonical_paths != path_list:
            if not self.config.has_section("History"):
                self.config.add_section("History")
            self.config.set(
                "History",
                "recent_projects",
                json.dumps(canonical_paths, ensure_ascii=False),
            )
            self.save()

        refined_projects = []
        for raw_path in canonical_paths:
            ini_path = self._project_config_path(raw_path)
            project_name = os.path.basename(raw_path)
            if ini_path:
                try:
                    proj_cfg = configparser.ConfigParser()
                    proj_cfg.read(ini_path, encoding="utf-8")
                    section = "scene"
                    project_name = proj_cfg.get(section, "name", fallback=project_name)
                    last_edited = proj_cfg.get(section, "last_opened", fallback="")
                except Exception as exc:
                    logger.warning("Failed to read project info at %s: %s", ini_path, exc)
                    last_edited = ""
                if not last_edited:
                    try:
                        last_edited = datetime.datetime.fromtimestamp(
                            os.path.getmtime(ini_path)
                        ).strftime("%Y-%m-%d %H:%M:%S")
                    except Exception:
                        last_edited = "-"
                refined_projects.append(
                    {
                        "name": project_name,
                        "path": raw_path,
                        "if_exists": True,
                        "last_edited": last_edited,
                    }
                )
            else:
                refined_projects.append(
                    {
                        "name": project_name,
                        "path": raw_path,
                        "if_exists": False,
                        "last_edited": "-",
                    }
                )
        return refined_projects

    def add_recent_project(self, project_path: str):
        projects = json.loads(self.config.get("History", "recent_projects", fallback="[]"))
        project_path = self._canonical_project_path(project_path)
        project_key = os.path.normcase(project_path)
        normalized_projects = []
        seen_paths = {project_key}
        for existing in projects:
            if not isinstance(existing, str) or not existing.strip():
                continue
            canonical_path = self._canonical_project_path(existing)
            path_key = os.path.normcase(canonical_path)
            if path_key in seen_paths:
                continue
            seen_paths.add(path_key)
            normalized_projects.append(canonical_path)
        projects = [project_path, *normalized_projects][:10]
        if not self.config.has_section("History"):
            self.config.add_section("History")
        self.config.set(
            "History", "recent_projects", json.dumps(projects, ensure_ascii=False)
        )
        self.save()

    def get_default_path(self) -> str:
        return self.config.get("General", "default_path", fallback="")

    def set_default_path(self, path: str):
        self.config.set("General", "default_path", path)
        self.save()


settings_manager = CoronaSettings()
