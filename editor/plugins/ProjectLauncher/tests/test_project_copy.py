import configparser
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from plugins.ProjectLauncher import main as project_launcher
from runtime import project_copy
from config.settings import CoronaSettings
from runtime import project_templates as project_utils


class ProjectCopyTests(unittest.TestCase):
    def test_project_template_is_owned_by_project_launcher_and_can_create_project(self):
        repo_root = Path(__file__).resolve().parents[4]
        template_root = repo_root / "editor" / "plugins" / "ProjectLauncher" / "templates"
        self.assertTrue((template_root / "project" / "project.ini").is_file())
        self.assertTrue((template_root / "scene" / "demo.scene").is_file())
        self.assertTrue((template_root / "actor" / "demo.actor").is_file())

        with tempfile.TemporaryDirectory() as temp_dir:
            target = Path(temp_dir) / "created"
            project_ini = project_utils.create_project_from_template(
                str(target), "Created Project", "3d"
            )

            self.assertEqual(Path(project_ini), target / "project.ini")
            self.assertTrue((target / "Scene" / "default.scene").is_file())

    def test_copy_existing_to_data_creates_new_runtime_copy(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            source_dir = temp_root / "source_save"
            source_scene_dir = source_dir / "Scene"
            source_scene_dir.mkdir(parents=True)
            (source_scene_dir / "default.scene").write_text("[base]\nname = default\n", encoding="utf-8")
            source_ini = source_dir / "project.ini"
            source_ini.write_text(
                "\n".join([
                    "[Project]",
                    "name = sample_save",
                    "mode = 3d",
                    "entrance_scene = Scene/default.scene",
                    "scenes = Scene/default.scene",
                    "active_scene = Scene/default.scene",
                    "",
                ]),
                encoding="utf-8",
            )

            original_core_path = project_copy.core_path
            project_copy.core_path = SimpleNamespace(repo_root=temp_root / "runtime")
            try:
                first = project_copy.ProjectCopy.copy_existing_to_data(str(source_ini))
                second = project_copy.ProjectCopy.copy_existing_to_data(str(source_ini))
            finally:
                project_copy.core_path = original_core_path

            first_path = Path(first["path"])
            second_path = Path(second["path"])

            self.assertEqual(first["name"], "sample_save")
            self.assertEqual(second["name"], "sample_save_1")
            self.assertTrue((first_path / "project.ini").is_file())
            self.assertTrue((first_path / "Scene" / "default.scene").is_file())
            self.assertTrue((second_path / "project.ini").is_file())
            self.assertEqual(first_path.parent, temp_root / "runtime" / "data")
            self.assertEqual(second_path.parent, temp_root / "runtime" / "data")

            source_cfg = configparser.ConfigParser()
            source_cfg.read(source_ini, encoding="utf-8")
            self.assertEqual(source_cfg.get("Project", "name"), "sample_save")

            copied_cfg = configparser.ConfigParser()
            copied_cfg.read(second_path / "project.ini", encoding="utf-8")
            self.assertEqual(copied_cfg.get("Project", "name"), "sample_save_1")

    def test_copy_existing_to_data_reuses_runtime_project(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            runtime_data = temp_root / "runtime" / "data"
            source_dir = runtime_data / "creative_world_5"
            source_scene_dir = source_dir / "Scene"
            source_scene_dir.mkdir(parents=True)
            (source_scene_dir / "default.scene").write_text(
                "[base]\nname = default\n", encoding="utf-8"
            )
            source_ini = source_dir / "project.ini"
            source_ini.write_text(
                "\n".join([
                    "[Project]",
                    "name = Coin Collector",
                    "mode = 3d",
                    "entrance_scene = Scene/default.scene",
                    "",
                ]),
                encoding="utf-8",
            )

            original_core_path = project_copy.core_path
            project_copy.core_path = SimpleNamespace(repo_root=temp_root / "runtime")
            try:
                result = project_copy.ProjectCopy.copy_existing_to_data(str(source_ini))
            finally:
                project_copy.core_path = original_core_path

            self.assertEqual(result["name"], "creative_world_5")
            self.assertEqual(Path(result["path"]), source_dir)
            self.assertEqual(
                sorted(path.name for path in runtime_data.iterdir()),
                ["creative_world_5"],
            )

    def test_project_launcher_python_does_not_import_project_copy_or_vision_import(self):
        source = Path(project_launcher.__file__).read_text(encoding="utf-8")
        self.assertNotIn("ProjectCopy", source)
        self.assertNotIn("vision_import", source)

    def test_settings_accepts_portable_scene_folder_without_project_ini(self):
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

            self.assertTrue(settings.set_active_project(str(scene)))
            self.assertEqual(settings.active_project_path, str(scene))
            self.assertEqual(settings.active_project_config.get("scene", "name"), "Portable Name")
            recent = settings.get_recent_projects()
            self.assertEqual(recent[0]["name"], "Portable Name")
            self.assertTrue(recent[0]["if_exists"])

    def test_failed_automatic_project_hydration_is_not_retried_on_every_access(self):
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

            with self.assertLogs("config.settings", level="ERROR") as captured:
                self.assertIsNone(settings.active_project_path)
                self.assertIsNone(settings.active_project_path)

            hydration_errors = [
                message for message in captured.output
                if "Failed to hydrate active project from last_project" in message
            ]
            self.assertEqual(len(hydration_errors), 1)

    def test_recent_projects_marks_project_ini_saves_as_legacy(self):
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
                f"[General]\nlast_project =\n[History]\nrecent_projects = {__import__('json').dumps([str(legacy)])}\n",
                encoding="utf-8",
            )
            settings = CoronaSettings(str(config_path))
            recent = settings.get_recent_projects()
            self.assertTrue(recent[0]["legacy"])

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
            self.assertTrue(settings.set_active_project(str(scene)))
            original_scene = scene_ini.read_bytes()
            self.assertTrue(settings.save_active_project_info())
            self.assertFalse((scene / "project.ini").exists())
            self.assertEqual(scene_ini.read_bytes(), original_scene)
            saved_editor = configparser.ConfigParser()
            saved_editor.read(config_path, encoding="utf-8")
            self.assertEqual(saved_editor.get("General", "last_project"), str(scene))


if __name__ == "__main__":
    unittest.main()
