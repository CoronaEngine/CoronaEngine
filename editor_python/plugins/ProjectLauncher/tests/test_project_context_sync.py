import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]


class NativeProjectContextSyncTests(unittest.TestCase):
    def test_project_switch_does_not_enqueue_scene_context(self):
        handlers = (ROOT / "src/systems/ui/cef/cef_editor_native_api_handlers.cpp").read_text(encoding="utf-8")
        api = (ROOT / "src/systems/ui/editor_api/cef_editor_api.cpp").read_text(encoding="utf-8")
        self.assertNotIn("enqueue_python_project_context_changed", handlers + api)
        self.assertIn('{"read_active_scene",', handlers)


if __name__ == "__main__":
    unittest.main()
