import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from plugins.SceneTools.main import SceneTools
from runtime.archive.parser import parse_archive


class VisionDenoiserPreferencesTests(unittest.TestCase):
    def test_archive_preserves_camera_types_and_inherits_legacy_scene(self):
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
            self.assertEqual([camera["vision_denoiser"] for camera in cameras], ["reblur", "svgf", "scene"])
            self.assertIs(cameras[0]["vision_denoise"], True)
            self.assertIs(cameras[1]["vision_denoise"], False)

    def test_facade_forwards_each_type_and_pending_response(self):
        for denoiser in ("scene", "svgf", "reblur"):
            with self.subTest(denoiser=denoiser):
                accepted = {"status": "success", "type": denoiser, "pending": True}
                with patch("api.editor_api._invoke_manifest_cpp_api", return_value=accepted) as invoke:
                    self.assertIs(SceneTools.set_vision_denoiser("scene.ini", "camera", denoiser), accepted)
                invoke.assert_called_once_with("scene_tools.set_vision_denoiser", ["scene.ini", "camera", denoiser])


if __name__ == "__main__":
    unittest.main()
