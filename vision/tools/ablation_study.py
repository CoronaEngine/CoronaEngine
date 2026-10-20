"""
ablation_study.py — Run Phase 2/3 ablation study.

Runs 4 configurations of the SSAT denoiser to verify each phase contributes
to RMSE reduction:
  1. raw:         denoiser fully disabled (baseline)
  2. phase3_only: Phase 2 skipped, only temporal accumulation
  3. phase2_only: Phase 3 skipped, only spatial-angular filter
  4. full:        both Phase 2 + Phase 3 (current pipeline)

Usage:
    python tools/ablation_study.py --scene spaceship_desktop4k --config quality
    python tools/ablation_study.py --scene spaceship_desktop4k --config quality --skip-build
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
BUILD_CMD = REPO_ROOT / "tools" / "build_standalone.cmd"
GOLDEN_ROOT = REPO_ROOT / "eval-out" / "golden"
ABLATION_ROOT = REPO_ROOT / "eval-out" / "ablation"

CMD_EXE = Path(r"C:\Windows\System32\cmd.exe")


ABLATION_CONFIGS = [
    # {
    #     "name": "raw",
    #     "description": "No SSAT (denoiser disabled)",
    #     "disable_denoiser": True,
    #     "env_overrides": {},
    #     "save_spp": 1,
    # },
    # {
    #     "name": "phase3_only",
    #     "description": "Phase 3 only (temporal accumulation, no spatial filter)",
    #     "disable_denoiser": False,
    #     "env_overrides": {"SSAT_SKIP_PHASE2": "1"},
    #     "save_spp": 40,
    # },
    {
        "name": "spatial_only",
        "description": "Spatial filtering only (angular_samples=1, no temporal)",
        "disable_denoiser": False,
        "env_overrides": {"SSAT_ANGULAR_SAMPLES": "1", "SSAT_SKIP_PHASE3": "1"},
        "save_spp": 1,
    },
    {
        "name": "phase2_only",
        "description": "Phase 2 only (spatial-angular filter, no temporal)",
        "disable_denoiser": False,
        "env_overrides": {"SSAT_SKIP_PHASE3": "1"},
        "save_spp": 1,
    },
    {
        "name": "full",
        "description": "Full pipeline (Phase 2 + Phase 3)",
        "disable_denoiser": False,
        "env_overrides": {},
        "save_spp": 1,
    },
    # {
    #     "name": "full_40f",
    #     "description": "Full pipeline, measured at frame 40 (Phase 3 converged)",
    #     "disable_denoiser": False,
    #     "env_overrides": {},
    #     "save_spp": 40,
    # },
]


def load_targets() -> dict:
    return json.loads(SCENE_TARGETS.read_text(encoding="utf-8"))


def get_runtime_env(extra_env: dict | None = None) -> dict:
    env = os.environ.copy()
    for k in ["SSAT_SKIP_PHASE2", "SSAT_SKIP_PHASE3"]:
        env.pop(k, None)
    if extra_env:
        env.update(extra_env)
    return env


def build(build_dir: Path) -> bool:
    targets = ["vision-denoiser-SSAT", "vision-eval"]
    for t in targets:
        print(f"[build] target={t}")
        result = subprocess.run(
            [str(CMD_EXE), "/c", str(BUILD_CMD), t, str(build_dir)],
            capture_output=True, text=True, cwd=str(REPO_ROOT), timeout=600,
        )
        if result.returncode != 0 or "FAILED:" in result.stdout:
            last_lines = result.stdout.strip().split("\n")[-20:]
            print(f"[build] {t} FAILED:")
            for line in last_lines:
                print(f"  {line}")
            return False
    print("[build] ALL TARGETS SUCCESS")
    return True


def ensure_golden(scene_key: str, exe: Path, runtime_dir: Path, scene: Path,
                  golden_spp: int = 4096) -> Path:
    golden_dir = GOLDEN_ROOT / scene_key
    golden_dir.mkdir(parents=True, exist_ok=True)
    golden_png = golden_dir / f"{scene.stem}_golden.png"

    if golden_png.exists():
        print(f"[golden] reusing {golden_png}")
        return golden_png

    print(f"[golden] generating {golden_spp} spp golden for {scene.name}...")
    cmd = [
        str(exe), "-r", str(runtime_dir), "-m", "cli", "-s", str(scene),
        "--save-spp", str(golden_spp), "--warmup-frames", "2",
        "--profile-frames", "4", "--disable-denoiser", "true",
        "-o", str(golden_png),
    ]
    result = subprocess.run(
        cmd, capture_output=True, text=True, cwd=str(REPO_ROOT),
        env=get_runtime_env(), timeout=3600,
    )
    if result.returncode != 0 or not golden_png.exists():
        print(f"[golden] FAILED")
        print(result.stdout[-500:] if result.stdout else "")
        sys.exit(1)

    print(f"[golden] saved to {golden_png}")
    return golden_png


def run_eval_config(
    config: dict,
    exe: Path,
    runtime_dir: Path,
    scene: Path,
    golden: Path,
    output_dir: Path,
) -> dict | None:
    name = config["name"]
    output = output_dir / f"{scene.stem}_{name}.png"
    metrics_file = output_dir / f"{scene.stem}_{name}_metrics.json"
    output_dir.mkdir(parents=True, exist_ok=True)

    save_spp = config.get("save_spp", 1)
    cmd = [
        str(exe), "-r", str(runtime_dir), "-m", "cli", "-s", str(scene),
        "--save-spp", str(save_spp), "--warmup-frames", "8", "--profile-frames", "32",
        "--metrics-file", str(metrics_file), "-o", str(output),
        "--stage-profile", "true",
    ]
    if save_spp > 1:
        cmd.extend(["--no-accumulation", "true"])
    if config["disable_denoiser"]:
        cmd.extend(["--disable-denoiser", "true"])
    if golden.exists():
        cmd.extend(["--rmse-reference", str(golden)])

    env = get_runtime_env(config["env_overrides"])

    print(f"\n[eval] config={name}: {config['description']}")
    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, cwd=str(REPO_ROOT),
            env=env, timeout=600,
        )
    except subprocess.TimeoutExpired:
        print(f"[eval] {name} TIMEOUT")
        return None

    if result.returncode != 0:
        print(f"[eval] {name} FAILED (exit={result.returncode})")
        stderr_tail = result.stderr.strip().split("\n")[-10:]
        for line in stderr_tail:
            print(f"  {line}")
        return None

    if not metrics_file.exists():
        print(f"[eval] {name} — no metrics file")
        return None

    metrics = json.loads(metrics_file.read_text(encoding="utf-8"))
    metrics["config_name"] = name
    print(f"[eval] {name}: rmse={metrics.get('rmse', '?'):.6f}  "
          f"frame_ms={metrics.get('average_frame_ms', '?'):.2f}  "
          f"spatial_ms={metrics.get('average_spatial_angular_ms', 0):.2f}  "
          f"temporal_ms={metrics.get('average_temporal_ms', 0):.2f}")
    return metrics


def print_ablation_table(results: list[dict], output_file: Path | None = None) -> None:
    raw_rmse = None
    for r in results:
        if r["config_name"] == "raw":
            raw_rmse = r.get("rmse", 0)
            break

    header = f"{'Config':<16} {'RMSE':>10} {'Delta':>10} {'Frame ms':>10} {'Spatial ms':>12} {'Temporal ms':>12}"
    sep = "-" * len(header)

    lines = [sep, header, sep]
    for r in results:
        rmse = r.get("rmse", 0)
        frame_ms = r.get("average_frame_ms", 0)
        spatial_ms = r.get("average_spatial_angular_ms", 0)
        temporal_ms = r.get("average_temporal_ms", 0)
        name = r["config_name"]

        if raw_rmse and raw_rmse > 0 and name != "raw":
            delta_pct = (rmse - raw_rmse) / raw_rmse * 100
            delta_str = f"{delta_pct:+.1f}%"
        else:
            delta_str = "—"

        lines.append(
            f"{name:<16} {rmse:>10.6f} {delta_str:>10} {frame_ms:>10.2f} {spatial_ms:>12.2f} {temporal_ms:>12.2f}"
        )
    lines.append(sep)

    table = "\n".join(lines)
    print(f"\n{'='*60}")
    print("ABLATION RESULTS")
    print(f"{'='*60}")
    print(table)

    if output_file:
        output_file.parent.mkdir(parents=True, exist_ok=True)
        with output_file.open("w", encoding="utf-8") as f:
            f.write("config\trmse\tdelta_pct\tframe_ms\tspatial_ms\ttemporal_ms\n")
            for r in results:
                rmse = r.get("rmse", 0)
                frame_ms = r.get("average_frame_ms", 0)
                spatial_ms = r.get("average_spatial_angular_ms", 0)
                temporal_ms = r.get("average_temporal_ms", 0)
                name = r["config_name"]
                delta = ((rmse - raw_rmse) / raw_rmse * 100) if raw_rmse and raw_rmse > 0 and name != "raw" else 0
                f.write(f"{name}\t{rmse:.6f}\t{delta:.1f}\t{frame_ms:.2f}\t{spatial_ms:.2f}\t{temporal_ms:.2f}\n")
        print(f"\n[output] TSV saved to {output_file}")


def main() -> None:
    parser = argparse.ArgumentParser(description="SSAT Phase 2/3 ablation study.")
    parser.add_argument("--scene", required=True, help="Scene key from scene_targets.json")
    parser.add_argument("--config", default="quality", choices=["quality", "performance"])
    parser.add_argument("--build-dir", default="cmake-build-codex-msvc3")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--golden-spp", type=int, default=4096)
    args = parser.parse_args()

    targets_data = load_targets()
    scene_root = Path(targets_data["scene_root"])

    if args.scene not in targets_data["scenes"]:
        print(f"[error] unknown scene '{args.scene}'. Available:")
        for k in targets_data["scenes"]:
            print(f"  {k}")
        sys.exit(1)

    scene_cfg = targets_data["scenes"][args.scene]
    scene_path = scene_root / scene_cfg["scene_file"]
    build_dir = REPO_ROOT / args.build_dir

    if not scene_path.exists():
        print(f"[error] scene file not found: {scene_path}")
        sys.exit(1)

    print(f"{'='*60}")
    print(f"[ablation] scene={args.scene} config={args.config}")
    print(f"{'='*60}")

    if not args.skip_build:
        if not build(build_dir):
            print("[error] build failed")
            sys.exit(2)

    exe = build_dir / "bin" / "vision-eval.exe"
    runtime_dir = build_dir / "bin"
    if not exe.exists():
        print(f"[error] exe not found: {exe}")
        sys.exit(1)

    golden = ensure_golden(args.scene, exe, runtime_dir, scene_path, args.golden_spp)

    timestamp = time.strftime("%Y%m%d_%H%M%S")
    output_dir = ABLATION_ROOT / f"{args.scene}_{timestamp}"

    results = []
    for cfg in ABLATION_CONFIGS:
        metrics = run_eval_config(cfg, exe, runtime_dir, scene_path, golden, output_dir)
        if metrics:
            results.append(metrics)
        else:
            print(f"[warning] config '{cfg['name']}' failed, skipping")

    if results:
        tsv_file = output_dir / "ablation_results.tsv"
        print_ablation_table(results, tsv_file)

        summary_file = output_dir / "ablation_summary.json"
        summary_file.write_text(json.dumps(results, indent=2), encoding="utf-8")
        print(f"[output] JSON saved to {summary_file}")
    else:
        print("[error] no results collected")
        sys.exit(1)


if __name__ == "__main__":
    main()
