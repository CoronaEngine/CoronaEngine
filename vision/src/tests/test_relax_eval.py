import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np

spec = importlib.util.spec_from_file_location("relax_eval", Path(__file__).parents[2] / "tools/relax_eval.py")
evaluation = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evaluation)


class EvaluationContract(unittest.TestCase):
    def test_missing_scene_is_reported_before_render(self):
        with tempfile.TemporaryDirectory() as tmp:
            manifest = {"scenarios": [{"name": "missing-kitchen", "scene": str(Path(tmp) / "absent.json")}]}
            with self.assertRaisesRegex(FileNotFoundError, "missing-kitchen.*absent.json"):
                evaluation.preflight(manifest)

    def test_missing_asset_is_reported_before_render(self):
        with tempfile.TemporaryDirectory() as tmp:
            scene = Path(tmp) / "scene.json"
            scene.write_text(json.dumps({"scene": {"shapes": [{"param": {"fn": "absent.obj"}}]}}))
            with self.assertRaisesRegex(FileNotFoundError, "missing-mesh.*absent.obj"):
                evaluation.preflight({"scenarios": [{"name": "missing-mesh", "scene": str(scene)}]})

    def test_roi_error_uses_linear_rgb(self):
        image = np.zeros((2, 2, 3)); reference = np.ones_like(image)
        image[0, 0] = [1, 1, 1]
        metrics = evaluation.measure(image, reference, {"full": [0, 0, 1, 1], "corner": [0, 0, .5, .5]})
        self.assertAlmostEqual(metrics["full"]["linear_rmse"], np.sqrt(.75))
        self.assertEqual(metrics["corner"]["linear_rmse"], 0)

    def test_moving_reference_requires_same_pose(self):
        pose = {"position": [0, 1, 2], "pitch": 0, "yaw": 10}
        evaluation.require_same_pose(pose, dict(pose))
        with self.assertRaisesRegex(ValueError, "pose"):
            evaluation.require_same_pose(pose, dict(pose, yaw=11))


if __name__ == "__main__":
    unittest.main()
