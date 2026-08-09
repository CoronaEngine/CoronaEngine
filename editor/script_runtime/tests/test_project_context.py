import tempfile
import unittest
from pathlib import Path
from unittest import mock

from script_runtime.blockly import main as blockly_main


class BlocklyProjectContextTests(unittest.TestCase):
    def test_request_path_recovers_missing_python_project_context(self):
        with tempfile.TemporaryDirectory() as directory:
            project_path = Path(directory).resolve()
            (project_path / "project.ini").write_text("[Project]\nname = test\n", encoding="utf-8")
            settings = mock.Mock()
            settings.active_project_path = None
            settings.config.get.return_value = ""

            def activate(path):
                settings.active_project_path = path
                return True

            settings.set_active_project.side_effect = activate
            with mock.patch.object(blockly_main, "settings_manager", settings):
                resolved, error = blockly_main.ScratchTool._request_project_context({
                    "project_path": str(project_path),
                })

            self.assertIsNone(error)
            self.assertEqual(resolved, project_path)
            settings.set_active_project.assert_called_once_with(str(project_path))

    def test_stale_request_does_not_replace_active_project(self):
        with tempfile.TemporaryDirectory() as active_directory, tempfile.TemporaryDirectory() as stale_directory:
            active_path = Path(active_directory).resolve()
            stale_path = Path(stale_directory).resolve()
            (active_path / "project.ini").write_text("[Project]\nname = active\n", encoding="utf-8")
            (stale_path / "project.ini").write_text("[Project]\nname = stale\n", encoding="utf-8")
            settings = mock.Mock()
            settings.active_project_path = str(active_path)
            settings.config.get.return_value = ""

            with mock.patch.object(blockly_main, "settings_manager", settings):
                resolved, error = blockly_main.ScratchTool._request_project_context({
                    "project_path": str(stale_path),
                })

            self.assertEqual(resolved, active_path)
            self.assertEqual(error["code"], "PROJECT_CONTEXT_CHANGED")
            self.assertEqual(error["project_path"], str(active_path))
            settings.set_active_project.assert_not_called()

    def test_missing_context_returns_structured_error(self):
        settings = mock.Mock()
        settings.active_project_path = None
        settings.config.get.return_value = ""
        with mock.patch.object(blockly_main, "settings_manager", settings), \
             mock.patch.object(blockly_main, "get_active_project_path", return_value=""):
            resolved, error = blockly_main.ScratchTool._request_project_context({})

        self.assertIsNone(resolved)
        self.assertEqual(error["code"], "NO_ACTIVE_PROJECT")


if __name__ == "__main__":
    unittest.main()
