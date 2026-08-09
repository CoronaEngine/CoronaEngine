import unittest
from pathlib import Path


class LegacyFrontendEntrypointTests(unittest.TestCase):
    def setUp(self):
        self.frontend_root = Path(__file__).resolve().parents[2]

    def test_legacy_cef_calls_are_isolated_to_compatibility_entrypoint(self):
        source_root = self.frontend_root / "src"
        legacy_calls = (
            "camera_lock_set",
            "object_key_down",
            "object_key_up",
        )
        source_files = [
            path
            for path in list(source_root.rglob("*.js")) + list(source_root.rglob("*.vue"))
            if "compat" not in path.parts
        ]
        for path in source_files:
            source = path.read_text(encoding="utf-8")
            for call in legacy_calls:
                self.assertNotIn(call, source, f"legacy call leaked into {path}")

    def test_legacy_entrypoint_documents_its_migration_boundary(self):
        source = (self.frontend_root / "index.html").read_text(encoding="utf-8")
        panel_source = (
            self.frontend_root / "src" / "compat" / "legacyCameraLockPanel.js"
        ).read_text(encoding="utf-8")
        panel_style = self.frontend_root / "src" / "compat" / "legacyCameraLockPanel.css"
        adapter_path = self.frontend_root / "src" / "compat" / "legacyEditorAdapter.js"
        compatibility_wrapper = (
            self.frontend_root / "src" / "utils" / "legacyEditorAdapter.js"
        )
        adapter_source = adapter_path.read_text(encoding="utf-8")
        wrapper_source = compatibility_wrapper.read_text(encoding="utf-8")
        self.assertIn("Legacy host compatibility panel", source)
        self.assertIn("editorApi.sceneTools.setActorCameraLock", source)
        self.assertIn("camera_lock_set", panel_source)
        self.assertIn("object_key_down", panel_source)
        self.assertIn("object_key_up", panel_source)
        self.assertIn("./src/compat/legacyCameraLockPanel.js", source)
        self.assertTrue(panel_style.is_file())
        self.assertIn("__cam_toggle_dot", panel_style.read_text(encoding="utf-8"))
        self.assertIn("legacyCameraLockPanel.css", source)
        self.assertNotIn("#__cam_toggle_dot", source)
        self.assertNotIn("camera_lock_set", source)
        self.assertNotIn("object_key_down", source)
        self.assertNotIn("object_key_up", source)
        self.assertIn("__coronaLegacyEditorAdapter", panel_source)
        self.assertNotIn("window.cefQuery", source)
        self.assertIn("window.cefQuery", adapter_source)
        self.assertIn("installLegacyEditorAdapter", adapter_source)
        self.assertIn("from './compat/legacyEditorAdapter.js'", (
            self.frontend_root / "src" / "main.js"
        ).read_text(encoding="utf-8"))
        self.assertIn("../compat/legacyEditorAdapter.js", wrapper_source)

    def test_obsolete_camera_follow_panel_is_not_kept_as_a_second_ui_owner(self):
        obsolete_panel = (
            self.frontend_root / "src" / "components" / "panels" / "CameraFollowPanel.vue"
        )
        object_panel = (
            self.frontend_root / "src" / "views" / "sidebar" / "Object.vue"
        )

        self.assertFalse(obsolete_panel.exists())
        self.assertIn("editorApi.sceneTools.setActorCameraLock", object_panel.read_text(encoding="utf-8"))

    def test_unreferenced_rich_text_component_is_not_kept_in_ui_directory(self):
        obsolete_component = (
            self.frontend_root / "src" / "components" / "ui" / "RichTextPart.vue"
        )
        self.assertFalse(obsolete_component.exists())

    def test_orphaned_frontend_utilities_are_not_kept(self):
        utils_root = self.frontend_root / "src" / "utils"
        for name in (
            "editorInputFocusGate.js",
            "richTextMarkdown.js",
            "viewportUiWarp.js",
        ):
            self.assertFalse((utils_root / name).exists(), name)

    def test_orphaned_ai_hint_services_are_not_kept(self):
        services_root = self.frontend_root / "src" / "services"
        for name in ("aiHintGenerator.js", "mouseTracker.js"):
            self.assertFalse((services_root / name).exists(), name)


if __name__ == "__main__":
    unittest.main()
