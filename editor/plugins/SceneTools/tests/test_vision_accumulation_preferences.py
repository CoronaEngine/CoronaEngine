import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from plugins.SceneTools import vision_document, vision_import
from plugins.SceneTools.main import SceneTools


class VisionAccumulationPreferencesTests(unittest.TestCase):
    def test_document_preferences_roundtrip_independently_for_pt_and_restir(self):
        infer = getattr(vision_document, "infer_vision_accumulation", None)
        self.assertTrue(callable(infer))
        for integrator, expected_mode in (("pt", "path_tracing"), ("rt", "restir")):
            for value, enabled in ((True, True), (False, False), ("true", True),
                                   ("false", False), ("on", True), ("0", False)):
                for denoise in (True, False):
                    with self.subTest(integrator=integrator, accumulation=value, denoise=denoise):
                        document = {
                            "render": {"integrator": {"type": integrator}},
                            "pipeline": {"param": {"frame_buffer": {
                                "type": "normal", "param": {"accumulation": value}}}},
                            "output": {"denoise": denoise},
                        }
                        restored = vision_document.decode_vision_document(
                            vision_document.encode_vision_document(document))
                        self.assertIs(infer(restored), enabled)
                        self.assertIs(vision_document.infer_vision_denoise(restored), denoise)
                        self.assertEqual(vision_document.infer_vision_render_mode(restored), expected_mode)

    def test_document_missing_or_malformed_accumulation_defaults_to_false(self):
        infer = getattr(vision_document, "infer_vision_accumulation", None)
        self.assertTrue(callable(infer))
        for document in ({}, None, {"pipeline": None}, {"pipeline": {"param": []}},
                         {"pipeline": {"param": {"frame_buffer": {"param": None}}}}):
            with self.subTest(document=document):
                self.assertIs(infer(document), False)

    def test_plugin_accumulation_routes_explicit_false_and_pending_result(self):
        setter = getattr(SceneTools, "set_vision_accumulation", None)
        self.assertTrue(callable(setter))
        accepted = {"status": "success", "enabled": False, "pending": True}
        with patch("api.editor_api._invoke_manifest_cpp_api", return_value=accepted) as invoke:
            self.assertIs(setter("scene.ini", "camera", False), accepted)
            self.assertIs(SceneTools.get_vision_accumulation("scene.ini", "camera"), accepted)
        self.assertEqual([call.args for call in invoke.call_args_list], [
            ("scene_tools.set_vision_accumulation", ["scene.ini", "camera", False]),
            ("scene_tools.get_vision_accumulation", ["scene.ini", "camera"]),
        ])

    def test_import_applies_accumulation_separately_from_restir_and_denoise(self):
        document = {
            "render": {"integrator": {"type": "rt"}},
            "pipeline": {"param": {"frame_buffer": {"param": {"accumulation": "false"}}}},
            "output": {"denoise": True},
        }
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
        self.assertIn(("scene_tools.set_vision_accumulation", ["scene.ini", "MainCamera", False]), calls)
        self.assertIn(("scene_tools.set_vision_render_mode", ["scene.ini", "MainCamera", "restir"]), calls)
        self.assertIn(("scene_tools.set_vision_denoise", ["scene.ini", "MainCamera", True]), calls)
        saved = next(args[1] for wrapper, args in calls if wrapper == "main.scene_save")
        restored = vision_document.decode_vision_document(saved["vision_document"]["data"])
        self.assertIs(restored["output"]["denoise"], True)
        self.assertEqual(restored["pipeline"]["param"]["frame_buffer"]["param"]["accumulation"], "false")

    def test_import_accepts_camera_without_accumulation_method(self):
        camera = SimpleNamespace(name="OldCamera", to_dict=lambda: {"name": "OldCamera"})

        def invoke(wrapper, args):
            if wrapper == "scene_tools.is_vision_available":
                return {"available": True}
            if wrapper == "scene.get_snapshot":
                return {"scene": "scene.ini", "actors": []}
            return {"status": "success"}

        with tempfile.TemporaryDirectory() as temp_dir:
            source = Path(temp_dir) / "source.json"
            source.write_text("{}", encoding="utf-8")
            with patch("api.editor_api._invoke_manifest_cpp_api", side_effect=invoke), patch.object(
                vision_import, "prepare_external_live_vision_scene", return_value=str(source)
            ), patch.object(vision_import._NativeVisionScene, "get_active_camera", return_value=camera):
                result = vision_import.import_vision_scene_into_current_scene("scene.ini", str(source))
        self.assertEqual(result["status"], "success", result)


if __name__ == "__main__":
    unittest.main()
