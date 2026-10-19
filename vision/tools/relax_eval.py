"""Render reproducible RELAX/raw/SVGF evidence with vision-eval (requires numpy).

Realtime images use one new sample per frame with framebuffer accumulation off.
Timing is host wall time with synchronization, not GPU timestamps. Device memory
samples include other processes; logical allocation counts are reported separately.
"""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import threading
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


def resolve_assets(value, root, scenario):
    if isinstance(value, dict):
        result = {}
        for key, item in value.items():
            if key in ("fn", "filename") and isinstance(item, str) and item:
                path = Path(os.path.expandvars(item))
                if not path.is_absolute():
                    path = root / path
                if not path.is_file():
                    raise FileNotFoundError(f"{scenario}: missing asset {path}")
                result[key] = str(path.resolve())
            else:
                result[key] = resolve_assets(item, root, scenario)
        return result
    if isinstance(value, list):
        return [resolve_assets(item, root, scenario) for item in value]
    return value


def preflight(manifest):
    for scenario in manifest["scenarios"]:
        if "fixture" in scenario:
            continue
        source = Path(os.path.expandvars(scenario["scene"]))
        if not source.is_file():
            raise FileNotFoundError(f"{scenario['name']}: missing scene {source}")
        data = json.loads(source.read_text(encoding="utf-8-sig"))
        resolve_assets(data["scene"], source.parent, scenario["name"])


def measure(image, reference, rois):
    if not np.isfinite(image).all():
        raise ValueError("Nonfinite rendered pixels")
    result = {}
    height, width = image.shape[:2]
    for name, (x0, y0, x1, y1) in rois.items():
        region = np.s_[int(y0 * height):int(y1 * height), int(x0 * width):int(x1 * width)]
        crop = image[region]
        if not crop.size:
            raise ValueError(f"Empty ROI: {name}")
        stats = {"mean_rgb": crop.mean(axis=(0, 1)).tolist()}
        if reference is not None:
            stats["linear_rmse"] = float(np.sqrt(np.mean((crop - reference[region]) ** 2)))
        result[name] = stats
    return result


def require_same_pose(actual, reference):
    for key in ("position", "pitch", "yaw"):
        if not np.allclose(actual[key], reference[key], atol=1e-5, rtol=0):
            raise ValueError(f"Reference pose mismatch: {key}")


def fixture_scene(kind, root):
    """Small, asset-free OBJ fixture with diffuse/compound/conductor/delta paths."""
    root.mkdir(parents=True, exist_ok=True)
    def sphere(name, center):
        points, normals, faces = [], [], []
        rows, cols = 24, 48
        for i in range(rows + 1):
            theta = math.pi * i / rows
            for j in range(cols):
                phi = 2 * math.pi * j / cols
                n = [math.sin(theta) * math.cos(phi), math.cos(theta), math.sin(theta) * math.sin(phi)]
                normals.append(n); points.append([center[k] + .43 * n[k] for k in range(3)])
        for i in range(rows):
            for j in range(cols):
                a = i * cols + j; d = i * cols + (j + 1) % cols
                b = a + cols; c = d + cols
                if i: faces.append((a, d, b))
                if i + 1 < rows: faces.append((d, c, b))
        path = root / (name + ".obj")
        with path.open("w") as out:
            for p in points: out.write("v " + " ".join(map(str, p)) + "\n")
            for n in normals: out.write("vn " + " ".join(map(str, n)) + "\n")
            for face in faces: out.write("f " + " ".join(f"{i+1}//{i+1}" for i in face) + "\n")
        return str(path.resolve())
    floor = root / "floor.obj"
    floor.write_text("v -5 -.45 2\nv 5 -.45 2\nv 5 -.45 -6\nv -5 -.45 -6\nvn 0 1 0\nf 1//1 2//1 3//1\nf 1//1 3//1 4//1\n")
    wall = root / "wall.obj"
    wall.write_text("v -5 -.45 -3.4\nv 5 -.45 -3.4\nv 5 4 -3.4\nv -5 4 -3.4\nvn 0 0 1\nf 1//1 2//1 3//1\nf 1//1 3//1 4//1\n")
    materials = [
        {"type": "substrate", "name": "compound", "param": {"color": [.7, .15, .05], "spec": [.08, .3, .8], "roughness": .25}},
        {"type": "metal", "name": "copper", "param": {"material_name": "Cu", "roughness": .18}},
        {"type": "mirror", "name": "mirror", "param": {"color": [.95, .95, .95]}},
        {"type": "glass", "name": "glass", "param": {"material_name": "BK7", "roughness": .001, "remapping_roughness": False}},
        {"type": "diffuse", "name": "floor", "param": {"color": [.18, .35, .6]}},
        {"type": "diffuse", "name": "wall", "param": {"color": [.7, .65, .45]}},
    ]
    def model(fn, material):
        return {"type": "model", "param": {"fn": fn, "material": material, "normalize_to_unit_bounds": False}}
    names = ["compound", "copper", "mirror"] if kind == "materials" else ["compound", "glass", "mirror"]
    shapes = [model(str(floor.resolve()), "floor"), model(str(wall.resolve()), "wall")]
    for x, material in zip([-.9, 0, .9], names): shapes.append(model(sphere(material, [x, 0, -1.8]), material))
    return {"scene": {"camera": {"type": "thin_lens", "param": {"fov_y": 42, "lens_radius": 0,
        "filter": {"type": "box"}, "transform": {"type": "look_at", "param": {
        "position": [0, .5, 1.5], "target_pos": [0, 0, -1.8], "up": [0, 1, 0]}}}},
        "materials": materials, "shapes": shapes,
        "lights": [{"type": "spherical", "param": {"color": {"channels": "xyz", "node": {"type": "number", "param": {"value": [1, 1, 1]}}}, "scale": 1}}]},
        "render": {"sampler": {"type": "independent", "param": {"spp": 1}}, "light_sampler": {"type": "uniform"},
        "integrator": {"type": "pt", "param": {"max_depth": 8, "min_depth": 3}}},
        "pipeline": {"type": "fixed", "param": {"frame_buffer": {"type": "normal", "param": {}}}}}


def gpu_info():
    try:
        return subprocess.check_output(["nvidia-smi", "--query-gpu=name,memory.total,driver_version", "--format=csv,noheader"], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unavailable"


def run_process(command, runtime, env, destination, timeout):
    samples, stop = [], threading.Event()
    def monitor():
        while not stop.is_set():
            try:
                value = subprocess.check_output(["nvidia-smi", "--query-gpu=memory.used", "--format=csv,noheader,nounits"], text=True)
                samples.append({"seconds": time.monotonic() - started, "device_used_mib": int(value.splitlines()[0])})
            except (OSError, ValueError, subprocess.CalledProcessError):
                pass
            stop.wait(.5)
    started = time.monotonic()
    watcher = threading.Thread(target=monitor, daemon=True); watcher.start()
    try:
        with (destination / "run.log").open("w", encoding="utf-8") as log:
            process = subprocess.run(command, cwd=runtime, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
        status = "passed" if process.returncode == 0 else "failed"
        code = process.returncode
    except subprocess.TimeoutExpired:
        status, code = "timeout", None
    finally:
        stop.set(); watcher.join()
    (destination / "memory.json").write_text(json.dumps(samples, indent=2))
    return {"status": status, "exit_code": code, "wall_seconds_including_compile": time.monotonic() - started,
            "device_wide_peak_mib": max((s["device_used_mib"] for s in samples), default=None), "command": command}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--scenario", action="append")
    parser.add_argument("--kind", choices=["all", "quality", "performance"], default="all")
    parser.add_argument("--resume", action="store_true", help="Reuse successful jobs from this evidence directory")
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    manifest["scenarios"] = [s for s in manifest["scenarios"] if not args.scenario or s["name"] in args.scenario]
    if not manifest["scenarios"]: raise ValueError("No scenarios selected")
    preflight(manifest)
    evidence = args.output_dir.resolve(); evidence.mkdir(parents=True, exist_ok=True)
    runtime = (args.build_dir / "bin").resolve()
    executable = runtime / ("vision-eval.exe" if os.name == "nt" else "vision-eval")
    if not executable.is_file(): raise FileNotFoundError(executable)
    env = dict(os.environ, VISION_EVAL_LINEAR_OUTPUT="1")
    env.pop("VISION_DISABLE_DENOISER", None); env.pop("VISION_RELAX_PROFILE", None)
    report_file = evidence / "report.json"
    digest = hashlib.sha256()
    for binary in sorted(runtime.glob("vision-*.dll")) + sorted(runtime.glob("ocarina-*.dll")) + [executable]:
        digest.update(binary.name.encode()); digest.update(binary.read_bytes())
    renderer_hash = digest.hexdigest()
    report = json.loads(report_file.read_text()) if args.resume and report_file.exists() else {"jobs": {}}
    manifest_hash = hashlib.sha256(args.manifest.read_bytes()).hexdigest()
    if args.resume and report.get("jobs") and (report.get("renderer_sha256") != renderer_hash or report.get("manifest_sha256") != manifest_hash):
        raise ValueError("Cannot resume evidence from different renderer binaries or manifest")
    report.update({"manifest": str(args.manifest.resolve()), "gpu": gpu_info(), "seed": "IndependentSampler TEA(pixel, frame, dimension); PathTracing and guide dimension 1024",
        "renderer_sha256": renderer_hash, "manifest_sha256": manifest_hash,
        "revision": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
        "uncommitted_diff_sha256": hashlib.sha256(subprocess.check_output(["git", "diff", "--binary", "HEAD"])).hexdigest(),
        "timing": "Synchronized host wall time; stage times include submission/synchronization, not GPU timestamps. PrepareGuides includes material evaluation and missing-lobe rays.",
        "reference_uncertainty": "Two 2048 spp PT estimates, seed offsets 0 and 104729; report their difference and compare against their average."})
    def save(): report_file.write_text(json.dumps(report, indent=2), encoding="utf-8")

    def job(key, base, scenario, method, frames, trajectory=None, pose=None, performance=False, seed=0):
        destination = evidence / key
        destination.mkdir(parents=True, exist_ok=True)
        if args.resume and key in report["jobs"] and report["jobs"][key]["status"] == "passed":
            return destination
        scene = copy.deepcopy(base)
        scene["render"]["sampler"] = {"type": "independent", "param": {"spp": 1, "seed": seed}}
        params = scene["render"]["integrator"].setdefault("param", {})
        scene["render"]["integrator"]["type"] = "pt" if method == "reference" else scenario.get("integrator", "pt")
        for channel in ("direct", "indirect"):
            block = params.setdefault(channel, {}); block.setdefault("spatial", {}); block.setdefault("temporal", {})
        params["direct"]["stable_planes"] = scenario.get("stable_planes", False)
        params["denoiser"] = {"type": "ReLAX" if method == "ReLAX" else "svgf"}
        scene["pipeline"]["param"]["frame_buffer"] = {"type": "normal", "param": {"resolution": scenario["resolution"], "accumulation": method == "reference"}}
        scene["output"] = {"spp": 1, "fn": "image.png", "denoise": method in ("svgf", "ReLAX"), "save_exit": False}
        scene_path = destination / "scene.json"; scene_path.write_text(json.dumps(scene, indent=2))
        command = [str(executable), "--runtime-dir", str(runtime), "-s", str(scene_path), "--headless", "--save-spp", str(frames),
            "--warmup-frames", "8", "--profile-frames", "16", "--output-dir", str(destination), "--metrics-file", str(destination / "metrics.json")]
        if method != "reference": command += ["--no-accumulation"]
        if method in ("svgf", "ReLAX"): command += ["--denoise"]
        if performance: command += ["--stage-profile"]
        if trajectory:
            trajectory_path = destination / "trajectory.json"; trajectory_path.write_text(json.dumps(trajectory, indent=2))
            command += ["--camera-poses", str(trajectory_path)]
        if pose:
            command += ["--camera-override", ",".join(map(str, pose["position"] + [pose["pitch"], pose["yaw"]]))]
        local_env = dict(env)
        if performance and method == "ReLAX":
            profile = destination / "relax-profile.jsonl"; profile.write_text("")
            local_env["VISION_RELAX_PROFILE"] = str(profile)
        record = run_process(command, runtime, local_env, destination, scenario.get("timeout", 600 if not performance else 180))
        if record["status"] == "passed":
            metadata = json.loads((destination / "image.pose.json").read_text())
            if metadata["accumulation"] != (method == "reference") or metadata["denoise"] != (method in ("svgf", "ReLAX")):
                raise ValueError(f"Actual render configuration differs from evaluation contract: {key}")
            if pose: require_same_pose(pose, metadata)
            record["pose"] = metadata
            record["metrics"] = json.loads((destination / "metrics.json").read_text())
            if performance and method == "ReLAX":
                lines = [json.loads(line) for line in (destination / "relax-profile.jsonl").read_text().splitlines()]
                times = {}
                for line in lines:
                    if "stage" in line and 8 <= line["frame"] < 24:
                        times.setdefault(line["stage"], {}).setdefault(line["frame"], 0)
                        times[line["stage"]][line["frame"]] += line["ms"]
                record["per_stage_ms_sum_over_layers"] = {name: float(np.mean(list(values.values()))) for name, values in times.items()}
                record["compile_cpu_ms"] = sum(line["ms"] for line in lines if line.get("stage") == "CompileCPU")
                record["resources"] = next((line for line in reversed(lines) if line.get("resources")), None)
        report["jobs"][key] = record; save()
        print(key, record["status"], f"{record['wall_seconds_including_compile']:.2f}s", flush=True)
        return destination if record["status"] == "passed" else None

    for scenario in manifest["scenarios"]:
        name = scenario["name"]
        if "fixture" in scenario:
            base = fixture_scene(scenario["fixture"], evidence / "fixtures" / scenario["fixture"])
        else:
            source = Path(os.path.expandvars(scenario["scene"])).resolve()
            base = json.loads(source.read_text(encoding="utf-8-sig"))
            base["scene"] = resolve_assets(base["scene"], source.parent, name)
        if "camera" in scenario: base["scene"]["camera"] = scenario["camera"]
        if args.kind != "performance":
            rois = scenario.get("rois", {"full": [0, 0, 1, 1]})
            trajectory = scenario.get("trajectory")
            if not trajectory:
                trajectory = {"keyframes": [{"frame": 0, "delta_pos": [0, 0, 0]}], "save_frames": list(range(48, 64))}
            outputs = {}
            for method in ("raw", "svgf", "ReLAX"):
                path = job(f"{name}/{method}", base, scenario, method, scenario.get("frames", 64), trajectory)
                if path: outputs[method] = path
            if len(outputs) != 3: continue
            reference_frames = scenario.get("reference_frames", [63] if "trajectory" not in scenario else [31, 47, 63, 95])
            for frame in reference_frames:
                pose = json.loads((outputs["raw"] / f"image_frame_{frame:04d}.pose.json").read_text())
                reference_dir = job(f"{name}/reference-{frame:04d}", base, scenario, "reference", scenario.get("reference_spp", 2048), pose=pose)
                if reference_dir is None: continue
                reference = read_pfm(reference_dir / "image.pfm")
                independent_dir = job(f"{name}/reference-independent-{frame:04d}", base, scenario, "reference",
                    scenario.get("reference_spp", 2048), pose=pose, seed=104729)
                if independent_dir is None: continue
                independent = read_pfm(independent_dir / "image.pfm")
                report["jobs"][f"{name}/reference-{frame:04d}"]["independent_difference"] = measure(reference, independent, rois)
                reference = .5 * (reference + independent)
                for method, path in outputs.items():
                    key = f"{name}/{method}"
                    actual_pose = json.loads((path / f"image_frame_{frame:04d}.pose.json").read_text())
                    require_same_pose(actual_pose, pose)
                    report["jobs"][key].setdefault("frames", {})[str(frame)] = measure(read_pfm(path / f"image_frame_{frame:04d}.pfm"), reference, rois)
            for method, path in outputs.items():
                # Temporal statistics only use a stationary tail (manifest specifies its start).
                tail_start = scenario.get("stationary_tail_start", 48)
                tail = [read_pfm(path / f"image_frame_{f:04d}.pfm") for f in trajectory["save_frames"] if f >= tail_start]
                if len(tail) > 1:
                    deviation = np.std(np.array(tail), axis=0)
                    report["jobs"][f"{name}/{method}"]["stationary_tail_std"] = measure(deviation, None, rois)
            save()
        if args.kind != "quality" and scenario.get("benchmark", False):
            for resolution in ([1280, 720], [1920, 1080]):
                for producer in ("pt", "rt"):
                    benchmark = dict(scenario, resolution=resolution, integrator=producer, stable_planes=producer == "rt")
                    for repeat in range(3):
                        key = f"performance/{name}-{producer}-{resolution[0]}x{resolution[1]}-{repeat}"
                        result = job(key, base, benchmark, "ReLAX", 24, performance=True)
                        if result is None:
                            for remaining in range(repeat + 1, 3):
                                pending = f"performance/{name}-{producer}-{resolution[0]}x{resolution[1]}-{remaining}"
                                report["jobs"][pending] = {"status": "not_run", "reason": "Earlier repeat failed or timed out; inspect its log before repeating this capacity attempt"}
                            save(); break
    report["quality_acceptance"] = {}
    for scenario in manifest["scenarios"]:
        name = scenario["name"]
        raw = report["jobs"].get(name + "/raw", {}).get("stationary_tail_std")
        relaxed = report["jobs"].get(name + "/ReLAX", {}).get("stationary_tail_std")
        if raw and relaxed:
            before = float(np.mean(raw["full"]["mean_rgb"]))
            after = float(np.mean(relaxed["full"]["mean_rgb"]))
            report["quality_acceptance"][name] = {"raw_mean_rgb_temporal_sigma": before,
                "relax_mean_rgb_temporal_sigma": after, "noise_below_raw": after < before}
    report["all_jobs_passed"] = all(job["status"] == "passed" for job in report["jobs"].values())
    save()
    quality_ok = all(value["noise_below_raw"] for value in report["quality_acceptance"].values())
    return 0 if report["all_jobs_passed"] and quality_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
