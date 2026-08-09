import os
from pathlib import Path
import tempfile
import sys
import time
import unittest

EDITOR_ROOT = Path(__file__).resolve().parents[2]
if str(EDITOR_ROOT) not in sys.path:
    sys.path.insert(0, str(EDITOR_ROOT))

from script_runtime.engine.scripts_manager import ScriptsManager


class _Scene:
    script_path = ""
    route = ""
    name = "reload-scene"
    _actors = []


def _project_script(marker: int) -> str:
    return f"""
from script_runtime.engine.entities.project_script import ProjectScript

class ReloadProjectScript(ProjectScript):
    marker = {marker}

    def initialize(self):
        self.initialized_marker = self.marker
"""


class ScriptsManagerReloadTests(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.path = Path(self.tempdir.name) / "project_script.py"
        self.scene = _Scene()

    def tearDown(self):
        self.tempdir.cleanup()

    def _write(self, text: str):
        self.path.write_text(text, encoding="utf-8")
        next_time = time.time() + 2.0
        os.utime(self.path, (next_time, next_time))

    def test_successful_reload_atomically_replaces_project_instance(self):
        self._write(_project_script(1))
        manager = ScriptsManager()
        self.assertTrue(manager.initialize_project(str(self.path), self.scene))
        original = manager.project_script
        original_module = original._corona_script_module_name

        self._write(_project_script(2))
        self.assertTrue(manager.reload_changed_scripts())

        self.assertIsNot(manager.project_script, original)
        self.assertEqual(manager.project_script.initialized_marker, 2)
        self.assertFalse(original.is_initialized)
        self.assertNotIn(original_module, sys.modules)
        manager.shutdown()

    def test_failed_reload_keeps_previous_instance(self):
        self._write(_project_script(1))
        manager = ScriptsManager()
        self.assertTrue(manager.initialize_project(str(self.path), self.scene))
        original = manager.project_script

        self._write("this is not valid Python !!!")
        self.assertFalse(manager.reload_changed_scripts())

        self.assertIs(manager.project_script, original)
        self.assertTrue(original.is_initialized)
        manager.shutdown()

    def test_shutdown_wins_over_pending_reload(self):
        self._write(_project_script(1))
        manager = ScriptsManager()
        self.assertTrue(manager.initialize_project(str(self.path), self.scene))
        manager.shutdown()
        self._write(_project_script(2))

        self.assertFalse(manager.reload_changed_scripts())
        self.assertEqual(manager.state, "stopped")

    def test_cpp_runtime_does_not_reload_editor_main_module(self):
        repo_root = Path(__file__).resolve().parents[3]
        source = (
            repo_root / "src" / "systems" / "script" / "python" / "python_api.cpp"
        ).read_text(encoding="utf-8")
        run_start = source.index("void PythonAPI::runPythonScript()")
        run_end = source.index("void PythonAPI::process_runtime_requests()", run_start)
        self.assertNotIn("performHotReload", source[run_start:run_end])
        self.assertNotIn("reload(main)", source)


if __name__ == "__main__":
    unittest.main()
