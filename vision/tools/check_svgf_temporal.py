"""Measure stationary edge flicker in consecutive, tone-mapped renderer frames.

Example: --frames "capture/frame_frame_*.png" --reference pt2048.png
The mask comes only from the PT reference, never from the candidate being tested.
Values are normalized display RGB; --roi is x0,y0,x1,y1 in native pixels.
"""
import argparse
import glob
import json
from pathlib import Path

import numpy as np
from PIL import Image


def measure(paths, reference, roi=None):
    ref = np.asarray(Image.open(reference).convert("RGB"), dtype=np.float32) / 255
    luma = ref @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    p = np.pad(luma, 1, mode="edge")
    gx = (p[:-2, 2:] + 2*p[1:-1, 2:] + p[2:, 2:]
          - p[:-2, :-2] - 2*p[1:-1, :-2] - p[2:, :-2]) / 4
    gy = (p[2:, :-2] + 2*p[2:, 1:-1] + p[2:, 2:]
          - p[:-2, :-2] - 2*p[:-2, 1:-1] - p[:-2, 2:]) / 4
    e = np.pad(np.hypot(gx, gy) > 0.08, 1)
    edge = np.logical_or.reduce([e[y:y+luma.shape[0], x:x+luma.shape[1]]
                                 for y in range(3) for x in range(3)])
    if roi:
        region = np.zeros(edge.shape, dtype=bool)
        x0, y0, x1, y1 = roi
        region[y0:y1, x0:x1] = True
        edge &= region
    if not edge.any() or len(paths) < 30:
        raise ValueError("Need reference edges and at least 30 consecutive frames")
    numbers = [int(Path(p).stem.rsplit("_", 1)[1]) for p in paths]
    if numbers != list(range(numbers[0], numbers[0] + len(numbers))):
        raise ValueError("Frame sequence has gaps or duplicates")
    total = np.zeros_like(ref, dtype=np.float64)
    squares = np.zeros_like(total)
    diff2 = error2 = 0.0
    previous = None
    minimum = np.full((int(edge.sum()), 3), np.inf, dtype=np.float32)
    maximum = np.full_like(minimum, -np.inf)
    for path in paths:
        frame = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32) / 255
        if frame.shape != ref.shape or not np.isfinite(frame).all():
            raise ValueError(f"Invalid frame {path}")
        total += frame
        squares += frame.astype(np.float64) ** 2
        minimum = np.minimum(minimum, frame[edge])
        maximum = np.maximum(maximum, frame[edge])
        error2 += float(np.mean((frame - ref) ** 2))
        if previous is not None:
            diff2 += float(np.mean((frame[edge] - previous[edge]) ** 2))
        previous = frame
    mean = total / len(paths)
    variance = np.maximum(squares / len(paths) - mean ** 2, 0)
    return {
        "frames": len(paths), "first_frame": numbers[0], "last_frame": numbers[-1],
        "edge_pixels": int(edge.sum()), "roi": roi,
        "edge_temporal_rms": float(np.sqrt(variance[edge].mean())),
        "edge_frame_delta_rms": float(np.sqrt(diff2 / (len(paths) - 1))),
        # Adjacent-frame deltas miss slow breathing left by a bounded EMA.
        # Measure full-sequence excursions on the reference-derived edge mask.
        "edge_range_p95": float(np.percentile(np.mean(maximum - minimum, axis=1), 95)),
        "mean_frame_psnr": float(-10 * np.log10(error2 / len(paths))),
        "temporal_mean_psnr": float(-10 * np.log10(np.mean((mean - ref) ** 2))),
        "edge_mean_bias_rmse": float(np.sqrt(np.mean((mean[edge] - ref[edge]) ** 2))),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frames", required=True)
    parser.add_argument("--reference", required=True)
    parser.add_argument("--first-frame", type=int)
    parser.add_argument("--last-frame", type=int)
    parser.add_argument("--roi", type=lambda s: [int(v) for v in s.split(",")])
    parser.add_argument("--max-edge-rms", type=float, default=0.012)
    parser.add_argument("--max-edge-range-p95", type=float,
                        help="Optional limit on slow edge excursions over the whole capture")
    parser.add_argument("--min-psnr", type=float, default=28.0,
                        help="Quality floor: a stable but black/blurred output must not pass")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    paths = sorted(glob.glob(args.frames))
    paths = [p for p in paths
             if (args.first_frame is None or int(Path(p).stem.rsplit("_", 1)[1]) >= args.first_frame)
             and (args.last_frame is None or int(Path(p).stem.rsplit("_", 1)[1]) <= args.last_frame)]
    result = measure(paths, args.reference, args.roi)
    result["max_edge_rms"] = args.max_edge_rms
    result["min_psnr"] = args.min_psnr
    result["max_edge_range_p95"] = args.max_edge_range_p95
    result["passed"] = (result["edge_temporal_rms"] <= args.max_edge_rms
                        and result["mean_frame_psnr"] >= args.min_psnr
                        and (args.max_edge_range_p95 is None
                             or result["edge_range_p95"] <= args.max_edge_range_p95))
    args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()
