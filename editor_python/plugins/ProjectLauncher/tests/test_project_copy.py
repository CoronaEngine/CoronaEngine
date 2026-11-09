import configparser
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from runtime import project_context
from config.project_state import CoronaSettings


class ProjectCopyTests(unittest.TestCase):


    def test_recent_projects_accepts_portable_scene_folder_without_project_ini(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            scene = root / "Portable"
            scene.mkdir()
            (scene / "scene.ini").write_text(
                "[format]\ntype = corona_scene_folder\nversion = 1\n"
                "[scene]\nname = Portable Name\n",
                encoding="utf-8",
            )
            config_path = root / "CoronaEditor.ini"
            config_path.write_text(
                "[General]\nlast_project =\n"
                "[History]\nrecent_projects = []\n",
                encoding="utf-8",
            )
            settings = CoronaSettings(str(config_path))

            settings.add_recent_project(str(scene))
            recent = settings.get_recent_projects()
            self.assertEqual(recent[0]["name"], "Portable Name")
            self.assertTrue(recent[0]["if_exists"])

    def test_launcher_history_does_not_hydrate_an_active_session(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            scene = root / "BrokenPortable"
            scene.mkdir()
            (scene / "scene.ini").write_text(
                "[format]\ntype = corona_scene_folder\nversion = 1\n"
                "[actors]\nBall.runtime.actor_version = 1\n"
                "Ball.runtime.actor_version = 1\n",
                encoding="utf-8",
            )
            config_path = root / "CoronaEditor.ini"
            config_path.write_text(
                f"[General]\nlast_project = {scene}\n"
                "[History]\nrecent_projects = []\n",
                encoding="utf-8",
            )
            settings = CoronaSettings(str(config_path))

            self.assertEqual(settings.config.get("General", "last_project"), str(scene))
            with patch("runtime.project_context._resolve_native_engine", return_value=None), \
                    patch.object(settings, "_project_config_path", side_effect=AssertionError("unexpected archive read")):
                self.assertEqual(project_context.get_active_project_path(), "")
                self.assertEqual(project_context.get_active_project_path(), "")

    def test_recent_projects_rejects_project_ini_only_directory(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            legacy = root / "Legacy"
            legacy.mkdir()
            (legacy / "project.ini").write_text(
                "[Project]\nname = Legacy\nentrance_scene = Scene/default.scene\n",
                encoding="utf-8",
            )
            config_path = root / "CoronaEditor.ini"
            config_path.write_text(
                f"[General]\nlast_project =\n[History]\nrecent_projects = {json.dumps([str(legacy)])}\n",
                encoding="utf-8",
            )
            settings = CoronaSettings(str(config_path))
            recent = settings.get_recent_projects()
            self.assertFalse(recent[0]["if_exists"])

    def test_portable_settings_save_does_not_modify_scene_ini_or_create_project_ini(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            scene = root / "Portable"
            scene.mkdir()
            scene_ini = scene / "scene.ini"
            scene_ini.write_text(
                "[format]\ntype = corona_scene_folder\nversion = 1\n"
                "[scene]\nname = Portable\n",
                encoding="utf-8",
            )
            config_path = root / "CoronaEditor.ini"
            config_path.write_text(
                "[General]\nlast_project =\n"
                "[History]\nrecent_projects = []\n",
                encoding="utf-8",
            )
            settings = CoronaSettings(str(config_path))
            original_scene = scene_ini.read_bytes()
            settings.add_recent_project(str(scene))
            settings.save()
            self.assertFalse((scene / "project.ini").exists())
            self.assertEqual(scene_ini.read_bytes(), original_scene)
            saved_editor = configparser.ConfigParser()
            saved_editor.read(config_path, encoding="utf-8")
            self.assertEqual(saved_editor.get("General", "last_project"), "")
            self.assertIn(str(scene), json.loads(saved_editor.get("History", "recent_projects")))


if __name__ == "__main__":
    unittest.main()
