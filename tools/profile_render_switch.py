"""Profile PT -> ReSTIR through the editor's production scene/view preparation.

Example: python tools/profile_render_switch.py --scene ../CoronaExample/test_vision/render_scene/cbox/vision_scene.json
Build corona_vision_embedded_mode_switch_tests first. No third-party Python packages.
"""
import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import re
import sys

from profile_process import run_bounded
from switch_profile_report import parse_log, write_report

ROOT = Path(__file__).resolve().parents[1]


def runtime_environment(build, runtime):
    env = os.environ.copy()
    paths = []
    ctest = build / "tests/integration/CTestTestfile.cmake"
    if ctest.is_file():
        for line in ctest.read_text(encoding="utf-8").splitlines():
            if "set_tests_properties" in line and "[=[VisionEmbeddedModeSwitchTests]=]" in line:
                match = re.search(r'ENVIRONMENT_MODIFICATION "([^"]+)"', line)
                if match:
                    paths = [item.removeprefix("PATH=path_list_prepend:") for item in match[1].split(";")
                             if item.startswith("PATH=path_list_prepend:")]
                break
    # CTest applies prepends in order, so the last entry takes precedence.
    env["PATH"] = os.pathsep.join([str(runtime), *reversed(paths), env.get("PATH", "")])
    env["CORONA_SWITCH_PROFILE"] = "1"
    return env


def positive_float(value):
    number = float(value)
    if not 0 < number < float("inf"):
        raise argparse.ArgumentTypeError("must be a positive finite number")
    return number


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--scene", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "cmake-build-relwithdebinfo")
    parser.add_argument("--runtime-dir", type=Path, help="DLL/cuda/.cache directory (default: BUILD/bin)")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/switch-profile" / datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    parser.add_argument("--runs", type=int, choices=(1, 2), default=2, help="1: current cache; 2: also repeat in a new process")
    parser.add_argument("--timeout", type=positive_float, default=300, help="hard seconds per process, plus at most 20 seconds cleanup")
    parser.add_argument("--denoise", action="store_true", help="enable SVGF for both algorithms")
    args = parser.parse_args(argv)
    scene, build, output = args.scene.resolve(), args.build_dir.resolve(), args.output_dir.resolve()
    runtime = (args.runtime_dir or build / "bin").resolve()
    executable = build / "tests/integration/corona_vision_embedded_mode_switch_tests.exe"
    for path in (scene, executable):
        if not path.is_file():
            parser.error(f"File not found: {path}. Build target: corona_vision_embedded_mode_switch_tests")
    if not runtime.is_dir():
        parser.error(f"Runtime directory not found: {runtime}")
    if output.exists() and any(output.iterdir()):
        parser.error(f"Output directory is not empty: {output}; choose a new directory")
    env = runtime_environment(build, runtime)
    command = [str(executable), "--profile-switches", str(scene)]
    if args.denoise:
        command.append("--denoise")
    report = dict(scene=str(scene), runtime_dir=str(runtime), ptx_cache=str(runtime / ".cache"),
                  build_dir=str(build), denoise=args.denoise, runs=[],
                  timing="CPU wall clock; first frame includes GPU synchronization",
                  backend_detail="See emitted function scopes for DSL/NVRTC/OptiX and cache I/O. No inferred driver-cache hits.")
    succeeded = True
    for index in range(1, args.runs + 1):
        run_dir = output / f"run-{index}"
        print(f"Run {index}/{args.runs}: {scene.name}; timeout {args.timeout:g}s; logs {run_dir}", flush=True)
        process = run_bounded(command, runtime, run_dir, args.timeout, env)
        log = Path(process["stdout"])
        profile = parse_log(log.read_text(encoding="utf-8-sig", errors="replace") if log.exists() else "")
        for event in profile["events"]:
            event["pid"] = process["pid"] or 0
        (run_dir / "trace.json").write_text(json.dumps({"traceEvents": profile["events"]}), encoding="utf-8")
        report["runs"].append(dict(process=process, profile=profile))
        report_path = write_report(output, report)
        for phase in profile["phases"]:
            duration = phase["duration_ms"]
            shown = "incomplete" if duration is None else f"{duration:.3f} ms"
            print(f"  {phase['name']}: {shown}", flush=True)
        if process["status"] != "success" or profile["warnings"] or len(profile["phases"]) != 4:
            succeeded = False
            print(f"Stopped: {process['status']}; see logs. No unchanged retry.", flush=True)
            break
    print(f"Report: {report_path}\nData: {output / 'report.json'}", flush=True)
    return 0 if succeeded else 1


if __name__ == "__main__":
    sys.exit(main())
