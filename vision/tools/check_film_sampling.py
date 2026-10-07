"""Check spatial film-sample decorrelation using actual vision-eval GPU readback.

Requires a normal perspective camera with lens_radius=0. Enable readback with
VISION_EVAL_DEBUG_FRAME and VISION_EVAL_DEBUG_DIR, and use the saved pose JSON
for that rendered frame. This checks the primary/shading wiring, not a CPU RNG
replica: project the GPU's world-space hits back to their actual film positions.
"""
import argparse
import json
from pathlib import Path

import numpy as np


def measure(scene_path, pose_path, diagnostics):
    scene = json.loads(Path(scene_path).read_text(encoding="utf-8-sig"))
    camera = scene["scene"]["camera"]["param"]
    if camera.get("lens_radius", 0) != 0:
        raise ValueError("Film projection check requires lens_radius=0")
    pose = json.loads(Path(pose_path).read_text(encoding="utf-8-sig"))
    root = Path(diagnostics)
    meta = json.loads((root / "metadata.json").read_text(encoding="utf-8-sig"))
    width, height = meta["resolution"]
    if pose["frame_index"] != meta["frame"] + 1:
        raise ValueError("Pose and diagnostic frame differ")

    def read(name):
        return np.fromfile(root / (name + ".f32"), np.float32).reshape(height, width, 4)

    position, shaded, visibility = read("position"), read("shaded"), read("visibility")
    yaw, pitch = np.radians([pose["yaw"], -pose["pitch"]])
    cy, sy, cx, sx = np.cos(yaw), np.sin(yaw), np.cos(pitch), np.sin(pitch)
    rotation = (np.diag([1, 1, -1]) @
                np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]) @
                np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]]))
    local = (position[..., :3] - pose["position"]) @ rotation
    valid = ((visibility[..., 0] < len(meta["instances"])) &
             (shaded[..., 2] == 0) & (local[..., 2] > 0.1) &
             np.all(np.isfinite(local), axis=-1))
    if valid.sum() < 100:
        raise ValueError("Not enough unreplaced surface hits")
    points = local[valid]
    focal = min(width, height) / (2 * np.tan(np.radians(camera["fov_y"]) / 2))
    film = np.stack([width / 2 + focal * points[:, 0] / points[:, 2],
                     height / 2 - focal * points[:, 1] / points[:, 2]], axis=-1)
    yy, xx = np.indices((height, width))
    offsets = film - np.stack([xx[valid] + 0.5, yy[valid] + 0.5], axis=-1)
    radius = camera["filter"]["param"].get("radius", 0.5)
    spread = offsets.std(axis=0)
    return {"pixels": int(valid.sum()), "offset_std_xy": spread.tolist(),
            "inside_filter_fraction": float(np.mean(np.all(np.abs(offsets) <= radius + 0.01, axis=-1))),
            "guide_hit_agreement": float(np.mean(shaded[..., 0][valid] == visibility[..., 0][valid])),
            "decorrelated": bool(np.all(spread > 0.1 * radius))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("scene", "pose", "diagnostics", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    result = measure(args.scene, args.pose, args.diagnostics)
    result["passed"] = (result["decorrelated"] and result["inside_filter_fraction"] > 0.999
                        and result["guide_hit_agreement"] == 1.0)
    args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()
