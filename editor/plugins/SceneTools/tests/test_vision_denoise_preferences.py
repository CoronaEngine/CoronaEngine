import unittest

from plugins.SceneTools.vision_document import infer_vision_denoise, infer_vision_render_mode


class VisionDenoisePreferencesTests(unittest.TestCase):
    def test_import_preferences_are_independent_of_pt_and_restir(self):
        for integrator, expected_mode in (("pt", "path_tracing"), ("rt", "restir")):
            for value, expected_enabled in (
                (True, True), (False, False), ("false", False), ("true", True),
                ("0", False), ("off", False), ("on", True),
            ):
                with self.subTest(integrator=integrator, denoise=value):
                    document = {
                        "render": {"integrator": {"type": integrator, "param": {
                            "denoiser": {"type": "svgf"}}}},
                        "output": {"denoise": value},
                    }
                    self.assertEqual(infer_vision_render_mode(document), expected_mode)
                    self.assertIs(infer_vision_denoise(document), expected_enabled)

    def test_import_without_a_preference_defaults_to_disabled(self):
        self.assertIs(infer_vision_denoise({}), False)
        self.assertIs(infer_vision_denoise({"output": {}}), False)

    def test_ssat_mode_remains_supported(self):
        document = {"render": {"integrator": {"type": "pt", "param": {
            "denoiser": {"type": "SSAT"}}}}, "output": {"denoise": True}}
        self.assertEqual(infer_vision_render_mode(document), "ssat")


if __name__ == "__main__":
    unittest.main()
