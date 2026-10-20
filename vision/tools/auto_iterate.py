"""
auto_iterate.py — Automated build → eval → judge → log pipeline.

Usage:
    python tools/auto_iterate.py --scene spaceship_desktop4k --config quality --change "describe what changed"
    python tools/auto_iterate.py --scene spaceship_desktop4k --config quality --change "Phase 2 rewrite v1" --iteration 7

Flow:
    1. Build vision-eval via build_standalone.cmd
    2. Run vision-eval directly (bypasses PowerShell wrapper)
    3. Compare RMSE and frame_time against previous best and paper guard
    4. Log results to .agent-os/autoresearch-results.tsv
    5. Exit 0 if improvement, exit 1 if regression/failure
"""

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCENE_TARGETS = REPO_ROOT / "tools" / "scene_targets.json"
RESULTS_LOG = REPO_ROOT / ".agent-os" / "autoresearch-results.tsv"
ITERATION_LOG = REPO_ROOT / ".agent-os" / "auto-iterate-log.jsonl"
BUILD_CMD = REPO_ROOT / "tools" / "build_standalone.cmd"
GOLDEN_ROOT = REPO_ROOT / "eval-out" / "golden"

CMD_EXE = Path(r"C:\Windows\System32\cmd.exe")



def load_targets() -> dict:
    return json.loads(SCENE_TARGETS.read_text(encoding="utf-8"))


def get_runtime_env() -> dict:
    env = os.environ.copy()
    return env


# ── Build ──────────────────────────────────────────────────────────────────

def build(build_dir: Path, target: str = "vision-eval") -> bool:
    targets = ["vision-denoiser-SSAT", target]
    for t in targets:
        print(f"[build] target={t} dir={build_dir}")
        result = subprocess.run(
            [str(CMD_EXE), "/c", str(BUILD_CMD), t, str(build_dir)],
            capture_output=True,
            text=True,
            cwd=str(REPO_ROOT),
            timeout=600,
        )
        success = result.returncode == 0 and "FAILED:" not in result.stdout
        if not success:
            last_lines = result.stdout.strip().split("\n")[-20:]
            print(f"[build] {t} FAILED — last 20 lines:")
            for line in last_lines:
                print(f"  {line}")
            if result.stderr.strip():
                print(f"[build] stderr: {result.stderr.strip()[:500]}")
            return False
    print("[build] ALL TARGETS SUCCESS")
    return True


# ── Eval ───────────────────────────────────────────────────────────────────

def run_eval(
    exe: Path,
    runtime_dir: Path,
    scene: Path,
    output: Path,
    metrics_file: Path,
    reference: Path | None,
    save_spp: int = 1,
    warmup: int = 8,
    profile: int = 32,
    stage_profile: bool = True,
) -> dict | None:
    cmd = [
        str(exe),
        "-r", str(runtime_dir),
        "-m", "cli",
        "-s", str(scene),
        "--save-spp", str(save_spp),
        "--warmup-frames", str(warmup),
        "--profile-frames", str(profile),
        "--metrics-file", str(metrics_file),
        "-o", str(output),
    ]
    if stage_profile:
        cmd.extend(["--stage-profile", "true"])
    if reference and reference.exists():
        cmd.extend(["--rmse-reference", str(reference)])

    metrics_file.parent.mkdir(parents=True, exist_ok=True)

    print(f"[eval] scene={scene.name} spp={save_spp}")
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            cwd=str(REPO_ROOT),
            env=get_runtime_env(),
            timeout=1200,
        )
    except subprocess.TimeoutExpired:
        print("[eval] TIMEOUT (1200s)")
        return None

    if result.returncode != 0:
        print(f"[eval] FAILED (exit={result.returncode})")
        stderr_tail = result.stderr.strip().split("\n")[-10:]
        for line in stderr_tail:
            print(f"  {line}")
        return None

    if not metrics_file.exists():
        print("[eval] FAILED — no metrics file produced")
        stdout_tail = result.stdout.strip().split("\n")[-10:]
        for line in stdout_tail:
            print(f"  {line}")
        return None

    metrics = json.loads(metrics_file.read_text(encoding="utf-8"))
    print(f"[eval] frame_ms={metrics.get('average_frame_ms', '?'):.2f}  "
          f"rmse={metrics.get('rmse', '?'):.6f}  "
          f"spatial_angular_ms={metrics.get('average_spatial_angular_ms', '?')}")
    return metrics


# ── Golden ─────────────────────────────────────────────────────────────────

def ensure_golden(
    scene_key: str,
    exe: Path,
    runtime_dir: Path,
    scene: Path,
    golden_spp: int = 4096,
) -> Path:
    golden_dir = GOLDEN_ROOT / scene_key
    golden_dir.mkdir(parents=True, exist_ok=True)
    golden_png = golden_dir / f"{scene.stem}_golden.png"
    golden_metrics = golden_dir / f"{scene.stem}_golden_metrics.json"

    if golden_png.exists():
        print(f"[golden] reusing {golden_png}")
        return golden_png

    print(f"[golden] generating {golden_spp} spp golden for {scene.name}...")
    cmd = [
        str(exe),
        "-r", str(runtime_dir),
        "-m", "cli",
        "-s", str(scene),
        "--save-spp", str(golden_spp),
        "--warmup-frames", "2",
        "--profile-frames", "4",
        "--metrics-file", str(golden_metrics),
        "-o", str(golden_png),
        "--disable-denoiser", "true",
    ]
    result = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        cwd=str(REPO_ROOT),
        env=get_runtime_env(),
        timeout=3600,
    )
    if result.returncode != 0 or not golden_png.exists():
        print(f"[golden] FAILED to generate golden")
        print(result.stdout[-500:] if result.stdout else "")
        sys.exit(1)

    print(f"[golden] saved to {golden_png}")
    return golden_png


# ── Judge ──────────────────────────────────────────────────────────────────

def load_best_rmse(scene_key: str) -> float | None:
    best_file = REPO_ROOT / ".agent-os" / "best_rmse.json"
    if not best_file.exists():
        return None
    data = json.loads(best_file.read_text(encoding="utf-8"))
    return data.get(scene_key)


def save_best_rmse(scene_key: str, rmse: float) -> None:
    best_file = REPO_ROOT / ".agent-os" / "best_rmse.json"
    data = {}
    if best_file.exists():
        data = json.loads(best_file.read_text(encoding="utf-8"))
    data[scene_key] = rmse
    best_file.write_text(json.dumps(data, indent=2), encoding="utf-8")


def judge(
    metrics: dict,
    guard_ms: float,
    rmse_target: float,
    scene_key: str,
) -> tuple[str, str]:
    """Returns (status, reason)."""
    rmse = metrics.get("rmse", 999.0)
    frame_ms = metrics.get("average_frame_ms", 999999.0)
    prev_best = load_best_rmse(scene_key)

    if frame_ms > guard_ms:
        return "discard", f"frame_time {frame_ms:.2f}ms > guard {guard_ms:.2f}ms"

    if prev_best is not None and rmse >= prev_best:
        return "discard", f"rmse {rmse:.6f} >= previous best {prev_best:.6f}"

    if rmse <= rmse_target:
        save_best_rmse(scene_key, rmse)
        return "target_hit", f"rmse {rmse:.6f} <= target {rmse_target:.6f}, frame_ms {frame_ms:.2f} <= guard {guard_ms:.2f}"

    save_best_rmse(scene_key, rmse)
    return "keep", f"rmse improved {prev_best or 'N/A'} -> {rmse:.6f}, frame_ms {frame_ms:.2f} <= guard {guard_ms:.2f}"


# ── Log ────────────────────────────────────────────────────────────────────

def log_result(
    iteration: int,
    status: str,
    change: str,
    scene_key: str,
    scene_file: str,
    save_spp: int,
    golden_spp: int,
    rmse: float,
    frame_ms: float,
    fps: float,
    guard_ms: float,
    notes: str,
) -> None:
    if not RESULTS_LOG.exists():
        RESULTS_LOG.write_text(
            "iteration\tstatus\tchange\tverify_scene\tsave_spp\tgolden_spp\t"
            "rmse\taverage_frame_ms\taverage_fps\tguard_frame_ms\tnotes\n",
            encoding="utf-8",
        )
    row = "\t".join([
        str(iteration), status, change, scene_file, str(save_spp),
        str(golden_spp), f"{rmse:.6f}", f"{frame_ms:.6f}", f"{fps:.6f}",
        f"{guard_ms:.6f}", notes,
    ])
    with RESULTS_LOG.open("a", encoding="utf-8", newline="\n") as f:
        f.write(row + "\n")

    entry = {
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "iteration": iteration,
        "scene_key": scene_key,
        "status": status,
        "change": change,
        "rmse": rmse,
        "frame_ms": frame_ms,
        "guard_ms": guard_ms,
        "notes": notes,
    }
    with ITERATION_LOG.open("a", encoding="utf-8") as f:
        f.write(json.dumps(entry) + "\n")

    print(f"[log] iter={iteration} status={status} rmse={rmse:.6f} frame_ms={frame_ms:.2f}")


# ── Main ───────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(description="Automated build → eval → judge → log.")
    parser.add_argument("--scene", required=True, help="Scene key from scene_targets.json")
    parser.add_argument("--config", default="quality", choices=["quality", "performance"],
                        help="Paper config: quality (ρ=1) or performance (ρ=1/3)")
    parser.add_argument("--change", required=True, help="Short description of what changed")
    parser.add_argument("--iteration", type=int, help="Iteration number (auto-increment if omitted)")
    parser.add_argument("--build-dir", default="cmake-build-codex-msvc3")
    parser.add_argument("--skip-build", action="store_true", help="Skip build step")
    parser.add_argument("--golden-spp", type=int, default=4096)
    parser.add_argument("--warmup", type=int, default=8)
    parser.add_argument("--profile", type=int, default=32)
    args = parser.parse_args()

    targets_data = load_targets()
    scene_root = Path(targets_data["scene_root"])

    if args.scene not in targets_data["scenes"]:
        print(f"[error] unknown scene '{args.scene}'. Available:")
        for k in targets_data["scenes"]:
            print(f"  {k}")
        sys.exit(1)

    scene_cfg = targets_data["scenes"][args.scene]
    config = scene_cfg["targets"][args.config]
    scene_path = scene_root / scene_cfg["scene_file"]

    if not scene_path.exists():
        print(f"[error] scene file not found: {scene_path}")
        if "derive_from" in scene_cfg:
            print(f"  → needs to be derived from {scene_cfg['derive_from']}")
            print(f"  → run: python tools/auto_iterate.py --derive-scene {args.scene}")
        sys.exit(1)

    guard_ms = config["guard_ms"]
    rmse_target = config["rmse_target"]
    build_dir = REPO_ROOT / args.build_dir

    iteration = args.iteration
    if iteration is None:
        if ITERATION_LOG.exists():
            lines = ITERATION_LOG.read_text(encoding="utf-8").strip().split("\n")
            last = json.loads(lines[-1]) if lines else {}
            iteration = last.get("iteration", -1) + 1
        else:
            iteration = 0

    print(f"{'='*60}")
    print(f"[auto_iterate] scene={args.scene} config={args.config}")
    print(f"[auto_iterate] guard={guard_ms}ms rmse_target={rmse_target}")
    print(f"[auto_iterate] iteration={iteration} change={args.change}")
    print(f"{'='*60}")

    # Step 1: Build
    if not args.skip_build:
        if not build(build_dir):
            log_result(iteration, "build_fail", args.change, args.scene,
                       scene_cfg["scene_file"], 1, args.golden_spp,
                       0.0, 0.0, 0.0, guard_ms, "build failed")
            sys.exit(2)

    exe = build_dir / "bin" / "vision-eval.exe"
    runtime_dir = build_dir / "bin"
    if not exe.exists():
        print(f"[error] exe not found: {exe}")
        sys.exit(1)

    # Step 2: Ensure golden reference
    golden = ensure_golden(args.scene, exe, runtime_dir, scene_path, args.golden_spp)

    # Step 3: Eval
    artifact_dir = REPO_ROOT / "eval-out" / f"iter-{iteration}-{args.scene}"
    output = artifact_dir / f"{scene_path.stem}_output.png"
    metrics_file = artifact_dir / f"{scene_path.stem}_metrics.json"

    metrics = run_eval(
        exe, runtime_dir, scene_path, output, metrics_file,
        golden, save_spp=1, warmup=args.warmup, profile=args.profile,
    )

    if metrics is None:
        log_result(iteration, "eval_fail", args.change, args.scene,
                   scene_cfg["scene_file"], 1, args.golden_spp,
                   0.0, 0.0, 0.0, guard_ms, "eval failed — exe crash or timeout")
        sys.exit(3)

    # Step 4: Judge
    rmse = metrics.get("rmse", 999.0)
    frame_ms = metrics.get("average_frame_ms", 999999.0)
    fps = metrics.get("average_fps", 0.0)

    status, reason = judge(metrics, guard_ms, rmse_target, args.scene)

    # Step 5: Log
    log_result(iteration, status, args.change, args.scene,
               scene_cfg["scene_file"], 1, args.golden_spp,
               rmse, frame_ms, fps, guard_ms, reason)

    print(f"\n{'='*60}")
    print(f"[result] status={status}")
    print(f"[result] {reason}")
    print(f"[result] paper target: frame_ms <= {guard_ms}, rmse <= {rmse_target}")
    print(f"[result] actual:       frame_ms = {frame_ms:.2f}, rmse = {rmse:.6f}")
    spatial = metrics.get("average_spatial_angular_ms", 0)
    if spatial == 0:
        print(f"[WARNING] spatial_angular_ms = 0 — Phase 2 is disabled!")
    print(f"{'='*60}")

    if status in ("keep", "target_hit"):
        sys.exit(0)
    else:
        sys.exit(1)


if __name__ == "__main__":
    main()
