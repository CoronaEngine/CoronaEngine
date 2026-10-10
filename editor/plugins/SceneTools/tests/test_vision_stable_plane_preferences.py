import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from plugins.SceneTools import vision_document, vision_import
from plugins.SceneTools.main import SceneTools
from runtime.archive.parser import parse_archive


class VisionStablePlanePreferencesTests(unittest.TestCase):
    def test_document_roundtrip_preserves_false_and_legacy_defaults(self):
        for value, expected in ((True, True), (False, False), ("true", True),
                                ("false", False), ("off", False), (0, False)):
            with self.subTest(value=value):
                document = {"render": {"integrator": {"type": "rt", "param": {
                    "direct": {"stable_planes": value}}}}}
                restored = vision_document.decode_vision_document(
                    vision_document.encode_vision_document(document))
                self.assertIs(vision_document.infer_vision_stable_planes(restored), expected)
                self.assertEqual(vision_document.infer_vision_render_mode(restored), "restir")
        for document in ({}, None, {"render": None}, {"render": {"integrator": {"param": []}}}):
            with self.subTest(document=document):
                self.assertIs(vision_document.infer_vision_stable_planes(document), True)

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

    def test_facade_forwards_explicit_false_and_pending_response(self):
        accepted = {"status": "success", "enabled": False, "pending": True}
        with patch("api.editor_api._invoke_manifest_cpp_api", return_value=accepted) as invoke:
            self.assertIs(SceneTools.set_vision_stable_planes("scene.ini", "camera", False), accepted)
            self.assertIs(SceneTools.get_vision_stable_planes("scene.ini", "camera"), accepted)
        self.assertEqual([call.args for call in invoke.call_args_list], [
            ("scene_tools.set_vision_stable_planes", ["scene.ini", "camera", False]),
            ("scene_tools.get_vision_stable_planes", ["scene.ini", "camera"]),
        ])

    def test_import_applies_and_saves_explicit_false(self):
        document = {"render": {"integrator": {"type": "rt", "param": {
            "direct": {"stable_planes": False}}}}, "output": {"denoise": True}}
        calls = []

        def invoke(wrapper, args):
            calls.append((wrapper, args))
            if wrapper == "scene_tools.is_vision_available":
                return {"available": True}
            if wrapper == "scene.get_snapshot":
                return {"scene": "scene.ini", "camera": {"name": "MainCamera"}, "actors": []}
            return {"status": "success", "pending": True}

        with tempfile.TemporaryDirectory() as temp_dir:
            source = Path(temp_dir) / "source.json"
            source.write_text(json.dumps(document), encoding="utf-8")
            with patch("api.editor_api._invoke_manifest_cpp_api", side_effect=invoke), patch.object(
                vision_import, "prepare_external_live_vision_scene", return_value=str(source)
            ):
                result = vision_import.import_vision_scene_into_current_scene("scene.ini", str(source))
        self.assertEqual(result["status"], "success", result)
        self.assertIn(("scene_tools.set_vision_stable_planes", ["scene.ini", "MainCamera", False]), calls)
        self.assertIn(("scene_tools.set_vision_render_mode", ["scene.ini", "MainCamera", "restir"]), calls)
        self.assertIn(("scene_tools.set_vision_denoise", ["scene.ini", "MainCamera", True]), calls)
        saved = next(args[1] for wrapper, args in calls if wrapper == "main.scene_save")
        restored = vision_document.decode_vision_document(saved["vision_document"]["data"])
        self.assertIs(restored["render"]["integrator"]["param"]["direct"]["stable_planes"], False)


if __name__ == "__main__":
    unittest.main()
