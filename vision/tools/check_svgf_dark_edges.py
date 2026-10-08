"""Compare moving SVGF coverage against an independently accumulated PT image.

Use the same camera pose, resolution, exposure and tone mapper for both images.
The reference mask excludes genuinely dark geometry. Rejecting valid coverage
history at subpixel grille holes produces a measurable excess of dark pixels.

Reproduction trajectory: tools/trajectories/kitchen_radio_rotation.json.
Example (1280x720, frame 80, PT reference at the saved frame's camera pose):
  --candidate frame_frame_0080.png --reference reference80.png
  --roi 520,394,550,418 --output coverage-check.json
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def measure(candidate, reference, roi):
    actual = np.asarray(Image.open(candidate).convert("RGB"), dtype=np.float32) / 255
    expected = np.asarray(Image.open(reference).convert("RGB"), dtype=np.float32) / 255
    if actual.shape != expected.shape:
        raise ValueError("Candidate and reference resolutions differ")
    x0, y0, x1, y1 = roi
    if not (0 <= x0 < x1 <= actual.shape[1] and 0 <= y0 < y1 <= actual.shape[0]):
        raise ValueError("ROI must be inside the image")
    luma = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    # Suppress individual reference-sample noise while retaining multi-pixel
    # bands. Filter before cropping so ROI borders do not bias the comparison.
    error_image = (actual - expected) @ luma
    padded = np.pad(error_image, 1, mode="edge")
    height, width = error_image.shape
    band_error = sum(padded[y:y + height, x:x + width] * wy * wx / 16
                     for y, wy in enumerate((1, 2, 1))
                     for x, wx in enumerate((1, 2, 1)))
    actual = actual[y0:y1, x0:x1]
    expected = expected[y0:y1, x0:x1]
    error = (actual - expected) @ luma
    mask = expected @ luma > 0.25
    if not mask.any():
        raise ValueError("Reference ROI has no illuminated pixels")
    return {"pixels": int(mask.sum()),
            "dark_fraction": float(np.mean(error[mask] < -0.2)),
            "band_rmse": float(np.sqrt(np.mean(band_error[y0:y1, x0:x1] ** 2))),
            "rmse": float(np.sqrt(np.mean((actual - expected) ** 2)))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--reference", required=True)
    parser.add_argument("--roi", required=True, type=lambda value: list(map(int, value.split(","))))
    parser.add_argument("--max-dark-fraction", type=float, default=0.025)
    parser.add_argument("--max-rmse", type=float, default=0.1)
    parser.add_argument("--max-band-rmse", type=float)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = measure(args.candidate, args.reference, args.roi)
    result["passed"] = result["dark_fraction"] <= args.max_dark_fraction and result["rmse"] <= args.max_rmse
    if args.max_band_rmse is not None:
        result["passed"] = result["passed"] and result["band_rmse"] <= args.max_band_rmse
    args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()
