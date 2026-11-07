"""Task-local Corona path-tracing capture. --config contract.json [--audit-only].
No GPU work occurs until main() run without --audit-only. The parent owns execution.
Checkpoints are ZERO-BASED integrator sample indexes; frames.csv is ONE-BASED.
Hook raw_total.f32 is untouched FP32 path radiance before denoising/combine,
not an inverse-transformed PNG. Native GPU ticks are reported separately from host times.
"""
from __future__ import annotations
import argparse
import csv
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import re
import statistics
import struct
import subprocess
import sys
import time

sys.dont_write_bytecode = True
REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools"))
from render_mitsuba_scenes import (Session, ensure_port_available, engine_environment,
    fixed_viewport_script, confirm_thermal_protection, save_json, api_data)
# Adapted from sibling DI harness SHA256 3bc61f739ea46edb42eca8eec06128cfd973a67de8f9b9fa5c9e8a5f593b1b79
WIDTH, HEIGHT = 1920, 1080
DEFAULT_SETTINGS = {"max_depth": 16, "min_depth": 5, "rr_threshold": 1}

class CaptureSession(Session):
    def api(self, name, args, timeout=30):
        if name != "ProjectLauncher.open_project":
            return super().api(name, args, timeout)
        # Dispatcher registration precedes lazy ArchiveService initialization.
        # Only these documented startup states are retryable; preserve failures.
        request = json.dumps({"api": name, "args": args})
        expression = "new Promise((resolve,reject)=>window.cefQuery({request:" + json.dumps(request) + ",persistent:false,onSuccess:r=>resolve(JSON.parse(r)),onFailure:(c,m)=>reject(new Error(c+':'+m))}))"
        response = self.evaluate(expression, timeout)
        with self.log.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps({"api": name, "args": args, "response": response}, ensure_ascii=False) + "\n")
        data = response.get("data", response)
        if response.get("success") and isinstance(data, dict) and data.get("status") in ("initializing", "service_initializing"):
            return data
        return api_data(name, response)

def open_project_ready(session, project, timeout=180):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if session.process.poll() is not None:
            raise RuntimeError("Engine exited while archive service initialized")
        value = session.api("ProjectLauncher.open_project", [project, {"load_policy": "degraded"}])
        if value.get("status") in ("initializing", "service_initializing"):
            continue
        if value.get("ok") is not True or not isinstance(value.get("scene"), dict):
            raise RuntimeError("Project open did not produce a scene: " + json.dumps(value))
        return value
    raise TimeoutError("Project archive service did not finish initialization")

def sha(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()

def fingerprint(executable):
    source = [REPO / "src/systems/optics/optics_system.cpp",
              REPO / "src/systems/ui/cef/cef_editor_native_api_handlers.cpp",
              REPO / "vision/src/base/integral/integrator.cpp",
              REPO / "vision/src/base/integral/integrator.h",
              REPO / "vision/src/base/mgr/evaluation_debug.h",
              REPO / "Horizon/src/backends/vulkan_compute/vk_stream.cpp"]
    for folder in ("vision/src/render_core/integrator", "vision/src/render_core/material",
                   "vision/src/render_core/sampler", "vision/src/render_core/light",
                   "vision/src/render_core/lightsampler", "vision/src/base",
                   "src/systems/optics/vision"):
        source.extend(p for p in (REPO / folder).rglob("*") if p.suffix in (".cpp", ".h"))
    runtime = [executable] + sorted(executable.parent.glob("*.dll"))
    def record(paths):
        return {str(p): {"sha256": sha(p), "bytes": p.stat().st_size,
                        "mtime_ns": p.stat().st_mtime_ns} for p in sorted(set(paths)) if p.is_file()}
    s, r = record(source), record(runtime)
    mapping = {
        "vision-integrator-rt.dll": ["vision/src/render_core/integrator/rt.cpp"],
        "vision-integrator-pt.dll": ["vision/src/render_core/integrator/pt.cpp",
                                     "vision/src/base/integral/integrator.cpp",
                                     "vision/src/base/integral/integrator.h"],
        "corona_engine.exe": ["src/systems/optics/optics_system.cpp",
                             "src/systems/ui/cef/cef_editor_native_api_handlers.cpp"]}
    stale = []
    for binary, inputs in mapping.items():
        target = executable.parent / binary
        for name in inputs:
            path = REPO / name
            if target.exists() and path.stat().st_mtime_ns > target.stat().st_mtime_ns:
                stale.append({"binary": str(target), "newer_source": str(path)})
    return {"source": s, "runtime": r, "newer_source_warnings": stale,
            "scope": "Hashes prove unchanged inputs/runtime during capture. Timestamps are not compilation provenance."}

def identities_equal(a, b):
    return all({p: v["sha256"] for p, v in a[k].items()} ==
               {p: v["sha256"] for p, v in b[k].items()} for k in ("source", "runtime"))

def load_config(path):
    c = json.loads(path.read_text(encoding="utf-8-sig"))
    if "scene_contract" in c:
        contract = json.loads(Path(c["scene_contract"]).read_text(encoding="utf-8-sig"))
        if contract.get("renderer_family") != "path_tracing":
            raise ValueError("Expected the PT scene contract, not a DI contract")
        if sha(contract["corona_scene"]) != contract["scene_sha256"]:
            raise ValueError("PT scene no longer matches the shared contract hash")
        c["scene_contract_sha256"] = sha(c["scene_contract"])
        c.setdefault("scene_path", contract["corona_scene"])
        if contract.get("camera", {}).get("corona_api"):
            c.setdefault("camera_pose", contract["camera"]["corona_api"])
    c["executable"] = str(Path(c.get("executable", REPO / "cmake-build-relwithdebinfo/examples/engine/corona_engine.exe")).resolve())
    c["output"] = str(Path(c["output"]).resolve())
    c["scene_path"] = str(Path(c["scene_path"]).resolve())
    c.setdefault("camera_id", None)
    c.setdefault("port", 9247)
    c.setdefault("timeout_seconds", 7200)
    c.setdefault("checkpoints", [0, 1, 7, 31, 63])
    c.setdefault("reference_frames", 0)
    c.setdefault("reference_start_index", 0)
    c.setdefault("quality_mean_frames", 0)
    c.setdefault("keep_reference_raw", False)
    if c["reference_frames"]:
        if type(c["reference_frames"]) is not int or c["reference_frames"] < 2 or c["reference_frames"] % 2:
            raise ValueError("reference_frames must be a positive even count >=2")
        if type(c["reference_start_index"]) is not int or c["reference_start_index"] < 0:
            raise ValueError("reference_start_index must be a nonnegative integer")
        c["render_frames"] = c["reference_start_index"] + c["reference_frames"]
        c["checkpoints"] = sorted(set(v for v in c["checkpoints"] if c["reference_start_index"] <= v < c["render_frames"]))
    c.setdefault("render_frames", max(c["checkpoints"]) + 1)
    c.setdefault("warmup_frames", 0)
    c.setdefault("primary_jitter", False)
    c.setdefault("seed", 0)
    c.setdefault("capture_raw", True)
    c.setdefault("settings", copy.deepcopy(DEFAULT_SETTINGS))
    if c["reference_frames"]:
        c["capture_raw"] = True
    if c["quality_mean_frames"]:
        if c["quality_mean_frames"] != c["render_frames"] or c["warmup_frames"] != 0 or c["reference_frames"]:
            raise ValueError("Quality mean must use every frame of a fresh non-reference run")
        if c["checkpoints"] != list(range(c["quality_mean_frames"])) or not c["capture_raw"]:
            raise ValueError("Quality mean requires all per-frame linear raw images")
    if not c["checkpoints"] or any(type(v) is not int or v < 0 or v >= c["render_frames"] for v in c["checkpoints"]):
        raise ValueError("checkpoints must be nonempty zero-based indexes within render_frames")
    if not (0 <= c["warmup_frames"] < c["render_frames"] <= 1000000):
        raise ValueError("Invalid frame counts")
    if c["seed"] != 0:
        raise ValueError("SceneTools has no sampler seed setter. Use seed=0 or author/verify native source sampler explicitly.")
    settings = c["settings"]
    if not subset_equal(settings, DEFAULT_SETTINGS) or set(settings) != set(DEFAULT_SETTINGS):
        raise ValueError("This PT task requires max_depth=16, min_depth=5, rr_threshold=1")
    if c["primary_jitter"]:
        raise ValueError("The matched comparison and reference require fixed center primary rays")
    return c

def subset_equal(actual, expected):
    if isinstance(expected, dict):
        return isinstance(actual, dict) and all(k in actual and subset_equal(actual[k], v) for k, v in expected.items())
    if isinstance(expected, list):
        return isinstance(actual, list) and len(actual) == len(expected) and all(subset_equal(a, b) for a, b in zip(actual, expected))
    if isinstance(expected, float):
        return math.isclose(actual, expected, rel_tol=1e-6, abs_tol=1e-7)
    return actual == expected

def raw_to_pfm(source, target, width=WIDTH, height=HEIGHT):
    import numpy as np
    values = np.fromfile(source, dtype="<f4")
    if values.size != width * height * 4:
        raise ValueError(f"Wrong float4 payload size: {source}")
    rgb = values.reshape(height, width, 4)[..., :3]
    if not np.isfinite(rgb).all():
        raise ValueError(f"Nonfinite raw image: {source}")
    with target.open("wb") as out:
        out.write(f"PF\n{width} {height}\n-1.0\n".encode("ascii"))
        out.write(np.ascontiguousarray(rgb[::-1], dtype="<f4").tobytes())
    return {"sha256": sha(target), "mean_rgb": rgb.mean(axis=(0, 1), dtype=np.float64).tolist(),
            "max_abs": float(np.abs(rgb).max()), "width": width, "height": height,
            "producer": str(source), "producer_sha256": sha(source)}

def validate_raw_window(output, indexes, started_ns, finished_ns, width=WIDTH, height=HEIGHT):
    entries = []
    for index in indexes:
        for channel in ("total",):
            source = output / "raw" / f"frame_{index:04d}" / ("raw_" + channel + ".f32")
            info = source.stat()
            if info.st_size != width * height * 16:
                raise ValueError(f"Wrong offline float4 payload size: {source}: {info.st_size}")
            if not started_ns <= info.st_mtime_ns <= finished_ns:
                raise ValueError(f"Raw file was written outside the official capture: {source}")
            entries.append({"sample_index": index, "channel": channel,
                            "bytes": info.st_size, "mtime_ns": info.st_mtime_ns})
    return entries

def raw_indexes(c):
    start = c["reference_start_index"]
    return range(start, start + c["reference_frames"]) if c["reference_frames"] else c["checkpoints"]


def reference_mean(output, c, prefix="reference"):
    import numpy as np
    count = c["reference_frames"]
    start = c.get("reference_start_index", 0)
    sums = [np.zeros((HEIGHT, WIDTH, 3), dtype=np.float64) for _ in range(2)]
    manifest = []
    raw_root = (output / "raw").resolve()
    for offset in range(count):
        index = start + offset
        source = raw_root / f"frame_{index:04d}" / "raw_total.f32"
        values = np.fromfile(source, dtype="<f4")
        if values.size != WIDTH * HEIGHT * 4 or not np.isfinite(values).all():
            raise ValueError(f"Invalid reference raw: {source}")
        rgb = values.reshape(HEIGHT, WIDTH, 4)[..., :3]
        sums[int(offset >= count // 2)] += rgb
        item = {"sample_index": index, "sha256": sha(source), "bytes": source.stat().st_size}
        manifest.append(item)
        if not c["keep_reference_raw"] and index not in c["checkpoints"]:
            # Only exact known outputs in this run's validated raw directory.
            for name in ("raw_total.f32",):
                path = source.parent / name
                if not path.resolve().is_relative_to(raw_root):
                    raise ValueError("Raw cleanup escaped the current run")
                if path.is_file(): path.unlink()
            if not any(source.parent.iterdir()): source.parent.rmdir()
            item["raw_deleted_after_mean"] = True
    halves = [value / (count // 2) for value in sums]
    mean = (halves[0] + halves[1]) * 0.5
    result = {"method": "Mean of independent per-frame 1-spp PT FP32 totals, matched task settings recorded in contract.json. No image accumulation or primary jitter. Uses the same finite-depth estimator as quality/performance; not an unbiased infinite-bounce truth.",
              "frames": count, "halves": count // 2, "sampler_seed": 0,
              "first_sample_index": start, "last_sample_index": start + count - 1,
              "discarded_prefix_frames": start, "images": {}, "samples": manifest}
    for name, rgb in ((prefix, mean), (prefix + "_first_half", halves[0]), (prefix + "_second_half", halves[1])):
        target = output / (name + ".pfm")
        with target.open("wb") as stream:
            stream.write(f"PF\n{WIDTH} {HEIGHT}\n-1.0\n".encode("ascii"))
            stream.write(np.ascontiguousarray(rgb[::-1], dtype="<f4").tobytes())
        result["images"][name] = {"path": str(target), "sha256": sha(target), "mean_rgb": rgb.mean(axis=(0, 1)).tolist()}
    delta = halves[0] - halves[1]
    rms = float(np.sqrt(np.mean(mean * mean)))
    result["half_difference"] = {"rmse": float(np.sqrt(np.mean(delta * delta))),
        "relative_rmse": float(np.sqrt(np.mean(delta * delta))) / max(rms, 1e-30),
        "mean_rgb_difference": delta.mean(axis=(0, 1)).tolist(),
        "interpretation": "Two finite independent halves estimate reference sampling variability; not a bias or convergence guarantee."}
    save_json(output / (prefix + "-manifest.json"), result)
    return {k: v for k, v in result.items() if k != "samples"}

def gpu_phase(name):
    low = name.lower()
    if low == "path_tracing": return "path_tracing"
    if "gbuffer" in low or low == "rt_geom": return "gbuffer"
    if "combine" in low: return "combine"
    if "final" in low or "tonemap" in low or "gamma" in low: return "resolve_postprocess"
    return "other"


def summarize_gpu(output, warmup):
    with (output / "stages.csv").open(encoding="utf-8-sig", newline="") as f:
        frames = list(csv.DictReader(f))
    with (output / "gpu-diagnostics.csv").open(encoding="utf-8-sig", newline="") as f:
        reader = csv.DictReader(f)
        native_rows = list(reader)
        # Each newly created compute stream may append its own identical header.
        # Skip only exact header rows; malformed numeric data still fails below.
        ticks = [row for row in native_rows if not all(row.get(key) == key for key in reader.fieldnames)]
        repeated_headers = len(native_rows) - len(ticks)
    result = []
    for frame in frames:
        start, end = int(frame["frame_qpc_begin"]), int(frame["frame_qpc_end"])
        if not 0 < start < end:
            raise ValueError("Missing native QPC frame boundaries")
        phase, dispatches = {}, []
        for tick in ticks:
            if start <= int(tick["submit_begin_qpc"]) and int(tick["submit_end_qpc"]) <= end:
                if int(tick["qpc_frequency"]) != int(frame["qpc_frequency"]):
                    raise ValueError("QPC frequency mismatch")
                elapsed = ((int(tick["end_tick"]) - int(tick["begin_tick"])) & int(tick["timestamp_mask"])) * float(tick["timestamp_period_ns"]) / 1e6
                key = gpu_phase(tick["name"])
                phase[key] = phase.get(key, 0.0) + elapsed
                dispatches.append({"name": tick["name"], "gpu_ms": elapsed,
                                   "profile_batch": int(tick["profile_batch"]),
                                   "submit_serial": int(tick["submit_serial"])})
        if not all(phase.get(k, 0) > 0 for k in ("path_tracing", "gbuffer")):
            raise ValueError(f"Missing PT/GBuffer native GPU ranges for frame {frame['frame']}")
        if sum(d["name"] == "path_tracing" for d in dispatches) != 1:
            raise ValueError("Expected exactly one 1-spp PT dispatch per frame")
        if any("restir" in d["name"].lower() for d in dispatches):
            raise ValueError("Unexpected ReSTIR kernel in the PT capture")
        if any(any(tag in d["name"].lower() for tag in ("nrd", "reblur", "relax", "svgf", "ssat")) for d in dispatches):
            raise ValueError("Unexpected denoising kernel in the PT capture")
        result.append({"frame": int(frame["frame"]), "sample_index": int(frame["integrator_frame"]) - 1,
                       "phase_gpu_ms": phase, "gpu_dispatch_sum_ms": sum(phase.values()), "dispatches": dispatches})
    measured = result[warmup:]
    phases = sorted({k for frame in measured for k in frame["phase_gpu_ms"]})
    return {"frames": result, "warmup_frames": warmup, "measured_frames": len(measured),
            "mean_phase_gpu_ms": {k: statistics.mean(f["phase_gpu_ms"].get(k, 0) for f in measured) for k in phases},
            "mean_gpu_dispatch_sum_ms": statistics.mean(f["gpu_dispatch_sum_ms"] for f in measured),
            "repeated_native_csv_headers_skipped": repeated_headers,
            "timing_scope": "Native compute dispatch timestamp sum. Excludes CPU gaps/readbacks/presentation.",
            "stages_csv_scope": "Host/Wall stage fields may be zero; only QPC intervals bind GPU submissions to fixed offline frames."}

def run(c):
    from vision_frontend_probe import Frontend
    output, executable = Path(c["output"]), Path(c["executable"])
    if output.exists() and any(output.iterdir()):
        raise FileExistsError(f"Preserving nonempty output: {output}")
    output.mkdir(parents=True, exist_ok=True)
    ensure_port_available(c["port"])
    before = fingerprint(executable)
    save_json(output / "identity-before.json", before)
    if before["newer_source_warnings"]:
        raise RuntimeError("Runtime predates relevant source; parent must rebuild and verify before capture.")
    save_json(output / "contract.json", c)
    save_json(output / "thermal-protection.json", confirm_thermal_protection())
    env = engine_environment(os.environ, output / "cef-cache", c["port"])
    # These only observe timestamps/choose primary lattice; no artificial pacing.
    env.update(CORONA_VISION_CAPTURE_PROFILE="1", VISION_DISABLE_DENOISER="1",
               VISION_PT_PRIMARY_JITTER=str(int(c["primary_jitter"])),
               REBLUR_GPU_PROFILE=str(output / "gpu.csv"),
               REBLUR_GPU_PROFILE_MODE="dispatch",
               REBLUR_GPU_PROFILE_DIAGNOSTICS=str(output / "gpu-diagnostics.csv"))
    for key in ("VISION_EVAL_DEBUG_DIR", "VISION_EVAL_DEBUG_FRAME", "VISION_EVAL_DEBUG_FRAMES", "VISION_STAGE_PROFILE"):
        env.pop(key, None)
    if c["capture_raw"]:
        selected_frames = raw_indexes(c)
        env.update(VISION_EVAL_DEBUG_DIR=str(output / "raw"),
                   VISION_EVAL_DEBUG_FRAMES=",".join(map(str, selected_frames)))
    ctest = executable.parents[2] / "tests/integration/CTestTestfile.cmake"
    paths = re.findall(r'PATH=path_list_prepend:([^;"\n]+)', ctest.read_text(encoding="utf-8")) if ctest.is_file() else []
    env["PATH"] = os.pathsep.join(dict.fromkeys([str(executable.parent), *paths, env.get("PATH", "")]))
    ini = executable.parent / "CoronaEditor.ini"
    ini_before = ini.read_bytes() if ini.exists() else None
    process = frontend = session = None
    record = {"status": "running", "source_scene_sha256": sha(c["scene_path"]),
              "script_sha256": sha(Path(__file__)), "raw_images": {}}
    try:
        with (output / "engine.log").open("w", encoding="utf-8") as log:
            process = subprocess.Popen([str(executable)], cwd=executable.parent, env=env,
                stdout=log, stderr=subprocess.STDOUT,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        deadline = time.monotonic() + 180
        while time.monotonic() < deadline:
            if process.poll() is not None: raise RuntimeError("Engine exited during startup")
            try:
                frontend = Frontend(c["port"])
                session = CaptureSession(frontend, process, output / "api.jsonl")
                if session.evaluate("Boolean(window.cefQuery)"): break
                frontend.socket.close()
            except (ConnectionError, OSError, RuntimeError): continue
        else: raise TimeoutError("Engine CEF endpoint startup")
        session.until("ProjectLauncher.get_project_load_status", [], lambda v: v.get("archive_service_ready"))
        project = c.get("project_path", c["scene_path"])
        loaded = open_project_ready(session, project)
        route = c.get("scene_route", loaded["scene"]["path"])
        record.update(load=loaded, route=route)
        session.api("MainView.on_init", [project])
        session.evaluate(fixed_viewport_script() + ";(()=>{const project=" + json.dumps(project) + ";localStorage.setItem('corona.activeProjectPath',project);window.dispatchEvent(new CustomEvent('corona-active-project-changed',{detail:{projectPath:project}}));location.hash='#/';return true})()")
        def assets(v):
            if v.get("failed", 0): raise RuntimeError("Asset loading failed: " + json.dumps(v))
            return v.get("active") and not v.get("loading") and v.get("pending", 0) == 0
        record["load_status"] = session.until("ProjectLauncher.get_project_load_status", [], assets)
        cameras = session.api("SceneTools.list_camera_views", [route])["cameras"]
        source_camera = next((v for v in cameras if c["camera_id"] in (v.get("camera_id"), v.get("id"))), None) if c["camera_id"] else cameras[0]
        if source_camera is None: raise ValueError("Configured source camera not found")
        # The UI-owned source camera can resize/reset after a batch and overwrite
        # raw frame_0000. A dedicated closed offscreen camera renders only while
        # its screenshot request is pending; other cameras must not emit PT dumps.
        for existing in cameras:
            session.api("SceneTools.set_render_backend", ["native", route, existing.get("camera_id", existing["id"])])
        camera = session.api("SceneTools.create_camera_view", [route, "PT offline capture"])["camera"]
        identity = camera.get("camera_id", camera["id"])
        session.api("SceneTools.close_camera_view", [route, identity])
        record["offscreen_camera"] = {"source_camera_id": source_camera.get("camera_id", source_camera["id"]),
            "capture_camera_id": identity, "dedicated_camera": True,
            "scope": "Closed offscreen camera: only pending screenshot requests execute; original cameras use native backend."}
        session.api("SceneTools.update_camera_view", [route, identity, {"width": WIDTH, "height": HEIGHT}])
        session.api("SceneTools.set_render_backend", ["vision", route, identity])
        session.until("SceneTools.get_render_backend", [route, identity], lambda v: v.get("mode") == "vision")
        session.api("SceneTools.set_output_mode", [route, identity, "final_color"])
        session.api("SceneTools.set_vision_render_mode", [route, identity, "path_tracing"])
        record["render_mode_before"] = session.until("SceneTools.get_vision_render_mode", [route, identity],
            lambda v: v.get("mode") == "path_tracing")
        session.api("SceneTools.set_vision_denoiser", [route, identity, "svgf"])
        for suffix in ("denoise", "accumulation", "stable_planes"):
            session.api("SceneTools.set_vision_" + suffix, [route, identity, False])
            session.until("SceneTools.get_vision_" + suffix, [route, identity], lambda v: v.get("enabled") is False)
        pose = {k: source_camera[k] for k in ("position", "forward", "world_up", "projection_x_sign", "near_plane", "far_plane")}
        pose.update(c.get("camera_pose", {}))
        pose.update(fov=c.get("camera_pose", {}).get("fov", 60), persist=False)
        session.api("SceneTools.set_camera_pose", [route, identity, pose])
        def matching_camera(v):
            active = next((x for x in v["cameras"] if identity in (x.get("id"), x.get("camera_id"))), None)
            fields = {k: pose[k] for k in ("position", "forward", "world_up", "fov", "projection_x_sign", "near_plane", "far_plane")}
            return active is not None and subset_equal(active, fields)
        record["camera_verified"] = session.until("SceneTools.list_camera_views", [route], matching_camera)
        session.api("SceneTools.set_pt_settings", [route, identity, c["settings"]])
        # A closed offscreen view is deliberately idle until requested. Prime its
        # context once, then wait for effective settings before the measured batch.
        # The official fixed-frame capture independently invalidates all history.
        setup = output / "setup"
        setup.mkdir()
        record["offscreen_setup_capture"] = session.api("SceneTools.save_screenshot",
            [route, str(setup / "image.png"), identity,
             {"render_frames": 1, "warmup_frames": 0, "timings_path": str(setup / "frames.csv"),
              "timeout_seconds": c["timeout_seconds"]}], c["timeout_seconds"] + 15)
        effective = session.until("SceneTools.get_pt_settings", [route, identity],
            lambda v: v.get("ready") and subset_equal(v.get("settings"), c["settings"]))
        record["effective_before"] = effective
        record["camera_before"] = session.api("SceneTools.list_camera_views", [route])
        if any(v.get("render_backend") != "native" for v in record["camera_before"]["cameras"]
               if identity not in (v.get("camera_id"), v.get("id"))):
            raise ValueError("Another Vision camera could overwrite the shared raw dump directory")
        save_json(output / "effective-before.json", record)
        options = {"render_frames": c["render_frames"], "warmup_frames": c["warmup_frames"],
                   "timings_path": str(output / "frames.csv"), "timeout_seconds": c["timeout_seconds"]}
        capture_started_ns = time.time_ns()
        record["capture"] = session.api("SceneTools.save_screenshot",
            [route, str(output / "image.png"), identity, options], c["timeout_seconds"] + 15)
        capture_finished_ns = time.time_ns()
        record["capture_wall_window_ns"] = {"begin": capture_started_ns, "end": capture_finished_ns}
        captured_camera = record["capture"]["camera"]
        if captured_camera.get("view_open") is not False or captured_camera.get("width") != WIDTH or captured_camera.get("height") != HEIGHT:
            raise ValueError("The dedicated capture camera lost its closed 1080p contract")
        if c["capture_raw"]:
            indexes = raw_indexes(c)
            record["raw_offline_window"] = validate_raw_window(output, indexes, capture_started_ns, capture_finished_ns)
        header = (output / "image.png").read_bytes()[:24]
        if header[:8] != b"\x89PNG\r\n\x1a\n" or struct.unpack(">II", header[16:24]) != (WIDTH, HEIGHT):
            raise ValueError("Expected a 1920x1080 PNG preview")
        with (output / "frames.csv").open(encoding="utf-8-sig", newline="") as stream:
            frame_rows = list(csv.DictReader(stream))
        if any(float(row["denoise_ms"]) != 0 for row in frame_rows):
            raise ValueError("Unexpected denoising in the matched raw capture")
        record["effective_after"] = session.api("SceneTools.get_pt_settings", [route, identity])
        if not record["effective_after"].get("ready") or not subset_equal(record["effective_after"]["settings"], c["settings"]):
            raise ValueError("Effective settings changed during capture")
        record["render_mode_after"] = session.api("SceneTools.get_vision_render_mode", [route, identity])
        if record["render_mode_after"].get("mode") != "path_tracing":
            raise ValueError("Capture left native PT mode")
        record["camera_after"] = session.api("SceneTools.list_camera_views", [route])
        if any(v.get("render_backend") != "native" for v in record["camera_after"]["cameras"]
               if identity not in (v.get("camera_id"), v.get("id"))):
            raise ValueError("Another camera switched to Vision during capture")
        # End GPU work before CPU decoding/averaging potentially many raw frames.
        try:
            session.api("CoronaEditor.close_process", [])
        except Exception as close_error:
            # Some CEF transports disconnect before returning this shutdown RPC.
            record["shutdown_rpc_disconnect"] = str(close_error)
        frontend.socket.close()
        frontend = session = None
        process.wait(timeout=20)
        if c["capture_raw"]:
            indexes = raw_indexes(c)
            unchanged = validate_raw_window(output, indexes, capture_started_ns, capture_finished_ns)
            if unchanged != record["raw_offline_window"]:
                raise ValueError("Raw dumps changed between capture completion and engine shutdown")
            record["raw_boundaries_unchanged_after_shutdown"] = True
            for index in c["checkpoints"]:
                source = output / "raw" / f"frame_{index:04d}" / "raw_total.f32"
                record["raw_images"][str(index)] = raw_to_pfm(source, output / f"image_frame_{index:04d}.pfm")
        if c["reference_frames"]:
            record["reference"] = reference_mean(output, c)
        if c["quality_mean_frames"]:
            record["quality_mean"] = reference_mean(output,
                {**c, "reference_frames": c["quality_mean_frames"], "keep_reference_raw": True}, "quality_64spp")
        record["raw_scope"] = "Untouched FP32 path total from param.colors/rt_buffer immediately after path_tracing, before denoising/combine/RGB21 lobe-buffer decode/tone mapping. PNG is display evidence only."
        record["status"] = "captured"
    except Exception as error:
        record.update(status="failed", error=str(error))
        raise
    finally:
        if session:
            try: session.api("CoronaEditor.close_process", [])
            except Exception: pass
        if frontend: frontend.socket.close()
        if process:
            try: process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], capture_output=True, check=False)
                process.wait(timeout=10)
        if ini_before is not None: ini.write_bytes(ini_before)
        elif ini.exists(): ini.unlink()
        after = fingerprint(executable)
        save_json(output / "identity-after.json", after)
        record["identities_unchanged"] = identities_equal(before, after)
        if record["status"] == "captured":
            try:
                if not record["identities_unchanged"]: raise RuntimeError("Source/runtime changed during capture")
                record["gpu"] = summarize_gpu(output, c["warmup_frames"])
                if len(record["gpu"]["frames"]) != c["render_frames"] or any(
                        frame["frame"] != index + 1 or frame["sample_index"] != index
                        for index, frame in enumerate(record["gpu"]["frames"])):
                    raise ValueError("Expected exact consecutive offline frames from invalidated history")
                record["sampler_index_evidence"] = {
                    "sampler_seed": 0, "seed_runtime_getter_available": False,
                    "native_sample_index_range": [0, c["render_frames"] - 1],
                    "averaged_sample_index_range": ([c["reference_start_index"], c["render_frames"] - 1]
                        if c["reference_frames"] else [0, c["render_frames"] - 1]),
                    "source_binding": "PTParam.frame_index=integrator frame; IndependentSampler hashes sample_index+constructor seed0. stages.csv integrator_frame is checked for every frame; raw directories bind the averaged subset.",
                    "disjoint_from_quality_0_through_63": bool(c["reference_frames"] and c["reference_start_index"] >= 64)}
                record["status"] = "complete"
            except Exception as error:
                record.update(status="failed", error=str(error))
        save_json(output / "result.json", record)
    return record

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--config", type=Path, required=True)
    p.add_argument("--audit-only", action="store_true")
    a = p.parse_args()
    c = load_config(a.config)
    if a.audit_only:
        report = fingerprint(Path(c["executable"]))
        destination = Path(__file__).resolve().parent / "cpu-validation/runtime-audit.json"
        destination.parent.mkdir(parents=True, exist_ok=True)
        save_json(destination, report)
        print(destination)
        return 0
    result = run(c)
    print(json.dumps({"status": result["status"], "output": c["output"]}))
    return int(result["status"] != "complete")

if __name__ == "__main__":
    raise SystemExit(main())



