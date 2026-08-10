import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]


class NativeFileDialogOwnershipTests(unittest.TestCase):
    def test_interactive_editor_dialog_routes_are_native(self):
        handlers = (ROOT / "src/systems/ui/cef/cef_editor_native_api_handlers.cpp").read_text(
            encoding="utf-8"
        )

        for route in (
            "import_resource_file",
            "select_model_file",
            "browse_scene_file",
            "select_screenshot_path",
        ):
            self.assertNotIn(f'{{"{route}", script_method}}', handlers)

        self.assertIn("show_native_file_dialog", handlers)
        self.assertIn("CLSID_FileOpenDialog", handlers)
        self.assertIn("CLSID_FileSaveDialog", handlers)
        self.assertIn("Corona::API::import_media", handlers)

    def test_python_runtime_contains_no_editor_file_dialog_implementation(self):
        production_files = (
            ROOT / "editor/plugins/MainView/main.py",
            ROOT / "editor/plugins/SceneTools/main.py",
            ROOT / "editor/plugins/ProjectLauncher/main.py",
        )
        combined = "\n".join(path.read_text(encoding="utf-8") for path in production_files)

        self.assertNotIn("FileHandler", combined)
        self.assertNotIn("tkinter", combined)
        self.assertFalse((ROOT / "editor/CoronaCore/utils/file_handler.py").is_file())


if __name__ == "__main__":
    unittest.main()
