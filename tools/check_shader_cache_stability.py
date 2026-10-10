"""Check real PT/ReSTIR cache reuse after perturbing bindless allocation history.

Runs two independent processes without deleting existing cache files. The second
process registers unused slots before creating each view; shader variants must
keep their identities and hit the PTX cache warmed by the first process.
"""
import argparse
import json
import re
from collections import Counter
from datetime import datetime
from pathlib import Path

from profile_process import run_bounded
from profile_render_switch import positive_float, runtime_environment
from switch_profile_report import parse_log


ROOT = Path(__file__).resolve().parents[1]


def compare_runs(runs):
    errors = []
    phases = ("PT.initial", "PT_to_ReSTIR", "ReSTIR_to_PT", "PT_to_ReSTIR.repeat")
    for index, run in enumerate(runs, 1):
        if run["process"]["status"] != "success":
            errors.append(f"run {index}: {run['process']['status']}")
        profile = run["profile"]
        errors.extend(f"run {index}: {warning}" for warning in profile["warnings"])
        if [p["name"] for p in profile["phases"]] != list(phases):
            errors.append(f"run {index}: incomplete switch phases")
        if any(p["duration_ms"] is None for p in profile["phases"]):
            errors.append(f"run {index}: incomplete measured phase")
        for phase in phases[:2]:
            if not any(s["phase"] == phase for s in profile["shaders"]):
                errors.append(f"run {index}: no shader evidence for {phase}")
        if any(s["phase"] in phases[2:] for s in profile["shaders"]):
            errors.append(f"run {index}: repeated switch rebuilt shaders")
        expected_padding = [[phase, str(run["padding"])] for phase in phases[:2]] if run["padding"] else []
        if run["padding_events"] != expected_padding:
            errors.append(f"run {index}: binding allocation perturbation was not confirmed")
        if [entry[0] for entry in run["bindings"]] != list(phases):
            errors.append(f"run {index}: actual binding slot evidence is incomplete")
    if len(runs) != 2:
        return errors + ["two completed processes are required"]
    def identities(run):
        return Counter((s["phase"], s["name"]) for s in run["profile"]["shaders"])
    if identities(runs[0]) != identities(runs[1]):
        errors.append("changing binding slots changed shader identities")
    if any(s["cache"] != "hit" for s in runs[1]["profile"]["shaders"]):
        errors.append("second process did not hit every warmed PTX cache entry")
    before = {phase: (int(visibility), int(surfaces)) for phase, visibility, surfaces in runs[0]["bindings"]}
    after = {phase: (int(visibility), int(surfaces)) for phase, visibility, surfaces in runs[1]["bindings"]}
    for phase in phases[:2]:
        if phase not in before or phase not in after:
            errors.append(f"{phase}: actual shader buffer slots are missing")
        # PT does not allocate the ReSTIR-only surfaces buffer.
        elif any(a == b or 0xffffffff in (a, b)
                 for a, b in list(zip(before[phase], after[phase]))[:1 if phase == "PT.initial" else 2]):
            errors.append(f"{phase}: real shader buffer slots must change between processes")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene", required=True, type=Path)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "cmake-build-relwithdebinfo")
    parser.add_argument("--runtime-dir", type=Path)
    parser.add_argument("--output-dir", type=Path,
                        default=ROOT / "build/cache-stability" / datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    parser.add_argument("--timeout", type=positive_float, default=300)
    args = parser.parse_args()
    build, scene, output = args.build_dir.resolve(), args.scene.resolve(), args.output_dir.resolve()
    runtime = (args.runtime_dir or build / "bin").resolve()
    executable = build / "tests/integration/corona_vision_embedded_mode_switch_tests.exe"
    for path in (scene, executable):
        if not path.is_file():
            parser.error(f"File not found: {path}")
    if output.exists() and any(output.iterdir()):
        parser.error(f"Output directory is not empty: {output}")
    env = runtime_environment(build, runtime)
    report = dict(scene=str(scene), runtime_dir=str(runtime), runs=[])
    for index, padding in enumerate((0, 7), 1):
        print(f"Run {index}/2: padding={padding}, hard timeout={args.timeout:g}s", flush=True)
        process = run_bounded([str(executable), "--profile-bindings", str(scene), str(padding)],
                              runtime, output / f"run-{index}", args.timeout, env)
        log = Path(process["stdout"])
        text = log.read_text(encoding="utf-8-sig", errors="replace") if log.exists() else ""
        profile = parse_log(text)
        padding_events = [list(event) for event in re.findall(r"BINDING_PADDING (\S+) slots=(\d+)", text)]
        bindings = re.findall(r"BINDING_SLOTS (\S+) visibility=(\d+) surfaces=(\d+)", text)
        report["runs"].append(dict(padding=padding, padding_events=padding_events,
                                   bindings=bindings, process=process, profile=profile))
        (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(f"  {process['status']}: {profile['cache']}", flush=True)
        if process["status"] != "success":
            break
    report["errors"] = compare_runs(report["runs"])
    report["passed"] = not report["errors"]
    (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    for error in report["errors"]:
        print(f"FAIL: {error}")
    print(f"{'PASS' if report['passed'] else 'FAIL'}: {output / 'report.json'}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
