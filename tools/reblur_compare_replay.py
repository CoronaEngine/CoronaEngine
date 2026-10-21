"""Compare validated fixed-input REBLUR replay GPU timings.

Usage: python tools/reblur_compare_replay.py NATIVE_RUN EDSL_RUN --output report.json
Runs must complete the 64 warmup + 32 measured frame, three-layer, ten-pass
protocol. Default --scope pass_sum sums individual pass queries. Scopes nrd and
filter instead read actual contiguous GPU interval queries, then sum the three
layer intervals in each measured frame. These totals are not pass_sum timings.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import re
import statistics
from pathlib import Path


WARMUP, MEASURED, LAYERS = 64, 32, 3
STAGES = (
    "Pack", "ClassifyTiles", "HitDistReconstruction5x5", "PrePass",
    "TemporalAccumulation", "HistoryFix", "Blur", "PostBlur",
    "TemporalStabilization", "Unpack",
)
EDSL_NAMES = tuple("REBLUR-EDSL-HistoryFix" if name == "HistoryFix"
                   else "NRD-EDSL-REBLUR-" + name for name in STAGES)
NATIVE_NAMES = dict(zip((
    "Pack", "REBLUR_DiffuseSpecular - Classify tiles",
    "REBLUR_DiffuseSpecular - Hit distance reconstruction",
    "REBLUR_DiffuseSpecular - Pre-pass",
    "REBLUR_DiffuseSpecular - Temporal accumulation",
    "REBLUR_DiffuseSpecular - History fix", "REBLUR_DiffuseSpecular - Blur",
    "REBLUR_DiffuseSpecular - Post-blur",
    "REBLUR_DiffuseSpecular - Temporal stabilization", "Unpack",
), STAGES))
MATRICES = ("world_to_view", "world_to_view_prev", "view_to_clip", "view_to_clip_prev")
HEADER = re.compile(
    r"^REBLUR fixed-input replay backend=(native|edsl) size=(\d+)x(\d+) "
    r"layers=(\d+) warmup=(\d+) measure_frames=(\d+) frame_pause_ms=(\d+)$",
    re.MULTILINE,
)


def single_match(pattern, text, description):
    matches = list(re.finditer(pattern, text, re.MULTILINE))
    if len(matches) != 1:
        raise ValueError(f"expected exactly one {description}, found {len(matches)}")
    return matches[0]


def load_run(directory, backend):
    path = directory / "run.json"
    run = json.loads(path.read_text(encoding="utf-8-sig"))
    if (not isinstance(run, dict) or run.get("reason") != "completed" or run.get("returncode") != 0
            or run.get("cleanup_ok") is not True or run.get("gpu_cleanup_ok") is not True):
        raise ValueError(f"{path}: run must be completed, returncode=0, cleanup_ok=true, gpu_cleanup_ok=true")
    text = (directory / "stdout.log").read_text(encoding="utf-8-sig")
    matches = list(HEADER.finditer(text))
    if len(matches) != 1 or matches[0][1] != backend:
        raise ValueError(f"{directory}: expected one {backend} replay header")
    values = list(map(int, matches[0].groups()[1:]))
    width, height, layers, warmup, measured, pause = values
    if width <= 0 or height <= 0 or (layers, warmup, measured) != (LAYERS, WARMUP, MEASURED):
        raise ValueError(f"{directory}: requires positive size, {LAYERS} layers, {WARMUP} warmup and {MEASURED} measured frames")
    settings = single_match(r"^Settings: (.+)$", text, "Settings line")[1]
    camera = single_match(r"^Camera: (.+)$", text, "Camera line")[1]
    fingerprint = single_match(r"^Input combined_fnv64=([0-9a-fA-F]{16})(?: .*)?$", text, "input hash")[1].lower()
    matrices = {}
    for name in MATRICES:
        match = single_match(
            rf"^{name} \(column-major bytes, displayed as rows\):\n((?:  [^\n]+\n){{4}})",
            text, name + " matrix",
        )
        rows = [row.strip() for row in match[1].splitlines()]
        for row in rows:
            entries = row.split(",")
            if len(entries) != 4 or not all(math.isfinite(float(entry)) for entry in entries):
                raise ValueError(f"{directory}: invalid {name} matrix row: {row!r}")
        matrices[name] = rows
    if "PASS: replay completed;" not in text:
        raise ValueError(f"{directory}: missing replay completion marker")
    return {
        "size": [width, height], "layers": layers, "warmup": warmup,
        "measure_frames": measured, "frame_pause_ms": pause,
        "settings": settings, "camera": camera, "matrices": matrices,
        "input_combined_fnv64": fingerprint,
    }


def load_csv(directory, required):
    path = directory / "passes.csv"
    with path.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        if not required.issubset(reader.fieldnames or ()):
            raise ValueError(f"{path}: missing columns {sorted(required - set(reader.fieldnames or ()))}")
        rows = list(reader)
    for number, row in enumerate(rows, 2):
        try:
            value = float(row["ms"])
            if not math.isfinite(value) or value < 0:
                raise ValueError("duration must be finite and nonnegative")
            row["ms"] = value
        except (TypeError, ValueError) as error:
            raise ValueError(f"{path}:{number}: invalid GPU ms: {error}") from error
    return rows


def measured_keys():
    return {(frame, layer, stage) for frame in range(WARMUP, WARMUP + MEASURED)
            for layer in range(LAYERS) for stage in STAGES}


def native_samples(directory, size):
    rows = load_csv(directory, {"frame", "layer", "name", "width", "height", "ms"})
    samples, clears = {}, 0
    for number, row in enumerate(rows, 2):
        frame, layer = int(row["frame"]), int(row["layer"])
        if not (0 <= frame < WARMUP + MEASURED and 0 <= layer < LAYERS):
            raise ValueError(f"{directory}/passes.csv:{number}: frame/layer outside replay protocol")
        if row["name"].startswith("Clear ("):
            clears += 1
            continue
        stage = NATIVE_NAMES.get(row["name"])
        if stage is None:
            raise ValueError(f"{directory}/passes.csv:{number}: unexpected native stage {row['name']!r}")
        if [int(row["width"]), int(row["height"])] != size:
            raise ValueError(f"{directory}/passes.csv:{number}: native image dimensions disagree with stdout")
        if frame < WARMUP:
            continue
        key = frame, layer, stage
        if key in samples:
            raise ValueError(f"{directory}/passes.csv:{number}: duplicate measured frame/layer/stage {key}")
        samples[key] = row["ms"]
    missing = measured_keys() - samples.keys()
    if missing:
        raise ValueError(f"{directory}/passes.csv: missing {len(missing)} measured frame/layer/stage samples; first={min(missing)}")
    return samples, {"rows": len(rows), "clear_rows_ignored": clears, "measured_rows": len(samples)}


def edsl_samples(directory, size):
    rows = load_csv(directory, {"batch", "name", "width", "height", "ms"})
    expected = (WARMUP + MEASURED) * LAYERS * len(STAGES)
    if len(rows) != expected:
        raise ValueError(f"{directory}/passes.csv: expected {expected} rows (96 frames x 3 layers x 10 stages), found {len(rows)}")
    samples = {}
    for index, row in enumerate(rows):
        stage_index = index % len(STAGES)
        frame = index // (LAYERS * len(STAGES))
        layer = (index // len(STAGES)) % LAYERS
        if row["name"] != EDSL_NAMES[stage_index]:
            raise ValueError(f"{directory}/passes.csv:{index + 2}: frame={frame} layer={layer} expected {EDSL_NAMES[stage_index]!r}, found {row['name']!r}")
        if stage_index in (0, len(STAGES) - 1) and [int(row["width"]), int(row["height"])] != size:
            raise ValueError(f"{directory}/passes.csv:{index + 2}: Pack/Unpack dimensions disagree with stdout")
        if frame >= WARMUP:
            samples[frame, layer, STAGES[stage_index]] = row["ms"]
    return samples, {"rows": len(rows), "measured_rows": len(samples),
                     "frame_layer_source": "validated contiguous ten-stage groups; 30 rows per frame"}


def interval_samples(directory, backend, size, scope):
    rows = load_csv(directory, {"batch", "name", "width", "height", "ms"})
    names = ("REBLUR-Filter", "REBLUR-NRD") if backend == "native" else (
        "REBLUR-NRD" if scope == "nrd" else "REBLUR-Filter",
    )
    intervals = (WARMUP + MEASURED) * LAYERS
    expected = intervals * len(names)
    if len(rows) != expected:
        raise ValueError(f"{directory}/passes.csv: expected {expected} aggregate rows "
                         f"(96 frames x 3 layers x {len(names)} interval names), found {len(rows)}")
    selected = "REBLUR-NRD" if scope == "nrd" else "REBLUR-Filter"
    samples, previous_batch = {}, -1
    for index, row in enumerate(rows):
        ordinal, name_index = divmod(index, len(names))
        frame, layer = divmod(ordinal, LAYERS)
        location = f"{directory}/passes.csv:{index + 2}"
        if row["name"] != names[name_index]:
            raise ValueError(f"{location}: frame={frame} layer={layer} expected "
                             f"{names[name_index]!r}, found {row['name']!r}")
        if [int(row["width"]), int(row["height"])] != size:
            raise ValueError(f"{location}: aggregate image dimensions disagree with stdout")
        batch = int(row["batch"])
        if batch < 0 or batch < previous_batch:
            raise ValueError(f"{location}: aggregate batches must be nonnegative and ordered")
        if backend == "native":
            if name_index == 0 and index and batch != previous_batch + 1:
                raise ValueError(f"{location}: native interval pairs require consecutive batches")
            if name_index == 1:
                if batch != previous_batch:
                    raise ValueError(f"{location}: native Filter/NRD pair must share one batch")
                if row["ms"] > rows[index - 1]["ms"]:
                    raise ValueError(f"{location}: native NRD interval exceeds its enclosing Filter interval")
        previous_batch = batch
        if row["name"] == selected and frame >= WARMUP:
            samples[frame, layer] = row["ms"]
    return samples, {"rows": len(rows), "interval_name": selected,
                     "measured_rows": len(samples),
                     "frame_layer_source": "validated CSV order; three layer intervals per frame; batch is a submission serial, not a frame number"}


def percentile(values, p):
    ordered = sorted(values)
    position = (len(ordered) - 1) * p
    low = math.floor(position)
    high = math.ceil(position)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def summarize(values):
    return {"samples": len(values), "mean_ms": statistics.fmean(values),
            "p50_ms": percentile(values, .5), "p95_ms": percentile(values, .95)}


def comparison_row(stage, native, edsl):
    left, right = summarize(native), summarize(edsl)
    return {"stage": stage, "hlsl": left, "edsl": right,
            "edsl_over_hlsl": {key.removesuffix("_ms"): right[key] / left[key] if left[key] else None
                               for key in ("mean_ms", "p50_ms", "p95_ms")}}


def compare_replay(native_dir, edsl_dir, scope="pass_sum"):
    if scope not in ("pass_sum", "nrd", "filter"):
        raise ValueError(f"unsupported replay scope: {scope!r}")
    native = load_run(native_dir, "native")
    edsl = load_run(edsl_dir, "edsl")
    mismatched = [key for key in native if native[key] != edsl[key]]
    if mismatched:
        raise ValueError("replay metadata/input mismatch: " + ", ".join(mismatched))
    if scope == "pass_sum":
        left, left_validation = native_samples(native_dir, native["size"])
        right, right_validation = edsl_samples(edsl_dir, native["size"])
        per_frame_left, per_frame_right, comparisons = [], [], []
        for stage in STAGES:
            a = [sum(left[frame, layer, stage] for layer in range(LAYERS))
                 for frame in range(WARMUP, WARMUP + MEASURED)]
            b = [sum(right[frame, layer, stage] for layer in range(LAYERS))
                 for frame in range(WARMUP, WARMUP + MEASURED)]
            comparisons.append(comparison_row(stage, a, b))
            per_frame_left.append(a)
            per_frame_right.append(b)
        comparisons.append(comparison_row("pass_sum", list(map(sum, zip(*per_frame_left))),
                                          list(map(sum, zip(*per_frame_right)))))
        note = "Each sample sums GPU pass queries across three layers in one frame. pass_sum adds all ten stages including Pack/Unpack; it is not a contiguous GPU interval."
    else:
        left, left_validation = interval_samples(native_dir, "native", native["size"], scope)
        right, right_validation = interval_samples(edsl_dir, "edsl", native["size"], scope)
        a = [sum(left[frame, layer] for layer in range(LAYERS))
             for frame in range(WARMUP, WARMUP + MEASURED)]
        b = [sum(right[frame, layer] for layer in range(LAYERS))
             for frame in range(WARMUP, WARMUP + MEASURED)]
        comparisons = [comparison_row(left_validation["interval_name"], a, b)]
        note = ("Each sample sums three actual contiguous GPU query intervals, one per layer in a frame. "
                "These are aggregate interval measurements, not pass_sum or a sum of individual pass queries. "
                + ("NRD excludes Pack/Unpack." if scope == "nrd" else "Filter includes Pack/Unpack."))
    return {
        "status": "ok", "scope": scope, "note": note,
        "native_run": str(native_dir.resolve()), "edsl_run": str(edsl_dir.resolve()),
        "protocol": native, "measured_frame_range": [WARMUP, WARMUP + MEASURED - 1],
        "validation": {"run_completion_and_cleanup": True, "matching_metadata_and_input": True,
                       "native": left_validation, "edsl": right_validation},
        "percentile_method": "linear interpolation at (n-1)*p",
        "stages": comparisons,
    }


def print_table(report):
    if report["scope"] == "pass_sum":
        print("Three-layer GPU pass sums per measured frame (ms); not a contiguous GPU interval")
    else:
        print(f"Three-layer {report['stages'][0]['stage']} GPU intervals per measured frame (ms); "
              "sum of three contiguous layer intervals, not pass_sum")
    print(f"{'stage':29} {'HLSL mean':>10} {'p50':>9} {'p95':>9} {'EDSL mean':>10} {'p50':>9} {'p95':>9} {'ratio':>8}")
    for row in report["stages"]:
        a, b = row["hlsl"], row["edsl"]
        ratio = row["edsl_over_hlsl"]["mean"]
        ratio_text = f"{ratio:.3f}x" if ratio is not None else "n/a"
        print(f"{row['stage']:29} {a['mean_ms']:10.4f} {a['p50_ms']:9.4f} {a['p95_ms']:9.4f} "
              f"{b['mean_ms']:10.4f} {b['p50_ms']:9.4f} {b['p95_ms']:9.4f} {ratio_text:>8}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("native_run", type=Path)
    parser.add_argument("edsl_run", type=Path)
    parser.add_argument("--scope", choices=("pass_sum", "nrd", "filter"), default="pass_sum")
    parser.add_argument("--output", type=Path, required=True, help="JSON report, including validation failures")
    args = parser.parse_args(argv)
    try:
        report = compare_replay(args.native_run, args.edsl_run, args.scope)
    except (OSError, ValueError, KeyError, TypeError) as error:
        report = {"status": "invalid", "error": str(error),
                  "scope": args.scope,
                  "native_run": str(args.native_run.resolve()), "edsl_run": str(args.edsl_run.resolve())}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    if report["status"] != "ok":
        print("Invalid comparison: " + report["error"])
        return 2
    print_table(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
