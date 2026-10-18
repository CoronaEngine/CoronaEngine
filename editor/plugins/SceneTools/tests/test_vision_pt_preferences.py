import tempfile
import unittest
from pathlib import Path

from runtime.archive.parser import parse_archive


class VisionPtPreferencesTests(unittest.TestCase):
    def test_archive_retains_independent_pt_parameters_and_legacy_defaults(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            scene = Path(temp_dir) / "scene.ini"
            scene.write_text(
                '[format]\ntype = corona_scene_folder\nversion = 1\n'
                '[scene]\nname = PT\n[camera]\ncount = 2\n'
                'camera0.vision_pt_settings = {"max_depth":8,"min_depth":2,"rr_threshold":0.5}\n'
                'camera0.vision_reblur_settings = {"max_history":12}\n', encoding="utf-8")
            cameras = parse_archive(str(scene))["scene"]["cameras"]
            self.assertEqual(cameras[0].get("vision_pt_settings"),
                             {"max_depth": 8, "min_depth": 2, "rr_threshold": 0.5})
            self.assertEqual(cameras[1].get("vision_pt_settings"), {})
            self.assertEqual(cameras[0]["vision_reblur_settings"], {"max_history": 12})


if __name__ == "__main__":
    unittest.main()
