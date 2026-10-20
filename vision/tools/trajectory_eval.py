"""
trajectory_eval.py — Camera trajectory evaluation for ghosting and temporal stability.

Generates 4096spp golden references at each camera pose, then runs trajectory
eval with SSAT enabled and computes per-pose RMSE.

Usage:
    python tools/trajectory_eval.py --scene spaceship_desktop4k --trajectory translate_forward
    python tools/trajectory_eval.py --scene spaceship_desktop4k --trajectory look_up --skip-build
    python tools/trajectory_eval.py --scene spaceship_desktop4k --trajectory combined
"""

import argparse
import json
import math
import os
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCENE_TARGETS = REPO_ROOT / "tools" / "scene_targets.json"
TRAJECTORIES_DIR = REPO_ROOT / "tools" / "trajectories"
BUILD_CMD = REPO_ROOT / "tools" / "build_standalone.cmd"
GOLDEN_ROOT = REPO_ROOT / "eval-out" / "golden"
TRAJECTORY_ROOT = REPO_ROOT / "eval-out" / "trajectory"

CMD_EXE = Path(r"C:\Windows\System32\cmd.exe")



def load_targets() -> dict:
    return json.loads(SCENE_TARGETS.read_text(encoding="utf-8"))


def load_trajectory(name: str) -> dict:
    path = TRAJECTORIES_DIR / f"{name}.json"
    if not path.exists():
        print(f"[error] trajectory not found: {path}")
        print(f"Available trajectories:")
        for f in TRAJECTORIES_DIR.glob("*.json"):
            print(f"  {f.stem}")
        sys.exit(1)
    return json.loads(path.read_text(encoding="utf-8"))


def get_runtime_env() -> dict:
    env = os.environ.copy()
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


def interpolate_pose(keyframes: list, frame: int) -> dict:
    """Linearly interpolate pose at the given frame."""
    if not keyframes:
        return {"delta_pos": [0, 0, 0], "delta_pitch": 0, "delta_yaw": 0}
    if frame <= keyframes[0]["frame"]:
        kf = keyframes[0]
        return {"delta_pos": kf.get("delta_pos", [0, 0, 0]),
                "delta_pitch": kf.get("delta_pitch", 0),
                "delta_yaw": kf.get("delta_yaw", 0)}
    if frame >= keyframes[-1]["frame"]:
        kf = keyframes[-1]
        return {"delta_pos": kf.get("delta_pos", [0, 0, 0]),
                "delta_pitch": kf.get("delta_pitch", 0),
                "delta_yaw": kf.get("delta_yaw", 0)}
    for i in range(len(keyframes) - 1):
        a, b = keyframes[i], keyframes[i + 1]
        if a["frame"] <= frame <= b["frame"]:
            t = (frame - a["frame"]) / (b["frame"] - a["frame"])
            dp_a = a.get("delta_pos", [0, 0, 0])
            dp_b = b.get("delta_pos", [0, 0, 0])
            return {
                "delta_pos": [dp_a[j] + t * (dp_b[j] - dp_a[j]) for j in range(3)],
                "delta_pitch": a.get("delta_pitch", 0) + t * (b.get("delta_pitch", 0) - a.get("delta_pitch", 0)),
                "delta_yaw": a.get("delta_yaw", 0) + t * (b.get("delta_yaw", 0) - a.get("delta_yaw", 0)),
            }
    kf = keyframes[-1]
    return {"delta_pos": kf.get("delta_pos", [0, 0, 0]),
            "delta_pitch": kf.get("delta_pitch", 0),
            "delta_yaw": kf.get("delta_yaw", 0)}


def get_initial_camera_from_scene(scene_path: Path) -> dict | None:
    """Extract initial camera position and orientation from scene JSON."""
    try:
        text = scene_path.read_text(encoding="utf-8")
        # Scene JSON may have trailing commas; try parsing with tolerance
        # For now, use regex to extract camera position
        import re
        pos_match = re.search(r'"position"\s*:\s*\[\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*\]', text)
        target_match = re.search(r'"target_pos"\s*:\s*\[\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*\]', text)
        if pos_match and target_match:
            pos = [float(pos_match.group(i)) for i in range(1, 4)]
            target = [float(target_match.group(i)) for i in range(1, 4)]
            # Compute pitch and yaw from look_at
            dx = target[0] - pos[0]
            dy = target[1] - pos[1]
            dz = target[2] - pos[2]
            dist_xz = math.sqrt(dx * dx + dz * dz)
            pitch = math.degrees(math.atan2(-dy, dist_xz))
            yaw = math.degrees(math.atan2(dx, -dz))
            return {"position": pos, "pitch": pitch, "yaw": yaw}
    except Exception as e:
        print(f"[warning] Could not parse scene camera: {e}")
    return None


def generate_golden_at_pose(
    exe: Path, runtime_dir: Path, scene: Path,
    golden_path: Path, position: list, pitch: float, yaw: float,
    golden_spp: int = 4096,
) -> bool:
    """Generate a 4096spp golden reference at a specific camera pose."""
    if golden_path.exists():
        print(f"[golden] reusing {golden_path.name}")
        return True

    override_str = f"{position[0]},{position[1]},{position[2]},{pitch},{yaw}"
    cmd = [
        str(exe), "-r", str(runtime_dir), "-m", "cli", "-s", str(scene),
        "--save-spp", str(golden_spp), "--warmup-frames", "2",
        "--profile-frames", "4", "--disable-denoiser", "true",
        "--camera-override", override_str,
        "-o", str(golden_path),
    ]

    print(f"[golden] generating at pos=({position[0]:.3f}, {position[1]:.3f}, {position[2]:.3f}) "
          f"pitch={pitch:.1f} yaw={yaw:.1f}")
    result = subprocess.run(
        cmd, capture_output=True, text=True, cwd=str(REPO_ROOT),
        env=get_runtime_env(), timeout=3600,
    )
    if result.returncode != 0 or not golden_path.exists():
        print(f"[golden] FAILED")
        print(result.stdout[-500:] if result.stdout else "")
        return False
    print(f"[golden] saved {golden_path.name}")
    return True


def compute_rmse_python(ref_path: Path, out_path: Path) -> float | None:
    """Compute RMSE between two images using Python (numpy + PIL)."""
    try:
        from PIL import Image
        import numpy as np
        ref = np.array(Image.open(ref_path)).astype(np.float64) / 255.0
        out = np.array(Image.open(out_path)).astype(np.float64) / 255.0
        if ref.shape != out.shape:
            print(f"[warning] shape mismatch: ref={ref.shape} out={out.shape}")
            return None
        # Use RGB only (first 3 channels)
        if ref.ndim == 3 and ref.shape[2] >= 3:
            ref = ref[:, :, :3]
            out = out[:, :, :3]
        diff = ref - out
        mse = np.mean(diff * diff)
        return float(np.sqrt(mse))
    except ImportError:
        print("[warning] PIL/numpy not available, skipping Python RMSE")
        return None
    except Exception as e:
        print(f"[warning] RMSE computation failed: {e}")
        return None


def run_trajectory_eval(
    exe: Path, runtime_dir: Path, scene: Path,
    trajectory_file: Path, golden_dir: Path,
    output_dir: Path, metrics_file: Path,
    warmup: int = 8, profile: int = 32,
) -> dict | None:
    """Run vision-eval with camera trajectory and compute per-pose RMSE."""
    output = output_dir / f"{scene.stem}_trajectory.png"

    cmd = [
        str(exe), "-r", str(runtime_dir), "-m", "cli", "-s", str(scene),
        "--save-spp", "1", "--warmup-frames", str(warmup),
        "--profile-frames", str(profile),
        "--metrics-file", str(metrics_file),
        "-o", str(output),
        "--stage-profile", "true",
        "--camera-poses", str(trajectory_file),
    ]

    output_dir.mkdir(parents=True, exist_ok=True)
    print(f"\n[eval] running trajectory evaluation...")
    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, cwd=str(REPO_ROOT),
            env=get_runtime_env(), timeout=1200,
        )
    except subprocess.TimeoutExpired:
        print("[eval] TIMEOUT")
        return None

    if result.returncode != 0:
        print(f"[eval] FAILED (exit={result.returncode})")
        stderr_tail = result.stderr.strip().split("\n")[-10:]
        for line in stderr_tail:
            print(f"  {line}")
        stdout_tail = result.stdout.strip().split("\n")[-10:]
        for line in stdout_tail:
            print(f"  {line}")
        return None

    if not metrics_file.exists():
        print("[eval] no metrics file produced")
        return None

    metrics = json.loads(metrics_file.read_text(encoding="utf-8"))
    print(f"[eval] frame_ms={metrics.get('average_frame_ms', '?'):.2f}")
    return metrics


def print_trajectory_report(metrics: dict, trajectory_name: str) -> None:
    """Print a formatted report of trajectory evaluation results."""
    print(f"\n{'='*70}")
    print(f"TRAJECTORY EVALUATION REPORT: {trajectory_name}")
    print(f"{'='*70}")
    print(f"Average frame time: {metrics.get('average_frame_ms', 0):.2f} ms")
    print(f"Static RMSE: {metrics.get('rmse', 0):.6f}")

    pose_metrics = metrics.get("pose_metrics", [])
    if not pose_metrics:
        print("[warning] No per-pose metrics available")
        return

    print(f"\n{'Frame':>6} {'RMSE':>10} {'Delta Pos':>24} {'dPitch':>8} {'dYaw':>8} {'Status':>10}")
    print("-" * 70)

    static_rmse = None
    for pm in pose_metrics:
        frame = pm.get("frame", 0)
        rmse = pm.get("rmse", 0)
        dp = pm.get("delta_pos", [0, 0, 0])
        dpitch = pm.get("delta_pitch", 0)
        dyaw = pm.get("delta_yaw", 0)

        if frame == 0:
            static_rmse = rmse

        is_moving = any(abs(dp[i]) > 0.001 for i in range(3)) or abs(dpitch) > 0.1 or abs(dyaw) > 0.1
        if static_rmse and static_rmse > 0 and rmse > 0:
            ratio = rmse / static_rmse
            if ratio > 1.5:
                status = "GHOSTING?"
            elif ratio > 1.2:
                status = "ELEVATED"
            else:
                status = "OK"
        else:
            status = "—"

        dp_str = f"({dp[0]:+.3f}, {dp[1]:+.3f}, {dp[2]:+.3f})"
        print(f"{frame:>6} {rmse:>10.6f} {dp_str:>24} {dpitch:>8.1f} {dyaw:>8.1f} {status:>10}")

    print("-" * 70)
    if static_rmse and static_rmse > 0:
        rmse_values = [pm.get("rmse", 0) for pm in pose_metrics if pm.get("rmse", 0) > 0]
        if rmse_values:
            max_rmse = max(rmse_values)
            max_ratio = max_rmse / static_rmse
            print(f"Static RMSE (frame 0): {static_rmse:.6f}")
            print(f"Max RMSE:              {max_rmse:.6f} ({max_ratio:.2f}x static)")
            if max_ratio > 1.5:
                print("CONCLUSION: Significant ghosting detected — Phase 3 temporal rejection needs tuning")
            elif max_ratio > 1.2:
                print("CONCLUSION: Mild RMSE elevation during motion — acceptable temporal behavior")
            else:
                print("CONCLUSION: Stable RMSE across trajectory — good temporal accumulation")
    print(f"{'='*70}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Camera trajectory evaluation for ghosting and temporal stability.")
    parser.add_argument("--scene", required=True, help="Scene key from scene_targets.json")
    parser.add_argument("--trajectory", required=True,
                        help="Trajectory name (translate_forward, look_up, combined) or path to JSON")
    parser.add_argument("--build-dir", default="cmake-build-codex-msvc3")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--golden-spp", type=int, default=4096)
    parser.add_argument("--warmup", type=int, default=8)
    parser.add_argument("--profile", type=int, default=32)
    args = parser.parse_args()

    targets_data = load_targets()
    scene_root = Path(targets_data["scene_root"])

    if args.scene not in targets_data["scenes"]:
        print(f"[error] unknown scene '{args.scene}'")
        sys.exit(1)

    scene_cfg = targets_data["scenes"][args.scene]
    scene_path = scene_root / scene_cfg["scene_file"]
    build_dir = REPO_ROOT / args.build_dir

    if not scene_path.exists():
        print(f"[error] scene file not found: {scene_path}")
        sys.exit(1)

    # Load trajectory
    traj_path = Path(args.trajectory)
    if not traj_path.exists():
        traj_path = TRAJECTORIES_DIR / f"{args.trajectory}.json"
    if not traj_path.exists():
        print(f"[error] trajectory not found: {args.trajectory}")
        sys.exit(1)

    traj_data = json.loads(traj_path.read_text(encoding="utf-8"))
    traj_name = traj_path.stem

    print(f"{'='*60}")
    print(f"[trajectory_eval] scene={args.scene}")
    print(f"[trajectory_eval] trajectory={traj_name}: {traj_data.get('description', '')}")
    print(f"{'='*60}")

    # Build
    if not args.skip_build:
        if not build(build_dir):
            print("[error] build failed")
            sys.exit(2)

    exe = build_dir / "bin" / "vision-eval.exe"
    runtime_dir = build_dir / "bin"
    if not exe.exists():
        print(f"[error] exe not found: {exe}")
        sys.exit(1)

    # Get initial camera from scene
    cam = get_initial_camera_from_scene(scene_path)
    if cam is None:
        print("[error] could not extract initial camera from scene JSON")
        sys.exit(1)

    initial_pos = cam["position"]
    initial_pitch = cam["pitch"]
    initial_yaw = cam["yaw"]
    print(f"[camera] initial: pos=({initial_pos[0]:.3f}, {initial_pos[1]:.3f}, {initial_pos[2]:.3f}) "
          f"pitch={initial_pitch:.1f} yaw={initial_yaw:.1f}")

    # Generate golden references for each save frame
    golden_dir = GOLDEN_ROOT / args.scene / "trajectory" / traj_name
    golden_dir.mkdir(parents=True, exist_ok=True)

    keyframes = traj_data.get("keyframes", [])
    save_frames = traj_data.get("save_frames", [])

    print(f"\n[golden] generating {len(save_frames)} golden references...")
    for sf in save_frames:
        pose = interpolate_pose(keyframes, sf)
        dp = pose["delta_pos"]
        abs_pos = [initial_pos[i] + dp[i] for i in range(3)]
        abs_pitch = initial_pitch + pose["delta_pitch"]
        abs_yaw = initial_yaw + pose["delta_yaw"]

        golden_path = golden_dir / f"golden_frame_{sf:04d}.png"
        ok = generate_golden_at_pose(
            exe, runtime_dir, scene_path, golden_path,
            abs_pos, abs_pitch, abs_yaw, args.golden_spp,
        )
        if not ok:
            print(f"[error] failed to generate golden for frame {sf}")
            sys.exit(1)

    # Run trajectory eval
    timestamp = time.strftime("%Y%m%d_%H%M%S")
    output_dir = TRAJECTORY_ROOT / f"{args.scene}_{traj_name}_{timestamp}"
    metrics_file = output_dir / "trajectory_metrics.json"

    metrics = run_trajectory_eval(
        exe, runtime_dir, scene_path, traj_path, golden_dir,
        output_dir, metrics_file,
        warmup=args.warmup, profile=args.profile,
    )

    if metrics is None:
        print("[error] trajectory eval failed")
        sys.exit(3)

    # Compute per-pose RMSE in Python
    print(f"\n[rmse] computing per-pose RMSE...")
    pose_metrics = []
    for sf in save_frames:
        pose = interpolate_pose(keyframes, sf)
        golden_path = golden_dir / f"golden_frame_{sf:04d}.png"

        # Find the output frame image
        out_stem = scene_path.stem + "_trajectory"
        out_frame = output_dir / f"{out_stem}_frame_{sf:04d}.png"

        rmse_val = None
        if golden_path.exists() and out_frame.exists():
            rmse_val = compute_rmse_python(golden_path, out_frame)
            if rmse_val is not None:
                print(f"[rmse] frame {sf:4d}: rmse={rmse_val:.6f}")
            else:
                print(f"[rmse] frame {sf:4d}: computation failed")
        else:
            missing = []
            if not golden_path.exists():
                missing.append(f"golden={golden_path.name}")
            if not out_frame.exists():
                missing.append(f"output={out_frame.name}")
            print(f"[rmse] frame {sf:4d}: missing {', '.join(missing)}")

        pose_metrics.append({
            "frame": sf,
            "rmse": rmse_val or 0.0,
            "delta_pos": pose["delta_pos"],
            "delta_pitch": pose["delta_pitch"],
            "delta_yaw": pose.get("delta_yaw", 0),
            "output_file": str(out_frame) if out_frame.exists() else "",
            "golden_file": str(golden_path) if golden_path.exists() else "",
        })

    metrics["pose_metrics"] = pose_metrics
    metrics["trajectory_active"] = True

    print_trajectory_report(metrics, traj_name)

    # Save summary
    summary = {
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "scene": args.scene,
        "trajectory": traj_name,
        "metrics": metrics,
    }
    summary_file = output_dir / "trajectory_summary.json"
    summary_file.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"\n[output] Summary: {summary_file}")
    print(f"[output] Metrics: {metrics_file}")
    print(f"[output] Outputs: {output_dir}")


if __name__ == "__main__":
    main()
