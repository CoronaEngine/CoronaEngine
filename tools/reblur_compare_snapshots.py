"""Compare native NRD and EDSL raw stage snapshots without numerical tolerances.

Usage: python tools/reblur_compare_snapshots.py NATIVE_DIR EDSL_DIR --output report.json
Both directories contain metadata.jsonl and fN_lN_pN_uN.bin captures. Native
format names and NRD 4.14.3 numeric Format values are accepted. The latter are
used only when a format name is absent, as in the EDSL capture metadata.
Repeated metadata paths use the last record, matching the capture's overwrite
behavior; their count is reported. Pack constants have different host layouts
and are skipped. Every other stage requires byte-identical constants.
"""
from __future__ import annotations

import argparse
import json
import math
import re
import struct
from pathlib import Path


TEXTURE = re.compile(r"^f(\d+)_l(\d+)_p(\d+)_u(\d+)\.bin$")
CONSTANTS = re.compile(r"^f(\d+)_l(\d+)_p(\d+)\.constants\.bin$")
# Canonical format: bytes/pixel, component names, little-endian unpack code.
FORMATS = {
    "R8_UNORM": (1, "R", "B"),
    "RG8_UNORM": (2, "RG", "BB"),
    "R16_FLOAT": (2, "R", "e"),
    "R16_UINT": (2, "R", "H"),
    "R32_FLOAT": (4, "R", "f"),
    "R32_UINT": (4, "R", "I"),
    "RGBA16_FLOAT": (8, "RGBA", "eeee"),
    "RGB10A2_UNORM": (4, "RGBA", "I"),
}
NRD_FORMATS = {
    0: "R8_UNORM", 4: "RG8_UNORM", 15: "R16_UINT", 17: "R16_FLOAT",
    27: "RGBA16_FLOAT", 28: "R32_UINT", 30: "R32_FLOAT", 40: "RGB10A2_UNORM",
}


def canonical_format(record):
    name = record.get("format")
    if name:
        name = str(name).split("::")[-1].upper().replace("SFLOAT", "FLOAT")
        name = {"R10_G10_B10_A2_UNORM": "RGB10A2_UNORM",
                "R10G10B10A2_UNORM": "RGB10A2_UNORM"}.get(name, name)
    else:
        name = NRD_FORMATS.get(record.get("formatValue"))
    if name not in FORMATS:
        raise ValueError(f"unsupported format: {record.get('format', record.get('formatValue'))!r}")
    return name


def load_capture(directory):
    if not directory.is_dir():
        raise ValueError(f"snapshot directory does not exist: {directory}")
    metadata = directory / "metadata.jsonl"
    if not metadata.is_file():
        raise ValueError(f"missing metadata: {metadata}")
    records, superseded = {}, 0
    for line_number, line in enumerate(metadata.read_text(encoding="utf-8-sig").splitlines(), 1):
        if not line.strip():
            continue
        try:
            record = json.loads(line)
            name = Path(record["path"]).name
            match = TEXTURE.fullmatch(name)
            if not match:
                raise ValueError("path is not a stage texture filename")
            key = tuple(map(int, match.groups()))
            if tuple(record[field] for field in ("frame", "layer", "pass", "slot")) != key:
                raise ValueError("filename and metadata frame/layer/pass/slot disagree")
            if name in records:
                superseded += 1
            records[name] = record
        except (ValueError, KeyError, TypeError) as error:
            raise ValueError(f"{metadata}:{line_number}: {error}") from error
    files = {path.name: path for path in directory.glob("*.bin")
             if TEXTURE.fullmatch(path.name) or CONSTANTS.fullmatch(path.name)}
    return {"directory": str(directory.resolve()), "records": records,
            "files": files, "superseded_metadata_records": superseded}


def filename_key(name):
    match = CONSTANTS.fullmatch(name)
    if match:
        return (*map(int, match.groups()), -1)
    return tuple(map(int, TEXTURE.fullmatch(name).groups()))


def exact_bytes(left, right):
    first = next((index for index, (a, b) in enumerate(zip(left, right)) if a != b), None)
    if first is None and len(left) != len(right):
        first = min(len(left), len(right))
    return {"left_bytes": len(left), "right_bytes": len(right),
            "mismatch_bytes": sum(a != b for a, b in zip(left, right)) + abs(len(left) - len(right)),
            "first_byte_offset": first}


def pixel_values(data, offset, format_name):
    _, _, code = FORMATS[format_name]
    values = struct.unpack_from("<" + code, data, offset)
    if format_name in ("R8_UNORM", "RG8_UNORM"):
        return tuple(value / 255.0 for value in values)
    if format_name == "RGB10A2_UNORM":
        word = values[0]
        return tuple(((word >> shift) & mask) / mask
                     for shift, mask in ((0, 1023), (10, 1023), (20, 1023), (30, 3)))
    return values


def json_number(value):
    if isinstance(value, float) and not math.isfinite(value):
        return "NaN" if math.isnan(value) else ("+Infinity" if value > 0 else "-Infinity")
    return value


def texture_metrics(left, right, shape, format_name, first_byte):
    width, height, row_bytes = shape
    pixel_bytes, components, code = FORMATS[format_name]
    max_abs = max_relative = 0.0
    nonfinite_pairs = 0
    for offset in range(0, len(left), pixel_bytes):
        for a, b in zip(pixel_values(left, offset, format_name), pixel_values(right, offset, format_name)):
            if not math.isfinite(a) or not math.isfinite(b):
                nonfinite_pairs += 1
                continue
            difference = abs(a - b)
            scale = max(abs(a), abs(b))
            max_abs = max(max_abs, difference)
            max_relative = max(max_relative, difference / scale if scale else 0.0)
    first_pixel = None
    if first_byte is not None:
        y, in_row = divmod(first_byte, row_bytes)
        x, in_pixel = divmod(in_row, pixel_bytes)
        offset = y * row_bytes + x * pixel_bytes
        if format_name == "RGB10A2_UNORM":
            changed = struct.unpack_from("<I", left, offset)[0] ^ struct.unpack_from("<I", right, offset)[0]
            component = next(index for index, (shift, mask) in enumerate(((0, 1023), (10, 1023), (20, 1023), (30, 3)))
                             if changed & (mask << shift))
        else:
            component = in_pixel // struct.calcsize("<" + code[0])
        a, b = pixel_values(left, offset, format_name), pixel_values(right, offset, format_name)
        first_pixel = {"x": x, "y": y, "index": y * width + x, "component": component,
                       "channel": components[component], "left": json_number(a[component]),
                       "right": json_number(b[component])}
    return {"max_abs": None if nonfinite_pairs else max_abs,
            "max_relative": None if nonfinite_pairs else max_relative,
            "max_finite_abs": max_abs, "max_finite_relative": max_relative,
            "nonfinite_component_pairs": nonfinite_pairs, "first_pixel": first_pixel}


def texture_shape(record, format_name):
    shape = tuple(record[field] for field in ("width", "height", "rowBytes"))
    if any(type(value) is not int or value <= 0 for value in shape):
        raise ValueError(f"invalid width/height/rowBytes: {shape}")
    width, _, row_bytes = shape
    expected = FORMATS[format_name][0]
    if row_bytes % width or row_bytes // width != expected:
        raise ValueError(f"rowBytes/width does not match {format_name}: {row_bytes}/{width}, expected {expected}")
    return shape


def compare_file(name, left_capture, right_capture):
    frame, layer, stage, slot = filename_key(name)
    result = {"path": name, "frame": frame, "layer": layer, "stage": stage,
              "kind": "constants" if slot < 0 else "texture", "slot": None if slot < 0 else slot}
    if slot < 0 and stage == 0:
        return result | {"status": "skipped", "reason": "Pack constant host layouts differ"}
    missing = [side for side, capture in (("left", left_capture), ("right", right_capture))
               if name not in capture["files"]]
    if missing:
        return result | {"status": "error", "reason": "missing file", "missing_from": missing}
    left, right = left_capture["files"][name].read_bytes(), right_capture["files"][name].read_bytes()
    result.update(exact_bytes(left, right))
    if slot >= 0:
        missing = [side for side, capture in (("left", left_capture), ("right", right_capture))
                   if name not in capture["records"]]
        if missing:
            return result | {"status": "error", "reason": "missing texture metadata", "missing_from": missing}
        a, b = left_capture["records"][name], right_capture["records"][name]
        try:
            left_format, right_format = canonical_format(a), canonical_format(b)
            left_shape, right_shape = texture_shape(a, left_format), texture_shape(b, right_format)
        except (KeyError, TypeError, ValueError) as error:
            return result | {"status": "error", "reason": str(error)}
        result.update({"left_format": left_format, "right_format": right_format,
                       "left_shape": list(left_shape), "right_shape": list(right_shape),
                       "left_shader": a.get("shader"), "right_shader": b.get("shader")})
        if left_format != right_format or left_shape != right_shape:
            return result | {"status": "error", "reason": "texture shape or format mismatch"}
        expected = left_shape[1] * left_shape[2]
        if len(left) != expected or len(right) != expected:
            return result | {"status": "error", "reason": "binary size does not match metadata", "expected_bytes": expected}
        result.update(texture_metrics(left, right, left_shape, left_format, result["first_byte_offset"]))
    result["status"] = "different" if result["mismatch_bytes"] else "identical"
    return result


def compare_snapshots(left_directory, right_directory):
    report = {"left": str(left_directory.resolve()), "right": str(right_directory.resolve()),
              "relative_error_definition": "abs(left-right)/max(abs(left),abs(right)); zero/zero=0; no tolerance",
              "files": [], "errors": []}
    try:
        left, right = load_capture(left_directory), load_capture(right_directory)
    except (OSError, ValueError) as error:
        return report | {"status": "error", "errors": [str(error)], "first_divergence": {"reason": str(error)}}
    report["superseded_metadata_records"] = {"left": left["superseded_metadata_records"], "right": right["superseded_metadata_records"]}
    names = set(left["files"]) | set(right["files"]) | set(left["records"]) | set(right["records"])
    for name in tuple(names):
        frame, layer, stage, slot = filename_key(name)
        if slot >= 0 and stage > 0:
            names.add(f"f{frame}_l{layer}_p{stage}.constants.bin")
    if not names:
        report["errors"].append("no stage snapshot files or metadata records")
    for name in sorted(names, key=filename_key):
        try:
            report["files"].append(compare_file(name, left, right))
        except OSError as error:
            frame, layer, stage, slot = filename_key(name)
            report["files"].append({"path": name, "frame": frame, "layer": layer, "stage": stage,
                                    "kind": "constants" if slot < 0 else "texture", "status": "error", "reason": str(error)})
    failures = [record for record in report["files"] if record["status"] in ("error", "different")]
    report["first_divergence"] = failures[0] if failures else None
    report["summary"] = {status: sum(record["status"] == status for record in report["files"])
                         for status in ("identical", "different", "error", "skipped")}
    report["summary"]["mismatch_bytes"] = sum(record.get("mismatch_bytes", 0) for record in report["files"])
    report["status"] = ("error" if report["errors"] or report["summary"]["error"] else "different" if failures else "identical")
    return report


def print_stage_table(report):
    stages = {}
    for record in report["files"]:
        key = record["frame"], record["layer"], record["stage"]
        stages.setdefault(key, []).append(record)
    print("frame layer stage textures different-bytes max-abs constants")
    for index, (key, records) in enumerate(sorted(stages.items())):
        if index == 40:
            print(f"... {len(stages) - 40} more stages in JSON")
            break
        textures = [record for record in records if record["kind"] == "texture"]
        constants = next((record["status"] for record in records if record["kind"] == "constants"), "n/a")
        errors = any(record["status"] == "error" for record in textures)
        values = [record.get("max_abs") for record in textures]
        maximum = "error" if errors else "n/a" if any(value is None for value in values) else f"{max(values, default=0):.6g}"
        print(*key, len(textures), sum(record.get("mismatch_bytes", 0) for record in textures), maximum, constants)
    print(f"Result: {report['status']}")
    if report["first_divergence"]:
        print("First divergence: " + json.dumps(report["first_divergence"], ensure_ascii=False, allow_nan=False))
    for error in report["errors"]:
        print("Error: " + error)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("left", type=Path, help="native/reference snapshot directory")
    parser.add_argument("right", type=Path, help="EDSL/candidate snapshot directory")
    parser.add_argument("--output", type=Path, required=True, help="JSON comparison report")
    args = parser.parse_args(argv)
    report = compare_snapshots(args.left, args.right)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")
    print_stage_table(report)
    return {"identical": 0, "different": 1, "error": 2}[report["status"]]


if __name__ == "__main__":
    raise SystemExit(main())
