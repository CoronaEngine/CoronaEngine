"""Repeatable linear-light REBLUR/SVGF/raw comparisons using vision-eval.

Requires numpy. Scene paths may use environment variables; relative assets are
resolved against the source scene. No framebuffer accumulation in realtime runs.
Reference convergence and moving-image quality still require human inspection.
"""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import time

import numpy as np


def read_pfm(path):
    with path.open("rb") as stream:
        if stream.readline().strip() != b"PF":
            raise ValueError(f"Expected RGB PFM: {path}")
        width, height = map(int, stream.readline().split())
        scale = float(stream.readline())
        data = np.fromfile(stream, dtype="<f4" if scale < 0 else ">f4")
    return np.flipud(data.reshape(height, width, 3)).astype(np.float64) * abs(scale)


def resolve_assets(value, root):
    if isinstance(value, dict):
        return {k: resolve_assets(v, root) for k, v in value.items()}
    if isinstance(value, list):
        return [resolve_assets(v, root) for v in value]
    if isinstance(value, str) and value and (root / value).is_file():
        return str((root / value).resolve())
    return value


def measure(image, reference, rois):
    if not np.isfinite(image).all():
        raise ValueError("Nonfinite rendered pixels")
    result = {}
    height, width = image.shape[:2]
    for name, (x0, y0, x1, y1) in rois.items():
        region = np.s_[int(y0 * height):int(y1 * height), int(x0 * width):int(x1 * width)]
        crop = image[region]
        stats = {"mean_rgb": crop.mean(axis=(0, 1)).tolist()}
        if reference is not None:
            stats["linear_rmse"] = float(np.sqrt(np.mean((crop - reference[region]) ** 2)))
        result[name] = stats
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--scenario", action="append", help="Run only named scenarios")
    args = parser.parse_args()
    runtime = (args.build_dir / "bin").resolve()
    executable = runtime / ("vision-eval.exe" if os.name == "nt" else "vision-eval")
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    evidence = args.output_dir.resolve()
    env = dict(os.environ, VISION_EVAL_LINEAR_OUTPUT="1")
    env.pop("VISION_DISABLE_DENOISER", None)
    report = {"manifest": str(args.manifest.resolve()), "scenarios": {}}
    for scenario in manifest["scenarios"]:
        name = scenario["name"]
        if args.scenario and name not in args.scenario:
            continue
        source = Path(os.path.expandvars(scenario["scene"])).resolve()
        base = resolve_assets(json.loads(source.read_text(encoding="utf-8-sig")), source.parent)
        base["pipeline"]["param"]["frame_buffer"]["param"]["resolution"] = scenario["resolution"]
        if "camera" in scenario:
            base["scene"]["camera"] = scenario["camera"]
        base["render"]["sampler"]["param"]["spp"] = 1
        root = evidence / name
        root.mkdir(parents=True, exist_ok=True)
        static = "trajectory" not in scenario
        methods = ["reference", "raw", "svgf", "ReBLUR"] if static else ["raw", "svgf", "ReBLUR"]
        records, reference = {}, None
        for method in methods:
            scene = copy.deepcopy(base)
            parameters = scene["render"]["integrator"].setdefault("param", {})
            scene["render"]["integrator"]["type"] = "pt" if method == "reference" else scenario.get("integrator", "pt")
            parameters.setdefault("direct", {})["stable_planes"] = scenario.get("stable_planes", False)
            for channel in ("direct", "indirect"):
                channel_parameters = parameters.setdefault(channel, {})
                channel_parameters.setdefault("temporal", {})
                channel_parameters.setdefault("spatial", {})
            parameters["denoiser"] = {"type": "ReBLUR" if method == "ReBLUR" else "svgf"}
            scene["output"] = {"spp": 1, "fn": method + ".png", "denoise": method in ("svgf", "ReBLUR")}
            scene["pipeline"]["param"]["frame_buffer"]["param"]["accumulation"] = method == "reference"
            destination = root / method
            destination.mkdir(exist_ok=True)
            scene_path = destination / "scene.json"
            scene_path.write_text(json.dumps(scene, indent=2), encoding="utf-8")
            frames = scenario.get("reference_spp", 2048) if method == "reference" else scenario.get("frames", 64)
            command = [str(executable), "--runtime-dir", str(runtime), "-s", str(scene_path), "--headless",
                       "--save-spp", str(frames), "--warmup-frames", "8", "--profile-frames", "16",
                       "--output-dir", str(destination), "--metrics-file", str(destination / "metrics.json")]
            if method != "reference":
                command += ["--no-accumulation", "--stage-profile"]
            if method in ("svgf", "ReBLUR"):
                command += ["--denoise"]
            if not static:
                trajectory = destination / "trajectory.json"
                trajectory.write_text(json.dumps(scenario["trajectory"], indent=2), encoding="utf-8")
                command += ["--camera-poses", str(trajectory)]
            started = time.perf_counter()
            with (destination / "run.log").open("w", encoding="utf-8") as log:
                subprocess.run(command, cwd=runtime, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
            image = read_pfm(destination / (method + ".pfm"))
            pose = json.loads((destination / (method + ".pose.json")).read_text())
            if pose["accumulation"] != (method == "reference"):
                raise ValueError("Framebuffer accumulation does not match the evaluation contract")
            if method == "reference":
                reference = image
            rois = scenario.get("rois", {"full": [0, 0, 1, 1]})
            record = {"wall_seconds_including_compile": time.perf_counter() - started,
                      "command": command, "metrics": json.loads((destination / "metrics.json").read_text()),
                      "regions": measure(image, reference, rois)}
            if not static:
                record["frames"] = {str(frame): measure(read_pfm(destination / f"{method}_frame_{frame:04d}.pfm"), None, rois)
                                    for frame in scenario["trajectory"]["save_frames"]}
            records[method] = record
            print(name, method, record["regions"], flush=True)
        report["scenarios"][name] = records
        (evidence / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
