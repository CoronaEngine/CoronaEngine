import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from runtime.archive.parser import parse_archive


class VisionStablePlanePreferencesTests(unittest.TestCase):

    def test_archive_preserves_camera_preference_and_defaults_to_enabled(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            scene = Path(temp_dir) / "scene.ini"
            for value, expected in ((None, True), ("true", True), ("false", False), ("off", False)):
                with self.subTest(value=value):
                    content = (
                        "[format]\ntype = corona_scene_folder\nversion = 1\n"
                        "[scene]\nname = Stable planes\n[camera]\ncount = 2\n"
                        "camera0.vision_render_mode = restir\n"
                        "camera0.vision_accumulation = true\ncamera0.vision_denoise = false\n"
                    )
                    if value is not None:
                        content += f"camera0.vision_stable_planes = {value}\n"
                    scene.write_text(content, encoding="utf-8")
                    cameras = parse_archive(str(scene))["scene"]["cameras"]
                    self.assertIs(cameras[0]["vision_stable_planes"], expected)
                    self.assertIs(cameras[1]["vision_stable_planes"], True)
                    self.assertIs(cameras[0]["vision_accumulation"], True)
                    self.assertIs(cameras[0]["vision_denoise"], False)

if __name__ == "__main__":
    unittest.main()
