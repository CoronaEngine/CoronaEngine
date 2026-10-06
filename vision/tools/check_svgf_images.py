"""Compare exported display RGB images from matched PT/SVGF evaluation runs.

Requires numpy and Pillow. This is an opt-in image regression gate: callers must
supply images from the same scene, camera, resolution and display transform.
Example:
  python check_svgf_images.py --reference pt.png --candidate svgf.png \
      --baseline old_svgf.png --min-psnr 25 --min-luma-ratio .9 \
      --max-luma-ratio 1.1 --min-shadow-ratio .7 --output quality.json
Metrics describe tone-mapped PNGs, not linear HDR radiometric error.
"""

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def read_rgb(path):
    with Image.open(path) as image:
        return np.asarray(image.convert("RGB"), dtype=np.float64) / 255.0


def compare(reference, candidate):
    if reference.shape != candidate.shape:
        raise ValueError(f"Image dimensions differ: {reference.shape} vs {candidate.shape}")
    mse = float(np.mean(np.square(reference - candidate)))
    weights = np.array([0.2126, 0.7152, 0.0722])
    reference_y = reference @ weights
    candidate_y = candidate @ weights
    shadow = reference_y < 0.2
    reference_mean = float(reference_y.mean())
    shadow_mean = float(reference_y[shadow].mean()) if shadow.any() else 0.0
    return {
        "rmse": mse ** 0.5,
        "psnr_db": -10.0 * np.log10(max(mse, 1e-15)),
        "luma_ratio": float(candidate_y.mean() / reference_mean) if reference_mean > 0 else None,
        "shadow_ratio": float(candidate_y[shadow].mean() / shadow_mean) if shadow_mean > 0 else None,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--min-psnr", type=float, default=0.0)
    parser.add_argument("--min-luma-ratio", type=float, default=0.9)
    parser.add_argument("--max-luma-ratio", type=float, default=1.1)
    parser.add_argument("--min-shadow-ratio", type=float, default=0.7)
    args = parser.parse_args()
    reference = read_rgb(args.reference)
    result = {"metric_domain": "display RGB [0,1]; shadow mask is reference Y' < 0.2",
              "candidate": compare(reference, read_rgb(args.candidate))}
    metrics = result["candidate"]
    failures = []
    if metrics["psnr_db"] < args.min_psnr:
        failures.append("PSNR below threshold")
    if metrics["luma_ratio"] is None or not args.min_luma_ratio <= metrics["luma_ratio"] <= args.max_luma_ratio:
        failures.append("Mean display luma outside allowed range")
    if metrics["shadow_ratio"] is not None and metrics["shadow_ratio"] < args.min_shadow_ratio:
        failures.append("Shadow display luma below threshold")
    if args.baseline:
        result["baseline"] = compare(reference, read_rgb(args.baseline))
        if metrics["rmse"] >= result["baseline"]["rmse"]:
            failures.append("Candidate does not improve baseline RGB RMSE")
    result["failures"] = failures
    result["passed"] = not failures
    text = json.dumps(result, indent=2)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")
    print(text)
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
