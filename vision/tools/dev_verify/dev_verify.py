"""Dev verification orchestrator — one-click script to run build, test, and format checks.

Usage:
    python dev_verify.py                        # run all stages
    python dev_verify.py --stage test format    # multiple stages
    python dev_verify.py --stage build --config Release
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

STAGES = ["build", "test", "format"]

# Enable OpenCV EXR support for regression comparison
os.environ["OPENCV_IO_ENABLE_OPENEXR"] = "1"


def find_project_root() -> Path:
    p = Path(__file__).resolve()
    for parent in p.parents:
        if (parent / "CMakeLists.txt").exists() and (parent / "src").is_dir():
            return parent
    sys.exit("ERROR: cannot locate Vision project root")


def script_dir() -> Path:
    return Path(__file__).resolve().parent


def run_script(name: str, args: list[str], label: str) -> bool:
    """Run a sibling Python script. Returns True on success."""
    script = script_dir() / name
    if not script.exists():
        print(f"  ERROR: script not found: {script}")
        return False

    print(f"{'=' * 60}")
    print(f"  STAGE: {label}")
    print(f"{'=' * 60}")
    t0 = time.time()

    result = subprocess.run(
        [sys.executable, str(script)] + args,
    )

    elapsed = time.time() - t0
    status = "PASS" if result.returncode == 0 else "FAIL"
    print(f"\n  [{status}] {label}  ({elapsed:.1f}s)\n")
    return result.returncode == 0


def _find_vcvarsall() -> str | None:
    """Auto-detect vcvarsall.bat via vswhere, fall back to common paths."""
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
              "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.exists():
        result = subprocess.run(
            [str(vswhere), "-latest", "-products", "*",
             "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
             "-property", "installationPath"],
            capture_output=True, text=True
        )
        vs_path = result.stdout.strip()
        if vs_path:
            candidate = Path(vs_path) / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat"
            if candidate.exists():
                return str(candidate)
    # Fallback: common installation paths
    for edition in ("Community", "Professional", "Enterprise"):
        for year in ("2022", "2019"):
            candidate = Path(rf"C:\Program Files\Microsoft Visual Studio\{year}\{edition}"
                             r"\VC\Auxiliary\Build\vcvarsall.bat")
            if candidate.exists():
                return str(candidate)
    return None


def stage_build(project_root: Path, config: str, target: str) -> bool:
    """Build the project using ninja."""
    build_dir = project_root / f"cmake-build-{config.lower()}"
    if not build_dir.is_dir():
        print(f"  ERROR: build directory not found: {build_dir}")
        return False

    vcvarsall = _find_vcvarsall()
    if not vcvarsall:
        print("  ERROR: vcvarsall.bat not found. Install Visual Studio with C++ workload.")
        return False
    cmd = f'"{vcvarsall}" x64 >nul 2>&1 && ninja -j8 {target}'

    print(f"{'=' * 60}")
    print(f"  STAGE: build ({config}, target={target})")
    print(f"{'=' * 60}")
    t0 = time.time()

    result = subprocess.run(
        ["cmd", "/c", cmd],
        cwd=build_dir,
    )

    elapsed = time.time() - t0
    status = "PASS" if result.returncode == 0 else "FAIL"
    print(f"\n  [{status}] build  ({elapsed:.1f}s)\n")
    return result.returncode == 0


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Vision development verification pipeline",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=f"Available stages: {', '.join(STAGES)}\n"
               "Default: run all stages in order.",
    )
    parser.add_argument("--stage", nargs="*", default=None,
                        choices=STAGES,
                        help="Stages to run (default: all)")
    parser.add_argument("--config", type=str, default="Release",
                        help="Build configuration (default: Release)")
    parser.add_argument("--target", type=str, default="vision-base",
                        help="Build target (default: vision-base)")
    parser.add_argument("--stop-on-fail", action="store_true",
                        help="Stop at first failing stage")
    args = parser.parse_args()

    project_root = find_project_root()
    stages = args.stage if args.stage else STAGES
    build_dir_str = str(project_root / f"cmake-build-{args.config.lower()}")

    print(f"Vision Dev Verify — stages: {', '.join(stages)}")
    print(f"  project: {project_root}")
    print(f"  config:  {args.config}")
    print()

    results = {}
    t_total = time.time()

    for stage in stages:
        if stage == "build":
            ok = stage_build(project_root, args.config, args.target)

        elif stage == "test":
            test_args = ["--build-dir", build_dir_str,
                         "--tests", "test-param_schema", "test-material-energy",
                         "--ensure-targets", "test-param_schema", "test-material-energy"]
            ok = run_script("run_tests.py", test_args, "unit tests")

        elif stage == "format":
            ok = run_script("format_check.py", [], "format check")

        else:
            continue

        results[stage] = ok
        if not ok and args.stop_on_fail:
            break

    # Summary
    elapsed_total = time.time() - t_total
    print(f"\n{'=' * 60}")
    print(f"  SUMMARY  ({elapsed_total:.1f}s total)")
    print(f"{'=' * 60}")
    all_pass = True
    for stage in stages:
        status = results.get(stage)
        if status is None:
            tag = "SKIP"
        elif status:
            tag = "PASS"
        else:
            tag = "FAIL"
            all_pass = False
        print(f"  {tag:<6s} {stage}")

    print()
    # Write summary report
    report_path = script_dir() / "report_summary.md"
    with open(report_path, "w", encoding="utf-8") as f:
        f.write(f"# Dev Verify Summary Report\n\n")
        f.write(f"- **Date**: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
        f.write(f"- **Config**: {args.config}\n")
        f.write(f"- **Total Time**: {elapsed_total:.1f}s\n\n")
        f.write(f"| Stage | Result |\n")
        f.write(f"|-------|--------|\n")
        for stage in stages:
            status = results.get(stage)
            tag = "SKIP" if status is None else ("PASS" if status else "FAIL")
            f.write(f"| {stage} | {tag} |\n")
        f.write(f"\n**Overall**: {'ALL PASSED' if all_pass else 'SOME FAILED'}\n")
    print(f"Report: {report_path}")

    if all_pass:
        print("All stages passed.")
    else:
        print("Some stages failed.")
        sys.exit(1)


if __name__ == "__main__":
    main()
