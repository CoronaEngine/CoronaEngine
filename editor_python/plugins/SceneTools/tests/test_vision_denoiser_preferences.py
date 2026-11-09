import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from runtime.archive.parser import parse_archive


class VisionDenoiserPreferencesTests(unittest.TestCase):
    def test_archive_retains_camera_local_reblur_parameters(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            scene = Path(temp_dir) / "scene.ini"
            scene.write_text(
                '[format]\ntype = corona_scene_folder\nversion = 1\n'
                '[scene]\nname = Reblur\n[camera]\ncount = 2\n'
                'camera0.vision_reblur_settings = {"max_history":12,"reconstruction":2,"anti_firefly":false}\n',
                encoding="utf-8",
            )
            cameras = parse_archive(str(scene))["scene"]["cameras"]
            self.assertEqual(cameras[0].get("vision_reblur_settings"),
                             {"max_history": 12, "reconstruction": 2, "anti_firefly": False})
            self.assertEqual(cameras[1].get("vision_reblur_settings"), {})

    def test_archive_retains_independent_relax_parameters(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            scene = Path(temp_dir) / "scene.ini"
            scene.write_text(
                '[format]\ntype = corona_scene_folder\nversion = 1\n'
                '[scene]\nname = Relax\n[camera]\ncount = 2\n'
                'camera0.vision_denoiser = ReLAX\n'
                'camera0.vision_relax_settings = {"diffuse_history":12,"atrous_iterations":3,"anti_firefly":true}\n'
                'camera0.vision_reblur_settings = {"max_history":20}\n', encoding="utf-8")
            cameras = parse_archive(str(scene))["scene"]["cameras"]
            self.assertEqual(cameras[0]["vision_denoiser"], "relax")
            self.assertEqual(cameras[0].get("vision_relax_settings"),
                             {"diffuse_history":12,"atrous_iterations":3,"anti_firefly":True})
            self.assertEqual(cameras[0]["vision_reblur_settings"], {"max_history":20})
            self.assertEqual(cameras[1].get("vision_relax_settings"), {})

    def test_archive_preserves_camera_types_and_defaults_to_reblur(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            scene = Path(temp_dir) / "scene.ini"
            scene.write_text(
                "[format]\ntype = corona_scene_folder\nversion = 1\n"
                "[scene]\nname = Denoisers\n[camera]\ncount = 3\n"
                "camera0.vision_denoiser = ReBLUR\ncamera0.vision_denoise = true\n"
                "camera1.vision_denoiser = svgf\ncamera1.vision_denoise = false\n",
                encoding="utf-8",
            )
            cameras = parse_archive(str(scene))["scene"]["cameras"]
            self.assertEqual([camera["vision_denoiser"] for camera in cameras], ["reblur", "svgf", "reblur"])
            self.assertIs(cameras[0]["vision_denoise"], True)
            self.assertIs(cameras[1]["vision_denoise"], False)

if __name__ == "__main__":
    unittest.main()
