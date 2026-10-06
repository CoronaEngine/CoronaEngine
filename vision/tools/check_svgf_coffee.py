"""Opt-in regression check for the Coffee backdrop's SVGF bright-band artifact.

Supply matched 800x1000 PNGs from Coffee's default camera: a 2048-spp PT
reference and the final frame of a 192-frame, 1-spp SVGF run. The background
region is intentionally away from the coffee maker and contains the erroneous
horizontal bright band. Metrics describe display RGB, not linear HDR energy.

Example: python check_svgf_coffee.py --reference reference.png \
    --candidate svgf.png --output coffee-check.json
"""

import argparse
import json
from pathlib import Path

import numpy as np

from check_svgf_images import compare, read_rgb


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    reference, candidate = read_rgb(args.reference), read_rgb(args.candidate)
    if reference.shape != (1000, 800, 3) or candidate.shape != reference.shape:
        raise ValueError("Coffee default-camera gate requires matched 800x1000 RGB images")

    # Native pixel coordinates; excludes the metal support and machine silhouette.
    roi = (20, 545, 160, 575)
    x0, y0, x1, y1 = roi
    metrics = compare(reference, candidate)
    band_error = candidate[y0:y1, x0:x1] - reference[y0:y1, x0:x1]
    band_rmse = float(np.mean(band_error ** 2) ** 0.5)
    failures = []
    if band_rmse > 0.04:
        failures.append("Backdrop band RGB RMSE exceeds 0.04")
    if metrics["psnr_db"] < 28.0:
        failures.append("Full-image PSNR is below 28 dB")
    if metrics["luma_ratio"] is None or not 0.97 <= metrics["luma_ratio"] <= 1.03:
        failures.append("Mean display luma differs from PT by more than 3%")
    result = {"passed": not failures, "failures": failures,
              "metric_domain": "8-bit display RGB normalized to [0,1]",
              "backdrop_roi_xyxy": roi, "backdrop_rmse": band_rmse,
              "full_image": metrics}
    text = json.dumps(result, indent=2)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")
    print(text)
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
