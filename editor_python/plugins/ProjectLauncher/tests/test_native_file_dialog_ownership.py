import unittest
from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[3]
ENGINE_ROOT = EDITOR_ROOT.parent


class NativeFileDialogOwnershipTests(unittest.TestCase):
    def test_interactive_editor_dialog_routes_are_native(self):
        handlers = (ENGINE_ROOT / "src/systems/ui/cef/cef_editor_native_api_handlers.cpp").read_text(
            encoding="utf-8"
        )

        for route in (
            "import_resource_file",
            "select_model_file",
            "select_screenshot_path",
        ):
            self.assertNotIn(f'{{"{route}", script_method}}', handlers)

        self.assertIn("show_native_file_dialog", handlers)
        self.assertIn("CLSID_FileOpenDialog", handlers)
        self.assertIn("CLSID_FileSaveDialog", handlers)
        self.assertIn("Corona::API::import_media", handlers)

    def test_python_runtime_contains_no_editor_file_dialog_implementation(self):
        combined = "\n".join(p.read_text(encoding="utf-8") for p in (EDITOR_ROOT / "plugins").rglob("*.py") if "tests" not in p.parts)
        self.assertNotIn("FileHandler", combined)
        self.assertNotIn("tkinter", combined)
        self.assertFalse((EDITOR_ROOT / "CoronaCore/utils/file_handler.py").is_file())


if __name__ == "__main__":
    unittest.main()
