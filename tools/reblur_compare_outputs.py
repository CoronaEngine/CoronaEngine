"""Compare final RGBA32F outputs from two validated fixed-input REBLUR replays.

Usage: python tools/reblur_compare_outputs.py NATIVE_RUN EDSL_RUN --output report.json
Each run contains outputs/{diffuse,specular}_l{0..2}.rgba32f. RGB and hit
distance are evaluated separately using the existing per-component threshold:
abs(edsl - native) <= 0.001 + 0.002 * abs(native). Nonfinite values always fail.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np

from reblur_compare_replay import load_run


ABSOLUTE_TOLERANCE = 0.001
RELATIVE_TOLERANCE = 0.002


def json_value(value):
    value = float(value)
    if math.isfinite(value):
        return value
    return "NaN" if math.isnan(value) else "+Infinity" if value > 0 else "-Infinity"


def error_statistics(errors):
    if not errors.size:
        return {"max_abs": None, "rmse": None, "p99_abs": None}
    # Only this owned error array is reordered by percentile computation.
    maximum = float(np.max(errors))
    rmse = math.sqrt(float(np.dot(errors, errors)) / int(errors.size))
    p99 = float(np.quantile(errors, .99, method="linear", overwrite_input=True))
    return {"max_abs": maximum, "rmse": rmse, "p99_abs": p99}


def component_metrics(reference, candidate, width, labels):
    a = np.asarray(reference, dtype=np.float64)
    b = np.asarray(candidate, dtype=np.float64)
    finite_a, finite_b = np.isfinite(a), np.isfinite(b)
    finite_pair = finite_a & finite_b
    difference = np.full(a.shape, np.nan, dtype=np.float64)
    np.subtract(b, a, out=difference, where=finite_pair)
    np.abs(difference, out=difference)
    tolerance = ABSOLUTE_TOLERANCE + RELATIVE_TOLERANCE * np.abs(a)
    mismatch = ~finite_pair | (difference > tolerance)
    finite_errors = difference[finite_pair]
    statistics = error_statistics(finite_errors)
    all_finite = bool(np.all(finite_pair))
    first = None
    count = int(np.count_nonzero(mismatch))
    if count:
        pixel, channel = divmod(int(np.argmax(mismatch)), len(labels))
        first = {"pixel": pixel, "x": pixel % width, "y": pixel // width,
                 "component": labels[channel], "reference": json_value(a[pixel, channel]),
                 "candidate": json_value(b[pixel, channel]),
                 "abs_error": float(difference[pixel, channel]) if finite_pair[pixel, channel] else None,
                 "tolerance": float(tolerance[pixel, channel]) if finite_a[pixel, channel] else None}
    result = {
        "components": int(a.size), "all_finite": all_finite,
        "reference_nonfinite_components": int(np.count_nonzero(~finite_a)),
        "candidate_nonfinite_components": int(np.count_nonzero(~finite_b)),
        "nonfinite_component_pairs": int(np.count_nonzero(~finite_pair)),
        "finite_component_pairs": int(np.count_nonzero(finite_pair)),
        "mismatch_components": count,
        "mismatch_pixels": int(np.count_nonzero(np.any(mismatch, axis=1))),
        "first_mismatch": first,
        **(statistics if all_finite else {key: None for key in statistics}),
    }
    if not all_finite:
        result["finite_pair_statistics"] = statistics
    return result, finite_errors


def aggregate_metrics(records, errors):
    all_finite = all(record["all_finite"] for record in records)
    statistics = error_statistics(np.concatenate(errors))
    errors.clear()
    result = {key: sum(record[key] for record in records) for key in (
        "components", "reference_nonfinite_components", "candidate_nonfinite_components",
        "nonfinite_component_pairs", "finite_component_pairs", "mismatch_components", "mismatch_pixels")}
    result.update(all_finite=all_finite)
    result.update(statistics if all_finite else {key: None for key in statistics})
    if not all_finite:
        result["finite_pair_statistics"] = statistics
    return result


def read_output(directory, name, pixels):
    path = directory / "outputs" / name
    expected = pixels * 4 * 4
    if path.stat().st_size != expected:
        raise ValueError(f"{path}: expected {expected} bytes for {pixels} RGBA32F pixels, found {path.stat().st_size}")
    return np.fromfile(path, dtype="<f4").reshape(pixels, 4)


def compare_outputs(native_dir, edsl_dir):
    native = load_run(native_dir, "native")
    edsl = load_run(edsl_dir, "edsl")
    mismatched = [key for key in native if native[key] != edsl[key]]
    if mismatched:
        raise ValueError("replay metadata/input mismatch: " + ", ".join(mismatched))
    width, height = native["size"]
    files = []
    group_records = {"rgb": [], "hit": []}
    group_errors = {"rgb": [], "hit": []}
    first_mismatch = None
    for layer in range(native["layers"]):
        for signal in ("diffuse", "specular"):
            name = f"{signal}_l{layer}.rgba32f"
            a = read_output(native_dir, name, width * height)
            b = read_output(edsl_dir, name, width * height)
            record = {"path": "outputs/" + name, "layer": layer, "signal": signal}
            for group, columns, labels in (("rgb", slice(0, 3), "RGB"), ("hit", slice(3, 4), "H")):
                metrics, errors = component_metrics(a[:, columns], b[:, columns], width, labels)
                record[group] = metrics
                group_records[group].append(metrics)
                group_errors[group].append(errors)
                if first_mismatch is None and metrics["first_mismatch"] is not None:
                    first_mismatch = {"path": record["path"], "group": group, **metrics["first_mismatch"]}
            record["rgb"]["channels"] = {
                label: component_metrics(a[:, index:index + 1], b[:, index:index + 1], width, label)[0]
                for index, label in enumerate("RGB")}
            files.append(record)
    summary = {group: aggregate_metrics(group_records[group], group_errors[group]) for group in ("rgb", "hit")}
    correct = all(row["all_finite"] and row["mismatch_components"] == 0 for row in summary.values())
    return {
        "status": "ok" if correct else "different",
        "native_run": str(native_dir.resolve()), "edsl_run": str(edsl_dir.resolve()),
        "protocol": native,
        "validation": {"run_completion_and_cleanup": True, "matching_metadata_and_input": True,
                       "six_rgba32f_output_shapes": True},
        "tolerance": {"absolute": ABSOLUTE_TOLERANCE, "relative": RELATIVE_TOLERANCE,
                      "formula": "abs(edsl-native) <= 0.001 + 0.002*abs(native)", "reference": "native"},
        "percentile_method": "linear interpolation at (n-1)*0.99",
        "nonfinite_policy": "Every nonfinite component fails. Aggregate numerical metrics are null if any component is nonfinite; finite-only diagnostics are reported separately.",
        "summary": summary, "first_mismatch": first_mismatch, "files": files,
    }


def number(value):
    return "nonfinite" if value is None else f"{value:.8g}"


def print_table(report):
    print("native reference; per-component tolerance = 0.001 + 0.002*abs(native)")
    print(f"{'output':24} {'group':5} {'finite':>7} {'max-abs':>12} {'RMSE':>12} {'p99-abs':>12} {'mismatch':>10}")
    for row in [*report["files"], {"path": "ALL", **report["summary"]}]:
        for group in ("rgb", "hit"):
            item = row[group]
            print(f"{Path(row['path']).name:24} {group:5} {str(item['all_finite']):>7} "
                  f"{number(item['max_abs']):>12} {number(item['rmse']):>12} "
                  f"{number(item['p99_abs']):>12} {item['mismatch_components']:10}")
    if report["first_mismatch"]:
        print("First mismatch: " + json.dumps(report["first_mismatch"], allow_nan=False))
    print("Result: " + report["status"])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("native_run", type=Path)
    parser.add_argument("edsl_run", type=Path)
    parser.add_argument("--output", type=Path, required=True, help="JSON report, including validation failures")
    args = parser.parse_args(argv)
    try:
        report = compare_outputs(args.native_run, args.edsl_run)
    except (OSError, ValueError, KeyError, TypeError) as error:
        report = {"status": "invalid", "error": str(error),
                  "native_run": str(args.native_run.resolve()), "edsl_run": str(args.edsl_run.resolve())}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    if report["status"] == "invalid":
        print("Invalid comparison: " + report["error"])
        return 2
    print_table(report)
    return 0 if report["status"] == "ok" else 1


if __name__ == "__main__":
    raise SystemExit(main())
