"""Render Mitsuba comparisons through the engine's normal screenshot API.

No image conversion is performed here. Fixed-frame sampling and PNG export are
both handled by SceneTools.save_screenshot, using the viewport render target.
Run with the coronaengine-dev Python environment (websocket-client required).
"""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import socket
import statistics
import struct
import subprocess
import time
import xml.etree.ElementTree as ET


REPO = Path(__file__).resolve().parents[1]
MODES = {
    "pt2048": ("path_tracing", False, True, 2048, 0),
    "pt_reblur": ("path_tracing", True, False, 64, 32),
    "restir_reblur": ("restir", True, False, 64, 32),
}
UNSUPPORTED_HAIR = {"hair-curl", "curly-hair", "straight-hair", "furball"}
WIDTH, HEIGHT = 1920, 1080


def save_json(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def select_scenes(resources, requested=None):
    scenes = json.loads((resources / "manifest.json").read_text(encoding="utf-8-sig"))["scenes"]
    unknown = set(requested or []) - {scene["id"] for scene in scenes}
    if unknown:
        raise ValueError("Unknown scene: " + ", ".join(sorted(unknown)))
    selected = []
    for scene in scenes:
        if requested and scene["id"] not in requested:
            continue
        xml_files = scene["xml_files"]
        source = resources / scene["directory"] / ("scene_v3.xml" if "scene_v3.xml" in xml_files else xml_files[0])
        if not source.is_file():
            raise FileNotFoundError(source)
        selected.append({**scene, "source": source})
    return selected


def capture_options(mode, frames, output, timeout):
    _, denoise, _, default_frames, warmup = MODES[mode]
    count = default_frames if frames is None else frames
    if count < 1:
        raise ValueError("frames must be positive")
    return {"render_frames": count, "warmup_frames": min(warmup, count // 2) if denoise else 0,
            "timings_path": str(output / "frames.csv"), "timeout_seconds": timeout}


def fixed_viewport_script():
    # A fixed 1080p capture must fit inside the live viewport; changing only
    # the render arguments would stretch that image to the window's aspect.
    return """(()=>{
const bridge=window.coronaBridge;
if(!bridge?.setCameraViewport)throw Error('Viewport bridge unavailable');
const original=bridge.setCameraViewport.bind(bridge);
bridge.setCameraViewport=(...args)=>{
 const scale=Math.min(args[3]/1920,args[4]/1080);
 const width=1920*scale,height=1080*scale;
 args[1]+=(args[3]-width)/2;args[2]+=(args[4]-height)/2;
 args[3]=width;args[4]=height;args[5]=1920;args[6]=1080;
 return original(...args);
};return true})()"""


def prepare_output(output):
    if output.exists() and any(output.iterdir()):
        raise FileExistsError(f"Output directory is not empty; preserving existing captures: {output}")
    output.mkdir(parents=True, exist_ok=True)


def ensure_port_available(port):
    if not 1 <= port <= 65535:
        raise ValueError("Debug port must be between 1 and 65535")
    with socket.socket() as probe:
        if os.name == "nt":
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        else:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            probe.bind(("127.0.0.1", port))
        except OSError as error:
            raise RuntimeError(f"Debug port {port} is already in use or unavailable; refusing to attach") from error


def api_data(name, response):
    data = response.get("data", response)
    if not response.get("success") or (isinstance(data, dict) and
            (data.get("status") in ("error", "failed") or data.get("ok") is False)):
        raise RuntimeError(f"{name}: {json.dumps(response, ensure_ascii=False)}")
    return data


def engine_environment(source, cache, port):
    environment = dict(source)
    for name in list(environment):
        if name.startswith(("VISION_EVAL_", "REBLUR_", "RELAX_", "HORIZON_BENCH_")) or name in {
            "CORONA_OPTICS_MAX_FPS", "CORONA_BATCH_CAPTURE_DIR", "VISION_DISABLE_DENOISER",
            "VISION_STAGE_PROFILE", "CORONA_VISION_FRAME_PROFILE", "CORONA_OPTICS_MEASURE_RENDER_TIME",
        }:
            environment.pop(name)
    environment.update(CORONA_CEF_REMOTE_DEBUG_PORT=str(port), CORONA_CEF_ROOT_CACHE=str(cache),
                       CORONA_SWITCH_PROFILE="0")
    return environment


def confirm_thermal_protection():
    result = subprocess.run(["nvidia-smi", "-q", "-x"], capture_output=True, text=True,
                            check=True, timeout=10, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    devices = []
    for gpu in ET.fromstring(result.stdout).findall("gpu"):
        slowdown = gpu.findtext("temperature/gpu_temp_slow_threshold", "N/A")
        shutdown = gpu.findtext("temperature/gpu_temp_max_threshold", "N/A")
        if not re.fullmatch(r"\d+ C", slowdown) or not re.fullmatch(r"\d+ C", shutdown):
            raise RuntimeError("NVIDIA driver did not report thermal slowdown and shutdown thresholds")
        devices.append({"name": gpu.findtext("product_name"), "slowdown": slowdown, "shutdown": shutdown,
                        "current_temperature": gpu.findtext("temperature/gpu_temp")})
    if not devices:
        raise RuntimeError("No NVIDIA GPU found")
    return devices


def read_capture(output, frames, warmup, denoise):
    with (output / "image.png").open("rb") as image:
        header = image.read(24)
    if header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR" or len(header) != 24:
        raise ValueError("Invalid screenshot PNG header")
    if struct.unpack(">II", header[16:24]) != (WIDTH, HEIGHT):
        raise ValueError("Screenshot resolution is not 1920x1080")
    with (output / "frames.csv").open(encoding="utf-8-sig", newline="") as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != frames or any(int(row["frame"]) != i or int(row["integrator_frame"]) != i
                                 for i, row in enumerate(rows, 1)):
        raise ValueError(f"Expected exactly {frames} consecutive frames from a reset history")
    rendering = [float(row["render_including_denoise_ms"]) for row in rows]
    filtering = [float(row["denoise_ms"]) for row in rows]
    if any(not math.isfinite(t) or t < 0 for t in rendering + filtering) or sum(rendering) <= 0:
        raise ValueError("Invalid rendering timings")
    if denoise and sum(filtering) <= 0:
        raise ValueError("Denoising did not execute")
    return {"frames": frames, "warmup_frames": warmup, "measured_frames": frames - warmup,
            "spp_per_frame": 1, "width": WIDTH, "height": HEIGHT,
            "render_including_denoise_total_ms": sum(rendering), "denoise_total_ms": sum(filtering),
            "render_including_denoise_mean_ms": statistics.mean(rendering[warmup:]),
            "denoise_mean_ms": statistics.mean(filtering[warmup:]),
            "png_sha256": hashlib.sha256((output / "image.png").read_bytes()).hexdigest()}


class Session:
    def __init__(self, frontend, process, log):
        self.frontend, self.process, self.log = frontend, process, log

    def evaluate(self, expression, timeout=30):
        self.frontend.socket.settimeout(timeout + 10)
        response = self.frontend.call("Runtime.evaluate", {"expression": expression, "returnByValue": True,
            "awaitPromise": True, "timeout": timeout * 1000, "userGesture": True})
        if "exceptionDetails" in response:
            raise RuntimeError(response["exceptionDetails"])
        return response.get("result", {}).get("value")

    def api(self, name, args, timeout=30):
        request = json.dumps({"api": name, "args": args})
        expression = "new Promise((resolve,reject)=>window.cefQuery({request:" + json.dumps(request) + ",persistent:false,onSuccess:r=>resolve(JSON.parse(r)),onFailure:(c,m)=>reject(new Error(c+':'+m))}))"
        response = self.evaluate(expression, timeout)
        with self.log.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps({"api": name, "args": args, "response": response}, ensure_ascii=False) + "\n")
        return api_data(name, response)

    def until(self, name, args, ready, timeout=180):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError("Engine exited; see engine.log")
            value = self.api(name, args)
            if ready(value):
                return value
        raise TimeoutError(name + " did not become ready")


def run_scene(scene, modes, args, output):
    from vision_frontend_probe import Frontend

    output.mkdir()
    record = {"scene_id": scene["id"], "source": str(scene["source"]), "modes": {}}
    if scene["id"] in UNSUPPORTED_HAIR:
        record.update(status="unsupported_geometry", error="Original hair geometry is unsupported; scene kept unchanged")
        save_json(output / "scene-result.json", record)
        return record
    ensure_port_available(args.port)
    environment = engine_environment(os.environ, output / "cef-cache", args.port)
    ctest = args.executable.parents[2] / "tests/integration/CTestTestfile.cmake"
    paths = re.findall(r'PATH=path_list_prepend:([^;"\n]+)', ctest.read_text(encoding="utf-8")) if ctest.is_file() else []
    environment["PATH"] = os.pathsep.join(dict.fromkeys([str(args.executable.parent), *paths, environment.get("PATH", "")]))
    ini = args.executable.parent / "CoronaEditor.ini"
    ini_before = ini.read_bytes() if ini.exists() else None
    frontend = process = session = None
    try:
        with (output / "engine.log").open("w", encoding="utf-8") as log:
            process = subprocess.Popen([str(args.executable)], cwd=args.executable.parent, env=environment,
                stdout=log, stderr=subprocess.STDOUT, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("Engine exited during startup; see engine.log")
            try:
                frontend = Frontend(args.port)
                session = Session(frontend, process, output / "api.jsonl")
                if session.evaluate("Boolean(window.cefQuery)"):
                    break
                frontend.socket.close()
            except (ConnectionError, OSError, RuntimeError):
                continue
        else:
            raise TimeoutError("Engine frontend startup timed out")
        session.until("ProjectLauncher.get_project_load_status", [], lambda state: state.get("archive_service_ready"))
        loaded = session.api("ProjectLauncher.open_project", [str(scene["source"]), {"load_policy": "degraded"}])
        record["load"] = loaded
        route = loaded["scene"]["path"]
        session.api("MainView.on_init", [str(scene["source"])])
        session.evaluate(fixed_viewport_script() + ";(()=>{const project=" + json.dumps(str(scene["source"])) + ";localStorage.setItem('corona.activeProjectPath',project);window.dispatchEvent(new CustomEvent('corona-active-project-changed',{detail:{projectPath:project}}));location.hash='#/';return true})()")
        def assets_ready(state):
            if state.get("failed", 0):
                raise RuntimeError("Asset loading failed: " + json.dumps(state))
            return state.get("active") and not state.get("loading") and state.get("pending", 0) == 0
        record["load_status"] = session.until("ProjectLauncher.get_project_load_status", [], assets_ready)
        if record["load_status"].get("total", 0) == 0:
            raise RuntimeError("Importer produced no renderable assets")
        camera = session.api("SceneTools.list_camera_views", [route])["cameras"][0]
        identity = camera.get("camera_id", camera["id"])
        session.api("SceneTools.update_camera_view", [route, identity, {"width": WIDTH, "height": HEIGHT}])
        session.api("SceneTools.set_render_backend", ["vision", route, identity])
        session.until("SceneTools.get_render_backend", [route, identity], lambda state: state.get("mode") == "vision")
        session.api("SceneTools.set_output_mode", [route, identity, "final_color"])
        session.until("SceneTools.get_output_mode", [route, identity], lambda state: state.get("mode") == "final_color")
        session.api("SceneTools.set_vision_stable_planes", [route, identity, True])
        session.api("SceneTools.set_camera_pose", [route, identity, {
            **{key: camera[key] for key in ("position", "forward", "world_up", "fov", "projection_x_sign", "near_plane", "far_plane")},
            "persist": False}])
        record["camera"] = camera
        for mode in modes:
            mode_output = output / mode
            mode_output.mkdir()
            render_mode, denoise, accumulation, _, _ = MODES[mode]
            session.api("SceneTools.set_vision_render_mode", [route, identity, render_mode])
            session.api("SceneTools.set_vision_denoiser", [route, identity, "reblur"])
            session.api("SceneTools.set_vision_denoise", [route, identity, denoise])
            session.api("SceneTools.set_vision_accumulation", [route, identity, accumulation])
            session.api("SceneTools.set_" + ("restir" if render_mode == "restir" else "pt") + "_settings",
                        [route, identity, {"max_depth": 16, "min_depth": 5, "rr_threshold": 1}])
            options = capture_options(mode, args.frames, mode_output, args.timeout)
            start = time.monotonic()
            capture = session.api("SceneTools.save_screenshot", [route, str(mode_output / "image.png"), identity, options], args.timeout + 15)
            result = read_capture(mode_output, options["render_frames"], options["warmup_frames"], denoise)
            result.update(status="complete", directory=str(mode_output), screenshot=capture,
                          capture_wall_seconds=time.monotonic() - start)
            save_json(mode_output / "result.json", result)
            record["modes"][mode] = result
            save_json(output / "scene-result.json", record)
            print(json.dumps({"scene": scene["id"], "mode": mode, "frames": result["frames"],
                              "render_ms": result["render_including_denoise_mean_ms"]}), flush=True)
        record["status"] = "complete"
    except Exception as error:
        record.update(status="failed", error=str(error))
    finally:
        if session:
            try:
                session.api("CoronaEditor.close_process", [])
            except Exception:
                pass
        if frontend:
            frontend.socket.close()
        if process:
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                if os.name == "nt":
                    subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], capture_output=True, check=False)
                else:
                    process.terminate()
                process.wait(timeout=10)
        if ini_before is not None:
            ini.write_bytes(ini_before)
        elif ini.exists():
            ini.unlink()
        save_json(output / "scene-result.json", record)
    return record


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resources", type=Path, default=REPO.parent / "render_resource")
    parser.add_argument("--executable", type=Path, default=REPO / "cmake-build-relwithdebinfo/examples/engine/corona_engine.exe")
    parser.add_argument("--output", type=Path, default=REPO / ".workspace/experiments" / datetime.now().strftime("mitsuba-viewport-%Y%m%d-%H%M%S"))
    parser.add_argument("--scene", action="append", help="Scene id; repeat to select several (default: all)")
    parser.add_argument("--mode", action="append", choices=MODES, help="Repeat to select modes (default: all three)")
    parser.add_argument("--frames", type=int, help="Override frame count for a short validation run")
    parser.add_argument("--timeout", type=int, default=7200, help="Per-capture engine timeout in seconds")
    parser.add_argument("--port", type=int, default=9237)
    args = parser.parse_args(argv)
    args.resources, args.executable, args.output = args.resources.resolve(), args.executable.resolve(), args.output.resolve()
    if args.frames is not None and args.frames < 1 or args.timeout < 1:
        parser.error("--frames and --timeout must be positive")
    try:
        scenes = select_scenes(args.resources, args.scene)
        if not args.executable.is_file():
            raise FileNotFoundError(args.executable)
        ensure_port_available(args.port)
        prepare_output(args.output)
        thermal = confirm_thermal_protection()
        save_json(args.output / "batch-config.json", {"resolution": [WIDTH, HEIGHT], "gpu": thermal,
            "executable": str(args.executable), "screenshot_api": "SceneTools.save_screenshot",
            "started_utc": datetime.now(timezone.utc).isoformat(), "frames_override": args.frames,
            "modes": args.mode or list(MODES), "scenes": [scene["id"] for scene in scenes]})
        records = []
        for scene in scenes:
            record = run_scene(scene, args.mode or list(MODES), args, args.output / scene["id"])
            records.append(record)
            save_json(args.output / "summary.json", {"records": records, "resolution": [WIDTH, HEIGHT]})
            print(json.dumps({"scene": scene["id"], "status": record["status"], "error": record.get("error")}), flush=True)
        return int(any(record["status"] == "failed" for record in records))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print("Batch failed: " + str(error), flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
