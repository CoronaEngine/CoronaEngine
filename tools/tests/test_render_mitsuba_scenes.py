"""Offline checks for batch selection, evidence retention and capture validation."""

import importlib.util
import json
from pathlib import Path
import struct
import socket
import tempfile
import subprocess
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "render_mitsuba_scenes.py"


class RenderMitsubaScenesTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(SCRIPT.is_file(), "The reusable batch screenshot tool is missing")
        spec = importlib.util.spec_from_file_location("render_mitsuba_scenes", SCRIPT)
        self.tool = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.tool)
        root = SCRIPT.parents[1] / ".workspace/tmp/batch-screenshot-tests"
        root.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=root)
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)

    def manifest(self):
        scenes = [
            {"id": "kitchen", "directory": "kitchen", "xml_files": ["scene_v0.6.xml", "scene_v3.xml"]},
            {"id": "cornell-box", "directory": "cornell-box", "xml_files": ["scene.xml"]},
        ]
        for scene in scenes:
            source = self.directory / scene["directory"]
            source.mkdir()
            for filename in scene["xml_files"]:
                (source / filename).write_text("<scene/>", encoding="utf-8")
        (self.directory / "manifest.json").write_text(json.dumps({"scenes": scenes}), encoding="utf-8")

    def test_selected_scene_uses_mitsuba3_source_without_reordering_manifest(self):
        self.manifest()
        selected = self.tool.select_scenes(self.directory, ["cornell-box", "kitchen"])
        self.assertEqual([scene["id"] for scene in selected], ["kitchen", "cornell-box"])
        self.assertEqual(selected[0]["source"], self.directory / "kitchen/scene_v3.xml")

    def test_unknown_scene_is_rejected_instead_of_silently_rendering_nothing(self):
        self.manifest()
        with self.assertRaisesRegex(ValueError, "missing-scene"):
            self.tool.select_scenes(self.directory, ["missing-scene"])

    def test_occupied_debug_port_is_rejected_before_connecting_to_another_engine(self):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            with self.assertRaisesRegex(RuntimeError, "already in use"):
                self.tool.ensure_port_available(listener.getsockname()[1])

    def test_fixed_frame_screenshot_options_keep_exact_counts_and_steady_window(self):
        output = self.directory / "pt2048"
        pt = self.tool.capture_options("pt2048", None, output, 7200)
        reblur = self.tool.capture_options("restir_reblur", None, output, 7200)
        smoke = self.tool.capture_options("pt_reblur", 8, output, 7200)
        self.assertEqual((pt["render_frames"], pt["warmup_frames"]), (2048, 0))
        self.assertEqual((reblur["render_frames"], reblur["warmup_frames"]), (64, 32))
        self.assertEqual((smoke["render_frames"], smoke["warmup_frames"]), (8, 4))
        self.assertEqual(pt["timings_path"], str(output / "frames.csv"))
        self.assertEqual(pt["timeout_seconds"], 7200)

    def test_existing_capture_directory_is_preserved_and_rejected(self):
        output = self.directory / "output"
        output.mkdir()
        evidence = output / "result.json"
        evidence.write_text("original evidence", encoding="utf-8")
        with self.assertRaises(FileExistsError):
            self.tool.prepare_output(output)
        self.assertEqual(evidence.read_text(encoding="utf-8"), "original evidence")

    def test_api_error_payload_cannot_be_recorded_as_a_successful_capture(self):
        with self.assertRaisesRegex(RuntimeError, "Screenshot save failed"):
            self.tool.api_data("SceneTools.save_screenshot", {
                "success": True, "data": {"status": "error", "error": "Screenshot save failed"},
            })
        self.assertEqual(self.tool.api_data("SceneTools.save_screenshot", {
            "success": True, "data": {"status": "success", "ok": True, "path": "image.png"},
        })["path"], "image.png")

    def test_inherited_batch_pacing_and_render_overrides_are_removed(self):
        environment = self.tool.engine_environment({
            "PATH": "runtime", "CORONA_OPTICS_MAX_FPS": "20", "CORONA_BATCH_CAPTURE_DIR": "old",
            "VISION_DISABLE_DENOISER": "1", "REBLUR_TEST": "1", "KEEP_ME": "unchanged",
        }, self.directory, 9237)
        self.assertNotIn("CORONA_OPTICS_MAX_FPS", environment)
        self.assertNotIn("CORONA_BATCH_CAPTURE_DIR", environment)
        self.assertNotIn("VISION_DISABLE_DENOISER", environment)
        self.assertNotIn("REBLUR_TEST", environment)
        self.assertEqual(environment["KEEP_ME"], "unchanged")

    def test_fixed_resolution_viewport_is_centered_without_stretching(self):
        script = self.tool.fixed_viewport_script()
        javascript = """globalThis.calls=[];
globalThis.window={coronaBridge:{setCameraViewport:(...args)=>{calls.push(args);return true}}};
""" + script + """;
window.coronaBridge.setCameraViewport(7,10,20,1600,1000,999,999);
window.coronaBridge.setCameraViewport(7,0,0,800,1000,999,999);
window.coronaBridge.setCameraViewport(7,0,0,1600,900,999,999);
console.log(JSON.stringify(calls));"""
        result = subprocess.run(["node", "--input-type=module", "-e", javascript],
                                check=True, capture_output=True, text=True)
        self.assertEqual(json.loads(result.stdout), [
            [7, 10, 70, 1600, 900, 1920, 1080],
            [7, 0, 275, 800, 450, 1920, 1080],
            [7, 0, 0, 1600, 900, 1920, 1080],
        ])

    def test_incomplete_frame_csv_is_rejected_even_when_png_exists(self):
        output = self.directory / "capture"
        output.mkdir()
        (output / "image.png").write_bytes(b"\x89PNG\r\n\x1a\n" + struct.pack(">I4sII", 13, b"IHDR", 1920, 1080))
        (output / "frames.csv").write_text(
            "frame,integrator_frame,render_including_denoise_ms,denoise_ms\n1,1,10,0\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "frames"):
            self.tool.read_capture(output, 2, 0, False)


if __name__ == "__main__":
    unittest.main()
