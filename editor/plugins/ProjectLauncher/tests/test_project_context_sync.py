import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]


class NativeProjectContextSyncTests(unittest.TestCase):
    def test_open_project_enqueues_python_context_control_request(self):
        handlers = (ROOT / "src/systems/ui/cef/cef_editor_native_api_handlers.cpp").read_text(
            encoding="utf-8"
        )
        api = (ROOT / "src/systems/ui/editor_api/cef_editor_api.cpp").read_text(encoding="utf-8")
        python_api = (ROOT / "src/systems/script/python/python_api.cpp").read_text(encoding="utf-8")

        self.assertIn("enqueue_python_project_context_changed(state.project_path)", handlers)
        self.assertIn('request.function = "project_context_changed"', api)
        self.assertIn('request.function == "project_context_changed"', python_api)
        self.assertIn('nanobind::getattr(pEditor, "update_project_context")', python_api)


if __name__ == "__main__":
    unittest.main()
