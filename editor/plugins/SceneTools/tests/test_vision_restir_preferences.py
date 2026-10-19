import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from plugins.SceneTools.main import SceneTools
from runtime.archive.parser import parse_archive


class VisionRestirPreferencesTests(unittest.TestCase):
    def test_archive_retains_nested_restir_overrides_and_legacy_defaults(self):
        settings = {
            "max_depth": 8,
            "direct": {"open": False, "M_light": 12, "temporal": {"history_limit": 7}},
            "indirect": {"spatial": {"radius": 12.5}},
        }
        with tempfile.TemporaryDirectory() as temp_dir:
            scene = Path(temp_dir) / "scene.ini"
            scene.write_text(
                '[format]\ntype = corona_scene_folder\nversion = 1\n'
                '[scene]\nname = ReSTIR\n[camera]\ncount = 2\n'
                f'camera0.vision_restir_settings = {json.dumps(settings)}\n'
                'camera0.vision_pt_settings = {"max_depth":6}\n', encoding="utf-8")
            cameras = parse_archive(str(scene))["scene"]["cameras"]
            self.assertEqual(cameras[0].get("vision_restir_settings"), settings)
            self.assertEqual(cameras[1].get("vision_restir_settings"), {})
            self.assertEqual(cameras[0]["vision_pt_settings"], {"max_depth": 6})

    def test_facade_forwards_nested_settings_reset_and_requested_feedback(self):
        for settings in ({"direct": {"temporal": {"open": False}}}, {}):
            with self.subTest(settings=settings):
                accepted = {"status": "success", "pending": True, "settings": settings}
                with patch("api.editor_api._invoke_manifest_cpp_api", return_value=accepted) as invoke:
                    self.assertIs(SceneTools.set_restir_settings("scene.ini", "camera", settings), accepted)
                    self.assertIs(SceneTools.get_restir_settings("scene.ini", "camera"), accepted)
                self.assertEqual([call.args for call in invoke.call_args_list], [
                    ("scene_tools.set_restir_settings", ["scene.ini", "camera", settings]),
                    ("scene_tools.get_restir_settings", ["scene.ini", "camera"]),
                ])


if __name__ == "__main__":
    unittest.main()
